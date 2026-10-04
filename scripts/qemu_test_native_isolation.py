#!/usr/bin/env python3
"""Run two CN32 processes at the same virtual address and check zero reuse."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zlib

from qemu_test_full_display_profile import VM


ROOT = Path(__file__).resolve().parents[1]
HEADER = struct.Struct('<4sHHIIIIIII')


def native_image(source: Path, define: str, output: Path) -> None:
    with tempfile.TemporaryDirectory(prefix='ciuki-native-isolation-') as scratch:
        code = Path(scratch) / 'code.bin'
        subprocess.run(['nasm', '-f', 'bin', f'-D{define}=1', '-o', str(code),
                        str(source)], check=True, cwd=ROOT)
        payload = code.read_bytes() + b'\0\0\0\0'
    output.write_bytes(HEADER.pack(b'CN32', 1, 36, 1, 0, len(payload)-4, 4,
                                   4096, len(payload), zlib.crc32(payload)) + payload)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path,
                        default=ROOT/'build/full/ciukios-full.img')
    parser.add_argument('--module', type=Path,
                        default=ROOT/'build/full/obj/vm-window/session/CVSESSION.DLL')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--memory', type=int, default=256)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    image = args.image.resolve()
    module = args.module.resolve()
    source_hash = hashlib.sha256(image.read_bytes()).hexdigest()
    disk = output/'disk.img'
    shutil.copyfile(image, disk)
    source = ROOT/'scripts/fixtures/native_isolation_probe.asm'
    writer, reader = output/'NWRITE.N32', output/'NREAD.N32'
    launcher = output/'NATIVE.COM'
    subprocess.run(['nasm', '-f', 'bin', '-o', str(launcher),
                    str(ROOT/'src/com/native_launch.asm')], check=True, cwd=ROOT)
    native_image(source, 'WRITER', writer)
    native_image(source, 'READER', reader)
    for path, target in ((module, 'CVSESS.DLL'), (launcher, 'NATIVE.COM'),
                         (writer, 'NWRITE.N32'),
                         (reader, 'NREAD.N32')):
        subprocess.run(['mcopy', '-o', '-i', str(disk), str(path), f'::VM/{target}'],
                       check=True)
    report = {'source_sha256': source_hash,
              'module_sha256': hashlib.sha256(module.read_bytes()).hexdigest(),
              'launcher_sha256': hashlib.sha256(launcher.read_bytes()).hexdigest(),
              'writer_sha256': hashlib.sha256(writer.read_bytes()).hexdigest(),
              'reader_sha256': hashlib.sha256(reader.read_bytes()).hexdigest(),
              'memory_mib': args.memory, 'commands': []}
    vm = VM(disk, output, memory=args.memory)
    try:
        vm.wait('[DESKTOP] READY', timeout=90)
        vm.key('f4')
        vm.wait('CiukiOS SHELL C:\\APPS>', timeout=45)
        for name, status in (('NWRITE.N32', 3), ('NREAD.N32', 4)):
            command = f'\\VM\\NATIVE.COM \\VM\\{name}'
            at = vm.offset()
            vm.text(command)
            vm.wait(f'[NATIVE32] EXIT status={status:08X}', at, 45)
            vm.wait('CiukiOS SHELL C:\\APPS>', at, 30)
            body = subprocess.check_output([str(ROOT/'scripts/serial_log_normalize.py'),
                                            '--offset', str(at), str(vm.serial)])
            if b'[NATIVE32] FAIL' in body:
                raise AssertionError(f'{name}: native failure despite a marker')
            report['commands'].append({'image': name, 'exit_status': status,
                                       'serial_offset': at,
                                       'zero_reuse_checked': name == 'NREAD.N32'})
        at = vm.offset()
        vm.text('\\VM\\NATPAGE.COM')
        vm.wait('[NATIVE-PAGES] PASS 8 owner/zero/release probes', at, 45)
        vm.wait('CiukiOS SHELL C:\\APPS>', at, 30)
        report['page_allocator_probe'] = True
    finally:
        vm.close()
    report['canonical_unchanged'] = (
        hashlib.sha256(image.read_bytes()).hexdigest() == source_hash)
    report['passed'] = report['canonical_unchanged'] and len(report['commands']) == 2
    (output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
