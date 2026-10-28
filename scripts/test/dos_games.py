#!/usr/bin/env python3
"""fx-games20: pinned DOS acceptance media, temporary Games links and lead tools.

No download or pin update is implicit. No game bytes enter canonical builds.
See docs/validation/fx-games20.md for sources and the qualification boundary.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import urllib.request

import dos_titles
import dos_fixtures
from evidence import EvidenceNotRun, EvidenceError

ROOT = Path(__file__).resolve().parents[2]
CATALOG = 'tests/dos-titles/games20.json'
PINS = 'config/dos-titles.json'
CACHE = 'build/downloads/dos-titles'
MAX_BYTES = 96 * 1024**2
MAX_FILES = 2048
TITLE_KEYS = {'id', 'name', 'target', 'executable', 'genre', 'coverage_targets', 'audio', 'enter', 'play', 'quit', 'lan', 'calibration', 'installed', 'terms_reviewed', 'music'}


def require(ok, message):
    if not ok:
        raise ValueError('DOS games: ' + message)


def load(root=ROOT):
    root = Path(root)
    data = json.loads((root / CATALOG).read_text())
    pins = json.loads((root / PINS).read_text())
    require(isinstance(data, dict) and isinstance(pins, dict) and set(data) == set(pins) == {'schema_version', 'titles'}, 'manifest fields')
    require(type(data['schema_version']) is int and data['schema_version'] == pins['schema_version'] == 1, 'schema version')
    require(isinstance(data['titles'], list) and isinstance(pins['titles'], list), 'title lists')
    require(all(isinstance(t, dict) and set(t) == TITLE_KEYS for t in data['titles']), 'title fields')
    require(len(data['titles']) == len(pins['titles']) == 20, 'exactly twenty titles')
    ids = [t['id'] for t in data['titles']]
    require(len(set(ids)) == 20 and set(ids) == {p['id'] for p in pins['titles']}, 'title/pin census')
    require(len({p['archive'] for p in pins['titles']}) == 20, 'unique archive names')
    for title in data['titles']:
        require(isinstance(title['id'], str) and dos_titles.ID.fullmatch(title['id']), 'title ID')
        require(isinstance(title['name'], str) and 1 <= len(title['name']) <= 25 and title['name'].isascii(), 'DOS Box title fits 31 bytes')
        dos_titles.dos_path(title['target']); dos_titles.dos_path(title['executable'])
        require('/' not in title['target'] and '.' not in title['target'] and '/' not in title['executable'], 'direct executable')
        require(title['audio'] in ('digital', 'opl', 'both'), 'audible title required')
        require(title['music'] in ('digital', 'opl', 'general-midi'), 'music source')
        for phase in ('enter', 'play', 'quit'):
            validate_keys(title[phase])
        coverage = title['coverage_targets']
        require(isinstance(coverage, dict) and set(coverage) == {'video', 'extender', 'music', 'mouse', 'joystick'}, 'coverage fields')
        require(type(coverage['mouse']) is bool and type(coverage['joystick']) is bool, 'input coverage boolean')
        require(isinstance(coverage['video'], list) and coverage['video'] and set(coverage['video']) <= {'ega', 'mode13h', 'modex', 'vesa'}, 'video coverage')
        installed = title['installed']
        require(isinstance(installed, dict) and set(installed) == {'directory', 'files'}, 'installed fields')
        require(installed['directory'] == CACHE + '/' + title['id'] + '-installed', 'installed directory')
        validate_files(installed['files'])
        require(type(title['terms_reviewed']) is bool, 'terms review boolean')
        pin = next(p for p in pins['titles'] if p['id'] == title['id'])
        require(pin['licence']['class'] in ('bundled', 'test-media'), 'redistributable editions only')
        require(re.fullmatch(r'[a-zA-Z0-9_.-]{1,64}', pin['archive']) is not None, 'archive basename')
        require(pin['url'].startswith('https://') and pin['url'].endswith('/' + pin['archive']), 'pinned HTTPS download URL')
        if pin['status'] == 'lead-pins':
            require(pin['sha256'] == 'lead-pins' and pin['size'] is None and pin['licence']['class'] == 'test-media', 'explicit pending pin')
        else:
            require(pin['status'] == 'pinned' and isinstance(pin['sha256'], str) and dos_titles.SHA.fullmatch(pin['sha256']) and type(pin['size']) is int and 0 < pin['size'] <= MAX_BYTES, 'archive SHA-256 and size')
        # A provenance record is required for every new candidate, but existing
        # battery entries with inline licence member citations retain that form.
        source = pin['licence']['source']
        # Older battery rows cite the licence inside the archive. Where an
        # installed package provenance record exists, it remains authoritative
        # for the archive name and digest too (not just licence classification).
        if not source.startswith('third_party/'):
            installed_source = root / 'third_party/games' / (title['id'] + '-shareware') / 'UPSTREAM.json'
            if installed_source.is_file():
                upstream = json.loads(installed_source.read_text())
                require(upstream['archive'] == pin['archive'] and upstream['sha256'] == pin['sha256'] and upstream['size'] == pin['size'], 'installed package pin mismatch')
        if source.startswith('third_party/'):
            upstream = json.loads((root / source).read_text())
            require(upstream['archive'] == pin['archive'] and upstream['sha256'] == pin['sha256'] and upstream['size'] == pin['size'], 'upstream pin mismatch')
        calibration = title['calibration']
        if calibration is not None:
            validate_calibration(calibration)
        if title['lan'] is not None:
            lan = title['lan']
            require(set(lan) == {'host_args', 'join_args', 'host_input', 'join_input', 'host_executable', 'join_executable'}, 'LAN fields')
            for role in ('host', 'join'):
                executable = lan[role + '_executable']
                if executable is not None:
                    dos_titles.dos_path(executable)
                    require('/' not in executable and executable.endswith(('.EXE', '.COM', '.BAT')), 'LAN driver executable')
                args = lan[role + '_args']
                require(args is None or isinstance(args, str) and len(args) <= 70 and args.isascii() and all(ord(c) >= 32 for c in args) and not any(c in args for c in '\r\n"&|<>%'), 'bounded LAN arguments')
                if lan[role + '_input'] is not None:
                    validate_keys(lan[role + '_input'])
    require(sum(t['lan'] is not None for t in data['titles']) >= 6, 'six IPX titles')
    return data['titles'], pins


def validate_keys(keys):
    # Reuse the production acceptance qcode/chord grammar, not a second list.
    import acceptance
    require(isinstance(keys, list) and 1 <= len(keys) <= 64, '1..64 key chords')
    for key in keys:
        acceptance.chord(key)


def validate_files(files):
    require(isinstance(files, list) and len(files) <= MAX_FILES, 'installed file count')
    seen = set(); total = 0
    for row in files:
        require(isinstance(row, dict) and set(row) == {'path', 'size', 'sha256'}, 'installed file fields')
        dos_titles.dos_path(row['path'])
        require(row['path'] not in seen and isinstance(row['sha256'], str) and dos_titles.SHA.fullmatch(row['sha256']), 'unique installed file/hash')
        require(type(row['size']) is int and 0 <= row['size'] <= MAX_BYTES, 'installed file size')
        seen.add(row['path']); total += row['size']
    require(total <= MAX_BYTES, 'installed bytes budget')


def validate_calibration(c):
    require(isinstance(c, dict) and set(c) == {'image_sha256', 'profile', 'reviewer', 'single', 'host', 'join'}, 'calibration fields')
    require(isinstance(c['image_sha256'], str) and dos_titles.SHA.fullmatch(c['image_sha256']) and c['profile'] == 'qemu-t23' and isinstance(c['reviewer'], str) and c['reviewer'].strip(), 'calibration identity/reviewer')
    for role in ('single', 'host', 'join'):
        if c[role] is None:
            continue
        screen = c[role]
        require(set(screen) == {'region', 'sha256'}, 'gameplay region/hash')
        region = screen['region']
        require(isinstance(region, list) and len(region) == 4 and all(type(v) is int and 0 <= v <= 2048 for v in region) and min(region[2:]) > 0, 'nonempty gameplay region')
        require(isinstance(screen['sha256'], str) and dos_titles.SHA.fullmatch(screen['sha256']), 'gameplay hash')


def title_for(identity, root=ROOT):
    titles, pins = load(root)
    title = next((t for t in titles if t['id'] == identity), None)
    require(title is not None, 'unknown title ' + str(identity))
    return title, next(p for p in pins['titles'] if p['id'] == identity)


def payload(title, pin, root=ROOT):
    """Only the lead's measured installed tree; never infer installer contents."""
    root = Path(root)
    if pin['status'] != 'pinned':
        raise EvidenceNotRun('archive_pin_pending: ' + title['id'])
    source = root / CACHE / pin['archive']
    # The old vendored pins need not be copied to the new cache.
    if not source.exists() and pin.get('package'):
        source = root / 'third_party/games' / pin['package'] / pin['archive']
    if not source.is_file():
        raise EvidenceNotRun('archive_absent: ' + title['id'])
    require(not source.is_symlink() and source.stat().st_size == pin['size'] and dos_fixtures.sha(source) == pin['sha256'], 'archive pin mismatch')
    if not title['terms_reviewed']:
        raise EvidenceNotRun('package_terms_review_pending: ' + title['id'])
    if not title['installed']['files']:
        raise EvidenceNotRun('installed_tree_pending: ' + title['id'])
    directory = root / title['installed']['directory']
    require(not (root / CACHE).is_symlink() and (root / CACHE).resolve().is_relative_to(root.resolve()), 'cache escape')
    require(directory.resolve().is_relative_to((root / CACHE).resolve()) and not directory.is_symlink(), 'installed tree escape')
    result = {}
    for row in title['installed']['files']:
        path = directory / row['path']
        require(path.resolve().is_relative_to(directory.resolve()) and not path.is_symlink(), 'installed file escape')
        if not path.is_file():
            raise EvidenceNotRun('installed_file_absent: ' + row['path'])
        require(path.stat().st_size == row['size'] and dos_fixtures.sha(path) == row['sha256'], 'installed pin mismatch: ' + row['path'])
        result[row['path']] = path
    require(title['executable'] in result, 'installed executable absent')
    return result


