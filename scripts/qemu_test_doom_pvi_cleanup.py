#!/usr/bin/env python3
"""Check real launcher failure cleanup with both initial PVI states and formats."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

from qemu_test_installed_hdd import FAT16, InstalledVM


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--image', required=True, type=Path)
    ap.add_argument('--output', required=True, type=Path)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    disk = out/'installed.img'
    shutil.copyfile(args.image, disk)
    fat = FAT16(disk)
    volume = f'{disk}@@{fat.start}'
    report = {'source_sha256': hashlib.sha256(args.image.read_bytes()).hexdigest(),
              'payload_sha256': {p: hashlib.sha256(fat.read(p)).hexdigest()
                                 for p in ('APPS/DOOM/DOOM.COM', 'APPS/DOOM/DOOM.EXE')},
              'cases': [], 'passed': False}
    # Remove a required child only from the disposable disk to force a real
    # DOS file-not-found return after the launcher's PVI setup.
    subprocess.run(['mdel', '-i', volume, '::SBEMU/HDPMI32I.EXE'], check=True)
    for name, instruction in (('PVISET', 'or al,2'), ('PVICLEAR', 'and al,0xfd')):
        asm = out/(name+'.asm')
        binary = out/(name+'.COM')
        asm.write_text(f'bits 16\norg 0x100\nmov eax,cr4\n{instruction}\nmov cr4,eax\nmov ax,0x4c00\nint 0x21\n')
        subprocess.run(['nasm', '-f', 'bin', str(asm), '-o', str(binary)], check=True)
        subprocess.run(['mcopy', '-i', volume, str(binary), '::APPS/'+binary.name], check=True)
    vm = InstalledVM(disk, out)

    def cr4(name):
        data = vm.hmp('info registers')
        (out/(name+'.log')).write_bytes(data)
        return int(re.search(rb'CR4=([0-9a-fA-F]+)', data)[1], 16)

    try:
        vm.wait('[DESKTOP] READY', timeout=90)
        vm.key('f4')
        vm.wait('CiukiOS SHELL C:\\APPS>')
        for initial in (0, 2):
            vm.result('pviset' if initial else 'pviclear')
            for fmt, command in (('mz', 'doom'), ('com', 'run \\APPS\\DOOM\\DOOM.COM')):
                name = f'{fmt}-initial-{initial}'
                before = cr4(name+'-before')
                assert before & 2 == initial
                offset = vm.offset()
                vm.text(command)
                vm.wait('[DOOM] EXEC FAIL', offset, 60)
                vm.wait('CiukiOS SHELL C:\\APPS>', offset, 60)
                after = cr4(name+'-after')
                assert before == after, (name, before, after)
                vm.result('comdemo', 'COM demo via INT21h')
                report['cases'].append({'format': fmt, 'before_cr4': before, 'after_cr4': after})
        vm.result('pviclear')
        vm.result('echo LAUNCHER CLEANUP READY', 'LAUNCHER CLEANUP READY')
        vm.shot('cleanup')
        report['passed'] = True
    finally:
        vm.close()
        assert report['source_sha256'] == hashlib.sha256(args.image.read_bytes()).hexdigest()
        (out/'result.json').write_text(json.dumps(report, indent=2)+'\n')
        print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    main()
