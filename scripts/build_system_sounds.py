#!/usr/bin/env python3
"""Convert the pinned CC0 Kenney files to the native event-player formats."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


EVENTS = {'ERROR': 'error_001', 'INFO': 'confirmation_001',
          'WARN': 'question_002', 'CONFIRM': 'confirmation_003'}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, default=Path('assets/sounds/native'))
    args = ap.parse_args()
    upstream = Path('assets/sounds/kenney-interface')
    provenance = json.loads((upstream/'UPSTREAM.json').read_text())
    args.output.mkdir(parents=True, exist_ok=True)
    manifest = {'license': 'CC0-1.0', 'source': provenance['source'], 'events': {}}
    for event, original in EVENTS.items():
        source = upstream/'Audio'/f'{original}.ogg'
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        if digest != provenance['files'][source.name]:
            raise SystemExit(f'Upstream hash mismatch: {source}')
        formats = {}
        for suffix, codec, channels, rate in [('PCM','s16le',2,48000),('SB','u8',1,8000)]:
            target = args.output/f'{event}.{suffix}'
            subprocess.run(['ffmpeg','-nostdin','-v','error','-y','-i',str(source),
                            '-ac',str(channels),'-ar',str(rate),'-f',codec,str(target)], check=True)
            payload = target.read_bytes()
            if not 0 < len(payload) <= (65532 if suffix == 'PCM' else 4096):
                raise SystemExit(f'Event exceeds native DMA buffer: {target}')
            formats[suffix] = {'bytes':len(payload),'sha256':hashlib.sha256(payload).hexdigest()}
        manifest['events'][event] = {'original':source.name, 'outputs':formats}
    shutil.copyfile(upstream/'License.txt', args.output/'LICENSE.TXT')
    shutil.copyfile('assets/sounds/CREDITS.TXT', args.output/'CREDITS.TXT')
    (args.output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')


if __name__ == '__main__':
    main()
