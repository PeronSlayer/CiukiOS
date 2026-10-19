#!/usr/bin/env python3
"""Pinned Wine source observations and PE import evidence, never a loader gate.

SPDX-License-Identifier: GPL-2.0-only
Source token matches are candidates, not a linked or runtime import closure.
"""
import argparse
from bisect import bisect_right
from collections import defaultdict
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
PIN = ROOT / 'config/wine-pin.json'
AUDIT = ROOT / 'config/wine-audit.json'
CATALOG = ROOT / 'tests/host/win32/wine_specs'
CORE = ('ntdll', 'kernel32', 'user32', 'gdi32')
ROOTS = ('shell32', 'comctl32', 'comdlg32', 'ole32', 'oleaut32', 'rpcrt4',
         'msvcrt', 'version', 'setupapi', 'advapi32', 'winmm', 'dinput', 'ddraw',
         'dsound', 'shlwapi', 'riched20', 'riched32', 'cabinet', 'advpack',
         'lz32', 'msacm32', 'msadp32.acm', 'imaadp32.acm', 'msg711.acm',
         'msvfw32', 'msrle32', 'msvidc32')
STATIC = {'uuid', 'dxguid', 'winecrt0'}
MODERN = ('kernelbase', 'combase', 'coml2', 'sechost', 'shcore', 'win32u')
BEGIN = '<!-- BEGIN GENERATED WINE AUDIT -->'
END = '<!-- END GENERATED WINE AUDIT -->'
SPEC = re.compile(r'^(\d+|@)\s+(\w+)\s+((?:-\S+\s+)*)([^\s(]+)(.*)$')
C_LEX = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
ALIASES = {f'{op}{kind}LongPtr{enc}': f'{op}{kind}Long{enc}'
           for op in ('Get', 'Set') for kind in ('Window', 'Class') for enc in ('A', 'W')}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def dump(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')


def arch_matches(flags):
    for flag in flags:
        if flag.startswith('-arch='):
            arches = flag[6:].split(',')
            if 'i386' not in arches and 'win32' not in arches:
                return False
    return True


def parse_spec(text):
    """Parse i386 names, explicit ordinals, stub/private flags and forwards.

    Auto ordinals are deliberately None: allocating them requires winebuild.
    Anonymous ordinal-only stubs have no importable name and are omitted.
    """
    out = {}
    for number, raw in enumerate(text.splitlines(), 1):
        line = re.split(r'\s+#', raw.strip(), maxsplit=1)[0]
        if line.startswith('#'):
            continue
        if not line:
            continue
        if line.startswith('apiset '):
            # winebuild(1): "apiset apiset_dll = target.dll [host.dll:target.dll]"
            # declares an API-set alias of the module (kernelbase.spec in Wine
            # 10.0); it defines no importable export and is not an i386 name.
            continue
        match = SPEC.fullmatch(line)
        if not match:
            raise ValueError(f'unsupported spec syntax at line {number}: {raw}')
        ordinal, kind, flag_text, name, rest = match.groups()
        flags = flag_text.split()
        if not arch_matches(flags) or name == '@':
            continue
        if rest.startswith('('):
            end = rest.find(')')
            if end < 0:
                raise ValueError(f'unclosed spec arguments at line {number}')
            rest = rest[end + 1:]
        target = rest.strip().split()[0] if rest.strip() else None
        forward = target if target and re.fullmatch(r'[\w-]+\.[^\s]+', target) else None
        if name in out:
            raise ValueError(f'duplicate i386 export {name} at line {number}')
        out[name] = {'line': number, 'ordinal': int(ordinal) if ordinal != '@' else None,
                     'kind': kind, 'flags': flags, 'forward': forward,
                     'import': '-import' in flags, 'link_name': target}
    return out


def parse_makefile(text):
    values = {}
    for line in text.replace('\\\n', ' ').splitlines():
        match = re.match(r'^(\w+)\s*=\s*(.*)$', line)
        if match:
            values[match[1]] = match[2].split('#', 1)[0].strip()
    direct = values.get('IMPORTS', '')
    delayed = values.get('DELAYIMPORTS', '')
    variables = sorted(set(re.findall(r'\$\(([^)]+)\)', direct + ' ' + delayed)))
    words = lambda value: re.sub(r'\$\([^)]+\)', '', value).split()
    imports = words(direct)
    defaults = []
    if values.get('MODULE') and '-nodefaultlibs' not in values.get('EXTRADLLFLAGS', '').split():
        crt = next((name for name in imports if name.startswith(('msvcr', 'ucrt'))), None)
        if not crt and not values.get('MODULE', '').startswith(('msvcr', 'ucrt', 'crtdll')):
            crt = 'ucrtbase'
        defaults = ['winecrt0'] + ([crt] if crt else []) + ['kernel32', 'ntdll']
    return {'module': values.get('MODULE'), 'direct': imports, 'delay': words(delayed),
            'defaults': defaults, 'variables': variables, 'unixlib': values.get('UNIXLIB'),
            'sources': values.get('SOURCES', '').split(), 'parent': values.get('PARENTSRC')}


def blank_literals(text):
    return C_LEX.sub(lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0]), text)


