#!/usr/bin/env python3
"""Gate for the CVSESSION VM manager (several DOS VMs, roadmap phase 2).

Boots the main image (the desktop starts the VM manager at boot) with the
Jemm386 and CVSESS.DLL under test, opens the DOS console and runs the parts
selected with --parts (default: all, in this order, in one boot):

switch  \\VMMTEST.COM creates a second VM running \\VMMCHILD.COM; both print on
        COM1 in loops that never yield. Needs interleaved [VM0]/[VM1] lines,
        the child running on while the system VM waits for a key inside DOS,
        its exit code 42 reported by the VM manager.
io      \\VMMIO.COM writes and reads back one file from each of two VMs at
        once. Needs both files intact on the disk image and a clean FAT
        (fsck.fat -n), checked from the host after shutdown.
window  \\VMWTEST.COM runs \\VMWCHILD.COM in a VM that owns the DOS window
        session (virtual VGA, device-model keyboard). The system VM reads its
        virtual screen and gives it the keyboard; Ctrl+Esc takes it back.
        Needs each VM to get exactly its own keys (VM 1 never sees the
        hotkey) and VM 1's text never on the physical screen.
sound   \\VMSTEST.COM runs \\VMSCHILD.COM in VM 1, which plays a 220 Hz square
        wave through the virtual SB16 while the system VM keeps the CPU busy.
        Needs the recorded AC'97 output to be that square wave (the DMA
        buffer of VM 1 read while the system VM runs, too).
kill    \\VMKTEST.COM: VMs holding the DOS window session that exit without
        ending it, end it themselves, or hang with CLI and are ended by
        VMM_KILL. Needs the session inactive after each, reusable by the
        system VM, and the next physical key arriving there unchanged.
ivt     \\VMITEST.COM hooks INT 1Ch/09h in its own block and forks
        \\VMICHILD.COM, in whose VM that block is freed. Needs the vectors
        restored there (VM 1 then runs 40 ticks over the overwritten memory)
        and the system VM's own hooks still counting.
clock   \\VMCTEST.COM and \\VMCCHILD.COM (VM 1), both busy, count BIOS timer
        ticks over 10 s of real time, both reading the CMOS clock through
        its one index register. Needs 165-200 ticks in each VM (18.2/s):
        the ticks a VM misses while the other runs reach it afterwards, and
        each VM's CMOS index is its own.
multi   \\VMDTEST.COM runs \\VMWCHILD.COM in VM 1 and VM 2, each with its own
        DOS window session. Needs both virtual screens seen apart, a
        reaching VM 1 only, b VM 2 only, Ctrl+Esc taking the keyboard back,
        x read by the system VM, both ended through their device models,
        and neither VM's text on the physical screen.
mouse   \\VMOTEST.COM runs \\VMOCHILD.COM in VM 1 and VM 2, each with a DOS
        window session whose device model has the mouse; each installs a
        BIOS pointing-device handler (INT 15h C2xxh). The physical mouse
        moves right with VM 1 focused, then down with VM 2 focused, then
        DEV_MOUSE moves VM 1's pointer. Needs every movement in the focused
        (or addressed) VM only, the injected packet exact, both ended.
audio   \\VMATEST.COM runs \\VMSCHILD.COM in VM 1 (220 Hz square wave) and
        VM 2 (551 Hz) through their own SB16 models at once, and moves the
        focus: VM 1, VM 2, the system VM (no sound: VM 1 plays), then ends
        VM 1 (VM 2 plays). Needs the recorded AC'97 output of each 3 s state
        to be the owner's wave alone, and each VM's blocks played in real
        time (a muted session keeps going).

After every part the DOS console must still answer. QEMU evidence only.
"""
import argparse
import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze_audio_wav import pcm_payload                      # noqa: E402
from qemu_test_installed_hdd import FAT16                     # noqa: E402
from qemu_test_native_windows import WindowVM                 # noqa: E402
from qemu_test_vga_window import Harness                       # noqa: E402
from PIL import Image                                          # noqa: E402

