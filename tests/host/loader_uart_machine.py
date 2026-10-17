"""Bounded host executor for the assembled loader's UART instruction subset.

ndisasm decodes the production bytes; unsupported instructions fail closed.
Only port I/O is faked. DS is the real loader segment, FS/SS start at zero.
This is branch/ABI evidence, not firmware or CPU timing qualification.
"""
import re
import struct
import subprocess


class LoaderUARTMachine:
    def __init__(self, binary, bda=0x3f8, scratch=True, lsr=0x60,
                 divisor_low=3, divisor_high=0):
        data = binary.read_bytes()
        self.init, self.putc, self.budget, self.info = struct.unpack('<4H', data[-8:])
        self.mem = bytearray(0x40000)
        self.mem[0x20000:0x20000 + 24576] = data[:24576]
        self.mem[0x400:0x402] = struct.pack('<H', bda)
        self.regs = dict(eax=0, ebx=0, ecx=0, edx=0, esp=0x7c00, fs=0, ss=0, ds=0x2000)
        self.ports = {0x3ff: 0x37, 0x3fb: 0x80}
        self.scratch, self.lsr = scratch, lsr
        self.divisor_low, self.divisor_high = divisor_low, divisor_high
        self.sent = bytearray()
        self.reads = []
        decoded = subprocess.check_output(['ndisasm', '-b', '16', '-e', str(self.init),
                                           '-o', str(self.init), '-s', str(self.putc),
                                           str(binary)], text=True)
        self.code = {}
        for line in decoded.splitlines():
            match = re.fullmatch(r'([0-9A-F]{8})\s+([0-9A-F]+)\s+(.+)', line)
            if match:
                addr, raw, instruction = match.groups()
                address = int(addr, 16)
                self.code[address] = (address + len(raw) // 2, instruction)
        # ndisasm wraps long encodings onto continuation lines. The next
        # decoded address, rather than the first line's bytes, is the end.
        addresses = sorted(self.code)
        for address, following in zip(addresses, addresses[1:]):
            self.code[address] = (following, self.code[address][1])
        self.zero = False

    @staticmethod
    def reg_info(name):
        if name in ('fs', 'ss', 'ds'):
            return name, 0, 16
        if name.startswith('e'):
            return name, 0, 32
        if name.endswith(('l', 'h')):
            return 'e' + name[0] + 'x', 8 if name.endswith('h') else 0, 8
        return 'e' + name, 0, 16

    def width(self, operand, default=16):
        for prefix, bits in (('byte ', 8), ('word ', 16), ('dword ', 32)):
            if operand.startswith(prefix):
                return bits
        if '[' not in operand and not operand.startswith('0x'):
            return self.reg_info(operand)[2]
        return default

    def address(self, operand):
        expression = operand[operand.index('[')+1:operand.index(']')]
        segment, expression = expression.split(':') if ':' in expression else ('ds', expression)
        offset = sum(self.read(p) for p in expression.split('+'))
        return self.regs[segment] * 16 + offset

    def read(self, operand, bits=None):
        if '[' in operand:
            at = self.address(operand)
            return int.from_bytes(self.mem[at:at+(bits or self.width(operand))//8], 'little')
        if operand.startswith('0x'):
            return int(operand, 16)
        name, shift, size = self.reg_info(operand)
        return (self.regs[name] >> shift) & ((1 << size)-1)

    def write(self, operand, value, bits=None):
        size = bits or self.width(operand)
        value &= (1 << size)-1
        if '[' in operand:
            at = self.address(operand)
            self.mem[at:at+size//8] = value.to_bytes(size//8, 'little')
        else:
            name, shift, size = self.reg_info(operand)
            mask = ((1 << size)-1) << shift
            self.regs[name] = (self.regs[name] & ~mask) | ((value << shift) & mask)

    def field(self, offset, size=4):
        return self.read(f'dword [0x{self.info+offset:x}]', size*8)

    def set_field(self, offset, value, size=4):
        self.write(f'dword [0x{self.info+offset:x}]', value, size*8)

    def port_read(self, port):
        assert 0x3f8 <= port <= 0x3ff
        self.reads.append(port)
        if port == 0x3ff and not self.scratch and self.ports[port] == 0x5a:
            return 0xff
        if port == 0x3fd:
            return self.lsr
        if self.ports.get(0x3fb, 0) & 0x80:
            if port == 0x3f8:
                return self.divisor_low
            if port == 0x3f9:
                return self.divisor_high
        return self.ports.get(port, 0)

    def run(self, address, char=65):
        self.write('al', char)
        initial_sp = self.read('esp')
        for _ in range(40000):
            following, instruction = self.code[address]
            op, _, args = instruction.partition(' ')
            operands = args.split(',')
            dest = operands[0]
            bits = self.width(dest)
            if op == 'ret':
                assert self.read('esp') == initial_sp
                return
            if op == 'mov':
                self.write(dest, self.read(operands[1], bits), self.width(dest, self.width(operands[1])))
            elif op in ('xor', 'and', 'or', 'cmp', 'test'):
                a, b = self.read(dest, bits), self.read(operands[1], bits)
                value = {'xor': lambda: a ^ b, 'and': lambda: a & b, 'or': lambda: a | b,
                         'cmp': lambda: a - b, 'test': lambda: a & b}[op]()
                self.zero = (value & ((1 << bits)-1)) == 0
                if op not in ('cmp', 'test'):
                    self.write(dest, value, bits)
            elif op in ('inc', 'dec'):
                value = self.read(dest) + (1 if op == 'inc' else -1)
                self.write(dest, value, bits)
                self.zero = (value & ((1 << bits)-1)) == 0
            elif op in ('jz', 'jnz', 'jmp'):
                if op == 'jmp' or (self.zero if op == 'jz' else not self.zero):
                    following = int(dest, 16)
            elif op == 'loop':
                self.write('cx', self.read('cx') - 1)
                if self.read('cx'):
                    following = int(dest, 16)
            elif op == 'in':
                assert operands == ['al', 'dx']
                self.write('al', self.port_read(self.read('dx')))
            elif op == 'out':
                assert operands == ['dx', 'al']
                port, value = self.read('dx'), self.read('al')
                assert 0x3f8 <= port <= 0x3ff
                if port == 0x3f8 and not self.ports.get(0x3fb, 0) & 0x80:
                    self.sent.append(value)
                self.ports[port] = value
            elif op == 'push':
                self.write('esp', self.read('esp')-2)
                self.write('word [ss:esp]', self.read(dest), 16)
            elif op == 'pop':
                self.write(dest, self.read('word [ss:esp]', 16))
                self.write('esp', self.read('esp')+2)
            else:
                raise AssertionError(f'unsupported UART instruction: {instruction}')
            address = following
        raise AssertionError('loader UART instruction budget exceeded')
