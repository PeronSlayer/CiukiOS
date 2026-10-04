#!/usr/bin/env python3
"""Execute two persistent private CPL3 processes across multiple quanta."""
import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import zlib
from pathlib import Path

from qemu_test_full_display_profile import VM
from qemu_test_vm_session import FAT16


ROOT = Path(__file__).resolve().parents[1]


def install(disk: Path, output: Path) -> dict:
    volume = f'{disk}@@{FAT16(disk).start}'
    artifacts = {}
    for filename, source, define in (
        ('NRESUMA.N32', 'resume_quantum.asm', '-DNATIVE_COOKIE=0x41414943'),
        ('NRESUMB.N32', 'resume_quantum.asm', '-DNATIVE_COOKIE=0x42424943'),
        ('NLOOP.N32', 'entry_guards.asm', '-DNATIVE_CASE=6'),
    ):
        blocks = []
        for part in ('CODE', 'DATA'):
            binary = output / f'{filename}.{part.lower()}.bin'
            subprocess.run(['nasm', '-f', 'bin', define,
                            f'-DNATIVE_{part}_ONLY=1', '-o', str(binary),
                            str(ROOT / 'src/probes/native' / source)], check=True)
            blocks.append(binary.read_bytes())
        code, data = blocks
        payload = code + data
        header = struct.pack('<4sHHIIIIIII', b'CN32', 1, 36, 1, 0,
                             len(code), len(data), 4096, len(payload),
                             zlib.crc32(payload) & 0xFFFFFFFF)
        fixture = output / filename
        fixture.write_bytes(header + payload)
        subprocess.run(['mcopy', '-o', '-i', volume, str(fixture),
                        '::VM/' + filename], check=True)
        artifacts[filename] = hashlib.sha256(fixture.read_bytes()).hexdigest()
    probe = output / 'NATRESUM.COM'
    subprocess.run(['nasm', '-f', 'bin', '-I', str(ROOT) + '/',
                    '-o', str(probe), str(ROOT / 'src/probes/native/native_resume.asm')],
                   check=True, cwd=ROOT)
    subprocess.run(['mcopy', '-o', '-i', volume, str(probe),
                    '::VM/NATRESUM.COM'], check=True)
    artifacts[probe.name] = hashlib.sha256(probe.read_bytes()).hexdigest()
    return artifacts


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--memory', type=int, default=128)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = args.image.resolve()
    before = hashlib.sha256(source.read_bytes()).hexdigest()
    disk = output / 'disk.img'
    shutil.copyfile(source, disk)
    report = {'passed': False, 'memory_mib': args.memory,
              'source_sha256': before, 'fixtures': install(disk, output)}
    vm = VM(disk, output, memory=args.memory)
    try:
        vm.wait('[DESKTOP] READY', timeout=90)
        vm.key('f4')
        vm.wait('CiukiOS SHELL C:\\APPS>', timeout=30)
        offset = vm.offset()
        vm.text('\\VM\\NATRESUM.COM')
        for marker in (
            '[NATIVE-RESUME] TWO LIVE HANDLES',
            '[NATIVE-RESUME] TWO COMPLETE reports=2 private cookies steps>1200',
            '[NATIVE-RESUME] STOP AND GENERATION GUARD PASS',
            '[NATIVE-RESUME] PASS',
        ):
            vm.wait(marker, offset, 45)
        vm.wait('CiukiOS SHELL C:\\APPS>', offset, 20)
        body = subprocess.check_output([str(ROOT / 'scripts/serial_log_normalize.py'),
                                        '--offset', str(offset), str(vm.serial)]).decode('cp437')
        assert '[NATIVE-RESUME] FAIL' not in body, body
        report['probe_output'] = body
        offset = vm.offset()
        vm.text('\\VM\\NATPAGE.COM')
        vm.wait('[NATIVE-PAGES] PASS 8 owner/zero/release probes', offset, 45)
        vm.wait('CiukiOS SHELL C:\\APPS>', offset, 20)
        report['allocator_after_stop'] = True
        offset = vm.offset()
        vm.text('\\VM\\NATIVE.COM')
        vm.wait('[NATIVE32] SAMPLE PASS reports=2 tag=2 value=CIUK', offset, 45)
        vm.wait('[NATIVE32] EXIT 0', offset, 20)
        vm.wait('CiukiOS SHELL C:\\APPS>', offset, 20)
        report['one_shot_after_stop'] = True
        report['passed'] = True
    except Exception as error:
        report['error'] = repr(error)
        raise
    finally:
        vm.close()
        report['source_unchanged'] = hashlib.sha256(source.read_bytes()).hexdigest() == before
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    assert report['source_unchanged']
    print('[native-resume] PASS two isolated continuations, STOP, allocator and DOS return')


if __name__ == '__main__':
    main()
