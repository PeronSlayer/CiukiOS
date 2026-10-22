#!/usr/bin/env python3
"""Small synthetic CD source, using the actual on-disc f9-01 manifest schema."""
# SPDX-License-Identifier: GPL-2.0-only
import hashlib
import json
from pathlib import Path
import sys
out = Path(sys.argv[1])
(out / 'C/system').mkdir(parents=True, exist_ok=True)
(out / 'D/SOURCES').mkdir(parents=True, exist_ok=True)
records = []
def put(path, data, component='system', attributes=32):
    (out / 'C' / path).parent.mkdir(parents=True, exist_ok=True)
    (out / 'C' / path).write_bytes(data)
    records.append(dict(path='/' + path, size=len(data), sha256=hashlib.sha256(data).hexdigest(), component=component, attributes=attributes))
for i in range(220):
    put(f'Tree/Directory {i % 9}/Long filename number {i:03d}.bin', bytes((i + j) % 256 for j in range(4100 if i % 7 == 0 else i * 3)), attributes=33 if i % 2 else 38)
records[0]['component'] = 'lwip'
records[1]['component'] = 'jitterentropy'
put('system/RELEASE.JSON', b'{"disk_image_sha256":"' + b'1' * 64 + b'"}\n')
put('system/BOOT.CFG', b'version=1 serial=1 setup=1\n')
# These must never be copied, even if a future manifest includes them.
put('SYSTEM/PAGEFILE.SWP', b'never copy a live swap file')
put('Optional/DOS component.txt', b'DOS personality', 'dos-unified')
# Sparse read-only source makes the 1 GiB destination fail preflight without
# allocating a large host buffer or copying a disk image.
huge = out / 'C/Optional/Too large.bin'
with huge.open('wb') as f:
    f.truncate(1024 * 1024 * 1024)
zero = bytes(65536)
digest = hashlib.sha256()
for _ in range(16384):
    digest.update(zero)
records.append(dict(path='/Optional/Too large.bin', size=1024*1024*1024,
                    sha256=digest.hexdigest(), component='devtools', attributes=32))
source = b'corresponding sources fixture\n'
(out / 'D/SOURCES/CIUKIOS.ZIP').write_bytes(source)
loader = (out / 'loader.bin').read_bytes()
(out / 'D/CIUKBOOT.IMG').write_bytes(bytes(512) + loader)
directories = [dict(path='/Empty directory', attributes=16, component='system')]
(out / 'C/Empty directory').mkdir(exist_ok=True)
manifest = dict(schema_version=1, build=dict(disk_image_sha256='1' * 64, loader_sha256=hashlib.sha256(loader).hexdigest()), boot_image=dict(iso_path='CIUKBOOT.IMG', bytes=512+len(loader), boot_options='version=1 serial=1 setup=1\n', files=records, directories=directories), iso_tree=[dict(path='SOURCES/CIUKIOS.ZIP', size=len(source), sha256=hashlib.sha256(source).hexdigest())])
(out / 'D/CDMANIFE.JSN').write_text(json.dumps(manifest, sort_keys=True))
print('PASS libsetup source fixture: 220 long-name files, attributes, nested/empty directories, optional DOS and sources')
