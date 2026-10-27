"""Synthetic icon, program and shortcut fixtures for the fx-exeicons tests.

Every file is generated here from the published formats (no commercial or
third-party bytes): ICO/CUR (Hornick, "Icons", MSDN 1995), PE32 .rsrc trees
(Microsoft PE format, section 6.9), NE resource tables (MS-DOS Encyclopedia,
Appendix K), RIFF ACON animated cursors, BMP, MS-SHLLINK and CiukiOS CIUKLNK3
shortcuts, Windows 95 PIFs ("WINDOWS VMM 4.0" section), desktop.ini and
AUTORUN.INF. Pixels are (a, r, g, b) tuples; `expected` gives what the
decoder must produce for an image at its own size.
"""
# SPDX-License-Identifier: MIT
import struct


def argb(p):
    a, r, g, b = p
    return 0 if not a else (a << 24 | r << 16 | g << 8 | b)


class Image:
    """w x h pixels f(x, y) -> (a, r, g, b); bpp 1/4/8 use a palette built
    from the opaque colours, 16/24 take RGB, 32 keeps alpha when `alpha`."""

    def __init__(self, w, h, f, bpp=32, alpha=False, core=False, mask_override=None):
        self.w, self.h, self.bpp, self.alpha, self.core = w, h, bpp, alpha, core
        self.px = [[f(x, y) for x in range(w)] for y in range(h)]
        self.mask_override = mask_override

    def expected(self):
        out = []
        for row in self.px:
            for a, r, g, b in row:
                if self.alpha:
                    out.append(argb((a, r, g, b)))
                else:
                    out.append(0 if a < 128 else (0xff000000 | self.quant(r, g, b)))
        return out

    def quant(self, r, g, b):
        if self.bpp == 16:
            def q(v):
                v5 = v >> 3
                return (v5 << 3) | (v5 >> 2)
            return q(r) << 16 | q(g) << 8 | q(b)
        return r << 16 | g << 8 | b

    def dib(self, icon=True):
        w, h, bpp = self.w, self.h, self.bpp
        palette = []
        if bpp <= 8:
            for row in self.px:
                for a, r, g, b in row:
                    c = (r, g, b) if a >= 128 else (0, 0, 0)
                    if c not in palette:
                        palette.append(c)
            if len(palette) > (1 << bpp):
                raise ValueError('too many colours')
            while len(palette) < (1 << bpp):
                palette.append((0, 0, 0))
        pitch = ((w * bpp + 31) // 32) * 4
        rows = []
        for y in range(h - 1, -1, -1):
            bits = bytearray(pitch)
            for x in range(w):
                a, r, g, b = self.px[y][x]
                if not self.alpha and a < 128:
                    r = g = b = 0
                if bpp <= 8:
                    i = palette.index((r, g, b))
                    if bpp == 8:
                        bits[x] = i
                    elif bpp == 4:
                        bits[x // 2] |= i << (0 if x & 1 else 4)
                    else:
                        bits[x // 8] |= i << (7 - (x & 7))
                elif bpp == 16:
                    struct.pack_into('<H', bits, x * 2, (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3))
                elif bpp == 24:
                    bits[x * 3:x * 3 + 3] = bytes((b, g, r))
                else:
                    bits[x * 4:x * 4 + 4] = bytes((b, g, r, a if self.alpha else 0))
            rows.append(bytes(bits))
        mask_pitch = ((w + 31) // 32) * 4
        masks = []
        for y in range(h - 1, -1, -1):
            m = bytearray(mask_pitch)
            for x in range(w):
                transparent = self.px[y][x][0] < 128
                if self.mask_override:
                    transparent = self.mask_override(x, y)
                if transparent:
                    m[x // 8] |= 0x80 >> (x & 7)
            masks.append(bytes(m))
        height = h * 2 if icon else h
        if self.core:
            header = struct.pack('<IHHHH', 12, w, height, 1, bpp)
            pal = b''.join(bytes((b, g, r)) for r, g, b in palette)
        else:
            header = struct.pack('<IiiHHIIiiII', 40, w, height, 1, bpp, 0, 0, 0, 0, 0, 0)
            pal = b''.join(bytes((b, g, r, 0)) for r, g, b in palette)
        return header + pal + b''.join(rows) + (b''.join(masks) if icon else b'')

    def entry(self, data, offset_or_id, cursor=False, hotspot=(0, 0), file=True):
        colors = (1 << self.bpp) if self.bpp < 8 else 0
        w = 0 if self.w >= 256 else self.w
        h = 0 if self.h >= 256 else self.h
        if cursor:
            head = struct.pack('<BBBBHH', w, h, colors, 0, *hotspot)
        else:
            head = struct.pack('<BBBBHH', w, h, colors, 0, 1, self.bpp)
        tail = struct.pack('<I', offset_or_id) if file else struct.pack('<H', offset_or_id)
        return head + struct.pack('<I', len(data)) + tail


def ico(images, cursor=False, raw=None):
    """An ICO (or CUR) file; raw: optional list of byte strings replacing image data."""
    datas = raw or [i.dib() for i in images]
    offset = 6 + 16 * len(images)
    head = struct.pack('<HHH', 0, 2 if cursor else 1, len(images))
    entries, blobs = b'', b''
    for image, data in zip(images, datas):
        entries += image.entry(data, offset + len(blobs), cursor=cursor)
        blobs += data
    return head + entries + blobs


def group(images, ids):
    """GRPICONDIR for images whose RT_ICON IDs are ids."""
    out = struct.pack('<HHH', 0, 1, len(images))
    for image, rid in zip(images, ids):
        out += image.entry(image.dib(), rid, file=False)
    return out


def _resource_tree(types):
    """types: {type_id: [(name, data), ...]} with name an int ID or a str.
    Returns the .rsrc bytes for RVA base 0x1000."""
    # Layout: root dir, type dirs, name dirs (one language each), strings, data entries, data.
    def order(entries):
        named = sorted((e for e in entries if isinstance(e[0], str)), key=lambda e: e[0])
        ids = sorted((e for e in entries if not isinstance(e[0], str)), key=lambda e: e[0])
        return named, ids
    tids = sorted(types)
    dirs = []          # (kind, payload) in emission order
    root_size = 16 + 8 * len(tids)
    offset = root_size
    type_offsets, name_offsets = {}, {}
    for t in tids:
        named, ids = order(types[t])
        type_offsets[t] = offset
        offset += 16 + 8 * (len(named) + len(ids))
    for t in tids:
        named, ids = order(types[t])
        for e in named + ids:
            name_offsets[(t, e[0])] = offset
            offset += 16 + 8
    strings_at = offset
    string_offsets = {}
    strings = b''
    for t in tids:
        for name, _ in types[t]:
            if isinstance(name, str) and name not in string_offsets:
                string_offsets[name] = strings_at + len(strings)
                encoded = name.encode('utf-16-le')
                strings += struct.pack('<H', len(name)) + encoded
    offset = strings_at + len(strings)
    offset = (offset + 3) & ~3
    data_entries_at = offset
    leaf_offsets = {}
    count = sum(len(v) for v in types.values())
    offset += 16 * count
    data_at = offset
    blobs = b''
    data_rva = {}
    k = 0
    for t in tids:
        named, ids = order(types[t])
        for e in named + ids:
            leaf_offsets[(t, e[0])] = data_entries_at + 16 * k
            k += 1
            pad = (-len(blobs)) % 8
            blobs += b'\0' * pad
            data_rva[(t, e[0])] = (0x1000 + data_at + len(blobs), len(e[1]))
            blobs += e[1]
    out = bytearray(data_at + len(blobs))
    struct.pack_into('<IIHHHH', out, 0, 0, 0, 4, 0, 0, len(tids))
    for i, t in enumerate(tids):
        struct.pack_into('<II', out, 16 + 8 * i, t, 0x80000000 | type_offsets[t])
    for t in tids:
        named, ids = order(types[t])
        at = type_offsets[t]
        struct.pack_into('<IIHHHH', out, at, 0, 0, 4, 0, len(named), len(ids))
        for i, e in enumerate(named + ids):
            name = 0x80000000 | string_offsets[e[0]] if isinstance(e[0], str) else e[0]
            struct.pack_into('<II', out, at + 16 + 8 * i, name, 0x80000000 | name_offsets[(t, e[0])])
    for (t, name), at in name_offsets.items():
        struct.pack_into('<IIHHHH', out, at, 0, 0, 4, 0, 0, 1)
        struct.pack_into('<II', out, at + 16, 0x409, leaf_offsets[(t, name)])
    out[strings_at:strings_at + len(strings)] = strings
    for key, at in leaf_offsets.items():
        rva, size = data_rva[key]
        struct.pack_into('<IIII', out, at, rva, size, 0, 0)
    out[data_at:] = blobs
    return bytes(out)


def pe(groups, plus=False, extra_types=None):
    """A minimal PE32 (PE32+ when plus) image with RT_ICON/RT_GROUP_ICON.
    groups: [(name, [Image, ...]), ...] in any order; name int or str."""
    icons, grps, next_id = [], [], 1
    for name, images in groups:
        ids = list(range(next_id, next_id + len(images)))
        next_id += len(images)
        icons += [(rid, image.dib()) for rid, image in zip(ids, images)]
        grps.append((name, group(images, ids)))
    types = {3: icons, 14: grps} if groups else {}
    if extra_types:
        types.update(extra_types)
    rsrc = _resource_tree(types) if types else b''
    lfanew = 0x80
    optsize = 240 if plus else 224
    dos = bytearray(lfanew)
    dos[0:2] = b'MZ'
    struct.pack_into('<I', dos, 60, lfanew)
    coff = struct.pack('<4sHHIIIHH', b'PE\0\0', 0x8664 if plus else 0x14c, 1, 0, 0, 0, optsize, 0x0102)
    opt = bytearray(optsize)
    struct.pack_into('<H', opt, 0, 0x20b if plus else 0x10b)
    count_at = 108 if plus else 92
    struct.pack_into('<I', opt, count_at, 16)
    if rsrc:
        struct.pack_into('<II', opt, count_at + 4 + 16, 0x1000, len(rsrc))
    section = struct.pack('<8sIIIIIIHHI', b'.rsrc', len(rsrc) or 1, 0x1000, (len(rsrc) + 511) & ~511, 0x200, 0, 0, 0, 0, 0x40000040)
    head = bytes(dos) + coff + bytes(opt) + section
    head += b'\0' * (0x200 - len(head))
    body = rsrc + b'\0' * ((-len(rsrc)) % 512)
    return head + body


def ne(groups, shift=4):
    """A minimal NE file whose resource table holds the icon groups (in the order given)."""
    lfanew = 0x40
    icons, grps, next_id = [], [], 1
    for name, images in groups:
        ids = list(range(next_id, next_id + len(images)))
        next_id += len(images)
        icons += [(rid, image.dib()) for rid, image in zip(ids, images)]
        grps.append((name, group(images, ids)))
    types = [(0x8003, icons), (0x800E, grps)]
    table_len = 2 + sum(8 + 12 * len(v) for _, v in types) + 2
    rsrctab = 0x40
    table_at = lfanew + rsrctab
    data_at = (table_at + table_len + (1 << shift) - 1) & ~((1 << shift) - 1)
    data = b''
    table = struct.pack('<H', shift)
    for tid, entries in types:
        table += struct.pack('<HHI', tid, len(entries), 0)
        for rid, blob in entries:
            pad = (-(data_at + len(data))) % (1 << shift)
            data += b'\0' * pad
            off = data_at + len(data)
            length = (len(blob) + (1 << shift) - 1) >> shift
            table += struct.pack('<HHHHHH', off >> shift, length, 0x1030, (rid | 0x8000) & 0xffff, 0, 0)
            data += blob
        data += b''
    table += struct.pack('<H', 0)
    head = bytearray(table_at)
    head[0:2] = b'MZ'
    struct.pack_into('<I', head, 60, lfanew)
    head[lfanew:lfanew + 2] = b'NE'
    struct.pack_into('<HH', head, lfanew + 0x24, rsrctab, rsrctab + len(table))
    out = bytes(head) + table
    out += b'\0' * (data_at - len(out))
    out += data
    out += b'\0' * ((-len(out)) % (1 << shift))
    return out


def ani(frames, flags=1):
    """RIFF ACON with anih (flags bit 0: frames are ICO/CUR files) and LIST fram."""
    anih = struct.pack('<9I', 36, len(frames), len(frames), 0, 0, 0, 0, 10, flags)
    chunks = b'anih' + struct.pack('<I', len(anih)) + anih
    fram = b'fram'
    for f in frames:
        fram += b'icon' + struct.pack('<I', len(f)) + f + (b'\0' if len(f) & 1 else b'')
    chunks += b'LIST' + struct.pack('<I', len(fram)) + fram
    return b'RIFF' + struct.pack('<I', 4 + len(chunks)) + b'ACON' + chunks


def bmp(image):
    dib = image.dib(icon=False)
    header_len = 40 + (4 * (1 << image.bpp) if image.bpp <= 8 else 0)
    return b'BM' + struct.pack('<IHHI', 14 + len(dib), 0, 0, 14 + header_len) + dib


def dos_exe():
    head = bytearray(64)
    head[0:2] = b'MZ'
    struct.pack_into('<HH', head, 2, 64, 1)
    return bytes(head) + b'\xcd\x20' * 32


def ciuki_link(label, target, icon='', index=0, arguments='', directory=''):
    fields = [s.encode() for s in (label, target, arguments, directory, icon)]
    head = b'CIUKLNK3' + struct.pack('<Ii', 1, index) + b''.join(struct.pack('<I', len(f)) for f in fields) + struct.pack('<I', 0)
    return head + b''.join(fields)


def ms_link(target=None, icon=None, index=0, unicode=True, relative=None, arguments=None, env_icon=None, suffix=None):
    """MS-SHLLINK: LinkInfo with LocalBasePath (target), StringData, optional IconEnvironmentDataBlock."""
    flags = 0x80 if unicode else 0
    body = b''
    if target is not None:
        flags |= 2
        base = target.encode('cp1252') + b'\0'
        tail = (suffix or '').encode('cp1252') + b'\0'
        header = 28
        volume = struct.pack('<IIII', 16, 3, 0, 16)   # VolumeID with an empty label
        volume_at = header
        base_at = volume_at + len(volume)
        suffix_at = base_at + len(base)
        size = suffix_at + len(tail)
        body += struct.pack('<IIIIIII', size, header, 1, volume_at, base_at, 0, suffix_at) + volume + base + tail
    strings = [(4, None), (8, relative), (0x10, None), (0x20, arguments), (0x40, icon)]
    for bit, value in strings:
        if value is None:
            continue
        flags |= bit
        if unicode:
            body += struct.pack('<H', len(value)) + value.encode('utf-16-le')
        else:
            body += struct.pack('<H', len(value)) + value.encode('cp1252')
    if env_icon is not None:
        flags |= 0x4000
        block = bytearray(788)
        struct.pack_into('<II', block, 0, 788, 0xA0000007)
        a = env_icon.encode('cp1252')
        block[8:8 + len(a)] = a
        w = env_icon.encode('utf-16-le')
        block[268:268 + len(w)] = w
        body += bytes(block)
    body += struct.pack('<I', 0)
    header = struct.pack('<I16sIIQQQIiIHHII', 76, bytes([1, 0x14, 2, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46]),
                         flags, 0, 0, 0, 0, 0, index, 1, 0, 0, 0, 0)
    assert len(header) == 76
    return header + body


def pif(program, icon=None, index=0, directory=''):
    """Basic section (0x171), its PIFEX heading, then WINDOWS 386 3.0 and VMM 4.0 headings."""
    basic = bytearray(0x171)
    p = program.encode('cp1252')[:62]
    basic[0x24:0x24 + len(p)] = p
    d = directory.encode('cp1252')[:63]
    basic[0x65:0x65 + len(d)] = d
    out = bytearray(basic)
    def heading(name, nxt, data, size):
        h = bytearray(22)
        h[0:len(name)] = name.encode()
        struct.pack_into('<HHH', h, 16, nxt, data, size)
        return bytes(h)
    w386 = bytearray(0x68)
    vmm = bytearray(0x1ac)
    if icon is not None:
        i = icon.encode('cp1252')
        vmm[0x58:0x58 + len(i)] = i
        struct.pack_into('<H', vmm, 0xa8, index)
    h0 = 0x171
    h1 = h0 + 22
    d1 = h1 + 22
    h2 = d1 + len(w386)
    d2 = h2 + 22
    out += heading('MICROSOFT PIFEX', h1, 0, 0x171)
    out += heading('WINDOWS 386 3.0', h2, d1, len(w386)) + w386
    out += heading('WINDOWS VMM 4.0', 0xffff, d2, len(vmm)) + vmm
    return bytes(out)


def area(src, w, h, size):
    """Reference of iconx's integer area filter (w x h straight ARGB -> size x size)."""
    out = [0] * (size * size)
    total = w * h
    for dy in range(size):
        for dx in range(size):
            sa = sr = sg = sb = 0
            ylo, yhi, xlo, xhi = dy * h, dy * h + h, dx * w, dx * w + w
            for sy in range(ylo // size, (yhi - 1) // size + 1):
                wy = min((sy + 1) * size, yhi) - max(sy * size, ylo)
                for sx in range(xlo // size, (xhi - 1) // size + 1):
                    wx = min((sx + 1) * size, xhi) - max(sx * size, xlo)
                    p = src[sy * w + sx]
                    wa = wx * wy * (p >> 24)
                    sa += wa
                    sr += wa * (p >> 16 & 255)
                    sg += wa * (p >> 8 & 255)
                    sb += wa * (p & 255)
            a = (sa + total // 2) // total
            out[dy * size + dx] = (a << 24 | ((sr + sa // 2) // sa) << 16 | ((sg + sa // 2) // sa) << 8 |
                                   (sb + sa // 2) // sa) if a and sa else 0
    return out


def fnv(pixels):
    h = 0x811c9dc5
    for p in pixels:
        for k in range(4):
            h = ((h ^ ((p >> (8 * k)) & 255)) * 0x01000193) & 0xffffffff
    return h
