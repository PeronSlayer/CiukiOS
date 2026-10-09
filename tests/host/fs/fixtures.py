#!/usr/bin/env python3
"""Independent mkfs.fat/mtools fixtures; all paths beneath build/host/fs."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/host/fs'
SEED = 0xC1A001
ENV = dict(os.environ, LC_ALL='C.UTF-8', MTOOLS_SKIP_CHECK='1', MTOOLS_NO_VFAT='0', MTOOLSRC=str(OUT/'mtoolsrc'))

def run(*args):
    return subprocess.run(args, check=True, env=ENV, capture_output=True, text=True).stdout

def geometry(path):
    b = path.open('rb').read(512)
    u16 = lambda n: struct.unpack_from('<H', b, n)[0]
    u32 = lambda n: struct.unpack_from('<I', b, n)[0]
    fat = u16(22) or u32(36)
    total = u16(19) or u32(32)
    root = (u16(17)*32+511)//512
    return dict(sector_size=u16(11), spc=b[13], reserved=u16(14), fats=b[16], fat_sectors=fat,
                root_entries=u16(17), sectors=total, clusters=(total-u16(14)-b[16]*fat-root)//b[13])

def main():
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT/'mtoolsrc').write_text('default_codepage=437\n')
    payload = bytes(((i*37+(i>>8)+SEED)&255) for i in range(12345))
    (OUT/'seed.bin').write_bytes(payload)
    os.utime(OUT/'seed.bin', (946684800, 946684800))
    report = dict(seed=SEED, mkfs=run('mkfs.fat','--help').splitlines()[-1],
                  mtools=run('mcopy','-V').splitlines()[0], fixtures={})
    for bits, mib, spc in [(12,12,8),(16,16,2),(32,40,1)]:
        path = OUT/f'fat{bits}.img'
        with path.open('wb') as f: f.truncate(mib*1024*1024)
        output=run('mkfs.fat','--invariant','-F',str(bits),'-S','512','-s',str(spc),'-f','2','-i','00C1A001',str(path))
        run('mcopy','-m','-i',str(path),str(OUT/'seed.bin'),'::/Seed data with spaces.bin')
        run('mcopy','-m','-i',str(path),str(OUT/'seed.bin'),'::/SHORT.BIN')
        run('mcopy','-m','-i',str(path),str(OUT/'seed.bin'),'::/café.txt')
        run('mmd','-i',str(path),'::/Fixture dir')
        run('mcopy','-m','-i',str(path),str(OUT/'seed.bin'),'::/Fixture dir/Nested.bin')
        if bits == 12:
            boundary = bytes(((i*37+(i>>8)+SEED)&255) for i in range(350*4096+123))
            (OUT/'boundary.bin').write_bytes(boundary)
            run('mcopy','-i',str(path),str(OUT/'boundary.bin'),'::/Boundary.bin')
        fsck=run('fsck.fat','-n',str(path))
        g=geometry(path)
        report['fixtures'][str(bits)]=dict(bytes=path.stat().st_size, geometry=g,
            sha256=hashlib.file_digest(path.open('rb'),'sha256').hexdigest(), payload_sha256=hashlib.sha256(payload).hexdigest(),
            listing=run('mdir','-i',str(path),'::/'), fsck=fsck, mkfs_output=output)
        print(f'fixture FAT{bits}: bytes={path.stat().st_size} sectors={g["sectors"]} spc={g["spc"]} clusters={g["clusters"]} seed={SEED}')
    (OUT/'fixtures.json').write_text(json.dumps(report,indent=2)+'\n')
    print('tools: '+report['mkfs']+'; '+report['mtools'])

if __name__=='__main__': main()
