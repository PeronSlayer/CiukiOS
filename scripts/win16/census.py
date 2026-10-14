#!/usr/bin/env python3
"""NE static import census using the production C parser; payload bytes stay local."""
# SPDX-License-Identifier: MIT
import argparse
import hashlib
import json
from pathlib import Path
import sys

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from scripts.win16.native import Parsed, NEError


def inspect(data):
    parsed = Parsed(data)
    n = parsed.image
    modules = [parsed.text(m) for m in n.modules[:n.module_count]]
    imports = []
    for index, module in enumerate(modules, 1):
        refs = []
        for r in n.relocs[:n.reloc_count]:
            kind = r.flags & 3
            if kind in (1, 2) and r.target1 == index:
                symbol = '#'+str(r.target2) if kind == 1 else None
                if kind == 2:
                    base = n.ne_offset + int.from_bytes(data[n.ne_offset+42:n.ne_offset+44], 'little')
                    size = data[base+r.target2]
                    symbol = data[base+r.target2+1:base+r.target2+1+size].decode('latin-1')
                if symbol not in refs:
                    refs.append(symbol)
        imports.append({'module_index': index, 'dll': module, 'symbols': refs, 'kind': 'static'})
    exports = []
    for index, entry in enumerate(n.entries[:n.entry_count], 1):
        if entry.kind:
            exports.append({'ordinal': index, 'names': [parsed.text(nm.text) for nm in n.names[:n.name_count]
                                                        if nm.ordinal == index],
                            'kind': ('unused', 'fixed', 'movable', 'constant')[entry.kind],
                            'segment': entry.segment, 'offset': entry.offset, 'flags': entry.flags})
    def resource_id(value):
        return value.integer if value.integer else parsed.text(value.text)
    return {'format': 'NE', 'machine': 'i386-compatible-16', 'target': n.target,
            'dll': bool(n.flags & 0x8000), 'module_flags': n.flags, 'other_flags': n.other_flags,
            'linker_version': f'{n.linker_version >> 8}.{n.linker_version & 255}',
            'expected_version': n.expected_version, 'alignment': n.alignment,
            'alignment_stored': n.alignment_stored, 'resource_alignment': n.resource_alignment,
            'autodata_bytes': n.autodata_bytes,
            'selfload': None if not n.selfload_state else
                        {'header': 'read' if n.selfload_state == 1 else 'outside-segment-1',
                         'version': n.selfload_version, 'boot_app': n.selfload_boot,
                         'load_app_seg': n.selfload_loadseg, 'exit_proc': n.selfload_exit},
            'osfixups': [{'segment': r.segment, 'source': r.source, 'source_type': r.source_type,
                          'additive': bool(r.flags & 4), 'value': r.target1 | (r.target2 << 16)}
                         for r in n.relocs[:n.reloc_count] if (r.flags & 3) == 3],
            'startup': {k: getattr(n, k) for k in ('autodata', 'heap', 'stack', 'cs', 'ip', 'ss', 'sp')},
            'load_policy': parsed.policy(), 'imports': imports, 'exports': exports,
            'names': [{'text': parsed.text(nm.text), 'ordinal': nm.ordinal, 'nonresident': bool(nm.nonresident)}
                      for nm in n.names[:n.name_count]],
            'segments': [{k: getattr(s, k) for k in ('sector', 'file_offset', 'stored_size', 'initialized_size',
                                                   'allocation', 'flags', 'reloc_count')}
                         for s in n.segments[:n.segment_count]],
            'resources': [{'type': resource_id(r.type), 'name': resource_id(r.name),
                           'file_offset': r.file_offset, 'size': r.size, 'flags': r.flags}
                          for r in n.resources[:n.resource_count]],
            'relocations': [{k: getattr(r, k) for k in ('segment', 'source', 'source_type', 'flags', 'target1', 'target2')}
                            for r in n.relocs[:n.reloc_count]],
            'dynamic': 'not-observed', 'evidence': 'static-host-only'}


