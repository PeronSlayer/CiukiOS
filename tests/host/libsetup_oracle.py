#!/usr/bin/env python3
"""Independent mtools comparison of the immutable manifest subset."""
# SPDX-License-Identifier: GPL-2.0-only
import json
from pathlib import Path
import subprocess
import sys
root, volume = Path(sys.argv[1]), sys.argv[2]
m = json.loads((root / 'D/CDMANIFE.JSN').read_text())
generated = {'system/boot.cfg', 'system/setup.state', 'system/setup.log', 'system/hardware.txt', 'system/pagefile.swp'}
expected = {}
for f in m['boot_image']['files']:
    p = f['path'].lstrip('/')
    if f['component'] not in ('system', 'lwip', 'jitterentropy') or p.lower() in generated:
        continue
    expected[p.lower()] = (p, root / 'C' / p)
for f in m['iso_tree']:
    p = f['path']
    expected[p.lower()] = (p, root / 'D' / p)
listing = subprocess.check_output(['mdir', '-a', '-b', '-s', '-i', volume, '::']).decode().splitlines()
actual = {p.removeprefix('::/').lower() for p in listing if not p.endswith('/')}
immutable = {p for p in actual if p not in generated and not p.startswith('system/settings/')}
assert immutable == expected.keys(), (immutable - expected.keys(), expected.keys() - immutable)
for p, source in expected.values():
    result = subprocess.check_output(['mtype', '-i', volume, '::/' + p])
    assert result == source.read_bytes(), p
print(f'PASS mtools immutable manifest: {len(expected)} byte-equal files')
