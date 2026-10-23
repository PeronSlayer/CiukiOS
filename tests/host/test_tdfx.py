"""f2-117: production driver, register model, swdib goldens; no guest/image.

UBSan is the default, as in test_display_driver_model.py. CIUKI_DISPLAY_ASAN=1
additionally enables ASan with default leak checking. Scenarios run in fresh processes
so same-boot quarantine and durable marker gates need no test-only reset.
"""
from pathlib import Path
import importlib.util
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class TdfxTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='tdfx-', dir=folder)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / 'test'
        sanitizer = 'address,undefined' if os.environ.get('CIUKI_DISPLAY_ASAN') else 'undefined'
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
            '-fsanitize='+sanitizer, '-fno-sanitize-recover=undefined', '-DTDFX_HOST',
            '-I', str(ROOT/'src/kernel/include'), str(ROOT/'tests/host/tdfx_test.c'),
            *(str(ROOT/'src/kernel/drivers/video'/s) for s in
              ('tdfx/engine.c', 'tdfx/driver.c', 'registry.c', 'swdib.c')),
            str(ROOT/'tests/host/display_driver_stubs.c'), '-o', str(cls.binary)], check=True)

    def scenario(self, name):
        subprocess.run([str(self.binary), name], check=True)

    def test_engine(self):
        for name in ('goldens16', 'goldens32', 'trace', 'refusal', 'timeouts',
                     'quarantine', 'cursor16', 'cursor32', 'cursor_faults', 'slices'):
            with self.subTest(name=name):
                self.scenario(name)

    def test_lifecycle(self):
        for name in ('probe', 'start5', 'start9', 'cursorless', 'retained',
                     'marker_read', 'marker_commit', 'marker_short', 'marker_close',
                     'marker_clear', 'safe_mode', 'accel_off', 'bad_vram', 'bad_sli',
                     'bad_layout', 'bad_pixels', 'unsafe_start', 'claim_fail',
                     'map_fail', 'activate_fail', 'stop_fault', 'diagnostic'):
            with self.subTest(name=name):
                self.scenario(name)

    def test_target_objects(self):
        spec = importlib.util.spec_from_file_location('tdfx_flags', ROOT/'scripts/build_kernel.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        objects = []
        for source in ('engine', 'driver'):
            obj = Path(self.temp.name)/('tdfx_'+source+'.o')
            subprocess.run(['clang', *module.CFLAGS, '-I', str(ROOT/'src/kernel/include'),
                '-c', str(ROOT/'src/kernel/drivers/video/tdfx'/(source+'.c')),
                '-o', str(obj)], check=True)
            objects.append(obj)
        # Partial target link also catches duplicate externally defined symbols.
        subprocess.run(['ld.lld', '-m', 'elf_i386', '-r', *(str(o) for o in objects),
            '-o', str(Path(self.temp.name)/'tdfx.o')], check=True)
        builder = (ROOT/'scripts/build_kernel.py').read_text()
        self.assertIn('"tdfx/driver.c"', builder)
        self.assertIn('"tdfx/engine.c"', builder)


if __name__ == '__main__':
    unittest.main()
