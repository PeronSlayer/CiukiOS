#!/usr/bin/env python3
"""End-to-end music player test with QEMU AC'97 capture (run only when coordinated).

Builds 8-second WAV/MP3/Ogg/FLAC tracks in a private HDD image, opens them via
the real Files association, exercises controls, then checks guest transition
logs and captured PCM. This script does not build the image.
"""
import argparse
import json
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import uuid
import wave
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from analyze_audio_wav import pcm_payload  # noqa: E402
from qemu_test_installed_hdd import FAT16, listing_address  # noqa: E402
from qemu_test_native_windows import WindowVM  # noqa: E402


def make_tracks(directory):
    directory.mkdir(parents=True, exist_ok=True)
    rate, seconds = 48000, 8
    t = np.arange(rate * seconds, dtype=np.float64) / rate
    tone = 0.35 * np.sin(2 * np.pi * (330 * t + 14 * t * t))
    stereo = np.column_stack((tone, 0.8 * tone)).clip(-0.98, 0.98)
    pcm = np.rint(stereo * 32767).astype('<i2').tobytes()
    source = directory / 'A.WAV'
    with wave.open(str(source), 'wb') as f:
        f.setnchannels(2); f.setsampwidth(2); f.setframerate(rate); f.writeframes(pcm)
    conversions = (
        ('B.MP3', ['-codec:a', 'libmp3lame', '-b:a', '128k']),
        ('C.OGG', ['-codec:a', 'libvorbis', '-q:a', '4']),
        ('D.FLAC', ['-codec:a', 'flac']),
    )
    if not shutil.which('ffmpeg'):
        raise RuntimeError('ffmpeg with libmp3lame, libvorbis and FLAC encoders is required')
    for name, codec in conversions:
        subprocess.run(['ffmpeg', '-y', '-hide_banner', '-loglevel', 'error', '-i', str(source),
                        '-map_metadata', '-1', *codec, str(directory / name)], check=True)
    return [source, *(directory / name for name, _ in conversions)]


def wait(vm, marker, offset=None, timeout=15):
    vm.wait(marker, vm.offset() if offset is None else offset, timeout)


def open_audio_folder(vm):
    offset = vm.offset()
    vm.key('meta_l-r')
    vm.text('explorer c:\\qa\\audio')
    wait(vm, '[FILES] list C:\\QA\\AUDIO 4', offset, 25)


def open_track(vm, filename):
    stem = filename[0].lower()
    offset = vm.offset()
    vm.key(stem)
    vm.key('ret')
    wait(vm, f'[PLAYER] open path C:\\QA\\AUDIO\\{filename}', offset, 25)
    wait(vm, '[PLAYER] state Playing', offset, 25)


def player_rect(vm):
    x, y, width = vm.active_rect()
    return x, y, width


def count_nonzero_pcm_bytes(pcm):
    """Count nonzero bytes, rather than NumPy's truth value for `bytes`."""
    return int(np.count_nonzero(np.frombuffer(pcm, dtype=np.uint8)))


def click_control(vm, xoff, yoff=166):
    x, y, _ = player_rect(vm)
    vm.completed_control_click(x + xoff, y + yoff)


def transition_next(vm, filename, state='End'):
    offset = vm.offset()
    click_control(vm, 259)
    wait(vm, f'[PLAYER] open path C:\\QA\\AUDIO\\{filename}', offset, 25)
    wait(vm, '[PLAYER] state Playing', offset, 25)
    end_offset = vm.offset()
    wait(vm, f'[PLAYER] state {state}', end_offset, 15)


