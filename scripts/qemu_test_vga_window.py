#!/usr/bin/env python3
"""Monitored VGA guest inside the native desktop's DOS window (QEMU, Pentium III, 128 MiB).

The unchanged-semantics fixture VGASEMW.COM runs first on QEMU's VGA (full
screen reference), then as a CVSESSION guest launched from the desktop Run
field. Every guest VGA cycle reaches the software model; the native compositor
paints the window through the DOS-window runtime's band painter, which asks the
JLM to render the model into the compositor band. The compositor's own banked
drawing is served by CVSESSION host mode from the protected LFB mapping.

Checked: five displayed checkpoints equal to the native display (presenter
nearest-neighbour mapping, 6-bit DAC), keyboard focus (keys typed while
another window is active never reach the guest), redraw after a native
window is dragged over and away, minimize/restore, identical result hashes,
measured paint times, exact vector restore, session END, module and Jemm
unload and desktop return. Only QEMU keyboard/mouse events drive the guest;
memory is read, never written. Emulator results do not establish physical
T23/E500 behaviour or hardware acceleration.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import time

import numpy as np
from PIL import Image

from qemu_test_installed_hdd import FAT16
from qemu_test_native_windows import WindowVM
from qemu_test_dos_window import Session

ROOT = Path(__file__).resolve().parents[1]
CLIENT = (640, 400)
REPEAT = {1: 2, 2: 2, 3: 1, 4: 2, 5: 1}
KEYS = {' ': 'spc', '.': 'dot', '\\': 'backslash', '-': 'minus', '/': 'slash',
        ':': 'shift-semicolon', '=': 'equal'}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def rgb(path):
    return np.array(Image.open(path).convert('RGB'), dtype=np.uint8)


def expected(native, repeat):
    width, height = CLIENT
    rows, columns = native.shape[:2]
    xs = (np.arange(width) * columns) // width
    ys = ((np.arange(height) * rows * repeat) // height) // repeat
    return native[ys][:, xs]


def compare(frame, native, origin, repeat):
    x, y = origin
    width, height = CLIENT
    window = frame[y:y + height, x:x + width]
    diff = np.any((window >> 2) != (expected(native, repeat) >> 2), axis=2)
    return dict(native_size=list(native.shape[1::-1]), origin=list(origin),
                mismatched_pixels=int(np.count_nonzero(diff)), compared_pixels=int(diff.size))


class Harness:
    def __init__(self, vm, output):
        self.vm = vm
        self.output = output

    def typed(self, text):
        for ch in text:
            self.vm.key('shift-' + ch.lower() if ch.isupper() else KEYS.get(ch, ch))
        self.vm.key('ret')

    def console(self, line, *expected_text, timeout=60):
        offset = self.vm.offset()
        self.typed(line)
        self.vm.wait('CiukiOS SHELL C:\\APPS>', offset, timeout)
        body = self.serial(offset)
        for text in expected_text:
            assert text in body, (line, text, body)
        return body

    def serial(self, offset):
        return subprocess.check_output(['scripts/serial_log_normalize.py', '--offset', str(offset),
                                        str(self.vm.serial)]).decode('cp437', 'replace')

    def low(self, name):
        path = self.output / (name + '.bin')
        self.vm.hmp(f'pmemsave 0 0x110000 "{path}"')
        deadline = time.monotonic() + 5
        while not path.exists() or path.stat().st_size < 0x110000:
            assert time.monotonic() < deadline
            time.sleep(.02)
        return path.read_bytes()

    def checkpoint(self, k, timeout=60, name='cp'):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            data = self.low(name)
            if any(data[m.start() + 8] == k for m in re.finditer(b'VGASEMOB', data)):
                return
            time.sleep(.2)
        raise AssertionError(f'fixture checkpoint {k} not reached')

    def current_checkpoint(self):
        data = self.low('cp-now')
        values = [data[m.start() + 8] for m in re.finditer(b'VGASEMOB', data)]
        return max(values) if values else 0

    def shot(self, name):
        path = self.vm.shot(name)
        png = path.with_suffix('.png')
        Image.open(path).save(png)
        return png


def settled_frame(ui, h, name):
    """Capture only after the window's damage and paints are quiescent. The
    desktop pointer is parked outside the guest area first: the compositor
    rightly draws it over the window, which is not guest content."""
    h.vm.position(20, 700)
    time.sleep(1.2)
    for attempt in range(40):
        data, state = ui.module()
        dirty = struct.unpack_from('<H', data, 20)[0]
        if data[35] or struct.unpack_from('<I', data, dirty)[0] or ui.b('ui_comp_dirty'):
            time.sleep(.1)
            continue
        paints = struct.unpack_from('<I', data, 80)[0]
        png = h.shot(name)
        after, _ = ui.module()
        if struct.unpack_from('<I', after, 80)[0] == paints and not after[35]:
            return rgb(png), dict(capture_attempts=attempt + 1, paint_count=paints)
        time.sleep(.1)
    raise AssertionError(name + ': the window never became quiescent')


def launch(ui, vm, command, event):
    """Type into the desktop Run field at a bounded rate, verify, launch."""
    ring = re.findall(r'0x[0-9a-f]{4}\b', vm.hmp('xp /2hx 0x41a').decode('utf-8', errors='replace'))[-2:]
    event('BIOS keyboard ring before Run', head_tail=ring)
    vm.key('f3')
    ui.until(lambda: ui.b('ui_active_window') == 1, 'Run did not open')
    for attempt in range(3):
        length = ui.b('ui_run_len')
        vm.key('home')
        for _ in range(length):
            vm.key('delete')
            time.sleep(.1)
        for ch in command:
            ui.type_only(ch)
            time.sleep(.15)
        time.sleep(1)
        ui.refresh()
        if ui.ram[ui.offset('ui_run_text'):ui.offset('ui_run_text') + ui.b('ui_run_len')].decode() == command:
            break
    else:
        got = ui.ram[ui.offset('ui_run_text'):ui.offset('ui_run_text') + ui.b('ui_run_len')]
        raise AssertionError(f'Run field did not receive the command: {got!r}')
    event('Run field verified', command=command, attempts=attempt + 1)
    ui.click(40, 1)                  # Run: a DOS window whenever the desktop can host one
    ui.until(lambda: ui.b('dos_host_active') == 1 and ui.w('dw_segment') and ui.module()[1]['live'],
             'DOS window guest did not start', 60)


def metrics(data):
    """DOS-window header counters (dos_window_abi.inc) converted with the TSC
    rate the runtime measured at install (DW_TSC_KHZ)."""
    tsc_khz = struct.unpack_from('<I', data, 140)[0]
    callbacks = struct.unpack_from('<I', data, 52)[0]
    paints = struct.unpack_from('<I', data, 80)[0]
    callback_tsc, callback_max, paint_tsc, paint_max = struct.unpack_from('<4Q', data, 84)
    ms = lambda cycles: round(cycles / tsc_khz, 3) if tsc_khz else None
    return dict(tsc_khz=tsc_khz, host_callbacks=callbacks, compositor_paints=paints,
                mean_callback_ms=ms(callback_tsc / callbacks) if callbacks else None,
                max_callback_ms=ms(callback_max), mean_paint_ms=ms(paint_tsc / paints) if paints else None,
                max_paint_ms=ms(paint_max))


def window_size(ui):
    ui.refresh()
    return ui.w('ui_window_width', 11), ui.w('ui_window_height', 11)


def resize_window(ui, vm, dx, dy):
    """Drag the DOS window's grip (bottom-right, VGA session only) by dx/dy.
    Positions come from the geometry tables: a continuously repainting guest
    leaves no idle boundary for reading the hit table."""
    x, y = ui.geometry(11)
    w, hgt = window_size(ui)
    vm.position(x + w - 6, y + hgt - 6)
    vm.hmp('mouse_button 1'); time.sleep(.2)
    vm.position(x + w - 6 + dx, y + hgt - 6 + dy); time.sleep(.3)
    vm.hmp('mouse_button 0'); time.sleep(.3)
    size = window_size(ui)
    assert abs(size[0] - w - dx) <= 6 and abs(size[1] - hgt - dy) <= 6, ('resize', (w, hgt), (dx, dy), size)
    ui.until(lambda: struct.unpack_from('<2H', ui.module()[0], 210) == (size[0] - 20, size[1] - 60),
             'runtime did not receive the new client size')
    assert ui.geometry(11) == [x, y], 'resizing moved the window'
    return size


ACTIVE_TITLE = (52, 73, 121)          # focused title bar, default theme
FIRE_PALETTE = {(i // 2, (i * 22) // 128, i // 2 if i < 8 else 0) for i in range(128)}


def fire_profile(client):
    """Mean brightness of each client row: FIRE is black at the top and
    burns from the bottom, so the profile follows vertical scaling."""
    return client.max(axis=2).mean(axis=1)


def fire_client(ui, h, name, size, reference):
    """FIRE scaled into the current client: palette-only content, the
    default-size vertical profile resampled to the new height, and the lower
    half lit across the whole new width."""
    # FIRE is a screen saver: guest mouse motion ends it. Leave the grip
    # straight down, below the frame, never crossing the client.
    x, y = ui.geometry(11)
    below = min(y + size[1] + 12, ui.w('ui_height') - 40)
    h.vm.position(x + size[0] - 6, below)
    h.vm.position(20, below)
    time.sleep(1)
    frame = rgb(h.shot(name))
    x, y = ui.geometry(11)
    width, height = size[0] - 20, size[1] - 60
    client = frame[y + 34:y + 34 + height, x + 10:x + 10 + width] >> 2
    colours = {tuple(int(v) for v in c) for c in np.unique(client.reshape(-1, 3), axis=0)}
    expected = np.interp(np.linspace(0, len(reference) - 1, height), np.arange(len(reference)), reference)
    correlation = float(np.corrcoef(fire_profile(client), expected)[0, 1])
    lower = client[height // 2:height - height // 20].max(axis=2).mean(axis=0)
    covered = float(np.mean(lower > 2))
    result = dict(size=list(size), client=[width, height], distinct_colours=len(colours),
                  foreign_colours=sorted(colours - FIRE_PALETTE)[:8],
                  profile_correlation=round(correlation, 3), lower_columns_lit=round(covered, 3))
    assert len(colours) > 20 and len(colours - FIRE_PALETTE) <= 1, result
    assert correlation > .9, ('the guest was not rescaled to the new client height', result)
    assert covered > .95, ('the guest does not span the new client width', result)
    return frame, result


def fire_resize(ui, vm, h, event, reference):
    """Resize the live FIRE window through its grip: shrink, then grow past
    the original size; the guest is rescaled into each client and vacated
    desktop is repainted. Finally restore approximately 660x460."""
    original = window_size(ui)
    x, y = ui.geometry(11)
    checks = []
    small = resize_window(ui, vm, -160, -100)
    frame, result = fire_client(ui, h, 'fire-resized-small', small, reference)
    strip = frame[y + 40:y + small[1] - 30, x + small[0] + 12:x + original[0] - 4] >> 2
    stale = sum(int(np.all(strip == np.array(c), axis=2).sum()) for c in FIRE_PALETTE if max(c) > 8)
    result['vacated_fire_pixels'] = stale
    assert stale == 0, ('vacated desktop still shows the old guest image', result)
    checks.append(result)
    room_x = ui.w('ui_width') - 8 - x - original[0]
    room_y = ui.w('ui_height') - 37 - y - original[1]
    grow = (min(100, room_x - 10), min(60, room_y - 10))
    if grow[0] > 20 and grow[1] > 20:
        large = resize_window(ui, vm, original[0] + grow[0] - small[0], original[1] + grow[1] - small[1])
        checks.append(fire_client(ui, h, 'fire-resized-large', large, reference)[1])
    else:
        checks.append(dict(skipped_grow=True, room=[room_x, room_y]))
    current = window_size(ui)
    restored = resize_window(ui, vm, original[0] - current[0], original[1] - current[1])
    event('DOS window resized through its grip; FIRE rescaled into each client',
          original=list(original), checks=checks, restored=list(restored))
    return dict(original=list(original), checks=checks, restored=list(restored))


def fire_case(ui, vm, h, event, close_point, close_geometry):
    """Original DOS Navigator FIRE (continuous mode-13h animation) in the
    window; closed with the window's close button (cooperative Esc)."""
    launch(ui, vm, 'run \\APPS\\DOSNAV\\SSAVERS\\FIRE.EXE', event)
    time.sleep(3)
    first, _ = ui.module()
    t0 = time.monotonic()
    time.sleep(8)
    second, _ = ui.module()
    t1 = time.monotonic()
    h.vm.position(20, 700)
    time.sleep(.5)
    frame = rgb(h.shot('fire-in-window'))
    origin = ui.geometry(11)
    x, y = origin[0] + 10, origin[1] + 34
    window = frame[y:y + CLIENT[1], x:x + CLIENT[0]] >> 2
    colours = {tuple(int(v) for v in c) for c in np.unique(window.reshape(-1, 3), axis=0)}
    fire = {(i // 2, (i * 22) // 128, i // 2 if i < 8 else 0) for i in range(128)}
    foreign = colours - fire
    seconds = t1 - t0
    count = lambda d, o: struct.unpack_from('<I', d, o)[0]
    paints = count(second, 80) - count(first, 80)
    paint_tsc = struct.unpack_from('<Q', second, 100)[0] - struct.unpack_from('<Q', first, 100)[0]
    tsc_khz = count(second, 140)
    result = dict(measured_seconds=round(seconds, 2),
                  compositor_paints_per_second=round(paints / seconds, 2),
                  mean_paint_ms=round(paint_tsc / paints / tsc_khz, 3) if paints and tsc_khz else None,
                  host_callbacks_per_second=round((count(second, 52) - count(first, 52)) / seconds, 2),
                  distinct_window_colours=len(colours), colours_outside_fire_palette=sorted(foreign)[:8])
    assert paints > 0, 'the fire animation was never presented'
    assert len(colours) > 20 and len(foreign) <= 1, result   # a torn capture pixel at most
    # The launch scene must be on the visible page: frame and title present.
    title = tuple(int(v) for v in frame[origin[1] + 15, origin[0] + 100])
    result['title_pixel'] = list(title)
    assert title == ACTIVE_TITLE, ('DOS window frame missing from the visible page', title)
    result['resize'] = fire_resize(ui, vm, h, event, fire_profile(window))
    assert ui.geometry(11) == close_geometry, 'FIRE window geometry differs'
    x, y = close_geometry
    vm.position(x + window_size(ui)[0] - 18, y + 15)           # close: cooperative Esc
    vm.hmp('mouse_button 1'); time.sleep(.18)
    vm.hmp('mouse_button 0'); time.sleep(.25)
    ui.until(lambda: ui.b('dos_host_active') == 0, 'FIRE did not exit on window close', 30)
    ui.until(lambda: ui.b('ui_window_flags', 11) == 0 or ui.b('ui_window_flags', 11) == 1, 'window state', 10)
    if ui.b('ui_window_flags', 11):
        ui.click(18, 11)
    ui.until(lambda: ui.w('dw_segment') == 0, 'FIRE window memory not released', 30)
    event('original FIRE animated in the DOS window and closed through the window', **result)
    return result


def guestio_case(ui, vm, h, event, close_point, close_geometry):
    """Guest devices in the window: raw INT 9 keyboard with focus routing,
    virtual INT 33h mouse (polling and event handler) and SB16/OPL sound."""
    offset = vm.offset()
    launch(ui, vm, 'run GUESTIO.COM', event)
    vm.wait('[GUESTIO] READY MOUSE', offset, 60)
    result = {}

    def body():
        return h.serial(offset)

    def keys():
        return re.findall(r'\[GUESTIO\] KEY ([0-9A-F]{2})', body())

    def wait_for(predicate, description, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(.2)
        raise AssertionError(description)
    def unfocus():
        # Click an exposed part of another native window's body (action 70).
        ox, oy = ui.geometry(11)
        dos = (ox, oy, ox + 664, oy + 465)
        for x, y, right, bottom, encoded in reversed(ui.hit_records()):
            if encoded & 255 != 70 or (encoded >> 8) - 1 == 11:
                continue
            for px in (right - 30, x + 30, (x + right) // 2):
                for py in ((y + bottom) // 2, y + 40, bottom - 40):
                    inside_dos = dos[0] <= px < dos[2] and dos[1] <= py < dos[3]
                    if x < px < right and y < py < bottom and not inside_dos:
                        vm.position(px, py)
                        vm.hmp('mouse_button 1'); time.sleep(.18)
                        vm.hmp('mouse_button 0'); time.sleep(.3)
                        ui.until(lambda: ui.b('ui_active_window') != 11,
                                 'focus did not leave the DOS window')
                        # DOSWIN hands the new focus to the device model at
                        # its next host tick; keys before that are still the
                        # guest's, as on a real keyboard queue.
                        ui.until(lambda: not ui.module()[0][36], 'runtime kept guest focus')
                        time.sleep(.2)
                        return
        raise AssertionError('no exposed native window body to take focus')
    data, _ = ui.module()
    result['device_begin'] = dict(devices=struct.unpack_from('<H', data, 214)[0], audio=data[216],
                                  result=struct.unpack_from('<H', data, 226)[0])
    assert result['device_begin']['devices'] == 0x1b, result['device_begin']
    ready = re.search(r'READY MOUSE ([0-9A-F]{4}) ([0-9A-F]{4})', body())
    result['int33_reset'] = ready.groups()
    assert ready.groups() == ('FFFF', '0002'), 'virtual INT 33h mouse not reported present'
    assert ui.b('ui_active_window') == 11, 'launched window is not focused'
    time.sleep(.5)
    before = len(keys())
    vm.key('a'); time.sleep(.3); vm.key('b')
    wait_for(lambda: len(keys()) >= before + 4, 'focused raw keys did not arrive')
    result['focused_keys'] = keys()[before:]
    assert result['focused_keys'][:4] == ['1E', '9E', '30', 'B0'], result['focused_keys']
    unfocus()
    before = len(keys())
    vm.key('c'); time.sleep(1.5)
    result['unfocused_keys'] = keys()[before:]
    assert result['unfocused_keys'] == [], 'an unfocused key reached the guest'
    ui.click(211)
    ui.until(lambda: ui.b('ui_active_window') == 11, 'DOS window did not regain focus')
    ui.until(lambda: ui.module()[0][36], 'runtime did not restore guest focus')
    time.sleep(.5)
    before = len(keys())
    vm.hmp('sendkey d 3000')
    wait_for(lambda: '20' in keys()[before:], 'held key make did not arrive')
    unfocus()                                        # focus lost while D is held
    wait_for(lambda: 'A0' in keys()[before:], 'focus loss did not release the held key', 5)
    time.sleep(3)
    result['focus_loss_keys'] = keys()[before:]
    assert result['focus_loss_keys'] == ['20', 'A0'], result['focus_loss_keys']
    ui.click(211)
    ui.until(lambda: ui.b('ui_active_window') == 11, 'DOS window did not regain focus')
    # Mouse: pointer at the client centre is guest (320, 100) in mode 13h.
    origin = ui.geometry(11)
    centre = (origin[0] + 10 + 320, origin[1] + 34 + 200)
    vm.position(*centre)
    time.sleep(1)

    def mouse():
        found = re.findall(r'\[GUESTIO\] MOUSE ([0-9A-F]{4}) ([0-9A-F]{4}) ([0-9A-F]{4})', body())
        return [tuple(int(v, 16) for v in item) for item in found]
    wait_for(lambda: mouse() and abs(mouse()[-1][0] - 320) <= 8 and abs(mouse()[-1][1] - 100) <= 4,
             f'guest pointer not at the client centre: {mouse()[-3:]}')
    result['pointer_at_centre'] = mouse()[-1]
    vm.hmp('mouse_button 1'); time.sleep(.6)
    wait_for(lambda: mouse()[-1][2] == 1, 'left button not reported to the guest')
    vm.hmp('mouse_button 0'); time.sleep(.6)
    wait_for(lambda: mouse()[-1][2] == 0, 'left release not reported to the guest')
    events = re.findall(r'EVENTS ([0-9A-F]{4}) ([0-9A-F]{4})', body())
    result['handler_events'] = events[-1] if events else None
    assert events and int(events[-1][1], 16) & 0x07 == 0x07, 'event handler missed move/press/release'
    # A resized window maps the same guest range onto its new client: the
    # client's centre and bottom-right corner stay guest (320,100)/(639,199).
    size = resize_window(ui, vm, -160, -100)
    client = (size[0] - 20, size[1] - 60)
    vm.position(origin[0] + 10 + client[0] // 2, origin[1] + 34 + client[1] // 2)
    wait_for(lambda: abs(mouse()[-1][0] - 320) <= 10 and abs(mouse()[-1][1] - 100) <= 5,
             f'resized client centre is not the guest centre: {mouse()[-3:]}')
    result['resized_client'] = list(client)
    result['resized_centre'] = mouse()[-1]
    vm.position(origin[0] + 10 + client[0] - 6, origin[1] + 34 + client[1] - 6)
    wait_for(lambda: mouse()[-1][0] >= 620 and mouse()[-1][1] >= 192,
             f'resized client corner is not the guest corner: {mouse()[-3:]}')
    result['resized_corner'] = mouse()[-1]
    # Sound: 'P' plays six SB DMA blocks (IRQ7) and an OPL note.
    vm.key('p')
    vm.wait('[GUESTIO] OPL DONE', offset, 60)
    sb = re.search(r'SB DONE IRQS ([0-9A-F]{4})', body())
    result['sb_irqs'] = int(sb.group(1), 16)
    assert result['sb_irqs'] == 6, 'SB DMA blocks did not complete in the window'
    data, _ = ui.module()
    result['devices'] = struct.unpack_from('<H', data, 214)[0]
    result['audio'] = data[216]
    result['mouse_events'] = struct.unpack_from('<I', data, 224)[0]
    vm.key('esc')
    vm.wait('[GUESTIO] EXIT', offset, 30)
    ui.until(lambda: ui.b('dos_host_active') == 0, 'GUESTIO did not exit', 30)
    assert ui.geometry(11) == close_geometry
    vm.position(*close_point)
    vm.hmp('mouse_button 1'); time.sleep(.18)
    vm.hmp('mouse_button 0'); time.sleep(.25)
    ui.until(lambda: ui.w('dw_segment') == 0, 'GUESTIO window memory not released', 30)
    event('guest devices in the window: raw keyboard focus, virtual mouse, SB16/OPL', **result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--shell', type=Path, required=True, help='SHELL.COM with the V86 DOS-window host path')
    parser.add_argument('--listing', type=Path, required=True, help='NASM listing of that SHELL.COM')
    parser.add_argument('--runtime', type=Path, required=True, help='DOSWIN.DRV with the VGA session mode')
    parser.add_argument('--jemm', type=Path, required=True)
    parser.add_argument('--jload', type=Path, required=True)
    parser.add_argument('--module', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    before = sha(args.image)
    disk = output / 'disk.img'
    shutil.copyfile(args.image, disk)
    volume = f'{disk}@@{FAT16(disk).start}'
    fixture = output / 'VGASEMW.COM'
    subprocess.run(['nasm', '-f', 'bin', '-DHOLD_ALWAYS', str(ROOT / 'src/probes/vm/vga_semantics.asm'),
                    '-o', str(fixture)], check=True)
    files = [(args.kernel, 'SYSTEM/CIUKIDOS.SYS'), (args.shell, 'SYSTEM/SHELL.COM'),
             (args.runtime, 'SYSTEM/DOSWIN.DRV'), (args.jemm, 'JEMM386.EXE'), (args.jload, 'JLOAD.EXE'),
             (args.module, 'CVSESS.DLL'), (fixture, 'APPS/VGASEMW.COM')]
    # CiukiOS EXEC accepts only .COM/.EXE/.APP/.PRG names: the unchanged
    # DOS Navigator FIRE.SS bytes are placed beside it as FIRE.EXE.
    fire = output / 'FIRE.EXE'
    fire.write_bytes(FAT16(disk).read('APPS/DOSNAV/SSAVERS/FIRE.SS'))
    files.append((fire, 'APPS/DOSNAV/SSAVERS/FIRE.EXE'))
    guestio = output / 'GUESTIO.COM'
    subprocess.run(['nasm', '-f', 'bin', str(ROOT / 'src/probes/vm/guest_io.asm'), '-o', str(guestio)],
                   check=True)
    files.append((guestio, 'APPS/GUESTIO.COM'))
    for path, target in files:
        subprocess.run(['mcopy', '-o', '-i', volume, str(path), '::' + target], check=True)
    record = dict(passed=False, cpu='pentium3', memory_mib=128, accelerator='kvm',
                  source_image_sha256=before, inputs={t: sha(p) for p, t in files},
                  physical_hardware_qualified=False, hardware_acceleration_claimed=False,
                  guest_memory_writes=False, events=[])
    # AC'97 + WAV capture: the guest SB/OPL output is streamed to it.
    vm = WindowVM(disk, output, 'std', boot_capture=True, memory=128, palette='platinum')
    record['qemu_command'] = vm.process.args
    h = Harness(vm, output)
    started = time.monotonic()

    def event(name, **details):
        record['events'].append(dict(name=name, seconds=round(time.monotonic() - started, 2), **details))
        print('[vga-window] ' + name, flush=True)

    try:
        vm.ready()
        vm.key('f4')
        vm.wait('CiukiOS SHELL C:\\APPS>')
        # Native reference: the same fixture full screen on QEMU's VGA.
        offset = vm.offset()
        h.typed('run VGASEMW.COM \\VGANAT.OUT')
        native = {}
        for k in range(1, 6):
            h.checkpoint(k, name=f'native-cp{k}')
            time.sleep(.5)
            native[k] = rgb(h.shot(f'native-checkpoint-{k}'))
            vm.key('spc')
        vm.wait('[VGASEM] results written', offset, 60)
        vm.wait('CiukiOS SHELL C:\\APPS>', offset, 60)
        event('native reference captured')
        h.console('run \\JEMM386.EXE LOAD NOEMS X=A000-FFFF NODYN MAX=32M MIN=32M NOVME', 'Jemm386 loaded')
        h.console('run \\JLOAD.EXE \\CVSESS.DLL', 'loaded successfully')
        offset = vm.offset()
        h.typed('exit')
        vm.ready(offset)
        shell = FAT16(disk).read('SYSTEM/SHELL.COM')
        ui = Session(vm, shell, args.listing, dict(events=record['events'], cases=[]))
        assert ui.b('ui_vbe') == 1 and ui.b('vc_lfb') == 2, (
            'V86 desktop must complete a protected-LFB mode set and 4F03 D14 readback')
        assert ui.b('vc_lfb_ok') == 1 and ui.b('vc_session_bound') == 1, (
            'protected LFB needs validated CVSESSION row/fill capability and framebuffer binding')
        assert ui.b('vc_bank_ok') == 1, 'validated banked fallback descriptor should remain available'
        event('desktop uses protected LFB after CVSESSION capability, binding and VBE readback checks')
        vectors_before = ui.vectors()
        # Launch from the real Run field. Typing is rate limited because the
        # V86 UI keyboard ring can overflow under rapid synthetic input.
        launch(ui, vm, 'run VGASEMW.COM \\VGAWIN.OUT', event)
        data, state = ui.module()
        assert state['installed'] and data[192] == 1, ('VGA session mode not installed', state)
        event('guest launched in the DOS window', segment=hex(state['segment']))
        checks = {}
        for k in range(1, 6):
            h.checkpoint(k, name=f'window-cp{k}')
            origin = ui.geometry(11)
            client = (origin[0] + 10, origin[1] + 34)
            frame, capture = settled_frame(ui, h, f'window-checkpoint-{k}')
            checks[k] = dict(compare(frame, native[k], client, REPEAT[k]), **capture)
            assert checks[k]['mismatched_pixels'] == 0, (k, checks[k])
            if k == 1:
                # Focus: activate another native window, type, nothing reaches the guest.
                ui.click(60, 0)                    # title of the Application Library window
                ui.until(lambda: ui.b('ui_active_window') != 11, 'focus did not leave the DOS window')
                time.sleep(.5)
                assert not ui.module()[0][36], 'DOS window still focused'
                for _ in range(3):
                    vm.key('spc')
                    time.sleep(.3)
                time.sleep(1)
                assert h.current_checkpoint() == 1, 'unfocused keys reached the guest'
                ui.click(211)                      # taskbar button of the DOS window
                ui.until(lambda: ui.b('ui_active_window') == 11, 'DOS window did not regain focus')
                event('keys typed while another window was active were withheld from the guest')
                frame, capture = settled_frame(ui, h, 'window-checkpoint-1-refocused')
                checks['1-refocused'] = dict(compare(frame, native[1], client, REPEAT[1]), **capture)
                assert checks['1-refocused']['mismatched_pixels'] == 0, checks['1-refocused']
            if k == 2:
                # Redraw: drag the native Application Library window over the
                # guest area, then raise the DOS window again. Application
                # menus are disabled while a child runs, so an existing window
                # is used.
                x, y = ui.point(60, 0)
                vm.position(x, y)
                vm.hmp('mouse_button 1'); time.sleep(.2)
                vm.move(80, 60); time.sleep(.3)
                vm.hmp('mouse_button 0'); time.sleep(.3)
                ui.until(lambda: ui.b('ui_active_window') == 0, 'Application Library was not raised')
                covered, _ = settled_frame(ui, h, 'window-checkpoint-2-covered')
                checks['2-covered-mismatch'] = compare(covered, native[2], client, REPEAT[2])['mismatched_pixels']
                assert checks['2-covered-mismatch'] > 0, 'the native window never covered the guest area'
                ui.click(211)
                ui.until(lambda: ui.b('ui_active_window') == 11, 'DOS window did not regain focus')
                origin = ui.geometry(11)
                client = (origin[0] + 10, origin[1] + 34)
                frame, capture = settled_frame(ui, h, 'window-checkpoint-2-exposed')
                checks['2-exposed'] = dict(compare(frame, native[2], client, REPEAT[2]), **capture)
                assert checks['2-exposed']['mismatched_pixels'] == 0, checks['2-exposed']
                event('guest area repainted from the model after a native window covered it')
            if k == 3:
                # Minimize and restore through the taskbar.
                ui.click(17, 11)
                ui.until(lambda: ui.b('ui_window_flags', 11) == 2, 'DOS window did not minimize')
                time.sleep(1)
                ui.click(211)
                ui.until(lambda: ui.b('ui_window_flags', 11) == 1 and ui.b('ui_active_window') == 11,
                         'DOS window did not restore')
                origin = ui.geometry(11)
                client = (origin[0] + 10, origin[1] + 34)
                frame, capture = settled_frame(ui, h, 'window-checkpoint-3-restored')
                checks['3-restored'] = dict(compare(frame, native[3], client, REPEAT[3]), **capture)
                assert checks['3-restored']['mismatched_pixels'] == 0, checks['3-restored']
                event('minimize/restore repainted the guest exactly')
            data, _ = ui.module()
            record.setdefault('metrics', {})[f'checkpoint{k}'] = metrics(data)
            vm.key('spc')
        record['checkpoints'] = checks
        ui.until(lambda: ui.b('dos_host_active') == 0, 'guest exit did not return the window', 60)
        data, state = ui.module()
        assert not state['installed'] and not state['live'] and not state['errors'], state
        record['runtime_metrics'] = metrics(data)
        vectors_after = ui.vectors()
        record['vectors'] = dict(before=vectors_before, after=vectors_after)
        assert vectors_before == vectors_after, (vectors_before, vectors_after)
        event('runtime uninstalled; IRQ0/video/keyboard/mouse/multiplex vectors restored exactly')
        h.shot('window-finished')
        # FIRE keeps the compositor busy almost continuously, which can leave
        # no idle boundary to re-read the hit table; the DOS window geometry
        # is fixed, so take its close control from this idle window.
        close_point, close_geometry = ui.point(18, 11), ui.geometry(11)
        ui.click(18, 11)
        ui.until(lambda: ui.b('ui_window_flags', 11) == 0 and ui.w('dw_segment') == 0,
                 'closing the finished window did not release memory')
        record['fire_in_window'] = fire_case(ui, vm, h, event, close_point, close_geometry)
        record['guest_devices'] = guestio_case(ui, vm, h, event, close_point, close_geometry)
        assert ui.vectors() == vectors_before, 'vectors not restored after FIRE'
        offset = vm.offset()
        vm.key('f4')
        vm.wait('CiukiOS SHELL C:\\APPS>', offset, 30)
        h.console('run \\JLOAD.EXE /u \\CVSESS.DLL')
        h.console('run \\JEMM386.EXE UNLOAD', 'unloaded')
        offset = vm.offset()
        h.typed('exit')
        vm.ready(offset)
        h.shot('desktop-restored')
        event('module and Jemm unloaded; desktop returned')
        record['passed'] = True
    except Exception as error:
        record['error'] = repr(error)
        try:
            vm.shot('failure')
            record['pic_on_failure'] = vm.hmp('info pic').decode('utf-8', 'replace')[-200:]
            record['kbc_status_on_failure'] = vm.hmp('i /b 0x64').decode('utf-8', 'replace').split('\r\n')[-2]
            record['registers_on_failure'] = vm.hmp('info registers').decode('utf-8', 'replace')[:900]
            vm.hmp(f'pmemsave 0 0x2000000 "{output}/failure-ram.bin"')
            time.sleep(3)
        except Exception:
            pass
        raise
    finally:
        vm.close()
        fat = FAT16(disk)
        try:
            native_words = fat.read('VGANAT.OUT')
            window_words = fat.read('VGAWIN.OUT')
            diffs = [i // 4 for i in range(0, len(native_words), 4)
                     if native_words[i:i + 4] != window_words[i:i + 4]]
            record['result_word_differences'] = diffs
            # Only the characterised QEMU PEL-mask readback (word 2) may differ.
            if record.get('passed') and (diffs not in ([], [2]) or len(native_words) != len(window_words)):
                record['passed'] = False
                record['error'] = f'result hashes differ at {diffs}'
        except (KeyError, AssertionError) as error:
            record['result_files_error'] = repr(error)
            record['passed'] = False
        try:
            from qemu_test_devices import tone_windows
            _, windows = tone_windows(output / 'audio.wav')
            record['window_audio'] = dict(
                square_seconds=sum(.25 for w in windows if w['kind'] == 'square'),
                fm_seconds=sum(.25 for w in windows if w['kind'] == 'fm'))
            if record.get('guest_devices', {}).get('audio') == 1 and record.get('passed') and (
                    record['window_audio']['square_seconds'] < 1.5 or
                    record['window_audio']['fm_seconds'] < .5):
                record['passed'] = False
                record['error'] = f"guest SB/OPL not heard on the AC'97: {record['window_audio']}"
        except Exception as error:
            record['window_audio_error'] = repr(error)
        record['source_image_unchanged'] = sha(args.image) == before
        (output / 'report.json').write_text(json.dumps(record, indent=2) + '\n')
    assert record['passed'], record.get('error')
    assert record['source_image_unchanged']


if __name__ == '__main__':
    main()
