#!/usr/bin/env python3
"""Offline import and binary-distribution notice checks; never builds an image."""
# SPDX-License-Identifier: GPL-2.0-only
import hashlib
import json
from pathlib import Path
import runpy
import sys
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
port = ROOT / 'src/kernel/net/lwip'
record = json.loads((port / 'import.json').read_text())
recipe = runpy.run_path(str(port / 'build.py'))
source = recipe['source']()
assert record['sources'] == list(recipe['SOURCES']) and len(record['sources']) == 28
assert 'api/sockets.c' not in record['sources']
assert (source / 'COPYING').read_bytes() == (port / 'COPYING').read_bytes()
assert (source / 'COPYING').read_bytes() == (ROOT / 'docs/licenses/lwip-2.2.1-BSD-3-Clause.txt').read_bytes()
assert record['patches'] == {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in (port / 'patches').glob('*.patch')}
notices = (ROOT / 'docs/licenses/lwip-2.2.1-NOTICES.txt').read_text()
assert record['sha256'] in notices
assert all('src/' + name in notices for name in record['sources'])
image = runpy.run_path(str(ROOT / 'scripts/build_image.py'))
payloads, _, _ = image['desktop_payloads']()
for name in ('lwip-2.2.1-BSD-3-Clause.txt', 'lwip-2.2.1-NOTICES.txt', 'lwip-2.2.1-import.json'):
    assert payloads['/system/licenses/' + name].is_file(), name
print('PASS offline lwIP pin/source/patch/cache/notice checks and license payload inventory; no image build')
