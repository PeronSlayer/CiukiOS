#!/usr/bin/env python3
"""Check real PS/2 LED ACK interrupts through the installed kernel's IRQ1 hook."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import time

from qemu_test_installed_hdd import FAT16, InstalledVM
from qemu_test_full_display_profile import VM


class KeyboardVM(InstalledVM):
    def __init__(self, disk, output, rom):
        VM.__init__(self, disk, output, qemu_args=[
            '-vga', 'cirrus', '-option-rom', str(rom),
            '-audiodev', f'wav,id=snd,path={output}/audio.wav',
            '-device', 'AC97,audiodev=snd'])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--image', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--kernel', type=Path)
    ap.add_argument('--expect-blocked-ack', action='store_true')
    ap.add_argument('--empty-reentry', action='store_true')
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    disk = out / 'installed.img'
    assert disk != args.image.resolve()
    original_hash = hashlib.sha256(args.image.read_bytes()).hexdigest()
    shutil.copyfile(args.image, disk)
    fat = FAT16(disk)
    if args.kernel:
        subprocess.run(['mcopy', '-o', '-i', f'{disk}@@{fat.start}',
                        str(args.kernel), '::SYSTEM/CIUKIDOS.SYS'], check=True)
    rom = out / 'keyboard-led.rom'
    subprocess.run(['nasm', '-f', 'bin', 'scripts/fixtures/keyboard_led_irq.asm',
                    f'-DTEST_EMPTY_REENTRY={int(args.empty_reentry)}',
                    '-o', str(rom)], check=True)
    data = bytearray(rom.read_bytes())
    data[-1] = -sum(data) & 255
    rom.write_bytes(data)
    record = {'completed': False, 'source_sha256': original_hash,
              'kernel_sha256': hashlib.sha256(FAT16(disk).read('SYSTEM/CIUKIDOS.SYS')).hexdigest(),
              'expect_blocked_ack': args.expect_blocked_ack, 'snapshots': []}
    vm = KeyboardVM(disk, out, rom)

    def capture(name):
        path = out / (name + '-ram.bin')
        vm.hmp(f'pmemsave 0 0x100000 "{path}"')
        ram = path.read_bytes()
        limit = struct.unpack_from('<H', ram, 0x413)[0] * 1024
        assert ram[limit:limit+8] == b'KLEDIRQ1', 'resident firmware fixture missing'
        counts = struct.unpack_from('<4H', ram, limit + 14)
        entry = {'name': name, 'updates': counts[0], 'acks': counts[1],
                 'timeouts': counts[2], 'unexpected': counts[3],
                 'keyboard_flags': ram[0x417], 'led_flags': ram[0x497]}
        entry['empty_calls'], entry['empty_returns'] = struct.unpack_from('<2H', ram, limit + 23)
        record['snapshots'].append(entry)
        (out / (name + '-registers.log')).write_bytes(vm.hmp('info registers'))
        (out / (name + '-pic.log')).write_bytes(vm.hmp('info pic'))
        return entry

    try:
        vm.wait('[DESKTOP] READY', timeout=90)
        before = capture('before')
        vm.key('caps_lock')
        time.sleep(2)
        after = capture('caps-on')
        assert after['updates'] > before['updates'], 'Caps Lock did not issue a LED command'
        if args.expect_blocked_ack:
            assert after['timeouts'] > before['timeouts'], 'expected ACK timeout did not occur'
            assert after['acks'] == before['acks'], 'ACK reached firmware unexpectedly'
            vm.shot('blocked-ack')
        else:
            assert after['acks'] - before['acks'] == 2, 'ED and LED data must each receive a real ACK'
            assert after['timeouts'] == after['unexpected'] == 0, after
            vm.key('caps_lock')
            time.sleep(.3)
            vm.key('f4')
            vm.wait('CiukiOS SHELL C:\\APPS>')
            for key in ('caps_lock', 'num_lock', 'scroll_lock'):
                for _ in range(4):
                    vm.key(key)
                    time.sleep(.15)
            vm.result('dir \\SBEMU', 'HDPMI32I.EXE', 'VSBHDA.EXE')
            vm.result('run \\COMMAND.COM', 'HELP lists commands.')
            vm.result('comdemo', 'COM demo via INT21h')
            vm.result('mzdemo', 'MZ demo via INT21h')
            vm.result('exit')
            end = capture('after-commands')
            assert end['updates'] >= 14, end
            assert end['acks'] == end['updates'] * 2, end
            assert end['timeouts'] == end['unexpected'] == 0, end
            if args.empty_reentry:
                assert end['empty_calls'] == end['empty_returns'] == 1, end
            vm.shot('working-dos')
        record['completed'] = True
        print(json.dumps(record, indent=2), flush=True)
    except Exception:
        capture('failure')
        vm.shot('failure')
        raise
    finally:
        vm.close()
        assert hashlib.sha256(args.image.read_bytes()).hexdigest() == original_hash
        (out / 'result.json').write_text(json.dumps(record, indent=2) + '\n')


if __name__ == '__main__':
    main()