def wait_player_closed(vm, output, shell_listing, shell_physical, timeout=15):
    """A close log is a request; confirm destruction and CAPP release."""
    def array_address(name):
        for line in shell_listing:
            if re.search(r'\b' + re.escape(name) + r'\s+times\b', line):
                address = re.match(r'\s*\d+\s+([0-9A-F]{8})\s', line)
                if address:
                    return int(address[1], 16)
        raise AssertionError(f'Shell listing has no array {name}')
    flags = shell_physical + array_address('ui_window_flags') + 20
    segment = shell_physical + array_address('app_segs') + 26
    base, length = min(flags, segment), abs(flags - segment) + 2
    target = output / 'player-close-state.bin'
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        vm.hmp(f'pmemsave {base} {length} "{target}"')
        data = target.read_bytes()
        if len(data) == length and data[flags - base] == 0 and not any(data[segment - base:segment - base + 2]):
            return {'window_flags': 0, 'module_segment': 0}
        time.sleep(.1)
    raise AssertionError('Player close request did not destroy window 20 and release its module')


def chirp_evidence(pcm, sample_rate):
    """Find the generated WAV's rising 330-500 Hz sweep in captured PCM.

    Boot and desktop cues are short tones. Requiring a sustained, increasing
    dominant frequency distinguishes actual music samples from non-silent UI
    audio in the same QEMU capture.
    """
    samples = np.frombuffer(pcm, dtype='<i2')
    if samples.size < 2:
        return None
    mono = samples[0::2].astype(np.float64)
    window = max(1, int(sample_rate * .2))
    frequencies = []
    for offset in range(0, len(mono) - window + 1, window):
        part = mono[offset:offset + window]
        if np.sqrt(np.mean(part * part)) < 1800:
            frequencies.append(None)
            continue
        spectrum = np.abs(np.fft.rfft(part * np.hanning(window)))
        bins = np.fft.rfftfreq(window, 1 / sample_rate)
        lo, hi = np.searchsorted(bins, 280), np.searchsorted(bins, 600)
        peak = lo + int(np.argmax(spectrum[lo:hi]))
        frequencies.append(float(bins[peak]))
    for start, first in enumerate(frequencies):
        if first is None or not 300 <= first <= 390:
            continue
        last = first
        for end in range(start + 1, len(frequencies)):
            current = frequencies[end]
            if current is None or current + 8 < last or current > 590:
                break
            last = current
            if end - start >= 7 and last - first >= 30:
                return {'first_hz': round(first), 'last_hz': round(last),
                        'windows': end - start + 1,
                        'start_seconds': round(start * window / sample_rate, 2)}
    return None


