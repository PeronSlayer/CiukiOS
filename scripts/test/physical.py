"""Bounded import of operator-confirmed F0 serial/screen transcriptions.

This imports evidence only. It does not mount or write a disk, normalize records,
claim UART qualification, or substitute historical firmware replay for T4.
"""
import hashlib
import json
import re
from pathlib import Path
from evidence import Parser, EvidenceError, EvidenceNotRun
from loader_model import selector as base_selector, PROBES, F0_PROBES, F1_PROBES, F2_PROBES

LIMIT=128*1024
FIELDS=('model','unit_identity','bios_version','cpuid','installed_ram','pci_ids',
        'target_disk_identity','write_sha256','readback_sha256','capture_settings')


def cfg_selector(request, source='cfg'):
    """BOOT.CFG extension; the fw_cfg-only suffixes remain in the old parser."""
    if not isinstance(request, str) or not request.isascii() or len(request) > 64:
        raise ValueError('selector must be at most 64 ASCII bytes')
    match = re.fullmatch(r'(f[012]|all):sweep run=([0-9a-fA-F]{8})(?: step=([0-9]{1,2})(?: state=([0-9a-fA-F]{8}))?)?', request)
    if match:
        if source != 'cfg': raise ValueError('sweep requires BOOT.CFG')
        step = int(match[3] or 0); state = int(match[4] or '0', 16)
        if step > 63 or state >= 1 << 19: raise ValueError('invalid sweep cursor')
        return {'phase': 3 if match[1] == 'all' else int(match[1][1]),
                'probe': 'sweep', 'run': match[2], 'step': step, 'state': state,
                'platform': None, 'safe': False}
    return base_selector(request, source)


def wire_record(raw):
    """Validate the original controller envelope before routing; never rewrite it."""
    line = raw.removesuffix(b'\n').removesuffix(b'\r')
    if b'CIUKI_TEST' not in line: return None
    if not line.startswith(b'CIUKI_TEST ') or len(line) > 240:
        raise EvidenceError('malformed prefix or record exceeds 240 bytes')
    try: tokens = line.decode('ascii').split(' ')[1:]
    except UnicodeDecodeError as e: raise EvidenceError('non-ASCII marker') from e
    record = {}
    for token in tokens:
        if not re.fullmatch(r'[a-z][a-z0-9_]*=[A-Za-z0-9_:.,/+\-]+', token):
            raise EvidenceError('invalid key=value grammar')
        key, value = token.split('=', 1)
        if key in record: raise EvidenceError('duplicate key: ' + key)
        record[key] = value
    if not {'v', 'run', 'seq', 'probe', 'event'} <= record.keys():
        raise EvidenceError('missing envelope field')
    if record['v'] != '1' or not re.fullmatch('[0-9a-fA-F]{8}', record['run']) or not re.fullmatch('[0-9]{6}', record['seq']):
        raise EvidenceError('invalid envelope identity')
    if record['probe'] not in (*PROBES, 'sweep'): raise EvidenceError('unknown probe')
    if record['probe'] == 'sweep' and record['event'] not in ('BEGIN', 'SWEEP', 'SWEEP_END'):
        raise EvidenceError('invalid sweep controller scope')
    if record['event'] == 'SWEEP':
        if record.get('result') not in ('pass', 'fail', 'not_run') or not re.fullmatch('[0-9]{1,2}', record.get('step', '')):
            raise EvidenceError('invalid sweep completion')
    elif record['event'] == 'SWEEP_END':
        if record['probe'] != 'sweep' or any(not re.fullmatch('[0-9]{1,2}', record.get(k, '')) for k in ('passed', 'failed', 'not_run')):
            raise EvidenceError('invalid sweep summary')
    elif record['event'] not in ('BEGIN', 'READY', 'DATA', 'ARM', 'END', 'PANIC', 'ERROR', 'NOT_RUN'):
        raise EvidenceError('unknown controller event')
    return record


def split_boots(raw):
    """T23's real loader banner is L:CPU; UART noise may precede that banner.

    Keep bytes/CRLF/sequence numbers intact. SELECT_READY is a fallback for
    historical captures without CPU diagnostics, never a second split point.
    """
    if len(raw) > 4 * 1024**2: raise EvidenceError('sweep capture exceeds 4 MiB limit')
    pattern = rb'(?m)^[^\n]*?L:CPU signature='
    starts = [m.start() for m in re.finditer(pattern, raw)]
    if not starts: starts = [m.start() for m in re.finditer(rb'(?m)^SELECT_READY\r?$', raw)]
    if not starts: starts = [0]
    if starts[0] and b'CIUKI_TEST' in raw[:starts[0]]:
        raise EvidenceError('controller evidence before first loader banner')
    return [raw[a:b] for a, b in zip(starts, [*starts[1:], len(raw)])]


