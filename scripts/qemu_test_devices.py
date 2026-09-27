#!/usr/bin/env python3
"""Qualify CVSESSION guest devices with the actual V86 probe DEVTEST.COM.

QEMU has an ICH AC'97 and no Sound Blaster or OPL. DEVTEST.COM programs the
8042, ISA DMA, SB16 DSP and OPL ports and hooks INT 9 / IRQ7 like a DOS game;
the session serves them from the peripheral model and streams PCM to the
AC'97. Keys are real QEMU keyboard events. The WAV the emulated AC'97 wrote is
analysed for the probe's 441 Hz square wave (SB DMA) and 440 Hz FM note (OPL).
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import struct
import time

import numpy as np

from qemu_test_vm_session import MonitorVM, FAT16


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tone_windows(path, fundamental=440.0, window_seconds=.25):
    """Classify 0.25 s windows that carry a tone on ~440 Hz.

    square: 440 Hz dominant and only odd harmonics (the probe's SB DMA wave);
    fm: 440 Hz among the three strongest components with clear even
    harmonics (the probe's OPL FM voice, whose 3rd harmonic may dominate).
    """
    # QEMU's wav backend leaves the RIFF/data sizes at zero; read the format
    # chunk and take everything after the 44-byte header as PCM.
    data = path.read_bytes()
    assert data[:4] == b'RIFF' and data[8:16] == b'WAVEfmt ', 'not a QEMU WAV capture'
    channels, rate = struct.unpack_from('<HI', data, 22)
    width = struct.unpack_from('<H', data, 34)[0] // 8
    raw = data[44:len(data) - (len(data) - 44) % (channels * width)]
    assert width == 2, 'expected 16-bit PCM'
    samples = np.frombuffer(raw, dtype='<i2').reshape(-1, channels).mean(axis=1)
    size = int(rate * window_seconds)
    frequencies = np.fft.rfftfreq(size, 1 / rate)
    windows = []

    def level(spectrum, hz):
        index = int(round(hz * size / rate))
        return float(spectrum[max(index - 2, 1):index + 3].max())
    for start in range(0, len(samples) - size, size):
        chunk = samples[start:start + size]
        rms = float(np.sqrt(np.mean((chunk - chunk.mean()) ** 2)))
        if rms < 300:
            continue
        spectrum = np.abs(np.fft.rfft(chunk * np.hanning(size)))
        top = np.argsort(spectrum[1:])[-3:][::-1] + 1
        peaks = [float(frequencies[i]) for i in top]
        if not any(abs(peak - fundamental) <= 10 for peak in peaks):
            continue
        strongest = float(spectrum[top[0]])
        even = max(level(spectrum, fundamental * k) for k in (2, 4, 6)) / strongest
        kind = ('square' if abs(peaks[0] - fundamental) <= 10 and even < .1 else
                'fm' if even >= .1 else 'other')
        windows.append(dict(second=round(start / rate, 2), rms=round(rms, 1),
                            peaks_hz=[round(p, 1) for p in peaks],
                            even_harmonics=round(even, 3), kind=kind))
    return rate, windows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('image', 'kernel', 'jemm', 'jload', 'module', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    disk = output / 'disk.img'
    source_hash = sha(args.image)
    shutil.copyfile(args.image, disk)
    volume = f'{disk}@@{FAT16(disk).start}'
    probe = output / 'DEVTEST.COM'
    subprocess.run(['nasm', '-f', 'bin', 'src/probes/vm/dev_test.asm', '-o', str(probe)], check=True)
    payloads = ((args.kernel, 'SYSTEM/CIUKIDOS.SYS'), (args.jemm, 'JEMM386.EXE'),
                (args.jload, 'JLOAD.EXE'), (args.module, 'CVSESS.DLL'), (probe, 'DEVTEST.COM'))
    for source, target in payloads:
        subprocess.run(['mcopy', '-o', '-i', volume, str(source), '::' + target], check=True)
    record = dict(passed=False, source_image_sha256=source_hash,
                  fixtures={target: sha(source) for source, target in payloads}, events=[],
                  physical_hardware=False, host_audio='QEMU ICH AC97 (no SB16/OPL device present)')
    vm = MonitorVM(disk, output, memory=128)
    record['qemu_command'] = vm.process.args

    def serial_after(offset):
        raw = subprocess.check_output(['scripts/serial_log_normalize.py', '--offset', str(offset),
                                       str(vm.serial)])
        return raw.decode('cp437', errors='replace')

    def command(text, *expected):
        body = vm.result(text, *expected, timeout=90)
        record['events'].append(dict(command=text, output=body))
        return body

    try:
        vm.wait('[DESKTOP] READY', timeout=90)
        vm.key('f4')
        vm.wait('CiukiOS SHELL C:\\APPS>')
        command(r'run \JEMM386.EXE LOAD NOEMS X=A000-FFFF NODYN MAX=32M MIN=32M NOVME',
                'Jemm386 loaded')
        command(r'run \JLOAD.EXE \CVSESS.DLL', 'loaded successfully')
        offset = vm.offset()
        vm.text(r'run \DEVTEST.COM')
        vm.wait('[DEVTEST] KEYS FOCUSED', offset, 60)
        time.sleep(.3)
        vm.key('a')
        time.sleep(.3)
        vm.key('b')
        vm.wait('[DEVTEST] KEYS UNFOCUSED', offset, 60)
        time.sleep(.3)
        vm.key('c')
        vm.wait('[DEVTEST] HOLD KEY', offset, 60)
        time.sleep(.3)
        vm.hmp('sendkey d 3000')
        vm.wait('[DEVTEST] OPL DONE', offset, 120)
        vm.wait('CiukiOS SHELL C:\\APPS>', offset, 60)
        body = serial_after(offset)
        record['events'].append(dict(command=r'run \DEVTEST.COM', output=body))
        assert '[DEVTEST] PASS' in body, body

        def bytes_after(label):
            match = re.search(re.escape(label) + r' ((?:[0-9A-F]{2} )*)', body)
            assert match, label
            return match.group(1).split()
        record['focused_bytes'] = bytes_after('FOCUSED BYTES')
        record['unfocused_bytes'] = bytes_after('UNFOCUSED BYTES')
        record['focus_loss_bytes'] = bytes_after('FOCUS-LOSS BYTES')
        assert record['focused_bytes'] == ['1E', '9E', '30', 'B0'], record['focused_bytes']
        assert record['unfocused_bytes'] == [], 'an unfocused key reached the guest'
        assert record['focus_loss_bytes'] == ['20', 'A0'], record['focus_loss_bytes']
        sb = re.search(r'SB DONE IRQS/VERSION ([0-9A-F]{4}) ([0-9A-F]{4})', body)
        assert sb and int(sb.group(1), 16) == 6, 'six SB DMA blocks with IRQ7 expected'
        record['sb_irqs'] = int(sb.group(1), 16)
        record['dsp_version'] = sb.group(2)
        assert sb.group(2) == '0405', 'SB16 DSP 4.05 expected'
        state = re.search(r'\[DEVTEST\] STATE ((?:[0-9A-F]{8} ){32})', body)
        assert state, 'missing device state'
        words = [int(w, 16) for w in state.group(1).split()]
        names = ['magic', 'version_bytes', 'active', 'caps', 'focused', 'audio',
                 'keys_forwarded', 'keys_dropped', 'aux_bytes', 'lazy_pulls',
                 'irqs_raised', 'irq_failures', 'port_reads', 'port_writes',
                 'unclaimed_io', 'model_errors', 'last_error', 'last_port',
                 'buffers_rendered', 'underruns', 'polls', 'sb_blocks',
                 'opl_writes', 'dsp_commands', 'ac97_nam', 'ac97_nabm',
                 'bridge_calls', 'rate']
        record['device_state'] = dict(zip(names, words))
        assert words[0] == 0x56445643 and words[5] == 1, 'AC97 output was not running'
        assert record['device_state']['irq_failures'] == 0
        command(r'run \JLOAD.EXE -u \CVSESS.DLL', 'unloaded successfully')
        command(r'run \JEMM386.EXE UNLOAD', 'Jemm unloaded')
        command('comdemo', 'COM demo via INT21h')
        record['passed'] = True
    except Exception as error:
        record['error'] = repr(error)
        try:
            record['registers_on_failure'] = vm.hmp('info registers').decode('utf-8', errors='replace')
            record['pic_on_failure'] = vm.hmp('info pic').decode('utf-8', errors='replace')
            vm.shot('failure')
        except Exception as diagnostic_error:
            record['diagnostic_error'] = repr(diagnostic_error)
    finally:
        vm.close()
    try:
        rate, windows = tone_windows(output / 'audio.wav')
        record['audio_wav_rate'] = rate
        record['tone_windows_440hz'] = windows
        square = [w for w in windows if w['kind'] == 'square']
        fm = [w for w in windows if w['kind'] == 'fm']
        record['square_wave_seconds'] = len(square) * .25
        record['fm_note_seconds'] = len(fm) * .25
        if record['passed']:
            assert len(square) >= 6, 'SB DMA square wave not heard'
            assert len(fm) >= 2, 'OPL note not heard'
    except Exception as error:
        record['passed'] = False
        record['audio_error'] = repr(error)
    record['source_image_unchanged'] = sha(args.image) == source_hash
    record['passed'] &= record['source_image_unchanged']
    (output / 'report.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({k: v for k, v in record.items() if k not in ('events', 'qemu_command')}, indent=2))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
