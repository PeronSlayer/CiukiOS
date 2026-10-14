#!/usr/bin/env python3
"""Deterministic, lawful data CDs; independent xorriso extraction manifests.

No boot image build or copies. Scratch trees live under the caller's build/
run directory and are removed before returning. Manifest paths are the names
in the chosen primary/Joliet hierarchy, independently extracted by xorriso.
"""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import argparse

DATE = '2020010100000000'
EPOCH = 1577836800

def generate_directory(source, output, joliet=True):
    """Python-only extension of the f1-58 directory-record fixtures."""
    from iso_directory import build
    return build(source, output, joliet)

def generate_mixed_mode(source, directory, audio_seconds=4):
    """Authored data + CD-DA media; needs a track-aware guest transport."""
    from iso_directory import mixed_mode
    return mixed_mode(source, directory, audio_seconds)

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def command(args):
    p = subprocess.run(args, capture_output=True, text=True, env=dict(os.environ, LC_ALL='C.UTF-8', TZ='UTC'))
    if p.returncode:
        raise ValueError('ISO fixture command failed: ' + ' '.join(map(str, args)) + '\n' + p.stderr[-3000:])
    return p

def generate(spec, directory, index=0):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    level = spec.get('level', 3)
    if level not in (1, 2, 3) or spec.get('seed', 1) != 1:
        raise ValueError('unsupported ISO level/seed')
    source = directory / f'iso-source-{index}'
    extracted = directory / f'iso-extracted-{index}'
    image = directory / f'fixture-{index}.iso'
    if source.exists() or extracted.exists():
        raise ValueError('ISO scratch directory already exists')
    files = {'README.TXT': b'Ciuki F4 optical fixture seed=1\n',
             'DATA/ODD.BIN': bytes((i * 17 + 3) & 255 for i in range(4099)),
             'DATA/EMPTY.BIN': b''}
    if level > 1:
        files['Long directory/Long optical file.bin'] = bytes(range(256)) * 33
    if spec.get('joliet'):
        files['Caf\u00e9/Gr\u00fc\u00dfe.txt'] = 'Ciuki: caff\u00e8, Gr\u00fc\u00dfe.\n'.encode()
    try:
        source.mkdir()
        for path, data in sorted(files.items()):
            p = source / path
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
        for p in [*source.rglob('*'), source]:
            os.utime(p, (EPOCH, EPOCH))
        args = ['xorriso', '-as', 'mkisofs', '-iso-level', str(level),
                '--norock', '-V', 'CIUKI_F4', '--modification-date=' + DATE,
                '--set_all_file_dates', DATE, '-uid', '0', '-gid', '0']
        if spec.get('joliet'):
            args += ['-J']
        args += ['-o', str(image), str(source)]
        command(args)
        extracted.mkdir()
        result = command(['xorriso', '-osirrox', 'on', '-indev', str(image), '-extract', '/', str(extracted)])
        entries = []
        for p in sorted(extracted.rglob('*')):
            name = '/' + p.relative_to(extracted).as_posix()
            entries.append({'path': name, 'path_hex': name.encode().hex(),
                            'kind': 'directory' if p.is_dir() else 'file',
                            **({} if p.is_dir() else {'size': p.stat().st_size, 'sha256': sha(p)})})
        manifest = {'generator': 'xorriso', 'level': level, 'joliet': bool(spec.get('joliet')),
                    'seed': 1, 'path': str(image.resolve()), 'size': image.stat().st_size,
                    'declared_files': {p: hashlib.sha256(b).hexdigest() for p, b in sorted(files.items())},
                    'entries': entries, 'independent_tool': 'xorriso -osirrox on -extract /',
                    'tool_version': command(['xorriso', '-version']).stdout,
                    'extract_output': (result.stdout + result.stderr)[-3000:]}
        if spec.get('malformed'):
            # Broken root extent, with agreeing little/big-endian encodings.
            with image.open('r+b') as f:
                f.seek(16 * 2048 + 158)
                extent = image.stat().st_size // 2048 + 1
                f.write(extent.to_bytes(4, 'little') + extent.to_bytes(4, 'big'))
            manifest['malformed'] = 'root_extent_outside_volume'
        manifest['sha256'] = sha(image)
        (directory / f'fixture-{index}-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        return manifest
    finally:
        for tree in (source,extracted):
            if tree.exists():
                # ISO directories are read-only after independent extraction.
                for p in [tree,*tree.rglob('*')]:
                    if p.is_dir():p.chmod(0o700)
                shutil.rmtree(tree)

def verify_records(manifest, records):
    entries = [r for r in records if r.get('case') == 'entry']
    expected = manifest['entries']
    if manifest.get('malformed'):
        if entries:
            raise ValueError('malformed ISO exposed entries')
        if not any(r.get('case') == 'malformed' and r.get('refused') == '1' for r in records):
            raise ValueError('malformed ISO was not refused')
        return
    for cycle in ('1', '2'):
        actual = [r for r in entries if r.get('cycle') == cycle]
        if len(actual) != len(expected) or len({r['path_hex'] for r in actual}) != len(actual):
            raise ValueError('optical directory listing differs from independent manifest')
        by_path = {r['path_hex']: r for r in actual}
        for entry in expected:
            r = by_path.get(entry['path_hex'])
            if not r or r.get('kind') != entry['kind']:
                raise ValueError('optical name/type differs from independent manifest')
            if entry['kind']=='file':
                hashes=[h for h in records if h.get('case')=='file' and h.get('cycle')==cycle and h.get('id')==r.get('id')]
                if len(hashes)!=1 or hashes[0].get('sha256')!=entry['sha256'] or hashes[0].get('size')!=str(entry['size']):
                    raise ValueError('optical file hash/size differs from independent manifest')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--level', type=int, choices=(1, 2, 3), default=3)
    parser.add_argument('--joliet', action='store_true')
    parser.add_argument('--malformed', action='store_true')
    parser.add_argument('--source', type=Path, help='directory for Python-only mastering')
    parser.add_argument('--audio-seconds', type=int, help='authored mixed-mode CUE/PCM fixture')
    args = parser.parse_args()
    if args.audio_seconds is not None:
        if args.source is None: parser.error('--audio-seconds requires --source')
        result = generate_mixed_mode(args.source, args.out, args.audio_seconds)
    elif args.source is not None:
        if args.malformed: parser.error('--source does not support --malformed')
        result = generate_directory(args.source, args.out/'fixture.iso', args.joliet)
    else:
        result = generate(vars(args), args.out)
    print(json.dumps(result, indent=2))