def import_sweep(capture, canonical_hash, cases, physical_cases=()):
    """Import all boots once, then apply each case's unchanged predicates."""
    # The runner parser also verifies bounded supervisor console framing.
    from run import Parser as ControllerParser, failed_prerequisite, operator_confirmation_case
    capture = Path(capture); meta = capture / 'acquisition.json'; log = capture / 'f0.log'
    if meta.stat().st_size > LIMIT or log.stat().st_size > 4 * 1024**2:
        raise EvidenceError('physical acquisition exceeds bounded import limit')
    metadata = json.loads(meta.read_text())
    missing = [k for k in FIELDS if k not in metadata]
    if missing: raise EvidenceError('missing physical identity fields: ' + ','.join(missing))
    if metadata.get('operator_confirmed') is not True:
        raise EvidenceError('physical model/unit identity needs operator confirmation')
    if metadata['write_sha256'] != canonical_hash or metadata['readback_sha256'] != canonical_hash:
        raise EvidenceError('physical write/readback does not match canonical image')
    if metadata.get('build_id', 'unknown') == 'unknown': raise EvidenceError('embedded build identity missing')
    if metadata.get('disk_log', 'unavailable') != 'unavailable' and not metadata.get('storage_qualified', False):
        raise EvidenceError('unqualified storage log')
    sources = metadata.get('selectors', [metadata['selector']])
    selections = [cfg_selector(s, metadata.get('selector_source', 'cfg')) for s in sources]
    allowed = {s['run'] for s in selections}
    sweep = any(s['probe'] == 'sweep' for s in selections)
    approved = {}
    for selection in selections:
        names = PROBES if selection['phase'] == 3 else (F0_PROBES, F1_PROBES, F2_PROBES)[selection['phase']]
        if selection['probe'] not in ('all', 'core', 'sweep'): names = (selection['probe'],)
        approved.setdefault(selection['run'], set()).update((*names, 'sweep'))
    groups = {}; summaries = []; boots = split_boots(log.read_bytes())
    for number, boot in enumerate(boots):
        seq = 0; run = None; parsers = {}; panic = False
        build = re.search(rb'Ciuki VMM F0 build ([A-Za-z0-9_.+-]+) - CiukiOS', boot)
        records = []
        fat_read_not_run = False
        for line in boot.splitlines(keepends=True):
            if panic and line.strip(): raise EvidenceError('output after terminal panic')
            r = wire_record(line)
            if r is None: continue
            if r['run'] not in allowed or run is not None and r['run'] != run:
                raise EvidenceError('unapproved run identity in boot')
            if r['probe'] not in approved[r['run']]: raise EvidenceError('probe outside approved selector')
            run = r['run']
            if int(r['seq']) <= seq: raise EvidenceError('sequence must increase within each boot')
            seq = int(r['seq']); records.append(r)
            if r['probe'] == 'sweep' and r['event'] == 'BEGIN': continue
            if r['event'] in ('SWEEP', 'SWEEP_END'):
                if r['event'] == 'SWEEP_END': summaries.append(r)
                continue
            p = parsers.setdefault(r['probe'], ControllerParser(run, r['probe']))
            if r['probe'] == 'fat-read' and r['event'] == 'ERROR' and r.get('status') == 'not_run' and r.get('reason') == 'fixtures_absent':
                if fat_read_not_run or not p.started or p.terminal:
                    raise EvidenceError('duplicate or late fat-read fixtures_absent outcome')
                fat_read_not_run = True
                p.records.append(r)
                p.not_run_reason = 'fixtures_absent'
                continue
            if r['probe'] == 'fat-read' and r['event'] == 'END' and r.get('status') == 'NOT_RUN' and fat_read_not_run:
                if p.terminal or getattr(p, 'not_run_reason', None) != 'fixtures_absent':
                    raise EvidenceError('invalid fat-read fixtures_absent terminal')
                p.records.append(r); p.terminal = r; p.outcome = 'not_run'
                continue
            p.feed(line); panic = r['event'] == 'PANIC'
        if not records: continue # failed menu attempts in the real T23 capture
        if sweep and b'L:SELECT_SOURCE=cfg' not in boot.splitlines():
            raise EvidenceError('sweep boot lacks BOOT.CFG provenance')
        if build is not None:
            if build[1].decode() != metadata['build_id']: raise EvidenceError('physical boot build mismatch')
        elif not any(r.get('build_id') == metadata['build_id'] for r in records):
            raise EvidenceError('physical boot does not identify verified build')
        for probe, parser in parsers.items():
            groups.setdefault(probe, []).append((number, parser))
    hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in (meta, log)}
    results = []; blocked = None
    for case in cases:
        probe = case['probe']; expected = case['expected']
        # Physical target declarations replace only their corresponding case.
        target = next((c for c in physical_cases if c['probe'] == probe and c['target'] in metadata['model'].lower()), None)
        if target: expected = target['expected']
        result = {'schema_version': 1, 'case': case.get('id', probe), 'probe': probe,
                  'profile': 'physical', 'image_sha256': canonical_hash, 'physical': metadata,
                  'artifact_hashes': hashes, 'expected': expected, 'observed': [], 'boots': [],
                  'sweep_complete': not sweep or bool(summaries), 'sweep_summary': summaries,
                  'outcome': 'not_run', 'reason': 'probe absent from capture',
                  'operator_confirmation': False}
        observations = groups.get(probe, [])
        if observations:
            result['observed'] = [r for _, p in observations for r in p.records]
            result['boots'] = [n + 1 for n, _ in observations]
            declared = case.get('boots') or [{}]
            if len(observations) != len(declared):
                result['reason'] = 'capture boot count differs from case declaration'
            else:
                result['outcome'] = 'pass'; result['reason'] = 'original per-boot predicates passed'
                for (number, parser), part in zip(observations, declared):
                    try:
                        if parser.outcome == 'not_run':
                            raise EvidenceNotRun(getattr(parser, 'not_run_reason', 'probe marked NOT_RUN'))
                        parser.check(part.get('expected', expected))
                        if parser.terminal and parser.terminal['event'] == 'PANIC':
                            observation = metadata.get('panic_observations', {}).get(str(number + 1), metadata)
                            if observation.get('external_halt_seconds', 0) < 5 or observation.get('resumed', True):
                                raise EvidenceNotRun('physical panic lacks five-second halt observation')
                    except EvidenceNotRun as e:
                        result['outcome'] = 'not_run'; result['reason'] = str(e); break
                    except EvidenceError as e:
                        result['outcome'] = 'fail'; result['reason'] = str(e); break
                if result['outcome'] == 'pass' and any(case.get(k) for k in ('checks', 'digests', 'crash_sequence', 'desktop_screen')):
                    result['outcome'] = 'not_run'; result['reason'] = 'independent disk/screen observation required'
        if case.get('operator_confirmation') or operator_confirmation_case(case):
            confirmed = metadata.get('case_confirmations', {}).get(result['case']) is True
            result['operator_confirmation'] = confirmed
            if not confirmed:
                result['outcome'] = 'not_run'; result['reason'] = 'case requires operator confirmation'
        if blocked:
            result['outcome'] = 'not_run'; result['reason'] = 'prerequisite failed: ' + blocked
        elif failed_prerequisite(case, result) and not (probe == 'fat-read' and result['reason'] == 'fixtures_absent'):
            blocked = result['case']
        results.append(result)
    return results


