"""f2-108: sequential fake-register and i686 compile checks, no guest/image."""
from pathlib import Path
import os
import runpy
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class UniChromeTests(unittest.TestCase):
    def test_registers_and_lifecycle(self):
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='unichrome-', dir=folder) as tmp:
            binary = Path(tmp) / 'test'
            # As in the part A suite: opt in to ASan where LeakSanitizer can
            # run. Never alter ASAN_OPTIONS or suppress leak checking.
            sanitizer = 'address,undefined' if os.environ.get('CIUKI_DISPLAY_ASAN') else 'undefined'
            subprocess.run([
                'clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=' + sanitizer, '-fno-sanitize-recover=undefined',
                '-I', str(ROOT / 'src/kernel/include'),
                str(ROOT / 'tests/host/unichrome_test.c'),
                str(ROOT / 'src/kernel/drivers/video/unichrome/engine.c'),
                str(ROOT / 'src/kernel/drivers/video/swdib.c'),
                str(ROOT / 'src/kernel/lib/fmt.c'), str(ROOT/'tests/host/display_driver_stubs.c'), '-o', str(binary)
            ], check=True)
            subprocess.run([str(binary)], check=True)

    def test_target_and_linkage(self):
        flags = runpy.run_path(str(ROOT / 'scripts/build_kernel.py'))['CFLAGS']
        folder = ROOT / 'build/host'
        folder.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='unichrome-target-', dir=folder) as tmp:
            objects = []
            for name in ('engine', 'driver'):
                target = Path(tmp) / ('unichrome_' + name + '.o')
                subprocess.run(['clang', *flags, '-I', str(ROOT / 'src/kernel/include'),
                                '-c', str(ROOT / f'src/kernel/drivers/video/unichrome/{name}.c'),
                                '-o', str(target)], check=True)
                objects.append(target)
            # A native port, not a hosted donor library. Check the actual
            # external definitions as well as duplicate exports between units.
            symbols = []
            for target in objects:
                text = subprocess.run(['nm', '-g', '--defined-only', str(target)],
                                      check=True, text=True, capture_output=True).stdout
                symbols += [line.split()[-1] for line in text.splitlines()]
            self.assertEqual(len(symbols), len(set(symbols)))
            self.assertEqual(set(symbols), {'uc_engine_init', 'uc_engine_quarantine',
                                          'uc_engine_probe', 'uc_pci_supported', 'unichrome_driver'})
        builder = (ROOT / 'scripts/build_kernel.py').read_text()
        for name in ('unichrome/engine.c', 'unichrome/driver.c'):
            self.assertIn('"' + name + '"', builder)


if __name__ == '__main__':
    unittest.main()
