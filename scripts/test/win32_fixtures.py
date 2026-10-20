#!/usr/bin/env python3
"""f5-17B workload fixture media for the `installers` and `titles` runner tiers.

A case declares `{"generator": "win32-workload", "workload": <id>, "media":
fat16|fat32|iso|mixed, "source": extracted|installed, "redistribution":
"local-only"}`. The payload is the lead's local extraction of the verified
download (`build/downloads/win32/<id>/extracted/`, or `installed/` for a
reference-installed tree) or, for owner-local rows, the directory of the
manifest's `path_pattern`; it never enters the canonical image, the
repository or a release. The generator builds, inside the run directory only:

* a FAT16/FAT32 volume (`mkfs.fat --invariant`, mtools) with the tree copied
  in sorted order and every directory timestamp normalised to 2000-01-01, so
  the image SHA-256 depends on the file bytes and names alone; or
* a Joliet ISO 9660 image (`iso_directory.build`, Python only) or the data
  track of a mixed-mode image (`iso_directory.mixed_mode`); QEMU's `ide-cd`
  attaches the data track only, the CD-DA track waits for the f5-12B2
  transport.

Every image carries `\\CIUKI-ID.TXT` with the payload identity: the SHA-256 of
the sorted `path\\0size\\0sha256` lines of the tree. The guest probe echoes it as
`payload_sha256` in its identity record, which ties the serial evidence to the
exact payload (win32_evidence.Parser). An absent tree is `WorkloadAbsent`
(an EvidenceNotRun: the runner records not_run reason=fixtures_absent without
launching QEMU). Deterministic layout: Microsoft FAT specification sections
3-6 (https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf),
ECMA-119/Joliet through iso_directory.py.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dos_fixtures  # noqa: E402
import fat_fixtures  # noqa: E402
import iso_fixtures  # noqa: E402
from evidence import EvidenceNotRun  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
GENERATOR = 'win32-workload'
DISK_MEDIA = ('fat16', 'fat32')
OPTICAL_MEDIA = ('iso', 'mixed')
MEDIA = DISK_MEDIA + OPTICAL_MEDIA
SOURCES = ('extracted', 'installed')
DOWNLOADS = 'build/downloads/win32'
REDISTRIBUTION = 'local-only'
IDENTITY_FILE = 'CIUKI-ID.TXT'
VOLUME_ID = '43494b57'          # 'CIKW'
LABEL = 'CIUKIWIN32'
MAX_FILES = 20000
MAX_BYTES = 1536 * 1024 ** 2    # the runner's 2 GiB test-runs budget minus the overlay
MIN_BYTES = {'fat16': 8 * 1024 ** 2, 'fat32': 64 * 1024 ** 2}
FORBIDDEN = set('"*/:<>?\\|') | {chr(c) for c in range(32)}
# FAT directory entry timestamp fields (FAT spec table on page 23):
# 13 CrtTimeTenth, 14-15 CrtTime, 16-17 CrtDate, 18-19 LstAccDate, 22-23 WrtTime, 24-25 WrtDate.
CREATION_STAMP = bytes.fromhex('00000021282128')   # 2000-01-01 00:00:00, accessed 2000-01-01
WRITE_STAMP = bytes.fromhex('00002128')            # 2000-01-01 00:00:00


class WorkloadAbsent(dos_fixtures.FixturesAbsent):
    """The declared local tree is missing; the runner records not_run."""


class WorkloadUnqualified(EvidenceNotRun):
    """The archive has no reviewed digest pin; extraction cannot qualify it."""


def is_disk(fixture):
    return fixture.get('generator') == GENERATOR and fixture.get('media') in DISK_MEDIA


def is_optical(fixture):
    return fixture.get('generator') == GENERATOR and fixture.get('media') in OPTICAL_MEDIA


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def workload_row(workload, root=ROOT):
    root = Path(root)
    if str(root) not in sys.path:
        sys.path.insert(0, str(root))
    from scripts.win32 import workloads
    manifest = workloads.load(root / 'tests/win32/workloads.json')
    for row in manifest['workloads']:
        if row['id'] == workload:
            return row
    raise ValueError('unknown workload: ' + str(workload))


def validate(fixture, root=ROOT):
    if fixture.get('generator') != GENERATOR:
        raise ValueError('not a Win32 workload fixture')
    if fixture.get('media') not in MEDIA:
        raise ValueError('Win32 workload media must be one of ' + ', '.join(MEDIA))
    if fixture.get('source', 'extracted') not in SOURCES:
        raise ValueError('Win32 workload source must be extracted or installed')
    if fixture.get('redistribution') != REDISTRIBUTION:
        raise ValueError('Win32 workload fixtures are local-only; declare redistribution')
    extra = set(fixture) - {'generator', 'workload', 'media', 'source', 'redistribution', 'audio_seconds'}
    if extra:
        raise ValueError('unknown Win32 workload fixture fields: ' + ', '.join(sorted(extra)))
    if 'audio_seconds' in fixture and (fixture['media'] != 'mixed' or type(fixture['audio_seconds']) is not int
                                       or not 4 <= fixture['audio_seconds'] <= 60):
        raise ValueError('audio_seconds requires mixed media and 4..60 integral seconds')
    return workload_row(fixture.get('workload'), root)


def source_directory(row, fixture, root=ROOT):
    """Where the lead's local tree lives; never fetched here."""
    source = fixture.get('source', 'extracted')
    if row['licence']['class'] == 'owner-local':
        pattern = Path(row['source']['path_pattern'])
        base = Path(root) / (pattern.parent if pattern.name in ('*', '**') else pattern)
    else:
        base = Path(root) / DOWNLOADS / row['id']
    return base / source