def observe(text, path, catalogs, by_name=None):
    """First source occurrence per provider/name; comments/literals excluded."""
    clean = blank_literals(text)
    starts = [0] + [m.end() for m in re.finditer('\n', clean)]
    if by_name is None:
        by_name = defaultdict(list)
        for dll, exports in catalogs.items():
            for name, entry in exports.items():
                if '-noname' not in entry['flags']:
                    by_name[name].append(dll)
    found = {}
    for match in re.finditer(r'\b[A-Za-z_]\w*\b', clean):
        token = match[0]
        name = ALIASES.get(token, token)
        for dll in by_name.get(name, ()):
            key = dll + '!' + name
            found.setdefault(key, {'path': path, 'line': bisect_right(starts, match.start()), 'token': token})
    strings = []
    # Keep literal names independently; these are possible runtime references,
    # not inferred calls to a specific provider or proof of a LoadLibrary path.
    uncommented = C_LEX.sub(lambda m: m[0] if m[0].startswith(('"', "'")) else ''.join('\n' if c == '\n' else ' ' for c in m[0]), text)
    for match in re.finditer(r'"([A-Za-z_][\w.-]*)"', uncommented):
        name = match[1]
        providers = sorted(dll for dll, exports in catalogs.items() if name in exports)
        if providers or name.lower().endswith(('.dll', '.drv', '.acm', '.exe')):
            strings.append({'name': name, 'providers': providers, 'path': path,
                            'line': uncommented.count('\n', 0, match.start()) + 1})
    requests = sorted(set(re.findall(r'\b(?:WINE_)?SERVER_START_REQ\s*\(\s*(\w+)\s*\)', clean)))
    unix = sorted(set(re.findall(r'\b(?:WINE_UNIX_CALL|__wine_unix_call)\s*\(([^;\n]*)', clean)))
    runtime = []
    for match in re.finditer(r'\b(?:LoadLibrary(?:Ex)?[AW]?|GetProcAddress|LdrLoadDll|LdrGetProcedureAddress)\s*\(', clean):
        line = clean.count('\n', 0, match.start()) + 1
        runtime.append({'path': path, 'line': line})
    return found, strings, requests, unix, runtime


def closure(graph, start):
    todo, seen, missing = [start], set(), set()
    while todo:
        name = todo.pop()
        if name in seen or name in STATIC:
            continue
        if name not in graph:
            missing.add(name)
            continue
        seen.add(name)
        todo.extend(graph[name]['direct'] + graph[name]['delay'] + graph[name]['defaults'])
    return sorted(seen), sorted(missing)


