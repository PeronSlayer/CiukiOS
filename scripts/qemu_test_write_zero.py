#!/usr/bin/env python3
"""Test zero-byte DOS file resize on an HDD, checking guest I/O and physical FAT."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

from qemu_test_installed_hdd import FAT16
from qemu_test_full_display_profile import VM


CASES = [('CXEXT1', 9468, 16124), ('CXEXT2', 9468, 70013),
         ('CXSHRK', 70013, 4097), ('CXBND', 70013, 8192),
         ('CXZERO', 70013, 0), ('CXFROM', 0, 12345)]


def chain_numbers(fs, start):
    chain = []
    while 2 <= start < 0xfff8:
        assert start not in chain, 'FAT cycle'
        chain.append(start)
        start, = struct.unpack_from('<H', fs.data, fs.fat + 2 * start)
    return chain


def geometry(fs):
    boot = fs.data[fs.start:fs.start+512]
    spf, = struct.unpack_from('<H', boot, 22)
    total, = struct.unpack_from('<H', boot, 19)
    if not total:
        total, = struct.unpack_from('<I', boot, 32)
    clusters = (total - (fs.clusters-fs.start)//512)//fs.spc
    return spf, clusters


def free_clusters(fs):
    _, count = geometry(fs)
    return {n for n in range(2, count+2)
            if struct.unpack_from('<H', fs.data, fs.fat+n*2)[0] == 0}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--image', type=Path, default=Path(
        'build/full/t23-runtime-repair-2026-09-06/final-install/target.img'))
    ap.add_argument('--kernel', required=True, type=Path)
    ap.add_argument('--shell', required=True, type=Path)
    ap.add_argument('--output', required=True, type=Path)
    ap.add_argument('--full-volume', action='store_true')
    ap.add_argument('--expect-failure', action='store_true')
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    disk = out/'installed.img'
    shutil.copyfile(args.image, disk)
    fs = FAT16(disk)
    volume = f'{disk}@@{fs.start}'

    def inject(path, target):
        subprocess.run(['mcopy', '-o', '-i', volume, str(path), '::'+target], check=True)

    inject(args.kernel, 'SYSTEM/CIUKIDOS.SYS')
    inject(args.shell, 'SYSTEM/SHELL.COM')
    cfg = out/'DISPLAY.CFG'
    cfg.write_bytes(b'TEXT')
    inject(cfg, 'SYSTEM/VIDEO/DISPLAY.CFG')
    probe = out/'CX0TEST.COM'
    subprocess.run(['nasm', '-f', 'bin', 'scripts/fixtures/write_zero_resize.asm',
                    *( ['-D', 'FULL_VOLUME=1'] if args.full_volume else []),
                    '-o', str(probe)], check=True)
    inject(probe, 'APPS/CX0TEST.COM')
    cases = [('CXFULL', 9468, 70013)] if args.full_volume else CASES
    originals = {}
    for index, (name, initial, target) in enumerate(cases):
        payload = bytes((offset * 37 + (offset >> 8) + index * 19) & 255
                        for offset in range(initial))
        originals[name] = payload
        path = out/(name+'.BIN')
        path.write_bytes(payload)
        inject(path, 'APPS/'+path.name)
    if args.full_volume:
        # A deliberately full *test image*: retain two free clusters so that
        # extension allocates a partial tail before it must roll back.
        fs = FAT16(disk)
        free = sorted(free_clusters(fs))
        spf, _ = geometry(fs)
        with disk.open('r+b') as handle:
            for cluster in free[2:]:
                for fat in range(2):
                    handle.seek(fs.fat + fat * spf * 512 + cluster * 2)
                    handle.write(b'\xf8\xff')
    before = FAT16(disk)
    before_chains = {name: chain_numbers(before, before.entry('APPS/'+name+'.BIN')[1])
                     for name, _, _ in cases}
    before_free = free_clusters(before)
    results = {'kernel_sha256': hashlib.sha256(args.kernel.read_bytes()).hexdigest(),
               'full_volume': args.full_volume, 'completed': False, 'cases': []}
    vm = VM(disk, out)
    try:
        vm.wait('CiukiOS SHELL C:\\APPS>', timeout=90)
        marker = '[CX0] FAIL' if args.expect_failure else '[CX0] ALL PASS'
        vm.command('cx0test', marker, timeout=120)
        vm.command('echo CX0-SHELL-ALIVE', 'CX0-SHELL-ALIVE')
        vm.shot('result')
    finally:
        vm.close()
    after = FAT16(disk)
    for name, initial, target in cases:
        entry = after.entry('APPS/'+name+'.BIN')
        chain = chain_numbers(after, entry[1])
        actual = after.read('APPS/'+name+'.BIN')
        result = {'name': name, 'initial': initial, 'target': target,
                  'entry_size': entry[2], 'read_bytes': len(actual),
                  'before_chain': before_chains[name], 'after_chain': chain}
        results['cases'].append(result)
        if args.expect_failure:
            continue
        expected = initial if args.full_volume else target
        assert entry[2] == len(actual) == expected, result
        assert len(chain) == (expected + after.spc*512-1)//(after.spc*512), result
        assert actual[:min(initial, expected)] == originals[name][:expected], result
        if args.full_volume:
            assert chain == before_chains[name], result
        else:
            copied = after.read('APPS/'+name+'.CPY')
            assert copied == actual, f'{name}: guest copy did not read all bytes correctly'
            result['copied_sha256'] = hashlib.sha256(copied).hexdigest()
            retained = min(len(chain), len(before_chains[name]))
            assert chain[:retained] == before_chains[name][:retained], result
    after_free = free_clusters(after)
    results['free_clusters_before'] = len(before_free)
    results['free_clusters_after'] = len(after_free)
    spf, _ = geometry(after)
    assert after.data[after.fat:after.fat+spf*512] == after.data[after.fat+spf*512:after.fat+2*spf*512], 'FAT copies differ'
    if not args.expect_failure:
        if args.full_volume:
            assert after_free == before_free, 'partial extension leaked clusters'
        else:
            source_delta = sum(len(chain_numbers(after, after.entry('APPS/'+name+'.BIN')[1]))
                               - len(before_chains[name]) for name, _, _ in cases)
            copy_allocations = sum(len(chain_numbers(after, after.entry('APPS/'+name+'.CPY')[1]))
                                   for name, _, _ in cases)
            assert len(before_free)-len(after_free) == source_delta+copy_allocations, 'resize leaked clusters'
    else:
        first = results['cases'][0]
        assert first['entry_size'] > first['read_bytes'], 'baseline did not reproduce short allocation'
    results['completed'] = True
    (out/'results.json').write_text(json.dumps(results, indent=2)+'\n')
    print(json.dumps(results, indent=2), flush=True)


if __name__ == '__main__':
    main()
