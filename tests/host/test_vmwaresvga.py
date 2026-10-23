"""Sequential host-only protocol/lifecycle evidence; no image or guest.

UBSan by default. CIUKI_VMWARESVGA_ASAN=1 adds ASan with default leak detection.
"""
from pathlib import Path
import importlib.util
import hashlib
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / 'src/kernel/drivers/video/vmwaresvga'


class SvgaTests(unittest.TestCase):
    def test_import_pin(self):
        # Exclude only CiukiOS's two-line origin comment, then compare the
        # exact upstream Git blob. The fixture needs no downloaded checkout.
        header = (DRIVER / 'svga_reg.h').read_bytes().split(b'\n', 2)[2]
        blob = b'blob ' + str(len(header)).encode() + b'\0' + header
        self.assertEqual(hashlib.sha1(blob).hexdigest(), '0e1aa17b9bf7b5c3f890582dacf7fa39c537bba6')

    def run_host(self, fixture, extra=()):
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='vmwaresvga-', dir=folder) as tmp:
            binary = Path(tmp) / 'test'
            sanitizer = 'address,undefined' if os.environ.get('CIUKI_VMWARESVGA_ASAN') else 'undefined'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=' + sanitizer, '-fno-sanitize-recover=undefined',
                '-I', str(ROOT / 'src/kernel/include'), str(ROOT / 'tests/host' / fixture),
                str(DRIVER / 'engine.c'), str(ROOT / 'src/kernel/drivers/video/swdib.c'),
                *extra, str(ROOT/'tests/host/display_driver_stubs.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_engine(self):
        self.run_host('vmwaresvga_engine_test.c')

    def test_registry_lifecycle(self):
        self.run_host('vmwaresvga_driver_test.c', [str(ROOT / 'src/kernel/drivers/video/registry.c')])

    def test_target_units(self):
        spec = importlib.util.spec_from_file_location('svga_kernel_flags', ROOT / 'scripts/build_kernel.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        sources = [DRIVER / name for name in ('engine.c', 'driver.c', 'io.c')]
        sources += [ROOT / 'src/kernel/drivers/video/registry.c', ROOT / 'src/kernel/core/hardware_check.c']
        with tempfile.TemporaryDirectory(prefix='vmwaresvga-target-', dir=folder) as tmp:
            for source in sources:
                subprocess.run(['clang', *module.CFLAGS, '-I', str(ROOT / 'src/kernel/include'),
                    '-c', str(source), '-o', str(Path(tmp) / (source.stem + '.o'))], check=True)
        builder = (ROOT / 'scripts/build_kernel.py').read_text()
        for name in ('engine.c', 'driver.c', 'io.c'):
            self.assertIn('"vmwaresvga/' + name + '"', builder)


if __name__ == '__main__':
    unittest.main()