def input_snapshot(vm, output, name, shell_image, shell_listing):
    """Keep bounded VM/PIC/INT 33h evidence around a worker VM exit."""
    pointer = None
    try:
        pointer = vm.pointer()
    except Exception as error:
        pointer = {'error': repr(error)}
    registers = vm.hmp('info registers').decode('utf-8', 'replace')
    pic = vm.hmp('info pic').decode('utf-8', 'replace')
    irq = vm.hmp('info irq').decode('utf-8', 'replace')
    ram_path = output / f'{name}-ram8m.bin'
    vm.hmp(f'pmemsave 0 0x800000 "{ram_path}"')
    ram = ram_path.read_bytes()
    int33 = {'offset': int.from_bytes(ram[0xCC:0xCE], 'little'),
             'segment': int.from_bytes(ram[0xCE:0xD0], 'little')}
    fields = ('ui_mouse_ready', 'ui_pointer_visible', 'ui_mouse_x', 'ui_mouse_y',
              'ui_mouse_buttons', 'ui_mouse_press_pending',
              'ui_mouse_release_pending', 'ui_button_down', 'ui_active_window')
    candidates = []
    needle = shell_image[:64]
    pos = 0
    while needle and (pos := ram.find(needle, pos)) >= 0:
        values = {}
        for field in fields:
            try:
                address = pos + listing_address(shell_listing, field)
                size = 2 if field in ('ui_mouse_x', 'ui_mouse_y', 'ui_mouse_buttons') else 1
                values[field] = int.from_bytes(ram[address:address + size], 'little')
            except (AssertionError, IndexError):
                values[field] = None
        canary = None
        if shell_image[:6] == bytes.fromhex('fa8cc88ed0bc') and shell_image[8:11] == bytes.fromhex('2ec706'):
            stack_offset = int.from_bytes(shell_image[11:13], 'little')
            address = pos - 0x100 + stack_offset
            canary = int.from_bytes(ram[address:address + 2], 'little')
        candidate = {'physical_code_match': pos, 'mouse_fields': values,
                     'stack_canary': canary}
        try:
            trace_symbol = listing_address(shell_listing, 'app_return_trace_magic') - 0x100
            trace_at = pos + trace_symbol
            if ram[trace_at:trace_at + 8] == b'CAPPRET1':
                next_index, count, sequence, frozen = struct.unpack_from('<4I', ram, trace_at + 8)
                if next_index < 16 and count <= 16:
                    records = []
                    oldest = (next_index - count) % 16
                    for age in range(count):
                        ring_index = (oldest + age) % 16
                        raw = struct.unpack_from('<10H', ram, trace_at + 24 + ring_index * 20)
                        records.append({
                            'ring_index': ring_index, 'sequence': raw[0],
                            'module_ss': raw[1], 'module_sp': raw[2],
                            'app_caller': raw[3], 'return_ip': raw[4],
                            'return_cs': raw[5], 'service': raw[6],
                            'slot': raw[7], 'shell_ss': raw[8],
                            'shell_sp': raw[9], 'mismatch': raw[5] != raw[3],
                        })
                    candidate['app_return_trace'] = {
                        'physical_address': trace_at, 'next_index': next_index,
                        'count': count, 'sequence': sequence,
                        'frozen': frozen, 'records_oldest_first': records,
                    }
        except (AssertionError, struct.error):
            pass
        candidates.append(candidate)
        pos += 1
    result = {'pointer': pointer, 'int33_ivt_physical_zero': int33,
              'shell_candidates': candidates, 'ram_dump': ram_path.name,
              'ram_dump_bytes': len(ram), 'registers': registers,
              'pic': pic, 'irq': irq}
    serial_path = getattr(vm, 'serial', None)
    if serial_path is not None and serial_path.exists():
        serial = serial_path.read_text(errors='replace').splitlines()
        player_lines = [line for line in serial if '[PLAYER]' in line]
        worker_lines = [line for line in serial if '[DPMIRUN]' in line]
        # Preserve the player and worker lifecycle leading into the snapshot;
        # this exposes whether the parent polled a completed decode/EOF after
        # the child VM ended, without making the report unbounded.
        result['serial_tail'] = [line for line in serial[-500:]
                                 if '[PLAYER]' in line or '[DPMIRUN]' in line][-120:]
        result['poll_state'] = {
            'last_player_state': next((line for line in reversed(player_lines)
                                       if '[PLAYER] state ' in line), None),
            'last_player_progress': next((line for line in reversed(player_lines)
                                          if '[PLAYER] progress ' in line), None),
            'worker_sessions': sum('[DPMIRUN] VM SESSION' in line for line in worker_lines),
            'worker_ends': sum('[DPMIRUN] VM END' in line for line in worker_lines),
            'worker_idle_yields': sum('[DPMIRUN] background yields' in line
                                      for line in worker_lines),
        }
        result['serial_bytes'] = serial_path.stat().st_size
    (output / f'{name}-input.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def return_trace_files(output, socket_path):
    """Write the opt-in GDB batch script and its evidence paths.

    GDB documents `target remote /path/to/socket` for local Unix sockets and
    conditional watchpoints; see https://sourceware.org/gdb/current/onlinedocs/gdb.html/Connecting.html
    and https://sourceware.org/gdb/current/onlinedocs/gdb.html/Set-Watchpoints.html.
    """
    output = Path(output)
    gdb_log = output / 'return-watch.gdb.log'
    gdb_stderr = output / 'return-watch.gdb.stderr.log'
    ram_dump = output / 'return-watch-ram8m.bin'
    script_path = output / 'return-watch.gdb'

    def gdb_quote(path):
        return str(path.resolve()).replace('\\', '\\\\').replace('"', '\\"')

    script = f'''set pagination off
set confirm off
set can-use-hw-watchpoints 1
set architecture i386
target remote {socket_path}
hbreak *0x325f5
commands 1
  silent
  printf "\\n[RETURN-WATCH] entered the observed invalid far target at 0x325f5\\n"
  info registers
  x/12i $eip-8
  x/24wx 0x20d30
  monitor info registers
  monitor pmemsave 0 0x800000 "{gdb_quote(ram_dump)}"
  disable 1
  detach
  quit
end
continue
'''
    script_path.write_text(script)
    return {'socket': str(socket_path), 'script': script_path.name,
            'stdout': gdb_log.name, 'stderr': gdb_stderr.name,
            'ram_dump': ram_dump.name}, script_path, gdb_log, gdb_stderr


def start_return_trace(output, socket_path):
    deadline = time.monotonic() + 5
    while not Path(socket_path).exists():
        if time.monotonic() >= deadline:
            raise TimeoutError(f'QEMU GDB socket did not appear: {socket_path}')
        time.sleep(.05)
    paths, script_path, stdout_path, stderr_path = return_trace_files(output, socket_path)
    stdout = stdout_path.open('w')
    stderr = stderr_path.open('w')
    try:
        process = subprocess.Popen(['gdb', '--quiet', '--batch', '--command', str(script_path)],
                                   stdout=stdout, stderr=stderr)
    except Exception:
        stdout.close()
        stderr.close()
        raise
    return process, (stdout, stderr), paths


def stop_return_trace(process, streams):
    if process is not None and process.poll() is None:
        deadline = time.monotonic() + 10
        process.terminate()
        try:
            process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=max(0, deadline - time.monotonic()))
    for stream in streams or ():
        if not stream.closed:
            stream.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--image', type=Path, default=ROOT / 'build/full/ciukios-full.img')
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--trace-return', action='store_true',
                    help='attach GDB to trap the observed invalid far target at 0x325f5')
    ap.add_argument('--app-return-trace', action='store_true',
                    help='install a private shell build that records CAPP service far-return frames')
    ap.add_argument('--capture-active-host', action='store_true',
                    help='capture a bounded RAM/CPU snapshot while the first decoder host is installed')
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    disk = out / 'disk.img'
    shutil.copyfile(args.image, disk)
    volume = f'{disk}@@{FAT16(disk).start}'
    subprocess.run(['mmd', '-i', volume, '::QA', '::QA/AUDIO'], check=True)
    fixture_dir = out / 'tracks'
    tracks = make_tracks(fixture_dir)
    for track in tracks:
        subprocess.run(['mcopy', '-o', '-i', volume, str(track), '::QA/AUDIO/' + track.name], check=True)
    shell_path = out / 'shell-image.bin'
    subprocess.run(['mcopy', '-o', '-i', volume, '::SYSTEM/SHELL.COM', str(shell_path)], check=True)
    shell_image = shell_path.read_bytes()
    shell_path.unlink()
    shell_listing_path = out / 'shell-current.lst'
    shell_current = out / 'shell-current.bin'
    shell_command = ['nasm', '-f', 'bin']
    if args.app_return_trace:
        shell_command.append('-DAPP_RETURN_TRACE=1')
    shell_command.extend([str(ROOT / 'src/com/shell.asm'), '-l', str(shell_listing_path),
                          '-o', str(shell_current)])
    subprocess.run(shell_command, cwd=ROOT, check=True)
    if args.app_return_trace:
        shell_image = shell_current.read_bytes()
        subprocess.run(['mcopy', '-o', '-i', volume, str(shell_current),
                        '::SYSTEM/SHELL.COM'], check=True)
    else:
        assert shell_current.read_bytes() == shell_image, 'installed shell does not match diagnostic symbols'
    shell_listing = shell_listing_path.read_text(errors='replace').splitlines()
    shell_current.unlink()

    report = {'passed': False, 'physical_hardware_qualified': False,
              'app_return_trace_enabled': args.app_return_trace,
              'tracks_seconds': 8, 'formats': ['WAV', 'MP3', 'Ogg Vorbis', 'FLAC'],
              'actions': [], 'audio_capture': None}
    gdb_socket = Path(tempfile.gettempdir()) / f'ciukios-music-return-{uuid.uuid4().hex}.sock'
    extra_qemu_args = ('-gdb', f'unix:{gdb_socket},server=on,wait=off') if args.trace_return else ()
    vm = WindowVM(disk, out, 'std', boot_capture=True, memory=128, palette='platinum',
                  extra_qemu_args=extra_qemu_args)
    trace_process = None
    trace_streams = ()
    stage = 'boot'
    try:
        vm.ready()
        stage = 'open audio folder'
        open_audio_folder(vm)
        report['input_before_worker'] = input_snapshot(vm, out, 'before-worker',
                                                       shell_image, shell_listing)
        stage = 'open WAV and start decoder worker'
        open_track(vm, 'A.WAV')
        report['actions'].append('Files association opened WAV; playback started')
        vm.shot('player-wav-playing')
        if args.capture_active_host:
            report['input_active_host'] = input_snapshot(
                vm, out, 'active-host', shell_image, shell_listing)

        stage = 'pause and resume WAV'
        offset = vm.offset()
        click_control(vm, 122)
        wait(vm, '[PLAYER] state Paused', offset)
        time.sleep(1.1)
        stage = 'resume WAV'
        offset = vm.offset()
        click_control(vm, 122)
        wait(vm, '[PLAYER] state Playing', offset)
        report['actions'].append('pause and resume')

        stage = 'change volume and seek WAV'
        offset = vm.offset()
        click_control(vm, 506)
        vm.wait('[PLAYER] volume level=', offset, 5)
        x, y, width = player_rect(vm)
        offset = vm.offset()
        vm.completed_control_click(x + 24 + int((width - 56) * 0.78), y + 202)
        wait(vm, '[PLAYER] seek frame=', offset, 5)
        wait(vm, '[PLAYER] state End', offset, 8)
        report['actions'].append('volume change, seek, WAV EOF')

        if args.trace_return:
            # Trap the observed invalid target before Jemm's exception output
            # alters the client stack. Unlike a conditional write watch, this
            # does not stop on every ordinary shell-stack write.
            ordinary_wait = vm.wait
            def traced_wait(pattern, offset=0, timeout=30):
                return ordinary_wait(pattern, offset, min(timeout * 5, 90))
            vm.wait = traced_wait
            trace_process, trace_streams, trace_paths = start_return_trace(out, gdb_socket)
            report['return_trace'] = trace_paths
            report['return_trace']['armed_after'] = 'WAV EOF'
            report['return_trace']['intrusive_debugger'] = True

        stage = 'Next to MP3 and EOF'
        transition_next(vm, 'B.MP3')
        report['actions'].append('MP3 streamed through EOF (longer than the 64 KiB legacy limit)')
        stage = 'Next to Ogg and EOF'
        transition_next(vm, 'C.OGG')
        report['actions'].append('Ogg Vorbis streamed through EOF')
        stage = 'Next to FLAC and EOF'
        transition_next(vm, 'D.FLAC')
        report['actions'].append('FLAC streamed through EOF')

        stage = 'Previous to Ogg'
        offset = vm.offset()
        click_control(vm, 47)
        wait(vm, '[PLAYER] open path C:\\QA\\AUDIO\\C.OGG', offset, 25)
        wait(vm, '[PLAYER] state Playing', offset, 25)
        report['actions'].append('previous track navigation')
        stage = 'close and reopen Ogg'
        offset = vm.offset()
        vm.key('alt-f4')
        wait(vm, '[DESKTOP] WINDOW 20 CLOSE', offset, 15)
        candidates = report['input_before_worker']['shell_candidates']
        assert len(candidates) == 1, 'ambiguous master shell location'
        report['player_close_confirmed'] = wait_player_closed(
            vm, out, shell_listing, candidates[0]['physical_code_match'])
        open_track(vm, 'C.OGG')
        report['actions'].append('close and reopen Ogg track')
        stage = 'stop playback'
        offset = vm.offset()
        click_control(vm, 197)
        wait(vm, '[PLAYER] state Stopped', offset, 5)
        report['actions'].append('stop')
        vm.shot('player-final')
        report['input_at_success'] = input_snapshot(
            vm, out, 'after-playback', shell_image, shell_listing)
    except Exception as error:
        report['error'] = repr(error)
        report['failed_stage'] = stage
        try:
            report['input_at_failure'] = input_snapshot(
                vm, out, f'failure-{re.sub(r"[^a-z0-9]+", "-", stage.lower()).strip("-")}',
                shell_image, shell_listing)
        except Exception as diagnostic_error:
            report['diagnostic_error'] = repr(diagnostic_error)
        try:
            vm.shot('failure')
        except Exception as screenshot_error:
            report['screenshot_error'] = repr(screenshot_error)
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        raise
    finally:
        try:
            vm.close()
        finally:
            stop_return_trace(trace_process, trace_streams)
            gdb_socket.unlink(missing_ok=True)

    audio = out / 'audio.wav'
    if audio.exists():
        capture_bytes = audio.read_bytes()
        pcm, bits = pcm_payload(capture_bytes)
        # The PCM payload is still valid when QEMU is stopped before its WAV
        # writer can backpatch RIFF/data lengths; the format header is present.
        capture_rate = int.from_bytes(capture_bytes[24:28], 'little')
        post_boot = pcm[capture_rate * 4 * 3:]
        nonzero_bytes = count_nonzero_pcm_bytes(pcm)
        post_boot_nonzero_bytes = count_nonzero_pcm_bytes(post_boot)
        report['audio_capture'] = {'bytes': audio.stat().st_size, 'bits': bits,
                                   'sample_rate': capture_rate, 'pcm_bytes': len(pcm),
                                   'nonzero_bytes': nonzero_bytes,
                                   'post_boot_nonzero_bytes': post_boot_nonzero_bytes}
        assert len(pcm) > 1024 * 1024, 'QEMU capture is too short to cover the music tracks'
        assert nonzero_bytes > 0, 'QEMU AC97 capture is silent'
        assert post_boot_nonzero_bytes > 0, 'QEMU AC97 capture has no non-silent playback after boot audio'
        chirp = chirp_evidence(pcm, capture_rate)
        assert chirp, 'capture contains no sustained rising WAV music chirp'
        report['music_chirp'] = chirp
    serial = (out / 'serial.log').read_text(errors='replace')
    for state in ('Playing', 'Paused', 'Stopped', 'End'):
        assert f'[PLAYER] state {state}' in serial, f'missing player state log {state}'
    assert '[PLAYER] state Error' not in serial, 'player reported a decoder/audio error'
    progress = [int(value) for value in re.findall(r'\[PLAYER\] progress played=(\d+)', serial)]
    assert progress and max(progress) > 16384, 'progress did not cross one 64 KiB stereo ring'
    report['maximum_played_frames'] = max(progress)
    for filename in ('A.WAV', 'B.MP3', 'C.OGG', 'D.FLAC'):
        marker = f'[PLAYER] open path C:\\QA\\AUDIO\\{filename}'
        start = serial.index(marker)
        following = serial.find('[PLAYER] open path ', start + len(marker))
        section = serial[start:] if following < 0 else serial[start:following]
        values = [int(value) for value in re.findall(r'\[PLAYER\] progress played=(\d+)', section)]
        assert values and max(values) > 16384, f'{filename} did not advance beyond one 64 KiB ring'
    report['per_track_progress'] = 'all four formats advanced past 16,384 stereo frames'
    report['serial_log'] = 'serial.log'
    report['passed'] = True
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    main()
