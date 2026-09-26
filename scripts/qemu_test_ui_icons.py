#!/usr/bin/env python3
"""Check actual official-icon pixels, window restore and DOS return in QEMU.

Operates on an isolated copy; never edits the source disk or burns media.
This is emulator integration evidence, not a physical GPU performance test.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time

import numpy as np
from PIL import Image
from qemu_test_native_windows import WindowVM
from qemu_test_installed_hdd import FAT16


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path('build/full/ui-redesign-2026-09-25/development.img'))
    parser.add_argument('--build', type=Path, default=Path('build/full/retro-icons-2026-09-25/obj'))
    parser.add_argument('--output', type=Path, default=Path('build/full/retro-icons-2026-09-25'))
    parser.add_argument('--mode', choices=('800', '2560'), default='800')
    parser.add_argument('--packaged', action='store_true',
                        help='Test binaries/resources already in the freshly built disk; do not inject replacements')
    args = parser.parse_args()
    size = (800, 600) if args.mode == '800' else (2560, 1440)
    out = (args.output / ('ui-' + args.mode)).resolve()
    out.mkdir(parents=True, exist_ok=True)
    disk = out / 'target.img'
    source_hash = hashlib.sha256(args.source.read_bytes()).hexdigest()
    shutil.copyfile(args.source, disk)
    volume = f'{disk}@@{FAT16(disk).start}'
    payloads = [(args.build/'shell.com', '::SYSTEM/SHELL.COM'),
                     (args.build/('setup.com' if args.packaged else 'setup-preview.com'), '::APPS/SETUP.COM'),
                     (Path('assets/icons/native/ICONS.DAT'), '::SYSTEM/UI/ICONS.DAT'),
                     (Path('assets/icons/CREDITS.TXT'), '::SYSTEM/UI/CREDITS.TXT'),
                     (Path('assets/icons/upstream/COPYING'), '::SYSTEM/UI/TANGO.TXT'),
                     (Path('assets/icons/upstream/AUTHORS'), '::SYSTEM/UI/AUTHORS.TXT')]
    if args.packaged:
        filesystem = FAT16(disk)
        for src, dst in payloads:
            assert filesystem.read(dst[2:]) == src.read_bytes(), f'Packaged payload mismatch: {dst}'
    else:
        subprocess.run(['mmd', '-i', volume, '::SYSTEM/UI'], check=True)
        for src, dst in payloads:
            subprocess.run(['mcopy', '-o', '-i', volume, str(src), dst], check=True)
    profile = out/'profile.cfg'
    profile.write_bytes(b'0800' if args.mode == '800' else b'2560')
    subprocess.run(['mcopy', '-o', '-i', volume, str(profile), '::SYSTEM/VIDEO/DISPLAY.CFG'], check=True)
    vm = WindowVM(disk, out, 'std', memory=128, palette='platinum',
                  video=None if args.mode == '800' else '2560x1440')
    vm.auto_enter_dos = False
    report = {'status': 'running', 'memory_mib': 128, 'mode': size,
              'packaged_payloads_unmodified': args.packaged,
              'source_sha256': source_hash,
              'shell_sha256': hashlib.sha256((args.build/'shell.com').read_bytes()).hexdigest(),
              'resource_sha256': hashlib.sha256(Path('assets/icons/native/ICONS.DAT').read_bytes()).hexdigest(),
              'scope': 'Real QEMU screenshots, controls, DOS child and desktop return; no physical GPU claim',
              'checks': []}

    def compare(frame, source, x, y, name, background):
        approved = np.asarray(Image.open(source).convert('RGBA'))
        h, w = approved.shape[:2]
        actual = np.asarray(frame)[y:y+h, x:x+w, :3].astype(np.int16)
        expected = approved[:, :, :3].astype(np.int16).copy()
        expected[approved[:, :, 3] == 0] = background
        delta = np.abs(actual - expected)
        bad = (delta > 3).any(axis=2)
        record = {'name': name, 'origin': [x, y], 'compared_pixels': h*w,
                  'bad_pixels': int(bad.sum()), 'max_channel_difference': int(delta.max())}
        report['checks'].append(record)
        assert not bad.any(), record

    def desktop_icons(frame, name):
        background = frame.getpixel((4, 40))
        for icon, y in ((0, 56), (1, 126), (4, 196), (8, 266), (10, 336), (15, 406), (16, 476)):
            compare(frame, f'assets/icons/native/{icon:02d}.png', 37, y,
                    f'{name}-icon-{icon}', background)
        compare(frame, 'assets/brand/native/ciuki-24.png', 5, 2,
                f'{name}-ciuki-header', frame.getpixel((1, 1)))

    try:
        vm.wait('[DESKTOP] READY', timeout=90)
        time.sleep(2)
        frame = Image.open(vm.shot('desktop', size)).convert('RGB')
        vm.pointer()
        desktop_icons(frame, 'initial')
        if args.mode == '800':
            # A real maximize/restore cycle must recover every stable screen
            # pixel, including the icons that were covered by the large window.
            wx, wy, ww = vm.active_rect()
            vm.position(size[0]-35, size[1]-55)
            time.sleep(.4)
            before_pointer = vm.pointer()
            before = np.asarray(Image.open(vm.shot('before-maximize', size)))
            vm.completed_control_click(wx+ww-40, wy+15)
            assert vm.active_rect() == (8, 34, size[0]-16)
            vm.completed_control_click(size[0]-48, 49)
            assert vm.active_rect() == (wx, wy, ww)
            vm.position(size[0]-35, size[1]-55)
            time.sleep(.4)
            after_pointer = vm.pointer()
            after = np.asarray(Image.open(vm.shot('after-restore', size)))
            stable = np.ones((size[1], size[0]), dtype=bool)
            stable[-32:, -78:] = False
            for px, py in (before_pointer, after_pointer):
                stable[py:py+16, px:px+24] = False
            count = int(np.any(before != after, axis=2)[stable].sum())
            report['restore_changed_stable_pixels'] = count
            assert count == 0, f'window restore left {count} changed pixels'
        off = vm.offset(); vm.key('f1'); vm.wait('[DESKTOP] PAINT', off, 30)
        time.sleep(1)
        ax, ay, _ = vm.active_rect()
        frame = Image.open(vm.shot('about', size)).convert('RGB')
        compare(frame, 'assets/brand/native/ciuki-64.png', ax+22, ay+40,
                'about-approved-portrait', frame.getpixel((1, 1)))
        off = vm.offset(); vm.key('esc'); vm.wait('[DESKTOP] PAINT', off, 30)
        time.sleep(.4)
        off = vm.offset(); vm.key('f4'); vm.wait('CiukiOS SHELL C:\\APPS>', off, 30)
        if args.mode == '800':
            off = vm.offset(); vm.text('setup'); vm.wait('[SETUP-GUI] PAGE 00', off, 30)
            time.sleep(1)
            frame = Image.open(vm.shot('setup', (800, 600))).convert('RGB')
            compare(frame, 'assets/brand/native/ciuki-24.png', 5, 2,
                    'setup-ciuki-header', frame.getpixel((1, 1)))
            compare(frame, 'assets/brand/native/ciuki-24.png', 189, 175,
                    'setup-ciuki-monitor', frame.getpixel((167, 170)))
            off = vm.offset(); vm.key('esc'); vm.wait('CiukiOS SHELL C:\\APPS>', off, 30)
        off = vm.offset(); vm.text('comdemo'); vm.wait('COM demo via INT21h', off, 30)
        vm.wait('CiukiOS SHELL C:\\APPS>', off, 30)
        off = vm.offset(); vm.text('exit'); vm.wait('[DESKTOP] READY', off, 40)
        time.sleep(2)
        frame = Image.open(vm.shot('desktop-return', size)).convert('RGB')
        vm.pointer(); desktop_icons(frame, 'return')
        assert hashlib.sha256(args.source.read_bytes()).hexdigest() == source_hash
        report.update(status='passed', comdemo_and_desktop_return=True, source_unchanged=True)
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        vm.close()
        (out/'result.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report), flush=True)


if __name__ == '__main__':
    main()
