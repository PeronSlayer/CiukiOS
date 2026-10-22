#!/usr/bin/env python3
"""Compile libsetup using an existing SDK, without invoking its writable wrapper."""
# SPDX-License-Identifier: GPL-2.0-only
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--sdk', type=Path, required=True)
ap.add_argument('--out', type=Path, default=Path('build/host/fs/libsetup/target'))
a = ap.parse_args()
sdk = a.sdk.resolve()
m = json.loads((sdk / 'manifest.json').read_text())
cc = m['tools']['clang']['path']
assert hashlib.sha256(Path(cc).read_bytes()).hexdigest() == m['tools']['clang']['sha256'], 'pinned clang changed'
resource = subprocess.check_output([cc, '-print-resource-dir'], text=True).strip()
a.out.mkdir(parents=True, exist_ok=True)
flags = [cc, *m['flags'], '--sysroot=' + str(sdk / 'sysroot'), '-nostdinc',
         '-isystem', str(sdk / 'sysroot/include'), '-isystem', resource + '/include',
         '-Isrc/kernel/include', '-Isdk/libdisk/include', '-Isdk/libsetup/include',
         '-O2', '-Wall', '-Wextra', '-Werror']
units = ('txn', 'manifest', 'native', 'native_media')
for unit in units:
    subprocess.run([*flags, '-c', f'sdk/libsetup/{unit}.c', '-o', str(a.out / f'{unit}.o')], check=True)
subprocess.run(['ar', 'rcs', str(a.out / 'libsetup.a'), *(str(a.out / f'{u}.o') for u in units)], check=True)
nm = subprocess.check_output(['nm', '-g', '--defined-only', str(a.out / 'libsetup.a')], text=True)
seen = set()
for line in nm.splitlines():
    fields = line.split()
    if len(fields) == 3:
        symbol = fields[2]
        assert symbol not in seen, f'duplicate external symbol: {symbol}'
        seen.add(symbol)
print(f'PASS i686 libsetup.a: {len(units)} production units, pinned SDK flags, no duplicate external symbols')
