#!/usr/bin/env python3
"""QEMU gate for the first native 32-bit process memory contract.

The future NPROCCHK.EXE fixture must emit the serial protocol documented in
docs/native-memory-architecture-2026-10-02.md before this gate can pass. The
records are assertions from the guest's allocator/process test, not a host
inspection of guest page tables.
"""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qemu_test_full_display_profile import VM


PROMPT = 'CiukiOS SHELL C:\\APPS>'
RECORDS = {
    'above_1m': re.compile(
        rb'^\[NATIVE-PROC\] ABOVE1M PASS addr=([0-9A-Fa-f]{8}) '
        rb'bytes=([0-9]+) checksum=([0-9A-Fa-f]{8})$', re.M),
    'isolation': re.compile(
        rb'^\[NATIVE-PROC\] ISOLATION PASS processes=2 private=1$', re.M),
    'normal_exit': re.compile(
        rb'^\[NATIVE-PROC\] RECLAIM_EXIT PASS allocated=([0-9]+) '
        rb'freed=([0-9]+)$', re.M),
    'zero_on_reuse': re.compile(
        rb'^\[NATIVE-PROC\] ZERO_REUSE PASS pages=([0-9]+) nonzero=0$', re.M),
    'fault_cleanup': re.compile(
        rb'^\[NATIVE-PROC\] RECLAIM_FAULT PASS allocated=([0-9]+) '
        rb'freed=([0-9]+)$', re.M),
    'done': re.compile(rb'^\[NATIVE-PROC\] DONE PASS$', re.M),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--command', default='NPROCCHK',
                        help='shell command that runs the native process conformance fixture')
    parser.add_argument('--memory', type=int, default=512,
                        help='QEMU RAM in MiB; use a measured supported profile')
    args = parser.parse_args()
    if args.memory < 32:
        parser.error('--memory must be at least 32 MiB')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    image = args.image.resolve()
    if not image.is_file():
        parser.error(f'full HDD image does not exist: {image}')
    disk = output / 'disk.img'
    shutil.copyfile(image, disk)
    vm = VM(disk, output, memory=args.memory)
    report = {
        'passed': False,
        'image_sha256': hashlib.sha256(image.read_bytes()).hexdigest(),
        'command': args.command,
        'memory_mib': args.memory,
        'checks': {},
    }

    def normalized(offset=0):
        return subprocess.check_output([
            str(Path(__file__).resolve().parent / 'serial_log_normalize.py'),
            '--offset', str(offset), str(vm.serial)])

    try:
        vm.wait(PROMPT, timeout=120)
        offset = vm.offset()
        vm.text(args.command)
        vm.wait('[NATIVE-PROC] DONE PASS', offset, timeout=120)
        body = normalized(offset)
        parsed = {}
        for name, pattern in RECORDS.items():
            match = pattern.search(body)
            assert match, f'missing or malformed {name} serial record'
            parsed[name] = match.groups()

        address = int(parsed['above_1m'][0], 16)
        size = int(parsed['above_1m'][1])
        checksum = int(parsed['above_1m'][2], 16)
        assert address > 0x100000, f'probe address {address:#x} is not above 1 MiB'
        assert size >= 4096, f'probe covered only {size} bytes'
        assert checksum != 0, 'probe checksum is zero'
        report['checks']['above_1m'] = {'address': f'0x{address:08x}', 'bytes': size,
                                        'checksum': f'{checksum:08x}'}
        report['checks']['isolation'] = 'PASS'
        for name in ('normal_exit', 'fault_cleanup'):
            allocated, freed = map(int, parsed[name])
            assert allocated > 0 and allocated == freed, (
                f'{name}: allocated={allocated}, freed={freed}')
            report['checks'][name] = {'allocated_pages': allocated, 'freed_pages': freed}
        pages = int(parsed['zero_on_reuse'][0])
        assert pages > 0, 'zero-on-reuse covered no pages'
        report['checks']['zero_on_reuse'] = {'pages': pages, 'nonzero_bytes': 0}
        report['checks']['done'] = 'PASS'
        report['passed'] = True
    except Exception as error:
        report['error'] = repr(error)
        if vm.serial.exists():
            report['serial_tail'] = normalized()[-12000:].decode('cp437', 'replace')
        raise
    finally:
        vm.close()
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('[native-process] PASS', output, flush=True)


if __name__ == '__main__':
    main()