def collect(source, pin):
    graph, specs, spec_paths = {}, {}, {}
    # Only downloaded/pinned directories are scanned, never the repository tree.
    for makefile in sorted((source / 'dlls').glob('*/Makefile.in')):
        info = parse_makefile(makefile.read_text())
        name = makefile.parent.name
        if info['module'] and info['module'].endswith(('.exe16', '.dll16', '.drv16')):
            # Win16 modules (krnl386.exe16, user.exe16, ...) are not reusable
            # upper DLLs: the Win16 subsystem is CiukiOS's own (f5-19), and
            # their specs mix 16-bit pascal and win32 entries under one name.
            continue
        if info['module'] and info['module'].endswith('.dll'):
            name = info['module'][:-4]
        elif info['module'] and info['module'].endswith('.drv'):
            # winspool.drv keeps its bare name (the audit tables use it); a
            # driver whose stem is also a DLL module (msacm32.drv beside
            # msacm32.dll in Wine 10.0) keeps the full module name instead of
            # overwriting the DLL's entry with the driver's sources.
            stem = info['module'][:-4]
            name = info['module'] if (source / 'dlls' / stem).is_dir() else stem
        graph[name] = info
        info['makefile'] = makefile.relative_to(source).as_posix()
        for path in sorted(makefile.parent.glob('*.spec')):
            # Test/fake secondary specs must not replace the module's spec.
            if info['module'] and path.stem not in (name, info['module'], makefile.parent.name):
                continue
            specs[name] = parse_spec(path.read_text())
            spec_paths[name] = path.relative_to(source).as_posix()
    missing = set(ROOTS) - specs.keys()
    if missing:
        raise ValueError(f'missing root specs: {sorted(missing)}')
    rows = {}
    by_name = defaultdict(list)
    for provider, exports in specs.items():
        for name, entry in exports.items():
            if '-noname' not in entry['flags']:
                by_name[name].append(provider)
    for dll in ROOTS:
        directory = source / 'dlls' / dll
        paths = [directory / name for name in graph[dll]['sources'] if name.endswith(('.c', '.cpp', '.idl'))]
        paths += sorted(directory.glob('*.h'))
        references, literals, requests, unix, runtime, private_identifiers = {}, [], set(), set(), [], set()
        for path in paths:
            if not path.is_file():
                raise ValueError(f'missing declared source: {path}')
            observations = observe(path.read_text(), path.relative_to(source).as_posix(), specs, by_name)
            private_identifiers.update(re.findall(r'\b__wine_\w+\b', blank_literals(path.read_text())))
            references.update({k: v for k, v in observations[0].items() if k not in references})
            literals.extend(observations[1]); requests.update(observations[2]); unix.update(observations[3]); runtime.extend(observations[4])
        candidates = {core: sorted(key.split('!', 1)[1] for key in references if key.startswith(core + '!')) for core in CORE}
        modern = {core: sorted(key.split('!', 1)[1] for key in references if key.startswith(core + '!')) for core in MODERN}
        dependencies, unresolved = closure(graph, dll)
        forwards = {name: entry['forward'] for name, entry in specs[dll].items() if entry['forward']}
        import_exports = {}
        link_order = graph[dll]['direct'] + graph[dll]['delay'] + graph[dll]['defaults']
        for name, entry in specs[dll].items():
            if entry['import']:
                import_exports[name] = [provider for provider in link_order if name in specs.get(provider, {}) and provider != dll]
        private = sorted(key for key in references if key.split('!', 1)[1].startswith(('__wine_', 'wine_')))
        rows[dll] = {'evidence': 'source-reference-candidates; not linked imports',
                     'source_files': sorted(path.relative_to(source).as_posix() for path in paths),
                     'core_exports': candidates, 'core_counts': {name: len(exports) for name, exports in candidates.items()},
                     'modern_exports': modern,
                     'references': {key: value for key, value in references.items() if key.split('!', 1)[0] in CORE or key in private},
                     'wine_private_references': private, 'forwards': forwards, 'import_exports': import_exports,
                     'wine_private_identifiers': sorted(private_identifiers),
                     'wineserver_requests_direct': sorted(requests), 'unix_calls_direct': sorted(unix),
                     'runtime_sites': runtime, 'runtime_literals': literals,
                     'module_closure': dependencies, 'unresolved_modules': unresolved,
                     'pe_build': 'not-built',
                     'subset': 'full upstream module; requested parts/subset not yet specified' if dll in ('rpcrt4', 'advapi32', 'winmm') else 'full upstream module'}
    union = {core: sorted({name for row in rows.values() for name in row['core_exports'][core]}) for core in CORE}
    audit = {'schema_version': 1, 'release': pin['release'], 'commit': pin['commit'],
            'status': 'provisional-source-audit; core-freeze-blocked', 'roots': rows,
            'graph': graph, 'spec_paths': spec_paths, 'core_union': union,
            'core_union_counts': {dll: len(exports) for dll, exports in union.items()},
            'title_census': {'status': 'not-present-in-this-worktree', 'core_exports': None}}
    audit['modern_reference_candidates'] = {dll: sorted({name for row in rows.values() for name in row['modern_exports'][dll]}) for dll in MODERN}
    excluded = {'dinput', 'ddraw', 'dsound', 'winmm', 'advapi32', 'msvcrt'}
    audit['p4_p5_only_source_union_counts'] = {core: len({name for dll, row in rows.items() if dll not in excluded for name in row['core_exports'][core]}) for core in CORE}
    survey = {}
    for path in ('dlls/ntdll/unix/server.c', 'dlls/ntdll/unix/file.c', 'dlls/ntdll/unix/sync.c'):
        clean = blank_literals((source / path).read_text())
        requests = {}
        for match in re.finditer(r'\bSERVER_START_REQ\s*\(\s*(\w+)\s*\)', clean):
            requests.setdefault(match[1], []).append(clean.count('\n', 0, match.start()) + 1)
        survey[path] = requests
    audit['server_backend_survey'] = {'evidence': 'three NTDLL Unix files; exact named source request sites, NOT per-root reachable protocol closure', 'files': survey}
    helpers = {}
    for path in ('include/wine/debug.h', 'include/wine/exception.h', 'include/wine/unixlib.h', 'dlls/winecrt0/debug.c', 'dlls/winecrt0/unix_lib.c', 'dlls/winecrt0/register.c', 'dlls/winecrt0/exception.c', 'dlls/winecrt0/delay_load.c'):
        found, _, _, unix, runtime = observe((source / path).read_text(), path, specs, by_name)
        helpers[path] = {'core_candidates': sorted(k for k in found if k.split('!', 1)[0] in CORE),
                         'private_candidates': sorted(k for k in found if k.split('!', 1)[1].startswith(('__wine_', 'wine_'))),
                         'unix_calls': unix, 'runtime_sites': runtime}
    audit['helper_survey'] = helpers
    return audit


