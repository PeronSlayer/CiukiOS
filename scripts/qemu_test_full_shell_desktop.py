#!/usr/bin/env python3
"""Exercise shell editing at real console positions, menus and file completion."""
import argparse
from pathlib import Path
import shutil
import subprocess
import time
from qemu_test_full_display_profile import VM


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--shell', type=Path)
    ap.add_argument('--graphics', action='store_true')
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    disk = out / 'test.img'
    shutil.copyfile(args.image, disk)
    if args.shell:
        subprocess.run(['mcopy', '-o', '-i', str(disk), str(args.shell),
                        '::SYSTEM/SHELL.COM'], check=True)
    fixture = out / 'ALPHA.TXT'
    fixture.write_bytes(b'completion works\r\n')
    subprocess.run(['mcopy', '-o', '-i', str(disk), str(fixture), '::APPS/ALPHA.TXT'], check=True)
    subprocess.run(['mmd', '-i', str(disk), '::APPS/EMPTY'], check=True)
    if args.graphics:
        profile = out / 'DISPLAY.CFG'
        profile.write_bytes(b'1024')
        subprocess.run(['mcopy', '-o', '-i', str(disk), str(profile),
                        '::SYSTEM/VIDEO/DISPLAY.CFG'], check=True)
    vm = VM(disk, out, ('-audiodev', f'wav,id=snd,path={out}/startup.wav',
                       '-device', 'AC97,audiodev=snd'))

    def type_line(text):
        for ch in text:
            vm.key({' ': 'spc', '.': 'dot'}.get(ch, ch))

    try:
        vm.wait('CiukiOS SHELL C:\\APPS>', timeout=45)
        vm.shot('welcome')
        vm.command('sysinfo', 'Largest free DOS block')
        vm.command('help files', 'FILES AND FOLDERS')
        offset = vm.offset()
        type_line('echo hexlo')
        vm.key('home')
        for _ in range(7): vm.key('right')
        vm.key('delete'); vm.key('l'); vm.key('end'); vm.key('ret')
        vm.wait('hello\r\nCiukiOS SHELL', offset)
        print('[shell-desktop] PASS insert/delete/Home/End', flush=True)
        offset = vm.offset()
        type_line('type alp'); vm.key('tab'); vm.key('ret')
        vm.wait('completion works', offset)
        vm.text('CD EMPTY')
        vm.wait('CiukiOS SHELL C:\\APPS\\EMPTY>')
        vm.key('tab'); vm.key('esc'); vm.text('echo empty tab returned')
        vm.wait('empty tab returned\r\nCiukiOS SHELL')
        vm.text('CD..')
        time.sleep(.5)
        print('[shell-desktop] PASS completion and empty-directory recovery', flush=True)
        vm.command('echo ' + 'a' * 110, 'a' * 110)
        vm.command('ver', 'CiukiOS pre-Alpha')
        offset = vm.offset()
        vm.key('up'); vm.key('up'); vm.key('down'); vm.key('ret')
        vm.wait('CiukiOS pre-Alpha', offset)
        vm.command('echo wrap returned', 'wrap returned\r\nCiukiOS SHELL')
        vm.shot('wrapped-editor')
        vm.command('history', 'HISTORY')
        offset = vm.offset()
        vm.key('f1'); vm.wait('QUICK HELP', offset)
        vm.key('f2'); time.sleep(1); vm.shot('apps')
        vm.key('esc'); vm.wait('A modern Retro OS', offset)
        vm.command('echo menu returned', 'menu returned\r\nCiukiOS SHELL')
        vm.command('sound off', 'preference saved')
        setting = subprocess.check_output(['mtype', '-i', str(disk), '::SYSTEM/BOOT.SND'])
        assert setting == b'0', setting
        vm.command('sound on', 'preference saved')
        print('[shell-desktop] PASS wrapped history, F1/F2, menus and sound preferences', flush=True)
    finally:
        vm.shot('final')
        vm.close()


if __name__ == '__main__':
    main()
