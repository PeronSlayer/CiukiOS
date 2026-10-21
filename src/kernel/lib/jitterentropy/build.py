"""Pinned, offline jitterentropy kernel/host recipe; extract only under build/."""
# SPDX-License-Identifier: GPL-2.0-only
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
ROOT = Path(__file__).resolve().parents[4]
HERE = Path(__file__).resolve().parent
SHA256 = 'f5eaccc9d2977c83308651be9379f09f34348398f419e8f8b5bbd95928c777ed'

def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

def source():
    pin = json.loads((ROOT / 'config/jitterentropy.json').read_text())
    archive = ROOT / pin['archive']
    if pin['sha256'] != SHA256 or not archive.is_file() or archive.stat().st_size != 121930 or digest(archive) != SHA256:
        raise ValueError('missing/changed pinned jitterentropy 3.7.0 archive; no download/fallback')
    folder = ROOT / 'build/external/jitterentropy-library-3.7.0'
    patches = sorted((HERE / 'patches').glob('*.patch'))
    hashes = {p.name: digest(p) for p in patches}
    if hashes != pin['patches']:
        raise ValueError('unrecorded/changed jitterentropy patch')
    stamp = folder / 'ciuki-source-hashes.json'
    if stamp.exists():
        saved = json.loads(stamp.read_text())
        if saved.get('patches') == hashes and set(saved.get('files', {})) == set(pin['files']) and all(
                (folder / n).is_file() and digest(folder / n) == h for n, h in saved['files'].items()):
            return folder
    if folder.exists():
        shutil.rmtree(folder)
    folder.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as stream:
        wanted = set(pin['files'] + pin['licenses'] + ['LICENSE', 'README.md'])
        members = []
        for item in stream.getmembers():
            path = Path(item.name)
            if path.is_absolute() or '..' in path.parts or path.parts[0] != folder.name:
                raise ValueError('unsafe jitterentropy archive member')
            relative = str(Path(*path.parts[1:]))
            if relative in wanted:
                if not item.isfile():
                    raise ValueError('jitterentropy import contains a non-regular file')
                members.append(item)
        if {str(Path(*Path(m.name).parts[1:])) for m in members} != wanted:
            raise ValueError('jitterentropy archive lacks imported files')
        stream.extractall(folder.parent, members=members, filter='data')
    for patch in patches:
        subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i', str(patch)], cwd=folder,
                       check=True, stdout=subprocess.DEVNULL)
    stamp.write_text(json.dumps({'patches': hashes, 'files': {n: digest(folder / n) for n in pin['files']}}, sort_keys=True)+'\n')
    return folder

def includes(src):
    return ['-I', str(src), '-I', str(src / 'src'), '-include', str(HERE / 'platform.h')]

def sources(src):
    pin = json.loads((ROOT / 'config/jitterentropy.json').read_text())
    return [src / n for n in pin['sources']] + [HERE / 'port.c']
