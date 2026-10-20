#!/usr/bin/env python3
"""Resolve acceptance PE imports against actual builtin exports and coverage.

Metadata only: never download or execute a title or rebuild providers. Missing
media, dynamic observations and unclassified exports keep the lead gate open.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from scripts.win32 import census, workloads

RESOLUTIONS = ('implemented', 'W21 stub', 'W24 refusal', 'unresolved')
DIRECTIVES = {
    'ntdll': 'f5-03/f5-04/f5-05B', 'kernel32': 'f5-06/f5-20A',
    'advapi32': 'f5-07A/f5-20A', 'version': 'f5-07A/f5-20A', 'lz32': 'f5-07A/f5-20A',
    'user32': 'f5-10/f5-20B', 'gdi32': 'f5-10/f5-20B', 'ddraw': 'f5-11',
    'dsound': 'f5-12', 'winmm': 'f5-12B', 'dinput': 'f5-13', 'dinput8': 'f5-13',
    'wsock32': 'f5-06 (offline)', 'dsetup': 'f5-15A', 'dsetup32': 'f5-15A',
    'mciavi32': 'f5-12B', 'msacm32': 'f5-12B', 'avifil32': 'f5-12B',
    'msvfw32': 'f5-12B',
}


def sha(path):
    return workloads.hash_file(path)['sha256']


def status(value):
    if value.startswith('W21') or 'W21 stub' in value:
        return 'W21 stub'
    if value.startswith('W24'):
        return 'W24 refusal'
    if value == 'implemented':
        return 'implemented'
    return 'unresolved'


def coverage_rows(data, dll=None):
    """Normalize the existing builders' coverage and stubs.json formats."""
    if 'dlls' in data:
        for entry in data['dlls']:
            for export in entry.get('exports', []):
                if export.get('implementation'):
                    continue  # actual PE forwarder resolves the target
                for name in (export.get('name'), '#' + str(export['ordinal']) if export.get('ordinal') else None):
                    if name:
                        yield entry['name'].lower(), name, 'W21 stub', 'f5-08'
    elif isinstance(data.get('exports'), list):
        for row in data['exports']:
            yield dll, row['name'], status(row['status']), row.get('directive')
    else:
        for name, value in data.items():
            if isinstance(value, str) and dll:
                yield dll, name, status(value), None
            elif isinstance(value, dict):
                yield from coverage_rows(value, name.lower())
            elif isinstance(value, list):
                for row in value:
                    yield name.lower(), row['name'], status(row.get('status', '')), row.get('directive')


class Resolver:
    def __init__(self, providers=(), coverage=()):
        self.exports, self.annotations, self.inputs = {}, {}, []
        for folder in providers:
            folder = Path(folder)
            if not folder.is_dir():
                continue
            store = folder / 'builtin-store.json'
            declared = json.loads(store.read_text())['dlls'] if store.is_file() else None
            paths = [folder / d['path'] for d in declared] if declared is not None else sorted(folder.iterdir())
            for path in paths:
                if path.suffix.lower() not in ('.dll', '.drv', '.acm') or not path.is_file():
                    continue
                digest = sha(path)
                if declared is not None:
                    record = next(d for d in declared if folder / d['path'] == path)
                    if record['sha256'] != digest:
                        raise ValueError('builtin hash mismatch: ' + path.name)
                name = path.name.lower()
                if name in self.exports:
                    raise ValueError('duplicate provider: ' + name)
                pe = census.PE(path.read_bytes())
                if not pe.flags & 0x2000:
                    raise ValueError('provider is not a DLL: ' + path.name)
                symbols = {}
                for entry in pe.exports():
                    for symbol in ['#' + str(entry['ordinal']), *entry['names']]:
                        symbols[symbol] = entry
                self.exports[name] = symbols
                self.inputs.append({'path': str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else path.name,
                                    'sha256': digest})
        # Explicit coverage order permits replacement facades to supersede the
        # old generated placeholder table. Still require a real matching export.
        for path in coverage:
            path = Path(path)
            if not path.is_file():
                continue
            stem = path.name.removesuffix('-coverage.json')
            dll = stem if stem.endswith(('.dll', '.drv', '.acm')) else stem + '.dll'
            for library, name, resolution, directive in coverage_rows(json.loads(path.read_text()), dll):
                if library:
                    self.annotations[library.lower(), name] = (resolution, directive)
            self.inputs.append({'path': str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else path.name,
                                'sha256': sha(path)})

    def resolve(self, dll, symbol, chain=()):
        dll = dll.lower()
        key = dll + '!' + symbol
        if key in chain or len(chain) >= 16:
            return {'resolution': 'unresolved', 'directive': None, 'reason': 'forwarder-cycle-or-depth', 'chain': [*chain, key]}
        chain = (*chain, key)
        entry = self.exports.get(dll, {}).get(symbol)
        annotation = self.annotations.get((dll, symbol))
        if entry is None:
            # W24 is an explicit policy refusal, never guessed from an absent
            # DLL or from a filename/NE header. A decision needs its directive.
            if annotation and annotation[0] == 'W24 refusal' and annotation[1]:
                return {'resolution': annotation[0], 'directive': annotation[1], 'chain': list(chain)}
            return {'resolution': 'unresolved', 'directive': None, 'reason': 'provider-or-export-absent', 'chain': list(chain)}
        if entry.get('forwarder'):
            target, _, name = entry['forwarder'].partition('.')
            if not target or not name:
                return {'resolution': 'unresolved', 'directive': None, 'reason': 'invalid-forwarder', 'chain': list(chain)}
            return self.resolve(target + '.dll', name, chain)
        # The ordinal and named import must receive the same classification.
        aliases = [symbol, *entry['names'], '#' + str(entry['ordinal'])]
        values = [self.annotations[dll, n] for n in aliases if (dll, n) in self.annotations]
        resolution, directive = values[0] if values else ('unresolved', None)
        if any(v[0] != resolution for v in values):
            resolution, directive = 'unresolved', None
        directive = directive or (DIRECTIVES.get(dll.rsplit('.', 1)[0]) if resolution != 'unresolved' else None)
        if not directive:
            resolution = 'unresolved'
        return {'resolution': resolution, 'directive': directive, 'chain': list(chain),
                **({'reason': 'export-classification-pending'} if resolution == 'unresolved' else {})}