def fat_name_ok(name):
    if name in ('', '.', '..') or name != name.rstrip(' .') or len(name) > 255:
        return False
    return not any(c in FORBIDDEN or ord(c) > 0xffff for c in name)


def scan(source):
    """Sorted regular files and directories below `source`; refuse anything else."""
    source = Path(source)
    if not source.is_dir():
        raise WorkloadAbsent('fixtures_absent: ' + str(source))
    if source.is_symlink():
        raise ValueError('workload source must not be a symbolic link')
    files, directories, total, names = [], [], 0, set()
    for path in sorted(source.rglob('*'), key=lambda p: p.relative_to(source).as_posix()):
        relative = path.relative_to(source).as_posix()
        if relative.casefold() in names or relative.casefold() == IDENTITY_FILE.casefold():
            raise ValueError('ambiguous or reserved workload name: ' + relative)
        names.add(relative.casefold())
        if path.is_symlink() or not (path.is_file() or path.is_dir()):
            raise ValueError('symbolic link or special file in workload tree: ' + relative)
        if not all(fat_name_ok(part) for part in relative.split('/')):
            raise ValueError('name not representable on FAT/ISO media: ' + relative)
        if len(names) > MAX_FILES:
            raise ValueError('workload tree exceeds the entry budget')
        if path.is_dir():
            directories.append(relative)
            continue
        size = path.stat().st_size
        total += size
        files.append({'path': relative, 'size': size, 'sha256': sha(path)})
        if len(files) > MAX_FILES:
            raise ValueError('workload tree exceeds %d files' % MAX_FILES)
        if total > MAX_BYTES:
            raise ValueError('workload tree exceeds the fixture byte budget')
    if not files:
        raise ValueError('workload tree holds no regular file: ' + str(source))
    return {'files': files, 'directories': directories, 'total_bytes': total}


def payload_identity(files):
    h = hashlib.sha256()
    for item in sorted(files, key=lambda f: f['path']):
        h.update(item['path'].encode('utf-8') + b'\0' + str(item['size']).encode() + b'\0' + item['sha256'].encode() + b'\n')
    return h.hexdigest()


def identity_text(workload, media, identity):
    return ('CiukiOS f5-17B workload fixture\r\nworkload=%s\r\nmedia=%s\r\npayload_sha256=%s\r\n'
            % (workload, media, identity)).encode('ascii')


def tool(args, limit=65536):
    result = subprocess.run([str(a) for a in args], capture_output=True, timeout=600,
                            env=dict(os.environ, LC_ALL='C', TZ='UTC', MTOOLS_SKIP_CHECK='1'))
    output = (result.stdout + result.stderr).decode(errors='replace')
    if result.returncode:
        raise ValueError('Win32 fixture command failed: ' + ' '.join(map(str, args)) + '\n' + output[-2000:])
    return output[:limit]


def fat_size(scanned, media):
    size = int(scanned['total_bytes'] * 1.10) + (len(scanned['files']) + len(scanned['directories'])) * 8192 + 4 * 1024 ** 2
    size = max(size, MIN_BYTES[media])
    size = (size + 1024 ** 2 - 1) // 1024 ** 2 * 1024 ** 2
    if size > MAX_BYTES:
        raise ValueError('FAT fixture would exceed the byte budget')
    return size


