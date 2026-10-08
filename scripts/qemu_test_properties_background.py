#!/usr/bin/env python3
"""Exercise Display Properties from Desktop and verify real wallpaper pixels.

The gate uses a copied full HDD image, fresh matching SHELL.COM/listing,
actual QEMU pointer/key input, read-only shell RAM observation and screenshots.
It does not patch guest RAM or claim physical-hardware qualification.
"""
import argparse
import hashlib
import json
import re
import shutil
import struct
import subprocess
import time
from pathlib import Path

import numpy as np
from PIL import Image

from qemu_test_installed_hdd import FAT16
from qemu_test_native_utilities import Utilities
from qemu_test_native_windows import WindowVM, burst, check_damage
from qemu_test_photo_wallpaper import (PHOTO_NAMES, check_assets,
                                       install_bad_fixtures, parse_catalog)


ROOT = Path(__file__).resolve().parents[1]
DISPLAY_WINDOW = 18
PROFILES = {
    '640-std': {'mode': (640, 480), 'cfg': b'M110', 'vga': 'std',
                'video': '640x480', 'memory': 256},
    '800-std': {'mode': (800, 600), 'cfg': b'0800', 'vga': 'std',
                'video': None, 'memory': 256},
    '1024-std': {'mode': (1024, 768), 'cfg': b'1024', 'vga': 'std',
                 'video': '1024x768', 'memory': 256},
    '1280-std': {'mode': (1280, 800), 'cfg': b'M17A', 'vga': 'std',
                 'video': '1280x800', 'memory': 256},
    '1280-virtio': {'mode': (1280, 800), 'cfg': b'M17A', 'vga': 'none',
                    'video': None, 'memory': 256, 'extra': [
                        '-device', 'virtio-vga,xres=1280,yres=800']},
}
STYLES = ('Fill', 'Fit', 'Stretch', 'Center', 'Tile')
WP_FIT = 1


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def assemble_shell(output):
    shell = output / 'assembled-SHELL.COM'
    listing = output / 'SHELL.lst'
    subprocess.run(['nasm', '-f', 'bin', '-l', str(listing), '-o', str(shell),
                    'src/com/shell.asm'], cwd=ROOT, check=True)
    return shell, listing


def configure_private_disk(source, disk, cfg):
    shutil.copyfile(source, disk)
    volume = f'{disk}@@{FAT16(disk).start}'
    profile = disk.parent / 'DISPLAY.CFG'
    profile.write_bytes(cfg)
    subprocess.run(['mcopy', '-o', '-i', volume, str(profile),
                    '::SYSTEM/VIDEO/DISPLAY.CFG'], check=True)


def c_div2(value):
    """Match C signed integer division, which truncates toward zero."""
    return value // 2 if value >= 0 else -((-value) // 2)


def wallpaper_shape(style, screen_w, screen_h, image_w, image_h):
    width, height = screen_w, screen_h - 61
    dw, dh = image_w, image_h
    if style in (0, 1):
        by_width = width * image_h >= height * image_w
        if style == 1:
            by_width = not by_width
        if by_width:
            dw = width
            dh = (image_h * width + (image_w - 1 if style == 0 else 0)) // image_w
        else:
            dh = height
            dw = (image_w * height + (image_h - 1 if style == 0 else 0)) // image_h
    elif style in (2, 4):
        dw, dh = width, height
    x0 = c_div2(screen_w - dw)
    y0 = 29 + c_div2(height - dh)
    left, top = max(0, x0), max(29, y0)
    right, bottom = min(screen_w, x0 + dw), min(29 + height, y0 + dh)
    return x0, y0, dw, dh, left, top, right, bottom


def desktop_icon_boxes(vm):
    serial = subprocess.check_output(['scripts/serial_log_normalize.py',
                                      str(vm.serial)]).decode('cp437', 'replace')
    with Image.open(vm.shot('icon-box-screen')) as screen:
        width, height = screen.size
    boxes = []
    for _, cx, cy in re.findall(r'\[DESK\] icon (\S+) (\d+) (\d+)', serial):
        x, y = int(cx) - 42, int(cy) - 20
        boxes.append((max(0, x - 4), max(0, y - 4), 92, min(270, height - y + 8)))
    boxes.extend(((0, 0, width, 29), (0, height - 32, width, 32)))
    return boxes


