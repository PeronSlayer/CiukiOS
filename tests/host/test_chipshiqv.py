"""f2-122 register/pixel/lifecycle evidence, sequential and guest-free.

UBSan by default; CIUKI_DISPLAY_ASAN=1 adds ASan with default leak checking,
as in the part A model suite. No sanitizer options are suppressed.
"""
from pathlib import Path
import importlib.util
import json
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / 'src/kernel/drivers/video/chipshiqv'


class HiQVTests(unittest.TestCase):
    def test_registers_pixels_and_lifecycle(self):
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='chipshiqv-', dir=folder) as tmp:
            binary = Path(tmp) / 'test'
            sanitizer = 'address,undefined' if os.environ.get('CIUKI_DISPLAY_ASAN') else 'undefined'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=' + sanitizer, '-fno-sanitize-recover=undefined',
                            '-I', str(ROOT / 'src/kernel/include'),
                            str(ROOT / 'tests/host/chipshiqv_test.c'), str(DRIVER / 'hiqv_engine.c'),
                            str(ROOT / 'src/kernel/drivers/video/swdib.c'), str(ROOT/'tests/host/display_driver_stubs.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_target_objects_and_unique_symbols(self):
        spec = importlib.util.spec_from_file_location('hiqv_kernel_flags', ROOT / 'scripts/build_kernel.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='chipshiqv-target-', dir=folder) as tmp:
            symbols = set()
            for name in ('hiqv_engine.c', 'chipshiqv.c'):
                obj = Path(tmp) / ('chipshiqv_' + Path(name).stem + '.o')
                subprocess.run(['clang', *module.CFLAGS, '-I', str(ROOT / 'src/kernel/include'),
                                '-c', str(DRIVER / name), '-o', str(obj)], check=True)
                output = subprocess.check_output(['nm', '-g', '--defined-only', str(obj)], text=True)
                current = {line.split()[-1] for line in output.splitlines()}
                self.assertFalse(symbols & current, 'duplicate native driver symbols')
                symbols |= current
            self.assertIn('chipshiqv_driver', symbols)
            self.assertIn('hiqv_engine_check', symbols)
        builder = (ROOT / 'scripts/build_kernel.py').read_text()
        self.assertIn('"drivers/video/chipshiqv"', builder)
        for name in ('hiqv_engine.c', 'chipshiqv.c'):
            self.assertIn('"' + name + '"', builder)

    def test_notice_is_installed_and_identity_is_generic(self):
        spec = importlib.util.spec_from_file_location('hiqv_image', ROOT / 'scripts/build_image.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        payloads = module.desktop_license_payloads()
        self.assertEqual(payloads['/system/licenses/chipshiqv-NOTICES.txt'],
                         ROOT / 'docs/licenses/chipshiqv-NOTICES.txt')
        self.assertEqual(payloads['/system/licenses/chipshiqv-import.json'], DRIVER / 'import.json')
        adapter = (DRIVER / 'chipshiqv.c').read_text()
        manifest = json.loads((DRIVER / 'import.json').read_text())
        self.assertEqual(manifest['revision'], '9187d76bd4f8d500fc3403b6d4a7bef55508bf2b')
        self.assertEqual({item['path'] for item in manifest['files']},
                         {'src/ct_accel.c', 'src/ct_cursor.c', 'src/ct_driver.c', 'src/ct_BltHiQV.h'})
        for name in manifest['local_files']:
            source = (DRIVER / name).read_text()
            self.assertIn(manifest['revision'], source)
            self.assertIn('Permission to use, copy, modify, distribute, and sell', source)
        self.assertIn('.id="chipshiqv",.name="Chips & Technologies HiQV"', adapter)
        self.assertNotIn('panic(', adapter)
        self.assertNotIn('panic(', (DRIVER / 'hiqv_engine.c').read_text())


if __name__ == '__main__':
    unittest.main()
