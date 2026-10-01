#!/usr/bin/env python3
"""Independent read-only FAT16 + VFAT long file name parser for the gates.

It shares no code with the guest (src/lfn) or with mtools. Usage as a tool:
    scripts/fat16_lfn.py IMAGE [PATH]      list PATH (default the root)
    scripts/fat16_lfn.py IMAGE --check     check every directory's LFN entries
As a module: VFAT(path).listing(path), .entry(path), .read(path), .check().
Paths use long or short names, any case, '/' or '\\'.
"""
import struct
import sys
from pathlib import Path


def lfn_checksum(name11):
    s = 0
    for b in name11:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
    return s


def short_name(e):
    base = bytes([0xE5]) + e[1:8] if e[0] == 5 else e[:8]
    name = base.decode('cp437').rstrip()
    ext = e[8:11].decode('cp437').rstrip()
    return name + ('.' + ext if ext else '')


class Entry:
    def __init__(self, raw, long_name, slot, first):
        self.raw = raw
        self.short = short_name(raw)
        self.long = long_name
        self.name = long_name or self.short
        self.attr = raw[11]
        self.cluster = struct.unpack_from('<H', raw, 26)[0]
        self.size = struct.unpack_from('<I', raw, 28)[0]
        self.slot = slot                # index of the short entry
        self.first = first              # index of its first long name entry

    @property
    def is_dir(self):
        return bool(self.attr & 0x10)

    def __repr__(self):
        return f'Entry({self.name!r}, short={self.short!r}, attr={self.attr:#x}, size={self.size})'


class VFAT:
    def __init__(self, path, offset=None):
        self.data = Path(path).read_bytes()
        if offset is None:
            offset = (0 if self.data[54:62] == b'FAT16   '
                      else struct.unpack_from('<I', self.data, 454)[0] * 512)
        self.start = offset
        b = self.data[offset:offset + 512]
        assert struct.unpack_from('<H', b, 11)[0] == 512, 'not 512-byte sectors'
        self.spc = b[13]
        reserved, = struct.unpack_from('<H', b, 14)
        self.nfats = b[16]
        roots, = struct.unpack_from('<H', b, 17)
        self.spf, = struct.unpack_from('<H', b, 22)
        self.fat = offset + reserved * 512
        self.root = self.fat + self.nfats * self.spf * 512
        self.root_size = roots * 32
        self.clusters = self.root + self.root_size

    def next_cluster(self, c):
        return struct.unpack_from('<H', self.data, self.fat + c * 2)[0]

    def chain(self, cluster):
        seen = []
        while 2 <= cluster < 0xFFF8:
            assert cluster not in seen, 'FAT cycle'
            seen.append(cluster)
            cluster = self.next_cluster(cluster)
        return seen

    def dir_bytes(self, cluster):
        if not cluster:
            return self.data[self.root:self.root + self.root_size]
        size = self.spc * 512
        return b''.join(self.data[self.clusters + (c - 2) * size:self.clusters + (c - 1) * size]
                        for c in self.chain(cluster))

    def scan(self, cluster=0):
        """(entries, problems): live entries with their long names, and every
        long name entry that does not belong to the short entry after it."""
        raw = self.dir_bytes(cluster)
        entries, problems = [], []
        run = []                                    # (slot, entry bytes)
        def orphan(why):
            if run:
                problems.append(f'dir {cluster}: slots {run[0][0]}-{run[-1][0]} {why}')
        for slot in range(len(raw) // 32):
            e = raw[slot * 32:slot * 32 + 32]
            if e[0] == 0:
                orphan('long name entries before the end mark')
                run = []
                break
            if e[0] == 0xE5:
                orphan('long name entries before a deleted entry')
                run = []
                continue
            if e[11] == 0x0F:
                if e[0] & 0x40:
                    orphan('long name entries interrupted')
                    run = [(slot, e)]
                elif run:
                    run.append((slot, e))
                else:
                    problems.append(f'dir {cluster}: slot {slot} long name entry without a start')
                continue
            if e[11] & 8:
                orphan('long name entries before a volume label')
                run = []
                continue
            long_name, first = None, slot
            if run:
                ok = True
                count = run[0][1][0] & 0x1F
                if len(run) != count:
                    ok = False
                chars = []
                for k, (s, le) in enumerate(run):
                    if (le[0] & 0x1F) != count - k or le[13] != lfn_checksum(e[:11]):
                        ok = False
                for s, le in reversed(run):
                    for a, b in ((1, 11), (14, 26), (28, 32)):
                        chars += struct.unpack_from(f'<{(b - a) // 2}H', le, a)
                if ok:
                    text = []
                    for u in chars:
                        if u == 0:
                            break
                        text.append(chr(u))
                    long_name, first = ''.join(text), run[0][0]
                else:
                    orphan(f'long name entries that do not match {short_name(e)!r}')
                run = []
            entries.append(Entry(e, long_name, slot, first))
        return entries, problems

    def listing(self, path='/'):
        return self.scan(self.entry(path).cluster if self._parts(path) else 0)[0]

    @staticmethod
    def _parts(path):
        return [p for p in path.replace('\\', '/').split('/') if p and p != '.']

    def entry(self, path):
        cluster = 0
        found = None
        for part in self._parts(path):
            for e in self.scan(cluster)[0]:
                if part.upper() in (e.short.upper(), e.name.upper()):
                    found = e
                    break
            else:
                raise FileNotFoundError(path)
            cluster = found.cluster
        if found is None:
            raise FileNotFoundError(path)
        return found

    def read(self, path):
        e = self.entry(path)
        size = self.spc * 512
        data = b''.join(self.data[self.clusters + (c - 2) * size:self.clusters + (c - 1) * size]
                        for c in self.chain(e.cluster))
        return data[:e.size]

    def check(self):
        """Every directory reachable from the root: long name problems, and
        duplicate short or long names in one directory."""
        problems, todo, seen = [], [(0, '/')], set()
        while todo:
            cluster, path = todo.pop()
            if cluster in seen:
                continue
            seen.add(cluster)
            entries, bad = self.scan(cluster)
            problems += [f'{path}: {b}' for b in bad]
            shorts, longs = set(), set()
            for e in entries:
                if e.short in ('.', '..'):
                    continue
                if e.short.upper() in shorts:
                    problems.append(f'{path}: duplicate short name {e.short}')
                if e.name.upper() in longs:
                    problems.append(f'{path}: duplicate name {e.name}')
                shorts.add(e.short.upper())
                longs.add(e.name.upper())
                if e.is_dir and e.cluster:
                    todo.append((e.cluster, path.rstrip('/') + '/' + e.name))
        return problems


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    fs = VFAT(sys.argv[1])
    if len(sys.argv) > 2 and sys.argv[2] == '--check':
        problems = fs.check()
        for p in problems:
            print(p)
        print('VFAT OK' if not problems else f'VFAT {len(problems)} problems')
        return 1 if problems else 0
    for e in fs.listing(sys.argv[2] if len(sys.argv) > 2 else '/'):
        print(f'{e.short:12} {e.attr:02X} {e.size:10}  {e.long or ""}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
