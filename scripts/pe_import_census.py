#!/usr/bin/env python3
"""Read-only PE/INF driver census; no third-party modules or driver execution.

See docs/design/ndis-miniport-host-contract.md for sources and limitations.
Counts refer to paths unless explicitly marked content-deduplicated. INF OS
decorations are minimum targets, not proof of a driver's NDIS runtime version.
SPDX-License-Identifier: GPL-2.0-only
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MAX_PE = 64 * 1024 * 1024
MAX_INF = 4 * 1024 * 1024
TOKEN = re.compile(r'%([^%]+)%')
BINARY = re.compile(r'(?i)([\w .+()@\-\\/]+\.(?:sys|vxd))(?=[\s,"\x00]|$)')
PCI = re.compile(r'(?i)^PCI\\VEN_([0-9a-f]{4})&DEV_([0-9a-f]{4})')
DECORATION = re.compile(r'(?i)(?:^|\.)(nt(?:x86|amd64|ia64|arm64|arm)?(?:\.\d+)*)$')
CLASSES = {c.casefold(): c for c in ('Net', 'Media', 'Display', 'HDC', 'USB',
           'System', 'SCSIAdapter', 'Modem', 'NetService', 'NetTrans', 'Bluetooth',
           'SmartCardReader', 'HIDClass', 'DiskDrive', 'PCMCIA', 'Ports', 'Mouse',
           'Infrared', 'MTD', 'Biometric', 'DisplayCodec')}
# Representative PCI IDs, not all revisions/subsystems of each marketing family.
# The report retains full model IDs so a family hit is never an install grant.
BASELINE = [
    ('3Com 3C90x', {'10b7:9000', '10b7:9001', '10b7:9050', '10b7:9051',
                     '10b7:9055', '10b7:9200'}, False),
    ('Realtek RTL8139', {'10ec:8139'}, True),
    ('Realtek RTL8169', {'10ec:8169', '10ec:8167'}, False),
    ('Intel PRO/100', {'8086:1229', '8086:1209', '8086:2449', '8086:1031',
                       '8086:1038', '8086:1039'}, True),
    ('Intel PRO/1000', {'8086:1000', '8086:1001', '8086:1004', '8086:1008',
                        '8086:1009', '8086:100e', '8086:1010'}, False),
    ('VIA Rhine', {'1106:3043', '1106:3065', '1106:3106'}, False),
    ('SiS 900', {'1039:0900', '1039:7016'}, False),
    ('Broadcom 440x', {'14e4:4401', '14e4:4402', '14e4:170c'}, False),
    ('DEC/Intel 21x4x', {'1011:0002', '1011:0009', '1011:0014', '1011:0019'}, False),
    ('AMD PCnet', {'1022:2000', '1022:2001'}, False),
    ('NatSemi DP8381x', {'100b:0020'}, False),
    ('Davicom', {'1282:9100', '1282:9102', '1282:9132'}, False),
]


class PEError(ValueError):
    """Invalid, unsupported or incompletely readable image."""


def read_limited(path, limit):
    with path.open('rb') as stream:
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise ValueError(f'file exceeds {limit} byte census limit')
    return data


def parse_pe(data):
    """Parse PE32/PE32+ imports, retaining ordinals as #decimal and delay imports.

    Conservative file parser, not a Windows loader/compatibility validator.
    Zero-fill is valid image data, but is not a readable import-table backing.
    """
    def unpack(fmt, offset):
        size = struct.calcsize(fmt)
        if offset < 0 or offset + size > len(data):
            raise PEError('truncated header/table')
        return struct.unpack_from(fmt, data, offset)

    if data[:2] != b'MZ':
        raise PEError('missing MZ signature')
    pe, = unpack('<I', 0x3c)
    signature = data[pe:pe + 4]
    if signature[:2] in (b'LE', b'LX', b'NE'):
        return {'format': signature[:2].decode(), 'imports': {}, 'delay_imports': {}}
    if signature != b'PE\0\0':
        raise PEError('missing PE signature')
    machine, count, _, _, _, opt_size, characteristics = unpack('<HHIIIHH', pe + 4)
    opt = pe + 24
    magic, = unpack('<H', opt)
    if magic not in (0x10b, 0x20b):
        raise PEError('unsupported optional header magic')
    wide = magic == 0x20b
    directory_offset = 112 if wide else 96
    if opt_size < directory_offset or opt + opt_size > len(data):
        raise PEError('truncated optional header')
    image_base, = unpack('<Q' if wide else '<I', opt + (24 if wide else 28))
    image_size, header_size = unpack('<II', opt + 56)
    os_version = unpack('<HH', opt + 40)
    subsystem_version = unpack('<HH', opt + 48)
    subsystem, = unpack('<H', opt + 68)
    directories, = unpack('<I', opt + directory_offset - 4)
    if directories > 16 or directories * 8 > opt_size - directory_offset:
        raise PEError('data directories exceed optional header')
    if not image_size or header_size > min(len(data), image_size):
        raise PEError('invalid image/header size')
    if count > 96 or opt + opt_size + count * 40 > header_size:
        raise PEError('invalid section table')
    sections = []
    for i in range(count):
        at = opt + opt_size + i * 40
        virtual_size, rva, raw_size, raw = unpack('<IIII', at + 8)
        extent = max(virtual_size, raw_size)
        if raw_size and raw + raw_size > len(data):
            raise PEError('section raw data outside file')
        if extent and (rva < header_size or rva + extent > image_size or
                       rva + extent > 0x100000000):
            raise PEError('section virtual range outside image/overlaps headers')
        if extent:
            if any(rva < old_rva + old_extent and old_rva < rva + extent
                   for old_rva, old_extent, _, _ in sections):
                raise PEError('overlapping virtual sections')
            sections.append((rva, extent, raw, raw_size))

    def backing(rva, size):
        if rva < 0 or size < 0 or rva + size > min(image_size, 0x100000000):
            raise PEError('RVA range outside image')
        if rva < header_size and rva + size <= header_size:
            return rva, header_size - rva
        for base, _, raw, raw_size in sections:
            delta = rva - base
            if delta >= 0 and delta + size <= raw_size:
                return raw + delta, raw_size - delta
        raise PEError('RVA has no file backing')

    def rva_unpack(fmt, rva):
        at, _ = backing(rva, struct.calcsize(fmt))
        return unpack(fmt, at)

    def string(rva):
        at, available = backing(rva, 1)
        end = data.find(b'\0', at, at + min(available, 4096))
        if end < 0:
            raise PEError('unterminated import name')
        try:
            name = data[at:end].decode('ascii')
        except UnicodeDecodeError as exc:
            raise PEError('non-ASCII import name') from exc
        if not name or any(ord(c) < 32 or ord(c) > 126 for c in name):
            raise PEError('invalid import name')
        return name

    def directory(index):
        return unpack('<II', opt + directory_offset + index * 8) if index < directories else (0, 0)

    def imports(index, delay=False):
        base, size = directory(index)
        if not base and not size:
            return {}
        if not base or size < (32 if delay else 20) or base + size > image_size:
            raise PEError('invalid import directory range')
        result = defaultdict(set)
        step = 32 if delay else 20
        for i in range(min(size // step, 4096)):
            row = rva_unpack('<8I' if delay else '<5I', base + i * step)
            if not any(row):
                return {dll: sorted(names) for dll, names in sorted(result.items())}
            if delay:
                attrs, name, _, iat, lookup, _, _, _ = row
                if attrs & ~1:
                    raise PEError('unsupported delay import attributes')
                if not attrs & 1:
                    name -= image_base
                    lookup = lookup - image_base if lookup else 0
                    iat = iat - image_base if iat else 0
            else:
                lookup, _, _, name, iat = row
            dll = string(name).upper()
            if '\\' in dll or '/' in dll:
                raise PEError('import DLL is not a basename')
            thunk = lookup or iat
            if not thunk:
                raise PEError('missing import thunk')
            width = 8 if wide else 4
            for j in range(65536):
                value, = rva_unpack('<Q' if wide else '<I', thunk + j * width)
                if not value:
                    break
                if value & (1 << (width * 8 - 1)):
                    symbol = f'#{value & 0xffff}'
                else:
                    if delay and not attrs & 1:
                        value -= image_base
                    backing(value, 2)  # IMAGE_IMPORT_BY_NAME hint
                    symbol = string(value + 2)
                result[dll].add(symbol)
            else:
                raise PEError('unterminated/oversized import thunk table')
        raise PEError('unterminated/oversized import descriptor table')

    return {'format': 'PE32+' if wide else 'PE32', 'machine': f'0x{machine:04x}',
            'subsystem': subsystem, 'os_version': list(os_version),
            'subsystem_version': list(subsystem_version), 'image_size': image_size,
            'characteristics': characteristics, 'imports': imports(1),
            'delay_imports': imports(13, True)}


def decode_inf(data):
    if data.startswith((b'\xff\xfe', b'\xfe\xff')):
        return data.decode('utf-16', errors='replace')
    if b'\0' in data:
        return data.decode('utf-16-be' if data[::2].count(0) > data[1::2].count(0)
                           else 'utf-16-le', errors='replace')
    try:
        return data.decode('utf-8-sig')
    except UnicodeDecodeError:
        return data.decode('cp1252', errors='replace')


def fields(line):
    """INF comma fields, including empty fields, quotes and doubled quotes."""
    out, current, quoted = [], [], False
    i = 0
    while i < len(line):
        char = line[i]
        if char == '"':
            if quoted and i + 1 < len(line) and line[i + 1] == '"':
                current.append('"'); i += 1
            else:
                quoted = not quoted
        elif char == ',' and not quoted:
            out.append(''.join(current).strip()); current = []
        else:
            current.append(char)
        i += 1
    return out + [''.join(current).strip()]


def inf_sections(text):
    sections, current, pending = {}, None, ''
    for raw in text.splitlines():
        quoted, line = False, []
        for char in raw:
            if char == '"':
                quoted = not quoted
            if char == ';' and not quoted:
                break
            line.append(char)
        line = pending + ''.join(line).strip()
        if line.endswith('\\'):
            pending = line[:-1]; continue
        pending = ''
        if line.startswith('[') and line.endswith(']'):
            current = line[1:-1].strip().casefold()
            sections.setdefault(current, [])
        elif current and line:
            sections[current].append(line)
    return sections


def target(decoration):
    match = DECORATION.search(decoration)
    if not match:
        return {'os': 'Win9x' if decoration.casefold() == 'win9x' else 'unspecified',
                'architecture': 'unspecified', 'decoration': decoration}
    bits = match.group(1).lower().split('.')
    arch = bits[0][2:] or 'any'
    if len(bits) > 1:
        os_name = 'NT' + '.'.join(bits[1:3]) + '+'
    else:
        os_name = 'NT-unspecified'
    return {'os': os_name, 'architecture': arch, 'decoration': decoration}


def parse_inf(text):
    """Return binary references with model/install provenance; no INF execution.

    This is an inventory parser, not SetupAPI. Includes/Needs in other INFs are
    disclosed, not evaluated; unmatched file-list references get no model IDs.
    """
    sections = inf_sections(text)
    strings = {}
    for name in sorted(sections, key=lambda s: (s != 'strings', s)):
        if name == 'strings' or name.startswith('strings.'):
            for line in sections[name]:
                key, sep, value = line.partition('=')
                if sep:
                    strings.setdefault(key.strip().casefold(), fields(value)[0])

    def expand(value):
        for _ in range(16):
            updated = TOKEN.sub(lambda m: strings.get(m[1].casefold(), m[0]), value)
            if updated == value:
                break
            value = updated
        return value

    def values(section, key):
        return [expand(value.strip()) for line in sections.get(section, [])
                for lhs, sep, value in [line.partition('=')]
                if sep and lhs.strip().casefold() == key]

    class_names = values('version', 'class')
    cls = fields(class_names[0])[0] if class_names else 'Unknown'
    cls = CLASSES.get(cls.casefold(), cls)
    signature = ' '.join(values('version', 'signature')).casefold()
    warnings, references, models, follow_cache = set(), [], [], {}

    def names(value):
        # Match individual fields rather than letting spaces cross assignment.
        return {m[1].strip().replace('\\', '/').split('/')[-1].casefold()
                for f in fields(expand(value)) for m in BINARY.finditer(f.lstrip('@'))}

    def follow(start):
        cache_key = tuple(start)
        if cache_key in follow_cache:
            return follow_cache[cache_key]
        seen, queue, found = set(), list(start), set()
        while queue:
            section = queue.pop().casefold()
            if section in seen:
                continue
            seen.add(section)
            if section not in sections:
                warnings.add(f'missing section: {section}'); continue
            for line in sections[section]:
                lhs, sep, rhs = line.partition('=')
                key = lhs.strip().casefold()
                found.update(names(rhs if sep else line))
                if key == 'include' and sep:
                    warnings.add(f'external Include not resolved: {expand(rhs).strip()}')
                if sep and key in ('copyfiles', 'addreg', 'needs'):
                    queue.extend(f for f in fields(expand(rhs)) if f and not f.startswith('@'))
                if sep and key == 'addservice':
                    parts = fields(expand(rhs))
                    if len(parts) >= 3 and parts[2]:
                        queue.append(parts[2])
        follow_cache[cache_key] = found
        return found

    for line in sections.get('manufacturer', []):
        _, sep, rhs = line.partition('=')
        if not sep:
            continue
        parts = fields(expand(rhs))
        if not parts[0]:
            continue
        base = parts[0].casefold()
        decorations = [p for p in parts[1:] if p]
        model_sections = [(base + '.' + d.casefold(), d) for d in decorations]
        # Older combined 9x/NT INFs still have an undecorated model section.
        # Inventory both; do not pretend the undecorated row is XP-specific.
        if base in sections:
            model_sections.append((base, ''))
        if not model_sections:
            model_sections = [(base, '')]
        for section, deco in model_sections:
            if section not in sections:
                warnings.add(f'missing Models section: {section}'); continue
            for row in sections[section]:
                description, sep, rhs = row.partition('=')
                parts = fields(expand(rhs))
                if not sep or len(parts) < 2 or not parts[0]:
                    continue
                ids = sorted({p.upper() for p in parts[1:] if '\\' in p})
                if not ids:
                    continue
                install = parts[0].casefold()
                variants = [(install, deco)] if install in sections else []
                model_t = target(deco)
                # Legacy unversioned models use platform-decorated DDInstall.
                for candidate in sections:
                    suffix = candidate[len(install) + 1:]
                    if candidate.startswith(install + '.') and DECORATION.fullmatch(suffix):
                        t = target(suffix)
                        if deco and t['architecture'] != 'any' and model_t['architecture'] not in ('any', 'unspecified', t['architecture']):
                            continue
                        if model_t['os'] == 'Win9x':
                            continue
                        if model_t['os'].startswith('NT') and t['os'] != 'NT-unspecified' and model_t['os'] != 'NT-unspecified' and (
                                tuple(map(int, t['os'][2:-1].split('.'))) >
                                tuple(map(int, model_t['os'][2:-1].split('.')))):
                            continue
                        variants.append((candidate, suffix if model_t['os'] in ('unspecified', 'NT-unspecified') else deco))
                # Explicit NT models must not inherit the undecorated Win9x
                # CopyFiles branch when a compatible NT DDInstall exists.
                if len(variants) > 1 and (model_t['os'].startswith('NT') or '$windows nt$' in signature):
                    variants = [v for v in variants if v[0] != install]
                    if model_t['os'] not in ('unspecified', 'NT-unspecified'):
                        def install_rank(variant):
                            suffix = variant[0][len(install) + 1:]
                            t = target(suffix)
                            version = () if t['os'] == 'NT-unspecified' else tuple(map(int, t['os'][2:-1].split('.')))
                            return t['architecture'] != 'any', version, variant[0]
                        variants = [max(variants, key=install_rank)]
                if not variants:
                    warnings.add(f'missing DDInstall section: {install}')
                for chosen, effective_deco in variants:
                    info = target(effective_deco)
                    if info['architecture'] == 'any' and model_t['architecture'] != 'unspecified':
                        info['architecture'] = model_t['architecture']
                    if info['os'] == 'unspecified':
                        if '$windows nt$' in signature or chosen + '.services' in sections:
                            info['os'] = 'NT-unspecified'
                        elif '$chicago$' in signature:
                            # Chicago signatures also occur in NT-only INFs.
                            info['os'] = 'Win9x-or-NT'
                    record = {'class': cls, **info, 'model_section': section,
                              'manufacturer_decoration': deco,
                              'install_section': chosen, 'hardware_ids': ids,
                              'description': expand(fields(description)[0])}
                    models.append(record)
                    starts = [chosen]
                    starts.extend(chosen + suffix for suffix in ('.services', '.hw')
                                  if chosen + suffix in sections)
                    for binary in sorted(follow(starts)):
                        references.append({'binary': binary, 'evidence': 'model-install', **record})

    # Account for binaries without Manufacturer/model links (filter helpers,
    # DefaultInstall, SourceDisksFiles) without giving them all INF hardware IDs.
    linked = {r['binary'] for r in references}
    all_names = set()
    for rows in sections.values():
        for row in rows:
            lhs, sep, rhs = row.partition('=')
            all_names.update(names(lhs))
            if sep:
                all_names.update(names(rhs))
    for binary in sorted(all_names - linked):
        references.append({'binary': binary, 'class': cls, 'os': 'unknown',
                           'architecture': 'unspecified', 'decoration': '',
                           'evidence': 'file-reference', 'hardware_ids': []})
    # Many model rows share an install chain. Merge their IDs rather than
    # repeating large binary records or counting missing inbox files per model.
    grouped = {}
    for ref in references:
        key = tuple(sorted((k, v) for k, v in ref.items() if k not in ('hardware_ids', 'description')))
        if key not in grouped:
            grouped[key] = {**ref, 'hardware_ids': set(ref['hardware_ids'])}
        else:
            grouped[key]['hardware_ids'].update(ref['hardware_ids'])
    references = [{**ref, 'hardware_ids': sorted(ref['hardware_ids'])} for ref in grouped.values()]
    return {'class': cls, 'references': references, 'models': models,
            'warnings': sorted(warnings)}


def symbol_set(driver, dlls=None):
    return {f'{dll}!{symbol}' for kind in ('imports', 'delay_imports')
            for dll, symbols in driver.get(kind, {}).items()
            if dlls is None or dll in dlls for symbol in symbols}


def coverage(drivers, dlls=None):
    """Frequency-ranked prefixes, and a deterministic greedy complete-set plan.

    Coverage means ALL imports of a driver in this surface are in the subset,
    not that one popular import is present. Greedy is not a minimum-set proof.
    """
    sets = [symbol_set(d, dlls) for d in drivers]
    frequency = Counter(symbol for required in sets for symbol in required)
    ranked = sorted(frequency, key=lambda s: (-frequency[s], s))
    thresholds, chosen = {}, set()
    for i in range(len(ranked) + 1):
        if i:
            chosen.add(ranked[i - 1])
        complete = sum(required <= chosen for required in sets)
        for percent in (50, 80, 95):
            if str(percent) not in thresholds and complete >= math.ceil(len(sets) * percent / 100):
                thresholds[str(percent)] = {'symbols': i, 'drivers': complete,
                                            'required_drivers': math.ceil(len(sets) * percent / 100)}
    selected, greedy = set(), []
    for percent in (50, 80, 95):
        while sum(s <= selected for s in sets) < math.ceil(len(sets) * percent / 100):
            missing = [s - selected for s in sets if not s <= selected]
            best = min(missing, key=lambda s: (len(s), -sum(t <= selected | s for t in sets), tuple(sorted(s))))
            selected.update(best)
        greedy.append({'percent': percent, 'symbols': sorted(selected),
                       'drivers': sum(s <= selected for s in sets)})
    return {'driver_count': len(drivers), 'symbol_count': len(ranked),
            'ranked': [{'dll': s.split('!', 1)[0], 'symbol': s.split('!', 1)[1],
                        'drivers': frequency[s]} for s in ranked],
            'frequency_prefix_thresholds': thresholds, 'greedy_complete_sets': greedy}


def nt5_binding(binding):
    return binding['evidence'] == 'model-install' and binding['class'] == 'Net' and (
        binding['os'] == 'NT-unspecified' or binding['os'].startswith(('NT5.', 'NT5+')))


def census(root):
    root = root.resolve()
    files, scan_errors = [], []
    for directory, dirs, names in os.walk(root, followlinks=False, onerror=lambda e: scan_errors.append(str(e))):
        dirs[:] = sorted(d for d in dirs if not (Path(directory) / d).is_symlink())
        files.extend(Path(directory) / n for n in sorted(names)
                     if Path(n).suffix.casefold() in ('.inf', '.sys', '.vxd', '.sy_', '.vx_')
                     and not (Path(directory) / n).is_symlink()
                     and (Path(directory) / n).is_file())
    files.sort(key=lambda p: (p.relative_to(root).as_posix().casefold(), p.as_posix()))
    binaries = [p for p in files if p.suffix.casefold() in ('.sys', '.vxd')]
    compressed = [p for p in files if p.suffix.casefold() in ('.sy_', '.vx_')]
    index = defaultdict(list)
    for path in binaries:
        index[path.name.casefold()].append(path)
    bindings, inf_records, unresolved = defaultdict(list), [], []
    fingerprint = hashlib.sha256()
    for path in (p for p in files if p.suffix.casefold() == '.inf'):
        rel = path.relative_to(root).as_posix()
        try:
            data = read_limited(path, MAX_INF)
            sha = hashlib.sha256(data).hexdigest()
            parsed = parse_inf(decode_inf(data))
            fingerprint.update((rel + '\0' + sha + '\n').encode())
        except (OSError, ValueError) as exc:
            inf_records.append({'path': rel, 'error': str(exc)})
            continue
        inf_records.append({'path': rel, 'sha256': sha, 'class': parsed['class'],
                            'warnings': parsed['warnings'], 'model_count': len(parsed['models'])})
        for ref in parsed['references']:
            candidates = index.get(ref['binary'], [])
            local = [p for p in candidates if p.parent == path.parent]
            descendants = [p for p in candidates if path.parent in p.parents]
            matches = local or descendants
            # Never use an unrelated package's same basename, even if unique.
            if len(matches) != 1:
                compressed_name = ref['binary'][:-1] + '_'
                packed = [p.relative_to(root).as_posix() for p in compressed
                          if p.name.casefold() == compressed_name and path.parent in p.parents]
                unresolved.append({'inf': rel, **ref, 'reason': 'ambiguous' if matches else 'missing-local-binary',
                                   'candidates': [p.relative_to(root).as_posix() for p in matches],
                                   'compressed_candidates': packed})
                continue
            bindings[matches[0]].append({'inf': rel, **ref})
    records = []
    for path in binaries:
        rel = path.relative_to(root).as_posix()
        record = {'path': rel, 'kind': path.suffix[1:].lower(), 'bindings': bindings[path]}
        try:
            data = read_limited(path, MAX_PE)
            record['sha256'] = hashlib.sha256(data).hexdigest()
            record['file_size'] = len(data)
            fingerprint.update((rel + '\0' + record['sha256'] + '\n').encode())
            record.update(parse_pe(data))
        except (OSError, ValueError) as exc:
            record['error'] = str(exc)
        record['classes'] = sorted({b['class'] for b in bindings[path]}) or ['Unknown']
        record['os_targets'] = sorted({b['os'] for b in bindings[path]}) or ['unknown']
        records.append(record)

    candidates = [d for d in records if d['kind'] == 'sys' and not d.get('error')
                  and d.get('format') == 'PE32' and d['machine'] == '0x014c'
                  and d['subsystem'] == 1 and any(nt5_binding(b) for b in d['bindings'])]
    miniports = [d for d in candidates if 'NDIS.SYS!NdisMRegisterMiniport' in symbol_set(d)
                 and 'NDIS.SYS!NdisMRegisterMiniportDriver' not in symbol_set(d)]

    def dedup(rows):
        return list({d['sha256']: d for d in rows}.values())

    family_rows, baseline_drivers = [], {}
    for name, ids, native in BASELINE:
        hits, model_ids = [], set()
        for driver in miniports:
            matched = {hwid for b in driver['bindings'] if nt5_binding(b)
                       for hwid in b['hardware_ids'] for m in [PCI.match(hwid)]
                       if m and f'{m[1].lower()}:{m[2].lower()}' in ids}
            if matched:
                hits.append(driver['path']); model_ids.update(matched)
                baseline_drivers[driver['sha256']] = driver
        family_rows.append({'family': name, 'native_family': native,
                            'representative_ids': sorted(ids), 'drivers': hits,
                            'model_hardware_ids': sorted(model_ids),
                            'unresolved_inf_references': [r for r in unresolved if r['class'] == 'Net'
                                and any(f'{m[1].lower()}:{m[2].lower()}' in ids
                                        for hwid in r['hardware_ids'] for m in [PCI.match(hwid)] if m)]})

    def counts(rows, field):
        return dict(sorted(Counter(value for d in rows for value in d[field]).items()))

    cross = Counter(pair for d in records if d['kind'] == 'sys'
                    for pair in ({(b['class'], b['os']) for b in d['bindings']} or {('Unknown', 'unknown')}))
    unique = dedup(candidates)
    def surfaces(rows):
        return {surface: coverage(rows, dlls) for surface, dlls in (
            ('all', None), ('ndis', {'NDIS.SYS'}), ('ntoskrnl_hal', {'NTOSKRNL.EXE', 'HAL.DLL'}))}

    return {'schema_version': 1, 'input_fingerprint': fingerprint.hexdigest(),
            'method': {'unit': 'binary paths; content SHA256 dedup stated separately',
                       'walk': 'regular files only, no symlink traversal; scan errors disclosed',
                       'fingerprint': 'ordered relative paths and SHA256 of successfully read INF/SYS/VXD; compressed payload contents excluded',
                       'os': 'INF minimum target; generic NT included as candidates, no NDIS version proof',
                       'coverage': 'complete static+delay import sets, not runtime compatibility',
                       'association': 'same directory, else unique descendant; no global basename fallback'},
            'totals': {'inf': len(inf_records), 'sys': sum(d['kind'] == 'sys' for d in records),
                       'vxd': sum(d['kind'] == 'vxd' for d in records),
                       'compressed_files_not_parsed': len(compressed),
                       'binary_errors': sum('error' in d for d in records),
                       'inf_errors': sum('error' in d for d in inf_records),
                       'scan_errors': len(scan_errors),
                       'unresolved_references': len(unresolved),
                       'net_nt5_candidate_paths': len(candidates),
                       'net_nt5_candidate_unique': len(unique),
                       'miniport_candidate_paths': len(miniports),
                       'miniport_candidate_unique': len(dedup(miniports))},
            'sys_by_class': counts([d for d in records if d['kind'] == 'sys'], 'classes'),
            'sys_by_os': counts([d for d in records if d['kind'] == 'sys'], 'os_targets'),
            'vxd_by_class': counts([d for d in records if d['kind'] == 'vxd'], 'classes'),
            'sys_by_class_os': [{'class': c, 'os': o, 'drivers': n} for (c, o), n in sorted(cross.items())],
            'net_nt5': surfaces(unique), 'miniport_nt5': surfaces(dedup(miniports)),
            'baseline': family_rows, 'baseline_imports': coverage(list(baseline_drivers.values())),
            'drivers': records, 'infs': inf_records, 'unresolved_references': unresolved,
            'scan_errors': scan_errors,
            'compressed_files_not_parsed': [p.relative_to(root).as_posix() for p in compressed]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pack', type=Path)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/pe-import-census.json')
    args = parser.parse_args(argv)
    if not args.pack.is_dir():
        parser.error(f'not a driver collection directory: {args.pack}')
    root, output = args.pack.resolve(), args.output.resolve()
    if output == root or root in output.parents:
        parser.error('output must be outside the read-only input collection')
    result = census(root)
    output.parent.mkdir(parents=True, exist_ok=True)
    # Atomic replacement also avoids modifying an input through an existing
    # output hardlink. Temporary output lives beside the result, not in /tmp.
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='ascii', dir=output.parent,
                                         prefix=output.name + '.', suffix='.tmp', delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(result, stream, indent=2, sort_keys=True, ensure_ascii=True)
            stream.write('\n')
        os.replace(temporary, output)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    print(json.dumps(result['totals'], sort_keys=True))
    print(f'JSON: {output}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