def verify_source(source, pin):
    for relative, entry in pin['source_files'].items():
        data = (source / relative).read_bytes()
        if digest(data) != entry['sha256']:
            raise ValueError(f'pinned source changed: {relative}')
    if (source / 'VERSION').read_text().strip() != 'Wine version ' + pin['release']:
        raise ValueError('wrong Wine VERSION')


def pe_evidence(path):
    spec = importlib.util.spec_from_file_location('ciuki_pe_import_census', ROOT / 'scripts/pe_import_census.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    data = path.read_bytes()
    result = module.parse_pe(data)
    if result['format'] != 'PE32' or result['machine'] != '0x014c' or not result['characteristics'] & 0x2000:
        raise ValueError('expected an i386 PE32 DLL')
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    opt = pe + 24
    # The production import parser has already checked header/section bounds.
    count = struct.unpack_from('<H', data, pe + 6)[0]
    opt_size = struct.unpack_from('<H', data, pe + 20)[0]
    sections = [struct.unpack_from('<IIII', data, opt + opt_size + i * 40 + 8) for i in range(count)]
    header_size = struct.unpack_from('<I', data, opt + 60)[0]

    def offset(rva, size=1):
        if rva + size <= header_size:
            return rva
        for _, base, raw_size, raw in sections:
            if base <= rva and rva + size <= base + raw_size:
                return raw + rva - base
        raise ValueError('export RVA has no file backing')

    def string(rva):
        at = offset(rva)
        end = data.find(b'\0', at, at + 4096)
        if end == -1:
            raise ValueError('unterminated export name')
        offset(rva, end - at + 1)
        return data[at:end].decode('ascii')

    exports, forwards = {}, {}
    rva, size = struct.unpack_from('<II', data, opt + 96)
    if rva:
        if size < 40:
            raise ValueError('truncated export directory')
        entry = struct.unpack_from('<IIHHIIIIIII', data, offset(rva, 40))
        base, nfunc, nname, funcs, names, ordinals = entry[5:]
        if nfunc > 65536 or nname > 65536:
            raise ValueError('export table exceeds census limit')
        offset(funcs, 4 * nfunc); offset(names, 4 * nname); offset(ordinals, 2 * nname)
        for i in range(nname):
            name = string(struct.unpack_from('<I', data, offset(names + 4 * i, 4))[0])
            index = struct.unpack_from('<H', data, offset(ordinals + 2 * i, 2))[0]
            if index >= nfunc:
                raise ValueError('bad export ordinal index')
            addr = struct.unpack_from('<I', data, offset(funcs + 4 * index, 4))[0]
            exports[name] = base + index
            if rva <= addr < rva + size:
                forwards[name] = string(addr)
    result.update({'sha256': digest(data), 'file_bytes': len(data), 'exports': exports, 'forwards': forwards,
                   'resource_bytes': struct.unpack_from('<I', data, opt + 96 + 2 * 8 + 4)[0],
                   'stack_reserve': struct.unpack_from('<I', data, opt + 72)[0],
                   'stack_commit': struct.unpack_from('<I', data, opt + 76)[0],
                   'heap_reserve': struct.unpack_from('<I', data, opt + 80)[0],
                   'heap_commit': struct.unpack_from('<I', data, opt + 84)[0]})
    return result


def render_tables(audit):
    lines = [BEGIN, '', '## Reproducible source-reference tables', '',
             'These counts and lists are i386 source-reference candidates, not exact PE',
             'imports. They include conditional/unreachable source paths and exclude',
             'unexpanded IDL/header-generated code. Private/header helpers are additional',
             'requirements; zero direct Unix/server sites does not mean zero transitive use.', '',
             '| DLL | ntdll | kernel32 | user32 | gdi32 | Module closure | Direct server / Unix sites | PE evidence |',
             '| --- | ---: | ---: | ---: | ---: | ---: | --- | --- |']
    for dll, row in sorted(audit['roots'].items()):
        counts = ' | '.join(str(row['core_counts'][core]) for core in CORE)
        direct = ', '.join(row['wineserver_requests_direct']) or 'none observed'
        unix = ', '.join(row['unix_calls_direct']) or 'none observed'
        lines.append(f"| {dll} | {counts} | {len(row['module_closure'])} | {direct} / {unix} | {row['pe_build']} |")
    lines += ['', '| Core | Unique source-reference candidates |', '| --- | ---: |']
    lines.extend(f"| {core} | {audit['core_union_counts'][core]} |" for core in CORE)
    lines += ['', '### Modern provider candidates', '',
              'Names may overlap the four-core table; do not add these counts to it.', '',
              '| Provider | Unique source-reference candidates |', '| --- | ---: |']
    lines.extend(f"| {dll} | {len(names)} |" for dll, names in sorted(audit['modern_reference_candidates'].items()))
    if 'build_experiment' in audit:
        pe = audit['build_experiment']['pe']
        lines += ['', '### Actual Wine 10.0 version.dll PE imports', '',
                  '| Provider | Count | Exact linked names |', '| --- | ---: | --- |']
        for dll, names in sorted(pe['imports'].items()):
            lines.append(f"| {dll} | {len(names)} | {', '.join('`' + name + '`' for name in names)} |")
        lines += ['', '| Export | PE forwarder |', '| --- | --- |']
        lines.extend(f'| {name} | `{target}` |' for name, target in sorted(pe['forwards'].items()))
        lines += ['', f"SHA-256 `{pe['sha256']}`; file {pe['file_bytes']} bytes; SizeOfImage {pe['image_size']} bytes; resource directory {pe['resource_bytes']} bytes."]
    lines += ['', '### NTDLL Unix backend request survey', '',
              'These are exact request tokens in the three pinned files below, with line',
              'locations in JSON. This survey is not the reachable protocol of any root DLL.', '',
              '| Source file | Count | Named requests |', '| --- | ---: | --- |']
    for path, requests in sorted(audit['server_backend_survey']['files'].items()):
        lines.append(f"| {path} | {len(requests)} | {', '.join('`' + name + '`' for name in sorted(requests))} |")
    lines += ['', '### Per-DLL export names', '']
    for dll, row in sorted(audit['roots'].items()):
        lines += [f'#### {dll}', '', '| Core | Count | Names |', '| --- | ---: | --- |']
        for core in CORE:
            names = ', '.join('`' + name + '`' for name in row['core_exports'][core]) or '—'
            lines.append(f"| {core} | {row['core_counts'][core]} | {names} |")
        lines += ['', f"Private source names: {', '.join(row['wine_private_references']) or 'none observed directly'}. "
                  f"Explicit forwarders: {len(row['forwards'])}; `-import` exports: {len(row['import_exports'])}. "
                  'Exact targets, potential providers and source locations are in the JSON census.', '']
    lines += ['### Recursive Makefile closure', '',
              'Direct, delay and implicit default libraries are kept separately. Static',
              'GUID/helper libraries and unresolved configure variables are not PE DLLs.',
              'This is the build graph, including cycles; it is not a runtime working set.', '',
              '| Module | Direct | Delay | Defaults | Unix companion | Configure variables |',
              '| --- | --- | --- | --- | --- | --- |']
    for dll, row in sorted(audit['graph'].items()):
        values = [' '.join(row[key]) or '—' for key in ('direct', 'delay', 'defaults')]
        lines.append(f"| {dll} | {' | '.join(values)} | {row['unixlib'] or '—'} | {', '.join(row['variables']) or '—'} |")
    lines += ['', END]
    return '\n'.join(lines)


def validate(audit, pin, catalog=CATALOG):
    if audit['commit'] != pin['commit'] or audit['release'] != pin['release']:
        raise ValueError('audit/pin release mismatch')
    specs = {}
    for dll, relative in audit['spec_paths'].items():
        data = (catalog / relative).read_bytes()
        if digest(data) != pin['source_files'][relative]['sha256']:
            raise ValueError(f'spec snapshot hash mismatch: {relative}')
        specs[dll] = parse_spec(data.decode())
    for dll, row in audit['graph'].items():
        relative = row['makefile']
        data = (catalog / relative).read_bytes()
        if digest(data) != pin['source_files'][relative]['sha256']:
            raise ValueError(f'Makefile snapshot hash mismatch: {relative}')
        parsed = parse_makefile(data.decode())
        parsed['makefile'] = relative
        if parsed != row:
            raise ValueError(f'Makefile graph drift: {dll}')
    if set(audit['roots']) != set(ROOTS):
        raise ValueError('missing or extra audit roots')
    for dll, row in audit['roots'].items():
        for core in CORE:
            names = row['core_exports'][core]
            if names != sorted(set(names)) or row['core_counts'][core] != len(names):
                raise ValueError(f'bad counts/duplicates: {dll} -> {core}')
            for name in names:
                if name not in specs[core]:
                    raise ValueError(f'export absent from pinned spec: {core}!{name}')
                ref = row['references'][core + '!' + name]
                if ref['path'] not in row['source_files'] or ref['path'] not in pin['source_files'] or ref['line'] < 1:
                    raise ValueError('missing pinned source reference')
        actual_forwards = {name: entry['forward'] for name, entry in specs[dll].items() if entry['forward']}
        if row['forwards'] != actual_forwards:
            raise ValueError(f'forwarder drift: {dll}')
        for target in row['forwards'].values():
            provider, name = target.split('.', 1)
            entries = specs.get(provider, {})
            if name.startswith('#'):
                if not any(entry['ordinal'] == int(name[1:]) for entry in entries.values()):
                    raise ValueError(f'forward ordinal absent from pinned spec: {target}')
            elif name not in entries:
                raise ValueError(f'forward export absent from pinned spec: {target}')
        if set(row['import_exports']) != {name for name, entry in specs[dll].items() if entry['import']}:
            raise ValueError(f'import-export drift: {dll}')
        for name, providers in row['import_exports'].items():
            if any(name not in specs.get(provider, {}) for provider in providers):
                raise ValueError(f'bad import provider: {dll}!{name}')
        modules, missing = closure(audit['graph'], dll)
        if modules != row['module_closure'] or missing != row['unresolved_modules']:
            raise ValueError(f'closure drift: {dll}')
        for provider, names in row['modern_exports'].items():
            if names != sorted(set(names)) or any(name not in specs[provider] for name in names):
                raise ValueError('bad modern-provider reference')
    for core in CORE:
        names = sorted({name for row in audit['roots'].values() for name in row['core_exports'][core]})
        if audit['core_union'][core] != names or audit['core_union_counts'][core] != len(names):
            raise ValueError('union count drift')
    for provider in MODERN:
        names = sorted({name for row in audit['roots'].values() for name in row['modern_exports'][provider]})
        if audit['modern_reference_candidates'][provider] != names:
            raise ValueError('modern union drift')
    excluded = {'dinput', 'ddraw', 'dsound', 'winmm', 'advapi32', 'msvcrt'}
    subset = {core: len({name for dll, row in audit['roots'].items() if dll not in excluded for name in row['core_exports'][core]}) for core in CORE}
    if audit['p4_p5_only_source_union_counts'] != subset:
        raise ValueError('P4/P5 subset count drift')
    for survey in audit['helper_survey'].values():
        for key in survey['core_candidates'] + survey['private_candidates']:
            provider, name = key.split('!', 1)
            if name not in specs[provider]:
                raise ValueError('helper absent from pinned spec')
    for dll, names in audit.get('build_experiment', {}).get('pe', {}).get('imports', {}).items():
        provider = dll.casefold().removesuffix('.dll')
        if any(name not in specs.get(provider, {}) for name in names):
            raise ValueError(f'actual PE import absent from pinned spec: {dll}')
    if 'build_experiment' in audit:
        pe = audit['build_experiment']['pe']
        if set(pe['exports']) != set(specs['version']) or pe['forwards'] != audit['roots']['version']['forwards']:
            raise ValueError('actual PE exports/forwards differ from pinned spec')
    return specs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    for name in ('collect', 'verify-source'):
        sub = commands.add_parser(name); sub.add_argument('source', type=Path)
    sub = commands.add_parser('pe'); sub.add_argument('binary', type=Path); sub.add_argument('output', type=Path)
    commands.add_parser('check')
    commands.add_parser('render')
    args = parser.parse_args()
    pin = json.loads(PIN.read_text())
    if args.command == 'pe':
        dump(args.output, pe_evidence(args.binary)); return
    if args.command == 'verify-source':
        verify_source(args.source, pin); print('PASS pinned Wine sources'); return
    if args.command == 'collect':
        verify_source(args.source, pin)
        audit = collect(args.source, pin)
        if AUDIT.exists():
            old = json.loads(AUDIT.read_text())
            if old.get('commit') == audit['commit']:
                for field in ('build_experiment', 'memory_proxy'):
                    if field in old:
                        audit[field] = old[field]
                if 'build_experiment' in audit:
                    audit['roots']['version']['pe_build'] = 'built-experiment'
        for relative in list(audit['spec_paths'].values()) + [row['makefile'] for row in audit['graph'].values()]:
            target = CATALOG / relative; target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(args.source / relative, target)
        dump(AUDIT, audit)
        print(f"Collected {len(audit['roots'])} roots and {len(audit['graph'])} build nodes")
        return
    audit = json.loads(AUDIT.read_text())
    validate(audit, pin)
    if args.command == 'render':
        path = ROOT / 'docs/design/win32-wine-dependency-audit.md'
        document = path.read_text()
        generated = render_tables(audit)
        if BEGIN in document:
            start, end = document.index(BEGIN), document.index(END) + len(END)
            document = document[:start] + generated + document[end:]
        else:
            document = document.rstrip() + '\n\n' + generated + '\n'
        path.write_text(document)
    else:
        document = (ROOT / 'docs/design/win32-wine-dependency-audit.md').read_text()
        if render_tables(audit) not in document:
            raise ValueError('audit Markdown tables differ from JSON')
        print('PASS Wine audit/spec/table consistency (source evidence only)')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, KeyError) as exc:
        sys.exit(f'Wine audit: {exc}')
