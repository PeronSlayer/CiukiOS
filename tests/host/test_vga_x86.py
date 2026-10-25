"""f3-02: migrated instruction-emulator shim (tests/host/test_vga_x86_shim.c).

The archived differential script compared vga_x86.c with Unicorn; Unicorn is
not a project dependency, so these are fixed hand-derived vectors through the
unchanged shim on a plain RAM bus: semantics, bus-cycle order, REP restart,
V86 segment limits and the unsupported/divide outcomes the VM relies on.
"""
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
DONE, PARTIAL, UNSUPPORTED, BUS, DIVIDE = 0, 1, 2, 3, 4
READ, WRITE, FETCH = 0, 1, 2
CF, ZF, VM = 0x1, 0x40, 0x20000


class CPU(ctypes.Structure):
    _fields_ = [('gpr', ctypes.c_uint32 * 8), ('eip', ctypes.c_uint32), ('eflags', ctypes.c_uint32),
                ('seg_base', ctypes.c_uint32 * 6), ('sreg', ctypes.c_uint16 * 6),
                ('code32', ctypes.c_uint8), ('base_valid', ctypes.c_uint8), ('lock_sp', ctypes.c_uint8),
                ('reserved', ctypes.c_uint8)]


class Result(ctypes.Structure):
    _fields_ = [('length', ctypes.c_uint32), ('elements', ctypes.c_uint32), ('bytes', ctypes.c_uint8 * 16)]


EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI = range(8)


@unittest.skipUnless(shutil.which('clang'), 'requires clang')
class VgaX86ShimTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch = ROOT / 'build/host'
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='vga-x86-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        library = Path(cls.temp.name) / 'libcvx.so'
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fPIC', '-shared',
                        '-I', str(ROOT / 'src/kernel/vm'), str(ROOT / 'src/kernel/vm/vga_x86.c'),
                        str(ROOT / 'tests/host/test_vga_x86_shim.c'), '-o', str(library)],
                       check=True, env=dict(os.environ, TMPDIR=cls.temp.name))
        cls.lib = ctypes.CDLL(str(library))
        assert cls.lib.cvx_shim_cpu_size() == ctypes.sizeof(CPU)
        assert cls.lib.cvx_shim_result_size() == ctypes.sizeof(Result)
        cls.lib.cvx_shim_execute.restype = ctypes.c_int

    def run_code(self, code, regs=None, memory=None, budget=64, eflags=0x2, eip=0):
        mem = (ctypes.c_uint8 * 0x20000)()
        for address, data in (memory or {}).items():
            for i, b in enumerate(data):
                mem[address + i] = b
        for i, b in enumerate(code):
            mem[eip + i] = b
        cpu = CPU()
        cpu.eip, cpu.eflags = eip, eflags
        for index, value in (regs or {}).items():
            cpu.gpr[index] = value
        log = (ctypes.c_uint32 * 256)()
        count = ctypes.c_uint32()
        result = Result()
        status = self.lib.cvx_shim_execute(ctypes.byref(cpu), mem, len(mem), budget, log, 128,
                                           ctypes.byref(count), ctypes.byref(result))
        cycles = [(log[i * 2], log[i * 2 + 1]) for i in range(min(count.value, 128))]
        return status, cpu, mem, [c for c in cycles if c[1] != FETCH]

    def test_store_load_and_cycle_order(self):
        status, cpu, mem, cycles = self.run_code(bytes([0x88, 0x07]), {EBX: 0x100, EAX: 0x5A})
        self.assertEqual((status, cpu.eip, mem[0x100]), (DONE, 2, 0x5A))
        self.assertEqual(cycles, [(0x100, WRITE)])
        # MOVSW: source before destination, low byte first.
        status, cpu, mem, cycles = self.run_code(bytes([0xA5]), {ESI: 0x300, EDI: 0x310}, {0x300: b'\x34\x12'})
        self.assertEqual((status, mem[0x310], mem[0x311], cpu.gpr[ESI], cpu.gpr[EDI]), (DONE, 0x34, 0x12, 0x302, 0x312))
        self.assertEqual(cycles, [(0x300, READ), (0x301, READ), (0x310, WRITE), (0x311, WRITE)])

    def test_read_modify_write_flags(self):
        status, cpu, mem, cycles = self.run_code(bytes([0x00, 0x07]), {EBX: 0x120, EAX: 1}, {0x120: b'\xff'})
        self.assertEqual((status, mem[0x120]), (DONE, 0))
        self.assertTrue(cpu.eflags & CF and cpu.eflags & ZF)
        self.assertEqual(cycles, [(0x120, READ), (0x120, WRITE)])

    def test_rep_budget_restarts_like_interrupted_rep(self):
        regs = {ECX: 10, EDI: 0x200, EAX: 0x11}
        status, cpu, mem, _ = self.run_code(bytes([0xF3, 0xAA]), regs, budget=4)
        self.assertEqual((status, cpu.eip, cpu.gpr[ECX], cpu.gpr[EDI]), (PARTIAL, 0, 6, 0x204))
        self.assertEqual(bytes(mem[0x200:0x205]), b'\x11' * 4 + b'\x00')

    def test_register_setup_and_short_control_flow(self):
        status, cpu, _, cycles = self.run_code(bytes([0xB9, 4, 0]), {ECX: 0xABCD0000})
        self.assertEqual((status, cpu.eip, cpu.gpr[ECX], cycles), (DONE, 3, 0xABCD0004, []))
        status, cpu, _, _ = self.run_code(bytes([0xE2, 0xFE]), {ECX: 0xCAFE0002})
        self.assertEqual((status, cpu.eip, cpu.gpr[ECX]), (DONE, 0, 0xCAFE0001))
        status, cpu, _, _ = self.run_code(bytes([0xE3, 0xFE]), {ECX: 0xCAFE0000})
        self.assertEqual((status, cpu.eip, cpu.gpr[ECX]), (DONE, 0, 0xCAFE0000))
        status, cpu, _, _ = self.run_code(bytes([0x74, 0xFE]), eflags=2 | ZF)
        self.assertEqual((status, cpu.eip), (DONE, 0))
        status, cpu, _, _ = self.run_code(bytes([0x48]), {EAX: 0}, eflags=2 | CF)
        self.assertEqual((status, cpu.gpr[EAX]), (DONE, 0xFFFF))
        self.assertTrue(cpu.eflags & CF)

    def test_unsupported_divide_and_v86_limits(self):
        # BT m16, r16 is outside the supported memory forms: nothing changes.
        status, cpu, mem, cycles = self.run_code(bytes([0x0F, 0xA3, 0x07]), {EBX: 0x100, EAX: 3})
        self.assertEqual((status, cpu.eip, cycles), (UNSUPPORTED, 0, []))
        status, cpu, _, _ = self.run_code(bytes([0xF6, 0x37]), {EBX: 0x100, EAX: 0x1234})
        self.assertEqual((status, cpu.gpr[EAX], cpu.eip), (DIVIDE, 0x1234, 0))
        # V86: a word at offset FFFFh crosses the 64 KiB segment limit.
        status, cpu, mem, cycles = self.run_code(bytes([0xA3, 0xFF, 0xFF]), {EAX: 0xBEEF}, eflags=0x2 | VM)
        self.assertEqual((status, cpu.eip, cycles), (BUS, 0, []))


if __name__ == '__main__':
    unittest.main()
