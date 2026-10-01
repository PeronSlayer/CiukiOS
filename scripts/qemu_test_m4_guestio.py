#!/usr/bin/env python3
"""QEMU gate: the M4 DOS window forwards a real guest's mouse and audio I/O."""
import argparse
import json
import re
import shutil
import subprocess
import time
from pathlib import Path

from qemu_test_installed_hdd import FAT16
from qemu_test_native_windows import WindowVM

ROOT = Path(__file__).resolve().parent.parent


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--image', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    disk = args.output / 'disk.img'
    shutil.copyfile(args.image, disk)
    probe = args.output / 'GUESTIO.COM'
    subprocess.run(['nasm', '-f', 'bin', str(ROOT / 'src/probes/vm/guest_io.asm'),
                    '-o', str(probe)], check=True)
    subprocess.run(['mcopy', '-o', '-i', f'{disk}@@{FAT16(disk).start}',
                    str(probe), '::APPS/GUESTIO.COM'], check=True)
    vm = WindowVM(disk, args.output, 'std', boot_capture=True,
                  memory=128, palette='platinum')
    report = {'passed': False, 'host': 'QEMU', 'checks': {}}

    def serial(offset=0):
        return subprocess.check_output([
            str(ROOT / 'scripts/serial_log_normalize.py'), '--offset',
            str(offset), str(vm.serial)]).decode('cp437', 'replace')

    def send(key):
        vm.hmp(f'sendkey {key} 180')
        time.sleep(.25)

    def wait_mouse(predicate, after, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            records = [tuple(int(x, 16) for x in row) for row in
                       re.findall(r'\[GUESTIO\] MOUSE ([0-9A-F]{4}) ([0-9A-F]{4}) ([0-9A-F]{4})',
                                  serial(after))]
            if records and predicate(records[-1]):
                return records[-1]
            time.sleep(.15)
        raise AssertionError(f'guest mouse did not reach requested state: {records[-5:]}')

    try:
        vm.ready()
        offset = vm.offset()
        send('meta_l-r')
        vm.wait('WINDOW 01 OPEN', offset, 20)
        vm.text('\\APPS\\GUESTIO.COM')
        send('ret')
        vm.wait('[DOSVM] fork', offset, 45)
        vm.wait('[GUESTIO] READY MOUSE', offset, 60)
        vm.shot('guest-video')
        report['checks']['guest_session'] = True

        before = vm.offset()
        vm.position(700, 450)          # DOS window client, graphics mode 13h
        initial = wait_mouse(lambda row: row[0] > 0 and row[1] > 0, before)
        report['checks']['guest_mouse_move'] = initial
        before = vm.offset()
        vm.hmp('mouse_button 1')
        pressed = wait_mouse(lambda row: row[2] & 1, before)
        vm.hmp('mouse_button 0')
        released = wait_mouse(lambda row: not row[2] & 1, before)
        report['checks']['guest_mouse_click'] = {'pressed': pressed, 'released': released}

        before = vm.offset()
        send('p')
        vm.wait('[GUESTIO] OPL DONE', before, 45)
        report['checks']['sb16_opl_completed'] = True
        send('esc')
        vm.wait('[GUESTIO] EXIT', before, 20)
        report['checks']['guest_exit'] = True

        # QEMU's WAV header is finalized when its process exits below.
        report['passed'] = True
    except Exception as exc:
        report['error'] = repr(exc)
        try:
            vm.shot('failure')
        except Exception:
            pass
        raise
    finally:
        (args.output / 'serial.log').write_text(serial())
        vm.close()
        audio = args.output / 'audio.wav'
        if report['passed'] and audio.exists():
            data = audio.read_bytes()
            # QEMU can leave the RIFF/data sizes as zero on exit even though
            # it wrote valid 16-bit PCM after the 44-byte header.
            if data[:4] != b'RIFF' or data[8:12] != b'WAVE' or data[36:40] != b'data':
                raise AssertionError('QEMU audio capture is not PCM WAV')
            samples = data[44:]
            energy = sum(abs(int.from_bytes(samples[i:i+2], 'little', signed=True))
                         for i in range(0, len(samples)-1, 128))
            report['checks']['audible_pcm_energy'] = energy
            report['passed'] = energy > 10000
            if not report['passed']:
                report['error'] = 'audio capture remained silent'
        (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    assert report['passed'], report.get('error')
    print('[m4-guestio] PASS', args.output)


if __name__ == '__main__':
    main()
