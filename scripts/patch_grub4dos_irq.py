#!/usr/bin/env python3
"""Close the raw INT13 GDT/IRQ window in the pinned upstream GRLDR binary.

Before switching GDTs, mask interrupts. An audio IRQ handled by HDPMI can
otherwise replace GDTR after LGDT but before the later CLI/CR0 transition.
The next MOV DS then uses an unrelated descriptor table. Keep every offset
unchanged: XOR EBX,EBX (3 bytes) becomes CLI; XOR BX,BX (3 bytes). EBX's high
word is not consumed on this path; the 64-bit branch clears EBX itself.
See the equivalent source patch packaged alongside the upstream source.
"""
import argparse
import hashlib
from pathlib import Path

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('source',type=Path)
ap.add_argument('output',type=Path)
args=ap.parse_args()
data=args.source.read_bytes()
assert hashlib.sha256(data).hexdigest()=='dece3f8d20f84ae0d0fb892b5c3a2d19e7233d0d8885b0027a6f43d77239128d','unexpected GRLDR revision'
pattern=bytes.fromhex('66 31 db be 40 1e 8c cb 8e db 66 0f b6 4c 02 66 c1 e1 07 fc 2e 66 0f 01 06 72 01 2e 0f 01 16 42 01')
assert data.count(pattern)==1,'INT13 critical section is not unique'
offset=data.index(pattern)
patched=data[:offset]+bytes.fromhex('fa 31 db')+data[offset+3:]
assert len(patched)==len(data)
assert sum(a!=b for a,b in zip(data,patched))==1
args.output.write_bytes(patched)
print(f'[grub4dos] INT13 IRQ guard at {offset:#x}; sha256={hashlib.sha256(patched).hexdigest()}')
