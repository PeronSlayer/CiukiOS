"""f2-116 host evidence. UBSan by default, optional ASan with default leaks."""
from pathlib import Path
import importlib.util
import os
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]

class CirrusTests(unittest.TestCase):
    def run_host(self, source, extra=()):
        folder=ROOT/'build/host'; folder.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='cirrus-',dir=folder) as directory:
            binary=Path(directory)/'test'
            subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                '-fsanitize='+('address,undefined' if os.environ.get('CIUKI_DISPLAY_ASAN') else 'undefined'),
                '-fno-sanitize-recover=undefined',
                '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                '-I',str(ROOT/'src/kernel/include'),str(ROOT/source),
                str(ROOT/'src/kernel/drivers/video/cirrus/engine.c'),
                *(str(ROOT/s) for s in extra),str(ROOT/'tests/host/display_driver_stubs.c'), '-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

    def test_engine(self):
        self.run_host('tests/host/cirrus_engine_test.c',['src/kernel/drivers/video/swdib.c'])

    def test_adapter(self):
        self.run_host('tests/host/cirrus_driver_test.c',['src/kernel/drivers/video/registry.c'])

    def test_presenter(self):
        self.run_host('tests/host/cirrus_presenter_test.c')

    def test_notice_payload(self):
        spec=importlib.util.spec_from_file_location('cirrus_image_inventory',ROOT/'scripts/build_image.py')
        module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        notices=module.desktop_license_payloads()
        self.assertEqual(notices['/system/licenses/cirrusfb-v6.12-NOTICES.txt'],
                         ROOT/'docs/licenses/cirrusfb-v6.12-NOTICES.txt')

if __name__=='__main__': unittest.main()
