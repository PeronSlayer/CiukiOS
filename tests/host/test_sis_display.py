"""SiS native host evidence. Sequential UBSan/ASan, default leak checking."""
from pathlib import Path
import importlib.util
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCES = ['src/kernel/drivers/video/sis/engine.c',
           'src/kernel/drivers/video/sis/cursor.c']

class SisDisplayTests(unittest.TestCase):
    def test_target_linkage(self):
        spec = importlib.util.spec_from_file_location('sis_kernel_flags', ROOT / 'scripts/build_kernel.py')
        builder = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(builder)
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='sis-link-', dir=folder) as tmp:
            objects, symbols = [], set()
            for name in ('sis/engine.c', 'sis/cursor.c', 'sis/register.c'):
                source = ROOT / 'src/kernel/drivers/video' / name
                obj = Path(tmp) / (source.parent.name + '_' + source.stem + '.o')
                self.assertNotIn(obj, objects)
                objects.append(obj)
                self.assertIn('"' + name + '"', (ROOT / 'scripts/build_kernel.py').read_text())
                subprocess.run(['clang', *builder.CFLAGS, '-I', str(ROOT / 'src/kernel/include'),
                                '-c', str(source), '-o', str(obj)], check=True)
                output = subprocess.check_output(['nm', '-g', '--defined-only', str(obj)], text=True)
                for line in output.splitlines():
                    symbol = line.split()[-1]
                    self.assertNotIn(symbol, symbols, 'duplicate external symbol: ' + symbol)
                    symbols.add(symbol)
            subprocess.run(['ld.lld', '-m', 'elf_i386', '-r', *(str(p) for p in objects),
                            '-o', str(Path(tmp) / 'sis.o')], check=True)
            print(f'SiS target: {len(objects)} collision-free objects, {len(symbols)} unique external symbols; partial link PASS')

    def test_registers_and_pixels(self):
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='sis-', dir=folder) as tmp:
            binary = Path(tmp) / 'test'
            subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=' + os.environ.get('CIUKI_SIS_SANITIZERS', 'address,undefined'),
                            '-fno-sanitize-recover=undefined',
                            '-I', str(ROOT / 'src/kernel/include'),
                            str(ROOT / 'tests/host/sis_display_test.c'),
                            *(str(ROOT / s) for s in SOURCES),
                            str(ROOT / 'src/kernel/drivers/video/swdib.c'),
                            str(ROOT / 'src/kernel/lib/fmt.c'), str(ROOT/'tests/host/display_driver_stubs.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

if __name__ == '__main__':
    unittest.main()
