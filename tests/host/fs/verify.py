#!/usr/bin/env python3
"""Compare production enumeration with mtools paths, sizes and content hashes."""
import os
from pathlib import Path
import subprocess
import sys

root=Path(__file__).resolve().parents[3]
out=root/'build/host/fs'
image=Path(sys.argv[1])
env=dict(os.environ, LC_ALL='C.UTF-8', MTOOLS_SKIP_CHECK='1', MTOOLSRC=str(out/'mtoolsrc'))
def run(args, text=True):
    return subprocess.run(args,check=True,capture_output=True,text=text,env=env).stdout

def digest(data):
    h=2166136261
    for x in data: h=((h^x)*16777619)&0xffffffff
    return f'{h:08x}'

ours={}
for line in run([str(out/'test_fs'),'manifest',str(image)]).splitlines():
    fields=line.split('\t')
    if fields[0]=='D': ours[fields[1]]=('D',)
    else: ours[fields[1]]=('F',int(fields[2]),fields[3])
expected={}
for name in run(['mdir','-b','-s','-a','-i',str(image),'::/']).splitlines():
    assert name.startswith('::/'),name
    path=name[2:]
    if path.endswith('/'): expected[path[:-1]]=('D',)
    else:
        data=run(['mtype','-i',str(image),name],False)
        expected[path]=('F',len(data),digest(data))
assert ours==expected,dict(missing=set(expected)-set(ours),extra=set(ours)-set(expected),
                         differing=[p for p in ours.keys()&expected.keys() if ours[p]!=expected[p]])
print(f'PASS mtools equality: {image.name} entries={len(ours)} names/sizes/FNV1a32 match')
