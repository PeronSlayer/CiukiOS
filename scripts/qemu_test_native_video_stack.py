#!/usr/bin/env python3
"""Exercise shipped ISO's first native desktop with synthetic video BIOS stack.

The ROM touches only the caller's scratch stack before delegating unchanged
registers/arguments to real SeaBIOS. It does not fake a desktop, DOS result or
BIOS mode response. It matches the shipped SHELL entry bytes so bootloader,
kernel splash, BOOTSND and AUXSTACK's own callers are not stress targets.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from unittest.mock import patch

from qemu_test_cd_sessions import Session, digest
from qemu_test_installed_hdd import FAT16


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--stack-bytes', type=int, default=1024)
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    iso = args.iso.resolve()
    packed, raw = out/'cd.img.gz', out/'cd.img'
    subprocess.run(['xorriso', '-osirrox', 'on', '-indev', str(iso),
                    '-extract', '/ciukios-full-cd-disk.img.gz', str(packed)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    raw.write_bytes(gzip.decompress(packed.read_bytes()))
    fat = FAT16(raw)
    report = {'iso_sha256': digest(iso), 'scratch_bytes': args.stack_bytes,
              'scope': 'Synthetic first-boot video BIOS scratch; not a T23 trace',
              'payloads': {}}
    for path, source, options in (
        ('SYSTEM/SHELL.COM', 'src/com/shell.asm', []),
        ('SYSTEM/BOOTSND.COM', 'src/com/ac97init.asm', ['-DBOOT_SOUND=1']),
    ):
        shipped = fat.read(path)
        binary = out/Path(path).name
        binary.write_bytes(shipped)
        current = out/('current-'+binary.name)
        subprocess.run(['nasm', '-f', 'bin', source, *options,
                        '-o', str(current)], check=True)
        report['payloads'][path] = {'bytes': len(shipped),
            'sha256': hashlib.sha256(shipped).hexdigest(),
            'current_sha256': digest(current),
            'matches_current_source': shipped == current.read_bytes()}
    shell = (out/'SHELL.COM').read_bytes()
    assert shell[:6] == bytes.fromhex('fa8cc88ed0bc')
    stack_top = struct.unpack_from('<H', shell, 6)[0]
    stack_bottom = stack_top-2048
    assert not any(shell[stack_bottom-256:stack_top-256]), 'Unexpected shell stack layout'
    rom = out/'firmware.rom'
    subprocess.run(['nasm', '-f', 'bin', 'scripts/fixtures/native_video_stack.asm',
                    f'-DSTACK_BYTES={args.stack_bytes}', f'-DSHELL_STACK_TOP={stack_top}',
                    '-o', str(rom)], check=True)
    data = bytearray(rom.read_bytes()); data[-1] = (-sum(data)) & 255
    rom.write_bytes(data)
    # Session owns its VM cleanup. Intercept only construction to append a
    # test ROM; every other QEMU option and real Live-CD assertion is reused.
    launch = subprocess.Popen
    def with_rom(command, *positional, **keywords):
        command += ['-option-rom', str(rom)]
        return launch(command, *positional, **keywords)
    with patch('subprocess.Popen', with_rom):
        vm = Session(iso, out/'vm', 'live', args.accel)
    report['qemu_command'] = vm.command
    failure = None
    try:
        vm.run()
    except Exception as error:
        failure = error
        report['error'] = repr(error)
        try: vm.shot('failure')
        except Exception: pass
    finally:
        vm.hmp('stop')
        (out/'registers.log').write_bytes(vm.hmp('info registers'))
        ram_path = out/'ram.bin'
        vm.hmp(f'pmemsave 0 0x100000 "{ram_path}"')
        ram = ram_path.read_bytes()
        found = ram.find(b'C10STACK', 0x500, 0xa0000)
        assert found >= 0, 'Cold-boot fixture RAM missing'
        count, segment, lowest, scratch, mismatch, budget = struct.unpack_from('<IHHHHH', ram, found+12)
        shell_address = segment*16
        guard_actual = ram[shell_address+stack_bottom-64:shell_address+stack_bottom]
        guard_expected = shell[stack_bottom-256-64:stack_bottom-256]
        report['fixture'] = dict(address=found, calls=count, caller_cs=segment,
            lowest_entry_sp=lowest, lowest_scratch_sp=scratch, stack_bottom=stack_bottom,
            stack_top=stack_top, scratch_bytes=budget, wrong_ss_calls=mismatch,
            remaining_stack_bytes=scratch-stack_bottom,
            pre_stack_64_bytes_unchanged=guard_actual == guard_expected)
        vm.close()
        report['guest'] = vm.report
        report['passed'] = (failure is None and count > 20 and mismatch == 0
                            and scratch >= stack_bottom and guard_actual == guard_expected)
        (out/'result.json').write_text(json.dumps(report, indent=2)+'\n')
        print(json.dumps(report, indent=2), flush=True)
    if failure:
        raise failure
    assert report['passed'], 'Missing fixture coverage or native shell stack overwrite'


if __name__ == '__main__':
    main()
