"""f7-25 MMIO model, owner, target objects and production GL H1 replay.

Run sequentially with nice -n 19. Optional ASan preserves leak detection.
"""
from pathlib import Path
import importlib.util
import os
import runpy
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/host/matrox3d'
VIDEO = 'src/kernel/drivers/video/'
COMMON = [VIDEO + p for p in ('matrox/matrox3d.c', 'matrox/engine.c',
                              'render3d.c', 'render3d_scalar.c')]

class Matrox3DTests(unittest.TestCase):
    def build(self, name, sources):
        OUT.mkdir(parents=True, exist_ok=True)
        sanitizer = 'address,undefined' if os.environ.get('CIUKI_MATROX_ASAN') else 'undefined'
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-fsanitize=' + sanitizer, '-fno-sanitize-recover=all',
                        '-I', str(ROOT/'src/kernel/include'),
                        *[str(ROOT/p) for p in sources], '-lm', '-o', str(OUT/name)], check=True)
        return OUT/name

    def test_differential_and_faults(self):
        binary = self.build('test', ['tests/host/matrox3d_test.c', *COMMON])
        subprocess.run([str(binary)], check=True)

    def test_fullsize_precision(self):
        binary = self.build('fullsize', ['tests/host/matrox3d_test.c', *COMMON])
        subprocess.run([str(binary), 'fullsize'], check=True)

    def test_service(self):
        binary = self.build('service', ['tests/host/matrox3d_service_test.c', *COMMON,
                                        VIDEO+'render3d_native.c'])
        subprocess.run([str(binary)], check=True)

    def test_owner(self):
        binary = self.build('owner', ['tests/host/matrox3d_owner_test.c', *COMMON,
            VIDEO+'matrox/matrox3d_owner.c', VIDEO+'render3d_native.c',
            VIDEO+'attempt.c', 'src/kernel/lib/fmt.c'])
        for variant in range(18):
            subprocess.run([str(binary), str(variant)], check=True)

    def test_target(self):
        OUT.mkdir(parents=True, exist_ok=True)
        flags = runpy.run_path(str(ROOT/'scripts/build_kernel.py'))['CFLAGS']
        objects = []
        for name in ('driver', 'engine', 'matrox3d', 'matrox3d_owner'):
            obj = OUT/(name+'.o')
            subprocess.run(['clang', *flags, '-I', str(ROOT/'src/kernel/include'), '-c',
                str(ROOT/VIDEO/'matrox'/f'{name}.c'), '-o', str(obj)], check=True)
            dis = subprocess.check_output(['objdump', '-d', str(obj)], text=True)
            self.assertNotRegex(dis, r'\b(?:xmm\d|mm[0-7]|fld|fst|fadd|fmul|fdiv)\b')
            objects.append(str(obj))
        result = subprocess.check_output(['nm', '-g', '--defined-only', *objects], text=True)
        symbols = [line.split()[-1] for line in result.splitlines() if len(line.split()) == 3]
        self.assertEqual(len(symbols), len(set(symbols)), result)
        print('PASS Matrox four i686 objects: frame <=4096, no FP/SIMD, unique external symbols')

    def test_h1(self):
        # Reuse the exact H1 workload, production GL/GDI/provider/wire and
        # measurement hooks; replace only the family backend and fake silicon.
        sys.path.insert(0, str(ROOT))
        spec = importlib.util.spec_from_file_location('matrox_h1_gl', ROOT/'tests/host/win32/gl/test_gl.py')
        gl = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(gl)
        gl.GLTests.build(sanitize=True, kernel_engine='matrox_kernel_engine.c',
            backend=('matrox/matrox3d.c', 'matrox/engine.c'), family='MATROX')
        result = subprocess.run([str(gl.GLTests.binary), '--hardware-quake3'],
                                text=True, capture_output=True, timeout=900)
        print(result.stdout, end='')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        rows = [line for line in result.stdout.splitlines() if line.startswith('GL_MATROX_H1 ')]
        self.assertEqual(len(rows), 12)

if __name__ == '__main__':
    unittest.main()
