#!/usr/bin/env python3
"""Reject unclassified fsck diagnostics; never repair interrupted images."""
from collections import Counter
from pathlib import Path
import re

root=Path(__file__).resolve().parents[3]
counts=Counter()
cases=0
bits=0
oracle={}
for line in (root/'build/host/fs/crash-fsck.log').read_text().splitlines():
    line=line.strip()
    if not line: continue
    if line.startswith('FAT') and ' mode=' in line:
        bits=int(line.split()[0][3:]); cases+=1; continue
    if line.startswith('oracle '):
        oracle={k:int(v) for k,v in (s.split('=') for s in line.split()[1:])}
        assert not oracle['crosslinks'] and not oracle['corrupt']
        continue
    if line.startswith(('fsck.fat ', 'exit=', '/proc/self/fd/')): continue
    if line in {'Auto-correcting.', 'Auto-deleting.', 'Using first FAT.',
                'Automatically removing dirty bit.', 'Leaving filesystem unchanged.'}: continue
    if line=='FATs differ but appear to be intact.':
        assert oracle['divergent']; counts['divergent_fats']+=1
    elif line=='Dirty bit is set. Fs was not properly unmounted and some data may be corrupt.':
        assert oracle['dirty']; counts['dirty']+=1
    elif re.fullmatch(r'Reclaimed \d+ unused clusters? \(\d+ bytes\)\.',line):
        assert oracle['lost']; counts['lost_clusters']+=1
    elif re.fullmatch(r'Free cluster summary wrong \(\d+ vs\. really \d+\)',line):
        assert bits==32; counts['stale_fsinfo_hint']+=1
    elif re.fullmatch(r'Orphaned long file name part "[^"]+"',line):
        counts['orphan_lfn']+=1
    elif re.fullmatch(r'Cluster \d+ out of range \(\d+ > \d+\)\. Setting to EOF\.',line):
        assert bits==12 and oracle['lost']
        counts['unowned_fat12_fragment']+=1
    else: raise AssertionError(f'unclassified FAT{bits} fsck diagnostic: {line}')
print(f'PASS crash fsck classification: cuts={cases} unknown_diagnostics=0 '+
      ' '.join(f'{k}={v}' for k,v in sorted(counts.items())))
