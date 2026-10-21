#!/usr/bin/env python3
"""Assemble CIUKLDR alone and account for every byte of its fixed extent.

NASM's listing includes expanded macros, continuation rows and TIMES counts.
No kernel or disk image is built. Temporary outputs live under build/host.
"""
# SPDX-License-Identifier: GPL-2.0-only
import argparse
from collections import Counter
from pathlib import Path
import re
import subprocess
import tempfile

from ciuki_version import load

ROOT = Path(__file__).resolve().parents[1]
EXTENT = 24576
TRAIL = 32
CATEGORIES = ('header', 'code', 'tables', 'strings', 'scratch', 'padding', 'trail')


def assemble(binary, listing, version=None):
    defines = [] if version is None else [f"-dCIUKI_VERSION='{version}'"]
    subprocess.run(['nasm', '-f', 'bin', *defines, 'src/boot/ciukldr.asm',
                    '-o', str(binary), '-l', str(listing)], cwd=ROOT, check=True)


def measure(binary, listing):
    """Classify emitted listing bytes; reject gaps, overlaps or unknown forms.

    Strings are quoted data (messages, probe and option tokens, 8.3 names).
    Tables are other initialized data (state, pointers, descriptors). Scratch
    is initialized TIMES storage; padding is alignment and the free tail.
    Loader BSS after the file (`absolute`) consumes no file bytes.
    """
    data = Path(binary).read_bytes()
    if len(data) != EXTENT or data[-TRAIL:-TRAIL + 8] != b'CLDRPRG1':
        raise ValueError('expected the fixed 48-sector loader and progress trail')
    totals = Counter({name: 0 for name in CATEGORIES})
    cursor, used, kind = 0, 0, None
    for line in Path(listing).read_text().splitlines():
        # NASM -l uses fixed columns, including an optional include-depth field.
        address, encoded = line[7:15], line[16:35].strip()
        if not re.fullmatch(r'[0-9A-F]{8}', address) or not encoded or '?' in encoded or '<res ' in encoded:
            continue
        raw = encoded.rstrip('-').replace('[', '').replace(']', '').replace('(', '').replace(')', '')
        repeat = re.fullmatch(r'([0-9A-F]+)<rep ([0-9A-F]+)h>', raw)
        if repeat:
            count = len(repeat[1]) // 2 * int(repeat[2], 16)
        elif re.fullmatch(r'(?:[0-9A-F]{2})+', raw):
            count = len(raw) // 2
        else:
            raise ValueError(f'unknown NASM listing bytes: {encoded}')
        offset = int(address, 16)
        if offset != cursor:
            raise ValueError(f'listing discontinuity at {offset:#x}, expected {cursor:#x}')
        expanded = re.sub(r'^\s*(?:<\d+>\s*)?', '', line[35:])
        source = expanded.split(';', 1)[0].strip()
        if offset < 16:
            kind = 'header'
        elif offset >= EXTENT - TRAIL:
            kind = 'trail'
        elif source:
            if re.search(r'\balign\b', source) or source.startswith('times 24544-'):
                kind = 'padding'
            elif re.search(r'\btimes\b', source):
                kind = 'scratch'
            elif re.search(r'\bd[bwdq]\b', source):
                kind = 'strings' if "'" in source or '"' in source else 'tables'
            else:
                kind = 'code'
        if kind is None:
            raise ValueError('listing continuation without an initializer')
        totals[kind] += count
        cursor += count
        if offset < EXTENT - TRAIL and kind != 'padding':
            used = cursor
    if cursor != EXTENT or sum(totals.values()) != EXTENT:
        raise ValueError('listing does not account for the complete loader')
    return dict(totals, used=used, free=EXTENT - TRAIL - used, extent=EXTENT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bare', action='store_true', help='use the loader\'s unknown version fallback')
    args = parser.parse_args()
    version = None if args.bare else load()['display']
    base = ROOT / 'build/host'
    base.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='loader-budget-', dir=base) as folder:
        binary, listing = Path(folder) / 'loader.bin', Path(folder) / 'loader.lst'
        assemble(binary, listing, version)
        budget = measure(binary, listing)
    print(f'CIUKLDR byte budget ({version or "unknown"}):')
    for name in CATEGORIES:
        print(f'  {name:8s} {budget[name]:5d}')
    print(f'  used     {budget["used"]:5d} (before free tail and {TRAIL}-byte trail)')
    print(f'  free     {budget["free"]:5d}')
    print(f'  extent   {budget["extent"]:5d}')


if __name__ == '__main__':
    main()
