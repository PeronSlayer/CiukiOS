"""Bounded, pinned read-back references. These are never installed game trees."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import tempfile
import zipfile

import dos_titles
from evidence import EvidenceNotRun

MAX_BYTES = 96 * 1024**2
MAX_FILES = 2048
REFERENCE = 'build/dos-games-reference'


def require(ok, reason):
    if not ok:
        raise ValueError('DOS extraction: ' + reason)


def decoder():
    spec = importlib.util.spec_from_file_location('games_readback', Path(__file__).resolve().parents[1] / 'build_games_disk.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def checked_members(members):
    require(0 < len(members) <= MAX_FILES, 'member count')
    total = 0
    for name, data in members.items():
        dos_titles.dos_path(name)
        total += len(data)
        require(total <= MAX_BYTES, 'output budget')
    # A file cannot also be the ancestor of another file.
    require(all('/'.join(name.split('/')[:i]) not in members
                for name in members for i in range(1, len(name.split('/')))), 'file/directory collision')
    return members


def zip_members(data):
    """Validate the whole directory before allocating/decompressing members."""
    members, names, total = {}, set(), 0
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            infos = archive.infolist()
            require(0 < len(infos) <= MAX_FILES, 'ZIP member count')
            for info in infos:
                raw = info.orig_filename
                require('\0' not in raw and '\\' not in raw, 'unsafe ZIP path')
                name = raw.rstrip('/').upper() if info.is_dir() else raw.upper()
                dos_titles.dos_path(name)
                require(name not in names, 'duplicate ZIP path')
                names.add(name)
                mode = (info.external_attr >> 16) & 0o170000
                require(info.create_system != 3 or mode in (0, 0o040000 if info.is_dir() else 0o100000), 'ZIP link or special file')
                require(not info.flag_bits & 1, 'encrypted ZIP')
                total += info.file_size
                require(total <= MAX_BYTES, 'ZIP output budget')
            file_names = {i.filename.upper() for i in infos if not i.is_dir()}
            require(all('/'.join(name.split('/')[:i]) not in file_names
                        for name in names for i in range(1, len(name.split('/')))), 'file/directory collision')
            for info in infos:
                if not info.is_dir():
                    members[info.filename.upper()] = archive.read(info)
    except (zipfile.BadZipFile, NotImplementedError, RuntimeError) as error:
        raise ValueError('DOS extraction: invalid/unsupported ZIP: ' + str(error)) from error
    return checked_members(members)


def shr_members(data):
    """Apply aggregate bounds before each existing DCL decoder invocation."""
    module = decoder()
    at = module.SHR_HEADER
    require(len(data) >= at, 'short SHR header')
    members, total = {}, 0
    while at < len(data):
        require(at + module.SHR_ENTRY <= len(data), 'truncated SHR header')
        try:
            name = data[at:at + 16].split(b'\0', 1)[0].decode('ascii').upper()
        except UnicodeDecodeError as error:
            raise ValueError('DOS extraction: non-ASCII SHR name') from error
        dos_titles.dos_path(name)
        require('/' not in name and name not in members, 'duplicate/invalid SHR path')
        require(len(members) < MAX_FILES, 'SHR member count')
        size = struct.unpack_from('<I', data, at + module.SHR_SIZE)[0]
        start = at + module.SHR_ENTRY
        require(size > 0 and start + size <= len(data), 'truncated SHR member')
        output, used = module.dcl_explode(data[start:start + size], limit=min(16 * 1024**2, MAX_BYTES - total))
        require(used == size, 'trailing SHR data')
        members[name] = output
        total += len(output)
        at = start + size
    return checked_members(members)


def unpack(data, recipe):
    """Return original outer media and unmodified payload, never run an SFX."""
    require(len(data) <= MAX_BYTES, 'archive budget')
    outer = zip_members(data)
    kind = recipe['format']
    if kind == 'zip':
        return outer, outer
    try:
        if kind == 'zip-split':
            payload = zip_members(b''.join(outer[p] for p in recipe['parts']))
        elif kind == 'zip-sfx':
            payload = zip_members(outer[recipe['member']])
        elif kind == 'apogee-shr':
            payload = shr_members(outer[recipe['member']])
        elif kind == 'lha-sfx':
            payload = checked_members(decoder().lha_members(outer[recipe['member']][recipe['offset']:]))
        else:
            raise ValueError('DOS extraction: unsupported format ' + str(kind))
    except KeyError as error:
        raise ValueError('DOS extraction: missing container member ' + str(error)) from error
    return outer, payload


def archive_path(title, pin, root, archive_dir=None):
    if pin['status'] != 'pinned':
        raise EvidenceNotRun('archive_pin_pending: ' + title['id'])
    path = Path(archive_dir) / pin['archive'] if archive_dir is not None else Path(root) / 'build/downloads/dos-titles' / pin['archive']
    if archive_dir is None and not path.exists() and pin.get('package'):
        path = Path(root) / 'third_party/games' / pin['package'] / pin['archive']
    require(path.is_file() and not path.is_symlink(), 'pinned archive required')
    require(path.stat().st_size == pin['size'] and pin['size'] <= MAX_BYTES, 'archive size mismatch')
    data = path.read_bytes()
    require(len(data) == pin['size'] and hashlib.sha256(data).hexdigest() == pin['sha256'], 'archive pin mismatch')
    return data


def reference_payload(title, pin, plan, root, archive_dir=None):
    if plan['extract'] is None:
        raise EvidenceNotRun('segmented_sow_not_extractable: ' + title['id'])
    outer, payload = unpack(archive_path(title, pin, root, archive_dir), plan['extract'])
    require(title['executable'] in payload, 'game executable absent from reference')
    return outer, payload


def records(payload):
    return [{'path': name, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
            for name, data in sorted(payload.items())]


def extract(title, pin, plan, root, archive_dir=None):
    outer, payload = reference_payload(title, pin, plan, root, archive_dir)
    root = Path(root).resolve()
    parent = root / REFERENCE
    require(not parent.is_symlink() and parent.resolve().is_relative_to(root / 'build'), 'reference directory escape')
    parent.mkdir(parents=True, exist_ok=True)
    target = parent / title['id']
    require(not target.exists() and not target.is_symlink(), 'reference already exists; remove it explicitly before re-extracting')
    manifest = {'kind': 'extraction-reference-only', 'title': title['id'], 'archive': pin['archive'],
                'archive_sha256': pin['sha256'], 'recipe': plan['extract'],
                'files': records(payload), 'outer_media': records(outer)}
    with tempfile.TemporaryDirectory(prefix='extract-', dir=parent) as tmp:
        temp = Path(tmp) / title['id']
        (temp / 'payload').mkdir(parents=True)
        for name, data in payload.items():
            destination = temp / 'payload' / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
        (temp / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        temp.rename(target)
    return {'title': title['id'], 'kind': manifest['kind'], 'directory': str(target), 'files': len(payload)}


def scan_installed(title, root):
    root = Path(root).resolve()
    cache = root / 'build/downloads/dos-titles'
    directory = root / title['installed']['directory']
    require(cache.resolve().is_relative_to(root / 'build') and not cache.is_symlink(), 'cache escape')
    require(directory.is_dir() and not directory.is_symlink() and directory.resolve().is_relative_to(cache.resolve()), 'installed directory required/escape')
    files, total = [], 0
    for path in sorted(directory.rglob('*')):
        require(not path.is_symlink() and path.resolve().is_relative_to(directory.resolve()), 'installed symlink/escape')
        name = path.relative_to(directory).as_posix()
        dos_titles.dos_path(name)
        require(path.is_file() or path.is_dir(), 'installed special file')
        if path.is_file():
            total += path.stat().st_size
            require(total <= MAX_BYTES and len(files) < MAX_FILES, 'installed budget')
            files.append({'path': name, 'size': path.stat().st_size, 'sha256': dos_titles.dos_fixtures.sha(path)})
    require(any(row['path'] == title['executable'] for row in files), 'installed executable absent')
    return files


def compare_installed(title, pin, plan, root, archive_dir=None):
    outer, payload = reference_payload(title, pin, plan, root, archive_dir)
    actual = scan_installed(title, root)
    require(title['installed']['files'] and actual == title['installed']['files'], 'installed pins missing or changed; pin measured output first')
    ref = {row['path']: row for row in records(payload)}
    installed = {row['path']: row for row in actual}
    policy = plan['comparison']
    unchanged = sorted(n for n in ref.keys() & installed.keys() if ref[n] == installed[n])
    missing = sorted(ref.keys() - installed.keys())
    added = sorted(installed.keys() - ref.keys())
    changed = sorted(n for n in ref.keys() & installed.keys() if ref[n] != installed[n])
    reference_only = set(policy['reference_only'])
    installer_only = set(policy['installer_only'])
    config = set(policy['config'])
    # Exact names, never wildcard/extension exemptions. Config may be generated,
    # or change an original default, but may not disappear from the install.
    require(not config & (reference_only | installer_only) and not reference_only & installer_only, 'overlapping comparison exceptions')
    valid_policy = (reference_only <= ref.keys() - installed.keys() and
                    installer_only <= installed.keys() - ref.keys() and config <= installed.keys())
    errors = {'missing': sorted(set(missing) - reference_only),
              'unexpected': sorted(set(added) - config - installer_only),
              'changed': sorted(set(changed) - config)}
    return {'title': title['id'], 'archive_sha256': pin['sha256'],
            'status': 'PASS' if valid_policy and not any(errors.values()) else 'FAIL',
            'same': unchanged, 'reference_only': sorted(reference_only),
            'installer_only': sorted(installer_only), 'config': sorted(config),
            'outer_media': sorted(outer.keys() - payload.keys()), 'errors': errors,
            'policy_matches_tree': bool(valid_policy)}
