#!/usr/bin/env python3
"""Exercise Files image associations and rendered Viewer output in QEMU.

Creates private asymmetric four-colour images (BMP8/BMP24/top-down BMP32,
PNG, JPEG and static GIF), opens each through Files, and locates the actual
rendered RGB pixels in QEMU screenshots. JPEG comparisons allow codec error;
all lossless formats use a tight true-colour tolerance. The private disk copy
also contains a truncated BMP for the failure path. This is emulator evidence,
not a physical-display qualification.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qemu_test_desktop_apps import Gate                         # noqa: E402
from qemu_test_installed_hdd import FAT16                       # noqa: E402
from qemu_test_native_windows import WindowVM                   # noqa: E402

WIDTH, HEIGHT = 200, 120
PALETTE = ((235, 45, 35), (20, 205, 70), (35, 75, 235), (240, 205, 25))
FILES = ('A8.BMP', 'B24.BMP', 'C32.BMP', 'D.GIF', 'E.JPEG', 'F.PNG')


def sha256_file(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def fixture_pixels(variant=0):
    """A deliberately asymmetric palette pattern with internal orientation marks."""
    colors = PALETTE[variant:] + PALETTE[:variant]
    pixels = np.empty((HEIGHT, WIDTH, 3), dtype=np.uint8)
    for y in range(HEIGHT):
        for x in range(WIDTH):
            q = (0 if y < 47 else 2) + (0 if x < 117 else 1)
            pixels[y, x] = colors[q]
    # 4x4 coded cells at the top-left make orientation and scale easy to find.
    code = ((0, 1, 2, 3), (2, 0, 3, 1), (1, 3, 0, 2), (3, 2, 1, 0))
    for cy in range(4):
        for cx in range(4):
            pixels[8 + cy * 4:12 + cy * 4, 8 + cx * 4:12 + cx * 4] = colors[code[cy][cx]]
    for bit in range(3):
        marker_color = colors[(variant >> bit) & 1]
        pixels[28:36, 28 + bit * 8:36 + bit * 8] = marker_color
    # A distinct lower-right marker prevents accidental symmetry matches.
    pixels[88:112, 164:188] = colors[3]
    pixels[92:108, 168:184] = colors[0]
    pixels[96:104, 172:180] = colors[2]
    pixels[18:42, 146:151] = colors[0]
    return pixels


def write_fixtures(directory):
    directory.mkdir(parents=True, exist_ok=True)
    expected = {}
    for variant, filename in enumerate(FILES):
        rgb = fixture_pixels(variant)
        expected[filename] = rgb
        image = Image.fromarray(rgb, 'RGB')
        colors = PALETTE[variant:] + PALETTE[:variant]
        indices = np.zeros((HEIGHT, WIDTH), dtype=np.uint8)
        for idx, color in enumerate(colors):
            indices[np.all(rgb == color, axis=2)] = idx
        if filename == 'A8.BMP':
            stride8 = (WIDTH + 3) & ~3
            pixels8 = b''.join(indices[y].tobytes() + b'\0' * (stride8 - WIDTH)
                               for y in range(HEIGHT - 1, -1, -1))
            palette_bytes = b''.join(bytes((blue, green, red, 0)) for red, green, blue in colors)
            pixel_offset = 14 + 40 + len(palette_bytes)
            file_size = pixel_offset + len(pixels8)
            file_header = struct.pack('<2sIHHI', b'BM', file_size, 0, 0, pixel_offset)
            dib_header = struct.pack('<IiiHHIIiiII', 40, WIDTH, HEIGHT, 1, 8, 0,
                                     len(pixels8), 0, 0, len(colors), 0)
            (directory / filename).write_bytes(file_header + dib_header + palette_bytes + pixels8)
        elif filename == 'B24.BMP':
            image.save(directory / filename, format='BMP')
        elif filename == 'C32.BMP':
            # Microsoft's bitmap storage notes that a negative DIB height is
            # top-down: https://learn.microsoft.com/en-us/windows/win32/gdi/bitmap-storage.
            payload = bytearray()
            for row in rgb:
                for red, green, blue in row:
                    payload.extend((int(blue), int(green), int(red), 0))
            pixel_offset = 14 + 40
            file_size = pixel_offset + len(payload)
            header = struct.pack('<2sIHHI', b'BM', file_size, 0, 0, pixel_offset)
            dib = struct.pack('<IiiHHIIiiII', 40, WIDTH, -HEIGHT, 1, 32, 0,
                              len(payload), 0, 0, 0, 0)
            (directory / filename).write_bytes(header + dib + payload)
        elif filename == 'D.GIF':
            palette = Image.new('P', (WIDTH, HEIGHT))
            palette.putdata(indices.reshape(-1).tolist())
            palette.putpalette([v for color in colors for v in color] + [0] * (768 - 12))
            palette.save(directory / filename, format='GIF', optimize=False)
        elif filename == 'E.JPEG':
            image.save(directory / filename, format='JPEG', quality=96, subsampling=0)
        else:
            image.save(directory / filename, format='PNG', optimize=False)

    broken = bytearray((directory / 'B24.BMP').read_bytes())
    (directory / 'ZBAD.BMP').write_bytes(broken[:-4096])
    return expected


def open_from_files(vm, gate, letter, filename):
    gate.act('Files association ' + filename, ['type:' + letter.lower(), 'ret'],
             [f'[FILES] open C:\\QA\\VIEWER\\{filename}', '[DESKTOP] WINDOW 19 OPEN'], 20)


def compare_crop(frame, expected, origin, jpeg=False, scale=1, viewport=None):
    x, y = origin
    target = expected
    if scale != 1:
        target = np.repeat(np.repeat(expected, scale, axis=0), scale, axis=1)
    if viewport is None:
        viewport = (0, 0, frame.shape[1], frame.shape[0])
    left, top, right, bottom = viewport
    src_x0 = max(0, left - x)
    src_y0 = max(0, top - y)
    src_x1 = min(target.shape[1], frame.shape[1] - x, right - x)
    src_y1 = min(target.shape[0], frame.shape[0] - y, bottom - y)
    assert src_x0 < src_x1 and src_y0 < src_y1, (
        'rendered image does not intersect the visible Viewer viewport', origin, viewport)
    actual = frame[y + src_y0:y + src_y1, x + src_x0:x + src_x1].astype(np.int16)
    wanted = target[src_y0:src_y1, src_x0:src_x1].astype(np.int16)
    delta = np.abs(actual - wanted)
    if jpeg:
        record = {'mean_abs_channel_error': float(delta.mean()),
                  'p99_abs_channel_error': int(np.percentile(delta, 99)),
                  'compared_pixels': int((src_y1-src_y0) * (src_x1-src_x0))}
        assert record['mean_abs_channel_error'] <= 7 and record['p99_abs_channel_error'] <= 36, record
    else:
        bad = int((delta > 3).any(axis=2).sum())
        record = {'max_abs_channel_error': int(delta.max()), 'bad_pixels_over_3': bad,
                  'compared_pixels': int((src_y1-src_y0) * (src_x1-src_x0))}
        assert bad == 0, record
    record['viewport'] = list(viewport)
    record['compared_source_offset'] = [src_x0, src_y0]
    return record


def locate(frame, expected, jpeg=False, scale=1, viewport=None):
    """Find the coded top-left pattern marker, then validate the full visible crop."""
    if viewport is None:
        viewport = (0, 0, frame.shape[1], frame.shape[0])
    left, top, right, bottom = viewport
    target = expected
    if scale != 1:
        target = np.repeat(np.repeat(expected, scale, axis=0), scale, axis=1)
    sample_x, sample_y = 10 * scale, 10 * scale
    color = target[sample_y, sample_x].astype(np.int16)
    rgb_frame = frame.astype(np.int16)
    tolerance = 24 if jpeg else 3
    candidates = np.argwhere(np.max(np.abs(rgb_frame - color), axis=2) <= tolerance)
    points = ((8, 8), (12, 8), (16, 8), (20, 8), (8, 12), (12, 12),
              (16, 12), (20, 12), (8, 16), (12, 16), (16, 16), (20, 16),
              (8, 20), (12, 20), (16, 20), (20, 20))
    points += ((30, 30), (38, 30), (46, 30))
    # Anchor at adjacent coded-cell boundaries. The cells above are solid
    # 4-pixel source blocks, so samples inside them alone admit origins one
    # source pixel away; at zoom, that false origin can still match the marker
    # while shifting the full crop. These boundary samples remain visible in
    # the clipped, panned 4x viewport.
    points += ((11, 10), (12, 10), (9, 11), (9, 12))
    if scale == 1:
        points += ((174, 98), (176, 100))
    ys = candidates[:, 0].astype(np.int32) - sample_y
    xs = candidates[:, 1].astype(np.int32) - sample_x
    valid = ((xs + sample_x >= left) & (xs + sample_x < right) &
             (ys + sample_y >= top) & (ys + sample_y < bottom))
    xs, ys = xs[valid], ys[valid]
    checked = 0
    for sx, sy in points:
        sx *= scale
        sy *= scale
        visible = ((xs + sx >= left) & (xs + sx < right) &
                   (ys + sy >= top) & (ys + sy < bottom))
        if not np.any(visible):
            continue
        observed = frame[ys[visible] + sy, xs[visible] + sx].astype(np.int16)
        wanted = target[sy, sx].astype(np.int16)
        matches = np.max(np.abs(observed - wanted), axis=1) <= tolerance
        # An anchor outside the viewport cannot disqualify an origin. Keeping
        # invisible candidates matters when a zoomed image is panned and its
        # marker partly clips at the viewport edge.
        valid = ~visible
        valid[visible] = matches
        xs, ys = xs[valid], ys[valid]
        checked += 1
        if not len(xs):
            return None, None
    if checked < 8:
        return None, None
    for ox, oy in zip(xs, ys):
        origin = int(ox), int(oy)
        try:
            metrics = compare_crop(frame, expected, origin, jpeg, scale, viewport)
        except AssertionError:
            continue
        return origin, metrics
    return None, None


def viewer_viewport(vm, frame):
    """Measure this screenshot's active frame and derive webstore_image's clip."""
    paper = (246, 246, 242) if getattr(vm, 'palette', 'legacy') == 'platinum' else (247, 247, 239)
    title = (52, 72, 121) if getattr(vm, 'palette', 'legacy') == 'platinum' else (32, 53, 73)
    paper_mask = (np.max(np.abs(frame.astype(int) - paper), axis=2) <= 4)
    title_mask = (np.max(np.abs(frame.astype(int) - title), axis=2) <= 4)
    measured = None
    for row in range(32, frame.shape[0] - 35):
        active_x = np.flatnonzero(title_mask[row])
        if len(active_x) < 30 or int(active_x[-1] - active_x[0]) < 80 or row < 3:
            continue
        border_y = row - 3
        border = paper_mask[border_y]
        edges = np.diff(np.r_[False, border, False].astype(np.int8))
        starts, ends = np.flatnonzero(edges == 1), np.flatnonzero(edges == -1)
        spans = [(int(left), int(right)) for left, right in zip(starts, ends)
                 if right - left >= 300 and left <= active_x[-1] < right]
        if spans:
            left = int(active_x[0]) - 3
            right = max(end for _, end in spans)
            if left >= 0 and right - left >= 300:
                # The detected top-bevel run excludes the dark one-pixel outer
                # edge at its right end, so include that final frame column.
                measured = left, border_y, right - left + 1
                break
    assert measured is not None, 'could not locate Viewer frame from active title and top bevel'
    x, y, width = measured
    inner = frame[y + 180:, x:x + width].astype(int)
    dark_fraction = (np.max(inner, axis=2) < 80).mean(axis=1)
    bottom = None
    for offset in range(max(0, len(dark_fraction) - 1)):
        if dark_fraction[offset] >= .80 and dark_fraction[offset + 1] >= .80:
            bottom = y + 180 + offset
            break
    assert bottom is not None, ('could not measure Viewer bottom border', measured)
    height = bottom - y + 1
    assert y + height <= frame.shape[0], 'measured Viewer frame extends outside screenshot'
    # Production webstore_image clip: x+6, y+TITLE_H+38, w-12, h-TITLE_H-72.
    return x + 6, y + 68, x + width - 6, y + height - 34


def park_pointer(vm):
    """Keep the software cursor outside the captured image and pixel checks."""
    vm.position(2, 2)
    time.sleep(.05)


def wait_image(vm, expected, *, jpeg=False, scale=1, timeout=25, name='render'):
    park_pointer(vm)
    deadline = time.monotonic() + timeout
    last_path = None
    while time.monotonic() < deadline:
        last_path = vm.shot(name)
        frame = np.asarray(Image.open(last_path).convert('RGB'))
        viewport = viewer_viewport(vm, frame)
        origin, metrics = locate(frame, expected, jpeg=jpeg, scale=scale, viewport=viewport)
        if origin is not None:
            return frame, origin, metrics
        time.sleep(.25)
    raise AssertionError(f'{name}: rendered pixel pattern not found after {timeout}s; last={last_path}')


def wait_any_image(vm, fixtures, timeout=25, name='navigation', exclude=()):
    park_pointer(vm)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        path = vm.shot(name)
        frame = np.asarray(Image.open(path).convert('RGB'))
        viewport = viewer_viewport(vm, frame)
        for filename, (expected, jpeg) in fixtures.items():
            if filename in exclude:
                continue
            origin, metrics = locate(frame, expected, jpeg=jpeg, viewport=viewport)
            if origin is not None:
                return filename, frame, origin, metrics
        time.sleep(.25)
    raise AssertionError(f'{name}: no known image was rendered')


def click_viewer_button(vm, button):
    # The Viewer toolbar buttons start at client x+10 and y+TITLE_H+6.
    x0, y0, _ = vm.active_rect()
    offset = vm.offset()
    x = x0 + {1: 37, 3: 90, 4: 138}[button]
    vm.completed_control_click(x, y0 + 48)
    vm.wait('[DESKTOP] PAINT', offset, 15)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    disk = output / 'disk.img'
    source_hash = sha256_file(args.image)
    shutil.copyfile(args.image, disk)
    volume = f'{disk}@@{FAT16(disk).start}'
    subprocess.run(['mmd', '-i', volume, '::QA', '::QA/VIEWER'], check=True)
    fixture_dir = output / 'fixtures'
    expected = write_fixtures(fixture_dir)
    for filename in (*FILES, 'ZBAD.BMP'):
        subprocess.run(['mcopy', '-o', '-i', volume, str(fixture_dir / filename),
                        '::QA/VIEWER/' + filename], check=True)
    fixture_map = {name: (expected[name], name == 'E.JPEG') for name in FILES}
    report = {'status': 'running', 'physical_hardware_qualified': False,
              'scope': 'QEMU screenshots of actual Files association and Viewer cache presentation',
              'mode_expected': [1024, 768], 'fixtures': {}, 'steps': [],
              'source_sha256': source_hash}
    for filename in (*FILES, 'ZBAD.BMP'):
        report['fixtures'][filename] = hashlib.sha256((fixture_dir / filename).read_bytes()).hexdigest()

    vm = WindowVM(disk, output, 'std', memory=128, palette='platinum')
    vm.auto_enter_dos = False
    gate = Gate(vm)
    try:
        vm.ready()
        frame = np.asarray(Image.open(vm.shot('desktop')).convert('RGB'))
        assert frame.shape[:2] == (768, 1024), f'expected 1024x768 default mode, got {frame.shape[1::-1]}'
        report['mode_actual'] = [frame.shape[1], frame.shape[0]]
        gate.act('Open fixture directory in Files', ['meta_l-r', 'type:explorer c:\\qa\\viewer', 'ret'],
                 ['[FILES] list C:\\QA\\VIEWER 7', '[FILES] geometry'], 30)
        report['steps'].append('Opened fixture directory in Files')

        for letter, filename in zip('abcdef', FILES):
            open_from_files(vm, gate, letter, filename)
            current_expected = expected[filename]
            _, origin, metrics = wait_image(vm, current_expected, jpeg=filename == 'E.JPEG', name='render-' + filename.replace('.', '-'))
            report['fixtures'][filename] = {'sha256': report['fixtures'][filename],
                                            'files_association': True,
                                            'render_origin': list(origin), **metrics}
            report['steps'].append('Files opened and rendered ' + filename)

            if filename == 'A8.BMP':
                vm.key('1')
                _, one_origin, one_metrics = wait_image(vm, current_expected, name='viewer-100-percent')
                assert one_origin == origin, ('100% changed native-size image position', origin, one_origin)
                # HMP's positive dz is delivered as negative EV_WHEEL by the
                # DOS mouse driver (verified by the Viewer event markers).
                vm.position(origin[0] + 70, origin[1] + 70)
                for _ in range(3):
                    # QEMU HMP injects wheel movement through mouse_move's dz.
                    # See https://www.qemu.org/docs/master/system/monitor.html.
                    vm.hmp('mouse_move 0 0 1')
                    time.sleep(.2)
                frame, zoom_origin, zoom_metrics = wait_image(vm, current_expected, scale=4, timeout=10,
                                                              name='viewer-zoom-4x')
                report['zoom_4x'] = {'origin': list(zoom_origin), **zoom_metrics}
                vm.key('right')
                vm.key('down')
                frame, panned_origin, pan_metrics = wait_image(vm, current_expected, scale=4, timeout=10,
                                                                name='viewer-arrow-pan')
                assert panned_origin == (zoom_origin[0] - 32, zoom_origin[1] - 32), (
                    'arrow pan displacement', zoom_origin, panned_origin)
                # Drag inside the measured visible image/viewport intersection.
                viewport = viewer_viewport(vm, frame)
                image_right = panned_origin[0] + WIDTH * 4
                image_bottom = panned_origin[1] + HEIGHT * 4
                drag_left = max(viewport[0], panned_origin[0])
                drag_top = max(viewport[1], panned_origin[1])
                drag_right = min(viewport[2], image_right)
                drag_bottom = min(viewport[3], image_bottom)
                assert drag_right - drag_left > 24 and drag_bottom > drag_top, (
                    'panned image has no room for an in-viewport drag', viewport, panned_origin)
                drag_x = (drag_left + drag_right) // 2
                drag_y = (drag_top + drag_bottom) // 2
                vm.position(drag_x, drag_y)
                vm.hmp('mouse_button 1'); time.sleep(.2)
                vm.position(drag_x - 12, drag_y)
                time.sleep(.25); vm.hmp('mouse_button 0'); time.sleep(.3)
                _, drag_origin, drag_metrics = wait_image(vm, current_expected, scale=4, timeout=10,
                                                           name='viewer-mouse-pan')
                expected_drag = (panned_origin[0] - 12, panned_origin[1])
                assert max(abs(drag_origin[i] - expected_drag[i]) for i in range(2)) <= 4, (
                    'mouse pan displacement', expected_drag, drag_origin)
                vm.key('f')
                _, fit_origin, fit_metrics = wait_image(vm, current_expected, name='viewer-fit-restored')
                assert fit_origin == origin, ('Fit did not restore original placement', origin, fit_origin)
                report['fit_100_zoom_pan'] = {
                    '100_percent_origin': list(one_origin),
                    'zoom_4x_origin': list(zoom_origin),
                    'arrow_pan_origin': list(panned_origin),
                    'mouse_pan_origin': list(drag_origin),
                    'fit_restored_origin': list(fit_origin),
                    'checks': 'native pixels, 4x nearest-neighbour pixels, arrow and drag offsets'}

                click_viewer_button(vm, 4)
                next_name, _, _, next_metrics = wait_any_image(vm, fixture_map, name='viewer-next',
                                                                exclude=(filename,))
                assert next_name != filename, 'Next did not advance to another sibling image'
                click_viewer_button(vm, 3)
                prev_name, _, _, prev_metrics = wait_any_image(vm, fixture_map, name='viewer-prev',
                                                                exclude=(next_name,))
                assert prev_name == filename, ('Prev did not return to the original image', filename, prev_name)
                report['next_previous'] = {'start': filename, 'next': next_name, 'previous': prev_name,
                                           'next_pixels': next_metrics, 'previous_pixels': prev_metrics}

            if filename == 'E.JPEG':
                # The JPEG name is a long extension in VFAT; Next/Prev must
                # still include it in the same sibling cycle.
                click_viewer_button(vm, 4)
                next_name, _, _, _ = wait_any_image(vm, fixture_map, name='jpeg-next',
                                                     exclude=(filename,))
                click_viewer_button(vm, 3)
                prev_name, _, _, _ = wait_any_image(vm, fixture_map, name='jpeg-prev',
                                                     exclude=(next_name,))
                assert prev_name == filename, ('JPEG sibling was omitted from navigation', filename, prev_name)
                report['jpeg_navigation'] = {'next': next_name, 'previous': prev_name}

            offset = vm.offset()
            vm.key('alt-f4')
            vm.wait('[DESKTOP] WINDOW 19 CLOSE', offset, 20)
            report['steps'].append('Closed Viewer after ' + filename)

        open_from_files(vm, gate, 'z', 'ZBAD.BMP')
        time.sleep(1.0)
        bad_frame = np.asarray(Image.open(vm.shot('truncated-bmp-error')).convert('RGB'))
        bad_origin, _ = locate(bad_frame, expected['A8.BMP'])
        assert bad_origin is None, 'truncated BMP rendered image pixels instead of entering error state'
        report['truncated_bmp_rejected'] = True
        report['steps'].append('Truncated BMP was rejected before image presentation')
        offset = vm.offset(); vm.key('alt-f4'); vm.wait('[DESKTOP] WINDOW 19 CLOSE', offset, 20)

        # Re-open after close, proving decoder and cache cleanup permit a fresh session.
        open_from_files(vm, gate, 'f', 'F.PNG')
        _, reopen_origin, reopen_metrics = wait_image(vm, expected['F.PNG'], name='viewer-reopened')
        report['close_reopen'] = {'reopen_origin': list(reopen_origin), **reopen_metrics}
        report['steps'].append('Reopened PNG after Viewer close')
        report['status'] = 'passed'
    except Exception as error:
        report['status'] = 'failed'
        report['error'] = repr(error)
        try:
            vm.shot('failure')
        except Exception:
            pass
        raise
    finally:
        vm.close()
        report['source_unchanged'] = sha256_file(args.image) == source_hash
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        assert report['source_unchanged'], 'source image changed during the test harness run'
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    raise SystemExit(main())
