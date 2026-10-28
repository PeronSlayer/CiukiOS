"""f7-27 Mach64 3D backend: UBSan by default, CIUKI_DISPLAY_ASAN=1 adds ASan/LSan.

No sanitizer or leak-detection option is disabled, matching test_render3d.py.
Sequential, host-only: no QEMU, no image.
"""
from pathlib import Path
import os
import runpy
import subprocess
import unittest
ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/host/mach64_3d'
VIDEO = 'src/kernel/drivers/video/'
COMMON = ['tests/host/mach64_3d_model.c', VIDEO + 'mach64/mach64_3d.c', VIDEO + 'mach64/engine.c',
          VIDEO + 'render3d.c', VIDEO + 'render3d_scalar.c']


def build(name, sources):
    OUT.mkdir(parents=True, exist_ok=True)
    sanitizer = 'address,undefined' if os.environ.get('CIUKI_DISPLAY_ASAN') else 'undefined'
    binary = OUT / name
    subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=' + sanitizer, '-fno-sanitize-recover=all',
                    '-I', str(ROOT / 'src/kernel/include'),
                    *[str(ROOT / s) for s in sources], '-lm', '-o', str(binary)], check=True)
    return binary


class Mach64ThreeDTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.backend = build('backend', ['tests/host/mach64_3d_backend_test.c', *COMMON])

    def run_backend(self, *args):
        subprocess.run([str(self.backend), *args], check=True, timeout=900)

    def test_encoders(self):
        self.run_backend('encoders')

    def test_differential(self):
        self.run_backend('differential')

    def test_qualification(self):
        self.run_backend('qualification')

    def test_containment(self):
        for variant in range(14):
            self.run_backend('fault', str(variant))

    def test_fuzz(self):
        self.run_backend('fuzz')

    def test_fallback(self):
        self.run_backend('fallback')

    def test_owner(self):
        binary = build('owner', ['tests/host/mach64_3d_owner_test.c', *COMMON,
                                 VIDEO + 'mach64/mach64_3d_owner.c', VIDEO + 'attempt.c',
                                 'src/kernel/lib/fmt.c'])
        for variant in range(21):
            subprocess.run([str(binary), str(variant)], check=True, timeout=900)

    def test_target(self):
        # Real i686 kernel flags (frame limit included), no link or image.
        flags = runpy.run_path(str(ROOT / 'scripts/build_kernel.py'))['CFLAGS']
        symbols = set()
        for name in ('engine.c', 'driver.c', 'mach64_3d.c', 'mach64_3d_owner.c'):
            obj = OUT / ('target_' + name + '.o')
            subprocess.run(['clang', *flags, '-I', str(ROOT / 'src/kernel/include'), '-c',
                            str(ROOT / VIDEO / 'mach64' / name), '-o', str(obj)], check=True)
            for line in subprocess.check_output(['nm', '--defined-only', '--extern-only', str(obj)],
                                                text=True).splitlines():
                symbol = line.split()[-1]
                self.assertNotIn(symbol, symbols)
                symbols.add(symbol)
            dis = subprocess.check_output(['objdump', '-d', str(obj)], text=True)
            self.assertNotRegex(dis, r'\b(?:xmm\d|mm[0-7]|fld|fst|fadd|fmul|fdiv)\b')
        source = (ROOT / VIDEO / 'mach64/mach64_3d.c').read_text()
        self.assertNotIn('panic(', source)
        self.assertNotIn('panic(', (ROOT / VIDEO / 'mach64/mach64_3d_owner.c').read_text())
        print(f'PASS mach64 i686: four objects, {len(symbols)} unique symbols, frame limit, no FPU/SIMD, no panic paths')


if __name__ == '__main__':
    unittest.main()