def normalize_timestamps(image):
    """Rewrite every directory entry's creation/access/write stamps (all
    directories, FAT16 root region or FAT32 root chain): mtools stamps them
    from the host clock, which would make the image hash depend on the run time."""
    image = Path(image)
    with image.open('r+b') as stream:
        boot = stream.read(512)
        g = fat_fixtures.geometry(boot)
        if g['kind'] not in (16, 32):
            raise ValueError('timestamp normalisation supports FAT16/FAT32 only')
        width = g['kind'] // 8
        mask = 0xffff if width == 2 else 0x0fffffff
        stream.seek(g['reserved'] * 512)
        fat = stream.read(g['fat_sectors'] * 512)

        def chain(first):
            clusters, seen = [], set()
            cluster = first
            while 2 <= cluster <= g['clusters'] + 1:
                if cluster in seen or len(clusters) > g['clusters']:
                    raise ValueError('FAT chain loops')
                seen.add(cluster)
                clusters.append(cluster)
                cluster = int.from_bytes(fat[cluster * width:(cluster + 1) * width], 'little') & mask
            return clusters

        def extents(first):
            return [((g['data'] + (c - 2) * g['spc']) * 512, g['spc'] * 512) for c in chain(first)]

        if g['kind'] == 16:
            root_sectors = g['data'] - g['reserved'] - g['fats'] * g['fat_sectors']
            todo = [[(g['root'] * 512, root_sectors * 512)]]
        else:
            todo = [extents(g['root_cluster'])]
        visited, normalised = set(), 0
        while todo:
            for offset, length in todo.pop():
                stream.seek(offset)
                data = bytearray(stream.read(length))
                for at in range(0, len(data), 32):
                    first = data[at]
                    if first == 0:
                        break
                    attr = data[at + 11]
                    if first == 0xe5 or attr == 0x0f or attr & 0x08:
                        continue
                    data[at + 13:at + 20] = CREATION_STAMP
                    data[at + 22:at + 26] = WRITE_STAMP
                    normalised += 1
                    if attr & 0x10 and data[at:at + 11].rstrip(b' ') not in (b'.', b'..'):
                        cluster = int.from_bytes(data[at + 26:at + 28], 'little')
                        if width == 4:
                            cluster |= int.from_bytes(data[at + 20:at + 22], 'little') << 16
                        if cluster not in visited:
                            visited.add(cluster)
                            todo.append(extents(cluster))
                stream.seek(offset)
                stream.write(data)
    return normalised


def fat_image(scanned, source, image, media, identity):
    image = Path(image)
    with image.open('wb') as stream:
        stream.truncate(fat_size(scanned, media))
    cluster_args = []
    if media == 'fat16':
        spc = 1
        while image.stat().st_size // (512 * spc) > 60000:
            spc *= 2
        cluster_args = ['-s', str(spc)]
    tool(['mkfs.fat', '--invariant', '-F', media[3:], *cluster_args, '-i', VOLUME_ID, '-n', LABEL, image])
    for directory in scanned['directories']:            # sorted: parents precede children
        tool(['mmd', '-i', image, '::/' + directory])
    identity_path = image.with_name(image.stem + '-' + IDENTITY_FILE)
    identity_path.write_bytes(identity)
    tool(['mcopy', '-i', image, identity_path, '::/' + IDENTITY_FILE])
    identity_path.unlink()
    by_directory = {}
    for item in scanned['files']:
        parent = item['path'].rpartition('/')[0]
        by_directory.setdefault(parent, []).append(Path(source) / item['path'])
    for parent in sorted(by_directory):
        names = by_directory[parent]
        for start in range(0, len(names), 200):
            tool(['mcopy', '-i', image, *names[start:start + 200], '::/' + parent + ('/' if parent else '')])
    normalised = normalize_timestamps(image)
    checker = tool(['fsck.fat', '-n', image])
    listing = tool(['mdir', '-i', image, '-/', '::/'], 16384)
    return {'checkers': {'fsck': checker, 'listing': listing}, 'normalised_entries': normalised}


