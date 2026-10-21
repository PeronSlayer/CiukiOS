#!/usr/bin/env python3
"""Validate original-workload metadata and import pinned local download fixtures.

No payload is bundled or executed. Unknown provenance stays unknown until the
lead reviews an import receipt and the original distribution terms.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
from urllib.parse import urlparse
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[2]
DOWNLOAD_DIRECTORY = 'build/downloads/win32'
APPROVED = frozenset(('diablo-shareware', 'starcraft-shareware', 'aoe-trial',
    'half-life-uplink', 'ut-demo-348', 'worms-armageddon-demo', 'heroes3-demo',
    'rct-demo', 'jazz2-shareware', 'ta-demo'))
RESERVES = frozenset(('quake3-demo', 'nfs2se-demo', 'gta2-freeware', 'tomb-raider4-demo'))


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(value, length):
    return isinstance(value, str) and re.fullmatch('[0-9a-f]{'+str(length)+'}', value) is not None


def https(value):
    if not isinstance(value, str):
        return False
    u = urlparse(value)
    return u.scheme == 'https' and bool(u.hostname) and not u.username and not u.password


def validate(manifest):
    """Nulls are permitted for discovery; absence of required fields is not."""
    try:
        require(manifest['schema_version'] == 1, 'unsupported workload schema')
        require(manifest['download_directory'] == DOWNLOAD_DIRECTORY, 'wrong download directory')
        require(set(manifest['corpora']) == {'self-authored', 'period-demo', 'owner-local-retail'}, 'corpus labels')
        rows = manifest['workloads']
        require(isinstance(rows, list), 'workloads must be a list')
        ids, probes = set(), set()
        for row in rows:
            ident = row['id']
            require(isinstance(ident, str) and re.fullmatch('[a-z0-9]+(?:-[a-z0-9]+)*', ident), 'invalid workload id')
            require(ident not in ids, 'duplicate workload id')
            ids.add(ident)
            probe = row['probe']
            require(isinstance(probe, str) and len(probe.encode('utf-8')) <= 23 and
                    re.fullmatch(r'win32-[a-z0-9]+(?:-[a-z0-9]+)*', probe), 'invalid workload probe alias')
            require(probe not in probes, 'duplicate workload probe alias')
            probes.add(probe)
            require(isinstance(row['title'], str) and bool(row['title']), 'missing title')
            require(row['corpus'] in manifest['corpora'], 'invalid corpus')
            require(row['gate'] in ('F5', 'F7', 'reserve', 'fixture', 'owner-local'), 'invalid gate')
            require((row['gate'] == 'F5') == (ident in APPROVED), 'approved F5 set changed')
            require(isinstance(row['unknowns'], list) and all(isinstance(v, str) for v in row['unknowns']), 'unknowns required')
            source, licence = row['source'], row['licence']
            for key in ('url', 'file', 'size', 'sha1', 'sha256', 'path_pattern'):
                value = source[key]
                if value is None and key not in ('url', 'path_pattern'):
                    require('source.'+key in row['unknowns'], 'unlabelled unknown source.'+key)
            require(source['url'] is None or https(source['url']), 'HTTPS download URL required')
            name = source['file']
            require(name is None or isinstance(name, str) and name not in ('', '.', '..') and not any(c in name for c in '/\\\0'), 'unsafe source filename')
            require(source['size'] is None or type(source['size']) is int and source['size'] > 0, 'invalid source size')
            for key, width in (('sha1', 40), ('sha256', 64)):
                require(source[key] is None or digest(source[key], width), 'invalid '+key)
            require(licence['class'] in ('free-demo', 'shareware', 'owner-local'), 'invalid licence class')
            require(type(licence['distribution_verified']) is bool, 'distribution status must be boolean')
            if licence['distribution_verified']:
                require(bool(licence['text_path']) and digest(licence['text_sha256'], 64), 'distribution needs terms evidence')
            local = licence['class'] == 'owner-local'
            require(local == (row['corpus'] == 'owner-local-retail'), 'local corpus/class mismatch')
            if local:
                require(source['url'] is None and isinstance(source['path_pattern'], str), 'owner-local never fetched')
                p = PurePosixPath(source['path_pattern'])
                require(not p.is_absolute() and '..' not in p.parts and p.parts[:3] == ('legacy','local','win32'), 'unsafe owner-local pattern')
            else:
                require(source['path_pattern'] is None, 'download row has local pattern')
                require(https(source['item_url']) and https(source['metadata_url']), 'provenance URLs required')
            installer = row['installer']
            require(installer['method'] == 'interactive-defaults', 'default installer choices required')
            require(installer['arguments'] is None or isinstance(installer['arguments'], list) and all(isinstance(v, str) for v in installer['arguments']), 'installer arguments')
            require(installer['arguments'] is None or https(installer['parameters_source']), 'switches require documentation')
            require(installer['operator_script'] == 'tests/win32/operators/'+ident+'.json', 'operator script path')
            validate_inventory(row['installed_files'])
            require(row['disc']['requirement'] in ('none', 'ISO', 'track-aware'), 'disc requirement')
            scene = row['scene']
            require(type(scene['play_seconds']) is int and scene['play_seconds'] >= 180, 'play must last at least 180 s')
            require(type(scene['case_timeout_seconds']) is int and scene['play_seconds'] < scene['case_timeout_seconds'] <= 600, 'case timeout')
            require(type(scene['start_deadline_seconds']) is int and 0 < scene['start_deadline_seconds'] <= 120, 'start deadline')
            require(scene['focus_round_trips'] in (0, 10), 'focus round trips')
            require(isinstance(scene['screen_checks'], list) and bool(scene['measure']), 'scene evidence')
        require(APPROVED | RESERVES <= ids, 'missing approved title or reserve')
    except (KeyError, TypeError, AttributeError) as error:
        raise ValueError('malformed workload manifest: '+str(error)) from error
    return manifest


INVENTORY_KEYS = {'status', 'entries', 'reason', 'directories', 'changed', 'removed', 'links', 'registry',
                  'calibration', 'uninstall'}


def validate_files(rows, label):
    require(isinstance(rows, list), label + ' list')
    names = set()
    for entry in rows:
        require(isinstance(entry, dict) and set(entry) <= {'name', 'size', 'sha256'}, label + ' entry fields')
        name = entry['name']
        require(isinstance(name, str) and name and name.casefold() not in names, 'duplicate/invalid installed name')
        require(type(entry['size']) is int and entry['size'] >= 0, 'installed size')
        require(entry.get('sha256') is None or digest(entry['sha256'], 64), 'installed sha256')
        names.add(name.casefold())


def validate_registry(delta):
    require(delta is None or isinstance(delta, dict) and set(delta) == {'added_keys', 'removed_keys', 'changed_values'},
            'registry delta shape')
    if delta:
        require(all(isinstance(v, list) for v in delta.values()), 'registry delta lists')


def validate_inventory(inventory):
    """unknown (nothing observed), calibrated (written by an f5-15C QEMU
    calibration run, awaiting the lead) or verified (accepted by the lead)."""
    require(isinstance(inventory, dict) and set(inventory) <= INVENTORY_KEYS, 'installed inventory fields')
    require(inventory['status'] in ('unknown', 'calibrated', 'verified'), 'installed inventory status')
    require(isinstance(inventory['entries'], list), 'installed entries')
    if inventory['status'] == 'unknown':
        require(not inventory['entries'] and bool(inventory['reason']), 'unknown inventory must explain absence')
        require(not set(inventory) - {'status', 'entries', 'reason'}, 'unknown inventory carries no calibration')
        return
    require(bool(inventory['entries']), 'empty verified inventory')
    validate_files(inventory['entries'], 'installed')
    validate_files(inventory.get('changed', []), 'changed')
    for key in ('directories', 'removed'):
        require(isinstance(inventory.get(key, []), list) and all(isinstance(p, str) and p for p in inventory.get(key, [])), key)
    links = inventory.get('links', [])
    require(isinstance(links, list) and all(isinstance(l, dict) and set(l) == {'path', 'target'} for l in links), 'links')
    validate_registry(inventory.get('registry'))
    uninstall = inventory.get('uninstall')
    if uninstall is not None:
        require(isinstance(uninstall, dict) and set(uninstall) == {'registered', 'residue'} and type(uninstall['registered']) is bool,
                'uninstall block')
        residue = uninstall['residue']
        if residue is not None:
            require(isinstance(residue, dict) and set(residue) == {'files', 'directories', 'changed', 'removed', 'registry'}, 'residue shape')
            validate_files(residue['files'], 'residue')
            validate_files(residue['changed'], 'residue changed')
            validate_registry(residue['registry'])
    if inventory['status'] == 'calibrated' or 'calibration' in inventory:
        calibration = inventory.get('calibration')
        require(isinstance(calibration, dict) and re.fullmatch('[0-9a-f]{8}', str(calibration.get('run_id'))) and
                digest(calibration.get('image_sha256'), 64) and digest(calibration.get('payload_sha256'), 64),
                'calibrated inventory names its run, image and payload')


def load(path=ROOT/'tests/win32/workloads.json'):
    return validate(json.loads(Path(path).read_text()))


def hash_file(path):
    hashes = {name: hashlib.new(name) for name in ('sha1', 'sha256')}
    count = 0
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024*1024), b''):
            count += len(chunk)
            for h in hashes.values():
                h.update(chunk)
    return {'size': count, **{name: h.hexdigest() for name, h in hashes.items()}}


def verify(path, source, import_unpinned=False):
    require(type(source['size']) is int and source['size'] > 0, 'unknown size')
    require(digest(source['sha256'], 64) or import_unpinned and digest(source['sha1'], 40), 'SHA-256 pin required (or explicit SHA-1 candidate import)')
    actual = hash_file(path)
    for field in ('size', 'sha1', 'sha256'):
        if source[field] is not None:
            require(source[field] == actual[field], 'payload '+field+' mismatch')
    return actual


def fetch(row, root=ROOT, import_unpinned=False, opener=urlopen):
    """Stream once under build/downloads/win32/<id>; reverify cached bytes.

    An unpinned receipt is a candidate, not a new trusted manifest digest.
    Owner-local refusal occurs before any I/O, including calls to opener.
    """
    require(row['licence']['class'] != 'owner-local', 'owner-local payloads cannot be fetched')
    source = row['source']
    require(https(source['url']), 'unknown download URL')
    require(digest(source['sha256'], 64) or import_unpinned and digest(source['sha1'], 40), 'missing digest pin')
    require(type(source['size']) is int and source['size'] > 0, 'missing size pin')
    require(re.fullmatch('[a-z0-9]+(?:-[a-z0-9]+)*', row['id']), 'unsafe id')
    name = source['file']
    require(isinstance(name, str) and name not in ('', '.', '..') and not any(c in name for c in '/\\\0'), 'unsafe filename')
    root = Path(root).resolve()
    base = root/DOWNLOAD_DIRECTORY
    destination = base/row['id']/name
    require(destination.resolve().is_relative_to(base) and base.resolve().is_relative_to(root), 'download path escapes tree')
    require(not destination.is_symlink(), 'symlink destination')
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name+'.part')
    if not destination.exists():
        owned = False
        try:
            with temporary.open('xb') as output:
                owned = True
                with opener(source['url'], timeout=30) as stream:
                    final_url = stream.geturl() if hasattr(stream, 'geturl') else source['url']
                    require(https(final_url), 'redirect must remain HTTPS')
                    count = 0
                    while chunk := stream.read(1024*1024):
                        count += len(chunk)
                        require(count <= source['size'], 'download exceeds pinned size')
                        output.write(chunk)
            verify(temporary, source, import_unpinned)
            temporary.replace(destination)
        finally:
            if owned:
                temporary.unlink(missing_ok=True)
    actual = verify(destination, source, import_unpinned)
    receipt = {'schema_version': 1, 'id': row['id'], 'url': source['url'], **actual,
               'status': 'pinned' if source['sha256'] else 'candidate-unreviewed',
               'distribution_verified': row['licence']['distribution_verified']}
    destination.with_name(destination.name+'.receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
    return receipt


def import_local(row, root=ROOT, import_unpinned=False):
    """f5-15B: receipt for a file the lead already downloaded, without any I/O
    beyond hashing. The file is accepted at `build/downloads/win32/<id>/<file>`
    or, as the lead stores it, flat at `build/downloads/win32/<file>`; the flat
    file is linked into the per-title directory (same bytes, no copy) so the
    census and fixture generators find it. Returns None when the file is
    absent. Owner-local rows and rows without a pinned size are refused."""
    require(row['licence']['class'] != 'owner-local', 'owner-local payloads are never imported here')
    source = row['source']
    name = source['file']
    if name is None:
        return None
    require(isinstance(name, str) and name not in ('', '.', '..') and not any(c in name for c in '/\\\0'), 'unsafe filename')
    require(re.fullmatch('[a-z0-9]+(?:-[a-z0-9]+)*', row['id']), 'unsafe id')
    root = Path(root).resolve()
    base = root/DOWNLOAD_DIRECTORY
    destination = base/row['id']/name
    flat = base/name
    require(not destination.is_symlink() and not flat.is_symlink(), 'symlink payload')
    if not destination.exists():
        if not flat.is_file():
            return None
        destination.parent.mkdir(parents=True, exist_ok=True)
        try:
            os.link(flat, destination)
        except OSError:
            shutil.copyfile(flat, destination)
    require(destination.resolve().is_relative_to(base.resolve()), 'import path escapes tree')
    actual = verify(destination, source, import_unpinned)
    receipt = {'schema_version': 1, 'id': row['id'], 'url': source['url'], **actual,
               'status': 'pinned' if source['sha256'] else 'candidate-unreviewed',
               'sha1_verified': source['sha1'] is not None,
               'distribution_verified': row['licence']['distribution_verified'],
               'imported_from': 'local file supplied by the lead'}
    destination.with_name(destination.name+'.receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
    return receipt


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, default=ROOT/'tests/win32/workloads.json')
    parser.add_argument('--fetch', metavar='ID')
    parser.add_argument('--import-unpinned', action='store_true',
                        help='with --fetch: accept a SHA-1-only pin; alone: write receipts for '
                             'every downloaded file already present under build/downloads/win32/')
    args = parser.parse_args()
    manifest = load(args.manifest)
    if args.fetch:
        matches = [w for w in manifest['workloads'] if w['id'] == args.fetch]
        if not matches:
            parser.error('unknown workload')
        print(json.dumps(fetch(matches[0], import_unpinned=args.import_unpinned), indent=2))
    elif args.import_unpinned:
        receipts = []
        for row in manifest['workloads']:
            if row['licence']['class'] == 'owner-local' or row['source']['file'] is None:
                continue
            if row['source']['size'] is None:
                receipts.append({'id': row['id'], 'file': row['source']['file'], 'absent': True,
                                 'reason': 'no size pin in the manifest; import refused'})
                continue
            receipt = import_local(row, import_unpinned=True)
            receipts.append({'id': row['id'], 'file': row['source']['file'],
                             **({'absent': True} if receipt is None else receipt)})
        print(json.dumps(receipts, indent=2))
    else:
        print('Validated', len(manifest['workloads']), 'workload rows; no payload fetched.')
