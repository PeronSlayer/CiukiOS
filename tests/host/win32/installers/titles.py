"""f5-15B: the original acceptance installers on the H1 Linux adapter.

One driver per title (`tests/win32/installers/<id>.json`): the lead's imported
download (or the host-extracted CD tree) is staged into a fresh fixture tree,
the entry executable runs on the confined i386 adapter through the H1 file,
registry, process and wait services, and the adapter records decide each
phase. The H1 adapter has no desktop service, USER message queue or thread
creation (W32_SERVICE, W32_MESSAGE, W32_THREAD are ENOSYS by design); a run
that reaches one of those boundaries after a clean load is "boundary": the
personality accepted the image and served every earlier request. The UI,
copy, registry, link, restart and uninstall phases of such a run stay
`not_run` here and belong to the QEMU `installers` tier (the lead). A loader
refusal, a crash or an unexpected ENOSYS before the boundary is a failure of
the installer-reachable surface and is reported with the exact record.

No payload is read from the repository: titles come from
`build/downloads/win32/<id>/` (scripts/win32/workloads.py --import-unpinned)
and CD trees from `build/win32-workloads/<id>/cd/` (host-extracted ISO, the
lead's directory); nothing is copied into tracked files or the image.
"""
# SPDX-License-Identifier: MIT
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import time

from scripts.win32 import workloads
from . import host_services

ROOT = Path(__file__).resolve().parents[4]
HERE = Path(__file__).resolve().parent
OUT = ROOT / 'build/host/win32/installers'
SPECS = ROOT / 'tests/win32/installers'
DOWNLOADS = ROOT / 'build/downloads/win32'
CD_TREES = ROOT / 'build/win32-workloads'
PHASES = ('extraction', 'ui', 'copy', 'registry', 'links', 'restart', 'uninstall')
ENGINES = ('wise', 'installshield', 'iexpress', 'winzip', 'vendor', 'cd')
# Kernel operations the H1 adapter answers with ENOSYS by construction
# (tests/host/win32/wine/upper_h1.c, h1_services.c): reaching one of them
# after a clean load is the adapter's boundary, not a personality failure.
H1_BOUNDARIES = {0x200: 'W32_SERVICE (desktop service connect)',
                 0x104: 'W32_MESSAGE (USER message queue)',
                 0x00a: 'W32_THREAD (thread creation)'}
RECORD = re.compile(r'^\[win32\] (\S+) (\S+) (\S+)$')


def spec_ids():
    return sorted(p.stem for p in SPECS.glob('*.json'))


def load_spec(ident):
    spec = json.loads((SPECS / (ident + '.json')).read_text())
    if spec['schema_version'] != 1 or spec['workload'] != ident:
        raise ValueError('malformed title spec: ' + ident)
    if spec['engine'] not in ENGINES:
        raise ValueError('unknown engine: ' + spec['engine'])
    entry = spec['entry']
    if entry['kind'] not in ('download', 'cd'):
        raise ValueError('entry kind')
    if not spec['variants']:
        raise ValueError('a title needs at least one variant')
    for variant in spec['variants']:
        if not re.fullmatch(r'[a-z0-9-]+', variant['name']):
            raise ValueError('variant name')
        if not isinstance(variant.get('arguments', ''), str):
            raise ValueError('variant arguments')
    if spec['expected']['status'] not in ('unknown', 'verified'):
        raise ValueError('expected status')
    row = next((w for w in workloads.load()['workloads'] if w['id'] == ident), None)
    if row is None:
        raise ValueError('title spec without a workload row: ' + ident)
    spec['_row'] = row
    return spec


def payload(spec):
    """The imported download (and, for CD installs, the extracted tree) or None."""
    row = spec['_row']
    if row['licence']['class'] == 'owner-local':
        return None  # never used on the adapter unless the manifest marks it distributable
    name = row['source']['file']
    download = DOWNLOADS / row['id'] / name if name else None
    if download is None or not download.is_file():
        return None
    if spec['entry']['kind'] == 'cd':
        tree = CD_TREES / row['id'] / spec['entry']['tree']
        if not tree.is_dir():
            return None
        return {'download': download, 'tree': tree}
    return {'download': download}


