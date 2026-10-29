"""Sequential f2-110 host evidence; optional ASan retains default leak checks."""
from pathlib import Path
import importlib.util
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class MatroxTests(unittest.TestCase):
    def run_host(self, fixture):
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='matrox-', dir=folder) as temp:
            binary = Path(temp) / 'test'
            sanitizer = 'address,undefined' if os.environ.get('CIUKI_MATROX_ASAN') else 'undefined'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize='+sanitizer, '-fno-sanitize-recover=undefined',
                '-I', str(ROOT/'src/kernel/include'), str(ROOT/'tests/host'/fixture),
                str(ROOT/'src/kernel/drivers/video/matrox/engine.c'),
                str(ROOT/'src/kernel/drivers/video/swdib.c'), str(ROOT/'src/kernel/lib/fmt.c'),
                str(ROOT/'tests/host/display_driver_stubs.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_engine(self):
        self.run_host('matrox_engine_test.c')

    def test_adapter(self):
        self.run_host('matrox_adapter_test.c')

    def test_target_objects(self):
        spec = importlib.util.spec_from_file_location('matrox_flags', ROOT/'scripts/build_kernel.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        folder = ROOT/'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='matrox-target-', dir=folder) as temp:
            objects = []
            for name in ('engine', 'driver'):
                obj = Path(temp)/('matrox_'+name+'.o')
                subprocess.run(['clang', *module.CFLAGS, '-I', str(ROOT/'src/kernel/include'),
                    '-c', str(ROOT/f'src/kernel/drivers/video/matrox/{name}.c'), '-o', str(obj)], check=True)
                objects.append(obj)
            # No upstream .c includes are used; still check the port's target
            # objects for duplicate external definitions and unexpected exports.
            result = subprocess.run(['nm', '-g', '--defined-only', *map(str, objects)],
                                    check=True, text=True, capture_output=True).stdout
            symbols = [line.split()[-1] for line in result.splitlines() if len(line.split()) == 3]
            self.assertEqual(len(symbols), len(set(symbols)), result)
            self.assertEqual(set(symbols), {'matrox_driver', 'matrox_probe', 'matrox_mode',
                'matrox_engine_init', 'matrox_engine_prove', 'matrox_engine_quarantine', 'matrox_mono',
                'matrox_engine_reserve', 'matrox_engine_sync'})
            builder = (ROOT/'scripts/build_kernel.py').read_text()
            self.assertIn('SRC / "drivers/video/matrox"', builder)
            self.assertIn('("engine.c", "driver.c", "matrox3d.c", "matrox3d_owner.c")', builder)

if __name__ == '__main__':
    unittest.main()
