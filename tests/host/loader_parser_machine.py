"""Bounded executor for assembled CIUKLDR parser paths; no firmware execution.

Reuse the UART harness's register/memory helpers. NASM/ndisasm supply the real
production instructions; unknown instructions fail closed. Disk reads alone
are replaced by exact-size fixture copies. This is not hardware qualification.
"""
import re
import struct
import subprocess

from loader_uart_machine import LoaderUARTMachine

SYMBOLS = ('selector_validate', 'selector_accept', 'config_read', 'boot_info',
           'selector_buffer', 'selector_fw', 'selector_force', 'selector_safe',
           'selector_sweep', 'selector_end', 'preferred_mode', 'directory_find',
           'file_open', 'file_read', 'fatal', 'found_attr', 'found_size',
           'file_size', 'system_cluster', 'boot_menu')


class LoaderParserMachine(LoaderUARTMachine):
    def __init__(self, binary, symbols=SYMBOLS):
        data = binary.read_bytes()
        self.symbols = dict(zip(symbols, struct.unpack(f'<{len(symbols)}H', data[-2*len(symbols):])))
        self.info = self.symbols['boot_info']
        self.initial = bytearray(0x40000)
        self.initial[0x20000:0x26000] = data[:24576]
        self.code = {}
        args = ['ndisasm', '-b', '16']
        for address in self.symbols.values():
            args += ['-s', str(address)]
        decoded = subprocess.check_output([*args, str(binary)], text=True)
        for line in decoded.splitlines():
            match = re.fullmatch(r'([0-9A-F]{8})\s+([0-9A-F]+)\s+(.+)', line)
            if match:
                address, raw, instruction = match.groups()
                self.code[int(address, 16)] = (int(address, 16) + len(raw)//2, instruction)
        addresses = sorted(self.code)
        for address, following in zip(addresses, addresses[1:]):
            self.code[address] = (following, self.code[address][1])
        self.reset()

    def reset(self):
        self.mem = self.initial.copy()
        self.regs = dict.fromkeys(('eax', 'ebx', 'ecx', 'edx', 'esi', 'edi', 'ebp'), 0)
        self.regs.update(esp=0x7c00, ss=0, fs=0, ds=0x2000, es=0x2000, gs=0)
        self.zero = self.carry = False
        self.hooks = {}
        self.interrupts = {}  # vector -> callable; BIOS calls are host fixtures
        self.iflag = False
        self.idle = None  # IRQ wake fixture; HLT without a fixture fails closed.
        self.budget = 100000

    @staticmethod
    def reg_info(name):
        if name in ('es', 'gs'):
            return name, 0, 16
        return LoaderUARTMachine.reg_info(name)

    def address(self, operand):
        # 16-bit effective address: signed terms such as [bx-0x1], wrapped to 64 KiB.
        expression = operand[operand.index('[')+1:operand.index(']')]
        segment, expression = expression.split(':') if ':' in expression else ('ds', expression)
        offset = sum(-self.read(term) if sign == '-' else self.read(term)
                     for sign, term in re.findall(r'([+-]?)([^+-]+)', expression))
        if not segment == 'ss':
            offset &= 0xffff
        return self.regs[segment] * 16 + offset

    def read(self, operand, bits=None):
        immediate = re.sub(r'^(?:byte|word|dword|short)\s+', '', operand)
        if re.fullmatch(r'[+-]?(?:0x[0-9a-f]+|[0-9]+)', immediate):
            return int(immediate, 0)
        return super().read(operand, bits)

    def var(self, name, value=None, bits=16):
        operand = f'word [0x{self.symbols[name]:x}]'
        if value is not None:
            self.write(operand, value, bits)
        return self.read(operand, bits)

    def push(self, value, bits=16):
        self.write('esp', self.read('esp') - bits//8)
        self.write('word [ss:esp]', value, bits)

    def pop(self, bits=16):
        value = self.read('word [ss:esp]', bits)
        self.write('esp', self.read('esp') + bits//8)
        return value

    def run(self, name, stop=None):
        address = self.symbols[name]
        self.push(0xffff)
        for _ in range(self.budget):
            if address == self.symbols['fatal']:
                return False
            if address == stop:
                return True
            if address in self.hooks:
                self.hooks[address]()
                address = self.pop()
                if address == 0xffff:  # a hooked tail call (jmp print) returned
                    return True
                continue
            following, instruction = self.code[address]
            op, _, args = instruction.partition(' ')
            operands = args.split(',')
            dest = operands[0]
            bits = self.width(dest, self.width(operands[1]) if len(operands) > 1 else 16)
            mask = (1 << bits) - 1
            if op == 'ret':
                following = self.pop()
                if following == 0xffff:
                    assert self.read('esp') == 0x7c00
                    return True
            elif op == 'call':
                self.push(following)
                following = int(dest, 16)
            elif op in ('mov', 'movzx'):
                self.write(dest, self.read(operands[1], self.width(operands[1], bits)), bits)
            elif op in ('add', 'sub', 'cmp', 'and', 'or', 'xor', 'test'):
                a, b = self.read(dest, bits), self.read(operands[1], bits) & mask
                value = {'add': lambda: a+b, 'sub': lambda: a-b, 'cmp': lambda: a-b,
                         'and': lambda: a & b, 'test': lambda: a & b,
                         'or': lambda: a | b, 'xor': lambda: a ^ b}[op]()
                self.zero = (value & mask) == 0
                self.carry = a < b if op in ('sub', 'cmp') else value > mask if op == 'add' else False
                if op not in ('cmp', 'test'):
                    self.write(dest, value, bits)
            elif op in ('inc', 'dec', 'shl', 'shr', 'imul'):
                value = self.read(dest, bits)
                if op in ('inc', 'dec'):
                    value += 1 if op == 'inc' else -1
                elif op in ('shl', 'shr'):
                    shift = self.read(operands[1])
                    if op == 'shr':
                        self.carry = bool(value & (1 << (shift-1))) if shift else self.carry
                        value >>= shift
                    else:
                        self.carry = bool(value & (1 << (bits-shift))) if shift else self.carry
                        value <<= shift
                else:
                    value = self.read(operands[1]) * self.read(operands[2])
                self.write(dest, value, bits)
                self.zero = (value & mask) == 0
            elif op in ('pushad', 'popad'):
                names = ('eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi')
                if op == 'pushad':
                    original = self.read('esp')
                    for reg in names:
                        self.push(original if reg == 'esp' else self.read(reg), 32)
                else:
                    for reg in reversed(names):
                        value = self.pop(32)
                        if reg != 'esp': self.write(reg, value, 32)
            elif op == 'not':
                self.write(dest, ~self.read(dest, bits), bits)
            elif op in ('mul', 'div'):
                assert bits == 32, instruction
                if op == 'mul':
                    value = self.read('eax') * self.read(dest, 32)
                    self.write('eax', value & 0xffffffff, 32)
                    self.write('edx', value >> 32, 32)
                else:
                    value = (self.read('edx') << 32) | self.read('eax')
                    quotient, remainder = divmod(value, self.read(dest, 32))
                    assert quotient <= 0xffffffff
                    self.write('eax', quotient, 32); self.write('edx', remainder, 32)
            elif op in ('push', 'pop'):
                if op == 'push':
                    self.push(self.read(dest), bits)
                else:
                    self.write(dest, self.pop(bits), bits)
            elif op in ('lodsb', 'stosb', 'rep', 'repe'):
                count = self.read('cx') if op in ('rep', 'repe') else 1
                for _ in range(count):
                    if op in ('lodsb', 'rep', 'repe'):
                        value = self.read('byte [si]', 8)
                        self.write('si', self.read('si') + 1)
                    if op == 'lodsb':
                        self.write('al', value)
                    elif op == 'repe' and args == 'cmpsb':
                        other = self.read('byte [es:di]', 8)
                        self.zero, self.carry = value == other, value < other
                    elif op == 'stosb' or (op == 'rep' and args == 'movsb'):
                        self.write('byte [es:di]', self.read('al') if op == 'stosb' else value, 8)
                    else:
                        raise AssertionError(instruction)
                    if op != 'lodsb':
                        self.write('di', self.read('di') + 1)
                    if op in ('rep', 'repe'):
                        self.write('cx', self.read('cx') - 1)
                    if op == 'repe' and not self.zero:
                        break
            elif op == 'loop':
                self.write('cx', self.read('cx') - 1)
                if self.read('cx'):
                    following = int(dest, 16)
            elif op in ('clc', 'stc'):
                self.carry = op == 'stc'
            elif op in ('sti', 'cli'):
                self.iflag = op == 'sti'
            elif op == 'hlt':
                assert self.iflag, 'HLT with interrupts disabled'
                assert self.idle is not None, 'HLT without an IRQ wake fixture'
                self.idle()
            elif op == 'in':
                assert operands == ['al', 'dx'], instruction
                self.write('al', self.port_read(self.read('dx')))
            elif op == 'int':
                handler = self.interrupts.get(int(dest.split()[-1], 16))
                if handler is None:
                    raise AssertionError(f'unexpected BIOS call: {instruction}')
                handler()
            elif op in ('jmp', 'jc', 'jnc', 'jb', 'jae', 'ja', 'jbe', 'jna', 'jz', 'jnz', 'je', 'jne'):
                take = {'jmp': True, 'jc': self.carry, 'jb': self.carry,
                        'jnc': not self.carry, 'jae': not self.carry,
                        'ja': not self.carry and not self.zero, 'jbe': self.carry or self.zero,
                        'jna': self.carry or self.zero,
                        'jz': self.zero, 'je': self.zero, 'jnz': not self.zero, 'jne': not self.zero}[op]
                if take:
                    following = int(dest, 16)
            else:
                raise AssertionError(f'unsupported parser instruction: {instruction}')
            address = following
        raise AssertionError('loader parser instruction budget exceeded')

    def select(self, request, source=0):
        self.reset()
        encoded = request.encode('ascii')
        start = self.symbols['selector_buffer']
        self.mem[0x20000+start:0x20000+start+len(encoded)] = encoded
        self.var('selector_fw', source, 8)
        self.write('si', start)
        self.write('cx', len(encoded))
        self.run('selector_validate')
        return not self.carry

    def config(self, text, flags=2):
        self.reset()
        data = text.encode('ascii')
        self.var('found_attr', 0, 8)
        self.var('found_size', len(data), 32)
        self.var('file_size', len(data), 32)
        self.write(f'dword [0x{self.symbols["boot_info"]+8:x}]', flags, 32)
        self.hooks[self.symbols['directory_find']] = lambda: setattr(self, 'carry', False)
        self.hooks[self.symbols['file_open']] = lambda: None

        def read_file():
            assert self.read('cx') == len(data)
            start = self.regs['es']*16 + self.read('di')
            self.mem[start:start+len(data)] = data
        self.hooks[self.symbols['file_read']] = read_file
        return self.run('config_read')
