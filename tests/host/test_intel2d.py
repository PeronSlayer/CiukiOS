"""Intel owned-GTT transport evidence. No guest/image execution."""
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class IntelTests(unittest.TestCase):
    def host(self, source, extra=()):
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='intel2d-', dir=folder) as tmp:
            binary = Path(tmp) / 'test'
            sanitizer = 'address,undefined' if os.environ.get('CIUKI_DISPLAY_ASAN') else 'undefined'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize='+sanitizer, '-fno-sanitize-recover=undefined',
                '-I', str(ROOT/'src/kernel/include'),
                str(ROOT/source),
                str(ROOT/'src/kernel/drivers/video/intel2d/engine.c'),
                str(ROOT/'src/kernel/drivers/video/swdib.c'), *extra, str(ROOT/'tests/host/display_driver_stubs.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)  # ASan mode retains default leak detection

    def test_registers_and_pixels(self):
        self.host('tests/host/intel2d_test.c')

    def test_adapter_lifecycle(self):
        self.host('tests/host/intel2d_adapter_test.c')

    def test_target_translation_units(self):
        spec = importlib.util.spec_from_file_location('intel_kernel_flags', ROOT/'scripts/build_kernel.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='intel2d-target-', dir=folder) as tmp:
            for source in ('engine.c', 'driver.c'):
                subprocess.run(['clang', *module.CFLAGS, '-I', str(ROOT/'src/kernel/include'),
                    '-c', str(ROOT/'src/kernel/drivers/video/intel2d'/source),
                    '-o', str(Path(tmp)/source.replace('.c', '.o'))], check=True)
            builder = (ROOT/'scripts/build_kernel.py').read_text()
            for source in ('intel2d/engine.c', 'intel2d/driver.c'):
                self.assertIn('"'+source+'"', builder)

if __name__ == '__main__':
    unittest.main()
