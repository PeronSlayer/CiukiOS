"""Sequential native-port tests. Like part A, UBSan is the default;
CIUKI_DISPLAY_ASAN=1 adds ASan with its unchanged default leak checks.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class IntelLegacyTests(unittest.TestCase):
    def test_memory_boundary(self):
        folder=ROOT/'build/host'
        folder.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='i810-memory-',dir=folder) as tmp:
            binary=Path(tmp)/'test'
            sanitizer='address,undefined' if os.environ.get('CIUKI_DISPLAY_ASAN') else 'undefined'
            subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                '-fsanitize='+sanitizer,'-fno-sanitize-recover=all',
                '-I',str(ROOT/'src/kernel/include'),str(ROOT/'tests/host/i810_memory_test.c'),
                '-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

    def test_registers_pixels_and_lifecycle(self):
        folder=ROOT/'build/host'
        folder.mkdir(parents=True,exist_ok=True)
        sources=['tests/host/intel_legacy_video_test.c','src/kernel/drivers/video/native.c',
                 'src/kernel/drivers/video/i810/driver.c','src/kernel/drivers/video/i740/driver.c',
                 'src/kernel/drivers/video/registry.c','src/kernel/drivers/video/swdib.c']
        with tempfile.TemporaryDirectory(prefix='intel-legacy-',dir=folder) as tmp:
            binary=Path(tmp)/'test'
            sanitizer='address,undefined' if os.environ.get('CIUKI_DISPLAY_ASAN') else 'undefined'
            subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror',
                '-fsanitize='+sanitizer,'-fno-sanitize-recover=all',
                '-I',str(ROOT/'src/kernel/include'),*(str(ROOT/s) for s in sources),
                str(ROOT/'tests/host/display_driver_stubs.c'), '-o',str(binary)],check=True)
            cases=['core','nomination','production','no-cursor','safe','disabled','map-refusal',
                   'claim-refusal','pitch-refusal','offset-refusal','decode-refusal','owner-refusal',
                   'ram-refusal','cursor-busy-refusal','probe-timeout','comparison','timeout','frozen',
                   'cancel','dead','submitted','cursor-fault','legacy-marker','own-marker',
                   'read-marker','commit-marker','close-marker','short-marker','readonly-marker']
            for chip in ['i810','i740']:
                for case in cases+(['reservation','ring-refusal','gtt-visible-refusal','gtt-alias-refusal','fence-refusal','master-refusal','full-gtt-refusal','beyond-aperture-refusal','alloc-refusal','pin-refusal','aperture-refusal','aperture32','firmware-ring-free','restore-refusal','lease-loss'] if chip=='i810' else []):
                    with self.subTest(chip=chip,case=case):
                        subprocess.run([str(binary),chip,case],check=True)

if __name__=='__main__': unittest.main()