def receipt(spec):
    path = DOWNLOADS / spec['workload'] / (spec['_row']['source']['file'] + '.receipt.json')
    return json.loads(path.read_text()) if path.is_file() else None


def parse_output(text):
    """Facts from the adapter's output: loader refusal, crash report, stubs,
    diagnostics, the ENOSYS operations, USER gate numbers, exit code."""
    result = {'refuse': None, 'crash': None, 'terminate': None, 'entry_returned': None,
              'records': [], 'stubs': [], 'enosys': {}, 'gate': {}, 'stats': None}
    for line in text.splitlines():
        if line.startswith('REFUSE '):
            parts = line.split(' ')
            result['refuse'] = {'reason': parts[1], 'module': parts[2] if len(parts) > 2 else '-',
                                'symbol': parts[3] if len(parts) > 3 else '-'}
        elif line.startswith('REPORT action=1 '):
            result['crash'] = line
        elif line.startswith('TERMINATE '):
            result['terminate'] = int(line.split(' ')[1])
        elif line.startswith('RESULT entry-returned '):
            result['entry_returned'] = int(line.split(' ')[2])
        elif line.startswith('STATS '):
            result['stats'] = line
            for op, count in re.findall(r' enosys:(0x[0-9a-f]+)=(\d+)', line):
                result['enosys'][int(op, 16)] = int(count)
            for number, count in re.findall(r' gate:(\d+)=(\d+)', line):
                result['gate'][int(number)] = int(count)
        else:
            m = RECORD.match(line)
            if m:
                record = {'event': m[1], 'module': m[2], 'detail': m[3]}
                result['records'].append(record)
                if m[1] == 'stub':
                    result['stubs'].append(m[2] + '!' + m[3])
    return result


def first_enosys(text):
    m = re.search(r'^KERNEL ENOSYS op=0x([0-9a-f]+) size=\d+$', text, re.M)
    return int(m[1], 16) if m else None


