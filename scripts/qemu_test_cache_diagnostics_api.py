#!/usr/bin/env python3
"""Run the CVSESSION framebuffer-diagnostics caller fixture in two VMs.

This QEMU gate checks system-VM bounds and child-VM isolation using the real
CVSESSION JLM. Run alone in the project's capped QEMU scope.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

from qemu_test_full_display_profile import VM


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'scripts/fixtures/cache_diagnostics_api.asm'


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def partition_offset(image):
    with image.open('rb') as stream:
        boot = stream.read(512)
    if boot[54:62] == b'FAT16   ' and struct.unpack_from('<H', boot, 11)[0] == 512:
        return 0
    return struct.unpack_from('<I', boot, 454)[0] * 512


def build_fixture(out, guest=False):
    output = out / ('CVFDCHLD.COM' if guest else 'CVFDTEST.COM')
    command = ['nasm', '-f', 'bin', '-I', './']
    if guest:
        command += ['-D', 'CVFD_GUEST=1']
    command += [str(FIXTURE), '-o', str(output)]
    subprocess.run(command, cwd=ROOT, check=True)
    return output


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=ROOT / 'build/full/ciukios-full.img')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(argv)
    image = args.image.resolve()
    out = args.output.resolve()
    if out.exists():
        raise SystemExit(f'refusing to overwrite existing output: {out}')
    out.mkdir(parents=True)
    before = sha256(image)
    disk = out / 'private.img'
    shutil.copyfile(image, disk)
    volume = f'{disk}@@{partition_offset(disk)}'
    system_fixture = build_fixture(out)
    child_fixture = build_fixture(out, guest=True)
    for fixture in (system_fixture, child_fixture):
        subprocess.run(['mcopy', '-o', '-i', volume, str(fixture), '::APPS/'], check=True)

    report = {'source_image_sha256': before, 'private_image': disk.name,
              'fixtures': [system_fixture.name, child_fixture.name], 'checks': []}
    vm = VM(disk, out, memory=512)
    try:
        vm.wait('[DESKTOP] READY', timeout=90)
        vm.key('f4')
        vm.wait('CiukiOS SHELL C:\\APPS>', timeout=30)
        vm.command('run \\APPS\\CVFDTEST.COM', '[CVFD-API] SYSTEM PASS', timeout=30)
        report['checks'].append('system VM short length and bad ES:DI reject without writes; valid 624-byte CVFD succeeds')
        vm.command('run \\VM\\VMFORK.COM \\APPS\\CVFDCHLD.COM',
                   '[CVFD-API] CHILD PASS', timeout=60)
        report['checks'].append('forked DOS VM request is rejected with VM_ERROR_OPERATION and leaves buffer canaries intact')
        report['status'] = 'passed'
    finally:
        vm.close()
        report['source_image_unchanged'] = sha256(image) == before
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    assert report['source_image_unchanged'], 'test modified the source image'
    print('[cache-diagnostics-api] PASS', json.dumps(report['checks']), flush=True)


if __name__ == '__main__':
    main()
