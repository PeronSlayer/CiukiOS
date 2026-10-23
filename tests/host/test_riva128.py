"""Sequential NV3 evidence. UBSan by default, CIUKI_DISPLAY_ASAN=1 adds ASan.
Leak checking is never disabled (same sanitizer policy as the part A suite).
"""
from pathlib import Path
import importlib.util
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class Riva128Tests(unittest.TestCase):
    def host(self, source):
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='riva128-', dir=folder) as tmp:
            binary = Path(tmp) / 'test'
            sanitizer = 'address,undefined' if os.environ.get('CIUKI_DISPLAY_ASAN') else 'undefined'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-function', '-fsanitize='+sanitizer, '-fno-sanitize-recover=undefined',
                '-I', str(ROOT/'src/kernel/include'), str(ROOT/source),
                str(ROOT/'src/kernel/drivers/video/riva128/engine.c'),
                str(ROOT/'src/kernel/drivers/video/swdib.c'),
                str(ROOT/'tests/host/display_driver_stubs.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_engine(self):
        self.host('tests/host/riva128_test.c')

    def test_adapter(self):
        self.host('tests/host/riva128_adapter_test.c')

    def test_target(self):
        spec = importlib.util.spec_from_file_location('kernel_flags', ROOT/'scripts/build_kernel.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        folder = ROOT/'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='riva128-target-', dir=folder) as tmp:
            objects=[]
            for name in ('engine','driver'):
                obj=Path(tmp)/(name+'.o')
                subprocess.run(['clang', *module.CFLAGS, '-I', str(ROOT/'src/kernel/include'),
                    '-c', str(ROOT/f'src/kernel/drivers/video/riva128/{name}.c'), '-o', str(obj)], check=True)
                objects.append(obj)
            symbols=subprocess.check_output(['nm', '-g', '--defined-only', *map(str,objects)], text=True)
            names=[line.split()[-1] for line in symbols.splitlines() if len(line.split())==3]
            self.assertEqual(len(names), len(set(names)), 'duplicate backend external symbol')

if __name__ == '__main__':
    unittest.main()