def iso_image(scanned, source, directory, index, media, identity, audio_seconds):
    staging = Path(directory) / f'win32-iso-source-{index}'
    if staging.exists():
        raise ValueError('ISO staging directory already exists')
    staging.mkdir()
    try:
        for relative in scanned['directories']:
            (staging / relative).mkdir()
        for item in scanned['files']:
            target = staging / item['path']
            try:
                os.link(Path(source) / item['path'], target)
            except OSError:
                shutil.copyfile(Path(source) / item['path'], target)
        (staging / IDENTITY_FILE).write_bytes(identity)
        image = Path(directory) / f'fixture-{index}.iso'
        if media == 'iso':
            manifest = iso_fixtures.generate_directory(staging, image, joliet=True)
            extra = {}
        else:
            mixed = iso_fixtures.generate_mixed_mode(staging, Path(directory) / f'fixture-{index}-mixed', audio_seconds)
            # QEMU's ide-cd reads the data track only; a hard link (copy as the
            # fallback) keeps one copy of a possibly large track on disk.
            try:
                os.link(mixed['data']['path'], image)
            except OSError:
                shutil.copyfile(mixed['data']['path'], image)
            manifest = {'mixed_mode': mixed}
            extra = {'audio_track_transport': 'not attached: QEMU ide-cd takes the data track only; '
                                              'CD-DA transport is directive f5-12B2'}
        return {'path': str(image), 'master': manifest, **extra}
    finally:
        if staging.exists():
            for p in [staging, *staging.rglob('*')]:
                if p.is_dir():
                    p.chmod(0o700)
            shutil.rmtree(staging)


def generate(fixture, directory, index=0, root=ROOT):
    row = validate(fixture, root)
    media = fixture['media']
    source_kind = fixture.get('source', 'extracted')
    source = source_directory(row, fixture, root)
    scanned = scan(source)
    if not source.resolve().is_relative_to(Path(root).resolve()):
        raise ValueError('workload source escapes the worktree')
    provenance = {'status': 'owner-local'}
    if row['licence']['class'] != 'owner-local':
        from scripts.win32 import workloads
        if row['source']['sha256'] is None:
            raise WorkloadUnqualified('workload SHA-256 pin pending lead review: ' + row['id'])
        archive = Path(root) / DOWNLOADS / row['id'] / row['source']['file']
        if not archive.is_file():
            raise WorkloadAbsent('verified workload archive absent: ' + row['id'])
        provenance = {'status': 'pinned', **workloads.verify(archive, row['source'])}
    directory = dos_fixtures.check_output(directory, root)
    directory.mkdir(parents=True, exist_ok=True)
    identity = payload_identity(scanned['files'])
    text = identity_text(row['id'], media, identity)
    manifest = {'generator': GENERATOR, 'workload': row['id'], 'title': row['title'], 'media': media,
                'source': source_kind, 'source_directory': str(source), 'redistribution': REDISTRIBUTION,
                'licence_class': row['licence']['class'], 'disc_requirement': row['disc']['requirement'],
                'payload_sha256': identity, 'identity_file': IDENTITY_FILE,
                'archive_verification': provenance,
                'file_count': len(scanned['files']), 'total_bytes': scanned['total_bytes'],
                'files': scanned['files'], 'directories': scanned['directories']}
    if media in DISK_MEDIA:
        image = directory / f'fixture-{index}.img'
        if image.exists():
            raise ValueError('Win32 fixture already exists: ' + image.name)
        manifest.update(fat_image(scanned, source, image, media, text))
        manifest['path'] = str(image)
    else:
        if (directory / f'fixture-{index}.iso').exists():
            raise ValueError('Win32 fixture already exists: fixture-%d.iso' % index)
        manifest.update(iso_image(scanned, source, directory, index, media, text, fixture.get('audio_seconds', 4)))
    manifest['size'] = Path(manifest['path']).stat().st_size
    manifest['sha256'] = sha(manifest['path'])
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('workload')
    parser.add_argument('directory', type=Path)
    parser.add_argument('--media', choices=MEDIA, default='fat32')
    parser.add_argument('--source', choices=SOURCES, default='extracted')
    parser.add_argument('--index', type=int, default=0)
    args = parser.parse_args()
    fixture = {'generator': GENERATOR, 'workload': args.workload, 'media': args.media,
               'source': args.source, 'redistribution': REDISTRIBUTION}
    try:
        manifest = generate(fixture, args.directory, args.index)
    except WorkloadAbsent as error:
        print(json.dumps({'status': 'not_run', 'reason': 'fixtures_absent', 'detail': str(error)}))
        return 0
    except WorkloadUnqualified as error:
        print(json.dumps({'status': 'not_run', 'reason': 'workload_unqualified', 'detail': str(error)}))
        return 0
    print(json.dumps({k: v for k, v in manifest.items() if k not in ('checkers', 'files')}, indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
