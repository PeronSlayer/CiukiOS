"""NeoMagic native port: sequential register/golden/lifecycle and target checks."""
from pathlib import Path
import ast
import importlib.util
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
VIDEO = 'src/kernel/drivers/video/neomagic/'

class NeoMagicTests(unittest.TestCase):
    def test_notice_payload(self):
        # Evaluate only this pure inventory function; no builder is executed.
        tree=ast.parse((ROOT/'scripts/build_image.py').read_text())
        function=next(node for node in tree.body
                      if isinstance(node, ast.FunctionDef) and node.name=='desktop_license_payloads')
        namespace={'ROOT':ROOT, 'SDK':ROOT/'build/sdk'}
        exec(compile(ast.Module(body=[function],type_ignores=[]),'<licence inventory>','exec'),namespace)
        sources=namespace['desktop_license_payloads']()
        notice=ROOT/'docs/licenses/neomagic-NOTICES.txt'
        self.assertEqual(sources['/system/licenses/neomagic-NOTICES.txt'],notice)
        self.assertTrue(notice.is_file())
        for name in ('engine.c','cursor.c','regs.h'):
            source=(ROOT/(VIDEO+name)).read_text()
            self.assertIn('Origin:',source)
            self.assertIn('Copyright',source)
            self.assertNotIn('#include "engine.c"',source)

    def test_registers_pixels_and_lifecycle(self):
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='neomagic-', dir=folder) as tmp:
            sanitizer = 'address,undefined' if os.environ.get('CIUKI_DISPLAY_ASAN') else 'undefined'
            for fixture in ('neomagic_engine_test.c', 'neomagic_driver_test.c'):
                binary = Path(tmp) / fixture.removesuffix('.c')
                subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize='+sanitizer, '-fno-sanitize-recover=undefined',
                    '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                    '-I', str(ROOT/'src/kernel/include'), str(ROOT/'tests/host'/fixture),
                    str(ROOT/(VIDEO+'engine.c')), str(ROOT/(VIDEO+'cursor.c')),
                    str(ROOT/'src/kernel/drivers/video/swdib.c'), str(ROOT/'tests/host/display_driver_stubs.c'), '-o', str(binary)], check=True)
                subprocess.run([str(binary)], check=True)

    def test_target_translation_units(self):
        spec = importlib.util.spec_from_file_location('kernel_flags', ROOT/'scripts/build_kernel.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='neomagic-target-', dir=folder) as tmp:
            objects=[]
            for source in ('driver.c', 'engine.c', 'cursor.c'):
                output=Path(tmp)/('neomagic_'+source+'.o')
                subprocess.run(['clang', *module.CFLAGS, '-I', str(ROOT/'src/kernel/include'),
                    '-c', str(ROOT/(VIDEO+source)), '-o', str(output)], check=True)
                objects.append(output)
            # Inspect separately compiled objects: donor .c files are never included.
            symbols=[]
            for obj in objects:
                nm=subprocess.run(['nm', '-g', '--defined-only', str(obj)],
                                  capture_output=True, text=True, check=True).stdout
                symbols.extend(line.split()[-1] for line in nm.splitlines() if line.strip())
            self.assertEqual(len(symbols), len(set(symbols)), 'duplicate external port symbol')
            builder=(ROOT/'scripts/build_kernel.py').read_text()
            self.assertIn('SRC / "drivers/video/neomagic"', builder)
            for source in ('driver.c', 'engine.c', 'cursor.c'):
                self.assertIn('"'+source+'"', builder)

if __name__ == '__main__':
    unittest.main()