def generate(root=ROOT, resolver=None):
    root = Path(root)
    resolver = resolver or Resolver()
    sys.path.insert(0, str(root / 'scripts/test'))
    from win32_fixtures import source_directory
    result = {'schema_version': 1, 'scope': 'acceptance-set', 'runtime_evidence': 'pending by the lead',
              'providers': resolver.inputs, 'workloads': [], 'imports': []}
    for row in workloads.load(root / 'tests/win32/workloads.json')['workloads']:
        if row['gate'] != 'F5':
            continue
        item = {'id': row['id'], 'title': row['title'], 'executables': [], 'missing': [], 'dynamic_census': 'pending'}
        for source in ('extracted', 'installed'):
            directory = source_directory(row, {'source': source}, root)
            if not directory.is_dir():
                item['missing'].append(source + '_tree_absent')
                continue
            if directory.is_symlink() or not directory.resolve().is_relative_to(root.resolve()):
                raise ValueError('acceptance tree escapes the worktree')
            for path in sorted(directory.rglob('*')):
                if path.is_symlink():
                    raise ValueError('symlink in acceptance census: ' + path.name)
                if not path.is_file() or path.suffix.lower() not in ('.exe', '.dll', '.drv', '.acm'):
                    continue
                binary = {'source': source, 'path': path.relative_to(directory).as_posix(), 'sha256': sha(path)}
                try:
                    metadata = census.inspect(path.read_bytes())
                except census.PEError as error:
                    binary.update(status='unresolved', reason=str(error))
                else:
                    binary['status'] = 'inspected'
                    for imported in metadata['imports']:
                        for symbol in imported['symbols']:
                            result['imports'].append({'workload': row['id'], 'source': source, 'path': binary['path'],
                                                      'binary_sha256': binary['sha256'], 'kind': imported['kind'],
                                                      'dll': imported['dll'].lower(), 'symbol': symbol,
                                                      **resolver.resolve(imported['dll'], symbol)})
                item['executables'].append(binary)
        if not item['executables']:
            item['missing'].append('acceptance_executables_absent')
        result['workloads'].append(item)
    counts = Counter(r['resolution'] for r in result['imports'])
    result['counts'] = {key: counts[key] for key in RESOLUTIONS}
    result['complete'] = False  # static imports alone never close the dynamic/opaque-child gate
    result['unresolved'] = [r for r in result['imports'] if r['resolution'] == 'unresolved']
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--providers', type=Path, action='append', default=[])
    parser.add_argument('--coverage', type=Path, action='append', default=[])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    data = generate(resolver=Resolver(args.providers, args.coverage))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(data, indent=2, sort_keys=True) + '\n')
    print(json.dumps({'counts': data['counts'], 'workloads': len(data['workloads']), 'complete': data['complete']}))


if __name__ == '__main__':
    main()
