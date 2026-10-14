#!/usr/bin/env python3
"""Generate deterministic source-owned NE models; no compiler/runtime claims."""
# SPDX-License-Identifier: MIT
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

ROOT = Path(__file__).resolve().parents[2]
SOURCES = ROOT/'tests/host/win16/fixtures'


def pack_names(records):
    out = bytearray()
    for text, ordinal in records:
        raw = text.encode('latin-1')
        out += bytes([len(raw)])+raw+struct.pack('<H', ordinal)
    return out+b'\0'


def synthetic(spec):
    model = spec.get('model', {})
    header = model.get('header', {})
    h = header.get('ne_offset', 0x80)
    segs = model.get('segments', [{'flags': 0x40, 'size': 32, 'allocation': 32},
                                  {'flags': 0x41, 'size': 16, 'allocation': 64}])
    entry_model = model.get('entries', [{'kind': 'fixed', 'segment': 1, 'offset': 0, 'flags': 1}])
    entry_bytes = bytearray()
    for entry in entry_model:
        kind = entry['kind']
        seg = {'unused': 0, 'constant': 254, 'movable': 255}.get(kind, entry.get('segment', 1))
        entry_bytes += bytes([1, seg])
        if kind == 'unused':
            continue
        entry_bytes += bytes([entry.get('flags', 1)])
        if kind == 'movable':
            entry_bytes += b'\xcd\x3f'+bytes([entry.get('segment', 1)])
        entry_bytes += struct.pack('<H', entry.get('offset', 0))
    entry_bytes += b'\0'
    resident = pack_names(model.get('resident', [['FIXTURE', 0], ['Main', 1]] if entry_model else [['FIXTURE', 0]]))
    nonresident = pack_names(model.get('nonresident', [['Synthetic NE', 0]]))
    modules = model.get('modules', [])
    symbols = model.get('import_names', [])
    imported = bytearray(b'\0')
    positions = {}
    for text in modules+symbols:
        if text not in positions:
            positions[text] = len(imported)
            raw = text.encode('latin-1')
            imported += bytes([len(raw)])+raw
    resources = model.get('resources', [])
    # One TYPEINFO per model resource makes combinations explicit. Shared type
    # groups need not be contiguous in the source model.
    res = bytearray(struct.pack('<H', model.get('resource_shift', 0)))
    for r in resources:
        res += bytes(8+12)
    res += b'\0\0'
    resource_strings = {}
    for r in resources:
        for key in ('type', 'name'):
            value = r[key]
            if isinstance(value, str) and value not in resource_strings:
                resource_strings[value] = len(res)
                raw = value.encode('latin-1')
                res += bytes([len(raw)])+raw
    offsets = {'segments': h+64}
    offsets['resources'] = offsets['segments']+len(segs)*8
    offsets['resident'] = offsets['resources']+len(res)
    offsets['modules'] = offsets['resident']+len(resident)
    offsets['imports'] = offsets['modules']+len(modules)*2
    offsets['entries'] = offsets['imports']+len(imported)
    shift = header.get('alignment', 4)
    # Corrupt shifts are applied below, after generating a valid body.
    # Microsoft's NE appendix (and Open Watcom's linker) treat a stored 0 as 9.
    disk_shift = 9 if shift == 0 else shift if shift <= 16 else 4
    alignment = 1 << disk_shift
    cursor = max(0x400, (offsets['entries']+len(entry_bytes)+alignment-1)//alignment*alignment)
    data = bytearray(cursor)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 60, h)
    data[h:h+4] = b'NE\x05\x01'
    def word(at, value):
        struct.pack_into('<H', data, h+at, value)
    word(4, offsets['entries']-h); word(6, len(entry_bytes))
    flags = header.get('flags', 2 if len(segs) > 1 else 0)
    word(12, flags); word(14, header.get('autodata', 2 if len(segs) > 1 else 0))
    word(16, header.get('heap', 16)); word(18, header.get('stack', 16))
    word(20, header.get('ip', 0)); word(22, header.get('cs', 1 if segs else 0))
    word(24, header.get('sp', 32 if len(segs) > 1 else 0)); word(26, header.get('ss', 2 if len(segs) > 1 else 0))
    word(28, len(segs)); word(30, len(modules)); word(32, len(nonresident))
    for at, key in ((34, 'segments'), (36, 'resources'), (38, 'resident'), (40, 'modules'), (42, 'imports')):
        word(at, offsets[key]-h)
    word(48, sum(e['kind'] == 'movable' for e in entry_model)); word(50, shift)
    word(52, len(resources)); data[h+54] = header.get('target', 2)
    data[h+55] = header.get('other_flags', 0); word(62, header.get('expected_version', 0x030a))
    for key, blob in (('resources', res), ('resident', resident), ('imports', imported), ('entries', entry_bytes)):
        data[offsets[key]:offsets[key]+len(blob)] = blob
    for i, module in enumerate(modules):
        struct.pack_into('<H', data, offsets['modules']+i*2, positions[module])
    reloc_offsets = []
    for i, seg in enumerate(segs):
        payload = bytes.fromhex(seg['hex']) if 'hex' in seg else bytes([0x90 if not seg['flags'] & 1 else 0x42])*seg.get('size', 32)
        if 'iterated' in seg:
            payload = b''.join(struct.pack('<HH', count, len(bytes.fromhex(raw)))+bytes.fromhex(raw)
                               for count, raw in seg['iterated'])
        zero = seg.get('zero', False)
        relocs = seg.get('relocs', [])
        flags = seg['flags'] | (0x100 if relocs else 0)
        sector = 0 if zero else cursor >> disk_shift
        struct.pack_into('<HHHH', data, offsets['segments']+i*8, sector, len(payload) & 0xffff,
                         flags, seg.get('allocation', max(1, len(payload))) & 0xffff)
        if zero:
            continue
        offsets['segment'+str(i+1)] = cursor
        blob = bytearray(payload)
        for r in relocs:
            # Patch test chains/addends before serialising the relocation table.
            for site, value in r.get('sites', [[r.get('source', 0), 0 if r.get('flags', 0) & 4 else 0xffff]]):
                width = r.get('site_width', 2)
                if site+width <= len(blob):
                    blob[site:site+width] = value.to_bytes(width, 'little')
        data += blob
        if relocs:
            data += struct.pack('<H', len(relocs))
            for r in relocs:
                reloc_offsets.append(len(data))
                target2 = positions[r['name']] if 'name' in r else r.get('target2', 0)
                data += struct.pack('<BBHHH', r.get('type', 3), r.get('flags', 0),
                                    r.get('source', 0), r.get('target1', 2), target2)
        cursor = (len(data)+alignment-1)//alignment*alignment
        data += bytes(cursor-len(data))
    res_shift = model.get('resource_shift', 0)
    for i, r in enumerate(resources):
        unit = 1 << min(res_shift, 16)
        cursor = (len(data)+unit-1)//unit*unit
        data += bytes(cursor-len(data))
        blob = bytes.fromhex(r.get('hex', '4369756b69'))
        size = (len(blob)+unit-1)//unit*unit
        off = offsets['resources']+2+i*20
        def identifier(value):
            return value | 0x8000 if isinstance(value, int) else resource_strings[value]
        struct.pack_into('<HHI', data, off, identifier(r['type']), 1, 0)
        struct.pack_into('<HHHHHH', data, off+8, cursor >> min(res_shift, 16), size >> min(res_shift, 16),
                         r.get('flags', 0x30), identifier(r['name']), 0, 0)
        data += blob+bytes(size-len(blob))
    offsets['nonresident'] = len(data)
    struct.pack_into('<I', data, h+44, len(data))
    data += nonresident
    offsets['relocs'] = reloc_offsets
    # Mutations are source-authored fault injections independent of the reader.
    for mutation in spec.get('mutations', []):
        table = mutation.get('table', 'header')
        base = h if table == 'header' else offsets[table]
        if isinstance(base, list):
            base = base[mutation.get('record', 0)]
        at = base+mutation.get('offset', 0)
        if 'hex' in mutation:
            raw = bytes.fromhex(mutation['hex'])
        else:
            raw = mutation['value'].to_bytes(mutation.get('width', 2), 'little')
        data[at:at+len(raw)] = raw
    if 'truncate' in spec:
        data = data[:spec['truncate']]
    return bytes(data)


