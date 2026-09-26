#!/usr/bin/env python3
"""Cross a real virtual-RTC minute with Run open; detect writes into kernel code."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time

import numpy as np
from PIL import Image

from qemu_test_full_display_profile import VM
from qemu_test_native_windows import WindowVM
from qemu_test_installed_hdd import FAT16, listing_address


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--image', type=Path, default=Path(
        'build/full/t23-runtime-repair-2026-09-06/final-install/target.img'))
    ap.add_argument('--kernel', required=True, type=Path)
    ap.add_argument('--shell', required=True, type=Path)
    ap.add_argument('--output', required=True, type=Path)
    ap.add_argument('--profile', default='0800', choices=('0800', 'AUTO'))
    ap.add_argument('--expect-corruption', action='store_true')
    args = ap.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    disk = out/'installed.img'; shutil.copyfile(args.image, disk)
    profile = out/'DISPLAY.CFG'; profile.write_bytes(args.profile.encode())
    for source, target in ((args.kernel, 'SYSTEM/CIUKIDOS.SYS'),
                           (args.shell, 'SYSTEM/SHELL.COM'),
                           (profile, 'SYSTEM/VIDEO/DISPLAY.CFG')):
        subprocess.run(['mcopy', '-o', '-i', f'{disk}@@32256', str(source), '::'+target], check=True)
    listing = args.kernel.with_suffix('.lst').read_text().splitlines()
    end = listing_address(listing, 'boot_drive'); xms = listing_address(listing, 'xms_entrypoint')
    reference = FAT16(disk).read('SYSTEM/CIUKIDOS.SYS')[:end]
    # Start far enough before the next minute for boot and actual key input.
    # Neither RTC state nor guest RAM is edited after startup.
    v = WindowVM.__new__(WindowVM)
    VM.__init__(v, disk, out, qemu_args=[
        '-rtc', 'base=2026-09-06T12:00:35,clock=vm',
        '-audiodev', f'wav,id=snd,path={out}/audio.wav', '-device', 'AC97,audiodev=snd'])
    report = {'shell_sha256': hashlib.sha256(args.shell.read_bytes()).hexdigest(),
              'kernel_sha256': hashlib.sha256(args.kernel.read_bytes()).hexdigest()}

    def kernel(name):
        path = out/(name+'.bin'); v.hmp(f'pmemsave 0x3000 {end} "{path}"')
        data = path.read_bytes()
        changes = [i for i, (a, b) in enumerate(zip(reference, data))
                   if a != b and not xms <= i < xms+5]
        return data, changes

    try:
        v.ready()
        v.repaint_key('f3')
        for char in 'clock minute test':
            v.key('spc' if char == ' ' else char)
        v.pointer()
        before = np.array(Image.open(v.shot('run-before-minute')))
        height, width = before.shape[:2]
        _, changes = kernel('kernel-before-minute')
        assert not changes, f'kernel already changed before minute: {changes}'
        clock = before[height-25:height-4, width-74:width-5].copy()
        started = time.monotonic(); deadline = started+70
        while time.monotonic() < deadline:
            time.sleep(.2)
            frame = np.array(Image.open(v.shot('clock-observed')))
            if not np.array_equal(frame[height-25:height-4, width-74:width-5], clock):
                time.sleep(.5)
                break
        else:
            raise AssertionError('the actual RTC minute never changed on screen')
        v.shot('run-after-minute')
        data, changes = kernel('kernel-after-minute')
        report.update({'mode': [width, height], 'seconds_until_minute_change': time.monotonic()-started,
                       'kernel_code_bytes': end, 'unexpected_code_changes': [hex(i) for i in changes]})
        if args.expect_corruption:
            assert changes and b'clock minute test' in data, 'old shell did not reproduce the misplaced Run-buffer copy'
            report['expected_corruption_reproduced'] = True
        else:
            assert not changes, report
            v.repaint_key('esc')
            off = v.offset(); v.key('f4'); v.wait('CiukiOS SHELL C:\\APPS>', off, 60)
            VM.command(v, 'echo CLOCK RETURN OK', 'CLOCK RETURN OK\r\nCiukiOS SHELL')
            off = v.offset(); v.text('exit'); v.ready(off); v.pointer(); v.shot('desktop-returned')
            _, changes = kernel('kernel-final')
            assert not changes, f'kernel changed after DOS return: {changes}'
            report['dos_command_and_desktop_return'] = True
        report['completed'] = True
    finally:
        (out/'results.json').write_text(json.dumps(report, indent=2)+'\n')
        v.close()
    print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    main()
