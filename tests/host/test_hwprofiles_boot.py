"""G30B assembled loader profiles: bounded BIOS/file seams, no QEMU."""
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from loader_parser_machine import LoaderParserMachine, SYMBOLS
from test_startup_menu import Machine as StartupMachine, NAMES as STARTUP_NAMES
ROOT = Path(__file__).resolve().parents[2]
NAMES = (*STARTUP_NAMES, 'hwprofiles_boot', 'hw_count', 'hw_default', 'hw_selected', 'hw_wait', 'hw_line', 'hw_names',
         'hw_raw', 'file_getc', 'progress_begin', 'progress_active', 'progress_trail', 'loader_bss_end')
class Machine(StartupMachine):
    def __init__(self, binary):
        LoaderParserMachine.__init__(self, binary, NAMES)
    def profile(self, data=None, keys=()):
        self.setup(keys=keys)
        self.input = data
        self.at = 0
        self.hooks[self.symbols['directory_find']] = self.find
        self.hooks[self.symbols['file_open']] = lambda: None
        self.hooks[self.symbols['file_getc']] = self.getbyte
        self.hooks[self.symbols['progress_begin']] = lambda: self.var('progress_active', 1, 8)
        self.run('hwprofiles_boot')
        start = 0x20000 + self.symbols['progress_trail']
        return bytes(self.mem[start + 24:start + 32])
    def find(self):
        self.carry = self.input is None
        if not self.carry:
            self.var('found_attr', 0, 8)
            self.var('found_size', len(self.input), 32)
            self.write('eax', 100)
    def getbyte(self):
        self.carry = self.at >= len(self.input)
        if not self.carry:
            self.write('al', self.input[self.at]); self.at += 1
class ProfileBootTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent = ROOT / 'build/host'; parent.mkdir(parents=True, exist_ok=True)
        cls.tmp = tempfile.TemporaryDirectory(prefix='g30b-loader-', dir=parent)
        asm = Path(cls.tmp.name) / 'profiles.asm'; binary = asm.with_suffix('.bin')
        asm.write_text('%include "src/boot/ciukldr.asm"\nsection .test_symbols align=1\ndw ' + ','.join(NAMES) + '\n')
        subprocess.run(['nasm', '-f', 'bin', str(asm), '-o', str(binary)], cwd=ROOT, check=True)
        cls.m = Machine(binary)
    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()
    def test_absent_single_default_and_exact_handoff(self):
        m = self.m
        self.assertEqual(m.profile(), bytes(8))
        data = b'version=1\nwait=-1\ndefault=0\nprofile=Original Configuration|0\n0:device=10058 disabled=1 removed=0 driver=-\n'
        self.assertEqual(m.profile(data), b'HP01\0\1' + struct.pack('<H', len(data)))
        self.assertEqual(m.symbols['hw_raw'], 32768)
        self.assertEqual(m.mem[0x28000:0x28000+len(data)], data)
        self.assertGreaterEqual(m.symbols['loader_bss_end'], 32768 + 24576)
    def test_default_timeout_choice_and_indefinite_wait(self):
        m = self.m
        data = b'version=1\nwait=2\ndefault=1\nprofile=Home|0\nprofile=Docked|1\n'
        self.assertEqual(m.profile(data)[4:6], bytes([1,2]))
        self.assertGreaterEqual(m.tick, 36)
        self.assertIn('Docked', m.joined())
        self.assertEqual(m.profile(data, [(0,ord('1'),0),(1,13,0)])[4], 0)
        self.assertEqual(m.profile(data.replace(b'wait=2',b'wait=-1'), [(100,ord('1'),0),(101,13,0)])[4], 0)
        self.assertGreaterEqual(m.tick, 100)
        self.assertEqual(m.profile(data, [(0,0,0x48),(1,13,0)])[4], 0)
        self.assertEqual(m.profile(data.replace(b'default=1',b'default=0'), [(0,0,0x50),(1,13,0)])[4], 1)
        self.assertEqual(m.profile(data, [(0,ord('9'),0),(1,13,0)])[4], 1)
    def test_zero_wait_invalid_default_and_profile_cap(self):
        m = self.m
        data = b'wait=0\ndefault=7\nprofile=Home|0\nprofile=Docked|1\n'
        self.assertEqual(m.profile(data)[4:6], bytes([0,2]))
        data = b'wait=0\ndefault=7\n' + b''.join(f'profile=P{i}|0\n'.encode() for i in range(10))
        self.assertEqual(m.profile(data)[4:6], bytes([7,8]))
        self.assertEqual(m.profile(b'x'*24576), bytes(8))
if __name__ == '__main__':
    unittest.main()
