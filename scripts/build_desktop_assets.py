#!/usr/bin/env python3
"""Pack the existing font and approved Ciuki pixels without changing them."""
from pathlib import Path
import hashlib
import json
import struct

ROOT = Path(__file__).resolve().parents[1]
paths = ['src/com/setup_font.bin'] + [
    f'assets/brand/native/ciuki-{size}.bin' for size in (16,24,64)]
parts = [(ROOT / path).read_bytes() for path in paths]
assert [len(p) for p in parts] == [6270,256,576,4096]
data = struct.pack('<8s4H',b'CIUKUI01',6270,4928,11214,0) + b''.join(parts)
out = ROOT / 'assets/desktop'
out.mkdir(exist_ok=True)
(out / 'DESKTOP.DAT').write_bytes(data)
(out / 'manifest.json').write_text(json.dumps({
    'format': 'CIUKUI01', 'bytes': len(data),
    'sha256': hashlib.sha256(data).hexdigest(),
    'unchanged_inputs': {path:hashlib.sha256(part).hexdigest()
                         for path,part in zip(paths,parts)},
},indent=2)+'\n')
print('[desktop-assets] packed unchanged font and Ciuki artwork: '+str(len(data))+' bytes')