def compare_pixels(frame, source_rgb, style, icons, label, report,
                   solid_frame=None, tolerance=48):
    height, width = frame.shape[:2]
    src_h, src_w = source_rgb.shape[:2]
    x0, y0, dw, dh, left, top, right, bottom = wallpaper_shape(
        style, width, height, src_w, src_h)
    deltas, blank_deltas, fit_bars = [], [], []
    sx_step = max(19, width // 32)
    sy_step = max(17, height // 24)
    for y in range(max(31, top), min(height - 34, bottom), sy_step):
        for x in range(max(5, left), min(width - 5, right), sx_step):
            if any(bx <= x < bx + bw and by <= y < by + bh
                   for bx, by, bw, bh in icons):
                continue
            if style == 4:
                sx, sy = (x - x0) % src_w, (y - y0) % src_h
            else:
                sx = min(src_w - 1, max(0, (x - x0) * src_w // dw))
                sy = min(src_h - 1, max(0, (y - y0) * src_h // dh))
            nearby = source_rgb[max(0, sy - 1):min(src_h, sy + 2),
                                max(0, sx - 1):min(src_w, sx + 2)].astype(np.int16)
            observed = frame[y, x].astype(np.int16)
            delta = np.max(np.abs(nearby - observed), axis=2)
            deltas.append(int(delta.min()))
    if style == WP_FIT:
        for y in range(31, height - 34, sy_step):
            for x in range(5, width - 5, sx_step):
                if left <= x < right and top <= y < bottom:
                    continue
                if any(bx <= x < bx + bw and by <= y < by + bh
                       for bx, by, bw, bh in icons):
                    continue
                if solid_frame is not None:
                    blank_deltas.append(int(np.max(np.abs(
                        frame[y, x].astype(np.int16) -
                        solid_frame[y, x].astype(np.int16)))))
                fit_bars.append(frame[y, x].astype(np.int16))
    result = {'name': label, 'style': STYLES[style], 'screen': [width, height],
              'source': [src_w, src_h], 'shape': [x0, y0, dw, dh],
              'sample_count': len(deltas), 'max_source_delta': max(deltas) if deltas else None,
              'median_source_delta': float(np.median(deltas)) if deltas else None,
              'fit_background_samples': len(blank_deltas),
              'fit_background_max_delta': max(blank_deltas) if blank_deltas else None,
              'fit_bar_samples': len(fit_bars), 'fit_shape_contained':
                  style != WP_FIT or (dw <= width and dh <= height - 61)}
    report['checks'].append(result)
    assert len(deltas) >= 40, f'{label}: insufficient wallpaper samples: {result}'
    assert max(deltas) <= tolerance and np.median(deltas) <= 24, result
    if style == WP_FIT:
        assert dw <= width and dh <= height - 61 and (dw < width or dh < height - 61), result
        assert len(fit_bars) >= 10, result
        bar_deltas = np.max(np.abs(np.asarray(fit_bars) - fit_bars[0]), axis=1)
        assert int(bar_deltas.max()) <= 4, result
        if solid_frame is not None:
            assert len(blank_deltas) >= 10 and max(blank_deltas) <= 4, result


def read_image(disk, index):
    fs = FAT16(disk)
    records = parse_catalog(fs.read('SYSTEM/UI/WALLS.DAT'))
    filename = records[index - 1][0]
    raw = fs.read('SYSTEM/UI/' + filename)
    if raw[:4] == b'CWP2':
        _, w, h, stride, encoding, flags, length = struct.unpack_from('<4sHHHBBI', raw)
        assert (stride, encoding, flags, length, len(raw)) == (w * 3, 1, 0, w * h * 3, 16 + w * h * 3)
        pixels = np.frombuffer(raw, np.uint8, offset=16).reshape(h, w, 3).copy()
        return filename, pixels, 2
    magic, w, h, colors, flags, length = struct.unpack_from('<4sHHHHI', raw)
    assert magic == b'CWP1' and colors == 256 and flags == 0 and length == w * h
    palette = np.frombuffer(raw, np.uint8, offset=16, count=768).reshape(256, 3)
    indices = np.frombuffer(raw, np.uint8, offset=784).reshape(h, w)
    return filename, palette[indices].copy(), 1


def image_frame(vm, name):
    with Image.open(vm.shot(name)) as image:
        return np.asarray(image.convert('RGB'), dtype=np.uint8)


def optional_file(disk, path):
    try:
        return FAT16(disk).read(path)
    except (FileNotFoundError, KeyError):
        return None


def wait_wallpaper(vm, filename, offset=0):
    ready = f'[WALLP] ready {filename}'
    deadline = time.monotonic() + 35
    latest = ''
    while time.monotonic() < deadline:
        latest = subprocess.check_output([
            'scripts/serial_log_normalize.py', '--offset', str(offset),
            str(vm.serial)]).decode('cp437', 'replace')
        ready_at = latest.rfind(ready)
        if ready_at >= 0 and latest.find('[DESKTOP] PAINT', ready_at + len(ready)) >= 0:
            return
        time.sleep(.08)
    raise AssertionError(
        f'Wallpaper did not complete a desktop paint after {ready!r}; '
        f'serial tail={latest[-1200:]!r}')


def display_log_snapshot(disk):
    try:
        return FAT16(disk).read('SYSTEM/DISPLAY.LOG')
    except (FileNotFoundError, KeyError):
        return b''


def wait_display_log(disk, marker, baseline=b'', timeout=30):
    deadline = time.monotonic() + timeout
    latest = b''
    while time.monotonic() < deadline:
        try:
            latest = FAT16(disk).read('SYSTEM/DISPLAY.LOG')
        except (FileNotFoundError, KeyError):
            latest = b''
        # Ignore any marker that was already in the pre-action log. If the
        # file was truncated/replaced during the action, search the new log
        # from its beginning instead of applying an obsolete byte offset.
        if len(latest) >= len(baseline) and latest[:len(baseline)] == baseline:
            new_bytes = latest[len(baseline):]
        else:
            new_bytes = latest
        if marker.encode() in new_bytes:
            return latest.decode('ascii', 'replace')
        time.sleep(.1)
    raise AssertionError(f'Display diagnostic marker missing: {marker!r}; tail={latest[-1000:]!r}')


def click_action(vm, ui, report, label, action, owner=DISPLAY_WINDOW):
    before = len(vm.control_latencies)
    # app_s_hit encodes native CApp controls as 100 + their local action ID.
    # The shell and external hit table are observed in separate HMP reads.
    # A repaint can rebuild the table between them. Retry only failures raised
    # before mouse input, never an action or pointer failure after a click.
    deadline = time.monotonic() + 20
    while True:
        try:
            ui.click(action + 100, owner=owner)
            break
        except AssertionError as exc:
            message = str(exc)
            pre_input = (message.startswith('no exposed control') or
                         message.startswith('invalid hit count'))
            if not pre_input or time.monotonic() >= deadline:
                raise
            time.sleep(.08)
    if len(vm.control_latencies) > before:
        report.setdefault('control_click_latency_seconds', []).append({
            'action': label, 'seconds': vm.control_latencies[-1]})


def open_properties(vm, ui):
    width, height = ui.w('ui_width'), ui.w('ui_height')
    offset = vm.offset()
    vm.position(min(22, width - 1), max(80, min(height - 100, height // 2)))
    vm.hmp('mouse_button 2')
    vm.wait('[DESKTOP] PAINT', offset, 15)
    vm.hmp('mouse_button 0')
    vm.wait('[DESK] menu background', offset, 10)
    # The background menu spells this mnemonic P&roperties.
    open_offset = vm.offset()
    vm.key('r')
    vm.wait('[DISPLAY] open', open_offset, 20)
    vm.wait('[DESKTOP] PAINT', open_offset, 20)
    ui.until(lambda: ui.b('ui_active_window') == DISPLAY_WINDOW,
             'Desktop Properties opens Display Properties on Background')


def dismiss_about(vm, ui):
    if not (ui.b('ui_window_flags', 2) & 1):
        return
    x, y, width = ui.w('ui_window_x', 2), ui.w('ui_window_y', 2), ui.w('ui_window_width', 2)
    vm.position(x + width // 2, y + 12)
    vm.hmp('mouse_button 1'); time.sleep(.15); vm.hmp('mouse_button 0')
    ui.until(lambda: ui.b('ui_active_window') == 2, 'Raise first-run About')
    vm.key('esc')
    ui.until(lambda: not (ui.b('ui_window_flags', 2) & 1), 'Dismiss About')


def select_wallpaper(vm, ui, report, current, target):
    # Start from the first entry so the selected catalog item and its visible
    # row stay synchronized even when the persisted selection is off-screen.
    vm.repaint_key('home'); current = 0
    while current < target:
        vm.repaint_key('down'); current += 1
    while current > target:
        vm.repaint_key('up'); current -= 1
    assert current == target
    # Display stores the visible row number in the byte-sized hit ID; the
    # application adds its current scroll offset to recover the catalog index.
    click_action(vm, ui, report, f'Catalog entry {target}', 100 + min(target, 8))


def apply_wallpaper(vm, ui, disk, report, target, style, current=None):
    open_properties(vm, ui)
    cfg = FAT16(disk).read('SYSTEM/UI/WALL.CFG')
    assert len(cfg) == 2, f'WALL.CFG must have index and style bytes: {cfg!r}'
    if current is None:
        current = cfg[0]
    select_wallpaper(vm, ui, report, current, target)
    click_action(vm, ui, report, f'{STYLES[style]} style', 50 + style)
    ui.until(lambda: ui.b('ui_active_window') == DISPLAY_WINDOW and
             not (ui.b('ui_window_flags', 5) & 1),
             f'{STYLES[style]} stays in Display Properties')
    report['checks'].append({'name': 'style-action-routing', 'style': STYLES[style],
                             'active_window': DISPLAY_WINDOW,
                             'legacy_display_window_05_closed': True})
    wallpaper_offset = vm.offset()
    click_action(vm, ui, report, 'Apply', 11)
    expected = bytes((target, style))
    assert FAT16(disk).read('SYSTEM/UI/WALL.CFG') == expected, \
        f'Apply did not save both WALL.CFG bytes: expected {expected!r}'
    click_action(vm, ui, report, 'OK', 15)
    ui.until(lambda: not (ui.b('ui_window_flags', DISPLAY_WINDOW) & 1),
             'Display Properties closes after OK')
    return expected, wallpaper_offset


def fresh_vm(disk, directory, profile):
    extra = profile.get('extra', [])
    vm = WindowVM(disk, directory, profile['vga'], memory=profile['memory'],
                  palette='platinum', video=profile['video'], extra_qemu_args=extra)
    vm.auto_enter_dos = False
    vm.ready()
    image = Image.open(vm.shot('observed-mode'))
    assert image.size == profile['mode'], (profile['name'], image.size)
    return vm


def compare_cwp1_tile(frame, source_rgb, icons, label, report):
    h, w = frame.shape[:2]
    src_h, src_w = source_rgb.shape[:2]
    deltas = []
    for y in range(31, h - 34, max(17, h // 24)):
        for x in range(5, w - 5, max(19, w // 32)):
            if any(bx <= x < bx + bw and by <= y < by + bh
                   for bx, by, bw, bh in icons):
                continue
            expected = source_rgb[(y - 29) % src_h, x % src_w].astype(np.int16)
            deltas.append(int(np.max(np.abs(frame[y, x].astype(np.int16) - expected))))
    result = {'name': label, 'style': 'Tile', 'source_kind': 'CWP1',
              'sample_count': len(deltas), 'max_source_delta': max(deltas),
              'median_source_delta': float(np.median(deltas))}
    report['checks'].append(result)
    assert len(deltas) >= 40 and max(deltas) <= 48 and np.median(deltas) <= 24, result


def run_profile(source_image, shell_bytes, listing, listing_hash, profile_name,
                output, photos, assets, exercise_full, skip_style_restarts=False,
                ui_only=False, mode_only=False):
    profile = dict(PROFILES[profile_name], name=profile_name)
    case = output / profile_name
    case.mkdir(parents=True, exist_ok=False)
    disk = case / 'target.img'
    configure_private_disk(source_image, disk, profile['cfg'])
    fs = FAT16(disk)
    assert fs.read('SYSTEM/SHELL.COM') == shell_bytes, 'Copied image shell changed during setup'
    records, _ = assets
    indices = {name: next(i + 1 for i, (_, title) in enumerate(records) if title == name)
               for name in PHOTO_NAMES}
    bad_fixtures = (install_bad_fixtures(disk, records, photos['Ciuk1'])
                    if exercise_full and not mode_only else [])
    report = {'status': 'running', 'profile': profile_name, 'mode': profile['mode'],
              'display_cfg': profile['cfg'].decode(), 'memory_mib': profile['memory'],
              'device': 'virtio-vga' if profile['vga'] == 'none' else 'std VGA',
              'source_image_sha256': sha256_file(source_image),
              'shell_sha256': sha256(shell_bytes), 'listing': str(listing),
              'listing_sha256': listing_hash, 'binary_overrides': False,
              'guest_ram_writes': False, 'physical_hardware_qualified': False,
              'ui_only_late_checks': ui_only,
              'mode_only': mode_only,
              'malformed_cwp2_fixtures': [
                  {'index': index, 'bytes': len(data),
                   'kind': 'truncated' if len(data) == 15 else 'trailing-byte'}
                  for index, data in bad_fixtures],
              'checks': [], 'scope': 'Copied HDD, real Utilities hit records and QEMU screenshots'}
    report_path = case / 'result.json'
    def save():
        report_path.write_text(json.dumps(report, indent=2) + '\n')

    vm = None
    ui = None
    try:
        vm = fresh_vm(disk, case / 'first', profile)
        report['qemu_command'] = vm.process.args
        ui = Utilities(vm, shell_bytes, listing)
        dismiss_about(vm, ui)
        cfg = FAT16(disk).read('SYSTEM/UI/WALL.CFG')
        assert cfg == bytes((indices['Ciuk1'], WP_FIT)), \
            f'Fresh wallpaper config must default to Ciuk1 Fit: {cfg!r}'
        wait_wallpaper(vm, assets[1]["Ciuk1"]["filename"])
        icon_boxes = desktop_icon_boxes(vm)
        frame = image_frame(vm, 'default-Ciuk1-Fit')
        compare_pixels(frame, assets[1]['Ciuk1']['rgb'], WP_FIT, icon_boxes,
                       f'{profile_name}-default-Ciuk1-Fit', report)
        save()

        # Prove each owner photo is selectable from Desktop Properties and
        # that Apply persists both the chosen catalog index and Fill style.
        current_index = indices['Ciuk1']
        for photo_name in (() if ui_only else ('Ciuk2', 'Ciuk3')):
            cfg, wall_offset = apply_wallpaper(vm, ui, disk, report,
                                               indices[photo_name], 0,
                                               current=current_index)
            current_index = cfg[0]
            filename = assets[1][photo_name]['filename']
            wait_wallpaper(vm, filename, wall_offset)
            frame = image_frame(vm, f'{photo_name}-Fill')
            icon_boxes = desktop_icon_boxes(vm)
            compare_pixels(frame, assets[1][photo_name]['rgb'], 0, icon_boxes,
                           f'{profile_name}-{photo_name}-Fill', report)
            save()

        if exercise_full:
            if not ui_only:
                # Capture the true fallback fill used outside a Fit image. It
                # is generated by the guest, then compared pixel-for-pixel.
                apply_wallpaper(vm, ui, disk, report, 0, 0,
                                current=indices['Ciuk3'])
                base = image_frame(vm, 'solid-background')
                current_index = 0
            else:
                base = None
                current_index = indices['Ciuk1']
            for style in (() if ui_only else range(5)):
                _, wall_offset = apply_wallpaper(vm, ui, disk, report,
                                                 indices['Ciuk1'], style,
                                                 current=current_index)
                current_index = indices['Ciuk1']
                filename = assets[1]['Ciuk1']['filename']
                wait_wallpaper(vm, filename, wall_offset)
                frame = image_frame(vm, f'Ciuk1-{STYLES[style]}')
                icon_boxes = desktop_icon_boxes(vm)
                compare_pixels(frame, assets[1]['Ciuk1']['rgb'], style, icon_boxes,
                               f'{profile_name}-Ciuk1-{STYLES[style]}', report,
                               solid_frame=base)
                save()
                if skip_style_restarts:
                    report['checks'].append({
                        'name': f'{profile_name}-{STYLES[style]}-cold-restart',
                        'skipped': True, 'reason': '--skip-style-restarts'})
                    save()
                    continue
                expected_cfg = bytes((indices['Ciuk1'], style))
                vm.close(); vm = None
                vm = fresh_vm(disk, case / f'style-{style}-cold-restart', profile)
                ui = Utilities(vm, shell_bytes, listing)
                dismiss_about(vm, ui)
                assert FAT16(disk).read('SYSTEM/UI/WALL.CFG') == expected_cfg
                wait_wallpaper(vm, filename)
                frame = image_frame(vm, f'{STYLES[style]}-cold-restart')
                compare_pixels(frame, assets[1]['Ciuk1']['rgb'], style,
                               desktop_icon_boxes(vm),
                               f'{profile_name}-{STYLES[style]}-cold-restart',
                               report, solid_frame=base)
                report['checks'].append({'name': f'{profile_name}-{STYLES[style]}-cold-restart',
                                         'wall_cfg': list(expected_cfg), 'persisted': True})
                save()
            if not ui_only:
                tile_index = next(i + 1 for i, (filename, _) in enumerate(records)
                                  if FAT16(disk).read('SYSTEM/UI/' + filename)[:4] == b'CWP1')
                tile_filename, tile_rgb, kind = read_image(disk, tile_index)
                assert kind == 1
                _, wall_offset = apply_wallpaper(vm, ui, disk, report, tile_index, 4,
                                                 current=indices['Ciuk1'])
                wait_wallpaper(vm, tile_filename, wall_offset)
                tile_frame = image_frame(vm, 'legacy-CWP1-Tile')
                compare_cwp1_tile(tile_frame, tile_rgb, desktop_icon_boxes(vm),
                                  f'{profile_name}-CWP1-Tile', report)
                assert FAT16(disk).read('SYSTEM/UI/WALL.CFG') == bytes((tile_index, 4))

            if not mode_only:
                # Malformed photo records are discovered by the real Display
                # catalog. Applying either must leave the valid selection and
                # rendered desktop intact.
                retained_cfg = FAT16(disk).read('SYSTEM/UI/WALL.CFG')
                vm.position(4, 4)
                before_bad = image_frame(vm, 'before-malformed-CWP2')
                for index, bad_data in bad_fixtures:
                    open_properties(vm, ui)
                    select_wallpaper(vm, ui, report, retained_cfg[0], index)
                    click_action(vm, ui, report, 'Apply malformed CWP2', 11)
                    assert FAT16(disk).read('SYSTEM/UI/WALL.CFG') == retained_cfg, \
                        f'malformed CWP2 entry {index} changed WALL.CFG'
                    ui.until(lambda: ui.b('ui_active_window') == DISPLAY_WINDOW,
                             f'malformed CWP2 entry {index} rejected without closing Properties')
                    click_action(vm, ui, report, 'Cancel rejected malformed CWP2', 14)
                    ui.until(lambda: not (ui.b('ui_window_flags', DISPLAY_WINDOW) & 1),
                             'Cancel closes Display Properties after malformed entry')
                    vm.position(4, 4)
                    after_bad = image_frame(vm, f'after-malformed-CWP2-{index}')
                    changed = int(np.any(before_bad != after_bad, axis=2).sum())
                    assert changed < 1000, \
                        f'malformed CWP2 entry {index} changed {changed} screen pixels'
                    report['checks'].append({
                        'name': f'{profile_name}-malformed-CWP2-{index}',
                        'kind': 'truncated' if len(bad_data) == 15 else 'trailing-byte',
                        'wall_cfg_unchanged': True, 'changed_pixels': changed})
                    save()

                # Cancel must leave the applied two-byte wallpaper preference alone.
                old_cfg = FAT16(disk).read('SYSTEM/UI/WALL.CFG')
                before_cancel = image_frame(vm, 'before-wallpaper-cancel')
                open_properties(vm, ui)
                select_wallpaper(vm, ui, report, old_cfg[0], indices['Ciuk2'])
                click_action(vm, ui, report, 'Select unsaved Center style', 53)
                click_action(vm, ui, report, 'Cancel', 14)
                ui.until(lambda: not (ui.b('ui_window_flags', DISPLAY_WINDOW) & 1),
                         'Cancel closes Display Properties')
                assert FAT16(disk).read('SYSTEM/UI/WALL.CFG') == old_cfg
                after_cancel = image_frame(vm, 'after-wallpaper-cancel')
                unchanged = int(np.any(before_cancel != after_cancel, axis=2).sum())
                report['checks'].append({'name': f'{profile_name}-unsaved-wallpaper-selection-cancel',
                                         'config_unchanged': True,
                                         'changed_pixels': unchanged})
                assert unchanged < 1000, f'Cancel changed the desktop: {unchanged} pixels'

                # Scheme preview is live; Cancel restores both the persisted theme
                # and the actual rendered palette. Reopen the same page for a stable
                # in-window before/after pixel comparison.
                theme_cfg = optional_file(disk, 'SYSTEM/UI/DESKTOP.CFG')
                open_properties(vm, ui)
                click_action(vm, ui, report, 'Appearance tab before theme baseline', 5)
                vm.position(4, 4)
                before_theme = image_frame(vm, 'properties-before-theme-preview')
                window = (ui.w('ui_window_x', DISPLAY_WINDOW), ui.w('ui_window_y', DISPLAY_WINDOW),
                          ui.w('ui_window_width', DISPLAY_WINDOW), ui.w('ui_window_height', DISPLAY_WINDOW))
                cursor_colors_before_ocean = vm.cursor_colors
                vm.cursor_colors = ((20, 32, 56), (242, 250, 255))
                click_action(vm, ui, report, 'Preview colour scheme', 61)
                vm.position(4, 4)
                preview_theme = image_frame(vm, 'theme-preview')
                px, py, pw, ph = window
                preview_delta = int(np.any(before_theme[py:py+ph, px:px+pw] !=
                                           preview_theme[py:py+ph, px:px+pw], axis=2).sum())
                assert preview_delta > 100, 'The appearance scheme preview was not visible'
                click_action(vm, ui, report, 'Cancel theme preview', 14)
                ui.until(lambda: not (ui.b('ui_window_flags', DISPLAY_WINDOW) & 1),
                         'Cancel closes theme preview')
                vm.cursor_colors = cursor_colors_before_ocean
                assert optional_file(disk, 'SYSTEM/UI/DESKTOP.CFG') == theme_cfg, \
                    'Theme Cancel changed the desktop config file'
                open_properties(vm, ui)
                click_action(vm, ui, report, 'Appearance tab after theme cancel', 5)
                vm.position(4, 4)
                after_theme = image_frame(vm, 'properties-after-theme-cancel')
                restored_window = (ui.w('ui_window_x', DISPLAY_WINDOW), ui.w('ui_window_y', DISPLAY_WINDOW),
                                   ui.w('ui_window_width', DISPLAY_WINDOW), ui.w('ui_window_height', DISPLAY_WINDOW))
                assert restored_window == window, \
                    f'Theme Cancel comparison window moved: {window} -> {restored_window}'
                restored_delta = int(np.any(before_theme[py:py+ph, px:px+pw] !=
                                            after_theme[py:py+ph, px:px+pw], axis=2).sum())
                click_action(vm, ui, report, 'Close Properties after theme check', 14)
                ui.until(lambda: not (ui.b('ui_window_flags', DISPLAY_WINDOW) & 1),
                         'Close reopened Display Properties')
                assert restored_delta == 0, f'Theme Cancel did not restore palette: {restored_delta}'
                report['checks'].append({'name': f'{profile_name}-theme-preview-cancel',
                                         'preview_changed_window_pixels': preview_delta,
                                         'restored_window_changed_pixels': restored_delta,
                                         'desktop_cfg_unchanged': True})

                # Real title drag plus completed paint timing and temporal damage
                # samples. Only the old F3 opening latency is used as a baseline.
                offset = vm.offset(); started = time.monotonic()
                vm.key('f3'); vm.wait('[DESKTOP] PAINT', offset, 15)
                report['f3_baseline_completed_paint_seconds'] = time.monotonic() - started
                offset = vm.offset(); vm.key('esc'); vm.wait('[DESKTOP] PAINT', offset, 15)
                open_properties(vm, ui)
                old = (ui.w('ui_window_x', DISPLAY_WINDOW), ui.w('ui_window_y', DISPLAY_WINDOW),
                       ui.w('ui_window_width', DISPLAY_WINDOW), ui.w('ui_window_height', DISPLAY_WINDOW))
                ox, oy, ow, oh = old
                # Properties may already sit against the right/bottom limits.
                # Choose a move inside the actual work area instead of asking
                # the window manager for a correctly clamped, unchanged position.
                dx = 34 if ox + ow + 42 <= ui.w('ui_width') else -34
                dy = 20 if oy + oh + 57 <= ui.w('ui_height') else -20
                start_pt = (ox + 38, oy + 14)
                end_pt = (start_pt[0] + dx, start_pt[1] + dy)
                vm.position(*start_pt)
                before_drag = image_frame(vm, 'before-properties-window-drag')
                offset = vm.offset(); vm.hmp('mouse_button 1')
                ui.until(lambda: ui.b('ui_dragging') == 3,
                         'Properties title accepts the held mouse button')
                vm.position(*end_pt)
                ui.until(lambda: ui.w('ui_window_x', DISPLAY_WINDOW) != ox or
                         ui.w('ui_window_y', DISPLAY_WINDOW) != oy,
                         'Properties window moves while the button is held')
                offset = vm.offset()
                started = time.monotonic(); vm.hmp('mouse_button 0')
                vm.wait('[DESKTOP] PAINT', offset, 20)
                drag_seconds = time.monotonic() - started
                ui.until(lambda: ui.w('ui_window_x', DISPLAY_WINDOW) != ox or
                         ui.w('ui_window_y', DISPLAY_WINDOW) != oy,
                         'Properties window moves with real drag')
                new = (ui.w('ui_window_x', DISPLAY_WINDOW), ui.w('ui_window_y', DISPLAY_WINDOW),
                       ui.w('ui_window_width', DISPLAY_WINDOW), ui.w('ui_window_height', DISPLAY_WINDOW))
                vm.position(4, 4)
                frames, times = burst(vm, 'properties-drag-temporal', count=6)
                screen_w = ui.w('ui_width')
                # Match app_damage's actual window footprint, including its shadow.
                boxes = [(ox, oy, ow + 5, oh + 6),
                         (new[0], new[1], new[2] + 5, new[3] + 6),
                         (min(start_pt[0], end_pt[0])-18, min(start_pt[1], end_pt[1])-18,
                          abs(end_pt[0]-start_pt[0])+36, abs(end_pt[1]-start_pt[1])+36),
                         (0, 0, 40, 32), (0, 0, screen_w, 29)]
                # The desktop telemetry/HUD strip changes with ticks and host
                # resource samples even while the moved Properties window is idle.
                check_damage(vm, report, f'{profile_name}-properties-window-drag',
                             before_drag, frames, boxes, times)
                report.setdefault('window_drag_completed_paint_seconds', []).append(drag_seconds)
                vm.key('esc')
                ui.until(lambda: not (ui.b('ui_window_flags', DISPLAY_WINDOW) & 1),
                         'Close moved Display Properties')

            # A real 640x480 preview is first rolled back, then accepted via
            # OK. The accepted mode must return to the Desktop wallpaper path.
            current_wall = FAT16(disk).read('SYSTEM/UI/WALL.CFG')
            open_properties(vm, ui)
            click_action(vm, ui, report, 'Screen tab', 0)
            vm.repaint_key('home')
            click_action(vm, ui, report, 'Select first 640x480 mode', 20)
            offset = vm.offset()
            rollback_log_baseline = display_log_snapshot(disk)
            click_action(vm, ui, report, 'Apply preview for rollback', 11)
            wait_display_log(disk, 'DISPLAY event=preview-ok id=0110',
                             rollback_log_baseline, 30)
            preview_frame = image_frame(vm, 'display-640-rollback-preview')
            assert preview_frame.shape[:2] == (480, 640), preview_frame.shape
            offset = vm.offset(); rollback_log_baseline = display_log_snapshot(disk)
            vm.key('esc')
            wait_display_log(disk, 'DISPLAY event=restored id=',
                             rollback_log_baseline, 20)
            restored_mode = image_frame(vm, 'display-rollback-restored')
            assert restored_mode.shape[:2] == (profile['mode'][1], profile['mode'][0])
            assert FAT16(disk).read('SYSTEM/VIDEO/DISPLAY.CFG') == profile['cfg']
            # Rollback queues a new probe, which recenters the mode list on
            # the restored mode. Wait for that probe before selecting Home.
            vm.wait('[DISPLAY] adapter', offset, 20)
            vm.repaint_key('home')
            click_action(vm, ui, report, 'Select 640x480 mode again', 20)
            # The display preview can rebuild the wallpaper's mode-specific
            # cache before the user confirms it. Start serial observation
            # before this Apply/OK action so that early ready+paint markers
            # cannot be missed.
            mode_offset = vm.offset()
            confirm_log_baseline = display_log_snapshot(disk)
            click_action(vm, ui, report, 'OK to confirm 640x480 mode', 15)
            wait_display_log(disk, 'DISPLAY event=preview-ok id=0110',
                             confirm_log_baseline, 30)
            confirm_frame = image_frame(vm, 'display-640-confirm-preview')
            assert confirm_frame.shape[:2] == (480, 640), confirm_frame.shape
            keep_log_baseline = display_log_snapshot(disk)
            vm.key('ret')
            wait_display_log(disk, 'DISPLAY event=keep id=0110', keep_log_baseline, 20)
            ui.until(lambda: not (ui.b('ui_window_flags', DISPLAY_WINDOW) & 1),
                     'Confirmed mode closes Display Properties')
            assert FAT16(disk).read('SYSTEM/VIDEO/DISPLAY.CFG') == b'M110'
            wait_wallpaper(vm, f'WALL{current_wall[0]:02d}.CWP', mode_offset)
            # A mode change recenters the software pointer. Park it in the
            # excluded HUD before sampling the wallpaper's actual pixels.
            vm.position(4, 4)
            resized = image_frame(vm, 'background-after-confirmed-640')
            assert resized.shape[:2] == (480, 640)
            filename, pixels, kind = read_image(disk, current_wall[0])
            if FAT16(disk).read('SYSTEM/UI/' + filename)[:4] == b'CWP1':
                assert kind == 1
            compare_pixels(resized, pixels, current_wall[1], desktop_icon_boxes(vm),
                           f'{profile_name}-background-return-640', report)
            assert FAT16(disk).read('SYSTEM/UI/WALL.CFG') == current_wall
            report['checks'].append({'name': f'{profile_name}-640-mode-preview',
                                     'rollback_resolution': list(profile['mode']),
                                     'confirmed_resolution': [640, 480],
                                     'wall_cfg_retained': True})

            vm.close(); vm = None
            restart_profile = dict(profile, mode=(640, 480), cfg=b'M110')
            vm = fresh_vm(disk, case / 'cold-restart', restart_profile)
            report['cold_restart_qemu_command'] = vm.process.args
            ui = Utilities(vm, shell_bytes, listing)
            dismiss_about(vm, ui)
            assert FAT16(disk).read('SYSTEM/UI/WALL.CFG') == current_wall
            wait_wallpaper(vm, filename)
            vm.position(4, 4)
            cold = image_frame(vm, 'cold-restart-wallpaper')
            compare_pixels(cold, pixels, current_wall[1], desktop_icon_boxes(vm),
                           f'{profile_name}-cold-restart-wallpaper', report)
            report['checks'].append({'name': f'{profile_name}-cold-restart',
                                     'wall_cfg': list(current_wall), 'mode': [640, 480]})

        if exercise_full:
            from inspect_boot_hardware import parse_cache_diagnostics
            cache = FAT16(disk).read('SYSTEM/VIDEO/CACHE.LOG')
            decoded_cache = parse_cache_diagnostics(cache)
            (case / 'CACHE.LOG').write_bytes(cache)
            report['cache_diagnostics'] = decoded_cache
            report['checks'].append({'name': 'display-persists-cache-diagnostics',
                                     'bytes': len(cache), 'sha256': sha256(cache)})

        report['status'] = 'pass'
        save()
        return report
    except Exception as error:
        report['status'] = 'failed'
        report['error'] = repr(error)
        save()
        if vm:
            vm.shot('failure')
            (case / 'registers.log').write_bytes(vm.hmp('info registers'))
        raise
    finally:
        if vm:
            vm.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--profiles', default=','.join(PROFILES),
                        help='comma-separated profile names; default: all profiles')
    parser.add_argument('--profile-only', action='store_true',
                        help='skip the style/cancel/theme/move/mode-change suite for full profiles')
    parser.add_argument('--skip-style-restarts', action='store_true',
                        help='skip only the five per-style cold-restart checks')
    parser.add_argument('--ui-only', action='store_true',
                        help='skip Ciuk2/3 and style/CWP1 checks, retaining malformed/cancel/theme/drag/mode checks')
    parser.add_argument('--mode-only', action='store_true',
                        help='run the default photo check and mode preview/rollback/Keep/restart checks only')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    image = args.image.resolve()
    source_hash = sha256_file(image)
    source_fs = FAT16(image)
    original_shell = source_fs.read('SYSTEM/SHELL.COM')
    shell, listing = assemble_shell(output)
    shell_bytes = shell.read_bytes()
    assert original_shell == shell_bytes, \
        'Source HDD SHELL.COM differs from freshly assembled SHELL.COM; refusing stale listing'
    records, photos = check_assets(source_fs)
    profiles = [item.strip() for item in args.profiles.split(',') if item.strip()]
    assert profiles and all(name in PROFILES for name in profiles), profiles
    report = {'status': 'running', 'source_image_sha256': source_hash,
              'shell_sha256': sha256(shell_bytes), 'shell_listing_sha256': sha256(listing.read_bytes()),
              'harness_sha256': sha256(Path(__file__).read_bytes()),
              'binary_overrides': False, 'guest_ram_writes': False,
              'physical_hardware_qualified': False, 'profiles': []}
    (output / 'provenance.json').write_text(json.dumps(report, indent=2) + '\n')
    for name in profiles:
        result = run_profile(image, shell_bytes, listing, sha256(listing.read_bytes()),
                             name, output, photos, (records, photos),
                             exercise_full=(not args.profile_only and
                                            name in ('800-std', '1280-virtio')),
                             skip_style_restarts=args.skip_style_restarts,
                             ui_only=(args.ui_only or args.mode_only),
                             mode_only=args.mode_only)
        report['profiles'].append({'name': name, 'status': result['status'],
                                   'checks': len(result['checks']), 'result': f'{name}/result.json'})
        (output / 'provenance.json').write_text(json.dumps(report, indent=2) + '\n')
        assert sha256_file(image) == source_hash, 'Source HDD image changed'
    report['status'] = 'pass'
    (output / 'provenance.json').write_text(json.dumps(report, indent=2) + '\n')
    print('[properties-background] PASS', json.dumps(report['profiles']), flush=True)


if __name__ == '__main__':
    main()
