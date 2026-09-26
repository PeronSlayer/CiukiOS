#!/usr/bin/env python3
"""Exercise real native mode preview/rollback/commit without changing Win16."""
import argparse
import hashlib
import json
import shutil
import subprocess
import time
from pathlib import Path

from PIL import Image
from qemu_test_full_display_profile import VM
from qemu_test_installed_hdd import FAT16


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--limited-vram', action='store_true')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    disk = out/'target.img'
    shutil.copyfile(args.image, disk)
    volume = f'{disk}@@{FAT16(disk).start}'
    profile = out/'profile.cfg'
    profile.write_bytes(b'0800')
    subprocess.run(['mcopy', '-o', '-i', volume, str(profile),
                    '::SYSTEM/VIDEO/DISPLAY.CFG'], check=True)

    def read(path):
        return subprocess.check_output(['mtype', '-i', volume, '::'+path])

    original_ini = read('WINDOWS/SYSTEM.INI')
    original_profile = read('SYSTEM/VIDEO/DISPLAY.CFG')
    report = {'image_sha256': hashlib.sha256(args.image.read_bytes()).hexdigest(),
              'memory_mib': 128, 'limited_vram': args.limited_vram, 'completed': False}
    vm = VM(disk, out, ('-vga', 'none', '-device',
                       'VGA,vgamem_mb='+('2' if args.limited_vram else '32')+
                       ',xres=2560,yres=1440'), memory=128)
    try:
        vm.wait('CiukiOS SHELL C:\\APPS>', timeout=90)
        if args.limited_vram:
            vm.command('vgasetup desktop 2560', 'Display mode is not supported', timeout=60)
            assert read('SYSTEM/VIDEO/DISPLAY.CFG') == original_profile
            assert read('WINDOWS/SYSTEM.INI') == original_ini
            vm.command('comdemo', 'COM demo via INT21h')
            report['unsupported_mode_preserved_settings'] = True
        else:
            off = vm.offset()
            vm.text('vgasetup desktop 1280')
            vm.wait('[VGASETUP] DESKTOP PREVIEW 1280 x 1024', off, 60)
            Image.open(vm.shot('preview-1280', (1280,1024))).save(out/'preview-1280.png')
            vm.key('esc')
            vm.wait('Preview cancelled; settings preserved.', off, 30)
            vm.wait('CiukiOS SHELL C:\\APPS>', off, 30)
            assert read('SYSTEM/VIDEO/DISPLAY.CFG') == original_profile
            assert read('WINDOWS/SYSTEM.INI') == original_ini
            report['escape_preserved_settings'] = True
            off = vm.offset()
            vm.text('vgasetup desktop 1920')
            vm.wait('[VGASETUP] DESKTOP PREVIEW 1920 x 1080', off, 60)
            started = time.monotonic()
            vm.wait('Preview cancelled; settings preserved.', off, 20)
            elapsed = time.monotonic()-started
            assert 10 <= elapsed <= 16, elapsed
            assert read('SYSTEM/VIDEO/DISPLAY.CFG') == original_profile
            assert read('WINDOWS/SYSTEM.INI') == original_ini
            vm.wait('CiukiOS SHELL C:\\APPS>', off, 30)
            report['timeout_seconds'] = elapsed
            off = vm.offset()
            vm.text('vgasetup desktop 2560')
            vm.wait('[VGASETUP] DESKTOP PREVIEW 2560 x 1440', off, 60)
            Image.open(vm.shot('preview-2560', (2560,1440))).save(out/'preview-2560.png')
            time.sleep(.3)
            vm.key('ret')
            vm.wait('Desktop resolution saved; Windows profile preserved.', off, 30)
            vm.wait('CiukiOS SHELL C:\\APPS>', off, 30)
            assert read('SYSTEM/VIDEO/DISPLAY.CFG') == b'2560'
            assert read('WINDOWS/SYSTEM.INI') == original_ini
            vm.shot('dos-2560', (2560,1440))
            vm.command('comdemo', 'COM demo via INT21h')
            vm.command('vgasetup desktop 9999', 'usage: vgasetup desktop')
            assert read('SYSTEM/VIDEO/DISPLAY.CFG') == b'2560'
            off = vm.offset()
            vm.text('exit')
            vm.wait('[DESKTOP] READY', off, 60)
            Image.open(vm.shot('desktop-2560', (2560,1440))).save(out/'desktop-2560.png')
            report['confirmed_mode'] = [2560,1440]
            report['windows_profile_unchanged'] = True
        report['completed'] = True
    except Exception:
        try:
            vm.shot('failure')
            (out/'registers.log').write_bytes(vm.hmp('info registers'))
        except (OSError,TimeoutError):
            pass
        raise
    finally:
        vm.close()
        (out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2),flush=True)


if __name__ == '__main__':
    main()
