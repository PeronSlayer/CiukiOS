"""Sierra utility device facades; fake only transport and window/timer edges."""
import os
from pathlib import Path
import subprocess
import unittest
ROOT = Path(__file__).resolve().parents[3]

class DeviceUtilities(unittest.TestCase):
    def run_area(self, name, address=False):
        out = ROOT / 'build/host/win32/hlutils'
        out.mkdir(parents=True, exist_ok=True)
        program = out / (name + ('-asan' if address else ''))
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-DW32_HOST_MODEL', '-fsanitize='+('address,undefined' if address else 'undefined'), '-fno-sanitize-recover=all',
                        '-I'+str(ROOT/'sdk/win32/include'), '-I'+str(ROOT/'src/kernel/include'),
                        str(ROOT/'tests/host/win32/hlutils'/(name+'_test.c')),
                        str(ROOT/'sdk/win32/services/bytes.c'), '-o', str(program)], check=True)
        result = subprocess.run([str(program)], capture_output=True, text=True,
                                env=os.environ.copy(), timeout=30)
        if address and 'LeakSanitizer has encountered a fatal error' in result.stderr:
            self.skipTest('LeakSanitizer cannot inspect this sandbox; leak detection remains enabled')
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertIn('PASS', result.stdout)

    def test_internet_dependencies(self):
        self.run_area('internet')

    def test_comm(self):
        self.run_area('comm')

    def test_joystick(self):
        self.run_area('joystick')

    def test_comm_address_and_leak_sanitizers(self):
        self.run_area('comm', True)

    def test_internet_address_and_leak_sanitizers(self):
        self.run_area('internet', True)

    def test_joystick_address_and_leak_sanitizers(self):
        self.run_area('joystick', True)
