#!/usr/bin/env python3
"""Reject unclassified fsck diagnostics; never repair interrupted images."""
from collections import Counter
from pathlib import Path
import re

root=Path(__file__).resolve().parents[3]
def classify(text):
    counts=Counter()
    cases=0
    bits=0
    oracle={}
    lines=iter(text.splitlines())
    for line in lines:
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
        elif line=='/Crash A/Crash file long.bin':
            # Append publishes its live tail before size. Only a longer chain is
            # accepted, with the unchanged published EOF and intact owned chains.
            detail=next(lines,'').strip()
            m=re.fullmatch(r'File size is (\d+) bytes, cluster chain length is > (\d+) bytes\.',detail)
            assert m,detail
            size,bound=map(int,m.groups())
            expected={12:(12588,4096),16:(3372,1024),32:(1836,512)}
            eof,cluster=expected[bits]
            assert size==eof and bound==((size+cluster-1)//cluster)*cluster
            assert next(lines,'').strip()==f'Truncating file to {size} bytes.'
            counts['owned_excess_tail']+=1
        elif line=='Long filename fragment "Renamed file." found outside a LFN sequence.':
            # The shorter same-index rename removes an old nonowning LFN part.
            assert next(lines,'').strip()=='(Maybe the start bit is missing on the last fragment)'
            assert next(lines,'').strip()=='Not auto-correcting this.'
            counts['orphan_lfn_fragment']+=1
        elif line=='Wrong checksum for long file name "R.bin".':
            # New LFN before the sole owning SFN: interrupted checksum transition
            # is discarded by the production parser, leaving the old SFN usable.
            assert next(lines,'').strip()=='(Short name RENAME~1.BIN may have changed without updating the long name)'
            assert next(lines,'').strip()=='Not auto-correcting this.'
            counts['lfn_checksum_disagreement']+=1
        else: raise AssertionError(f'unclassified FAT{bits} fsck diagnostic: {line}')
    return cases,counts


if __name__=="__main__":
    cases,counts=classify((root/"build/host/fs/crash-fsck.log").read_text())
    print(f'PASS crash fsck classification: cuts={cases} unknown_diagnostics=0 '+
          ' '.join(f'{k}={v}' for k,v in sorted(counts.items())))