class Snapshot:
    def __init__(self, service):
        self.service = service

    def files(self):
        result = {}
        for drive in ('C',):
            for path in (self.service.root / drive).rglob('*'):
                if path.is_file():
                    key = path.relative_to(self.service.root).as_posix()
                    result[key] = {'size': path.stat().st_size,
                                   'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
        return result


class TitleDriver:
    def __init__(self, spec, program, library, root):
        self.spec, self.program, self.library = spec, program, library
        self.service = host_services.Services(root, library, program)
        self.service.request_timeout = 60
        self.staged = set()

    def stage(self):
        found = payload(self.spec)
        if found is None:
            raise FileNotFoundError('payload absent for ' + self.spec['workload'])
        root = self.service.root
        if self.spec['entry']['kind'] == 'download':
            (root / 'C/Downloads').mkdir(exist_ok=True)
            target = root / 'C/Downloads' / found['download'].name
            self._copy(found['download'], target)
            self.staged.add(target.relative_to(root).as_posix())
            return 'C:\\Downloads\\' + found['download'].name
        # CD view: the read-only fixture drive D: carries the extracted tree.
        for source in found['tree'].rglob('*'):
            relative = source.relative_to(found['tree'])
            target = root / 'D' / relative
            if source.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            elif source.is_file() and not source.is_symlink():
                target.parent.mkdir(parents=True, exist_ok=True)
                self._copy(source, target)
        return None

    @staticmethod
    def _copy(source, target):
        # Same bytes, never a link into the payload store: a reflink when the
        # file system offers one, a plain copy otherwise.
        done = subprocess.run(['cp', '--reflink=auto', '--', str(source), str(target)],
                              capture_output=True)
        if done.returncode:
            shutil.copyfile(source, target)
        os.chmod(target, 0o644)

    def run(self, variant, image, timeout):
        before = Snapshot(self.service).files()
        start = len(self.service.trace)
        began = time.monotonic()
        try:
            code, output = self.service.run(image, variant.get('arguments', ''), timeout=timeout)
            timed_out = False
        except subprocess.TimeoutExpired as error:
            code, output, timed_out = None, 'TIMEOUT ' + ''.join(getattr(error, '__notes__', [])), True
        seconds = round(time.monotonic() - began, 2)
        children = []
        for child in self.service.children[1:]:
            try:
                child_code, child_output = child.finish(30)
            except subprocess.TimeoutExpired:
                child_code, child_output = None, 'TIMEOUT'
            children.append({'code': child_code, 'output': child_output,
                             'image': next((row[1] for row in self.service.trace
                                            if row[0] == 'spawn' and row[2] == child.process.pid), '?')})
        after = Snapshot(self.service).files()
        trace = self.service.trace[start:]
        return self.classify(variant, image, code, output, timed_out, seconds, before, after, trace, children)

    def classify(self, variant, image, code, output, timed_out, seconds, before, after, trace, children):
        facts = parse_output(output)
        written = sorted({row[1] for row in trace if row[0] == 'write'})
        spawned = [row[1] for row in trace if row[0] == 'spawn'][1:]
        delta = {key: value for key, value in after.items()
                 if key not in self.staged and before.get(key) != value}
        removed = sorted(key for key in before if key not in after)
        registry_ops = [row[2] for row in trace if row[0] == 'reply' and ' registry_op=' in row[2]]
        boundary = first_enosys(output)
        # A launcher that only spawns the real installer completes; the
        # variant's verdict then follows the child's run.
        child_results = []
        for child in children:
            child_facts = parse_output(child['output'])
            child_boundary = first_enosys(child['output'])
            child_verdict = ('refused' if child_facts['refuse'] else 'crashed' if child_facts['crash'] else
                             'boundary' if child_boundary in H1_BOUNDARIES else 'enosys' if child_boundary is not None
                             else 'completed' if child['code'] == 0 else 'exited')
            child_results.append({'image': child['image'], 'code': child['code'], 'verdict': child_verdict,
                                  'boundary': {'op': child_boundary, 'name': H1_BOUNDARIES.get(child_boundary)} if child_boundary is not None else None,
                                  'refuse': child_facts['refuse'], 'crash': child_facts['crash'],
                                  'stubs': child_facts['stubs'], 'enosys': {hex(k): v for k, v in child_facts['enosys'].items()},
                                  'tail': child['output'][-2000:]})
        verdict = 'exited'
        if facts['refuse']:
            verdict = 'refused'
        elif facts['crash']:
            verdict = 'crashed'
        elif timed_out:
            verdict = 'timeout'
        elif boundary in H1_BOUNDARIES:
            verdict = 'boundary'
        elif boundary is not None:
            verdict = 'enosys'
        elif code == 0 and facts['terminate'] in (0, None) and not facts['enosys']:
            verdict = 'completed'
            if child_results and any(c['verdict'] != 'completed' for c in child_results):
                verdict = next(c['verdict'] for c in child_results if c['verdict'] != 'completed')
                if verdict == 'boundary':
                    boundary = next(c['boundary']['op'] for c in child_results if c['verdict'] == 'boundary')
        phases = {name: 'not_run' for name in PHASES}
        engine = self.spec['engine']
        if engine == 'cd':
            phases['extraction'] = 'n/a (CD view, no self-extraction)'
        elif written:
            phases['extraction'] = f'observed ({len(written)} files written)'
        elif verdict == 'boundary':
            phases['extraction'] = 'not_reached (boundary before any write)'
        elif verdict in ('refused', 'crashed', 'enosys', 'timeout'):
            phases['extraction'] = 'fail (' + verdict + ')'
        if verdict == 'boundary':
            phases['ui'] = 'not_run (H1 boundary: %s; QEMU installers tier)' % H1_BOUNDARIES[boundary]
        elif verdict in ('refused', 'crashed', 'enosys'):
            phases['ui'] = 'blocked (' + verdict + ')'
        expected = self.spec['expected']
        if verdict == 'completed' and expected['status'] == 'verified':
            phases['copy'] = self.compare_files(delta, expected)
        elif verdict == 'completed':
            phases['copy'] = 'unverified (no reference manifest; delta recorded)'
        return {'variant': variant['name'], 'image': image, 'arguments': variant.get('arguments', ''),
                'code': code, 'seconds': seconds, 'verdict': verdict,
                'boundary': {'op': boundary, 'name': H1_BOUNDARIES.get(boundary)} if boundary is not None else None,
                'refuse': facts['refuse'], 'crash': facts['crash'], 'terminate': facts['terminate'],
                'entry_returned': facts['entry_returned'], 'stubs': facts['stubs'],
                'records': facts['records'], 'enosys': {hex(k): v for k, v in facts['enosys'].items()},
                'gate': facts['gate'], 'stats': facts['stats'],
                'spawned': spawned, 'children': child_results,
                'written': written[:200], 'written_count': len(written),
                'files_delta': delta, 'files_removed': removed, 'registry_requests': len(registry_ops),
                'phases': phases, 'output_tail': output[-4000:]}

    @staticmethod
    def compare_files(delta, expected):
        wanted = expected['files']
        missing = sorted(k for k in wanted if k not in delta)
        wrong = sorted(k for k in wanted if k in delta and (delta[k]['size'] != wanted[k]['size'] or
                       (wanted[k].get('sha256') and delta[k]['sha256'] != wanted[k]['sha256'])))
        extra = sorted(k for k in delta if k not in wanted)
        if missing or wrong or extra:
            return f'fail (missing {len(missing)}, wrong {len(wrong)}, extra {len(extra)})'
        return 'pass'

    def close(self):
        self.service.close()


def run_title(ident, program, library, out=OUT / 'titles', timeout=180):
    """Every variant of one title in its own fixture tree; the evidence record
    (JSON, adapter output and the H1 trace per variant) goes under `out`."""
    spec = load_spec(ident)
    folder = Path(out) / ident
    folder.mkdir(parents=True, exist_ok=True)
    record = {'schema_version': 1, 'workload': ident, 'engine': spec['engine'],
              'engine_detail': spec['engine_detail'], 'deferred': spec['deferred'],
              'receipt': receipt(spec), 'variants': []}
    if spec['deferred']:
        record['variants'] = []
        (folder / 'record.json').write_text(json.dumps(record, indent=2) + '\n')
        return record
    for variant in spec['variants']:
        with tempfile.TemporaryDirectory(dir=OUT) as root:
            driver = TitleDriver(spec, program, library, root)
            result = {'variant': variant['name'], 'verdict': 'error', 'output_tail': ''}
            try:
                staged = driver.stage()
                image = variant.get('run') or staged
                result = driver.run(variant, image, timeout)
            finally:
                try:
                    driver.close()
                except Exception as error:  # a surviving child is part of the evidence
                    result['close_error'] = repr(error)
            (folder / (variant['name'] + '.txt')).write_text(result.pop('output_tail'))
            (folder / (variant['name'] + '.trace')).write_text(
                '\n'.join(repr(row) for row in driver.service.trace) + '\n')
            record['variants'].append(result)
    (folder / 'record.json').write_text(json.dumps(record, indent=2) + '\n')
    return record


def win16_child(spec):
    """For a deferred title: the extracted setup stub's NE signature, so the
    deferral is a measured fact rather than an assumption."""
    child = spec['deferred'].get('ne_child')
    if not child:
        return None
    path = DOWNLOADS / spec['workload'] / 'extracted' / child
    if not path.is_file():
        return None
    raw = path.read_bytes()
    if raw[:2] != b'MZ' or len(raw) < 64:
        return 'not MZ'
    at = struct.unpack_from('<I', raw, 60)[0]
    return raw[at:at + 2].decode('latin-1') if at + 2 <= len(raw) else 'truncated'
