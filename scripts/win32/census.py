#!/usr/bin/env python3
"""Bounded, dependency-free PE32 metadata census. Never execute an input image.

This is a host discovery parser, not the guest loader. Structural diagnostics
follow the f5-02 corpus; resolution, mapping and runtime behavior are separate.
"""
# SPDX-License-Identifier: MIT
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import xml.etree.ElementTree as ET


class PEError(ValueError):
    """Stable first-failed-check diagnostic."""


def require(condition, reason):
    if not condition:
        raise PEError(reason)


def manifest_metadata(data):
    """Only identifiers, never embedded XML or arbitrary resource payloads."""
    require(len(data) <= 1024 * 1024, 'manifest-size-limit')
    try:
        decoded = data.decode('utf-8-sig') if not data.startswith((b'\xff\xfe', b'\xfe\xff')) else data.decode('utf-16')
        require('<!DOCTYPE' not in decoded.upper() and '<!ENTITY' not in decoded.upper(),
                'manifest-entity-declaration')
        root = ET.fromstring(decoded)
    except (ET.ParseError, UnicodeError):
        raise PEError('manifest-xml') from None
    result = []
    for node in root.iter():
        tag = node.tag.rsplit('}', 1)[-1]
        if tag in ('assemblyIdentity', 'requestedExecutionLevel'):
            allowed = ('name', 'version', 'processorArchitecture', 'publicKeyToken',
                       'type', 'language', 'level', 'uiAccess')
            result.append({'kind': tag, **{k: v for k, v in node.attrib.items() if k in allowed}})
    return result


