#!/usr/bin/env python3
"""Exercise the real display menu with stalled discovery or a rejected preview."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import time

from PIL import Image
from qemu_test_full_display_profile import VM
from qemu_test_installed_hdd import FAT16


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--image', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--expect-stall', action='store_true')
    ap.add_argument('--reject-preview', action='store_true')
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    disk = out/'target.img'
    shutil.copyfile(args.image, disk)
    fault = out/'PROBEFLT.COM'
    subprocess.run(['nasm','-f','bin','scripts/fixtures/vgasetup_probe_fault.asm',
                    *(['-DREJECT_PREVIEW=1'] if args.reject_preview else []),
                    '-o',str(fault)], check=True)
    volume = f'{disk}@@{FAT16(disk).start}'
    subprocess.run(['mcopy','-o','-i',volume,str(fault),'::APPS/PROBEFLT.COM'], check=True)
    profile = out/'DISPLAY.CFG'
    profile.write_bytes(b'AUTO')
    subprocess.run(['mcopy','-o','-i',volume,str(profile),'::SYSTEM/VIDEO/DISPLAY.CFG'],check=True)
    vm = VM(disk,out,('-debugcon',f'file:{out}/probe.log'))
    result = {}
    try:
        vm.wait('CiukiOS SHELL C:\\APPS>',timeout=90)
        vm.command('probeflt','[SETUP-PROBE-FAULT] installed')
        off = vm.offset()
        vm.text('vgasetup')
        if args.expect_stall:
            deadline = time.monotonic()+10
            while (out/'probe.log').read_bytes() != b'P':
                assert time.monotonic()<deadline, 'original did not enter discovery'
                time.sleep(.1)
            assert b'MENU READY' not in vm.serial.read_bytes()[off:]
            result['reproduced'] = 'original VGASETUP stalls before showing its menu'
        else:
            vm.wait('[VGASETUP] MENU READY',off,10)
            vm.hmp(f'pmemsave 0xb8000 4000 "{out}/menu.vram"')
            text = (out/'menu.vram').read_bytes()[::2].decode('cp437')
            assert '800' in text and '1024' in text and 'ENTER' in text.upper(), text
            assert not (out/'probe.log').read_bytes(), 'menu entered discovery BIOS'
            if args.reject_preview:
                off = vm.offset()
                vm.key('ret')
                vm.wait('VBE mode unavailable',off,10)
                vm.key('ret')
                vm.wait('[VGASETUP] MENU READY',off,10)
            off = vm.offset()
            vm.key('esc')
            vm.wait('CiukiOS SHELL C:\\APPS>',off,15)
            vm.command('comdemo','COM demo via INT21h')
            result['checks'] = ['menu visible without discovery','Escape returns to DOS','COM executes']
            if args.reject_preview:
                result['checks'].append('rejected VBE preview returns to menu')
        Image.open(vm.shot('final')).save(out/'final.png')
        (out/'registers.log').write_bytes(vm.hmp('info registers'))
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        print(result,flush=True)
    finally:
        vm.close()


if __name__ == '__main__':
    main()
