"""f2-118 host-only evidence. Tests run sequentially; no image or guest.

UBSan is the default, matching test_display_driver_model.py. Set
CIUKI_DISPLAY_ASAN=1 for ASan as well, with default leak detection. The register decoder
executes commands independently, then compares pixels with production swdib.
"""
from pathlib import Path
import importlib.util
import ast
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCES = ['drivers/video/s3legacy/engine.c', 'drivers/video/s3legacy/driver.c',
           'drivers/video/s3legacy/platform.c', 'drivers/video/s3trio64/engine.c',
           'drivers/video/s3virge/engine.c']


class S3Tests(unittest.TestCase):
    def test_build_registration_and_notice(self):
        builder = (ROOT / 'scripts/build_kernel.py').read_text()
        for source in SOURCES:
            self.assertIn('"' + source.removeprefix('drivers/video/') + '"', builder)
        names = [Path(s).parent.name + '_' + Path(s).stem + '.o' for s in SOURCES]
        self.assertEqual(len(names), len(set(names)))
        tree = ast.parse((ROOT / 'scripts/build_image.py').read_text())
        function = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'desktop_license_payloads')
        namespace = {'ROOT': ROOT, 'SDK': ROOT / 'sdk/sysroot'}
        exec(compile(ast.Module(body=[function], type_ignores=[]), '<licence inventory>', 'exec'), namespace)
        key = '/system/licenses/haiku-s3-r1beta5-NOTICES.txt'
        self.assertEqual(namespace['desktop_license_payloads']()[key], ROOT / 'docs/licenses/haiku-s3-r1beta5-NOTICES.txt')
        self.assertTrue(namespace['desktop_license_payloads']()[key].is_file())

    def test_resource_glue(self):
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        sanitizer = 'address,undefined' if os.environ.get('CIUKI_DISPLAY_ASAN') else 'undefined'
        with tempfile.TemporaryDirectory(prefix='s3-platform-', dir=folder) as tmp:
            binary = Path(tmp) / 'test'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=' + sanitizer, '-fno-sanitize-recover=all',
                            '-I', str(ROOT / 'src/kernel/include'),
                            str(ROOT / 'tests/host/s3legacy_platform_test.c'), str(ROOT/'tests/host/display_driver_stubs.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_registers_and_goldens(self):
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='s3legacy-', dir=folder) as tmp:
            binary = Path(tmp) / 'test'
            sources = ['tests/host/s3legacy_test.c', 'src/kernel/lib/fmt.c',
                       'src/kernel/drivers/video/registry.c',
                       'src/kernel/drivers/video/swdib.c']
            sources += ['src/kernel/' + s for s in SOURCES if not s.endswith('/platform.c')]
            sanitizer = 'address,undefined' if os.environ.get('CIUKI_DISPLAY_ASAN') else 'undefined'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=' + sanitizer, '-fno-sanitize-recover=all',
                            '-I', str(ROOT / 'src/kernel/include'),
                            *(str(ROOT / s) for s in sources), str(ROOT/'tests/host/display_driver_stubs.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_target_objects(self):
        spec = importlib.util.spec_from_file_location('s3_kernel_flags', ROOT / 'scripts/build_kernel.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='s3-target-', dir=folder) as tmp:
            objects = []
            for source in SOURCES:
                obj = Path(tmp) / (source.replace('/', '_') + '.o')
                subprocess.run(['clang', *module.CFLAGS, '-I', str(ROOT / 'src/kernel/include'),
                                '-c', str(ROOT / 'src/kernel' / source), '-o', str(obj)], check=True)
                objects.append(str(obj))
            # Link relocatably to catch collisions between the two backends.
            subprocess.run(['ld.lld', '-m', 'elf_i386', '-r', *objects,
                            '-o', str(Path(tmp) / 's3-combined.o')], check=True)


if __name__ == '__main__':
    unittest.main()
