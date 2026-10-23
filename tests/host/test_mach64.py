"""Sequential native-driver tests; UBSan, optional ASan with default leak checks."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class Mach64Tests(unittest.TestCase):
    def run_host(self, source, extra=(), page=False):
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='mach64-', dir=folder) as tmp:
            binary = Path(tmp) / 'test'
            if page:
                code = (ROOT / 'src/kernel/core/hardware_check.c').read_text()
                first = code.index('static void accelerator_compare(')
                last = code.index('\nstatic void audio_check(', first)
                (Path(tmp) / 'display_check_page.inc').write_text(code[first:last])
            sanitizer = 'address,undefined' if os.environ.get('CIUKI_DISPLAY_ASAN') else 'undefined'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=' + sanitizer, '-fno-sanitize-recover=undefined',
                '-I', str(ROOT / 'src/kernel/include'), '-I', tmp, str(ROOT / source),
                str(ROOT / 'src/kernel/drivers/video/mach64/engine.c'),
                *(str(ROOT / s) for s in extra), str(ROOT/'tests/host/display_driver_stubs.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_engine(self):
        self.run_host('tests/host/mach64_engine_test.c', ['src/kernel/drivers/video/swdib.c'])

    def test_registry_and_framebuffer(self):
        self.run_host('tests/host/mach64_driver_test.c', ['src/kernel/lib/fmt.c'])

    def test_hardware_check_page(self):
        self.run_host('tests/host/mach64_hardware_check_test.c', ['src/kernel/lib/fmt.c'], page=True)

if __name__ == '__main__':
    unittest.main()
