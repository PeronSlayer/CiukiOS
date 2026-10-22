#!/usr/bin/env python3
"""f5-15C host inventory of the `installers` tier: exact C: deltas.

After a verified QEMU teardown the runner compares the canonical image's
system volume with the run's copy-on-write overlay (a read-only FUSE slice
through run.Host.export_readonly): every added, changed and removed file and
directory (sizes and SHA-256 of the overlay bytes), the registry delta of the
committed w32srv hive slots (`C:\\WINDOWS\\SYSTEM.CRG.<0|1>`, `USER.CRG.<0|1>`,
the CRG1 text format of apps/w32srv/storage.c) and the targets of the
CiukiOS shell links (`CIUKLNK3`, apps/desktop/shell_link.h). This evidence is
independent of the guest: the guest observer's own inventory records are
compared with it, and the calibration writes it into the workload manifest.

FAT reading follows the Microsoft FAT specification (sections 3-7: BPB,
FAT12/16/32 determination by cluster count, directory entries, long-name
entries with their short-name checksum):
https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf
Read-only; never writes an image, never mounts anything.
"""
import hashlib
import struct
import zlib

# Mirrors src/probes/win32_installers/title_controller.h title_path_class():
# files the system itself rewrites on any boot, never installer output.
EXCLUDED = ('C:\\WINDOWS\\SYSTEM.CRG.', 'C:\\WINDOWS\\USER.CRG.', 'C:\\WINDOWS\\CIUKIRST.LOG',
            'C:\\SYSTEM\\LOGS\\', 'C:\\SYSTEM\\BOOTLOG', 'C:\\PAGEFILE.SYS')
TEMP = 'C:\\WINDOWS\\TEMP\\'
HIVES = (('SYSTEM', 0, 'HKLM'), ('USER', 1, 'HKU'))
CRG_COMMITTED = 0x43524731
MAX_FILE = 512 * 1024 ** 2
FOREIGN_LINK = '<foreign>'


def path_class(path):
    upper = path.upper()
    if any(upper.startswith(p.upper()) for p in EXCLUDED):
        return 'excluded'
    if upper.startswith(TEMP):
        return 'temp'
    if upper.startswith('C:\\SYSTEM\\') or upper == 'C:\\SYSTEM':
        return 'system'
    return 'ordinary'


def is_link(path):
    upper = path.upper()
    return upper.endswith('.LNK') and (upper.startswith('C:\\WINDOWS\\START MENU\\') or upper.startswith('C:\\WINDOWS\\DESKTOP\\'))


def is_menu_link(path):
    upper = path.upper()
    return upper.endswith('.LNK') and upper.startswith('C:\\WINDOWS\\START MENU\\PROGRAMS\\')


