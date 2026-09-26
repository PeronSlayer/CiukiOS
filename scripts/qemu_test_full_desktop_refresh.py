#!/usr/bin/env python3
"""Exercise Doom's menu and real Win16 retained-window redraw operations."""
import argparse
from pathlib import Path
import shutil
import subprocess
import time
from qemu_test_full_display_profile import VM


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--case', choices=('doom', 'windows'), required=True)
    ap.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    ap.add_argument('--iso', type=Path, help='test the release CD (Doom, no payload overrides)')
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--draw-test', type=Path)
    ap.add_argument('--doom-com', type=Path)
    ap.add_argument('--dos32a', type=Path, default=Path('/opt/watcom/binw/dos32a.exe'))
    ap.add_argument('--shell', type=Path)
    ap.add_argument('--resolution', choices=('safe', '800', '1024'), default='1024')
    ap.add_argument('--swap-interval', type=int)
    ap.add_argument('--linear', action='store_true')
    ap.add_argument('--dos-prompt', action='store_true')
    ap.add_argument('--safe', action='store_true', help='exercise DOOMSAFE without audio')
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    disk = out / 'test.img'
    drive = "D" if args.iso else "C"
    if args.iso:
        assert args.case == "doom" and not args.shell and not args.doom_com
        disk.write_bytes(bytes(512))
    else:
        shutil.copyfile(args.image, disk)
    if args.shell:
        subprocess.run(['mcopy', '-o', '-i', str(disk), str(args.shell),
                        '::SYSTEM/SHELL.COM'], check=True)
    if args.doom_com:
        subprocess.run(['mcopy', '-o', '-i', str(disk), str(args.doom_com),
                        '::APPS/DOOM/DOOM.COM'], check=True)
        subprocess.run(['mcopy', '-o', '-i', str(disk), str(args.dos32a),
                        '::SYSTEM/DRIVERS/DOS32A.EXE'], check=True)
    if args.case == 'windows':
        assert args.draw_test, '--draw-test is required for Windows'
        subprocess.run(['mcopy', '-o', '-i', str(disk), str(args.draw_test),
                        '::WINDOWS/DRAWTEST.EXE'], check=True)
        if args.swap_interval is not None:
            preset = {'safe': 'VGA', '800': '800', '1024': '102'}[args.resolution]
            data = subprocess.check_output(['mtype', '-i', str(disk),
                                            f'::WINDOWS/SYSTEM.{preset}'])
            data = data.replace(b'SwapBuffersInterval=0',
                                f'SwapBuffersInterval={args.swap_interval}'.encode())
            if args.linear:
                data = data.replace(b'PreferBankedModes=1', b'PreferBankedModes=0')
            profile = out / 'SYSTEM.INI'
            profile.write_bytes(data)
            subprocess.run(['mcopy', '-o', '-i', str(disk), str(profile),
                            f'::WINDOWS/SYSTEM.{preset}'], check=True)
    extra = ['-audiodev', f'wav,id=snd,path={out}/audio.wav',
             '-device', 'AC97,audiodev=snd']
    if args.iso:
        extra += ['-boot', 'd', '-cdrom', str(args.iso.resolve())]
    vm = VM(disk, out, extra)
    try:
        vm.wait(f'CiukiOS SHELL {drive}:\\APPS>')
        if args.case == 'doom':
            vm.text('CD DOOM')
            vm.wait(f'CiukiOS SHELL {drive}:\\APPS\\DOOM>')
            offset = vm.offset()
            vm.text('doomsafe.com' if args.safe else ('doom.com' if args.doom_com else 'doom.exe'))
            vm.wait('[DOOM] SAFE' if args.safe else '[DOOM] LAUNCH', offset)
            vm.wait('ST_Init: Init status bar.', offset, 60)
            deadline = time.monotonic() + 60
            while True:
                title = vm.shot('title')
                pixels = title.read_bytes().split(maxsplit=4)[4]
                sample = [pixels[i:i+3] for i in range(0, len(pixels), 48)]
                if (len(set(sample)) > 32 and
                        sum(p != b'\0\0\0' for p in sample) > len(sample) // 2):
                    break
                assert time.monotonic() < deadline, 'Doom title never finished loading'
                time.sleep(1)
            time.sleep(2)
            header = title.read_bytes().split(maxsplit=4)
            size = int(header[1]), int(header[2])
            assert size in ((320, 200), (640, 400)), f'Doom surface: {size}'
            vm.key('esc')
            time.sleep(2)
            before = vm.shot('menu', size).read_bytes()
            time.sleep(15)
            vm.key('down')
            time.sleep(.6)
            after = vm.shot('menu-moved', size).read_bytes()
            assert before != after, 'menu did not respond to keyboard'
            vm.key('up')
            for _ in range(3):
                vm.key('ret')
                time.sleep(.5)
            time.sleep(10)
            vm.shot('gameplay', size)
            vm.key('f10')
            time.sleep(1)
            vm.key('y')
            if not args.safe:
                vm.wait('[DOOM] AUDIO CLEANUP COMPLETE', offset, 30)
            vm.wait(f'CiukiOS SHELL {drive}:\\APPS\\DOOM>', offset)
            print('[desktop] PASS Doom title, idle menu, keyboard, new game and exit', flush=True)
        else:
            vm.command(f'vgasetup set {args.resolution}', 'Shared resolution saved')
            vm.text('cd \\windows')
            time.sleep(.5)
            vm.text('win')
            time.sleep(24)
            vm.shot('windows-start')
            vm.key('alt-f'); time.sleep(.4); vm.key('e'); time.sleep(.4)
            vm.text('drawtest.exe')
            time.sleep(5)
            vm.shot('resize')
            deadline = time.monotonic() + 90
            while True:
                data = subprocess.check_output(['mtype', '-i', str(disk), '::DRAWTEST.LOG'])
                if b'COMPLETE' in data:
                    break
                assert time.monotonic() < deadline, 'Win16 redraw test did not finish'
                time.sleep(1)
            (out / 'DRAWTEST.LOG').write_bytes(data)
            assert b'RESULT PASS' in data and b'FAIL' not in data, data.decode(errors='replace')
            vm.shot('windows-after-redraw')
            print('[desktop] PASS Win16 retained pixels after resize, move, minimize/restore', flush=True)
            if args.dos_prompt:
                offset = vm.offset()
                vm.key('alt-f'); time.sleep(.4); vm.key('e'); time.sleep(.4)
                vm.text('command.com')
                vm.wait('CiukiOS SHELL C:\\windows>', offset, 30)
                vm.text('echo WINDOWS DOS READY')
                vm.wait('WINDOWS DOS READY\r\nCiukiOS SHELL', offset)
                vm.shot('windows-dos-prompt')
                vm.text('exit')
                time.sleep(5)
                vm.shot('windows-dos-return')
                print('[desktop] PASS interactive DOS child inside Windows and EXIT return', flush=True)
            vm.key('alt-f4'); time.sleep(.5); vm.key('ret')
            vm.wait('[WIN31] AUDIO CLEANUP COMPLETE', timeout=30)
    finally:
        vm.shot('last-screen')
        (out / 'registers.log').write_bytes(vm.hmp('info registers'))
        vm.close()


if __name__ == '__main__':
    main()