def import_evidence(capture,canonical_hash,expected):
    import json
    capture=Path(capture)
    meta=capture/'acquisition.json';log=capture/'f0.log'
    if meta.stat().st_size>LIMIT or log.stat().st_size>LIMIT:
        raise EvidenceError('physical acquisition exceeds 128 KiB allowlist limit')
    metadata=json.loads(meta.read_text())
    if metadata.get('operator_confirmed') is not True:
        raise EvidenceError('physical model/unit identity needs operator confirmation')
    selection=cfg_selector(metadata['selector'],metadata.get('selector_source','menu'))
    if selection['probe'] in ('all','sweep'):raise EvidenceError('use multi-boot import for all/sweep')
    missing=[key for key in FIELDS if key not in metadata]
    if missing:raise EvidenceError('missing physical identity fields: '+','.join(missing))
    if metadata['write_sha256']!=canonical_hash or metadata['readback_sha256']!=canonical_hash:
        raise EvidenceError('physical write/readback does not match canonical image')
    if metadata.get('build_id','unknown')=='unknown':raise EvidenceError('embedded build identity missing')
    if metadata.get('disk_log','unavailable')!='unavailable' and not metadata.get('storage_qualified',False):
        raise EvidenceError('unqualified storage log')
    parser=Parser(selection['run'],selection['probe'])
    raw=log.read_bytes()
    for line in raw.splitlines(keepends=True):parser.feed(line)
    parser.check(expected)
    build_records=[r for r in parser.records if r.get('build_id')==metadata['build_id']]
    if not build_records:raise EvidenceError('physical records do not identify the verified build')
    if expected['terminal']=='PANIC' and (metadata.get('external_halt_seconds',0)<5 or metadata.get('resumed',True)):
        raise EvidenceError('physical panic lacks five-second halt observation')
    return {'schema_version':1,'run_id':selection['run'],'probe':selection['probe'],
            'suite':metadata.get('suite','unknown'),'utc_start':metadata.get('utc_start','unknown'),
            'utc_end':metadata.get('utc_end','unknown'),'image_size':metadata.get('image_size','unknown'),
            'build_manifest_hash':metadata.get('build_manifest_hash','unknown'),
            'build_git_revision':metadata.get('build_git_revision','unknown'),'build_dirty':metadata.get('build_dirty','unknown'),
            'runner_revision':metadata.get('runner_revision','unknown'),
            'timeout':{'occurred':False,'external_seconds':metadata.get('external_seconds','unknown')},
            'cleanup':{'kind':'read-only evidence import','children':0},
            'outcome':'pass','reason':'operator-confirmed capture satisfies declared predicates',
            'selector':metadata['selector'],'image_sha256':canonical_hash,'physical':metadata,
            'expected':expected,'observed':parser.records,'disk_log':metadata.get('disk_log','unavailable'),
            'artifact_hashes':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (meta,log)}}