def precheck(spec, root=ROOT, image_sha256=None):
    require(isinstance(spec, dict) and set(spec) == {'title', 'role'}, 'fixture title/role')
    require(spec['role'] in ('single', 'host', 'join'), 'fixture role')
    title, pin = title_for(spec['title'], root)
    files = payload(title, pin, root)
    c = title['calibration']
    if c is None or c[spec['role']] is None:
        raise EvidenceNotRun('gameplay_calibration_pending: ' + title['id'] + '/' + spec['role'])
    if image_sha256 is not None:
        require(c['image_sha256'] == image_sha256, 'gameplay calibration image mismatch')
    if spec['role'] != 'single':
        lan = title['lan']
        if lan is None or lan[spec['role'] + '_args'] is None or lan[spec['role'] + '_input'] is None or lan[spec['role'] + '_executable'] is None:
            raise EvidenceNotRun('ipx_host_join_calibration_pending: ' + title['id'])
        require(lan[spec['role'] + '_executable'] in files, 'installed LAN driver absent')
    return title, pin, files


def shortcut(title, role):
    # CiukiOS CIUKLNK3 (apps/desktop/shell_link.h), not MS-SHLLINK.
    # Each private overlay has one "Trial" entry, with a unique mnemonic.
    args = '"--name=' + title['name'] + '" ' + ('--network=ipx ' if role != 'single' else '')
    args += '"\\' + title['target'] + '\\G20RUN.BAT"'
    fields = ['Trial ' + title['name'], '/Programs/dosbox', args, '', '']
    parts = [f.encode('utf-8') for f in fields]
    return b'CIUKLNK3' + struct.pack('<7Ii', 1, 0, *(len(p) for p in parts), 0) + b''.join(parts)


