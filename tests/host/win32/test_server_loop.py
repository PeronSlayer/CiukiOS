"""Exercise the actual guest main-loop ordering after an indefinite idle wait."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]


class ServerLoopTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        out = ROOT / 'build/host/win32/server-loop'
        out.mkdir(parents=True, exist_ok=True)
        cls.binary = out / 'server-loop'
        sources = [ROOT / 'tests/host/win32/server_loop_test.c']
        sources += [ROOT / 'apps/w32srv' / (name + '.c') for name in
                    ('registry', 'storage', 'defaults', 'directory', 'protocol')]
        subprocess.run([
            'clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
            '-I' + str(ROOT / 'src/kernel/include'),
            '-I' + str(ROOT / 'sdk/win32/include'),
            '-idirafter', str(ROOT / 'sdk/sysroot-overlay/include'),
            *map(str, sources), '-o', str(cls.binary)], check=True)

    def execute(self, mode):
        result = subprocess.run([str(self.binary), str(mode)], capture_output=True,
                                text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS main-loop', result.stdout)
        print(result.stdout, end='')

    def test_session_window_after_idle(self):
        self.execute(0)

    def test_power_window_after_idle(self):
        self.execute(1)

    def test_shutdown_countdown_after_idle(self):
        self.execute(2)


if __name__ == '__main__':
    unittest.main()
