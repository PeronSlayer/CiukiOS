#!/usr/bin/env python3
"""Run the bounded CN32 CPL3 entry sample in a disposable full HDD guest."""
import argparse
import hashlib
import json
import re
import shutil
import struct
import subprocess
import zlib
from pathlib import Path

from qemu_test_full_display_profile import VM
from qemu_test_vm_session import FAT16


ROOT = Path(__file__).resolve().parents[1]
CASES = (
    (1, 'NSUPER.N32', 'supervisor write', 1, 14, 7),
    (2, 'NCODE.N32', 'readonly code write', 1, 14, 7),
    (3, 'NIO.N32', 'denied port I/O', 1, 13, 0),
    (4, 'NINT.N32', 'unauthorized software interrupt', 1, 13, 0x10A),
    (5, 'NGUARD.N32', 'absent stack guard', 1, 14, 6),
    (6, 'NLOOP.N32', 'POPFD and instruction budget', 2, 0, 0),
    (7, 'NABI.N32', 'CPL3 writable data and unknown syscall', 0, 0, 0),
    (8, 'NEXIT.N32', 'nonzero native exit status', 0, 0, 0),
    (9, 'NFPU.N32', 'unowned FPU state denied', 1, 7, 0),
)


def install_fixtures(disk: Path, output: Path) -> None:
    volume = f'{disk}@@{FAT16(disk).start}'
    for case, filename, *_ in CASES:
        blocks = []
        for part in ('CODE', 'DATA'):
            binary = output / f'{filename}.{part.lower()}.bin'
            subprocess.run(['nasm', '-f', 'bin', f'-DNATIVE_CASE={case}',
                            f'-DNATIVE_{part}_ONLY=1', '-o', str(binary),
                            str(ROOT / 'src/probes/native/entry_guards.asm')], check=True)
            blocks.append(binary.read_bytes())
        code, data = blocks
        payload = code + data
        header = struct.pack('<4sHHIIIIIII', b'CN32', 1, 36, 1, 0,
                             len(code), len(data), 4096, len(payload),
                             zlib.crc32(payload) & 0xFFFFFFFF)
        image = output / filename
        image.write_bytes(header + payload)
        subprocess.run(['mcopy', '-o', '-i', volume, str(image),
                        '::VM/' + filename], check=True)


def normalized(vm: VM, offset: int) -> str:
    return subprocess.check_output([str(ROOT / 'scripts/serial_log_normalize.py'),
                                    '--offset', str(offset), str(vm.serial)]).decode('cp437')


def run_sample(vm: VM) -> None:
    offset = vm.offset()
    vm.text('\\VM\\NATIVE.COM')
    vm.wait('[NATIVE32] SAMPLE PASS reports=2 tag=2 value=CIUK', offset, 45)
    vm.wait('[NATIVE32] EXIT 0', offset, 20)
    vm.wait('CiukiOS SHELL C:\\APPS>', offset, 20)


def run_guards(vm: VM, report: dict) -> None:
    report['guards'] = []
    for case, filename, name, result, vector, error in CASES:
        offset = vm.offset()
        vm.text('\\VM\\NATIVE.COM \\VM\\' + filename)
        marker = ('[NATIVE32] EXIT status=00000007' if case == 8 else
                  '[NATIVE32] EXIT 0' if result == 0 else
                  '[NATIVE32] FAIL native execution result=')
        vm.wait(marker, offset, 30)
        vm.wait('CiukiOS SHELL C:\\APPS>', offset, 20)
        body = normalized(vm, offset)
        record = {'name': name, 'fixture': filename, 'output': body}
        report['guards'].append(record)
        if result:
            match = re.search(r'result=([0-9A-F]{8}) vector=([0-9A-F]{8}) '
                              r'error=([0-9A-F]{8}) steps=([0-9A-F]{8})', body)
            assert match, (name, body)
            actual_result, actual_vector, actual_error, steps = (int(v, 16) for v in match.groups())
            assert (actual_result, actual_vector, actual_error) == (result, vector, error), record
            if result == 2:
                assert steps == 256, record
            else:
                assert steps <= 2, record
            record.update(result=actual_result, vector=actual_vector, error=actual_error, steps=steps)
        else:
            if case == 8:
                assert '[NATIVE32] EXIT status=00000007' in body, record
            else:
                assert '[NATIVE32] SAMPLE PASS reports=2 tag=2 value=CIUK' in body, record
            assert '[NATIVE32] FAIL' not in body, record
        record['passed'] = True
        # Reentry after every exit/fault catches stale IDT/TR/GDT/CR3 ownership.
        run_sample(vm)
        print('[native-entry] PASS ' + name + ' and native/DOS reentry', flush=True)
    offset = vm.offset()
    vm.text('\\VM\\NATPAGE.COM')
    vm.wait('[NATIVE-PAGES] PASS 8 owner/zero/release probes', offset, 45)
    vm.wait('CiukiOS SHELL C:\\APPS>', offset, 20)
    report['page_allocator_after_faults'] = True


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--memory', type=int, default=128)
    parser.add_argument('--faults', action='store_true', help='Run actual CPL3 fault/guard and reentry probes')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    image = args.image.resolve()
    before = hashlib.sha256(image.read_bytes()).hexdigest()
    disk = args.output / 'disk.img'
    shutil.copyfile(image, disk)
    if args.faults:
        install_fixtures(disk, args.output.resolve())
    report = {'passed': False, 'memory_mib': args.memory, 'source_sha256': before}
    vm = VM(disk, args.output, memory=args.memory)
    try:
        vm.wait('[DESKTOP] READY', timeout=90)
        vm.key('f4')
        vm.wait('CiukiOS SHELL C:\\APPS>', timeout=30)
        run_sample(vm)
        if args.faults:
            run_guards(vm, report)
        report['passed'] = True
    except Exception as error:
        report['error'] = repr(error)
        raise
    finally:
        vm.close()
        report['source_unchanged'] = hashlib.sha256(image.read_bytes()).hexdigest() == before
        (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    assert report['source_unchanged']
    print('[native-entry] PASS bounded CN32 sample and DOS return')


if __name__ == '__main__':
    main()
