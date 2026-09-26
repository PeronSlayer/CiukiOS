#!/usr/bin/env python3
"""Boot a test ISO with a deliberately inconsistent BPB; Setup must not write.

Only the RAM source image's hidden-sector BPB count changes (63 to 64). All actual
FAT/directory/data sectors and executable code remain unchanged. The fixed
layout boot loader can still reach Setup, whose source validation must reject
the inconsistency before the user can start an installation.
"""
import argparse
import gzip
import json
from pathlib import Path
import subprocess

from qemu_test_setup_graphical import SetupVM, digest, fill_disk


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--iso', type=Path, required=True)
    ap.add_argument('--source', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    source = bytearray(args.source.read_bytes())
    assert source[63*512+28:63*512+32] == b'\x3f\0\0\0'
    source[63*512+28] = 64
    packed = out/'bad-source.img.gz'
    with packed.open('wb') as stream:
        with gzip.GzipFile(fileobj=stream,mode='wb',compresslevel=1,mtime=0) as zipper:
            zipper.write(source)
    iso = out/'bad-source.iso'
    with (out/'xorriso.log').open('w') as log:
        subprocess.run(['xorriso','-indev',str(args.iso.resolve()),'-outdev',str(iso),
                        '-boot_image','any','keep','-map',str(packed),
                        '/ciukios-full-cd-disk.img.gz','-commit'],
                       stdout=log,stderr=subprocess.STDOUT,check=True)
    disk = out/'target.img'
    if disk.exists(): ap.error('use a fresh output directory')
    fill_disk(disk,128)
    before = digest(disk)
    report={'completed':False,'changed_byte':63*512+28,'before':63,'after':64}
    vm = SetupVM([(0,disk)],iso,out)
    try:
        vm.boot_menu(1)
        vm.wait('[BOOT-SESSION] SETUP',timeout=90)
        vm.page(0)
        vm.key('ret');vm.page(1)
        vm.key('ret');vm.page(6)
        vm.shot('rejected-source',(800,600))
        text = vm.serial.read_text(errors='replace')
        assert '[SETUP-GUI] PAGE 04' not in text, 'destructive operation started'
        report['rejected_before_confirmation'] = True
    finally:
        vm.close()
        report['target_unchanged'] = digest(disk)==before
        (out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    assert report['target_unchanged'], report
    report['completed'] = True
    (out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__ == '__main__':
    main()
