"""f2-113 sequential native-port evidence; no image or guest execution.

Like the part A display suite, UBSan is the default. CIUKI_DISPLAY_ASAN=1
adds ASan with its default leak checking; no sanitizer option is suppressed.
"""
from pathlib import Path
import importlib.util
import json
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
DRIVER = 'src/kernel/drivers/video/trident/'

class TridentTests(unittest.TestCase):
    def run_host(self, test, sources=()):
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='trident-', dir=folder) as tmp:
            binary = Path(tmp) / 'test'
            sanitizer = 'address,undefined' if os.environ.get('CIUKI_DISPLAY_ASAN') else 'undefined'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize='+sanitizer, '-fno-sanitize-recover=all',
                '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                '-I', str(ROOT/'src/kernel/include'), '-idirafter', str(ROOT/'sdk/sysroot-overlay/include'),
                str(ROOT/'tests/host'/test), str(ROOT/(DRIVER+'engine.c')),
                str(ROOT/'src/kernel/drivers/video/swdib.c'),
                *(str(ROOT/s) for s in sources), str(ROOT/'tests/host/display_driver_stubs.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_engine(self):
        self.run_host('trident_engine_test.c')

    def test_lifecycle(self):
        self.run_host('trident_driver_test.c')

    def test_device_manager_name(self):
        self.run_host('trident_device_ui_test.c', ['apps/control/display.c'])

    def test_packaged_notices(self):
        spec = importlib.util.spec_from_file_location('tb_image_payloads', ROOT/'scripts/build_image.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        payloads=module.desktop_license_payloads()
        for name in ('trident-blade-NOTICES.txt', 'trident-blade-GPL-2.0.txt', 'trident-blade-import.json'):
            self.assertTrue(payloads['/system/licenses/'+name].is_file())
        manifest=json.loads(payloads['/system/licenses/trident-blade-import.json'].read_text())
        self.assertEqual(manifest['revision'], 'v6.12')
        self.assertEqual(len(manifest['sources']), 2)

    def test_target_objects(self):
        spec = importlib.util.spec_from_file_location('tb_kernel_flags', ROOT/'scripts/build_kernel.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        folder = ROOT/'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='trident-target-', dir=folder) as tmp:
            objects=[]
            for name in ('engine.c', 'driver.c'):
                obj=Path(tmp)/('trident_'+name+'.o')
                subprocess.run(['clang', *module.CFLAGS, '-I', str(ROOT/'src/kernel/include'),
                    '-c', str(ROOT/(DRIVER+name)), '-o', str(obj)], check=True)
                objects.append(obj)
            symbols=[]
            for obj in objects:
                output=subprocess.check_output(['nm', '-g', '--defined-only', str(obj)], text=True)
                symbols.extend(line.split()[-1] for line in output.splitlines() if line.strip())
            self.assertEqual(len(symbols), len(set(symbols)))
        builder=(ROOT/'scripts/build_kernel.py').read_text()
        self.assertIn('SRC / "drivers/video/trident"', builder)
        self.assertIn('("engine.c", "driver.c")', builder)

if __name__ == '__main__': unittest.main()
