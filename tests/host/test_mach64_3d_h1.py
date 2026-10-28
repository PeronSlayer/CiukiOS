"""H1 Quake III replay through real GL/GDI/wire and the decoded Mach64 model.

Selects Mach64 through the shared tests/host/win32/gl family table. UBSan by default;
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
HERE = ROOT / 'tests/host/win32/gl'


def build():
    spec = importlib.util.spec_from_file_location('mach64_h1_gl', HERE / 'test_gl.py')
    gl = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gl)
    gl.GLTests.build(sanitize=bool(os.environ.get('CIUKI_DISPLAY_ASAN')),
                     ubsan=True, kernel_family='mach64')
    return gl.GLTests.binary


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
