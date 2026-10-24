"""Execute the production loader's assembled progress/deadline routines.

Only the BIOS text/UART sink and BDA tick are substituted. The decoder fails
on unsupported instructions; this is control-flow evidence, not PC timing.
"""
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tests/host'))
from loader_uart_machine import LoaderUARTMachine

SYMBOLS = ('progress_init', 'progress_begin', 'progress_check', 'progress_end',
           'progress_failure', 'progress_require', 'progress_clock', 'progress_print',
           'print', 'fatal', 'progress_active', 'progress_stage', 'progress_limit',
           'progress_tick', 'progress_polls', 'screen_quiet')


class ProgressMachine(LoaderUARTMachine):
    def __init__(self, path):
        super().__init__(path, bda=0)
        data = path.read_bytes()
        self.symbols = dict(zip(SYMBOLS, struct.unpack('<16H', data[24576:24576+32])))
        self.regs.update(ebp=0, esi=0, edi=0, es=0x2000)
        self.screen = ''
        self.carry = False
        args = ['ndisasm', '-b', '16']
        for name in SYMBOLS[:10]:
            args += ['-s', str(self.symbols[name])]
        decoded = subprocess.check_output([*args, str(path)], text=True)
        self.code = {}
        for line in decoded.splitlines():
            m = re.fullmatch(r'([0-9A-F]{8})\s+([0-9A-F]+)\s+(.+)', line)
            if m:
                at, raw, insn = m.groups()
                self.code[int(at, 16)] = (int(at, 16)+len(raw)//2, insn)
        addresses = sorted(self.code)
        for at, after in zip(addresses, addresses[1:]):
            self.code[at] = (after, self.code[at][1])

    @staticmethod
    def reg_info(name):
        if name == 'es':
            return name, 0, 16
        return LoaderUARTMachine.reg_info(name)

    def var(self, name, value=None, bits=32):
        operand = f'dword [0x{self.symbols[name]:x}]'
        if value is not None:
            self.write(operand, value, bits)
        return self.read(operand, bits)

    def read(self, operand, bits=None):
        if operand.isdecimal():
            return int(operand)
        return super().read(operand, bits)

    def tick(self, value):
        self.mem[0x46c:0x470] = struct.pack('<I', value)

    def masks(self):
        return struct.unpack_from('<4I', self.mem, 0x20000+24544+8)

    def push(self, value, bits):
        self.write('esp', self.read('esp')-bits//8)
        self.write('dword [ss:esp]', value, bits)

    def pop(self, bits):
        value = self.read('dword [ss:esp]', bits)
        self.write('esp', self.read('esp')+bits//8)
        return value

    def execute(self, name, stage=None):
        if stage is not None:
            self.write('al', stage)
        initial_sp = self.read('esp')
        at = self.symbols[name]
        for _ in range(20000):
            after, instruction = self.code[at]
            op, _, args = instruction.partition(' ')
            operands = args.split(',')
            dest = operands[0]
            bits = self.width(dest)
            if op == 'ret':
                if self.read('esp') == initial_sp:
                    return 'returned'
                after = self.pop(16)
            elif op == 'call':
                target = int(dest, 16)
                if target == self.symbols['print']:
                    while True:
                        char = self.read('byte [si]', 8)
                        self.write('si', self.read('si')+1)
                        if not char:
                            break
                        if not self.var('screen_quiet', bits=8):
                            self.screen += chr(char)
                else:
                    self.push(after, 16)
                    after = target
            elif op in ('mov', 'movzx'):
                value = self.read(operands[1], self.width(operands[1], bits))
                self.write(dest, value, self.width(dest, self.width(operands[1])))
            elif op in ('push', 'pop'):
                if op == 'push':
                    self.push(self.read(dest), bits)
                else:
                    self.write(dest, self.pop(bits), bits)
            elif op in ('pushad', 'popad'):
                registers = ('eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi')
                if op == 'pushad':
                    values = [self.read(reg) for reg in registers]
                    for value in values:
                        self.push(value, 32)
                else:
                    for reg in reversed(registers):
                        value = self.pop(32)
                        if reg != 'esp':
                            self.write(reg, value)
            elif op in ('bt', 'bts'):
                bit = self.read(operands[1])
                assert bit < 32, 'stage index must never carry caller high bits'
                value = self.read(dest, bits)
                self.carry = bool(value & (1 << bit))
                if op == 'bts':
                    self.write(dest, value | (1 << bit), bits)
            elif op in ('xor', 'sub', 'add', 'cmp', 'shl'):
                a, b = self.read(dest, bits), self.read(operands[1], bits)
                value = {'xor': lambda: a ^ b, 'sub': lambda: a-b,
                         'add': lambda: a+b, 'cmp': lambda: a-b, 'shl': lambda: a << b}[op]()
                self.carry = a < b if op in ('sub', 'cmp') else value > (1 << bits)-1
                self.zero = (value & ((1 << bits)-1)) == 0
                if op != 'cmp':
                    self.write(dest, value, bits)
            elif op == 'dec':
                value = self.read(dest, bits)-1
                self.write(dest, value, bits)
                self.zero = (value & ((1 << bits)-1)) == 0
            elif op in ('jmp', 'jc', 'jnc', 'jb', 'je', 'jz'):
                take = op == 'jmp' or (self.carry if op in ('jc', 'jb') else
                       not self.carry if op == 'jnc' else self.zero)
                if take:
                    after = int(dest, 16)
            elif op in ('clc', 'stc'):
                self.carry = op == 'stc'
            elif op == 'cli':
                pass
            elif op == 'hlt':
                return 'halted'
            else:
                raise AssertionError(f'unsupported progress instruction: {instruction}')
            at = after
        raise AssertionError('progress instruction budget exceeded')


@unittest.skipUnless(shutil.which('nasm') and shutil.which('ndisasm'), 'requires NASM tools')
class LoaderProgressTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        base = ROOT / 'build/host/reliability'
        base.mkdir(parents=True, exist_ok=True)
        cls.folder = tempfile.TemporaryDirectory(dir=base)
        source = Path(cls.folder.name) / 'loader.asm'
        cls.binary = source.with_suffix('.bin')
        source.write_text('%include "src/boot/ciukldr.asm"\nsection .symbols\ndw '+
                          ','.join(SYMBOLS)+'\ndw uart_init,uart_putc,uart_budget,boot_info\n')
        subprocess.run(['nasm', '-f', 'bin', str(source), '-o', str(cls.binary)], cwd=ROOT, check=True)

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    def machine(self, stage, tick=100):
        m = ProgressMachine(self.binary)
        m.execute('progress_init')
        m.var('screen_quiet', 1, 8)
        m.tick(tick)
        m.regs['ebx'] = 0xffffabcd
        m.execute('progress_begin', stage)
        return m

    def test_every_stage_has_visible_token_deadline_and_handoff_mask(self):
        for stage, name, limit in ((0, 'menu', 5461), (1, 'video', 364),
                                   (2, 'elf', 2184), (3, 'handoff', 55)):
            m = self.machine(stage)
            self.assertEqual(m.var('progress_limit', bits=16), limit)
            self.assertEqual(m.screen, f'L:STAGE {name}\r\n')
            self.assertEqual(m.var('screen_quiet', bits=8), 1)
            self.assertEqual(m.masks(), (1 << stage, 0, 0, 0))
            before = m.regs.copy()
            m.execute('progress_check')
            self.assertFalse(m.carry)
            self.assertEqual(m.regs, before)
            m.execute('progress_end')
            self.assertEqual(m.masks(), (1 << stage, 1 << stage, 0, 0))

    def test_elapsed_stalled_clock_and_midnight_record_once(self):
        for stage, name in enumerate(('menu', 'video', 'elf', 'handoff')):
            for frozen in (False, True):
                m = self.machine(stage)
                if frozen:
                    m.var('progress_polls', 1)
                else:
                    m.tick(100+m.var('progress_limit', bits=16))
                before = m.regs.copy()
                m.execute('progress_check')
                self.assertTrue(m.carry)
                self.assertEqual(m.regs, before)
                m.execute('progress_check')
                self.assertEqual(m.screen.count(f'L:STALL {name}'), 1)
                self.assertEqual(m.masks()[2], 1 << stage)
        m = self.machine(1, 0x1800af)
        m.var('progress_limit', 2, 16)
        m.tick(0)
        m.execute('progress_check')
        self.assertFalse(m.carry)
        m.tick(1)
        m.execute('progress_check')
        self.assertTrue(m.carry)

    def test_partial_elf_stops_visibly_without_keyboard_wait_and_clears_on_reentry(self):
        m = self.machine(2)
        m.var('progress_polls', 1)
        self.assertEqual(m.execute('progress_require'), 'halted')
        self.assertIn('L:STALL elf\r\nL:FAIL elf\r\nL:ABORT deadline', m.screen)
        self.assertEqual(m.masks(), (4, 0, 4, 4))
        m.execute('progress_init')
        self.assertEqual(m.masks(), (0, 0, 0, 0))

    def test_production_fallback_and_loop_checks(self):
        video = (ROOT / 'src/boot/ciukldr/video.inc').read_text()
        for label in ('.list:', '.mode:', '.best:', '.try:'):
            self.assertIn(label+'\n    call progress_check\n    jc .text', video)
        elf = (ROOT / 'src/boot/ciukldr/elf.inc').read_text()
        for label in ('.chunk:', '.bss:'):
            self.assertIn(label+'\n    call progress_require', elf)
        disk = (ROOT / 'src/boot/ciukldr/disk.inc').read_text()
        self.assertIn('disk_read:\n    call progress_require', disk)
        self.assertIn('.ok:\n    popad\n    call progress_require', disk)
        menu = (ROOT / 'src/boot/ciukldr/menu.inc').read_text().split('boot_menu:', 1)[1]
        self.assertIn('.poll:\n    call progress_check\n    jc .done', menu)


if __name__ == '__main__':
    unittest.main()
