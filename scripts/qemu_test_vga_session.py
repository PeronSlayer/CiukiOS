#!/usr/bin/env python3
"""Monitored VGA session acceptance on a private CiukiOS disk (QEMU, Pentium III, 128 MiB).

Phases (all by default):
  semantics  VGASEM.COM natively on QEMU's VGA, then as a CVSESSION guest. Result
             hashes must match, and five displayed checkpoints presented through
             the protected LFB must equal QEMU's native display of the same
             program (nearest-neighbour mapping of the presenter, 6-bit DAC).
  fire       Original DOS Navigator FIRE.SS (Turbo Pascal, mode 13h) as a guest:
             presenter output vs the model's exact state, frame timing.
  wolf       Original Wolfenstein 3-D (real mode, unchained 256-colour planar
             with latched copies) as a guest, title and menu checkpoints.
  cleanup    Unload CVSESS and Jemm; verify PTE/BDA restoration and desktop return.

Keyboard events and QEMU monitor reads only: no guest RAM patching, injected
success flags or stopped CPU. Emulator results do not establish physical
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
from qemu_test_vm_session import MonitorVM

ROOT = Path(__file__).resolve().parents[1]
WINDOW = (80, 60, 640, 480)                      # left, top, width, height (VGAHOST)
RESULT_LABELS = (['mode13 chained image', 'DAC block readback', 'PEL mask / DAC state'] +
                 [f'unchained plane {p}' for p in range(4)] +
                 [f'mode 12h plane {p}' for p in range(4)] +
                 ['read mode 1 compare 5/F', 'read mode 1 compare 3/B', 'BIOS pixel XOR'] +
                 [f'mode 0Dh plane {p}' for p in range(4)] +
                 ['text odd/even + TTY + scroll', 'text cursor after TTY'] +
                 [f'registers after mode {m:02X}h' for m in (3, 0x13, 0x12, 0x0d, 0x0e, 0x10, 0x01)])
CHECKPOINT_REPEAT = {1: 2, 2: 2, 3: 1, 4: 2, 5: 1}  # scanline repeat of each displayed mode
STATE_OFFSETS = dict(dac=262208, misc=262981, dac_mask=262984, display_changes=262992)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def nasm(source, output, *defines):
    subprocess.run(['nasm', '-f', 'bin', *defines, str(source), '-o', str(output)], check=True)


class Session:
    def __init__(self, vm, output, record):
        self.vm = vm
        self.output = output
        self.record = record

    def memory(self, address, count, name):
        path = self.output / (name + '.bin')
        self.vm.hmp(f'pmemsave {address:#x} {count} "{path}"')
        deadline = time.monotonic() + 5
        while not path.exists() or path.stat().st_size < count:
            assert time.monotonic() < deadline, 'pmemsave did not complete'
            time.sleep(.02)
        return path.read_bytes()

    def low(self, name='low'):
        return self.memory(0, 0x110000, name)

    def find(self, magic, predicate=None, timeout=30, name='scan'):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            data = self.low(name)
            for match in re.finditer(re.escape(magic), data):
                if predicate is None or predicate(data, match.start()):
                    return match.start(), data
            time.sleep(.2)
        raise AssertionError(f'observation {magic!r} not reached ({name})')

    def host_block(self, data=None):
        """Parse the VGAHOST observation block."""
        at, data = (None, data) if data else self.find(b'VGAHOST1', name='host')
        if at is None:
            at = data.index(b'VGAHOST1')
        state, exit_code, tsc, fb, pitch = struct.unpack_from('<5I', data, at + 8)
        packet_offset, observation_offset = struct.unpack_from('<2H', data, at + 28)
        cs_base = at - observation_offset
        share = at + 32
        magic, pages = struct.unpack_from('<2I', data, share)
        page_list = list(struct.unpack_from(f'<{pages}I', data, share + 8)) if magic == 0x48535643 else []
        video = share + 8 + 72 * 4
        text = data[video + 256:video + 256 + 4000]
        attributes = data[video + 256 + 4000:video + 256 + 8000]
        cells = bytes(b for pair in zip(text[0:4000:2], attributes[0:4000:2]) for b in pair)
        segment = at >> 4                         # COM: CS = start of block's segment region
        return dict(at=at, state=state, exit=exit_code, tsc_khz=tsc, fb=fb, pitch=pitch,
                    pages=page_list, video=data[video:video + 256],
                    text=[bytes(text[r * 160:r * 160 + 160:2]).decode('cp437').rstrip(' \x00')
                          for r in range(25)], cells=cells,
                    packet=cs_base + packet_offset, segment=segment)

    def live(self, pages, name='live'):
        """VIDEO_STATE mirror refreshed by the monitor on every timer tick."""
        page = self.memory(pages[0], 4096, name)
        assert page[:4] == b'CVVS', 'shared header missing'
        return page[64:64 + 256]

    def model(self, pages, name):
        """Read the exact shared model block (header page + cvga_state) by physical page."""
        chunks = [self.memory(page, 4096, f'{name}-page{index:02d}') for index, page in enumerate(pages)]
        block = b''.join(chunks)
        assert block[:4] == b'CVVS', 'shared model header missing'
        return block[4096:4096 + 267092]


VS = dict(faults=16, instructions=20, elements=24, unsupported=28, bus_faults=32, port_reads=36,
          port_writes=40, bios_calls=44, bios_unsupported=48, bios_last=52, mode_sets=56, mode=60,
          width=64, rows=68, repeat=72, text=76, display_changes=80, changes=84, fatal=88,
          fatal_csip=92, presents=112, present_rows=116, present_pixels=120, frames=124,
          full_redraws=128, last_present_tsc=132, max_present_tsc=136, total_present_tsc=140,
          host_mode=148, host_faults=156, host_bytes=160, tsc_khz=164, status_polls=168,
          fault_tsc=172, crtc_start=180, share_count=184, state_bytes=188, host_enters=192,
          last_fault_csip=200, pm_faults=204, pm_instructions=208, pm_unsupported=212,
          pm_attached=216, passes=220)


def video_fields(packet):
    out = {name: struct.unpack_from('<I', packet, offset)[0] for name, offset in VS.items()}
    out['total_present_tsc'] = struct.unpack_from('<Q', packet, VS['total_present_tsc'])[0]
    out['fault_tsc'] = struct.unpack_from('<Q', packet, VS['fault_tsc'])[0]
    out['fatal_bytes'] = packet[96:112].hex()
    return out


def rates(first, second, seconds):
    tsc = second['tsc_khz'] or 1
    presents = second['presents'] - first['presents']
    elapsed_present = second['total_present_tsc'] - first['total_present_tsc']
    return dict(
        seconds=round(seconds, 3),
        trapped_instructions_per_second=round((second['instructions'] - first['instructions']) / seconds),
        string_elements_per_second=round((second['elements'] - first['elements']) / seconds),
        emulation_cpu_fraction=round((second['fault_tsc'] - first['fault_tsc']) / (tsc * 1000 * seconds), 4),
        presents_per_second=round(presents / seconds, 2),
        completed_frames_per_second=round((second['frames'] - first['frames']) / seconds, 2),
        refresh_passes_per_second=round((second['passes'] - first['passes']) / seconds, 2),
        mean_present_ms=round(elapsed_present / presents / tsc, 3) if presents else None,
        max_present_ms=round(second['max_present_tsc'] / tsc, 3),
        presented_pixels_per_second=round((second['present_pixels'] - first['present_pixels']) / seconds))


def presented(shot_path):
    return np.array(Image.open(shot_path).convert('RGB'), dtype=np.uint8)


def expected_from_native(native, repeat):
    """Apply the presenter's nearest-neighbour mapping to a native display."""
    left, top, width, height = WINDOW
    rows, columns = native.shape[:2]
    physical = rows * repeat
    xs = (np.arange(width) * columns) // width
    ys = ((np.arange(height) * physical) // height) // repeat
    return native[ys][:, xs]


def compare_checkpoint(session_shot, native_shot, repeat, masks=(), focus=()):
    """masks: (x0, y0, x1, y1) rectangles in NATIVE coordinates whose content is
    driven by the program's own clock; their differences are reported apart.
    focus: rectangles holding the program's keyboard-focus highlight, whose
    on/off state after the same keys varies from run to run natively too."""
    session = presented(session_shot)
    native = presented(native_shot)
    left, top, width, height = WINDOW
    window = session[top:top + height, left:left + width]
    expected = expected_from_native(native, repeat)
    # Six-bit DAC components: presenters may expand 6->8 bits differently.
    diff = np.any((window >> 2) != (expected >> 2), axis=2)
    rows, columns = native.shape[:2]
    xs = (np.arange(width) * columns) // width
    ys = ((np.arange(height) * rows * repeat) // height) // repeat
    def region(rectangles):
        inside = np.zeros_like(diff)
        for x0, y0, x1, y1 in rectangles:
            inside |= ((ys[:, None] >= y0) & (ys[:, None] < y1) & (xs[None, :] >= x0) & (xs[None, :] < x1))
        return inside
    masked = region(masks)
    focused = region(focus) & ~masked
    outside = session.copy()
    outside[top:top + height, left:left + width] = 0
    return dict(native_size=list(native.shape[1::-1]),
                mismatched_pixels=int(np.count_nonzero(diff & ~masked & ~focused)),
                masked_program_clock_pixels=int(np.count_nonzero(diff & masked)),
                focus_highlight_pixels=int(np.count_nonzero(diff & focused)),
                compared_pixels=int(np.count_nonzero(~masked & ~focused)),
                outside_nonblack=int(np.count_nonzero(outside.any(axis=2))))


def fire_palette():
    """DAC entries FIRE.SS programs, derived from its own code (disassembled
    at offset 5Ah: out 3C8h,0 then i/2, i*22/128, i<8 ? i/2 : 0 for i<128)."""
    return [(i // 2, (i * 22) // 128, i // 2 if i < 8 else 0) for i in range(128)]


def fire_phase(vm, s):
    result = {}
    offset = vm.offset()
    line = 'run \\VGAHOST.COM P \\APPS\\DOSNAV\\SSAVERS\\FIRE.EXE'
    print('[vga-session] ' + line, flush=True)
    vm.text(line)
    at, data = s.find(b'VGAHOST1', lambda d, a: struct.unpack_from('<I', d, a + 8)[0] == 1,
                      timeout=60, name='fire-running')
    block = s.host_block(data)
    assert len(block['pages']) == 67, block['pages']
    deadline = time.monotonic() + 30
    while True:
        live = video_fields(s.live(block['pages'], 'fire-live'))
        if live['mode'] == 0x13 and live['instructions'] > 1000:
            break
        assert time.monotonic() < deadline, ('FIRE.SS never reached mode 13h output', live)
        time.sleep(.2)
    time.sleep(2)
    first = video_fields(s.live(block['pages'], 'fire-first'))
    t0 = time.monotonic()
    shots = []
    for i in range(3):
        time.sleep(2)
        shots.append(vm.shot(f'fire-{i}').with_suffix('.png'))
    second = video_fields(s.live(block['pages'], 'fire-second'))
    t1 = time.monotonic()
    model = s.model(block['pages'], 'fire-model')
    dac = [tuple(model[STATE_OFFSETS['dac'] + i * 3:STATE_OFFSETS['dac'] + i * 3 + 3]) for i in range(256)]
    result['palette_matches_program'] = dac[:128] == fire_palette()
    assert result['palette_matches_program'], [(i, dac[i], fire_palette()[i]) for i in range(128)
                                               if dac[i] != fire_palette()[i]][:8]
    result['geometry'] = dict(mode=second['mode'], width=second['width'], rows=second['rows'],
                              repeat=second['repeat'])
    assert (second['mode'], second['width'], second['rows'], second['repeat']) == (0x13, 320, 200, 2)
    assert second['fatal'] == 0 and second['unsupported'] == 0 and second['bus_faults'] == 0, second
    colours = {tuple(((c << 2) | (c >> 4)) >> 2 for c in entry) for entry in dac}
    left, top, width, height = WINDOW
    channels = [{c[i] for c in colours} for i in range(3)]
    for shot in shots:
        window = presented(shot)[top:top + height, left:left + width] >> 2
        values, counts = np.unique(window.reshape(-1, 3), axis=0, return_counts=True)
        found = {tuple(int(v) for v in value): int(n) for value, n in zip(values, counts)}
        foreign = {c: n for c, n in found.items() if c not in colours}
        # QEMU's screendump runs asynchronously to the vCPU: a 24-bit pixel
        # can be captured between byte stores. Such a tear still has every
        # channel from some DAC colour; anything else is a presenter error.
        for colour in foreign:
            assert all(colour[i] in channels[i] for i in range(3)), ('colour outside the guest DAC', colour)
        torn = sum(foreign.values())
        assert torn <= window.shape[0] * window.shape[1] // 2000, ('too many off-palette pixels', foreign)
        result.setdefault('distinct_presented_colours', []).append(len(found))
        result.setdefault('torn_capture_pixels', []).append(torn)
    assert max(result['distinct_presented_colours']) > 20, 'fire was not presented'
    result['measured'] = rates(first, second, t1 - t0)
    packet = s.memory(block['packet'], 256, 'fire-packet')
    pitch, width, height, depth = struct.unpack_from('<IHHB', packet, 16)
    result['framebuffer'] = dict(physical=hex(block['fb']), pitch=pitch, width=width, height=height,
                                 bytes_per_pixel=depth)
    result['presenter_packet'] = dict(zip(('interval_us', 'ticks', 'presents', 'last_tsc', 'max_tsc'),
                                          struct.unpack_from('<5I', packet, 212)))
    result['counters_end'] = {k: second[k] for k in ('faults', 'instructions', 'elements', 'port_writes',
                                                     'bios_calls', 'bios_unsupported', 'presents',
                                                     'frames', 'full_redraws', 'status_polls')}
    vm.key('spc')                                   # FIRE.SS exits on a key
    vm.wait('[VGAHOST] PASS', offset, 60)
    vm.wait('CiukiOS SHELL C:\\APPS>', offset, 60)
    body = subprocess.check_output(['scripts/serial_log_normalize.py', '--offset', str(offset),
                                    str(vm.serial)]).decode('cp437')
    result['host_summary'] = [l for l in body.splitlines() if '[VGAHOST]' in l]
    assert ' exit=00000000 ' in result['host_summary'][0], result['host_summary']
    return result


# The cell under Costa's keyboard cursor after the 'flag' keys; whether its
# white focus highlight is shown afterwards differs between runs on both the
# native VGA and the session (observed stable for 4 s either way).
COSTA_FOCUS = {'flag': ((369, 213, 384, 228),)}
COSTA_STEPS = (('start', []), ('flag', ['right', 'right', 'right', 'down', 'down', 'spc']),
               ('move', ['left', 'down', 'spc', 'up', 'up']), ('about', ['a']))


def settled_shot(vm, name, attempts=12, stable=2):
    """Capture once consecutive frames 0.5 s apart agree for `stable` intervals,
    ignoring the bottom status rows (Costa's running clock). A fixed delay
    alone raced the key-release redraw under host load. Changes observed
    while settling are recorded in SETTLE_LOG for the report."""
    previous = presented(vm.shot(name).with_suffix('.png'))
    agreed = changes = 0
    for attempt in range(attempts):
        time.sleep(.5)
        path = vm.shot(name).with_suffix('.png')
        current = presented(path)
        rows = int(current.shape[0] * .84)
        if current.shape == previous.shape and np.array_equal(current[:rows], previous[:rows]):
            agreed += 1
            if agreed >= stable:
                SETTLE_LOG[name] = dict(attempts=attempt + 1, changes=changes)
                return path
        else:
            agreed = 0
            changes += 1
        previous = current
    raise AssertionError(f'{name}: display did not settle')


SETTLE_LOG = {}


def costa_run(vm, prefix, repeat_settle):
    """Drive the original Costa MINES.EXE with a fixed keyboard sequence."""
    shots = {}
    time.sleep(4)
    for label, keys in COSTA_STEPS:
        for key in keys:
            vm.key(key)
            time.sleep(.3)
        time.sleep(repeat_settle)
        shots[label] = settled_shot(vm, f'{prefix}-costa-{label}')
    vm.key('esc')                                  # close About
    time.sleep(1)
    vm.key('x')                                    # Exit button (underlined letter)
    return shots


def owned_state(vm, s, name):
    """Guest-visible state a session must restore exactly, read by physical address."""
    registers = vm.hmp('info registers').decode(errors='replace')
    cr3 = int(re.search(r'CR3=([0-9a-f]{8})', registers).group(1), 16)
    pde, = struct.unpack('<I', s.memory(cr3 & ~0xfff, 4, name + '-pde'))
    assert pde & 1 and not pde & 0x80, 'expected a 4 KiB low page table'
    ptes = struct.unpack('<32I', s.memory((pde & ~0xfff) + 0xa0 * 4, 128, name + '-ptes'))
    return dict(cr3=cr3, ptes=list(ptes),
                bda_video=s.memory(0x449, 0x467 - 0x449, name + '-bda1').hex() +
                          s.memory(0x484, 7, name + '-bda2').hex(),
                vectors=s.memory(0x1f * 4, 4, name + '-int1f').hex() + s.memory(0x43 * 4, 4, name + '-int43').hex(),
                physical_text=hashlib.sha256(s.memory(0xb8000, 0x8000, name + '-b800')).hexdigest(),
                physical_graphics=hashlib.sha256(s.memory(0xa0000, 0x10000, name + '-a000')).hexdigest(),
                display=str(vm.shot(name + '-display').with_suffix('.png')))


def cleanup_phase(vm, s):
    result = {}
    offset = vm.offset()
    line = 'run \\VGAHOST.COM NH \\VGASEM.COM \\VGACLN.OUT W'
    print('[vga-session] ' + line, flush=True)
    vm.text(line)
    s.find(b'VGAHOST1', lambda d, a: struct.unpack_from('<I', d, a + 8)[0] == 0x10, timeout=60,
           name='cleanup-before-scan')
    time.sleep(.5)
    before = owned_state(vm, s, 'cleanup-before')
    vm.key('spc')
    s.find(b'VGASEMOB', lambda d, a: d[a + 8] == 1, timeout=60, name='cleanup-guest-scan')
    time.sleep(.5)
    during = owned_state(vm, s, 'cleanup-during')
    for k in range(1, 6):
        if k > 1:
            s.find(b'VGASEMOB', lambda d, a, k=k: d[a + 8] == k, timeout=60, name=f'cleanup-cp{k}')
        vm.key('spc')
    s.find(b'VGAHOST1', lambda d, a: struct.unpack_from('<I', d, a + 8)[0] == 0x11, timeout=60,
           name='cleanup-after-scan')
    time.sleep(.5)
    after = owned_state(vm, s, 'cleanup-after')
    vm.key('spc')
    vm.wait('[VGAHOST] PASS', offset, 60)
    vm.wait('CiukiOS SHELL C:\\APPS>', offset, 60)
    result.update(before=before, during=during, after=after)
    result['guarded_during'] = all((p & 7) == 3 for p in during['ptes'])
    result['ptes_changed_during'] = all(a != b for a, b in zip(before['ptes'], during['ptes']))
    result['ptes_restored_exactly'] = before['ptes'] == after['ptes']
    for key in ('bda_video', 'vectors', 'physical_text', 'physical_graphics'):
        result[key + '_restored'] = before[key] == after[key]
    for key in ('physical_text', 'physical_graphics'):
        result[key + '_unchanged_during'] = before[key] == during[key]
    for label, other in (('during', during), ('after', after)):
        result['physical_display_' + label] = display_difference(before['display'], other['display'])
    assert result['guarded_during'] and result['ptes_changed_during'], during['ptes']
    assert result['ptes_restored_exactly'], (before['ptes'], after['ptes'])
    for key in ('bda_video', 'vectors', 'physical_text', 'physical_graphics'):
        assert result[key + '_restored'], (key, before[key], after[key])
    for key in ('physical_text', 'physical_graphics'):
        assert result[key + '_unchanged_during'], key
    for label in ('during', 'after'):
        assert result['physical_display_' + label]['classification'] != 'changed', result
    return result


def display_difference(first, second):
    """Physical display comparison. QEMU renders the VGA hardware text cursor
    with its own blink timer, so a difference confined to one character cell
    and at most two scanlines is classified as cursor blink, nothing else."""
    a = presented(first)
    b = presented(second)
    if a.shape != b.shape:
        return dict(classification='changed', reason='display size', sizes=[a.shape, b.shape])
    diff = np.any(a != b, axis=2)
    count = int(np.count_nonzero(diff))
    if not count:
        return dict(classification='identical', pixels=0)
    ys, xs = np.nonzero(diff)
    box = [int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())]
    blink = box[3] - box[1] < 2 and box[2] - box[0] < 9 and box[0] // 9 == box[2] // 9
    return dict(classification='hardware cursor blink' if blink else 'changed', pixels=count, box=box)


WOLF_MESSAGE = 'You do not have enough memory to run Wolfenstein 3-D.'


def wolf_native(vm, s):
    """Original WOLF3D.EXE natively. On CiukiOS the shell's INT 10h layer draws
    text-mode programs inside its VBE console, so the evidence is the screen
    capture plus the return to the prompt, not physical B800 memory."""
    vm.result('cd WOLF3D', prompt='CiukiOS SHELL C:\\APPS\\WOLF3D>')
    offset = vm.offset()
    vm.text('run WOLF3D.EXE')
    vm.wait('CiukiOS SHELL C:\\APPS\\WOLF3D>', offset, 60)
    shot = vm.shot('wolf-native-screen').with_suffix('.png')
    vm.result('cd ..', prompt='CiukiOS SHELL C:\\APPS>')
    return shot.name


def wolf_session(vm, s, native_shot):
    offset = vm.offset()
    line = 'run \\VGAHOST.COM N \\APPS\\WOLF3D\\WOLF3D.EXE'
    print('[vga-session] ' + line, flush=True)
    vm.text(line)
    vm.wait('[VGAHOST] PASS', offset, 90)
    vm.wait('CiukiOS SHELL C:\\APPS>', offset, 60)
    block = s.host_block()
    text = [line for line in block['text'] if line.strip()]
    result = dict(exit=f"{block['exit']:04x}", native_screenshot=native_shot, session_text=text,
                  video=video_fields(block['video']))
    assert block['exit'] == 1, ('WOLF3D.EXE exit code', block['exit'])
    assert any(WOLF_MESSAGE in t for t in text), text
    assert result['video']['unsupported'] == 0 and result['video']['fatal'] == 0, result['video']
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--jemm', type=Path, required=True)
    parser.add_argument('--jload', type=Path, required=True)
    parser.add_argument('--module', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--phases', default='semantics,fire,wolf,costa,cleanup')
    args = parser.parse_args()
    phases = set(args.phases.split(','))
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    disk = output / 'disk.img'
    before = sha(args.image)
    shutil.copyfile(args.image, disk)
    volume = f'{disk}@@{FAT16(disk).start}'
    probes = {'VGASEM.COM': 'vga_semantics.asm', 'VGAHOST.COM': 'vga_host.asm', 'VMSESS.COM': 'session.asm',
              'MEMFREE.COM': 'mem_free.asm'}
    for name, source in probes.items():
        nasm(ROOT / 'src/probes/vm' / source, output / name)
    files = [(args.kernel, 'SYSTEM/CIUKIDOS.SYS'), (args.jemm, 'JEMM386.EXE'), (args.jload, 'JLOAD.EXE'),
             (args.module, 'CVSESS.DLL')] + [(output / name, name) for name in probes]
    # The CiukiOS EXEC loader accepts only .COM/.EXE/.APP/.PRG names. The
    # unchanged DOS Navigator FIRE.SS bytes are therefore also placed next to
    # the original as FIRE.EXE; identical hashes are recorded below.
    fire_bytes = FAT16(disk).read('APPS/DOSNAV/SSAVERS/FIRE.SS')
    (output / 'FIRE.EXE').write_bytes(fire_bytes)
    files.append((output / 'FIRE.EXE', 'APPS/DOSNAV/SSAVERS/FIRE.EXE'))
    for path, target in files:
        subprocess.run(['mcopy', '-o', '-i', volume, str(path), '::' + target], check=True)
    record = dict(passed=False, cpu='pentium3', memory_mib=128, accelerator='kvm',
                  source_image_sha256=before, prepared_image_sha256=sha(disk),
                  inputs={target: sha(path) for path, target in files},
                  physical_hardware_qualified=False, hardware_acceleration_claimed=False,
                  phases=sorted(phases), events=[],
                  fire_ss_sha256=hashlib.sha256(fire_bytes).hexdigest(),
                  fire_exe_copy_sha256=sha(output / 'FIRE.EXE'))
    vm = MonitorVM(disk, output, memory=128)
    record['qemu_command'] = vm.process.args
    s = Session(vm, output, record)
    started = time.monotonic()

    def command(line, *expected, timeout=60):
        print('[vga-session] ' + line, flush=True)
        body = vm.result(line, *expected, timeout=timeout)
        record['events'].append(dict(command=line, output=body, elapsed=round(time.monotonic() - started, 3)))
        return body

    def run_checkpoints(line, prefix, host):
        """Start a VGASEM run, capture its five display checkpoints."""
        offset = vm.offset()
        print('[vga-session] ' + line, flush=True)
        vm.text(line)
        shots = {}
        for k in range(1, 6):
            s.find(b'VGASEMOB', lambda d, a, k=k: d[a + 8] == k, timeout=60, name=f'{prefix}-cp{k}')
            time.sleep(1.2 if host else .5)       # presenter: damage-limited frames at <=70 Hz
            shots[k] = vm.shot(f'{prefix}-checkpoint-{k}').with_suffix('.png')
            if host and k == 1:
                block = s.host_block()
                record.setdefault('session', {})['pages'] = len(block['pages'])
            vm.key('spc')
        return offset, shots

    try:
        vm.wait('[DESKTOP] READY', timeout=120)
        vm.key('f4')
        vm.wait('CiukiOS SHELL C:\\APPS>')
        native_shots = {}
        if 'semantics' in phases:
            offset, native_shots = run_checkpoints('run \\VGASEM.COM \\VGANAT.OUT W', 'native', False)
            vm.wait('[VGASEM] results written', offset, 60)
            vm.wait('CiukiOS SHELL C:\\APPS>', offset, 60)
        wolf_shot = None
        if 'wolf' in phases:
            wolf_shot = wolf_native(vm, s)
        costa_native = {}
        if 'costa' in phases:
            vm.result('cd COSTA', prompt='CiukiOS SHELL C:\\APPS\\COSTA>')
            offset = vm.offset()
            vm.text('run MINES.EXE')
            costa_native = costa_run(vm, 'native', 1.0)
            vm.wait('CiukiOS SHELL C:\\APPS\\COSTA>', offset, 60)
            vm.result('cd ..', prompt='CiukiOS SHELL C:\\APPS>')
        memory = record.setdefault('conventional_memory_paragraphs', {})
        body = command('run \\MEMFREE.COM', '[MEMFREE]')
        memory['native'] = re.search(r'paragraphs=([0-9A-F]{4})', body).group(1)
        command('run \\VMSESS.COM', '[VMSESSION] No V86 monitor')
        command('run \\JEMM386.EXE LOAD NOEMS X=A000-FFFF NODYN MAX=32M MIN=32M NOVME')
        command('run \\JLOAD.EXE \\CVSESS.DLL')
        vm.shot('monitor-loaded')
        body = command('run \\MEMFREE.COM', '[MEMFREE]')
        memory['jemm_and_module'] = re.search(r'paragraphs=([0-9A-F]{4})', body).group(1)
        body = command('run \\VGAHOST.COM N \\MEMFREE.COM', '[MEMFREE]', '[VGAHOST] PASS')
        memory['session_guest'] = re.search(r'paragraphs=([0-9A-F]{4})', body).group(1)
        print('[vga-session] conventional memory', memory, flush=True)
        if 'semantics' in phases:
            offset, session_shots = run_checkpoints('run \\VGAHOST.COM P \\VGASEM.COM \\VGASES.OUT W',
                                                    'session', True)
            vm.wait('[VGASEM] results written', offset, 120)
            vm.wait('[VGAHOST] PASS', offset, 60)
            vm.wait('CiukiOS SHELL C:\\APPS>', offset, 60)
            body = subprocess.check_output(['scripts/serial_log_normalize.py', '--offset', str(offset),
                                            str(vm.serial)]).decode('cp437')
            record['semantics_host_summary'] = [l for l in body.splitlines() if '[VGAHOST]' in l]
            checks = {}
            for k in range(1, 6):
                checks[k] = compare_checkpoint(session_shots[k], native_shots[k], CHECKPOINT_REPEAT[k])
            record['display_checkpoints'] = checks
            for k, check in checks.items():
                assert check['mismatched_pixels'] == 0 and check['outside_nonblack'] == 0, (k, check)
        if 'fire' in phases:
            record['fire'] = fire_phase(vm, s)
        if 'wolf' in phases:
            record['wolf_memory_limit'] = wolf_session(vm, s, wolf_shot)
        if 'cleanup' in phases:
            record['cleanup'] = cleanup_phase(vm, s)
        if 'costa' in phases:
            offset = vm.offset()
            line = 'run \\VGAHOST.COM P \\APPS\\COSTA\\MINES.EXE'
            print('[vga-session] ' + line, flush=True)
            vm.text(line)
            at, data = s.find(b'VGAHOST1', lambda d, a: struct.unpack_from('<I', d, a + 8)[0] == 1,
                              timeout=60, name='costa-running')
            block = s.host_block(data)
            session_costa = costa_run(vm, 'session', 1.6)
            vm.wait('[VGAHOST] PASS', offset, 60)
            vm.wait('CiukiOS SHELL C:\\APPS>', offset, 60)
            body = subprocess.check_output(['scripts/serial_log_normalize.py', '--offset', str(offset),
                                            str(vm.serial)]).decode('cp437')
            costa = dict(host_summary=[l for l in body.splitlines() if '[VGAHOST]' in l], checkpoints={})
            for label in session_costa:
                # Minesweeper's elapsed-time display (bottom centre) follows the
                # game clock, which starts at the first move.
                costa['checkpoints'][label] = compare_checkpoint(
                    session_costa[label], costa_native[label], 1, masks=((300, 326, 380, 348),),
                    focus=COSTA_FOCUS.get(label, ()))
            costa['video'] = video_fields(s.host_block()['video'])
            costa['settling'] = SETTLE_LOG
            record['costa'] = costa
            assert ' exit=00000000 ' in costa['host_summary'][0], costa['host_summary']
            assert costa['video']['unsupported'] == 0 and costa['video']['fatal'] == 0, costa['video']
            for label, check in costa['checkpoints'].items():
                assert check['mismatched_pixels'] == 0 and check['outside_nonblack'] == 0, (label, check)
        if 'cleanup' in phases:
            command('run \\JLOAD.EXE /u \\CVSESS.DLL')
            command('run \\JEMM386.EXE UNLOAD', 'unloaded')
            command('run \\VMSESS.COM', '[VMSESSION] No V86 monitor')
            offset = vm.offset()
            vm.text('exit')
            vm.wait('[DESKTOP] READY', offset, 90)
            vm.shot('desktop-restored')
            record['monitor_unloaded_and_desktop_returned'] = True
        record['passed'] = True
    except Exception as error:
        record['error'] = repr(error)
        try:
            vm.shot('failure')
            record['registers'] = vm.hmp('info registers').decode(errors='replace')
        except Exception as nested:                   # pragma: no cover - diagnostics only
            record['diagnostic_error'] = repr(nested)
        raise
    finally:
        vm.close()
        fat = FAT16(disk)
        try:
            native = fat.read('VGANAT.OUT')
            session = fat.read('VGASES.OUT')
            n = [struct.unpack_from('<I', native, i)[0] for i in range(0, len(native), 4)]
            m = [struct.unpack_from('<I', session, i)[0] for i in range(0, len(session), 4)]
            record['result_words'] = dict(native=len(n), session=len(m))
            record['result_mismatches'] = [
                dict(index=i, label=RESULT_LABELS[i] if i < len(RESULT_LABELS) else str(i),
                     native=f'{a:08x}', session=f'{b:08x}')
                for i, (a, b) in enumerate(zip(n, m)) if a != b]
            # QEMU's VGA returns 00h from the PEL mask register (3C6h) after
            # 5Ah was written; IBM VGA defines it read/write and the model
            # returns 5Ah. That single, exactly characterised item is the only
            # tolerated difference; the DAC state byte must still agree.
            unexplained = [m for m in record['result_mismatches']
                           if not (m['index'] == 2 and m['native'][-2:] == '00' and m['session'][-2:] == '5a'
                                   and m['native'][:-2] == m['session'][:-2])]
            record['qemu_pel_mask_deviation'] = len(unexplained) != len(record['result_mismatches'])
            if unexplained or len(n) != len(RESULT_LABELS) or len(m) != len(n):
                record['passed'] = False
                record.setdefault('error', repr(('semantic results differ', unexplained, len(n), len(m))))
        except (KeyError, AssertionError, ValueError) as error:
            if 'semantics' in phases:
                record['passed'] = False
                record.setdefault('error', repr(error))
            record['result_files_error'] = repr(error)
        record['source_image_unchanged'] = sha(args.image) == before
        (output / 'report.json').write_text(json.dumps(record, indent=2) + '\n')
        assert record['source_image_unchanged']
        assert record['passed'], record.get('error')


if __name__ == '__main__':
    main()