def batch(title, role):
    args = '' if role == 'single' else title['lan'][role + '_args']
    executable = title['executable'] if role == 'single' else title['lan'][role + '_executable']
    return ('@ECHO OFF\r\nC:\r\nCD \\' + title['target'] + '\r\n' + executable + (' ' + args if args else '') + '\r\n').encode('ascii')


def partition(image):
    with Path(image).open('rb') as stream:
        mbr = stream.read(512)
    require(len(mbr) == 512 and mbr[510:] == b'\x55\xaa' and mbr[450] in (0x0b, 0x0c), 'canonical FAT32 partition')
    start, sectors = struct.unpack_from('<II', mbr, 454)
    require(start >= 1 and sectors > 0 and (start + sectors) * 512 <= Path(image).stat().st_size, 'partition bounds')
    return start * 512, sectors * 512


def nested_extent(volume):
    """Find the nested DOS base by following FAT entries, never fixed LBAs."""
    from win32_inventory import FatVolume
    with Path(volume).open('rb') as stream:
        fat = FatVolume(stream)
        first = 0
        for part in ('SYSTEM', 'DOS', 'UNIFIED.IMG'):
            rows = [e for e in fat.entries(first) if e[0].upper() == part]
            require(len(rows) == 1, 'nested DOS base path')
            name, attr, first, size, _ = rows[0]
            require(bool(attr & 16) == (part != 'UNIFIED.IMG'), 'nested path type')
        chain = fat.chain(first)
        require(chain == list(range(first, first + len(chain))) and size <= len(chain) * fat.cluster_bytes, 'nested DOS base must be contiguous')
        offset = fat.cluster_offset(first)
        boot = fat.read(offset, 512)
        require(boot[510:] == b'\x55\xaa' and boot[450] in (4, 6, 14), 'nested primary FAT16 partition')
        start, sectors = struct.unpack_from('<II', boot, 454)
        require(start and sectors and (start + sectors) * 512 <= size, 'nested partition bounds')
        return offset + start * 512, sectors * 512


