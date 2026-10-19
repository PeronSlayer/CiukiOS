#!/usr/bin/env python3
"""Offline, pinned FluidR3 install and deterministic reduced GM derivative.

Run with --archive build/downloads/fluid-soundfont_3.1.orig.tar.gz.
No downloading, tar extraction, external tools or non-free samples.
"""
# SPDX-License-Identifier: MIT
from array import array
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import tarfile
ARCHIVE_SHA256='2621acaa1c78e4abdb24bdd163230cc577e61276936d6aa6e3180582142f0343'
FULL_SHA256='74594e8f4250680adf590507a306655a299935343583256f3b722c48a1bc1cb0'
ARCHIVE_URL='https://deb.debian.org/debian/pool/main/f/fluid-soundfont/fluid-soundfont_3.1.orig.tar.gz'
HERE=Path(__file__).resolve().parent

def digest(data):
    return hashlib.sha256(data).hexdigest()

def chunks(data):
    at=0
    while at<len(data):
        if len(data)-at<8:
            raise ValueError('truncated RIFF chunk')
        tag,size=struct.unpack_from('<4sI',data,at)
        if size>len(data)-at-8:
            raise ValueError('RIFF chunk overflow')
        yield tag,data[at+8:at+8+size]
        at+=8+size+(size&1)
    if at!=len(data):
        raise ValueError('RIFF padding missing')

def chunk(tag,data):
    return struct.pack('<4sI',tag,len(data))+data+b'\0'*(len(data)&1)

def tables(data):
    tag,body=next(chunks(data))
    if tag!=b'RIFF' or body[:4]!=b'sfbk' or len(body)+8!=len(data):
        raise ValueError('not an exact SF2 RIFF')
    result={}
    for tag,body in chunks(body[4:]):
        if tag==b'LIST':
            for sub,payload in chunks(body[4:]):
                if sub in result:
                    raise ValueError('duplicate SF2 table')
                result[sub]=payload
    return result

def coverage(data):
    t=tables(data)
    if len(t[b'phdr'])%38 or len(t[b'shdr'])%46 or len(t[b'smpl'])%2:
        raise ValueError('unaligned SF2 table')
    presets=[struct.unpack_from('<HH',t[b'phdr'],at+20) for at in range(0,len(t[b'phdr'])-38,38)]
    missing=[p for p in range(128) if (p,0) not in presets]
    if missing or (0,128) not in presets:
        raise ValueError('incomplete GM programs/drums: '+str(missing))
    samples=len(t[b'smpl'])//2
    for at in range(0,len(t[b'shdr'])-46,46):
        start,end,loop_start,loop_end,rate=struct.unpack_from('<5I',t[b'shdr'],at+20)
        if not (start<end<samples and loop_start<=loop_end<=samples and rate):
            raise ValueError('invalid sample bounds')
    return {'gm_programs':128,'drum_bank':128,'presets':len(presets),'samples':samples}

def reduce(data,factor=8):
    """Box-filter/decimate global S16 data. Retain every preset/instrument.

    Address and loop positions use floor(index/factor), rates are divided by
    factor. A sample's pitch is preserved. Coarse offsets are rejected rather
    than silently corrupted; actual FluidR3 qualification must check this.
    """
    t=tables(data)
    coverage(data)
    raw=array('h');raw.frombytes(t[b'smpl'])
    if sys.byteorder!='little':raw.byteswap()
    out=array('h')
    for at in range(0,len(raw),factor):
        group=raw[at:at+factor]
        total=sum(group)
        # Exact signed truncation, independent of Python floor rounding.
        out.append((abs(total)//len(group))*(-1 if total<0 else 1))
    if sys.byteorder!='little':out.byteswap()
    t[b'smpl']=out.tobytes()
    sh=bytearray(t[b'shdr'])
    for at in range(0,len(sh),46):
        for off in (20,24,28,32):
            value=struct.unpack_from('<I',sh,at+off)[0]
            struct.pack_into('<I',sh,at+off,value//factor)
        rate=struct.unpack_from('<I',sh,at+36)[0]
        struct.pack_into('<I',sh,at+36,max(1,rate//factor))
    t[b'shdr']=bytes(sh)
    for name in (b'igen',b'pgen'):
        generators=bytearray(t[name])
        for at in range(0,len(generators),4):
            op,value=struct.unpack_from('<Hh',generators,at)
            if op in (4,12,45,50) and value:
                raise ValueError('nonzero coarse sample offset requires explicit conversion')
            if op in (0,1,2,3):
                struct.pack_into('<h',generators,at+2,value//factor)
        t[name]=bytes(generators)
    # Reconstruct in the original order, with deterministic RIFF sizes/padding.
    _,body=next(chunks(data));lists=[]
    for tag,body in chunks(body[4:]):
        if tag==b'LIST':
            body=body[:4]+b''.join(chunk(sub,t[sub]) for sub,_ in chunks(body[4:]))
        lists.append(chunk(tag,body))
    result=chunk(b'RIFF',b'sfbk'+b''.join(lists))
    if len(result)>32*1024*1024:
        raise ValueError('reduced bank exceeds 32 MiB')
    coverage(result)
    return result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive',required=True,type=Path)
    args=parser.parse_args()
    archive=args.archive.read_bytes()
    if digest(archive)!=ARCHIVE_SHA256:
        raise SystemExit('archive SHA-256 mismatch')
    with tarfile.open(args.archive,'r:gz') as tar:
        candidates=[m for m in tar.getmembers() if Path(m.name).name=='FluidR3_GM.sf2' and m.isfile()]
        if len(candidates)!=1:raise SystemExit('expected one GM bank')
        full=tar.extractfile(candidates[0]).read()
    if digest(full)!=FULL_SHA256:raise SystemExit('full bank SHA-256 mismatch')
    reduced=reduce(full)
    banks=[]
    for name,data in [('FluidR3_GM.sf2',full),('FluidR3_Ciuki.sf2',reduced)]:
        (HERE/name).write_bytes(data)
        banks.append({'file':name,'sha256':digest(data),'bytes':len(data),'license':'MIT',**coverage(data)})
    record={'schema':1,'archive':ARCHIVE_URL,'archive_sha256':ARCHIVE_SHA256,
            'recipe':'box-decimate-s16-factor-8-v1','banks':banks}
    (HERE/'inventory.json').write_text(json.dumps(record,indent=2,sort_keys=True)+'\n')
if __name__=='__main__':main()
