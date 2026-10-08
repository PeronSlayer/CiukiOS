#!/usr/bin/env python3
"""Validate the real CWP2 photo wallpaper path at 800x600 and 1024x768.

Each case uses a private HDD copy, real Wallpaper-window input, screenshots,
and cold restarts. The source image and guest RAM are never modified. The
malformed CWP2 fixtures are installed only into that private image before boot.
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
from qemu_test_native_windows import WindowVM
from qemu_test_wallpaper import palette as ui_palette

WP_FIT = 1

ROOT = Path(__file__).resolve().parent.parent
PHOTO_ROOT = ROOT / 'misc/ciukios_bg'
PHOTO_NAMES = ('Ciuk1', 'Ciuk2', 'Ciuk3')
PHOTO_SIZE = (1672, 941)
PHOTO_BYTES = PHOTO_SIZE[0] * PHOTO_SIZE[1] * 3
PHOTO_FILE_BYTES = 16 + PHOTO_BYTES


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def parse_catalog(raw):
    magic, count, reserved = struct.unpack_from('<4sHH', raw)
    assert magic == b'CWC1' and reserved == 0 and len(raw) >= 8 + 44 * count
    records = []
    for i in range(count):
        rec = raw[8 + i * 44:8 + (i + 1) * 44]
        filename = rec[:13].split(b'\0', 1)[0].decode('ascii')
        title = rec[13:44].split(b'\0', 1)[0].decode('ascii')
        records.append((filename, title))
    return records


def check_assets(fs):
    records = parse_catalog(fs.read('SYSTEM/UI/WALLS.DAT'))
    photos = {}
    for number, title in enumerate(PHOTO_NAMES, 1):
        matches = [i + 1 for i, (_, label) in enumerate(records) if label == title]
        assert len(matches) == 1, f'CWC1 must contain exactly one {title}: {matches}'
        index = matches[0]
        filename = records[index - 1][0]
        assert filename.upper().endswith('.CWP')
        payload = fs.read('SYSTEM/UI/' + filename)
        assert len(payload) == PHOTO_FILE_BYTES, (filename, len(payload))
        magic, width, height, stride, encoding, flags, length = struct.unpack_from('<4sHHHBBI', payload)
        assert (magic, width, height, stride, encoding, flags, length) == (
            b'CWP2', *PHOTO_SIZE, PHOTO_SIZE[0] * 3, 1, 0, PHOTO_BYTES), filename
        source_path = PHOTO_ROOT / f'{title}.png'
        with Image.open(source_path) as image:
            source_rgb = image.convert('RGB')
            assert source_rgb.size == PHOTO_SIZE
            source = np.asarray(source_rgb, dtype=np.uint8)
        assert payload[16:] == source.tobytes(), f'{filename} differs from exact owner RGB pixels'
        photos[title] = {'index': index, 'filename': filename, 'rgb': source,
                         'sha256': sha256(payload), 'source_sha256': sha256(source_path.read_bytes())}
    assert [photos[name]['index'] for name in PHOTO_NAMES] == sorted(
        photos[name]['index'] for name in PHOTO_NAMES), 'Ciuk photos must preserve append order'
    return records, photos


def image_frame(vm, label):
    return np.asarray(Image.open(vm.shot(label)).convert('RGB'), dtype=np.uint8)


def desktop_icon_boxes(vm, width, height):
    serial = subprocess.check_output(['scripts/serial_log_normalize.py',
                                      str(vm.serial)]).decode('cp437', 'replace')
    boxes = []
    for _, cx, cy in re.findall(r'\[DESK\] icon (\S+) (\d+) (\d+)', serial):
        x, y = int(cx) - 42, int(cy) - 20
        boxes.append((max(0, x - 4), max(0, y - 4), 92,
                      min(270, height - y + 8)))
    return boxes


def fit_samples(left, top, draw_w, draw_h, icons, target=36):
    candidates = []
    for yi in range(17):
        y = top + max(0, (draw_h - 1) * yi // 16)
        for xi in range(17):
            x = left + max(0, (draw_w - 1) * xi // 16)
            if not any(ix <= x < ix + iw and iy <= y < iy + ih
                       for ix, iy, iw, ih in icons):
                candidates.append((x, y))
    assert len(candidates) >= target, ('too many desktop icons obscure Fit sample area', len(candidates))
    return [candidates[round(i * (len(candidates) - 1) / (target - 1))]
            for i in range(target)]


def fit_bar_samples(width, height, icons, fit_rect):
    left, top, draw_w, draw_h = fit_rect
    right, bottom = left + draw_w, top + draw_h
    work_bottom = height - 32
    margins = []
    if left > 0:
        margins.append(('left', (left // 2, max(30, top + draw_h // 3)),
                        (left // 2, max(30, top + 2 * draw_h // 3))))
    if right < width:
        x = (right + width) // 2
        margins.append(('right', (x, max(30, top + draw_h // 3)),
                        (x, max(30, top + 2 * draw_h // 3))))
    if top > 29:
        y = (29 + top) // 2
        margins.append(('top', (left + draw_w // 3, y),
                        (left + 2 * draw_w // 3, y)))
    if bottom < work_bottom:
        y = (bottom + work_bottom) // 2
        margins.append(('bottom', (left + draw_w // 3, y),
                        (left + 2 * draw_w // 3, y)))
    points = []
    for name, *pair in margins:
        visible = [point for point in pair if not any(
            ix <= point[0] < ix + iw and iy <= point[1] < iy + ih
            for ix, iy, iw, ih in icons)]
        assert visible, ('desktop icons obscure a Fit margin', name, pair, icons)
        points.extend(visible)
    return points


def compare_photo(vm, title, photo, output, label):
    vm.position(4, 4)
    time.sleep(.25)
    actual = image_frame(vm, label)
    height, width = actual.shape[:2]
    source = photo['rgb']
    src_h, src_w = source.shape[:2]
    view_h = height - 61
    if width * src_h <= view_h * src_w:
        draw_w, draw_h = width, src_h * width // src_w
    else:
        draw_h, draw_w = view_h, src_w * view_h // src_h
    left, top = (width - draw_w) // 2, 29 + (view_h - draw_h) // 2
    assert draw_w <= width and draw_h <= view_h and (draw_w < width or draw_h < view_h), \
        (left, top, draw_w, draw_h, width, view_h)
    icons = desktop_icon_boxes(vm, width, height)
    samples = []
    for x, y in fit_samples(left, top, draw_w, draw_h, icons):
        sx = min(src_w - 1, (x - left) * src_w // draw_w)
        sy = min(src_h - 1, (y - top) * src_h // draw_h)
        # Permit the adjacent source sample for fixed-point scaling and
        # either nearest-neighbour tie convention.
        nearby = source[max(0, sy - 1):min(src_h, sy + 2),
                        max(0, sx - 1):min(src_w, sx + 2)].astype(np.int16)
        observed = actual[y, x].astype(np.int16)
        delta = np.max(np.abs(nearby - observed), axis=2)
        samples.append({'screen': [x, y], 'source': [sx, sy],
                        'max_channel_delta': int(delta.min())})
    deltas = [sample['max_channel_delta'] for sample in samples]
    bar_points = fit_bar_samples(width, height, icons, (left, top, draw_w, draw_h))
    bars = [actual[y, x].astype(np.int16) for x, y in bar_points]
    bar_delta = max((int(np.max(np.abs(pixel - bars[0]))) for pixel in bars), default=0)
    report = {'name': label, 'photo': title, 'frame': [width, height],
              'fit': {'left': left, 'top': top, 'width': draw_w, 'height': draw_h,
                      'photo_rect': [0, 29, width, view_h]},
              'bar_samples': [list(point) for point in bar_points], 'bar_max_delta': bar_delta,
              'sample_count': len(samples), 'median_delta': float(np.median(deltas)),
              'max_delta': max(deltas), 'samples': samples}
    output.append(report)
    assert len(samples) == 36 and max(deltas) <= 48 and np.median(deltas) <= 24, report
    assert not bar_points or bar_delta <= 4, report
    return actual


def tile_candidates(rgb, palette_rgb):
    rgb32 = rgb.astype(np.int32)
    candidates = [rgb32]
    # Supported direct-color renderings (the screenshots are RGB regardless
    # of the VBE channel masks).
    for red_bits, green_bits, blue_bits in ((5, 6, 5), (5, 5, 5)):
        levels = (red_bits, green_bits, blue_bits)
        expanded = np.empty_like(rgb32)
        for channel, bits in enumerate(levels):
            maximum = (1 << bits) - 1
            quantized = (rgb32[..., channel] * maximum + 127) // 255
            expanded[..., channel] = (quantized * 255 + maximum // 2) // maximum
        candidates.append(expanded)
    # The existing VGA16 wallpaper renderer maps source colors to the fixed
    # CiukiOS palette, then QEMU expands VGA DAC values to screenshot RGB.
    colors, inverse = np.unique(rgb.reshape(-1, 3), axis=0, return_inverse=True)
    pal = palette_rgb.astype(np.int32)
    distance = ((colors.astype(np.int32)[:, None, :] // 4 - pal[None, :, :]) ** 2).sum(2)
    chosen = pal[distance.argmin(1)]
    vga = ((chosen << 2) | ((chosen & 1) * 3)).astype(np.int32)[inverse].reshape(rgb.shape)
    candidates.append(vga)
    return candidates


def compare_legacy_fit(vm, fs, record, palette_rgb, checks, label):
    raw = fs.read('SYSTEM/UI/' + record[0])
    magic, width, height, colors, flags, size = struct.unpack_from('<4sHHHHI', raw)
    assert magic == b'CWP1' and colors == 256 and flags == 0 and size == width * height
    indexed = np.frombuffer(raw[16 + 768:], np.uint8).reshape(height, width)
    pal = np.frombuffer(raw[16:16 + 768], np.uint8).reshape(256, 3)
    rgb = pal[indexed]
    vm.position(4, 4)
    time.sleep(.25)
    actual = image_frame(vm, label)
    screen_h, screen_w = actual.shape[:2]
    icons = desktop_icon_boxes(vm, screen_w, screen_h)
    values = []
    for candidate in tile_candidates(rgb, palette_rgb):
        view_h = screen_h - 61
        if screen_w * height <= view_h * width:
            draw_w, draw_h = screen_w, height * screen_w // width
        else:
            draw_h, draw_w = view_h, width * view_h // height
        left, top = (screen_w - draw_w) // 2, 29 + (view_h - draw_h) // 2
        errors = []
        for x, y in fit_samples(left, top, draw_w, draw_h, icons):
            sx, sy = (x - left) * width // draw_w, (y - top) * height // draw_h
            expected = candidate[sy, sx]
            errors.append(int(np.max(np.abs(actual[y, x].astype(np.int16) - expected.astype(np.int16)))))
        values.append(errors)
    best = min(values, key=lambda errors: (sum(errors) / len(errors), max(errors)))
    entry = {'name': label, 'wallpaper': record[0], 'position': 'Fit', 'frame': [screen_w, screen_h],
             'sample_count': len(best), 'median_delta': float(np.median(best)),
             'max_delta': max(best)}
    checks.append(entry)
    assert max(best) <= 48 and np.median(best) <= 24, entry


def install_bad_fixtures(disk, records, photo):
    # The extra numbered entries are discovered by the real shell wallpaper
    # catalog. The malformed headers exercise both truncation and trailing
    # bytes while retaining the valid photo already cached by the desktop.
    count = len(records)
    raw = (ROOT / 'misc/ciukios_bg' / 'Ciuk1.png')
    valid = bytearray(16 + PHOTO_BYTES)
    with Image.open(raw) as image:
        source = image.convert('RGB').tobytes()
    struct.pack_into('<4sHHHBBI', valid, 0, b'CWP2', *PHOTO_SIZE,
                     PHOTO_SIZE[0] * 3, 1, 0, PHOTO_BYTES)
    valid[16:] = source
    fixtures = ((count + 1, bytes(valid[:15])),
                (count + 2, bytes(valid) + b'!'))
    out = disk.parent
    volume = f'{disk}@@{FAT16(disk).start}'
    for index, data in fixtures:
        path = out / f'bad-wall-{index:02}.cwp'
        path.write_bytes(data)
        subprocess.run(['mcopy', '-o', '-i', volume, str(path),
                        f'::SYSTEM/UI/WALL{index:02}.CWP'], check=True)
    return fixtures


def configure_resolution(vm, disk, resolution):
    vm.ready()
    vm.key('f4')
    offset = vm.offset()
    vm.wait('CiukiOS SHELL C:\\APPS>', offset, 60)
    offset = vm.offset()
    vm.text(f'vgasetup desktop {resolution}')
    vm.wait('[VGASETUP] DESKTOP PREVIEW', offset, 60)
    vm.key('ret')
    vm.wait('[VGASETUP] Desktop resolution saved', offset, 30)
    vm.wait('CiukiOS SHELL C:\\APPS>', offset, 30)
    offset = vm.offset()
    vm.text('exit')
    vm.ready(offset)
    expected = b'0800' if resolution == 800 else b'1024'
    assert FAT16(disk).read('SYSTEM/VIDEO/DISPLAY.CFG') == expected


def assemble_shell(output):
    shell = output / 'assembled-SHELL.COM'
    listing = output / 'SHELL.lst'
    subprocess.run([
        'nasm', '-f', 'bin', '-l', str(listing), '-o', str(shell),
        'src/com/shell.asm',
    ], cwd=ROOT, check=True)
    return shell, listing


def run_case(image_path, listing, shell_bytes, output, resolution, source_hash,
             shell_listing_sha256):
    case_dir = output / str(resolution)
    case_dir.mkdir(parents=True, exist_ok=True)
    disk = case_dir / 'target.img'
    shutil.copyfile(image_path, disk)
    fs = FAT16(disk)
    assert fs.read('SYSTEM/SHELL.COM') == shell_bytes, \
        'private HDD copy SHELL.COM differs from the verified canonical shell'
    catalog_raw = fs.read('SYSTEM/UI/WALLS.DAT')
    records, photos = check_assets(fs)
    bad_fixtures = install_bad_fixtures(disk, records, photos['Ciuk1'])
    shell = fs.read('SYSTEM/SHELL.COM')
    report = {'status': 'running', 'resolution': resolution,
              'source_image_sha256': source_hash, 'shell_sha256': sha256(shell),
              'shell_listing': listing.name,
              'shell_listing_sha256': shell_listing_sha256,
              'memory_mib': 128, 'binary_overrides': False, 'guest_ram_writes': False,
              'photo_assets': {name: {'index': p['index'], 'filename': p['filename'],
                                      'sha256': p['sha256'], 'source_sha256': p['source_sha256']}
                               for name, p in photos.items()},
              'malformed_fixtures': [{'index': i, 'bytes': len(data), 'kind':
                                      ('truncated' if len(data) == 15 else 'trailing-byte')}
                                     for i, data in bad_fixtures],
              'checks': [], 'scope': 'Copied full HDD image and QEMU screenshots; no physical display claim'}
    report_path = case_dir / 'result.json'
    vm = None
    ui = None
    current_page = 0
    def save():
        report_path.write_text(json.dumps(report, indent=2) + '\n')
    def open_wallpaper():
        nonlocal current_page
        # The classic Programs library is opened by the public Ctrl+Esc
        # shortcut.  The former desktop-menu action was removed when the
        # top brand menu became the entry point, and now resolves to action 13.
        vm.key('ctrl-esc')
        ui.until(lambda: ui.b('ui_window_visible') == 1,
                 'Ctrl+Esc opens the Programs library')
        ui.click(22, 0)
        ui.click(34, 0)
        vm.key('ret')
        ui.until(lambda: ui.b('ui_active_window') == 10, 'Wallpaper opens')
        current_page = ui.w('wp_page')
    def close_wallpaper():
        vm.repaint_key('alt-f4')
        ui.until(lambda: not (ui.b('ui_window_flags', 10) & 1),
                 'Wallpaper window closes', 20)
        if ui.b('ui_window_visible') == 1:
            vm.repaint_key('esc')
            ui.until(lambda: ui.b('ui_window_visible') == 0,
                     'Application Library closes before desktop pixel comparison', 20)
    def dismiss_default_about(label):
        # About is the first-run window in the fresh 800/1024 desktop profile.
        # Raise it by its visible title bar, then use the normal Escape action
        # so pixel checks see the desktop behind it.
        if ui.b('ui_window_flags', 2) & 1:
            x = ui.w('ui_window_x', 2)
            y = ui.w('ui_window_y', 2)
            width = ui.w('ui_window_width', 2)
            vm.position(x + width // 2, y + 12)
            vm.hmp('mouse_button 1')
            time.sleep(.15)
            vm.hmp('mouse_button 0')
            time.sleep(.3)
            ui.until(lambda: ui.b('ui_active_window') == 2,
                     'About title click raises the window', 10)
            offset = vm.offset()
            vm.key('esc')
            ui.until(lambda: not (ui.b('ui_window_flags', 2) & 1),
                     'default About window closes with Escape', 20)
            report['checks'].append({'name': f'{resolution}-{label}-about-dismissed',
                                     'method': 'Escape', 'window': 2})
            save()
    def choose(index):
        nonlocal current_page
        target_page = ((index - 1) // 5) * 5
        while current_page < target_page:
            ui.click(182, 10); current_page += 5
        while current_page > target_page:
            ui.click(181, 10); current_page -= 5
        ui.click(184 + ((index - 1) % 5), 10)
    def apply_photo(title):
        photo = photos[title]
        open_wallpaper()
        choose(photo['index'])
        offset = vm.offset()
        ui.click(183, 10)
        assert FAT16(disk).read('SYSTEM/UI/WALL.CFG') == bytes([photo['index'], WP_FIT]), \
            f'{title} selection was not persisted'
        close_wallpaper()
        vm.wait(f'[WALLP] ready {photo["filename"]}', offset, 30)
        compare_photo(vm, title, photo, report['checks'], f'{resolution}-{title}')
        save()
    try:
        video = '1024x768' if resolution == 1024 else None
        vm = WindowVM(disk, case_dir / 'first', 'std', memory=128, palette='platinum', video=video)
        vm.auto_enter_dos = False
        configure_resolution(vm, disk, resolution)
        ui = Utilities(vm, shell, listing)
        dismiss_default_about('initial')
        photo1 = photos['Ciuk1']
        assert FAT16(disk).read('SYSTEM/UI/WALL.CFG') == bytes([photo1['index'], WP_FIT]), \
            'Fresh profile must default to the Ciuk1 Fit entry'
        vm.wait(f'[WALLP] ready {photo1["filename"]}', 0, 45)
        compare_photo(vm, 'Ciuk1', photo1, report['checks'], f'{resolution}-default-Ciuk1')
        save()
        apply_photo('Ciuk2')
        apply_photo('Ciuk3')
        vm.close(); vm = None

        # A cold process must reload the persisted Ciuk3 selection and pixels.
        vm = WindowVM(disk, case_dir / 'restart', 'std', memory=128,
                      palette='platinum', video=video)
        vm.auto_enter_dos = False
        vm.ready()
        ui = Utilities(vm, shell, listing)
        dismiss_default_about('restart')
        photo3 = photos['Ciuk3']
        assert FAT16(disk).read('SYSTEM/UI/WALL.CFG') == bytes([photo3['index'], WP_FIT])
        vm.wait(f'[WALLP] ready {photo3["filename"]}', 0, 45)
        compare_photo(vm, 'Ciuk3', photo3, report['checks'], f'{resolution}-cold-restart')
        save()

        # Bad CWP2 entries are discovered through the normal catalog. Apply
        # must fail without changing WALL.CFG or the currently rendered photo.
        for index, bad_data in bad_fixtures:
            open_wallpaper()
            choose(index)
            ui.click(183, 10)
            ui.until(lambda: 'could not be loaded' in ui.z_at(ui.w('wp_status') - 256),
                     f'malformed CWP2 entry {index} rejected')
            assert FAT16(disk).read('SYSTEM/UI/WALL.CFG') == bytes([photo3['index'], WP_FIT]), \
                f'malformed CWP2 entry {index} changed persisted selection'
            close_wallpaper()
            compare_photo(vm, 'Ciuk3', photo3, report['checks'], f'{resolution}-retain-after-bad-{index}')
            report['checks'].append({'name': f'{resolution}-bad-entry-{index}',
                                     'kind': 'truncated' if len(bad_data) == 15 else 'trailing-byte',
                                     'rejected': True, 'retained_photo': 'Ciuk3'})
            save()

        # Legacy CWP1 remains decodable under the persisted default Fit style.
        legacy = next((i + 1, record) for i, record in enumerate(records)
                      if fs.read('SYSTEM/UI/' + record[0])[:4] == b'CWP1')
        index, record = legacy
        open_wallpaper()
        choose(index)
        ui.click(183, 10)
        assert FAT16(disk).read('SYSTEM/UI/WALL.CFG') == bytes([index, WP_FIT])
        close_wallpaper()
        compare_legacy_fit(vm, fs, record, ui_palette(case_dir).astype(np.uint8),
                            report['checks'], f'{resolution}-legacy-cwp1-fit')
        report['checks'].append({'name': f'{resolution}-legacy-cwp1-fit',
                                 'position': 'Fit',
                                 'filename': record[0], 'catalog_index': index,
                                 'catalog_unchanged': FAT16(disk).read('SYSTEM/UI/WALLS.DAT') == catalog_raw})
        assert FAT16(disk).read('SYSTEM/UI/WALLS.DAT') == catalog_raw
        save()
        vm.close(); vm = None
        assert sha256(image_path.read_bytes()) == source_hash, 'Source HDD image changed'
        report['status'] = 'pass'
        save()
        print(f'[photo-wallpaper] PASS {resolution}x{resolution * 3 // 4}', flush=True)
    except Exception as error:
        report['status'] = 'failed'
        report['error'] = str(error)
        save()
        if vm:
            vm.shot(f'{resolution}-failure')
            (case_dir / 'registers.log').write_bytes(vm.hmp('info registers'))
        raise
    finally:
        if vm:
            vm.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--resolution', choices=('both', '800', '1024'), default='both')
    args = parser.parse_args()
    image = args.image.resolve()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source_hash = sha256(image.read_bytes())
    shell_path, listing = assemble_shell(out)
    shell_bytes = shell_path.read_bytes()
    shipped_shell = FAT16(image).read('SYSTEM/SHELL.COM')
    assert shell_bytes == shipped_shell, \
        'freshly assembled SHELL.COM does not match the source HDD FAT16 copy'
    shell_listing_sha256 = sha256(listing.read_bytes())
    resolutions = (800, 1024) if args.resolution == 'both' else (int(args.resolution),)
    for resolution in resolutions:
        run_case(image, listing, shell_bytes, out, resolution, source_hash,
                 shell_listing_sha256)
    print('[photo-wallpaper] PASS all requested resolutions', flush=True)


if __name__ == '__main__':
    main()
