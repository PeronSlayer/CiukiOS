"""Sequential Rage 128 host evidence; no guest or image.

UBSan is the default, matching part A. CIUKI_DISPLAY_ASAN=1 adds ASan with
default leak detection; no sanitizer option is suppressed.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class Rage128Tests(unittest.TestCase):
    def run_fixture(self, fixture, extra=()):
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='rage128-', dir=folder) as tmp:
            binary = Path(tmp) / 'test'
            sanitizer = 'address,undefined' if os.environ.get('CIUKI_DISPLAY_ASAN') else 'undefined'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=' + sanitizer, '-fno-sanitize-recover=undefined',
                '-I', str(ROOT / 'src/kernel/include'),
                str(ROOT / 'tests/host' / fixture),
                str(ROOT / 'src/kernel/drivers/video/rage128/engine.c'),
                *[str(ROOT / p) for p in extra], str(ROOT/'tests/host/display_driver_stubs.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_registers_and_software_goldens(self):
        self.run_fixture('rage128_engine_test.c', ['src/kernel/drivers/video/swdib.c'])

    def test_registry_lifecycle(self):
        self.run_fixture('rage128_driver_test.c', ['src/kernel/lib/fmt.c'])

if __name__ == '__main__':
    unittest.main()
