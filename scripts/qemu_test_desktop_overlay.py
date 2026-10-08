#!/usr/bin/env python3
"""Retain an idle native DOSVM image under desktop popups and other windows.

Run alone in a capped systemd user scope. Only a private disk copy receives
the unchanged VGA fixture; guest RAM is read, never patched. This gate
qualifies emulator composition behavior, not physical T23 acceleration.
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

from qemu_test_installed_hdd import FAT16
from qemu_test_native_utilities import Utilities
from qemu_test_native_windows import WindowVM

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def geometry(ui, window):
    ui.refresh()
    return tuple(ui.w(field, window) for field in
                 ('ui_window_x', 'ui_window_y', 'ui_window_width', 'ui_window_height'))


def rectangle_mask(shape, rectangle):
    mask = np.zeros(shape[:2], dtype=bool)
    x, y, w, h = rectangle
    mask[max(0, y):max(0, y + h), max(0, x):max(0, x + w)] = True
    return mask


def popup_rectangle(frame, x, y):
    """Measure the popup's continuous bevel, independently of damaged pixels."""
    colour = frame[y, x]
    assert np.max(np.abs(colour.astype(int) - (246, 246, 242))) <= 4, 'menu bevel colour missing'
    paper = np.all(frame == colour, axis=2)
    assert paper[y, x:x+80].all(), 'menu has no expected continuous top bevel'
    right, bottom = x, y
    while right < frame.shape[1] and paper[y, right]:
        right += 1
    while bottom < frame.shape[0] and paper[bottom, x]:
        bottom += 1
    # The final edge is the dark bevel; include its pixel and 3-pixel shadow.
    width, height = right - x + 1, bottom - y + 1
    assert 120 <= width <= 400 and 200 <= height <= 400, (width, height)
    return x, y, width + 3, height + 3


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--vga', choices=('std', 'cirrus'), default='std')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    image = args.image.resolve()
    initial_hash = sha(image)
    disk = output / 'disk.img'
    shutil.copyfile(image, disk)
    fat = FAT16(disk)
    volume = f'{disk}@@{fat.start}'
    fixture = output / 'OVRSTATIC.COM'
    listing = output / 'SHELL.lst'
    shell = output / 'fresh-SHELL.COM'
    subprocess.run(['nasm', '-f', 'bin', '-o', str(fixture),
                    'scripts/fixtures/vga_overlay_guest.asm'], cwd=ROOT, check=True)
    subprocess.run(['nasm', '-f', 'bin', '-l', str(listing), '-o', str(shell),
                    'src/com/shell.asm'], cwd=ROOT, check=True)
    shipped_shell = fat.read('SYSTEM/SHELL.COM')
    assert shell.read_bytes() == shipped_shell, 'source image contains a stale SHELL.COM'
    subprocess.run(['mcopy', '-o', '-i', volume, str(fixture), '::APPS/OVRSTATIC.COM'], check=True)
    report = dict(passed=False, physical_hardware_qualified=False,
                  scope='Current native DOSVM, static VGA client, real input and read-only RAM',
                  source_image_sha256=initial_hash, shell_sha256=sha(shell),
                  desktop_sha256=hashlib.sha256(fat.read('SYSTEM/APPS/DESKTOP.APP')).hexdigest(),
                  guest_memory_writes=False, checks=[])
    vm, ui = None, None

    def capture(name):
        path = vm.shot(name)
        frame = np.array(Image.open(path).convert('RGB'))
        Image.fromarray(frame).save(output / (name + '.png'))
        return frame

    def stable_capture(name):
        previous = capture(name + '-sample')
        for _ in range(15):
            time.sleep(.2)
            current = capture(name)
            if np.array_equal(previous[cy:cy+ch, cx:cx+cw],
                              current[cy:cy+ch, cx:cx+cw]):
                return current
            previous = current
        raise AssertionError('idle DOS client never settled')

    def verify(name, frame, occluder=None):
        visible = rectangle_mask(frame.shape, (cx, cy, cw, ch))
        if occluder:
            visible &= ~rectangle_mask(frame.shape, occluder)
        difference = np.any(frame != baseline, axis=2)
        changed = int(np.count_nonzero(visible & difference))
        evidence = dict(name=name, compared_pixels=int(visible.sum()),
                        mismatched_pixels=changed, screenshot=name + '.png',
                        excluded_rectangle=occluder)
        report['checks'].append(evidence)
        assert visible.sum() > 10000, 'no meaningful exposed client region'
        assert changed == 0, evidence

    def drag(window, x, y):
        old_x, old_y, _, _ = geometry(ui, window)
        vm.position(old_x + 170, old_y + 14)
        vm.hmp('mouse_button 1'); time.sleep(.15)
        vm.position(x + 170, y + 14); time.sleep(.25)
        offset = vm.offset(); vm.hmp('mouse_button 0')
        vm.wait('[DESKTOP] PAINT', offset, 15)
        # DesktopVM.position accepts a three-pixel pointer tolerance; use the
        # resulting observed geometry, rather than requiring an exact corner.
        ui.until(lambda: all(abs(observed - wanted) <= 4
                             for observed, wanted in zip(geometry(ui, window)[:2], (x, y))),
                 'window drag failed', 15)
        report.setdefault('drags', []).append(dict(window=window, requested=[x,y],
                                                  observed=list(geometry(ui, window))))

    try:
        vm = WindowVM(disk, output, args.vga, memory=128, palette='platinum')
        vm.ready()
        vm.repaint_key('esc')  # dismiss the startup About window
        ui = Utilities(vm, shipped_shell, listing)
        offset = vm.offset(); vm.key('meta_l-r')
        vm.wait('WINDOW 01 OPEN', offset, 20)
        vm.text(r'\APPS\OVRSTATIC.COM')
        vm.wait('[DOSVM] fork', offset, 30)
        vm.wait('[OVERLAY-GUEST] static VGA ready', offset, 30)
        ui.until(lambda: ui.b('ui_window_flags', 11) == 1, 'DOSVM did not open', 15)
        drag(11, 50, 45)
        x, y, w, h = geometry(ui, 11)
        cx, cy, cw, ch = x + 6, y + 33, w - 12, h - 39
        vm.position(8, ui.w('ui_height') - 20)
        baseline = stable_capture('dos-static-baseline')
        client = baseline[cy:cy+ch, cx:cx+cw]
        assert len(np.unique(client.reshape(-1, 3), axis=0)) >= 12, 'guest image is blank'
        report['client_rectangle'] = [cx, cy, cw, ch]

        # Volume is drawn above the client; the rest of the client must survive
        # even while the guest is idle and produces no new VGA writes.
        ui.click(80)
        vm.wait('[DESK] volume open', offset, 20)
        ui.until(lambda: ui.b('app_overlay') == 1, 'Volume overlay absent', 15)
        volume_x = max(2, ui.w('ui_width') - 576)
        # Volume has no shadow. Its damage padding is not an opaque surface.
        verify('volume-open', stable_capture('volume-open'), (volume_x, 29, 220, 60))
        offset = vm.offset(); vm.key('esc')
        vm.wait('[DESKTOP] PAINT', offset, 15)
        ui.until(lambda: ui.b('app_overlay') == 0, 'Volume did not close', 15)
        verify('volume-exposed', stable_capture('volume-exposed'))

        # A second native window must be opaque where it covers the DOS client.
        offset = vm.offset(); vm.key('meta_l-e')
        vm.wait('[FILES]', offset, 30)
        ui.until(lambda: ui.b('ui_active_window') == 8, 'Files did not become active', 20)
        drag(8, 310, 180)
        files = geometry(ui, 8)
        with_files = stable_capture('files-over-dos')
        verify('files-over-dos', with_files, (files[0], files[1], files[2]+4, files[3]+4))
        overlap = rectangle_mask(with_files.shape, files) & rectangle_mask(with_files.shape, (cx,cy,cw,ch))
        overwritten = int(np.count_nonzero(overlap & np.any(with_files != baseline, axis=2)))
        assert overwritten > 10000, 'Files did not visibly cover the DOS client'
        report['native_window_occluded_pixels'] = overwritten

        # Raising an unchanged guest must recompose it from retained content,
        # rather than waiting for the guest to submit a fresh frame.
        ui.click(211)
        ui.until(lambda: ui.b('ui_active_window') == 11, 'DOS taskbar raise failed', 20)
        assert ui.b('ui_window_flags', 8) == 1, 'raising DOS unexpectedly closed Files'
        vm.position(8, ui.w('ui_height') - 20)
        verify('dos-raised-over-files', stable_capture('dos-raised-over-files'))

        # Minimize/restore must retain geometry and the same guest session.
        before_geometry = geometry(ui, 11)
        forks = vm.serial.read_bytes().count(b'[DOSVM] fork')
        ui.click(17, 11)
        ui.until(lambda: ui.b('ui_window_flags', 11) == 2, 'DOS did not minimize', 20)
        capture('dos-minimized')
        ui.click(211)
        ui.until(lambda: ui.b('ui_window_flags', 11) == 1 and ui.b('ui_active_window') == 11,
                 'DOS taskbar restore failed', 20)
        assert geometry(ui, 11) == before_geometry, 'restore changed DOS client geometry'
        assert vm.serial.read_bytes().count(b'[DOSVM] fork') == forks, 'restore relaunched the guest'
        report['minimize_restore'] = dict(geometry=list(before_geometry), retained_guest=True)
        vm.position(8, ui.w('ui_height') - 20)
        verify('dos-restored-over-files', stable_capture('dos-restored-over-files'))

        ui.click(208)
        ui.until(lambda: ui.b('ui_active_window') == 8, 'Files taskbar raise failed', 20)
        vm.position(8, ui.w('ui_height') - 20)
        files = geometry(ui, 8)
        verify('files-raised-over-dos', stable_capture('files-raised-over-dos'),
               (files[0], files[1], files[2]+4, files[3]+4))
        ui.click(18, 8)
        ui.until(lambda: ui.b('ui_window_flags', 8) == 0, 'Files did not close', 20)
        vm.position(8, ui.w('ui_height') - 20)
        verify('files-exposed', stable_capture('files-exposed'))

        # Check the tall CiukiOS menu, including bands below the Volume popup.
        offset = vm.offset(); ui.click(13)
        vm.wait('[DESK] menu start', offset, 20)
        ui.until(lambda: ui.b('app_overlay') == 1, 'start menu absent', 15)
        vm.position(8, ui.w('ui_height') - 20)
        # Measure the menu's own bevel; do not derive its mask from changed
        # client pixels, which could accidentally hide wallpaper corruption.
        start_frame = stable_capture('start-open')
        start_bounds = popup_rectangle(start_frame, 4, 29)
        report['start_menu_rectangle'] = list(start_bounds)
        verify('start-open', start_frame, start_bounds)
        offset = vm.offset(); vm.key('esc')
        vm.wait('[DESKTOP] PAINT', offset, 15)
        ui.until(lambda: ui.b('app_overlay') == 0, 'start menu did not close', 15)
        verify('start-exposed', stable_capture('start-exposed'))
        report['passed'] = True
    except Exception as error:
        report['error'] = repr(error)
        if vm:
            try:
                capture('failure')
            except Exception:
                pass
            if vm.serial.exists():
                report['serial_tail'] = subprocess.check_output([
                    'scripts/serial_log_normalize.py', str(vm.serial)
                ]).decode('cp437', 'replace')[-5000:]
    finally:
        if vm:
            vm.close()
        report['source_image_unchanged'] = sha(image) == initial_hash
        report['passed'] &= report['source_image_unchanged']
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