def safe(value):
    return json.dumps(str(value), ensure_ascii=True)[1:-1]


PROVENANCE_FIELDS = ('workload', 'archive', 'member', 'provenance', 'extraction')


def format_text(name, digest, result=None, error=None, size=None, provenance=None):
    lines = ['FILE '+safe(name), 'SHA256 '+digest]
    if size is not None:
        lines.append('SIZE '+str(size))
    # Contract section 6.2 step 1: bind every acceptance-set image to its
    # workload, parent archive, member path, provenance class and extraction.
    for key in PROVENANCE_FIELDS:
        if provenance and provenance.get(key):
            lines.append(key.upper()+' '+safe(provenance[key]))
    if error:
        return '\n'.join(lines+['REFUSED '+safe(error), ''])+'\n'
    lines += [f"FORMAT NE i386-compatible-16 {'DLL' if result['dll'] else 'EXE'}",
              f"TARGET Windows EXPECTED {result['expected_version']:04x} LINKER {result['linker_version']}",
              f"FLAGS {result['module_flags']:04x} LOAD-POLICY {result['load_policy']}"]
    lines.append('STARTUP '+json.dumps(result['startup'], sort_keys=True))
    if result['selfload']:
        lines.append('SELFLOAD '+json.dumps(result['selfload'], sort_keys=True))
    for fix in result['osfixups']:
        lines.append('OSFIXUP '+json.dumps(fix, sort_keys=True))
    for imp in result['imports']:
        lines.append(f"STATIC [{imp['module_index']}] {safe(imp['dll'])} -> "+
                     (', '.join(safe(s) for s in imp['symbols']) or '(no referenced symbols)'))
    for export in result['exports']:
        lines.append('EXPORT '+json.dumps(export, sort_keys=True, ensure_ascii=True))
    for kind in ('segments', 'resources', 'relocations', 'names'):
        for value in result[kind]:
            lines.append(kind.upper().rstrip('S')+' '+json.dumps(value, sort_keys=True, ensure_ascii=True))
    lines.append('DYNAMIC not-observed (original engine and child traces still required)')
    lines.append('EVIDENCE static-host-only; no NE execution')
    return '\n'.join(lines+[''])+'\n'


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('images', nargs='+', type=Path)
    ap.add_argument('--json', action='store_true', help='one metadata object per image')
    ap.add_argument('--workload', help='acceptance gate or fixture id, e.g. gate-6')
    ap.add_argument('--archive', help='parent archive/media name and its full SHA-256')
    ap.add_argument('--member', help='member path inside the archive (single image only)')
    ap.add_argument('--provenance', help='redistribution class, e.g. owner-local-restricted')
    ap.add_argument('--extraction', help='tool and version used to extract the member')
    args = ap.parse_args(argv)
    if args.member and len(args.images) != 1:
        ap.error('--member names exactly one image')
    provenance = {k: getattr(args, k) for k in PROVENANCE_FIELDS if getattr(args, k)}
    failed = False
    for path in args.images:
        data = None
        row = {'file': path.name}
        row.update(provenance)
        try:
            with path.open('rb') as stream:
                # One extra byte checks growth after stat without an unbounded read.
                data = stream.read(64*1024*1024+1)
            if len(data) > 64*1024*1024:
                data = None
                raise NEError('file-size-limit')
            row.update(sha256=hashlib.sha256(data).hexdigest(), size=len(data))
            row.update(inspect(data))
        except (NEError, OSError) as e:
            failed = True
            row['error'] = str(e)
            if isinstance(e, NEError):
                row['location'] = e.location
        if args.json:
            print(json.dumps(row, sort_keys=True, ensure_ascii=True))
        else:
            print(format_text(path.name, row.get('sha256', 'unavailable'), row if 'error' not in row else None,
                              row.get('error'), row.get('size'), provenance), end='')
    return int(failed)


if __name__ == '__main__':
    raise SystemExit(main())
