#!/usr/bin/env python3
"""Exercise the shipped root COMMAND.COM after leaving the installed desktop."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

from qemu_test_installed_hdd import FAT16, InstalledVM


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    disk = out / 'installed.img'
    assert disk.resolve() != args.image.resolve(), 'output must be a disposable copy'
    shutil.copyfile(args.image, disk)
    fat = FAT16(disk)
    record = {
        'source_sha256': hashlib.sha256(args.image.read_bytes()).hexdigest(),
        'command_sha256': hashlib.sha256(fat.read('COMMAND.COM')).hexdigest(),
        'completed': False,
    }
    vm = InstalledVM(disk, out, vga='cirrus')
    try:
        vm.wait('[DESKTOP] READY', timeout=90)
        vm.key('f4')
        vm.wait('CiukiOS SHELL C:\\APPS>')
        vm.result('run \\COMMAND.COM', 'HELP lists commands. WHERE shows launch targets.')
        mode = out / 'nested-video-mode.bin'
        vm.hmp(f'pmemsave 0x449 1 "{mode}"')
        assert mode.read_bytes() == b'\x03', 'nested interpreter did not enter text mode'
        vm.shot('nested-command')
        body = vm.result('dir \\SBEMU')
        expected = [name + (' <DIR>' if entry[0] & 16 else '')
                    for name, entry in fat.entries(fat.entry('SBEMU')[1]).items()]
        listing = body.split('Directory listing\r\n', 1)[1].split('CiukiOS SHELL', 1)[0]
        actual = [line for line in listing.splitlines() if line]
        assert actual == expected, (actual, expected)
        vm.shot('sbemu-text')
        vm.result('comdemo', 'COM demo via INT21h')
        vm.result('mzdemo', 'MZ demo via INT21h')
        vm.result('exit')
        vm.result('echo RETURNED', 'RETURNED\r\nCiukiOS SHELL')
        offset = vm.offset()
        vm.text('exit')
        vm.wait('[DESKTOP] READY', offset, 90)
        vm.shot('returned-desktop')
        record['sbemu_entries'] = len(actual)
        record['completed'] = True
        print('PASS shipped COMMAND.COM: text mode, exact SBEMU listing, COM/MZ, parent and desktop return')
    except Exception:
        vm.shot('failure')
        raise
    finally:
        vm.close()
        assert hashlib.sha256(args.image.read_bytes()).hexdigest() == record['source_sha256']
        (out / 'result.json').write_text(json.dumps(record, indent=2) + '\n')


if __name__ == '__main__':
    main()
