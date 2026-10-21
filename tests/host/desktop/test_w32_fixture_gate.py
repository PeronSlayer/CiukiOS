"""Shared gate readiness and failure paths, with leak detection preserved."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]


class FixtureGateTests(unittest.TestCase):
    def test_deferred_spawn_and_failures(self):
        out = ROOT/'build/host/desktop-startup'
        out.mkdir(parents=True, exist_ok=True)
        binary = out/'w32-fixture-gate'
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-include', str(ROOT/'tests/host/desktop/replay_base.h'),
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-I'+str(ROOT/'sdk/win32/include'), '-I'+str(ROOT/'apps/desktop'),
                        '-I'+str(ROOT/'src/kernel/include'), '-idirafter', str(ROOT/'sdk/sysroot-overlay/include'),
                        str(ROOT/'tests/host/desktop/w32_fixture_gate_test.c'), '-o', str(binary)], check=True)
        result = subprocess.run([binary], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertIn('PASS shared gate:', result.stdout)


if __name__ == '__main__':
    unittest.main()
