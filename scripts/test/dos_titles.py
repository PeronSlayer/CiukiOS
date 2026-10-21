#!/usr/bin/env python3
"""Private DOS title fixtures and strict acceptance; never downloads or rebuilds.

Only `run` invokes the canonical runner (lead use). validate/parse are host-only.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dos_fixtures
from evidence import Parser, EvidenceError, EvidenceNotRun

ROOT = Path(__file__).resolve().parents[2]
GENERATOR = 'dos-title'
ID = re.compile(r'[a-z][a-z0-9-]{0,22}')
SHA = re.compile(r'[0-9a-f]{64}')
DOS = re.compile(r'[A-Z0-9_-]{1,8}(?:\.[A-Z0-9_-]{1,3})?')
MAGIC = b'D53PLAN1'
MAX_INPUT = 128
PLAN_SIZE = 8 + 24 + 128 + 12 + 3 * 77 + MAX_INPUT * 12
PHASES = ('launch', 'play', 'exit')
CHECKS = ('boot', 'menu', 'gameplay')


def exact(value, keys, context):
    if not isinstance(value, dict) or set(value) != set(keys.split()):
        raise ValueError('invalid fields: ' + context)


def integer(value, low, high):
    if type(value) is not int or not low <= value <= high:
        raise ValueError('integer outside bounds')


def ascii_text(value, limit, empty=False):
    if not isinstance(value, str) or not (0 if empty else 1) <= len(value) <= limit or any(
            ord(c) < 32 or ord(c) > 126 for c in value):
        raise ValueError('expected bounded ASCII text')


def dos_path(path):
    if not isinstance(path, str) or len(path) > 100 or not all(DOS.fullmatch(p) for p in path.split('/')):
        raise ValueError('expected relative uppercase DOS 8.3 path')
    return path


def validate(data, tracked=False):
    exact(data, 'schema_version titles', 'manifest')
    if type(data['schema_version']) is not int or data['schema_version'] != 1 or not isinstance(data['titles'], list) or not 1 <= len(data['titles']) <= 64:
        raise ValueError('manifest v1 requires 1..64 titles')
    seen = set()
    for t in data['titles']:
        exact(t, 'id name year publisher licence install executable args checkpoints input audio lan memory', 'title')
        if not isinstance(t['id'], str) or not ID.fullmatch(t['id']) or t['id'] in seen:
            raise ValueError('invalid or duplicate title ID')
        seen.add(t['id'])
        ascii_text(t['name'], 96); ascii_text(t['publisher'], 96); integer(t['year'], 1980, 2100)
        exact(t['licence'], 'kind citation conditions verified', 'licence')
        lic = t['licence']
        if lic['kind'] not in ('redistributable', 'owner-copy') or tracked and lic['kind'] != 'redistributable':
            raise ValueError('tracked titles must be redistributable')
        ascii_text(lic['citation'], 300); ascii_text(lic['conditions'], 500)
        if type(lic['verified']) is not bool: raise ValueError('licence verification must be boolean')
        install = t['install']
        exact(install, 'layout pin source target files steps', 'install')
        if install['layout'] not in ('package', 'zip', 'directory'): raise ValueError('unsupported layout')
        if not isinstance(install['pin'], str) or not ID.fullmatch(install['pin']): raise ValueError('bad pin ID')
        if install['source'] is not None:
            ascii_text(install['source'], 1024)
        if tracked and install['source'] is not None: raise ValueError('tracked source paths come from pin file')
        if not isinstance(install['target'], str) or not DOS.fullmatch(install['target']) or '.' in install['target']: raise ValueError('bad install target')
        if not isinstance(install['steps'], list) or len(install['steps']) > 32: raise ValueError('bad install steps')
        for step in install['steps']: ascii_text(step, 300)
        if not isinstance(install['files'], list) or len(install['files']) > 2048: raise ValueError('bad file pins')
        paths = set()
        for f in install['files']:
            exact(f, 'path sha256', 'installed file')
            dos_path(f['path'])
            if f['path'] in paths or not isinstance(f['sha256'], str) or not SHA.fullmatch(f['sha256']):
                raise ValueError('bad or duplicate installed file pin')
            paths.add(f['path'])
        dos_path(t['executable'])
        if '/' in t['executable'] or not t['executable'].endswith(('.EXE', '.COM', '.BAT')):
            raise ValueError('executable must be directly in target directory')
        if not isinstance(t['args'], list): raise ValueError('arguments must be a list')
        for arg in t['args']:
            ascii_text(arg, 80)
            if any(c in arg for c in '&|<>%\"'): raise ValueError('shell metacharacters forbidden')
        command(t)
        cps = t['checkpoints']
        if not isinstance(cps, list) or len(cps) != 3: raise ValueError('need boot/menu/gameplay checkpoints')
        previous = -1
        for name, cp in zip(CHECKS, cps):
            exact(cp, 'name at_ms deadline_ms kind value', 'checkpoint')
            if cp['name'] != name or cp['kind'] not in ('sha256', 'text'): raise ValueError('bad checkpoint')
            integer(cp['at_ms'], 0, 59999); integer(cp['deadline_ms'], cp['at_ms'], 60000)
            if cp['at_ms'] < previous: raise ValueError('checkpoint times must be ordered')
            previous = cp['at_ms']
            if cp['value'] is not None:
                ascii_text(cp['value'], 64)
                if cp['kind'] == 'sha256' and not SHA.fullmatch(cp['value']): raise ValueError('bad frame hash')
        if not isinstance(t['input'], list) or not 1 <= len(t['input']) <= MAX_INPUT: raise ValueError('bad input count')
        previous = (-1, -1)
        held = set()
        for e in t['input']:
            exact(e, 'phase at_ms code down', 'input')
            if e['phase'] not in PHASES: raise ValueError('bad input phase')
            integer(e['at_ms'], 0, 60000 if e['phase'] == 'launch' else 30000)
            integer(e['code'], 1, 127)
            if type(e['down']) is not bool: raise ValueError('key edge must be boolean')
            order = (PHASES.index(e['phase']), e['at_ms'])
            if order < previous: raise ValueError('input must be ordered by phase and time')
            previous = order
            if (e['code'] in held) == e['down']: raise ValueError('unbalanced key edges')
            if e['down']: held.add(e['code'])
            else: held.remove(e['code'])
        if held or not any(e['phase'] == 'exit' for e in t['input']): raise ValueError('need released keys and exit script')
        exact(t['audio'], 'expectation base irq dma8 dma16', 'audio')
        a = t['audio']
        for key in ('base', 'irq', 'dma8', 'dma16'): integer(a[key], 0, 65535)
        if a['expectation'] not in ('digital', 'opl', 'both', 'none') or a['base'] != 544 or a['irq'] not in (5, 7) or a['dma8'] != 1 or a['dma16'] != 5:
            raise ValueError('unsupported SB16 wiring')
        if t['lan'] not in ('none', 'ipx'): raise ValueError('unsupported LAN mode')
        exact(t['memory'], 'conventional_kib ems_kib xms_kib dpmi', 'memory')
        for key in ('conventional_kib', 'ems_kib', 'xms_kib'): integer(t['memory'][key], 0, 65536)
        if t['memory']['conventional_kib'] > 640 or t['memory']['dpmi'] not in ('none', 'dos4gw', 'cwsdpmi', 'native'):
            raise ValueError('bad DOS memory needs')
    return data


def command(t):
    line = '\\RUNREC.COM \\' + t['install']['target'] + ' ' + t['executable']
    if t['args']: line += ' ' + ' '.join(t['args'])
    ascii_text(line, 126)
    return line


def load(path, tracked=False):
    def unique(pairs):
        obj = {}
        for k, v in pairs:
            if k in obj: raise ValueError('duplicate JSON key')
            obj[k] = v
        return obj
    path = Path(path)
    if path.stat().st_size > 2 * 1024**2: raise ValueError('manifest exceeds budget')
    return validate(json.loads(path.read_text(), object_pairs_hook=unique), tracked)


def manifests(private=None, include_private=False, root=ROOT):
    rows = load(Path(root) / 'tests/dos-titles/tracked.json', True)['titles']
    if private is not None or include_private:
        path = private or os.environ.get('CIUKI_DOS_TITLES') or Path(root) / 'legacy/local/piano/dos-titles.json'
        if private is not None or os.environ.get('CIUKI_DOS_TITLES') or Path(path).exists():
            rows += load(path)['titles']
    return validate({'schema_version': 1, 'titles': rows})['titles']


def ready(t, pins):
    if not t['licence']['verified']: raise EvidenceNotRun('licence_verification_pending')
    if any(cp['value'] is None for cp in t['checkpoints']): raise EvidenceNotRun('checkpoint_calibration_pending')
    if t['install']['layout'] == 'directory':
        if not t['install']['files'] or t['install']['source'] is None: raise EvidenceNotRun('installed_files_pending')
    else:
        pin = next((p for p in pins['titles'] if p['id'] == t['install']['pin']), None)
        if not pin or not isinstance(pin.get('sha256'), str) or not SHA.fullmatch(pin['sha256']) or type(pin.get('size')) is not int:
            raise EvidenceNotRun('archive_pin_pending')


def plan(t):
    """Fixed little-endian wire format, parsed by the production C model."""
    def fixed(s, size): return s.encode('ascii').ljust(size, b'\0')
    mask = {'none': 0, 'digital': 1, 'opl': 2, 'both': 3}[t['audio']['expectation']]
    blob = MAGIC + fixed(t['id'], 24) + fixed(command(t), 128)
    blob += struct.pack('<III', mask, t['audio']['irq'], len(t['input']))
    for cp in t['checkpoints']:
        if cp['value'] is None: raise EvidenceNotRun('checkpoint_calibration_pending')
        blob += struct.pack('<III', cp['at_ms'], cp['deadline_ms'], cp['kind'] == 'text') + fixed(cp['value'], 65)
    for e in t['input']:
        blob += struct.pack('<III', PHASES.index(e['phase']), e['at_ms'], e['code'] | (256 if e['down'] else 0))
    return blob.ljust(PLAN_SIZE, b'\0')


def files(t, pins, root=ROOT):
    """Read only explicitly selected, pinned material; do not fetch anything."""
    install = t['install']; target = '/' + install['target']
    if install['layout'] == 'package':
        pin = next(p for p in pins['titles'] if p['id'] == install['pin'])
        import importlib.util
        spec = importlib.util.spec_from_file_location('title_games_disk', Path(root) / 'scripts/build_games_disk.py')
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        upstream = module.upstream(pin['package'], root)
        if upstream['sha256'] != pin['sha256'] or upstream['size'] != pin['size']:
            raise ValueError('package and battery pins differ')
        record, payload, _ = module.package_files(pin['package'], root)
        if record['install']['directory'] != install['target']: raise ValueError('package target differs')
        return {p: data for p, data, _ in payload}
    result = {}
    if install['layout'] == 'directory':
        source = Path(install['source']).resolve(strict=True)
        if not source.is_dir() or not install['files']: raise ValueError('installed directory required')
        total = 0
        for row in install['files']:
            path = source / row['path']
            resolved = path.resolve(strict=True)
            if not resolved.is_relative_to(source) or path.is_symlink() or not resolved.is_file(): raise ValueError('unsafe installed file')
            total += resolved.stat().st_size
            if total > 96 * 1024**2: raise ValueError('title exceeds private disk budget')
            data = resolved.read_bytes()
            if hashlib.sha256(data).hexdigest() != row['sha256']: raise ValueError('installed file hash mismatch')
            result[target + '/' + row['path']] = data
    else:
        pin = next(p for p in pins['titles'] if p['id'] == install['pin'])
        source = Path(install['source']) if install['source'] else Path(root) / 'build/downloads/dos-titles' / pin['archive']
        if source.stat().st_size != pin['size'] or dos_fixtures.sha(source) != pin['sha256']: raise ValueError('archive pin mismatch')
        with zipfile.ZipFile(source) as archive:
            total = 0
            for info in archive.infolist():
                name = info.filename.upper().replace('\\', '/')
                if info.is_dir():
                    dos_path(name.rstrip('/')); continue
                dos_path(name)
                total += info.file_size
                path = target + '/' + name
                if total > 96 * 1024**2 or path in result or (info.external_attr >> 16) & 0o170000 == 0o120000:
                    raise ValueError('unsafe or excessive ZIP contents')
                result[path] = archive.read(info)
    if target + '/' + t['executable'] not in result: raise ValueError('executable absent from installed layout')
    return result


def generate(fixture, directory, index=0, root=ROOT, expected_dos_sha256=None, dos_image=None):
    exact(fixture, 'generator title', 'DOS title fixture')
    t = fixture['title']
    validate({'schema_version': 1, 'titles': [t]})
    pins = json.loads((Path(root) / 'config/dos-titles.json').read_text())
    ready(t, pins)
    payload = files(t, pins, root)
    directory = dos_fixtures.check_output(directory, root)
    directory.mkdir(parents=True, exist_ok=True)
    boot, loader, system, dos_sha = dos_fixtures.dos_system(dos_image or Path(root) / 'build/f0/dos/DOSBOOT.IMG', expected_dos_sha256)
    if '/RUNREC.COM' not in system: raise ValueError('canonical DOS system lacks RUNREC.COM')
    if set(system) & set(payload): raise ValueError('title collides with DOS system')
    combined = dict(system, **payload)
    dirs = sorted({'/' + '/'.join(p.strip('/').split('/')[:n]) for p in combined for n in range(1, len(p.strip('/').split('/')))})
    inner = directory / f'fixture-{index}.vm'; outer = directory / f'fixture-{index}.img'
    planfile = directory / 'DTITLE.PLN'
    if any(p.exists() for p in (inner, outer, planfile)): raise ValueError('title fixture already exists')
    try:
        dos_fixtures._build_dos().write_fat16(inner, boot, loader, combined, directories=dirs)
        dos_fixtures.run(['fsck.fat', '-n', inner])
        for path, data in combined.items():
            if dos_fixtures.run(['mcopy', '-n', '-i', inner, '::' + path, '-']).stdout != data:
                raise ValueError('private disk read-back mismatch')
        base_sha = dos_fixtures.sha(inner)
        with outer.open('xb') as stream: stream.truncate(dos_fixtures.OUTER_BYTES)
        dos_fixtures.run(['mkfs.fat', '--invariant', '-F', '16', '-S', '512', '-s', str(dos_fixtures.OUTER_SPC), '-R', '4', '-f', '2', '-r', '512', '-i', dos_fixtures.OUTER_ID, '-n', dos_fixtures.OUTER_LABEL, outer])
        dos_fixtures.embed(outer, inner)
        planfile.write_bytes(plan(t))
        dos_fixtures.run(['mcopy', '-n', '-i', outer, planfile, '::/DTITLE.PLN'])
        dos_fixtures.run(['fsck.fat', '-n', outer])
        if dos_fixtures.stream_sha(['mcopy', '-n', '-i', outer, '::/CIUKIGAM.IMG', '-']) != base_sha:
            raise ValueError('outer image read-back mismatch')
        if dos_fixtures.run(['mcopy', '-n', '-i', outer, '::/DTITLE.PLN', '-']).stdout != planfile.read_bytes():
            raise ValueError('plan read-back mismatch')
        return {'generator': GENERATOR, 'title_id': t['id'], 'path': str(outer), 'size': outer.stat().st_size,
                'sha256': dos_fixtures.sha(outer), 'base_sha256': base_sha, 'dos_image_sha256': dos_sha,
                'plan_sha256': hashlib.sha256(plan(t)).hexdigest(), 'redistribution': 'private-test-only',
                'payloads': [{'path': p, 'size': len(d), 'sha256': hashlib.sha256(d).hexdigest()} for p, d in sorted(payload.items())]}
    except BaseException:
        outer.unlink(missing_ok=True)
        raise
    finally:
        inner.unlink(missing_ok=True); planfile.unlink(missing_ok=True)


def case(t, profile='qemu-t23'):
    return {'id': t['id'], 'probe': 'title', 'profile': profile, 'timeout': 240,
            'selector': 'f3:title run={run_id} title=' + t['id'],
            'fixtures': [{'generator': GENERATOR, 'title': t}],
            'expected': {'terminal': 'END', 'status': 'PASS'}}


def assess(records, t):
    """Do not trust END PASS without complete, ordered measured subcases."""
    rows = [r for r in records if r.get('group') == 'dos_title']
    names = ['plan', 'start', 'boot', 'menu', 'gameplay', 'play', 'audio', 'pcm', 'cadence', 'input', 'exit', 'cleanup']
    if [r.get('case') for r in rows] != names or any(r.get('title') != t['id'] for r in rows):
        raise EvidenceError('missing, duplicate, reordered or wrong-title evidence')
    d = dict(zip(names, rows))
    if d['plan'].get('sha256') != hashlib.sha256(plan(t)).hexdigest(): raise EvidenceError('runtime plan differs from manifest')
    def n(row, key):
        value = row.get(key, '')
        if not isinstance(value, str) or not re.fullmatch('[0-9]{1,20}', value): raise EvidenceError('missing numeric evidence: ' + key)
        return int(value)
    if not n(d['start'], 'vm') or not n(d['start'], 'generation'): raise EvidenceError('missing VM identity')
    last = 0
    for name, cp in zip(CHECKS, t['checkpoints']):
        row = d[name]; elapsed = n(row, 'ms')
        if elapsed < last or elapsed > 60000 or n(row, 'matched') != 1 or not SHA.fullmatch(row.get('digest', '')):
            raise EvidenceError('checkpoint timing or match failed')
        after_launch = elapsed - n(d['start'], 'launch_ms')
        if not cp['at_ms'] <= after_launch <= cp['deadline_ms']: raise EvidenceError('checkpoint outside window')
        if cp['value'] is None or cp['kind'] == 'sha256' and row['digest'] != cp['value']:
            raise EvidenceError('uncalibrated or wrong screen digest')
        last = elapsed
    if n(d['play'], 'duration_ms') < 30000 or n(d['play'], 'faults') or n(d['play'], 'frame_changes') < 2:
        raise EvidenceError('gameplay did not run fault-free for 30 seconds')
    a = d['audio']; expect = t['audio']['expectation']
    c = d['cadence']; pcm = d['pcm']
    if n(a, 'underruns') or n(c, 'irq_latency_max_us') > 20000: raise EvidenceError('audio cadence failed')
    n(c, 'late_acks'); n(c, 'opl_writes')
    if expect != 'none' and (n(a, 'attached') != 1 or n(a, 'ready') != 1 or n(a, 'irq') != t['audio']['irq']):
        raise EvidenceError('audio backend or wiring unavailable')
    if expect in ('digital', 'both') and (not n(pcm, 'pcm_frames') or not n(pcm, 'blocks') or not n(pcm, 'irq_entries')):
        raise EvidenceError('no digital sound evidence')
    if expect in ('opl', 'both') and not n(c, 'opl_writes'): raise EvidenceError('no OPL music evidence')
    if n(d['input'], 'delivered') != len(t['input']) or n(d['input'], 'expected') != len(t['input']): raise EvidenceError('input incomplete')
    if n(d['exit'], 'clean') != 1 or n(d['exit'], 'rc') or n(d['exit'], 'faults') or n(d['cleanup'], 'restored') != 1:
        raise EvidenceError('exit/cleanup failed')
    return {'outcome': 'pass', 'title': t['id'], 'menu_ms': n(d['menu'], 'ms'),
            'gameplay_ms': n(d['play'], 'duration_ms'), 'audio': {**a, **pcm, **c},
            'input_delivered': n(d['input'], 'delivered'), 'exit_clean': True,
            'lan': 'pending f6-11' if t['lan'] == 'ipx' else 'none'}


def parse_serial(raw, run_id, t):
    if len(raw) > 4 * 1024**2 or raw and not raw.endswith(b'\n'): raise EvidenceError('partial or excessive serial log')
    parser = Parser(run_id, 'title')
    cadence = []
    for line in raw.splitlines():
        if b'panic' in line.lower() or b'double fault' in line.lower(): raise EvidenceError('kernel fault')
        # output.c assigns the same global sequence to disk-only diagnostics.
        # Gaps on UART are intentional; required title subcases are checked
        # independently and exactly once, with the normal monotonic parser.
        parser.feed(line)
        if b'[vm-audio]' in line: cadence.append(line.decode('ascii', errors='replace'))
    parser.check({'terminal': 'END', 'status': 'PASS'})
    result = assess(parser.records, t)
    result['cadence_diagnostics'] = cadence
    return result


def write_summary(path, results):
    path = Path(path)
    path.mkdir(parents=True, exist_ok=True)
    passed = sum(r['outcome'] == 'pass' for r in results)
    (path / 'summary.json').write_text(json.dumps({'schema_version': 1, 'target_title_count': 20,
        'observed_title_count': len(results), 'qualified_title_count': passed,
        'twenty_title_goal_met': len(results) >= 20 and passed == len(results), 'titles': results}, indent=2) + '\n')
    lines = ['| Title | Result | Menu ms | Gameplay ms | Late acks | Underruns | IRQ max us | OPL writes | Input | Clean exit | LAN |',
             '| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |']
    for r in results:
        a = r.get('audio', {})
        label = r['title']
        if r.get('name'): label += ': {} ({}, {})'.format(r['name'], r['year'], r['publisher'])
        cells = [label.replace('|', '\\|'), r['outcome'], r.get('menu_ms', ''), r.get('gameplay_ms', ''),
                 a.get('late_acks', ''), a.get('underruns', ''), a.get('irq_latency_max_us', ''), a.get('opl_writes', ''),
                 r.get('input_delivered', ''), r.get('exit_clean', ''), r.get('lan', 'pending f6-11')]
        lines.append('| ' + ' | '.join(map(str, cells)) + ' |')
    (path / 'summary.md').write_text('\n'.join(lines) + '\n')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('action', choices=('validate', 'run', 'parse'))
    ap.add_argument('--manifest', type=Path, help='explicit manifest, without automatic private input')
    ap.add_argument('--private', type=Path)
    ap.add_argument('--include-private', action='store_true')
    ap.add_argument('--title', action='append', default=[])
    ap.add_argument('--serial', type=Path); ap.add_argument('--run-id')
    ap.add_argument('--image', type=Path, default=ROOT / 'build/f0/ciukios.img')
    ap.add_argument('--profile', default='qemu-t23')
    args = ap.parse_args(argv)
    titles = load(args.manifest)['titles'] if args.manifest else manifests(args.private, args.include_private or args.action == 'run')
    if set(args.title) - {t['id'] for t in titles}: raise ValueError('unknown title ID')
    titles = [t for t in titles if not args.title or t['id'] in args.title]
    if args.action == 'validate':
        print('PASS: manifest schema, {} titles (runtime not verified)'.format(len(titles))); return 0
    if args.action == 'parse':
        if len(titles) != 1 or not args.serial or not args.run_id or not re.fullmatch('[0-9a-fA-F]{8}', args.run_id):
            raise ValueError('parse requires one title, --serial and --run-id')
        print(json.dumps(parse_serial(args.serial.read_bytes(), args.run_id, titles[0]), indent=2)); return 0
    import run as runner  # only explicit lead `run` reaches the process manager
    if not args.image.is_file(): raise ValueError('canonical image unavailable; no runtime evidence')
    results = []
    with runner.res.SlotLock(runner.res.common_lock(ROOT)):
        host = runner.Host(); host.preflight(ROOT / 'build/test-runs')
        executable = runner.shutil.which('qemu-system-i386')
        if not executable: raise ValueError('QEMU unavailable')
        firmware = runner.find_firmware(executable)
        profile = runner.load(ROOT / 'tests/profiles' / (args.profile + '.json'))
        for t in titles:
            r, directory = runner.run_case(ROOT, 'dos-titles', case(t, args.profile), profile, args.image.resolve(), executable, firmware)
            report = {'title': t['id'], 'outcome': r['outcome'], 'reason': r.get('reason'),
                      'image_sha256': r['image']['sha256'], 'run_id': r['run_id'],
                      'name': t['name'], 'year': t['year'], 'publisher': t['publisher']}
            if r['outcome'] == 'pass': report.update(assess(r['observed'], t))
            (directory / 'title-result.json').write_text(json.dumps(report, indent=2) + '\n')
            results.append(report)
            write_summary(ROOT / 'build/test-runs/dos-titles', results)
    return 0 if results and all(r['outcome'] == 'pass' for r in results) else 1


if __name__ == '__main__':
    try: sys.exit(main())
    except (ValueError, OSError) as error:
        print('FAIL: ' + str(error), file=sys.stderr); sys.exit(1)