def manifests():
    specs = json.loads((SOURCES/'corpus.json').read_text())
    if len(specs) < 40 or len({s['id'] for s in specs}) != len(specs):
        raise ValueError('need at least 40 unique NE cases')
    for s in specs:
        if not re.fullmatch(r'[a-z0-9][a-z0-9-]*', s['id']) or not s.get('rules') or 'expect' not in s:
            raise ValueError('incomplete NE manifest: '+s['id'])
    return specs


def build(out):
    out = Path(out).resolve()
    if not out.is_relative_to((ROOT/'build/host/win16').resolve()):
        raise ValueError('fixture output must be under build/host/win16')
    out.mkdir(parents=True, exist_ok=True)
    hashes = {}
    specs = manifests()
    wanted = {s['id']+'.ne' for s in specs}
    for stale in out.glob('*.ne'):
        if stale.name not in wanted:
            stale.unlink()  # A renamed case must not linger as unmanifested evidence.
    for spec in specs:
        name = spec['id']+'.ne'
        data = synthetic(spec)
        hashes[name] = hashlib.sha256(data).hexdigest()
        (out/name).write_bytes(data)
    (out/'hashes.json').write_text(json.dumps(hashes, sort_keys=True, indent=2)+'\n')
    return hashes


def census_text(out):
    from scripts.win16 import census
    from scripts.win16.native import NEError
    chunks = ['# f5-19a synthetic NE census; static host evidence only.\n\n']
    for name, digest in sorted(json.loads((Path(out)/'hashes.json').read_text()).items()):
        data = (Path(out)/name).read_bytes()
        try:
            chunks.append(census.format_text(name, digest, census.inspect(data), size=len(data)))
        except NEError as e:
            chunks.append(census.format_text(name, digest, error=str(e), size=len(data)))
    return ''.join(chunks).rstrip('\n')+'\n'  # No blank line at end of file.


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, default=ROOT/'build/host/win16/current')
    ap.add_argument('--census', type=Path)
    args = ap.parse_args()
    hashes = build(args.output)
    if args.census:
        args.census.write_text(census_text(args.output))
    print(f'Built {len(hashes)} synthetic NE fixtures under {args.output}')


if __name__ == '__main__':
    main()