class FatVolume:
    """A read-only FAT16/FAT32 volume in a seekable binary stream."""

    def __init__(self, stream, offset=0):
        self.stream, self.offset = stream, offset
        boot = self.read(0, 512)
        if boot[510:512] != b'\x55\xaa':
            raise ValueError('FAT boot sector signature missing')
        self.sector = struct.unpack_from('<H', boot, 11)[0]
        self.spc = boot[13]
        self.reserved = struct.unpack_from('<H', boot, 14)[0]
        self.fats = boot[16]
        root_entries = struct.unpack_from('<H', boot, 17)[0]
        total = struct.unpack_from('<H', boot, 19)[0] or struct.unpack_from('<I', boot, 32)[0]
        fat_size = struct.unpack_from('<H', boot, 22)[0] or struct.unpack_from('<I', boot, 36)[0]
        if self.sector not in (512, 1024, 2048, 4096) or not self.spc or self.spc & (self.spc - 1) or not self.fats:
            raise ValueError('invalid FAT geometry')
        self.root_sectors = (root_entries * 32 + self.sector - 1) // self.sector
        self.data = self.reserved + self.fats * fat_size + self.root_sectors
        self.clusters = (total - self.data) // self.spc
        if self.clusters < 4085:
            raise ValueError('FAT12 volumes are not inventoried')
        self.kind = 16 if self.clusters < 65525 else 32
        self.root_cluster = struct.unpack_from('<I', boot, 44)[0] if self.kind == 32 else 0
        self.cluster_bytes = self.sector * self.spc
        self.fat = self.read(self.reserved * self.sector, fat_size * self.sector)

    def read(self, at, length):
        self.stream.seek(self.offset + at)
        data = self.stream.read(length)
        if len(data) != length:
            raise ValueError('truncated FAT volume read')
        return data

    def next(self, cluster):
        if self.kind == 16:
            value = struct.unpack_from('<H', self.fat, cluster * 2)[0]
            return None if value >= 0xfff8 else value
        value = struct.unpack_from('<I', self.fat, cluster * 4)[0] & 0x0fffffff
        return None if value >= 0x0ffffff8 else value

    def chain(self, first):
        clusters, seen, cluster = [], set(), first
        while cluster is not None:
            if not 2 <= cluster < self.clusters + 2 or cluster in seen:
                raise ValueError('invalid or looping FAT chain')
            seen.add(cluster)
            clusters.append(cluster)
            cluster = self.next(cluster)
        return clusters

    def cluster_offset(self, cluster):
        return (self.data + (cluster - 2) * self.spc) * self.sector

    def directory_bytes(self, first):
        if first == 0 and self.kind == 16:
            return self.read((self.reserved + self.fats * (len(self.fat) // self.sector)) * self.sector,
                             self.root_sectors * self.sector)
        if first == 0:
            first = self.root_cluster
        return b''.join(self.read(self.cluster_offset(c), self.cluster_bytes) for c in self.chain(first))

    def entries(self, first):
        """(name, attr, first cluster, size, write stamp) of one directory."""
        data, long_parts, checksum = self.directory_bytes(first), {}, None
        for at in range(0, len(data), 32):
            e = data[at:at + 32]
            if e[0] == 0:
                break
            if e[0] == 0xe5:
                long_parts, checksum = {}, None
                continue
            attr = e[11]
            if attr == 0x0f:
                order = e[0] & 0x1f
                if e[0] & 0x40:
                    long_parts, checksum = {}, e[13]
                units = e[1:11] + e[14:26] + e[28:32]
                long_parts[order] = units
                continue
            short = e[0:11]
            total = 0
            for b in short:
                total = ((total >> 1) | ((total & 1) << 7)) + b & 0xff
            name = None
            if long_parts and checksum == total and set(long_parts) == set(range(1, len(long_parts) + 1)):
                raw = b''.join(long_parts[i] for i in sorted(long_parts))
                text = raw.decode('utf-16-le', errors='replace')
                name = text.split('\x00', 1)[0]
            long_parts, checksum = {}, None
            if attr & 0x08:
                continue
            if name is None:
                base, ext = short[:8].decode('cp437').rstrip(), short[8:].decode('cp437').rstrip()
                if base.startswith('\x05'):
                    base = '\xe5' + base[1:]
                if e[12] & 0x08:
                    base = base.lower()
                if e[12] & 0x10:
                    ext = ext.lower()
                name = base + ('.' + ext if ext else '')
            if name in ('.', '..'):
                continue
            first_cluster = struct.unpack_from('<H', e, 26)[0] | (struct.unpack_from('<H', e, 20)[0] << 16 if self.kind == 32 else 0)
            stamp = struct.unpack_from('<H', e, 24)[0] << 16 | struct.unpack_from('<H', e, 22)[0]
            yield name, attr, first_cluster, struct.unpack_from('<I', e, 28)[0], stamp

    def walk(self):
        """Every entry below the root: upper-case path -> record (C:\\ paths)."""
        result, pending, visited = {}, [('C:', 0)], set()
        while pending:
            prefix, cluster = pending.pop()
            for name, attr, first, size, stamp in self.entries(cluster):
                path = prefix + '\\' + name
                directory = bool(attr & 0x10)
                result[path.upper()] = {'path': path, 'directory': directory, 'size': 0 if directory else size,
                                        'first': first, 'stamp': stamp, 'attr': attr}
                if directory and first:
                    if first in visited:
                        raise ValueError('directory cycle at ' + path)
                    visited.add(first)
                    pending.append((path, first))
        return result

    def content(self, record):
        size = record['size']
        if size > MAX_FILE:
            raise ValueError('file exceeds the inventory bound: ' + record['path'])
        if not size:
            return b''
        out = bytearray()
        for cluster in self.chain(record['first']):
            out += self.read(self.cluster_offset(cluster), self.cluster_bytes)
            if len(out) >= size:
                break
        if len(out) < size:
            raise ValueError('FAT chain shorter than the file size: ' + record['path'])
        return bytes(out[:size])


def link_target(data):
    """Target of a CiukiOS v3 shell link, or None for another format."""
    if len(data) < 40 or data[:8] != b'CIUKLNK3' or data[8] != 1:
        return None
    lengths = struct.unpack_from('<5I', data, 16)
    at, fields = 40, []
    for n in lengths:
        if n > len(data) - at:
            return None
        fields.append(data[at:at + n].decode('utf-8', errors='replace'))
        at += n
    return fields[1] if at == len(data) else None


def _decode(text):
    out, i = bytearray(), 0
    while i < len(text):
        if text[i] == '\\':
            if text[i + 1:i + 2] != 'x':
                raise ValueError('invalid registry escape')
            out.append(int(text[i + 2:i + 4], 16))
            i += 4
        else:
            out += text[i].encode('utf-8')
            i += 1
    return out.decode('utf-8')


def parse_hive(data, hive):
    """Committed CRG1 slot -> (sequence, {KEY: (path, {VALUE: (name, type, hex)})})."""
    if len(data) < 40 or data[:4] != b'CRG1':
        return None
    version, number, low, high, length, text_crc, header_crc, committed, zero = struct.unpack_from('<IIIIIIIII', data, 4)
    if version != 1 or number != hive or committed != CRG_COMMITTED or zero or len(data) != 40 + length:
        return None
    if zlib.crc32(data[:28]) != header_crc or zlib.crc32(data[40:]) != text_crc:
        return None
    keys, current = {}, None
    for line in data[40:].decode('ascii').split('\n'):
        if not line:
            continue
        if line.startswith('['):
            path = _decode(line[1:-1])
            current = keys.setdefault(path.upper(), (path, {}))
        else:
            if current is None:
                raise ValueError('registry value before a key')
            name, rest = line.split('=', 1)
            vtype, hexdata = rest.split(':', 1)
            name = _decode(name)
            current[1][name.upper()] = (name, int(vtype, 16), hexdata)
    return (high << 32 | low), keys


def registry(volume, files):
    """Merged committed hives of a walked volume (highest valid sequence wins)."""
    merged = {}
    for stem, hive, _ in HIVES:
        best = None
        for slot in (0, 1):
            record = files.get(('C:\\WINDOWS\\%s.CRG.%d' % (stem, slot)).upper())
            if not record or record['directory']:
                continue
            parsed = parse_hive(volume.content(record), hive)
            if parsed and (best is None or parsed[0] > best[0]):
                best = parsed
        if best:
            merged.update(best[1])
    return merged


def registry_delta(before, after):
    added, removed, changed = [], [], []
    for key, (path, values) in sorted(after.items()):
        if key not in before:
            added.append({'key': path, 'values': {v[0]: '%08x:%s' % (v[1], v[2]) for v in values.values()}})
            continue
        old = before[key][1]
        for name in sorted(set(values) | set(old)):
            a, b = old.get(name), values.get(name)
            if a != b:
                changed.append({'key': path, 'value': (b or a)[0], 'before': a and '%08x:%s' % (a[1], a[2]),
                                'after': b and '%08x:%s' % (b[1], b[2])})
    for key, (path, _) in sorted(before.items()):
        if key not in after:
            removed.append({'key': path})
    return {'added_keys': added, 'removed_keys': removed, 'changed_values': changed}


def delta(base, after):
    """Exact file/registry delta between two walked FatVolumes."""
    before_files, after_files = base.walk(), after.walk()
    out = {'added': [], 'changed': [], 'removed': [], 'excluded': 0, 'temp': [], 'system_writes': 0, 'links': []}
    for key in sorted(after_files):
        record, old = after_files[key], before_files.get(key)
        if old and (record['directory'] or (old['size'], old['stamp'], old['first']) == (record['size'], record['stamp'], record['first'])):
            continue
        kind = path_class(record['path'])
        if kind == 'excluded':
            out['excluded'] += 1
            continue
        if kind == 'system':
            out['system_writes'] += 1
        row = {'path': record['path'], 'kind': 'directory' if record['directory'] else 'file'}
        if not record['directory']:
            data = after.content(record)
            row.update(size=record['size'], sha256=hashlib.sha256(data).hexdigest())
            if is_link(record['path']):
                # A link in another format (an installer's own MS-SHLLINK file) is
                # recorded with the same token the kernel observer publishes.
                out['links'].append({'path': record['path'], 'target': link_target(data) or FOREIGN_LINK})  # '' too
            if old:
                before_data = base.content(old)
                if before_data == data:
                    continue  # rewritten with identical bytes: no content delta
                row['before'] = {'size': old['size'], 'sha256': hashlib.sha256(before_data).hexdigest()}
        if kind == 'temp':
            out['temp'].append(row)
        else:
            out['changed' if old else 'added'].append(row)
    for key in sorted(before_files):
        if key not in after_files:
            kind = path_class(before_files[key]['path'])
            if kind == 'excluded':
                out['excluded'] += 1
                continue
            if kind == 'system':
                out['system_writes'] += 1
            out['removed'].append({'path': before_files[key]['path'],
                                   'kind': 'directory' if before_files[key]['directory'] else 'file'})
    out['registry'] = registry_delta(registry(base, before_files), registry(after, after_files))
    out['menu_links'] = sorted(link['path'] for link in out['links'] if is_menu_link(link['path']))
    return out


def partition(image):
    """(offset, size) of the canonical image's first MBR partition (C:)."""
    with open(image, 'rb') as stream:
        mbr = stream.read(512)
    if mbr[510:512] != b'\x55\xaa':
        raise ValueError('canonical image lacks an MBR signature')
    start, sectors = struct.unpack_from('<II', mbr, 454)
    if not start or not sectors:
        raise ValueError('canonical image has no first partition')
    return start * 512, sectors * 512


def overlay_delta(host, image, overlay, directory):
    """Host evidence of one run: canonical C: versus the overlay's C:."""
    offset, size = partition(image)
    with open(image, 'rb') as base_stream, host.export_readonly(overlay, directory, offset, size) as view:
        with open(view, 'rb') as after_stream:
            return delta(FatVolume(base_stream, offset), FatVolume(after_stream, 0))


# ---- manifest (tests/win32/workloads.json installed_files) --------------

def _fold(path):
    return str(path).replace('/', '\\').casefold()


def manifest_from_delta(observed, calibration):
    """Calibrated install manifest from an install-only run's host delta."""
    files = [{'name': r['path'], 'size': r['size'], 'sha256': r['sha256']} for r in observed['added'] if r['kind'] == 'file']
    return {'status': 'calibrated', 'entries': files,
            'directories': sorted(r['path'] for r in observed['added'] if r['kind'] == 'directory'),
            'changed': [{'name': r['path'], 'size': r['size'], 'sha256': r['sha256']} for r in observed['changed']],
            'removed': sorted(r['path'] for r in observed['removed']),
            'links': [link for link in observed['links']],
            'registry': observed['registry'],
            'calibration': calibration,
            'reason': 'calibrated by the QEMU installers tier; the lead accepts by setting status=verified'}


def residue_from_delta(observed):
    """What a full install+uninstall run leaves behind, exactly."""
    return {'files': [{'name': r['path'], 'size': r['size'], 'sha256': r['sha256']} for r in observed['added'] if r['kind'] == 'file'],
            'directories': sorted(r['path'] for r in observed['added'] if r['kind'] == 'directory'),
            'changed': [{'name': r['path'], 'size': r['size'], 'sha256': r['sha256']} for r in observed['changed']],
            'removed': sorted(r['path'] for r in observed['removed']),
            'registry': observed['registry']}


def _files(rows):
    return {_fold(r['name']): (r['size'], r.get('sha256')) for r in rows}


def compare_install(manifest, observed):
    """An install-only run's host delta against the verified manifest."""
    expected = {'files': _files(manifest['entries']), 'directories': {_fold(p) for p in manifest.get('directories', [])},
                'changed': _files(manifest.get('changed', [])), 'removed': {_fold(p) for p in manifest.get('removed', [])},
                'links': {_fold(l['path']): l['target'] for l in manifest.get('links', [])},
                'registry': manifest.get('registry')}
    actual = {'files': _files([{'name': r['path'], 'size': r['size'], 'sha256': r['sha256']} for r in observed['added'] if r['kind'] == 'file']),
              'directories': {_fold(r['path']) for r in observed['added'] if r['kind'] == 'directory'},
              'changed': _files([{'name': r['path'], 'size': r['size'], 'sha256': r['sha256']} for r in observed['changed']]),
              'removed': {_fold(r['path']) for r in observed['removed']},
              'links': {_fold(l['path']): l['target'] for l in observed['links']},
              'registry': observed['registry']}
    differences = sorted(k for k in expected if expected[k] is not None and expected[k] != actual[k])
    return {'match': not differences, 'differences': differences}


def compare_residue(residue, observed):
    expected = residue_from_delta({'added': [], 'changed': [], 'removed': [], 'registry': None}) if residue is None else residue
    actual = residue_from_delta(observed)
    keys = ('files', 'directories', 'changed', 'removed', 'registry')
    differences = []
    for key in keys:
        a, b = expected.get(key), actual.get(key)
        if key in ('files', 'changed'):
            a, b = _files(a or []), _files(b or [])
        elif key in ('directories', 'removed'):
            a, b = {_fold(p) for p in a or []}, {_fold(p) for p in b or []}
        if key == 'registry' and a is None:
            continue
        if a != b:
            differences.append(key)
    return {'match': not differences, 'differences': differences}


def compare_guest(manifest, inventory):
    """The guest observer's kernel-read inventory against the manifest: same
    files (case-insensitive paths, sizes, SHA-256) and the same link targets."""
    files = {_fold(r['path']): (r['size'], r['sha256']) for r in inventory if r['kind'] == 'file'}
    links = {_fold(r['path']): _fold(r['target']) for r in inventory if r['kind'] == 'link'}
    expected = _files(manifest['entries'])
    expected.update(_files(manifest.get('changed', [])))
    mismatched = sorted(k for k in expected if k not in files or files[k][0] != expected[k][0] or
                        (expected[k][1] and files[k][1] != expected[k][1]))
    unexpected = sorted(k for k in files if k not in expected)
    link_expected = {_fold(l['path']): _fold(l['target'] or '') for l in manifest.get('links', [])}
    return {'match': not mismatched and not unexpected and links == link_expected,
            'mismatched': mismatched, 'unexpected': unexpected, 'links_match': links == link_expected}

