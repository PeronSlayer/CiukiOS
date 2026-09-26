#!/usr/bin/env python3
"""Check actual loader errors survive native-shell video restoration."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

from qemu_test_installed_hdd import FAT16, InstalledVM


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--image', type=Path, required=True)
    ap.add_argument('--kernel', type=Path, required=True)
    ap.add_argument('--shell', type=Path, required=True)
    ap.add_argument('--command', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    disk = out / 'installed.img'
    assert disk != args.image.resolve()
    before_hash = hashlib.sha256(args.image.read_bytes()).hexdigest()
    shutil.copyfile(args.image, disk)
    volume = f'{disk}@@{FAT16(disk).start}'
    for source, destination in ((args.kernel, 'SYSTEM/CIUKIDOS.SYS'),
                                (args.shell, 'SYSTEM/SHELL.COM'),
                                (args.command, 'COMMAND.COM')):
        subprocess.run(['mcopy', '-o', '-i', volume, str(source), '::' + destination], check=True)
    # A missing Windows launcher exercises the desktop shortcut's real AX=2.
    subprocess.run(['mdel', '-i', volume, '::WINDOWS/WIN.COM'], check=True)
    # A real short FAT chain exercises loader AX=5. No code from SHORT.COM
    # can execute: its declared length exceeds its one-cluster allocation.
    short = out / 'SHORT.COM'
    short.write_bytes(b'\xCB')
    subprocess.run(['mcopy', '-i', volume, str(short), '::SHORT.COM'], check=True)
    fat = FAT16(disk)
    data = bytearray(fat.data)
    for offset in range(fat.root, fat.root + fat.root_size, 32):
        if data[offset:offset+11] == b'SHORT   COM':
            struct.pack_into('<I', data, offset + 28, fat.spc * 512 * 2)
            break
    else:
        raise AssertionError('test file was not added')
    disk.write_bytes(data)
    record = {'completed': False, 'source_sha256': before_hash, 'outputs': []}
    vm = InstalledVM(disk, out, vga='cirrus')

    def failure(command, expected):
        start = vm.offset()
        vm.text(command)
        vm.wait('CiukiOS SHELL C:\\APPS>', start, 60)
        body = subprocess.check_output(['scripts/serial_log_normalize.py',
                                        '--offset', str(start), str(vm.serial)]).decode('cp437')
        assert expected in body, (command, expected, body)
        record['outputs'].append({'command': command, 'output': body})
        vm.result('comdemo', 'COM demo via INT21h')

    try:
        vm.wait('[DESKTOP] READY', timeout=90)
        vm.key('f4')
        vm.wait('CiukiOS SHELL C:\\APPS>')
        failure('win', 'exec: cannot execute (DOS error 0002)')
        failure('run \\SHORT.COM', 'exec: cannot execute (DOS error 0005)')
        vm.result('run \\COMMAND.COM', 'HELP lists commands.')
        failure('run \\SHORT.COM', 'exec: cannot execute (DOS error 0005)')
        vm.shot('text-error-code')
        vm.result('exit')
        vm.result('mzdemo', 'MZ demo via INT21h')
        start = vm.offset()
        vm.text('exit')
        vm.wait('[DESKTOP] READY', start, 90)
        record['completed'] = True
        print('PASS real missing-launcher and short-chain errors, native/compatibility output, subsequent COM/MZ and desktop return')
    except Exception:
        vm.shot('failure')
        raise
    finally:
        vm.close()
        assert hashlib.sha256(args.image.read_bytes()).hexdigest() == before_hash
        (out / 'result.json').write_text(json.dumps(record, indent=2) + '\n')


if __name__ == '__main__':
    main()