PARTS = ('switch', 'io', 'window', 'sound', 'kill', 'ivt', 'clock', 'multi', 'mouse', 'audio')
PROBES = {'switch': ('VMMTEST.COM', 'VMMCHILD.COM'), 'io': ('VMMIO.COM',),
          'window': ('VMWTEST.COM', 'VMWCHILD.COM'), 'sound': ('VMSTEST.COM', 'VMSCHILD.COM'),
          'kill': ('VMKTEST.COM', 'VMKCHILD.COM'), 'ivt': ('VMITEST.COM', 'VMICHILD.COM'),
          'clock': ('VMCTEST.COM', 'VMCCHILD.COM'), 'multi': ('VMDTEST.COM', 'VMWCHILD.COM'),
          'mouse': ('VMOTEST.COM', 'VMOCHILD.COM'), 'audio': ('VMATEST.COM', 'VMSCHILD.COM')}


def wait_result(vm, h, offset, tag, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = h.serial(offset)
        if f'[{tag}] PASS' in text or f'[{tag}] FAIL' in text:
            break
        time.sleep(1)
    time.sleep(1)
    return h.serial(offset)


def console_alive(vm, h):
    offset = vm.offset()
    h.typed('echo VMM CONSOLE OK')
    vm.wait('VMM CONSOLE OK', offset, 30)
    return True


def part_switch(vm, h, out):
    r = {}
    offset = vm.offset()
    h.typed('run \\VMMTEST.COM')
    # VM0 waits for a key inside DOS; VM1 must finish meanwhile. The key is
    # sent after that, so it cannot reach VM 1.
    vm.wait('[VMMTEST] waiting for a key', offset, 60)
    waiting = h.serial(offset).index('[VMMTEST] waiting for a key')
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline and '[VM1] 0013' not in h.serial(offset)[waiting:]:
        time.sleep(0.5)
    time.sleep(1)
    after_wait = h.serial(offset)[waiting:]
    r['vm1_lines_during_key_wait'] = after_wait.count('[VM1]')
    r['vm0_blocked_during_key_wait'] = '[VM0]' not in after_wait
    vm.key('k')
    text = wait_result(vm, h, offset, 'VMMTEST', 240)
    lines = [l.strip() for l in text.splitlines() if l.startswith('[VM')]
    r['lines'] = lines[-40:]
    order = [l[:5] for l in lines if l.startswith('[VM0]') or l.startswith('[VM1]')]
    r['alternations'] = sum(1 for a, b in zip(order, order[1:]) if a != b)
    r['vm1_lines'] = sum(1 for l in order if l == '[VM1]')
    r['probe_pass'] = '[VMMTEST] PASS' in text
    r['key_read'] = '[VMMTEST] key read' in text
    r['passed'] = (r['probe_pass'] and r['vm1_lines'] == 20 and r['alternations'] >= 3 and
                   r['vm1_lines_during_key_wait'] >= 5 and r['vm0_blocked_during_key_wait'] and
                   r['key_read'])
    return r


def part_io(vm, h, out):
    r = {}
    offset = vm.offset()
    h.typed('run \\VMMIO.COM')
    text = wait_result(vm, h, offset, 'VMMIO', 240)
    io = [l.strip() for l in text.splitlines() if l.startswith('[IO') or l.startswith('[VMMIO]')]
    r['lines'] = io[-12:]
    tags = [l[:6] for l in io if l.startswith('[IO A]') or l.startswith('[IO B]')]
    r['alternations'] = sum(1 for a, b in zip(tags, tags[1:]) if a != b)
    r['probe_pass'] = '[VMMIO] PASS' in text
    # Files and FAT are checked from the host after shutdown (host_io_check).
    r['passed'] = r['probe_pass'] and r['alternations'] >= 10
    return r


def host_io_check(disk, volume, out):
    """The files VMMIO wrote from both VMs, and the FAT, read from the host."""
    r = {}
    intact = True
    for letter in 'AB':
        target = out / f'IOTEST.{letter}'
        subprocess.run(['mcopy', '-o', '-i', volume, f'::IOTEST.{letter}', str(target)], check=True)
        data = target.read_bytes()
        expected = bytes((i * 7 + j + ord(letter)) & 0xFF for i in range(40) for j in range(1000))
        intact = intact and data == expected
        r[f'file_{letter}'] = len(data)
    r['files_intact'] = intact
    start = FAT16(disk).start
    image = disk
    if start:
        image = out / 'volume.img'
        with open(disk, 'rb') as src, open(image, 'wb') as dst:
            src.seek(start)
            shutil.copyfileobj(src, dst)
    fsck = subprocess.run(['fsck.fat', '-n', str(image)], capture_output=True, text=True)
    r['fsck'] = (fsck.stdout + fsck.stderr).strip().splitlines()[-6:]
    r['fat_clean'] = fsck.returncode == 0
    return r


def part_window(vm, h, out):
    r = {}
    offset = vm.offset()
    h.typed('run \\VMWTEST.COM')
    vm.wait('[VMWTEST] focus VM1', offset, 90)
    for key in 'abc':
        vm.key(key)
        time.sleep(0.3)
    vm.wait('[VMWTEST] press the hotkey', offset, 60)
    vm.key('ctrl-esc')
    vm.wait('[VMWTEST] focus VM0', offset, 60)
    # The console runs programs full screen in text mode (VM 0's own mode
    # set); VM 1's "VM1 SCREEN" on row 0 must stay in its virtual VGA.
    during = Image.open(h.shot('window-during')).convert('L')
    row0 = np.asarray(during.crop((0, 0, min(during.width, 160), 16)))
    lit = int((row0 > 40).sum())
    vm.key('x')
    text = wait_result(vm, h, offset, 'VMWTEST', 90)
    r['lines'] = [l.strip() for l in text.splitlines() if l.startswith('[VMW')][-16:]
    r['probe_pass'] = '[VMWTEST] PASS' in text
    keys = re.findall(r'\[VMW1\] key ([0-9A-F]{4})', text)
    r['vm1_keys'] = keys
    # a, b, c typed while VM 1 had the focus; Esc from DEV_KEY; no x.
    r['vm1_keys_exact'] = [k[2:] for k in keys] == ['61', '62', '63', '1B']
    r['vm1_text_on_physical_screen_pixels'] = lit
    r['passed'] = r['probe_pass'] and r['vm1_keys_exact'] and lit == 0
    return r


def square_wave_check(pcm, band=(380, 500), min_seconds=3):
    """Left channel of 16-bit stereo PCM: is the loud part a clean square wave?"""
    samples = np.frombuffer(pcm[:len(pcm) // 4 * 4], dtype='<i2').reshape(-1, 2)[:, 0].astype(float)
    loud = np.abs(samples) > 1500
    r = {'samples': int(len(samples)), 'loud_samples': int(loud.sum())}
    if loud.sum() < 44100 * min_seconds / 3:
        r['clean'] = False
        return r
    first, last = np.argmax(loud), len(loud) - np.argmax(loud[::-1])
    body = samples[first:last]
    level = float(np.percentile(np.abs(body), 90))
    on_level = np.abs(np.abs(body) - level) <= 0.25 * level
    crossings = int(np.count_nonzero(np.diff(np.sign(body[np.abs(body) > 0.5 * level])) != 0))
    seconds = len(body) / 44100
    r.update(level=level, seconds=round(seconds, 2), on_level=round(float(on_level.mean()), 3),
             crossings_per_second=round(crossings / seconds, 1))
    # 220 Hz: 440 crossings per second. Garbage from another VM's memory would
    # spread the levels and add crossings.
    r['clean'] = (r['on_level'] >= 0.85 and band[0] <= r['crossings_per_second'] <= band[1] and
                  seconds >= min_seconds)
    return r


def part_sound(vm, h, out):
    r = {}
    wav = out / 'audio.wav'
    start = wav.stat().st_size
    offset = vm.offset()
    h.typed('run \\VMSTEST.COM')
    text = wait_result(vm, h, offset, 'VMSTEST', 180)
    end = wav.stat().st_size
    r['lines'] = [l.strip() for l in text.splitlines() if l.startswith('[VMS')
                  and not l.startswith('[VMS0]')][-10:]
    r['vm0_busy_lines'] = text.count('[VMS0]')
    r['probe_pass'] = '[VMSTEST] PASS' in text and '[VMS1] played' in text
    # The WAV header is final only after shutdown: judged in host_sound_check.
    r['wav_window'] = [start, end]
    r['passed'] = r['probe_pass'] and r['vm0_busy_lines'] >= 20
    return r


def host_sound_check(out, window):
    raw = (out / 'audio.wav').read_bytes()
    pcm, bits = pcm_payload(raw)
    header = len(raw) - len(pcm)
    lo = max(window[0] - header, 0) // 4 * 4
    hi = max(window[1] - header, 0) // 4 * 4
    (out / 'sound-part.pcm').write_bytes(pcm[lo:hi])
    return square_wave_check(pcm[lo:hi]) if bits == 16 else {'clean': False, 'bits': bits}


def part_kill(vm, h, out):
    r = {}
    offset = vm.offset()
    h.typed('run \\VMKTEST.COM')
    # One prompt per case (E, D, H): z must reach the system VM unchanged
    # after each teardown.
    for case in 'EDH':
        vm.wait(f'[VMKTEST] press z {case}', offset, 180)
        vm.key('z')
    text = wait_result(vm, h, offset, 'VMKTEST', 60)
    r['lines'] = [l.strip() for l in text.splitlines() if l.startswith('[VMK')][-16:]
    r['keys_ok'] = text.count('[VMKTEST] key z')
    r['probe_pass'] = '[VMKTEST] PASS' in text
    r['passed'] = r['probe_pass'] and r['keys_ok'] == 3
    return r


def part_ivt(vm, h, out):
    r = {}
    offset = vm.offset()
    h.typed('run \\VMITEST.COM')
    text = wait_result(vm, h, offset, 'VMITEST', 180)
    r['lines'] = [l.strip() for l in text.splitlines() if l.startswith('[VMI')][-8:]
    r['probe_pass'] = '[VMITEST] PASS' in text
    r['vm1_ok'] = '[VMI1] 40 ticks over overwritten memory' in text
    r['passed'] = r['probe_pass'] and r['vm1_ok']
    return r


def part_clock(vm, h, out):
    r = {}
    offset = vm.offset()
    h.typed('run \\VMCTEST.COM')
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        text = h.serial(offset)
        if '[VMCTEST] done' in text or '[VMCTEST] FAIL' in text:
            break
        time.sleep(1)
    text = h.serial(offset)
    r['lines'] = [l.strip() for l in text.splitlines() if l.startswith('[VMC')][-6:]
    ticks = {vm_: int(m, 16) for vm_, m in re.findall(r'\[VMC([01])\] ticks ([0-9A-F]{4})', text)}
    r['ticks'] = ticks
    r['probe_done'] = '[VMCTEST] done' in text
    r['passed'] = r['probe_done'] and len(ticks) == 2 and all(165 <= t <= 200 for t in ticks.values())
    return r


def part_multi(vm, h, out):
    r = {}
    offset = vm.offset()
    h.typed('run \\VMDTEST.COM')
    for marker, key in (('[VMDTEST] focus VM1', 'a'), ('[VMDTEST] focus VM2', 'b'),
                        ('[VMDTEST] press the hotkey', 'ctrl-esc')):
        vm.wait(marker, offset, 120)
        vm.key(key)
    vm.wait('[VMDTEST] focus VM0', offset, 60)
    shot = Image.open(h.shot('multi-during')).convert('L')
    row0 = np.asarray(shot.crop((0, 0, min(shot.width, 160), 16)))
    lit = int((row0 > 40).sum())
    vm.key('x')
    text = wait_result(vm, h, offset, 'VMDTEST', 90)
    r['lines'] = [l.strip() for l in text.splitlines() if l.startswith('[VMD') or l.startswith('[VMW')][-20:]
    keys = {n: [k[2:] for k in re.findall(r'\[VMW%d\] key ([0-9A-F]{4})' % n, text)] for n in (1, 2)}
    r['keys'] = keys
    r['keys_exact'] = keys == {1: ['61', '1B'], 2: ['62', '1B']}
    r['text_on_physical_screen_pixels'] = lit
    r['probe_pass'] = '[VMDTEST] PASS' in text
    r['passed'] = r['probe_pass'] and r['keys_exact'] and lit == 0
    return r


def part_mouse(vm, h, out):
    r = {}
    offset = vm.offset()
    h.typed('run \\VMOTEST.COM')
    # Physical motion until the probe saw it in the focused VM.
    for marker, done, motion in (('[VMOTEST] focus VM1, move right', '[VMOTEST] right reached', '12 0 0'),
                                 ('[VMOTEST] focus VM2, move down', '[VMOTEST] down reached', '0 12 0')):
        vm.wait(marker, offset, 120)
        deadline = time.monotonic() + 30
        moves = 0
        while time.monotonic() < deadline:
            text = h.serial(offset)
            if done in text or '[VMOTEST] FAIL' in text:
                break
            vm.hmp('mouse_move ' + motion)
            moves += 1
            time.sleep(0.5)
        r.setdefault('moves', []).append(moves)
    text = wait_result(vm, h, offset, 'VMOTEST', 90)
    shot = Image.open(h.shot('mouse-after')).convert('L')
    row0 = np.asarray(shot.crop((0, 0, min(shot.width, 160), 16)))
    lit = int((row0 > 40).sum())
    r['lines'] = [l.strip() for l in text.splitlines() if l.startswith('[VMO')][-24:]
    packets = {n: [tuple(int(v, 16) for v in m) for m in
                   re.findall(r'\[VMO%d\] mouse ([0-9A-F]{4}) ([0-9A-F]{4}) ([0-9A-F]{4}) ([0-9A-F]{4})' % n, text)]
               for n in (1, 2)}
    r['vm1_last'] = packets[1][-1] if packets[1] else None
    r['vm2_last'] = packets[2][-1] if packets[2] else None
    r['text_on_physical_screen_pixels'] = lit
    r['probe_pass'] = '[VMOTEST] PASS' in text
    r['both_ended'] = text.count('session ended') >= 2
    r['passed'] = r['probe_pass'] and r['both_ended'] and lit == 0
    return r


AUDIO_STATES = (('[VMATEST] owner VM1 (focus VM1)', 1), ('[VMATEST] owner VM2 (focus VM2)', 2),
                ('[VMATEST] owner VM1 (focus VM0)', 1), ('[VMATEST] owner VM2 (VM1 ended)', 2))
AUDIO_EVENTS = ('[VMS1] playing', '[VMS2] playing', '[VMATEST] both playing') + \
    tuple(m for m, _ in AUDIO_STATES) + ('[VMS1] played', '[VMS2] played', '[VMATEST] PASS', '[VMATEST] FAIL')


def part_audio(vm, h, out):
    r = {}
    wav = out / 'audio.wav'
    offset = vm.offset()
    h.typed('run \\VMATEST.COM')
    # The WAV position at which each event appeared on COM1.
    at = {}
    deadline = time.monotonic() + 240
    while time.monotonic() < deadline:
        text = h.serial(offset)
        size = wav.stat().st_size
        for event in AUDIO_EVENTS:
            if event not in at and event in text:
                at[event] = size
        if '[VMATEST] PASS' in at or '[VMATEST] FAIL' in at:
            break
        time.sleep(0.05)
    time.sleep(1)
    text = h.serial(offset)
    r['lines'] = [l.strip() for l in text.splitlines() if l.startswith('[VMA') or l.startswith('[VMS')][-16:]
    r['events'] = at
    blocks = {int(n): int(b, 16) for n, b in re.findall(r'\[VMS([12])\] played ([0-9A-F]{4}) blocks', text)}
    r['blocks'] = blocks
    r['probe_pass'] = '[VMATEST] PASS' in text
    r['passed'] = r['probe_pass'] and len(blocks) == 2
    return r


def host_audio_check(out, record):
    """Each state's window: the owner's wave only; blocks in real time."""
    raw = (out / 'audio.wav').read_bytes()
    pcm, bits = pcm_payload(raw)
    header = len(raw) - len(pcm)
    at = record['events']
    result = {'bits': bits, 'states': []}
    if bits != 16:
        result['clean'] = False
        return result
    rate = 44100 * 4
    ends = [m for m, _ in AUDIO_STATES[1:]] + ['[VMS2] played']
    ok = True
    for (marker, owner), end in zip(AUDIO_STATES, ends):
        if marker not in at or end not in at:
            ok = False
            result['states'].append({'state': marker.strip(), 'missing': True})
            continue
        lo = (at[marker] - header + int(0.8 * rate)) // 4 * 4
        hi = (at[end] - header - int(0.3 * rate)) // 4 * 4
        check = square_wave_check(pcm[lo:hi], (380, 500) if owner == 1 else (1000, 1200), 1.5)
        check.update(state=marker.strip(), owner=owner)
        (out / f'audio-state-{len(result["states"]) + 1}.pcm').write_bytes(pcm[lo:hi])
        result['states'].append(check)
        ok = ok and check['clean']
    # Blocks of 4000 samples at 11025 Hz while each VM played.
    block_seconds = 4000 / 11025
    realtime = {}
    for n in (1, 2):
        start, end = at.get(f'[VMS{n}] playing'), at.get(f'[VMS{n}] played')
        if start is None or end is None or n not in record['blocks']:
            ok = False
            continue
        expected = (end - start) / rate / block_seconds
        ratio = record['blocks'][n] / expected if expected else 0
        realtime[n] = {'blocks': record['blocks'][n], 'expected': round(expected, 1), 'ratio': round(ratio, 3)}
        ok = ok and 0.9 <= ratio <= 1.1
    result['realtime'] = realtime
    result['clean'] = ok
    return result


RUN = {'audio': part_audio, 'mouse': part_mouse, 'multi': part_multi, 'clock': part_clock, 'ivt': part_ivt, 'switch': part_switch, 'io': part_io, 'window': part_window, 'sound': part_sound,
       'kill': part_kill}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    parser.add_argument('--jemm', type=Path, required=True)
    parser.add_argument('--jload', type=Path, required=True)
    parser.add_argument('--module', type=Path, required=True)
    parser.add_argument('--probes', type=Path, required=True, help='directory with the probes and VMFORK.COM')
    parser.add_argument('--parts', default=','.join(PARTS))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    parts = [p for p in args.parts.split(',') if p]
    assert all(p in RUN for p in parts), parts
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    disk = out / 'disk.img'
    shutil.copyfile(args.image, disk)
    volume = f'{disk}@@{FAT16(disk).start}'
    copies = [(args.jemm, 'VM/JEMM386.EXE'), (args.jload, 'VM/JLOAD.EXE'),
              (args.module, 'VM/CVSESS.DLL'), (args.probes / 'VMFORK.COM', 'VM/VMFORK.COM')]
    copies += [(args.probes / name, name) for p in parts for name in PROBES[p]]
    for source, target in copies:
        subprocess.run(['mcopy', '-o', '-i', volume, str(source), '::' + target], check=True)
    record = dict(passed=False, physical_hardware_qualified=False, parts=parts)
    vm = WindowVM(disk, out, 'std', boot_capture=True, memory=128, palette='platinum')
    h = Harness(vm, out)
    try:
        vm.ready()
        record['vm_started_at_boot'] = 'VMSTART: ready.' in h.serial(0)
        vm.key('f4')
        vm.wait('CiukiOS SHELL C:\\APPS>')
        for part in parts:
            record[part] = RUN[part](vm, h, out)
            record[part]['console_alive_after'] = console_alive(vm, h)
        h.shot('after')
    except Exception as error:
        record['error'] = repr(error)
        try:
            h.shot('failure')
            record['serial_tail'] = h.serial(0)[-3000:]
            record['registers'] = vm.hmp('info registers').decode('utf-8', 'replace')[-1500:]
        except Exception as diagnostic:
            record['diagnostic_error'] = repr(diagnostic)
    finally:
        vm.close()
    if 'io' in record and record['io'].get('probe_pass'):
        record['io'].update(host_io_check(disk, volume, out))
        record['io']['passed'] = (record['io']['passed'] and record['io']['files_intact'] and
                                  record['io']['fat_clean'])
    if 'sound' in record and 'wav_window' in record['sound']:
        record['sound']['wave'] = host_sound_check(out, record['sound']['wav_window'])
        record['sound']['passed'] = record['sound']['passed'] and record['sound']['wave']['clean']
    if 'audio' in record and 'events' in record['audio']:
        record['audio']['sound'] = host_audio_check(out, record['audio'])
        record['audio']['passed'] = record['audio']['passed'] and record['audio']['sound']['clean']
    record['passed'] = bool(record.get('vm_started_at_boot') and 'error' not in record and
                            all(record.get(p, {}).get('passed') and
                                record.get(p, {}).get('console_alive_after') for p in parts))
    (out / 'report.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({'passed': record['passed'], 'error': record.get('error'),
                      **{p: record.get(p, {}).get('passed') for p in parts}}, indent=2))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