class PE:
    LIMIT = 65536

    def __init__(self, data):
        self.data = data
        self.warnings = []
        require(len(data) >= 64, 'dos-header-truncated')
        require(data[:2] == b'MZ', 'dos-magic')
        p = self.u32(60)
        if p % 4: self.warnings.append('pe-offset-alignment')
        require(p >= 64 and p + 24 <= len(data), 'pe-header-bounds')
        require(data[p:p+4] == b'PE\0\0', 'pe-signature')
        require(self.u16(p+4) == 0x14c, 'machine-not-i386')
        count = self.u16(p+6)
        require(0 < count <= 96, 'section-count')
        self.flags = self.u16(p+22)
        require(self.flags & 2, 'not-executable-image')
        self.opt = o = p + 24
        olen = self.u16(p+20)
        require(olen >= 96 and o + olen <= len(data), 'optional-header-bounds')
        require(self.u16(o) == 0x10b, 'optional-magic-not-pe32')
        nd = self.u32(o+92)
        require(nd <= 16 and olen >= 96 + nd * 8, 'directory-count')
        self.base = self.u32(o+28)
        self.entry = self.u32(o+16)
        self.sa, self.fa = self.u32(o+32), self.u32(o+36)
        pow2 = lambda n: n > 0 and n & (n-1) == 0
        require(pow2(self.fa) and ((512 <= self.fa <= 65536) or
                                  (self.sa < 4096 and self.fa == self.sa)), 'file-alignment')
        require(pow2(self.sa) and self.sa >= self.fa, 'section-alignment')
        self.size = self.u32(o+56)
        self.headers = self.u32(o+60)
        require(0 < self.size <= 512 * 1024 * 1024 and self.size % self.sa == 0, 'image-size')
        require(self.base + self.size <= 0x100000000, 'image-address-wrap')
        table = o + olen
        require(table + count * 40 <= len(data), 'section-table-bounds')
        require(table + count * 40 <= self.headers <= min(len(data), self.size), 'headers-size')
        self.subsystem = self.u16(o+68)
        require(self.subsystem in (2, 3), 'unsupported-subsystem')
        if self.u16(o+48) > 5:
            self.warnings.append('newer-subsystem-version')
        require(self.entry < self.size, 'entrypoint-bounds')
        self.sections = []
        for i in range(count):
            t = table + 40 * i
            vs, va, raw, ptr = struct.unpack_from('<4I', data, t+8)
            span = max(vs, raw)
            require(va + span <= self.size, 'section-image-bounds')
            require(va % self.sa == 0, 'section-rva-alignment')
            require(not raw or ptr % self.fa == 0, 'section-raw-alignment')
            require(ptr + raw <= 0x100000000, 'section-raw-wrap')
            require(not span or va >= self.headers, 'section-header-overlap')
            for s in self.sections:
                require(not span or not s['span'] or va >= s['rva'] + s['span'] or
                        va + span <= s['rva'], 'section-virtual-overlap')
            if raw and ptr + raw > len(data):
                self.warnings.append('raw-data-zero-filled')
            self.sections.append(dict(name=data[t:t+8].split(b'\0')[0].decode('ascii', 'replace'),
                                      rva=va, virtual_size=vs, raw_size=raw, offset=ptr,
                                      span=span, flags=self.u32(t+36)))
        self.dirs = [(0, 0)] * 16
        for i in range(nd):
            r, n = struct.unpack_from('<2I', data, o+96+i*8)
            if n:
                if i == 4:
                    require(r % 8 == 0 and r + n <= len(data), 'certificate-file-bounds')
                else:
                    require(r > 0 and r + n <= self.size, 'directory-image-bounds')
            self.dirs[i] = (r, n)

    def u16(self, p):
        require(0 <= p and p+2 <= len(self.data), 'file-read-bounds')
        return struct.unpack_from('<H', self.data, p)[0]

    def u32(self, p):
        require(0 <= p and p+4 <= len(self.data), 'file-read-bounds')
        return struct.unpack_from('<I', self.data, p)[0]

    def read(self, rva, n):
        require(0 <= n <= 1024 * 1024 and 0 <= rva and rva+n <= self.size, 'rva-bounds')
        result = bytearray()
        while n:
            if rva < self.headers:
                take = min(n, self.headers-rva)
                result.extend(self.data[rva:rva+take])
            else:
                s = next((s for s in self.sections if s['rva'] <= rva < s['rva']+s['span']), None)
                require(s is not None, 'rva-unmapped')
                delta = rva - s['rva']
                take = min(n, s['span']-delta)
                available = max(0, min(take, s['raw_size']-delta, len(self.data)-s['offset']-delta))
                result.extend(self.data[s['offset']+delta:s['offset']+delta+available])
                result.extend(bytes(take-available))
            rva += take
            n -= take
        return bytes(result)

    def check_span(self, rva, n):
        """Validate a large mapped resource without copying its payload."""
        require(0 <= n and 0 <= rva and rva+n <= self.size, 'rva-bounds')
        while n:
            if rva < self.headers:
                take = min(n,self.headers-rva)
            else:
                s = next((s for s in self.sections if s['rva'] <= rva < s['rva']+s['span']),None)
                require(s is not None,'rva-unmapped')
                take = min(n,s['rva']+s['span']-rva)
            rva += take
            n -= take

    def words(self, r, count):
        require(count <= self.LIMIT, 'table-count-limit')
        return struct.unpack('<'+'I'*count, self.read(r, count*4))

    def string(self, r):
        out = bytearray()
        for i in range(4096):
            c = self.read(r+i, 1)[0]
            if c == 0:
                return out.decode('ascii', 'replace')
            out.append(c)
        raise PEError('string-unterminated')

    def thunks(self, r, contains_va=False):
        require(r % 4 == 0, 'import-thunk-alignment')
        symbols = []
        for i in range(self.LIMIT):
            v = self.words(r+4*i, 1)[0]
            if v == 0:
                return symbols
            if v & 0x80000000:
                require(v & 0x7fff0000 == 0, 'import-ordinal-reserved-bits')
                symbols.append('#'+str(v & 65535))
            else:
                if contains_va:
                    require(self.base <= v < self.base+self.size, 'delay-thunk-va-bounds')
                    v -= self.base
                self.read(v, 2)  # hint
                symbols.append(self.string(v+2))
        raise PEError('import-thunks-unterminated')

    def imports(self, delayed=False):
        r, n = self.dirs[13 if delayed else 1]
        if not n:
            return []
        step = 32 if delayed else 20
        result = []
        for off in range(0, min(n, self.LIMIT * step)-step+1, step):
            v = self.words(r+off, step//4)
            if not any(v):
                return result
            if delayed:
                attr, name, module, iat, ilt, bound, unload, timestamp = v
                require(attr in (0, 1), 'delay-attributes')
                if not attr:
                    name, iat, ilt = (x-self.base if x else 0 for x in (name, iat, ilt))
                require(iat != 0, 'import-iat-missing')
                if ilt:
                    symbols = self.thunks(ilt, contains_va=not attr)
                else:
                    symbols = ['<delay-addresses-unresolved>']
                    self.warnings.append('delay-iat-without-names')
            else:
                ilt, timestamp, forward, name, iat = v
                require(iat != 0, 'import-iat-missing')
                if ilt and self.words(ilt, 1)[0] == 0:
                    ilt = 0
                # A bound IAT with no original table has VAs, not hint/name RVAs.
                if not ilt and timestamp and self.words(iat, 1)[0] >= self.size:
                    symbols = ['<bound-addresses-unresolved>']
                    self.warnings.append('bound-iat-without-names')
                else:
                    symbols = self.thunks(ilt or iat)
            self.read(iat, (len(symbols)+1)*4)
            result.append({'dll': self.string(name), 'symbols': symbols,
                           'kind': 'delay' if delayed else 'static', 'timestamp': timestamp})
        raise PEError('import-descriptors-unterminated')

    def bound_imports(self):
        r, n = self.dirs[11]
        result, off = [], 0
        def name_at(offset):
            require(offset < n, 'bound-name-bounds')
            raw = self.read(r+offset, min(n-offset, 4096))
            require(b'\0' in raw, 'bound-name-unterminated')
            return raw.split(b'\0')[0].decode('ascii', 'replace')
        while off+8 <= n and len(result) < self.LIMIT:
            ts, name, refs = struct.unpack('<IHH', self.read(r+off, 8))
            if ts == name == refs == 0:
                return result
            require(off+8+refs*8 <= n, 'bound-forwarder-bounds')
            forwards = []
            for j in range(refs):
                ft, fn, reserved = struct.unpack('<IHH', self.read(r+off+8+j*8, 8))
                forwards.append({'dll': name_at(fn), 'timestamp': ft})
            result.append({'dll': name_at(name), 'timestamp': ts, 'forwarders': forwards})
            off += 8+refs*8
        require(n == 0, 'bound-descriptors-unterminated')
        return result

    def exports(self):
        r, n = self.dirs[0]
        if not n:
            return []
        require(n >= 40, 'export-directory-size')
        v = self.words(r, 10)
        base, nf, nn, funcs, names, ords = v[4:]
        require(nn <= nf <= self.LIMIT, 'export-count')
        named = {}
        for i, nr in enumerate(self.words(names, nn)):
            idx = struct.unpack('<H', self.read(ords+i*2, 2))[0]
            require(idx < nf, 'export-ordinal-index')
            named.setdefault(idx, []).append(self.string(nr))
        result = []
        for i, address in enumerate(self.words(funcs, nf)):
            if not address:
                continue  # ordinal holes are not exported functions
            require(address < self.size, 'export-address-bounds')
            forward = self.string(address) if r <= address < r+n else None
            if forward:
                require('.' in forward, 'export-forwarder-format')
            result.append({'ordinal': base+i, 'names': named.get(i, []), 'forwarder': forward})
        return result

    def resources(self):
        r, n = self.dirs[2]
        if not n:
            return [], []
        leaves, manifests, active = [], [], set()
        budget = [4096]
        def relative(off, size):
            require(0 <= off and off+size <= n, 'resource-directory-bounds')
            return self.read(r+off, size)
        def walk(off, path):
            require(off not in active, 'resource-cycle')
            require(len(path) < 3, 'resource-depth')
            active.add(off)
            head = relative(off, 16)
            named, ids = struct.unpack_from('<HH', head, 12)
            entries = relative(off+16, (named+ids)*8)
            for i in range(named+ids):
                budget[0] -= 1
                require(budget[0] >= 0, 'resource-count-limit')
                name, ptr = struct.unpack_from('<II', entries, i*8)
                if name & 0x80000000:
                    loc = name & 0x7fffffff
                    length = struct.unpack('<H', relative(loc, 2))[0]
                    try:
                        key = relative(loc+2, length*2).decode('utf-16le')
                    except UnicodeError:
                        raise PEError('resource-name-encoding') from None
                else:
                    key = name
                keys = path+[key]
                if ptr & 0x80000000:
                    walk(ptr & 0x7fffffff, keys)
                else:
                    require(len(keys) == 3, 'resource-leaf-depth')
                    data, size, cp, reserved = struct.unpack('<4I', relative(ptr, 16))
                    self.check_span(data, size)
                    leaves.append({'type': keys[0], 'name': keys[1], 'language': keys[2], 'size': size})
                    if keys[0] == 24:
                        require(size <= 1024*1024, 'manifest-size-limit')
                        manifests.extend(manifest_metadata(self.read(data, size)))
            active.remove(off)
        walk(0, [])
        return leaves, manifests

    def relocations(self):
        r, n = self.dirs[5]
        types, off = set(), 0
        while off < n:
            require(off+8 <= n, 'relocation-block-size')
            page, size = self.words(r+off, 2)
            require(size >= 8 and size % 2 == 0 and off+size <= n, 'relocation-block-size')
            entries = struct.unpack('<'+'H'*((size-8)//2), self.read(r+off+8, size-8))
            i = 0
            while i < len(entries):
                e = entries[i]
                kind, delta = e >> 12, e & 4095
                require(kind in (0, 1, 2, 3, 4), 'relocation-type')
                if kind:
                    require(page+delta+(4 if kind == 3 else 2) <= self.size, 'relocation-target-bounds')
                if kind == 4:
                    require(i+1 < len(entries), 'relocation-highadj-pair')
                    i += 1
                types.add(kind)
                i += 1
            off += size
        return sorted(types)

    def tls(self):
        r, n = self.dirs[9]
        if not n:
            return None
        require(n >= 24, 'tls-directory-size')
        start, end, index, callbacks, zero, flags = self.words(r, 6)
        require(end >= start and (not start or self.base <= start <= end <= self.base+self.size),
                'tls-template-bounds')
        require(self.base <= index <= self.base+self.size-4 and index % 4 == 0, 'tls-index-bounds')
        count = 0
        if callbacks:
            require(callbacks >= self.base and callbacks % 4 == 0, 'tls-callback-alignment')
            for i in range(4096):
                value = self.words(callbacks-self.base+4*i, 1)[0]
                if not value:
                    break
                require(self.base <= value < self.base+self.size, 'tls-callback-bounds')
                count += 1
            else:
                raise PEError('tls-callbacks-unterminated')
        return {'callbacks': count, 'template_size': end-start, 'zero_fill': zero}

    def census(self):
        resources, manifests = self.resources()
        heuristics = []
        for s in self.sections:
            if s['name'].upper().startswith(('UPX', 'ASPACK', '.ASPACK')):
                heuristics.append('packer-section-name')
            if s['flags'] & 0xa0000000 == 0xa0000000:
                heuristics.append('writable-executable-section')
            raw = self.data[s['offset']:s['offset']+min(s['raw_size'], 1024*1024)]
            if len(raw) >= 256:
                freq = [raw.count(i) for i in range(256)]
                entropy = -sum((v/len(raw))*math.log2(v/len(raw)) for v in freq if v)
                if entropy > 7.5:
                    heuristics.append('high-entropy-section')
        return {'format': 'PE32', 'machine': 'i386', 'dll': bool(self.flags & 0x2000),
                'subsystem': 'gui' if self.subsystem == 2 else 'console',
                'linker_version': f'{self.data[self.opt+2]}.{self.data[self.opt+3]}',
                'imports': self.imports()+self.imports(True), 'bound_imports': self.bound_imports(),
                'exports': self.exports(), 'resources': resources, 'manifests': manifests,
                'relocation_types': self.relocations(), 'tls': self.tls(),
                'packer_heuristics': sorted(set(heuristics)), 'warnings': sorted(set(self.warnings))}


def inspect(data):
    return PE(data).census()


def safe(value):
    # Escape controls and non-ASCII so metadata cannot inject text census records.
    return json.dumps(str(value), ensure_ascii=True)[1:-1]


def format_text(name, digest, result=None, error=None):
    lines = [f'FILE {safe(name)}', f'SHA256 {digest}']
    if error:
        return '\n'.join(lines+[f'REFUSED {safe(error)}', ''])+'\n'
    lines += [f"FORMAT {result['format']} {result['machine']} {'DLL' if result['dll'] else 'EXE'}",
              f"SUBSYSTEM {result['subsystem']} LINKER {result['linker_version']}"]
    for imp in result['imports']:
        lines.append(f"{imp['kind'].upper()} {safe(imp['dll'])} -> "+', '.join(map(safe, imp['symbols'])))
    for bound in result['bound_imports']:
        lines.append(f"BOUND {safe(bound['dll'])} timestamp={bound['timestamp']} (re-resolve)")
        for fwd in bound['forwarders']:
            lines.append(f"BOUND-FORWARDER {safe(fwd['dll'])} timestamp={fwd['timestamp']}")
    for exp in result['exports']:
        lines.append(f"EXPORT #{exp['ordinal']}"+
                     (' '+','.join(map(safe, exp['names'])) if exp['names'] else '')+
                     (f" -> {safe(exp['forwarder'])}" if exp['forwarder'] else ''))
    for item in result['manifests']:
        lines.append('MANIFEST '+json.dumps(item, sort_keys=True, ensure_ascii=True))
    for item in result['resources']:
        lines.append('RESOURCE '+json.dumps(item, sort_keys=True, ensure_ascii=True))
    lines.append('RELOCATION-TYPES '+(', '.join(map(str,result['relocation_types'])) or 'none'))
    if result['tls']:
        lines.append('TLS '+json.dumps(result['tls'], sort_keys=True))
    if result['packer_heuristics']:
        lines.append('HEURISTICS '+', '.join(result['packer_heuristics'])+' (not identification)')
    lines += ['WARNING '+safe(w) for w in result['warnings']]
    lines.append('DYNAMIC not-observed (LoadLibrary/GetProcAddress/COM/codecs require runtime census)')
    return '\n'.join(lines+[''])+'\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('images', nargs='+', type=Path)
    ap.add_argument('--json', action='store_true', help='structured metadata only')
    args = ap.parse_args()
    failed = False
    for path in args.images:
        try:
            with path.open('rb') as f:
                require(path.stat().st_size <= 512*1024*1024, 'file-size-limit')
                data = f.read()
            result = inspect(data)
            sidecar = Path(str(path)+'.manifest')
            if sidecar.exists():
                require(sidecar.stat().st_size <= 1024*1024, 'manifest-size-limit')
                result['manifests'].extend(manifest_metadata(sidecar.read_bytes()))
            digest = hashlib.sha256(data).hexdigest()
            print(json.dumps({'file': path.name, 'sha256': digest, **result}, sort_keys=True)
                  if args.json else format_text(path.name, digest, result), end='\n' if args.json else '')
        except (PEError, OSError) as e:
            failed = True
            digest = hashlib.sha256(data).hexdigest() if 'data' in locals() else 'unavailable'
            print(json.dumps({'file': path.name, 'error': str(e)}, sort_keys=True) if args.json
                  else format_text(path.name, digest, error=str(e)), end='\n' if args.json else '')
        finally:
            if 'data' in locals():
                del data
    return int(failed)


if __name__ == '__main__':
    raise SystemExit(main())
