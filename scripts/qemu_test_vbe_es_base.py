#!/usr/bin/env python3
"""Regression for VBE LFB copy/fill ES-base preservation on the full HDD.

Builds a small COM fixture around the production VBE includes and installs it
in a private copy of the FAT16 full image. STARTUP.CFG selects DOS! so the test
runs before the VM manager; the fixture owns a 128 KiB DOS block and checks exact
physical fill/copy/readback plus canaries across offset 0xFFFF.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time

from qemu_test_full_display_profile import VM
from qemu_test_installed_hdd import FAT16


ROOT = Path(__file__).resolve().parents[1]
PASS = '[LFBES] PASS copy/fill, ES base and 64KiB boundary'
FAIL_PREFIX = '[LFBES] FAIL'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def wait_debug(path, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = path.read_text(errors='replace') if path.exists() else ''
        if PASS in text:
            return text
        if FAIL_PREFIX in text:
            raise AssertionError(text.strip())
        time.sleep(.05)
    text = path.read_text(errors='replace') if path.exists() else ''
    raise AssertionError(f'fixture marker timed out; debugcon={text!r}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    parser.add_argument('--output', type=Path, default=Path('build/full/qemu-vbe-es-base'))
    args = parser.parse_args()
    image = args.image.resolve()
    output = args.output.resolve()
    assert image.is_file(), f'missing full HDD image: {image}'
    output.mkdir(parents=True, exist_ok=False)
    disk = output / 'test.img'
    shutil.copyfile(image, disk)
    source_hash = sha(image)
    source_volume = FAT16(disk)
    source_volume.entry('SYSTEM/CIUKIDOS.SYS')
    source_volume.entry('SYSTEM/SHELL.COM')
    startup = output / 'STARTUP.CFG'
    startup.write_bytes(b'DOS!')
    display = output / 'DISPLAY.CFG'
    display.write_bytes(b'TEXT')
    volume = f'{disk}@@{source_volume.start}'
    subprocess.run(['mcopy', '-o', '-i', volume, str(startup),
                    '::SYSTEM/STARTUP.CFG'], check=True)
    subprocess.run(['mcopy', '-o', '-i', volume, str(display),
                    '::SYSTEM/VIDEO/DISPLAY.CFG'], check=True)

    fixture = output / 'LFBES.COM'
    subprocess.run(['nasm', '-f', 'bin', 'scripts/fixtures/vbe_es_base.asm',
                    '-o', str(fixture)], cwd=ROOT, check=True)
    subprocess.run(['mcopy', '-o', '-i', volume, str(fixture), '::APPS/LFBES.COM'],
                   check=True)

    debug = output / 'debugcon.log'
    vm = VM(disk, output, memory=128, qemu_args=[
        '-debugcon', f'file:{debug}', '-global', 'isa-debugcon.iobase=0xe9'])
    record = {'passed': False, 'source_image_sha256': source_hash,
              'test_image_sha256': sha(disk), 'fixture_sha256': sha(fixture),
              'cpu': 'pentium3', 'memory_mib': 128,
              'startup_mode': 'DOS!', 'dos_block_kib': 128,
              'target_offsets': [0, 0x20, 0xFFFC]}
    try:
        vm.wait('[BOOT-SESSION] DOS', timeout=60)
        vm.wait('CiukiOS SHELL C:\\APPS>', timeout=90)
        offset = vm.offset()
        vm.text(r'run \APPS\LFBES.COM')
        record['debugcon'] = wait_debug(debug, timeout=30)
        vm.wait('CiukiOS SHELL C:\\APPS>', offset=offset, timeout=30)
        record['passed'] = True
    finally:
        vm.close()
        record['source_image_unchanged'] = sha(image) == source_hash
        if not record['source_image_unchanged']:
            record['passed'] = False
        (output / 'report.json').write_text(json.dumps(record, indent=2) + '\n')
    print(f"[vbe-es-base] {'PASS' if record['passed'] else 'FAIL'}")
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
