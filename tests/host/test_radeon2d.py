"""Sequential Radeon port evidence. UBSan; CIUKI_DISPLAY_ASAN=1 adds ASan.

ASan retains its default leak checks, as in test_display_driver_model.py.
"""
from pathlib import Path
import importlib.util
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCES = ['src/kernel/drivers/video/radeon2d/' + name
           for name in ('engine.c', 'cursor.c', 'driver.c')]


class RadeonTests(unittest.TestCase):
    def test_register_model_and_presenter(self):
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='radeon-', dir=folder) as tmp:
            binary = Path(tmp) / 'test'
            sanitizer = 'address,undefined' if os.environ.get('CIUKI_DISPLAY_ASAN') else 'undefined'
            subprocess.run(['clang', '-std=gnu17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=' + sanitizer, '-fno-sanitize-recover=undefined',
                            '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                            '-I', str(ROOT / 'src/kernel/include'),
                            str(ROOT / 'tests/host/radeon2d_test.c'),
                            *(str(ROOT / path) for path in SOURCES[:2]), str(ROOT/'tests/host/display_driver_stubs.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_target_objects(self):
        spec = importlib.util.spec_from_file_location('radeon_kernel_flags', ROOT / 'scripts/build_kernel.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='radeon-target-', dir=folder) as tmp:
            objects = []
            for path in SOURCES:
                obj = Path(tmp) / (Path(path).stem + '.o')
                subprocess.run(['clang', *module.CFLAGS, '-I', str(ROOT / 'src/kernel/include'),
                                '-c', str(ROOT / path), '-o', str(obj)], check=True)
                objects.append(obj)
            symbols = subprocess.check_output(['nm', '-g', '--defined-only', *map(str, objects)], text=True)
            names = [line.split()[-1] for line in symbols.splitlines() if len(line.split()) == 3]
            self.assertEqual(len(names), len(set(names)), 'duplicate external symbols')
            print(f'i686 target: 3 objects, canonical CFLAGS, nm: {len(names)} unique external definitions PASS')


if __name__ == '__main__':
    unittest.main()
