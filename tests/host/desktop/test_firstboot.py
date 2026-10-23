"""Production firstboot engine, game registration, render goldens; no guest."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[3]

class FirstbootTests(unittest.TestCase):
    def test_native_services(self):
        out=ROOT/'build/host';out.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='firstboot-native-',dir=out) as temp:
            d=Path(temp).resolve();inc=d/'include/ciuki';inc.mkdir(parents=True)
            for name in ('devices.h','registry.h','display.h'):
                (inc/name).write_bytes((ROOT/'sdk/sysroot-overlay/include/ciuki'/name).read_bytes())
            binary=d/'native-test'
            sources=['tests/host/desktop/firstboot_native_test.c','apps/desktop/firstboot.c','apps/desktop/firstboot_games.c',
                     'apps/desktop/sounds.c','sdk/libreg/registry.c','sdk/libreg/storage.c','sdk/libreg/hardware.c']
            subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-D_POSIX_C_SOURCE=200809L',
                            '-fsanitize='+os.environ.get('CIUKI_FIRSTBOOT_SANITIZERS','address,undefined'),
                            '-fno-sanitize-recover=undefined','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                            '-Wl,--wrap=fsync',f'-DFB_ROOT="{d}"','-include',str(ROOT/'tests/host/desktop/replay_base.h'),
                            '-I'+str(d/'include'),'-I'+str(ROOT/'src/kernel/include'),'-idirafter',str(ROOT/'sdk/sysroot-overlay/include'),
                            *map(lambda s:str(ROOT/s),sources),'-o',str(binary)],check=True)
            subprocess.run([binary],check=True)

    def test_engine_registration_and_frames(self):
        out=ROOT/'build/host';out.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='firstboot-',dir=out) as temp:
            d=Path(temp);(d/'registry').mkdir()
            spec=importlib.util.spec_from_file_location('fb_portrait',ROOT/'apps/desktop/convert_portrait.py')
            converter=importlib.util.module_from_spec(spec);spec.loader.exec_module(converter)
            portrait=d/'portrait.xrgb';portrait.write_bytes(converter.convert(ROOT/'assets/brand/ciuki-logo.png'))
            inc=d/'include/ciuki';inc.mkdir(parents=True)
            for name in ('devices.h','registry.h','display.h'):
                (inc/name).write_bytes((ROOT/'sdk/sysroot-overlay/include/ciuki'/name).read_bytes())
            sources=['apps/desktop/firstboot.c','apps/desktop/firstboot_games.c','tests/host/desktop/firstboot_test.c',
                     'apps/libdeskui/draw.c','apps/desktop/theme.c','apps/desktop/settings.c',
                     'sdk/libreg/registry.c','sdk/libreg/storage.c','sdk/libreg/hardware.c']
            binary=d/'firstboot-test'
            subprocess.run(['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-D_POSIX_C_SOURCE=200809L',
                            '-fsanitize='+os.environ.get('CIUKI_FIRSTBOOT_SANITIZERS','address,undefined'),
                            '-fno-sanitize-recover=undefined','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-include',str(ROOT/'tests/host/desktop/replay_base.h'),
                            '-I'+str(d/'include'),'-I'+str(ROOT/'src/kernel/include'),
                            *map(lambda s:str(ROOT/s),sources),'-o',str(binary)],check=True)
            subprocess.run([binary,d/'registry',portrait],check=True)
            actual={p.stem:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((d/'registry').glob('*.frame'))}
            golden=ROOT/'tests/host/desktop/firstboot-goldens.json'
            if os.environ.get('CIUKI_RECORD_FIRSTBOOT'):
                golden.write_text(json.dumps(actual,indent=2,sort_keys=True)+'\n')
            self.assertEqual(actual,json.loads(golden.read_text()))
            self.assertEqual(len(actual),7)
            spec=importlib.util.spec_from_file_location('fb_review',ROOT/'tests/host/desktop/frame_review.py')
            review=importlib.util.module_from_spec(spec);spec.loader.exec_module(review)
            for p in (d/'registry').glob('*.frame'):review.capture('firstboot-'+p.stem,p.read_bytes(),640,480)

if __name__=='__main__':unittest.main()
