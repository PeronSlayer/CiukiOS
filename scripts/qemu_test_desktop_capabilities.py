#!/usr/bin/env python3
"""Exercise actual VBE depth fallbacks, EDID bounds, cursor edges and boot audio."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import time

from PIL import Image
from qemu_test_full_display_profile import VM
from qemu_test_native_desktop import DesktopVM
from qemu_test_setup_graphical import SetupVM


class HardwareVM(VM):
    auto_enter_dos = False
    pointer = DesktopVM.pointer
    _pointer = DesktopVM._pointer
    edges = DesktopVM.edges
    move = SetupVM.move

    def shot(self, name, size=None):
        p = super().shot(name, size)
        Image.open(p).save(p.with_suffix('.png'))
        return p


def disk_copy(out):
    out.mkdir(parents=True, exist_ok=True)
    disk = out / 'test.img'
    shutil.copyfile('build/full/ciukios-full.img', disk)
    for filename, target in (
        ('shell.com', 'SYSTEM/SHELL.COM'),
        ('vgasetup.com', 'SYSTEM/DRIVERS/VGASETUP.COM'),
        ('bootsnd.com', 'SYSTEM/BOOTSND.COM'),
        ('bootsnd.com', 'SYSTEM/DRIVERS/SOUND.COM'),
        ('BOOT.PCM', 'SYSTEM/BOOT.PCM'),
    ):
        subprocess.run(['mcopy', '-o', '-i', str(disk),
                        f'build/full/obj/{filename}', '::' + target], check=True)
    return disk


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--case', choices=('depths', 'audio', 'no-edid'), required=True)
    args = ap.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    if args.case == 'depths':
        for depth in (15, 16, 24, 8, 0):
            out = root / str(depth)
            disk = disk_copy(out)
            fixture = out / 'VBEFILT.COM'
            subprocess.run(['nasm', '-f', 'bin', 'scripts/fixtures/vbe_filter.asm',
                            '-D', f'TEST_DEPTH={depth}', '-o', str(fixture)], check=True)
            subprocess.run(['mcopy', '-o', '-i', str(disk), str(fixture), '::APPS/VBEFILT.COM'], check=True)
            vm = HardwareVM(disk, out, ('-vga', 'none', '-device', 'VGA,xres=1024,yres=768'))
            try:
                vm.wait('[DESKTOP] READY', timeout=90)
                vm.shot('edid-native', (1024, 768))
                vm.key('f4'); vm.wait('[DESKTOP] DOS')
                vm.command('vbefilt', '[VBE-FIXTURE] installed')
                offset = vm.offset()
                vm.text('c:\\system\\shell.com')
                vm.wait('[DESKTOP] READY', offset, 90)
                # BIOS fixture hides depths; physical mode set and bank calls
                # still run in SeaBIOS and real emulated VGA memory.
                size = (640, 480) if depth == 0 else (1024, 768)
                vm.wait(f'{4 if depth == 0 else depth}-bit color', offset)
                vm.shot('desktop', size)
                vm.edges(size)
                offset = vm.offset(); vm.key('f4'); vm.wait('[DESKTOP] DOS', offset)
                vm.text('echo COLOR READY'); vm.wait('COLOR READY\r\nCiukiOS SHELL', offset)
                vm.shot('dos')
                offset = vm.offset(); vm.text('exit'); vm.wait('[DESKTOP] READY', offset, 90)
                vm.pointer()
                print(f'[capabilities] PASS depth={depth}: BIOS selection, EDID, DOS and cursor', flush=True)
            finally:
                vm.close()
        return
    if args.case == 'no-edid':
        disk = disk_copy(root)
        vm = HardwareVM(disk, root, ('-vga', 'none', '-device', 'VGA,edid=off'))
        try:
            vm.wait('[DESKTOP] READY', timeout=90)
            size = Image.open(vm.shot('desktop')).size
            assert size == (640,480), f'unidentified monitor must use VGA: {size}'
            vm.edges(size)
            vm.key('f4'); vm.wait('[DESKTOP] DOS')
            vm.command('echo INPUT READY','INPUT READY\r\nCiukiOS SHELL')
            print(f'[capabilities] PASS missing EDID: safe VGA, cursor bounds and keyboard', flush=True)
        finally:
            vm.close()
        return
    for device in ('ac97', 'speaker', 'muted'):
        out = root / device
        disk = disk_copy(out)
        if device == 'muted':
            muted = out / 'BOOT.SND'; muted.write_bytes(b'0')
            subprocess.run(['mcopy', '-o', '-i', str(disk), str(muted), '::SYSTEM/BOOT.SND'], check=True)
        wav = out / 'audio.wav'
        extra = ['-audiodev', f'wav,id=snd,path={wav}']
        if device == 'speaker':
            extra += ['-machine', 'pcspk-audiodev=snd']
        else:
            extra += ['-device', 'AC97,audiodev=snd']
        vm = HardwareVM(disk, out, extra)
        try:
            vm.wait('[DESKTOP] READY', timeout=90)
            vm.shot('desktop', (1280, 800))
            vm.key('f4'); vm.wait('[DESKTOP] DOS')
            vm.command('echo AUDIO RETURNED','AUDIO RETURNED\r\nCiukiOS SHELL')
        finally:
            vm.close()
        if device in ('muted','speaker'):
            from analyze_audio_wav import pcm_payload
            pcm, _ = pcm_payload(wav.read_bytes())
            assert not any(pcm), 'muted/unsupported startup backend emitted audio'
        else:
            subprocess.run(['python3', 'scripts/analyze_audio_wav.py', str(wav),
                            '--label', device, '--min-bytes', '50000'], check=True)
        print(f'[capabilities] PASS startup sound: {device}', flush=True)


if __name__ == '__main__':
    main()
