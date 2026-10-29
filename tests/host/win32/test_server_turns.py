"""Replay overlapping session/power turns and the actual service wait loop."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]


class ServerTurnsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        out = ROOT / 'build/host/win32/server-turns'
        out.mkdir(parents=True, exist_ok=True)
        cls.binary = out / 'server-turns'
        sources = [ROOT / 'tests/host/win32/server_turns_test.c']
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
        self.assertIn('PASS ', result.stdout)
        print(result.stdout, end='')

    def test_power_wake_after_session_reset(self):
        self.execute(0)

    def test_power_wake_after_session_cancel(self):
        self.execute(1)

    def test_power_wake_after_session_completion(self):
        self.execute(2)

    def test_power_wake_after_session_expiry(self):
        self.execute(3)

    def test_main_loop_ignores_held_power_deadline(self):
        self.execute(4)

    def test_main_loop_keeps_session_deadline(self):
        self.execute(5)

    def test_main_loop_keeps_commit_deadline(self):
        self.execute(6)


if __name__ == '__main__':
    unittest.main()
