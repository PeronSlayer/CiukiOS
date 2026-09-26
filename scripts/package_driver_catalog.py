#!/usr/bin/env python3
"""Install the canonical, licensed Drivers tree into a FAT image via mtools."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', help='FAT image, optionally with @@byte-offset')
    parser.add_argument('--objects', type=Path, default=Path('build/full/obj'))
    parser.add_argument('--video', type=Path, default=Path('build/external/video-compat/output'))
    args = parser.parse_args()
    entries = json.loads(Path('assets/drivers/payloads.json').read_text())['payloads']
    native = {
        'hwdetect.com': 'DRIVERS/HWDETECT.COM',
        'inputini.com': 'DRIVERS/INPUTINI.COM',
        'sb16init.com': 'DRIVERS/AUDIO/SB16INIT.COM',
        'ac97init.com': 'DRIVERS/AUDIO/AC97INIT.COM',
        'bootsnd.com': 'DRIVERS/AUDIO/SOUND.COM',
        'audio.com': 'DRIVERS/AUDIO/AUDIO.COM',
        'audiotst.com': 'DRIVERS/AUDIO/AUDIOTST.COM',
        'audiokey.com': 'DRIVERS/AUDIO/AUDIOKEY.COM',
        'sbstart.com': 'DRIVERS/AUDIO/SBSTART.COM',
        'sbeminit.com': 'DRIVERS/AUDIO/SBEMINIT.COM',
        'vgasetup.com': 'DRIVERS/VIDEO/VGASETUP.COM',
        'drvload.com': 'DRIVERS/DRVLOAD.COM',
        'mouse.com': 'DRIVERS/MOUSE/MOUSE.COM',
    }
    entries += [dict(source=str(args.objects / source), target=target)
                for source, target in native.items()]
    for name in ('AUXSTACK.COM', 'AUXCHECK.COM', 'VIDMODES.COM',
                 'VBESVGA.DRV', 'VDDVBE.386', 'VBEVMDIB.3GR',
                 'VBESVGA.TXT', 'SOURCE.TXT'):
        entries.append(dict(source=str(args.video / name), target=f'DRIVERS/VIDEO/{name}'))
    prepared = []
    for entry in entries:
        source = Path(entry['source'])
        target = PurePosixPath(entry['target'])
        assert target.parts[0] == 'DRIVERS' and '..' not in target.parts
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        if 'sha256' in entry and digest != entry['sha256']:
            raise ValueError(f'Unverified driver payload: {source}')
        prepared.append((source, target, digest))
    directories = sorted({p for _, t, _ in prepared for p in t.parents
                          if str(p) != '.'}, key=lambda p: (len(p.parts), str(p)))
    for directory in directories:
        target = '::' + str(directory)
        exists = subprocess.run(['mdir', '-i', args.image, target],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if exists.returncode:
            subprocess.run(['mmd', '-i', args.image, target], check=True)
    for source, target, digest in prepared:
        subprocess.run(['mcopy', '-o', '-i', args.image, str(source),
                        '::' + str(target)], check=True)
    print(f'[drivers] Packaged {len(prepared)} files with source/license records')


if __name__ == '__main__':
    main()