def copy_files(volume, files, scratch, cancel_event=None):
    """mtools on an offline overlay slice. Verify every resulting byte."""
    dirs = set()
    for path in files:
        parent = str(Path(path).parent)
        while parent not in ('/', '.'):
            dirs.add(parent); parent = str(Path(parent).parent)
    for folder in sorted(dirs, key=lambda d: (d.count('/'), d)):
        # mdir distinguishes an existing directory from a failed mkdir.
        try:
            dos_fixtures.run(['mdir', '-i', volume, '::' + folder + '/'])
        except ValueError:
            dos_fixtures.run(['mmd', '-i', volume, '::' + folder])
    records = []
    for path, source in sorted(files.items()):
        if cancel_event is not None and cancel_event.is_set():
            raise EvidenceError('LAN peer failed during fixture staging')
        data = source if isinstance(source, bytes) else source.read_bytes()
        temporary = Path(scratch) / 'stage-file.bin'
        try:
            temporary.write_bytes(data)
            dos_fixtures.run(['mcopy', '-o', '-i', volume, temporary, '::' + path])
            require(dos_fixtures.run(['mtype', '-i', volume, '::' + path]).stdout == data, 'fixture readback: ' + path)
        finally:
            temporary.unlink(missing_ok=True)
        records.append({'path': path, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
    return records


def stage(host, spec, image, overlay, directory, root=ROOT, identity=None, cancel_event=None):
    """Lead runtime only: no whole-image copy, no rebuild, no canonical write."""
    title, pin, files = precheck(spec, root, dos_fixtures.sha(image))
    directory = dos_fixtures.check_output(directory, root)
    require(Path(overlay).resolve().is_relative_to(directory) and Path(overlay).suffix == '.qcow2', 'run overlay only')
    offset, size = partition(image)
    with host.export_writable(overlay, directory, offset, size) as volume:
        inner_offset, inner_size = nested_extent(volume)
    nested = {'/' + title['target'] + '/' + path: source for path, source in files.items()}
    require('G20RUN.BAT' not in files, 'reserved fixture batch name')
    nested['/' + title['target'] + '/G20RUN.BAT'] = batch(title, spec['role'])
    with host.export_writable(overlay, directory, offset + inner_offset, inner_size) as volume:
        records = copy_files(volume, nested, directory, cancel_event)
        dos_fixtures.run(['fsck.fat', '-n', volume])
    with host.export_writable(overlay, directory, offset, size) as volume:
        base_sha = dos_fixtures.stream_sha(['mtype', '-i', volume, '::/SYSTEM/DOS/UNIFIED.IMG'])
        outer = {'/SYSTEM/DOS/UNIFIED.SHA': (base_sha + '\n').encode(),
                 '/Windows/Start Menu/Programs/Games/Trial.lnk': shortcut(title, spec['role'])}
        if identity is not None:
            require(isinstance(identity, bytes) and len(identity) == 16 and any(identity), 'LAN host identity')
            outer['/System/NETID.BIN'] = identity
        records += copy_files(volume, outer, directory, cancel_event)
        dos_fixtures.run(['fsck.fat', '-n', volume])
    return {'title': title['id'], 'role': spec['role'], 'archive_sha256': pin['sha256'],
            'base_sha256': base_sha, 'redistribution': 'private-test-only', 'files': records}


def fetch(title, pin, root=ROOT, lead_pin=False):
    """Fetch explicitly; the normal path requires a pre-existing digest."""
    root = Path(root); cache = root / CACHE; cache.mkdir(parents=True, exist_ok=True)
    require(not cache.is_symlink() and cache.resolve().is_relative_to(root.resolve()), 'cache escape')
    if pin['status'] != 'pinned' and not lead_pin:
        raise EvidenceNotRun('lead-pins: use --lead-pin after checking the publisher URL')
    target = cache / pin['archive']; temp = cache / (pin['archive'] + '.download')
    require(not target.is_symlink() and not temp.exists(), 'unsafe/existing download temporary')
    try:
        with urllib.request.urlopen(pin['url'], timeout=30) as source, temp.open('xb') as out:
            total = 0
            while block := source.read(65536):
                total += len(block); require(total <= MAX_BYTES, 'download budget'); out.write(block)
        digest = dos_fixtures.sha(temp)
        if pin['status'] == 'pinned':
            require(total == pin['size'] and digest == pin['sha256'], 'download pin mismatch')
        else:
            require(lead_pin and total > 0, 'empty download')
            pin.update(size=total, sha256=digest, status='pinned')
            data = json.loads((root / PINS).read_text())
            data['titles'] = [pin if p['id'] == pin['id'] else p for p in data['titles']]
            (root / PINS).write_text(json.dumps(data, indent=2) + '\n')
            source = root / pin['licence']['source']
            upstream = json.loads(source.read_text()); upstream.update(size=total, sha256=digest, status='pinned')
            source.write_text(json.dumps(upstream, indent=2) + '\n')
        temp.replace(target)
    finally:
        temp.unlink(missing_ok=True)
    return {'id': title['id'], 'size': total, 'sha256': digest, 'path': str(target)}


def acceptance_case(title, role='single'):
    """One menu launch and natural return; no kill can substitute for quitting."""
    ident = title['id']
    base = {'id': 'acc-dos-' + ident + ('' if role == 'single' else '-' + role),
            'kind': 'acceptance', 'probe': 'acceptance', 'profile': 'qemu-t23', 'item': 3,
            'title': title['name'] + (' IPX ' + role if role != 'single' else ' full screen, sound and gameplay'),
            'timeout': 2400, 'expected': {'evidence_kind': 'acceptance'},
            'dos_game': {'title': ident, 'role': role}}
    if role != 'single':
        base['devices'] = {'network': 'i82559er'}
    fail = [r'^\[dosvm\] create refused ', r'^\[launch\] name=dosbox pid=\d+ error=',
            r'^\[dosvm\] fullscreen_(?:refused|transport_fault) vm=\d+ ',
            r'^\[dosvm\] lifecycle stage=stop vm=\d+ generation=\d+ reason=[234] ']
    base['forbid'] = fail
    steps = [
        {'template': 'boot', 'prefix': 'b1', 'check': 'boot', 'settle': 15},
        {'name': 'menu-refresh', 'check': 'menu', 'actions': [{'keys': ['ctrl+esc', 'p'], 'pause': 1}, {'sleep': 5}, {'keys': ['esc', 'esc']}]},
        {'name': 'launch', 'check': 'menu', 'actions': [{'keys': ['ctrl+esc', 'p', 'g', 'ret', 't', 'ret'], 'pause': 1}],
         'note': 'Start > Programs > Games > Trial ' + title['name'] + '; the only Trial entry in this private overlay'},
        {'name': 'vm', 'check': 'menu', 'timeout': 240,
         'wait': r'^\[launch-stage\] name=dosbox pid=(?P<game_pid>\d+) stage=vm-create window=' + re.escape(title['name']) + '$'},
        {'name': 'graphics', 'check': 'fullscreen', 'timeout': 900,
         'wait': r'^\[dosvm\] fullscreen_request vm=(?P<game_vm>\d+) generation=(?P<game_gen>\d+) mode=\d+$'},
        {'name': 'fullscreen', 'check': 'fullscreen', 'timeout': 120,
         'wait': r'^\[dosvm\] fullscreen=1 vm=<<game_vm>> generation=<<game_gen>> mode=\d+$'},
        {'name': 'enter-game', 'check': 'gameplay', 'settle': 5, 'actions': [{'keys': title['enter'], 'pause': 1}]},
    ]
    if title['audio'] in ('digital', 'both'):
        steps.append({'name': 'digital', 'check': 'sound', 'since': 'boot', 'timeout': 120,
                      'wait': r'^\[vm-audio\] vm=<<game_vm>> play_dma=[15] hz=[1-9]\d* units=[1-9]\d*$'})
    if title['audio'] in ('opl', 'both'):
        steps.append({'name': 'opl', 'check': 'sound', 'since': 'boot', 'timeout': 120,
                      'wait': r'^\[vm-audio\] vm=<<game_vm>> gen=<<game_gen>> opl_writes=[1-9]\d* key_writes=[1-9]\d*$'})
    if title['music'] == 'general-midi':
        steps.append({'name': 'midi', 'check': 'sound', 'since': 'boot', 'timeout': 120,
                      'wait': r'^\[vm-audio\] vm=<<game_vm>> gen=<<game_gen>> midi_bytes=[1-9]\d* note_ons=[1-9]\d*$'})
    if role != 'single':
        for event in ('tx', 'rx'):
            steps.append({'name': 'ipx-' + event, 'check': 'lan', 'since': 'boot', 'timeout': 180,
                          'wait': r'^\[dos-ipx\] vm=<<game_vm>> gen=<<game_gen>> event=' + event + r' count=[1-4] (?:bytes=\d+ )?src=[0-9a-f]{12} dst=[0-9a-f]{12}$'})
    steps += [
        {'name': 'gameplay', 'check': 'gameplay', 'settle': 10, 'screen': {'desktop': False},
         'note': 'precheck replaces this screen with the lead-reviewed gameplay region/hash for this image and role'},
        {'name': 'play', 'check': 'gameplay', 'actions': [{'keys': title['play'], 'pause': 1}, {'sleep': 30}]},
        {'name': 'quit', 'check': 'exit', 'actions': [{'keys': title['quit'], 'pause': 1}]},
        {'name': 'returned', 'check': 'exit', 'timeout': 120,
         'wait': r'^\[console\] dosbox: program returned vm=<<game_vm>> generation=<<game_gen>> early=0$'},
        {'name': 'cleanup', 'check': 'cleanup', 'timeout': 120,
         'wait': r'^\[dosvm\] lifecycle stage=cleanup vm=<<game_vm>> generation=<<game_gen>> result=0 data_loss=0 retained=0$'},
        {'name': 'desktop', 'check': 'cleanup', 'settle': 3, 'screen': {'geometry': [1024, 768], 'desktop': True}},
    ]
    if title['coverage_targets']['mouse']:
        step = next(s for s in steps if s.get('name') == 'play')
        step['actions'].insert(1, {'mouse': {'dx': 40, 'dy': -12, 'button': 'left'}, 'pause': 1})
        step['actions'].insert(2, {'mouse': {'dx': -40, 'dy': 12}, 'pause': 1})
    base['steps'] = steps
    return base


def acceptance_cases(titles):
    cases = [acceptance_case(t) for t in titles]
    for title in titles:
        if title['lan'] is not None:
            cases.append({'id': 'acc-ipx-' + title['id'], 'kind': 'acceptance', 'probe': 'acceptance',
                          'profile': 'qemu-t23', 'item': 3, 'timeout': 2400,
                          'title': title['name'] + ' two-guest IPX host/join',
                          'expected': {'evidence_kind': 'acceptance'},
                          'guests': [acceptance_case(title, role) for role in ('host', 'join')]})
    return cases


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('action', choices=('validate', 'fetch', 'pin-installed', 'write-cases'))
    ap.add_argument('--title'); ap.add_argument('--lead-pin', action='store_true')
    ap.add_argument('--require-pins', action='store_true')
    args = ap.parse_args(argv)
    titles, pins = load()
    if args.action == 'write-cases':
        path = ROOT / 'tests/suites/acc-t23.json'
        suite = json.loads(path.read_text())
        suite['cases'] = [c for c in suite['cases'] if 'dos_game' not in c and not c['id'].startswith('acc-ipx-')] + acceptance_cases(titles)
        path.write_text(json.dumps(suite, indent=1) + '\n')
        print('Wrote twenty single-title cases and their IPX pairs'); return 0
    if args.action != 'validate':
        require(args.title is not None, 'explicit --title required')
        title, pin = title_for(args.title)
        if args.action == 'fetch':
            print(json.dumps(fetch(title, pin, lead_pin=args.lead_pin), indent=2)); return 0
        directory = ROOT / title['installed']['directory']
        require(directory.is_dir() and not directory.is_symlink(), 'installed directory required')
        files = []
        for path in sorted(directory.rglob('*')):
            require(not path.is_symlink(), 'installed symlink')
            if path.is_file():
                files.append({'path': path.relative_to(directory).as_posix(), 'size': path.stat().st_size, 'sha256': dos_fixtures.sha(path)})
        validate_files(files)
        require(any(f['path'] == title['executable'] for f in files), 'executable missing')
        title['installed']['files'] = files
        data = {'schema_version': 1, 'titles': [title if t['id'] == title['id'] else t for t in titles]}
        (ROOT / CATALOG).write_text(json.dumps(data, indent=2) + '\n')
        print('Pinned installed tree:', title['id'], len(files)); return 0
    if args.require_pins:
        for title in titles:
            pin = next(p for p in pins['titles'] if p['id'] == title['id'])
            payload(title, pin)
    print('PASS: twenty title definitions; %d lead-pins, %d installed trees pending' %
          (sum(p['status'] == 'lead-pins' for p in pins['titles']), sum(not t['installed']['files'] for t in titles)))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, OSError, EvidenceNotRun) as error:
        print(str(error), file=sys.stderr); raise SystemExit(1)
