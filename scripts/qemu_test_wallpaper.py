#!/usr/bin/env python3
"""Exercise packaged wallpaper through real QEMU input and compare pixels.

Copies the source HDD; never replaces guest binaries/assets or edits guest RAM.
The high-color profile is selected through the actual VGASETUP preview. This
is emulator integration evidence, not a claim about physical GPU behavior.
"""
import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import time
from pathlib import Path

import numpy as np
from PIL import Image

from qemu_test_installed_hdd import FAT16
from qemu_test_native_windows import WindowVM


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def palette(out):
    source = out / 'palette.asm'
    source.write_text('%include "src/com/ui_theme.inc"\nCIUKIOS_PALETTE\nCIUKI_LOGO_PALETTE\nUI_ICONS_PALETTE\n')
    target = out / 'palette.bin'
    subprocess.run(['nasm', '-f', 'bin', str(source), '-o', str(target)], check=True)
    return np.frombuffer(target.read_bytes(), np.uint8).reshape(-1, 3)


def cover_frame(source, screen_width, screen_height):
    """Match WALLP's centered, aspect-preserving Fill geometry and sampling."""
    source_height, source_width = source.shape[:2]
    view_height = screen_height - 61
    if screen_width * source_height >= view_height * source_width:
        draw_width = screen_width
        draw_height = (source_height * screen_width + source_width - 1) // source_width
    else:
        draw_height = view_height
        draw_width = (source_width * view_height + source_height - 1) // source_height
    delta_x, delta_y = screen_width - draw_width, view_height - draw_height
    # The renderer divides signed coordinates with C truncation toward zero.
    left = delta_x // 2 if delta_x >= 0 else -((-delta_x) // 2)
    top_delta = delta_y // 2 if delta_y >= 0 else -((-delta_y) // 2)
    top = 29 + top_delta
    xs = np.arange(screen_width, dtype=np.int64) - left
    ys = np.arange(29, 29 + view_height, dtype=np.int64) - top
    sx = np.clip(xs * source_width // draw_width, 0, source_width - 1)
    sy = np.clip(ys * source_height // draw_height, 0, source_height - 1)
    return source[sy[:, None], sx[None, :]]


def source_tiles(image, sources, report):
    """Prove every packaged tile round-trips to its owner-supplied BMP."""
    catalog = image.read('SYSTEM/UI/WALLS.DAT')
    magic, count, reserved = struct.unpack_from('<4sHH', catalog)
    assert magic == b'CWC1' and reserved == 0 and count == 11
    result = {}
    for index, source in enumerate(sorted(sources.glob('*.bmp'), key=lambda p: p.name.casefold()), 1):
        name = catalog[8 + (index-1)*44:21 + (index-1)*44].split(b'\0')[0].decode()
        raw = image.read('SYSTEM/UI/' + name)
        magic, width, height, colors, flags, size = struct.unpack_from('<4sHHHHI', raw)
        assert magic == b'CWP1' and colors == 256 and flags == 0
        assert size == width*height and len(raw) == 16+768+size
        rgb = np.asarray(Image.open(source).convert('RGB'))
        pal = np.frombuffer(raw[16:784], np.uint8).reshape(256, 3)
        indices = np.frombuffer(raw[784:], np.uint8).reshape(height, width)
        assert np.array_equal(pal[indices], rgb), f'Lossless conversion mismatch: {source}'
        result[index] = rgb
        report['conversion'].append({'source': str(source), 'runtime': name,
                                     'pixels': size, 'exact_rgb_match': True,
                                     'runtime_sha256': hashlib.sha256(raw).hexdigest()})
    assert len(result) == count
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--mode', choices=['8', '32'], required=True)
    parser.add_argument('--sources', type=Path, default=Path('third_party/win_bg'))
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    disk = out / 'target.img'
    source_hash = digest(args.image)
    shutil.copyfile(args.image, disk)
    filesystem = FAT16(disk)
    report = {'status': 'running', 'mode_requested': int(args.mode), 'memory_mib': 128,
              'wallpaper_style': 'Fill (index/style pair)',
              'source_sha256': source_hash, 'binary_overrides': False,
              'ram_modifications': False, 'conversion': [], 'checks': [],
              'scope': 'Actual packaged QEMU input/screenshots, not physical GPU qualification',
              'shell_sha256': hashlib.sha256(filesystem.read('SYSTEM/SHELL.COM')).hexdigest()}
    report_path = out / 'result.json'
    tiles = source_tiles(filesystem, args.sources, report)
    ui_palette = palette(out).astype(np.int32)
    native = {}
    for index, rgb in tiles.items():
        if args.mode == '32':
            native[index] = rgb
        else:
            colors, inverse = np.unique(rgb.reshape(-1, 3), axis=0, return_inverse=True)
            distances = ((colors.astype(np.int32)[:, None, :] // 4-ui_palette[None, :, :])**2).sum(2)
            chosen = ui_palette[distances.argmin(1)]
            # QEMU VGA c6_to_8 repeats bit0 into the low two bits, rather
            # than multiplying by255/63. Match its documented presentation
            # exactly: https://gitlab.com/qemu-project/qemu/-/blob/master/hw/display/vga_int.h
            native[index] = ((chosen << 2) | ((chosen & 1)*3)).astype(np.uint8)[inverse].reshape(rgb.shape)

    vm = None
    library_visible = True
    def save():
        report_path.write_text(json.dumps(report, indent=2) + '\n')

    def frame(name):
        return np.asarray(Image.open(vm.shot(name)).convert('RGB'))

    def compare(index, name, excluded=()):
        vm.position(75, 35)
        time.sleep(.35)
        actual = frame(name)
        height, width = actual.shape[:2]
        tile = native[index]
        expected = actual.copy()
        expected[29:height-32] = cover_frame(tile, width, height)
        mask = np.zeros((height, width), bool)
        # Exclude chrome, desktop shortcuts and lower-right mode/clock text.
        mask[40:height-80, 150:width-15] = True
        for x, y, w, h in excluded:
            mask[max(0,y):min(height,y+h), max(0,x):min(width,x+w)] = False
        delta = np.abs(actual.astype(np.int16)-expected.astype(np.int16))
        tolerance = 0
        bad = (delta.max(2) > tolerance) & mask
        item = {'name': name, 'tile': index, 'frame': [width, height],
                'compared_pixels': int(mask.sum()), 'tolerance': tolerance,
                'mismatched_pixels': int(bad.sum()), 'max_channel_delta': int(delta[mask].max()), 'excluded_rectangles': list(excluded)}
        report['checks'].append(item)
        save()
        assert item['compared_pixels'] > 100000
        assert not bad.any(), f'{name}: {item}'
        return actual

    def ready(offset=0):
        vm.ready(offset)
        vm.pointer()

    def open_panel():
        nonlocal library_visible
        vm.repaint_key('f2')
        library_visible = True
        x, y, _ = vm.active_rect()
        vm.completed_control_click(x+258, y+50)   # Programs > System
        vm.repaint_key('down')                  # System item 3, Tasks
        vm.repaint_key('right')                 # System item 4, Wallpaper
        vm.repaint_key('ret')
        rect = vm.active_rect()
        assert rect[2] == 466, ('Wallpaper frame', rect)
        # Hide Programs while retaining Wallpaper, through real activation.
        vm.repaint_key('alt-tab')
        lx, ly, lw = vm.active_rect()
        assert lw > 466, ('Programs frame', lx, ly, lw)
        vm.completed_control_click(lx+lw-64, ly+15)
        library_visible = False
        return vm.active_rect()

    def apply(index):
        x, y, _ = open_panel()
        assert 1 <= index <= 5
        vm.completed_control_click(x+285, y+46+(index-1)*24)
        vm.completed_control_click(x+386, y+201)
        vm.shot(f'panel-applied-{index}')
        assert FAT16(disk).read('SYSTEM/UI/WALL.CFG') == bytes([index, 0]), 'Apply did not persist index and Fill style'
        vm.repaint_key('alt-f4')
        compare(index, f'applied-{index}')

    try:
        vm = WindowVM(disk, out/'first', 'std', memory=128, palette='platinum',
                      video='1280x1024' if args.mode == '32' else None)
        vm.auto_enter_dos = False
        report['qemu_command'] = vm.process.args
        ready()
        requested = '800' if args.mode == '8' else '1280'
        offset = vm.offset()
        vm.key('f4')
        vm.wait('CiukiOS SHELL C:\\APPS>', offset, 60)
        offset = vm.offset()
        vm.text('vgasetup desktop ' + requested)
        vm.wait('[VGASETUP] DESKTOP PREVIEW', offset, 60)
        time.sleep(.3)
        vm.key('ret')
        vm.wait('[VGASETUP] Desktop resolution saved', offset, 30)
        vm.wait('CiukiOS SHELL C:\\APPS>', offset, 30)
        offset = vm.offset()
        vm.text('exit')
        ready(offset)
        assert FAT16(disk).read('SYSTEM/VIDEO/DISPLAY.CFG') == (b'0800' if args.mode == '8' else b'1280')
        serial = subprocess.check_output(['scripts/serial_log_normalize.py', str(vm.serial)]).decode(errors='replace')
        assert f'/ {args.mode}-bit color' in serial, f'Requested depth not observed in actual guest: {serial[-1500:]}'
        apply(1)                 # maximum 65,536-pixel source; crosses read size edge
        apply(5)                 # non-power-of-two 160px tile, different palette
        vm.repaint_key('f3')
        x, y, w = vm.active_rect()
        vm.position(x+160, y+13)
        vm.hmp('mouse_button 1')
        time.sleep(.15)
        vm.move(30, 22)
        vm.hmp('mouse_button 0')
        time.sleep(.5)
        nx, ny, nw = vm.active_rect()
        assert (nx,ny) != (x,y), 'The real title drag did not move the window'
        compare(5, 'after-damage-drag', [(nx,ny,nw+5,232)])
        vm.repaint_key('alt-f4')
        compare(5, 'after-window-close')
        offset = vm.offset()
        vm.key('f4')
        vm.wait('CiukiOS SHELL C:\\APPS>', offset, 60)
        offset = vm.offset()
        vm.text('comdemo')
        vm.wait('COM demo via INT21h', offset, 30)
        vm.wait('CiukiOS SHELL C:\\APPS>', offset, 30)
        offset = vm.offset()
        vm.text('exit')
        ready(offset)
        compare(5, 'after-dos-child-return')
        vm.close()
        vm = None
        # A new QEMU process reads the persisted file from the same HDD copy;
        # no in-memory window state or texture can survive this cold restart.
        vm = WindowVM(disk, out/'restart', 'std', memory=128, palette='platinum',
                      video='1280x1024' if args.mode == '32' else None)
        vm.auto_enter_dos = False
        ready()
        x, y, w = vm.active_rect()
        vm.completed_control_click(x+w-64, y+15)
        compare(5, 'after-cold-restart')
        assert digest(args.image) == source_hash, 'The original HDD image was modified'
        report['status'] = 'pass'
        save()
        print('[wallpaper] PASS', args.mode, json.dumps(report['checks']), flush=True)
    except Exception as error:
        report['status'] = 'failed'
        report['error'] = str(error)
        save()
        if vm:
            vm.shot('failure')
            (out/'registers.log').write_bytes(vm.hmp('info registers'))
        raise
    finally:
        if vm:
            vm.close()


if __name__ == '__main__':
    main()
