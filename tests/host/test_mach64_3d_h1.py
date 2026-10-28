"""H1 Quake III replay through real GL/GDI/wire and the decoded Mach64 model.

Builds the existing tests/host/win32/gl harness with the Mach64 engine file in
place of the Savage one (test_gl.py is unchanged). UBSan by default;
CIUKI_DISPLAY_ASAN=1 adds ASan/LSan. Run separately from other host suites.
"""
from pathlib import Path
import importlib.util
import os
import re
import subprocess
import sys
import unittest
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from scripts.win32 import gdi32  # noqa: E402
HERE = ROOT / 'tests/host/win32/gl'
OUT = ROOT / 'build/host/win32/gl/mach64'


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location('glbuild', ROOT / 'sdk/libgl/build.py')
    gl = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gl)
    asan = bool(os.environ.get('CIUKI_DISPLAY_ASAN'))
    lib = gl.host_tinygl(sanitize=asan)
    units = [ROOT / 'sdk/win32/gdi32' / n for n in gdi32.SOURCES if n not in ('compiler.c', 'compiler.S')]
    units += [ROOT / 'sdk/win32/services/bytes.c', ROOT / 'tests/host/win32/gdi32/support.c', ROOT / 'tests/host/win32/no_environment.c',
              *(HERE / n for n in ('engine.c', 'census.c', 'amendment.c', 'review.c', 'amendment2.c',
                                   'transport.c', 'fixture.c', 'quake3.c', 'raster.c', 'raster_reference.c'))]
    units += list((ROOT / 'sdk/win32/opengl32').glob('*.c')) + [ROOT / 'sdk/libgl/software/renderer.c']
    units += [HERE / 'hardware.c', ROOT / 'tests/host/mach64_3d_gl_engine.c', HERE / 'kernel_replay.c']
    units += [ROOT / 'src/kernel/drivers/video' / n for n in
              ('render3d.c', 'render3d_scalar.c', 'render3d_native.c', 'render3d_transport.c',
               'mach64/mach64_3d.c', 'mach64/engine.c')]
    units += [ROOT / 'tests/host/mach64_3d_model.c', ROOT / 'src/kernel/lib/fmt.c']
    sanitizer = 'address,undefined' if asan else 'undefined'
    flags = ['-std=c17', '-D_GNU_SOURCE', '-DW32_HOST_MODEL', '-DGL_KERNEL_FAMILY_MACH64', '-O2', '-g', '-ffp-contract=off', '-Wall', '-Wextra',
             '-Werror', '-Wno-unused-function', '-fno-vectorize', '-fno-slp-vectorize',
             '-I' + str(ROOT / 'sdk/win32/include'), '-I' + str(ROOT / 'sdk/libgl/vendor/include'),
             '-I' + str(ROOT / 'src/kernel/include'), '-I' + str(ROOT / 'tests/host'), '-I' + str(OUT),
             '-fsanitize=' + sanitizer, '-fno-sanitize-recover=all', '-DSW_PROFILE']
    fixture = OUT / 'fixture.o'
    subprocess.run(['clang', *flags, '-include', str(HERE / 'aliases.h'), '-Dentry=gl_fixture_entry',
                    '-DwglUseFontBitmapsA=gl_fixture_font', '-c', str(ROOT / 'src/probes/win32_gl/exercise.c'),
                    '-o', str(fixture)], check=True)
    units += [fixture, ROOT / 'src/probes/win32_gl/scenes.c']
    binary = OUT / 'engine'
    subprocess.run(['clang', *flags, *map(str, units), str(lib), '-lm', '-pthread', '-o', str(binary)], check=True)
    # Match the shared GL harness: independent oracle definitions are renamed.
    nm = subprocess.check_output(['nm', '--defined-only', str(binary)], text=True)
    external = re.findall(r'^[0-9a-fA-F]+ [A-Z] (\S+)$', nm, re.M)
    assert len(external) == len(set(external)), 'duplicate external Mach64 GL symbol'
    return binary


class Mach64H1Tests(unittest.TestCase):
    def test_replay(self):
        binary = build()
        result = subprocess.run([str(binary), '--hardware-quake3'], capture_output=True, text=True, timeout=1800)
        print(result.stdout, end='')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        setups = re.findall(r'^GL_MACH64_SETUP width=(\d+) height=(\d+) .* qualification=(-?\d+) qualified=(\d)',
                            result.stdout, re.M)
        # 640x480 qualifies on the 8 MiB model; 1024x768 cannot hold two
        # mode-sized colour buffers, D16 and Q2 textures above the scanout.
        self.assertEqual([(w, h, q) for w, h, _, q in setups], [('640', '480', '1'), ('1024', '768', '0')])
        rows = re.findall(r'^GL_MACH64_H1 scope=(\S+) width=(\d+) height=(\d+) frame=(\d+) gpu_draws=(\d+) '
                          r'gpu_fragments=(\d+) cpu_draws=(\d+)', result.stdout, re.M)
        self.assertEqual(len(rows), 12)
        for scope, width, height, frame, gpu, fragments, cpu in rows:
            self.assertEqual((int(gpu), int(fragments)), (0, 0))
            self.assertEqual(int(cpu), 4600 if scope == 'complete-multipass' else 3000)


if __name__ == '__main__':
    unittest.main()
