# SPDX-License-Identifier: MIT
"""Builders for the WordPad import tests (G28b): OLE2 compound files, Word 6 and Word 97 documents and Write files.

The layouts follow the sources named in docs/design/wordpad.md ([MS-CFB]; Word 6.0 Binary File Format; Word 97-2007
Binary File Format (.doc) Specification; the Windows 3.1 SDK topic ".WRI Write File Format"). A real Word 97 file written
by LibreOffice (fixtures/wordpad-lo97.doc) and real Word 6 and Write 3.0 files (checked locally, never committed) were
read with the same importer, so the builders and the reader agree with files made by other producers too."""
import struct

END = 0xFFFFFFFE
FREE = 0xFFFFFFFF
FATSECT = 0xFFFFFFFD
NOSTREAM = 0xFFFFFFFF


def cfb(streams):
    """An OLE2 compound file (512-byte sectors, 64-byte mini sectors below 4096 bytes) holding `streams` (name, bytes)."""
    sec = 512
    mini = bytearray()
    mini_chain = {}
    entries = []
    regular = []
    for name, data in streams:
        if len(data) < 4096:
            start = len(mini) // 64 if data else END
            if data:
                mini += data + b"\0" * (-len(data) % 64)
            entries.append((name, start, len(data), True))
        else:
            regular.append((name, data))
            entries.append((name, None, len(data), False))
    n_dir = -(-((len(entries) + 1) * 128) // sec)
    n_minifat_entries = len(mini) // 64
    n_minifat = -(-(n_minifat_entries * 4) // sec) if n_minifat_entries else 0
    n_mini = -(-len(mini) // sec) if mini else 0
    reg_sectors = [-(-len(d) // sec) for _, d in regular]
    non_fat = n_dir + n_minifat + n_mini + sum(reg_sectors)
    n_fat = 1
    while n_fat * 128 < non_fat + n_fat:
        n_fat += 1
    fat = [FREE] * (n_fat * 128)
    pos = 0
    def chain(count):
        nonlocal pos
        start = pos
        for i in range(count):
            fat[pos + i] = pos + i + 1 if i + 1 < count else END
        pos += count
        return start
    dir_start = chain(n_dir)
    minifat_start = chain(n_minifat) if n_minifat else END
    mini_start = chain(n_mini) if n_mini else END
    reg_start = []
    for count in reg_sectors:
        reg_start.append(chain(count))
    fat_start = pos
    for i in range(n_fat):
        fat[fat_start + i] = FATSECT
    # mini FAT: every stream's chain
    minifat = [FREE] * (-(-n_minifat_entries // 128) * 128 if n_minifat_entries else 0)
    cursor = 0
    for name, start, size, in_mini in entries:
        if in_mini and size:
            count = -(-size // 64)
            for i in range(count):
                minifat[start + i] = start + i + 1 if i + 1 < count else END
    # directory
    def dentry(name, typ, left, right, child, start, size):
        e = bytearray(128)
        nm = name.encode("utf-16-le")
        e[:len(nm)] = nm
        struct.pack_into("<H", e, 64, len(nm) + 2)
        e[66] = typ
        e[67] = 1
        struct.pack_into("<III", e, 68, left, right, child)
        struct.pack_into("<I", e, 0x74, start)
        struct.pack_into("<Q", e, 0x78, size)
        return bytes(e)
    d = bytearray()
    d += dentry("Root Entry", 5, NOSTREAM, NOSTREAM, 1 if entries else NOSTREAM, mini_start, len(mini))
    ri = 0
    for i, (name, start, size, in_mini) in enumerate(entries):
        if not in_mini:
            start = reg_start[ri]; ri += 1
        right = i + 2 if i + 1 < len(entries) else NOSTREAM
        d += dentry(name, 2, NOSTREAM, right, NOSTREAM, start if size else END, size)
    d += b"\0" * (n_dir * sec - len(d))
    # the file
    out = bytearray()
    hdr = bytearray(512)
    hdr[:8] = bytes([0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1])
    struct.pack_into("<HHHHH", hdr, 0x18, 0x3E, 3, 0xFFFE, 9, 6)
    struct.pack_into("<III", hdr, 0x28, 0, n_fat, dir_start)
    struct.pack_into("<II", hdr, 0x38, 4096, minifat_start)
    struct.pack_into("<I", hdr, 0x40, n_minifat)
    struct.pack_into("<II", hdr, 0x44, END, 0)
    for i in range(109):
        struct.pack_into("<I", hdr, 0x4C + 4 * i, fat_start + i if i < n_fat else FREE)
    out += hdr
    out += d
    out += struct.pack("<%dI" % len(minifat), *minifat) + b"\0" * (n_minifat * sec - 4 * len(minifat)) if n_minifat else b""
    out += mini + b"\0" * (n_mini * sec - len(mini))
    for (name, data), count in zip(regular, reg_sectors):
        out += data + b"\0" * (count * sec - len(data))
    out += struct.pack("<%dI" % len(fat), *fat)
    return bytes(out)


# ---- sprms -------------------------------------------------------------------------------------------------------

def sp(op, arg=b""):
    """A Word 97 sprm: two-byte opcode, then the operand (variable ones get their count byte)."""
    spra = op >> 13
    if spra == 6:
        return struct.pack("<H", op) + bytes([len(arg)]) + arg
    return struct.pack("<H", op) + arg


def s1(op, arg=b""):
    """A Word 6 sprm: one-byte opcode (variable operands get their count byte)."""
    variable = {3, 12, 15, 23, 52, 68, 74, 81, 82, 103, 105, 106, 108, 120, 133}
    if op in variable:
        return bytes([op, len(arg)]) + arg
    return bytes([op]) + arg


def u16(v): return struct.pack("<H", v & 0xFFFF)
def i16(v): return struct.pack("<h", v)
def u32(v): return struct.pack("<I", v)


def chg_tabs(add, delete=()):
    """The operand of sprmPChgTabsPapx: tab stops to delete and to add (left tabs)."""
    return bytes([len(delete)]) + b"".join(i16(v) for v in delete) + bytes([len(add)]) + b"".join(i16(v) for v in add) + bytes(len(add))


class Para:
    """A paragraph: runs of (text, character grpprl), a paragraph grpprl and a style index; `mark` ends it."""
    def __init__(self, runs, papx=b"", istd=0, mark="\r"):
        self.runs = [(r, b"") if isinstance(r, str) else r for r in runs]
        self.papx = papx
        self.istd = istd
        self.mark = mark


# ---- Word 97 ----------------------------------------------------------------------------------------------------------

def _fkp_record(data, is_papx):
    """CHPX: count byte and grpprl; PAPX (istd and grpprl): count of words, then the data padded so that the whole is even."""
    if is_papx:
        if len(data) % 2 == 0:
            data += b"\0"
        return bytes([(len(data) + 1) // 2]) + data
    rec = bytes([len(data)]) + data
    return rec + b"\0" * (len(rec) % 2)


def _chunk_fkp(boundaries, entries, is_papx, w97):
    """FKP pages for the FCs boundaries[i]..boundaries[i+1] with entries[i] (CHPX/PAPX bytes or None for the defaults)."""
    bx = 13 if (is_papx and w97) else 7 if is_papx else 1
    pages = []
    i = 0
    n = len(entries)
    while i < n:
        j = i
        recs = {}
        body = 0
        while j < n:
            rec = _fkp_record(entries[j], is_papx) if entries[j] else b""
            crun = j - i + 1
            if crun > i - i and 4 * (crun + 1) + bx * crun + body + len(rec) > 510 and j > i:
                break
            body += len(rec)
            recs[j] = rec
            j += 1
        page = bytearray(512)
        crun = j - i
        for k in range(crun + 1):
            struct.pack_into("<I", page, 4 * k, boundaries[i + k])
        end = 511
        table = 4 * (crun + 1)
        for k in range(crun):
            rec = recs[i + k]
            if rec:
                end -= len(rec)
                end -= end % 2
                page[end:end + len(rec)] = rec
                page[table + bx * k] = end // 2
        page[511] = crun
        pages.append(bytes(page))
        i = j
    return pages


def _stsh(styles, w97, ftc=0):
    """A style sheet: styles is a list of dicts (sgc 1 paragraph / 2 character, base, name, papx, chpx), None for an empty slot."""
    base_len = 10 if w97 else 8
    out = bytearray()
    shi = struct.pack("<10H", len(styles), base_len, 1, 75, 15, 0, ftc, 0, 0, 0) if w97 else struct.pack("<7H", len(styles), base_len, 1, 75, 15, 0, ftc)
    out += u16(len(shi)) + shi
    for st in styles:
        if st is None:
            out += u16(0)
            continue
        sgc = st.get("sgc", 1)
        base = st.get("base", 0xFFF)
        cupx = 2 if sgc == 1 else 1
        std = bytearray()
        std += u16(st.get("sti", 0xFFE)) + u16(sgc | base << 4) + u16(cupx | 0 << 4) + u16(0)
        if w97: std += u16(0)
        name = st.get("name", "Style")
        if w97:
            std += u16(len(name)) + name.encode("utf-16-le") + u16(0)
        else:
            std += bytes([len(name)]) + name.encode("latin-1") + b"\0"
        if len(std) % 2: std += b"\0"
        upxs = []
        if sgc == 1:
            upxs.append(u16(st.get("papx_istd", 0)) + st.get("papx", b""))
            upxs.append(st.get("chpx", b""))
        else:
            upxs.append(st.get("chpx", b""))
        for upx in upxs:
            std += u16(len(upx)) + upx
            if len(upx) % 2: std += b"\0"
        out += u16(len(std)) + std
    return bytes(out)


def _plcf_bte(boundary_fcs, pages, w97):
    n = len(pages)
    out = b"".join(u32(f) for f in boundary_fcs[:n + 1])
    for pn in pages:
        out += u32(pn) if w97 else u16(pn)
    return out


def _sttbf_ffn(fonts, w97):
    if w97:
        out = u16(len(fonts)) + u16(0)
        for name, ff in fonts:
            body = bytes([ff << 4]) + u16(400) + bytes([0, 0]) + bytes(10) + bytes(24) + name.encode("utf-16-le") + u16(0)
            out += bytes([len(body)]) + body
        return out
    out = u16(0)
    for name, ff in fonts:
        body = bytes([ff << 4]) + u16(400) + bytes([0, 0]) + name.encode("latin-1") + b"\0"
        out += bytes([len(body)]) + body
    return out[:0] + u16(len(out) - 2) + out[2:]


def _lists(lists):
    """PlfLst and PlfLfo: lists is a list of (lsid, nfc, xst-characters, papx); every list has one level (fSimpleList)."""
    lst = u16(len(lists))
    lvls = b""
    for lsid, nfc, xst, papx in lists:
        lst += u32(lsid) + u32(0) + b"".join(u16(0xFFF) for _ in range(9)) + bytes([1, 0])
        lvl = bytearray(28)
        lvl[0:4] = u32(1)
        lvl[4] = nfc
        lvl[25] = len(papx)
        lvls += bytes(lvl) + papx + u16(len(xst)) + b"".join(u16(c) for c in xst)
    lfo = u32(len(lists))
    for lsid, *_ in lists:
        lfo += u32(lsid) + u32(0) + u32(0) + bytes([0, 0, 0, 0])
    return lst + lvls, lfo


def word97(paras, styles=None, fonts=None, unicode_tail=None, lists=None, data=b"", pieces=None, grpprls=(), flags=0, nfib=193):
    """A Word 97 document. paras is a list of Para; unicode_tail: text appended as a UTF-16 piece (each para string there)."""
    fonts = fonts or [("Times New Roman", 1), ("Arial", 2)]
    styles = styles if styles is not None else [{"sgc": 1, "name": "Normal", "sti": 0}]
    # the text and its runs
    text = bytearray()
    runs = []      # (fc0, fc1, chpx)
    pmarks = []    # (fc after the mark, papx-with-istd)
    fc_min = 0x800
    uni = False
    for p in paras:
        for r, chp in p.runs:
            raw = r.encode("cp1252")
            runs.append((fc_min + len(text), fc_min + len(text) + len(raw), chp))
            text += raw
        raw = p.mark.encode("cp1252")
        runs.append((fc_min + len(text), fc_min + len(text) + len(raw), b""))
        text += raw
        pmarks.append((fc_min + len(text), u16(p.istd) + p.papx))
    ccp = len(text)
    wd = bytearray(fc_min) + bytearray(text)
    wd += b"\0" * (-len(wd) % 512)
    # runs: merge into FKP boundaries
    bounds = [fc_min]
    chp_entries = []
    for a, b, chp in runs:
        if a != bounds[-1]:
            bounds.append(a); chp_entries.append(None)
        bounds.append(b); chp_entries.append(chp or None)
    chpx_pages = _chunk_fkp(bounds, chp_entries, False, True)
    pbounds = [fc_min] + [fc for fc, _ in pmarks]
    papx_pages = _chunk_fkp(pbounds, [pp if (pp[2:] or pp[:2] != b"\0\0") else None for _, pp in pmarks], True, True)
    page0 = len(wd) // 512
    for pg in chpx_pages: wd += pg
    chpx_first = page0
    papx_first = len(wd) // 512
    for pg in papx_pages: wd += pg
    # table stream
    tbl = bytearray()
    def put(data):
        off = len(tbl); tbl.extend(data); return off, len(data)
    stsh = _stsh(styles, True)
    o_stsh = put(stsh)
    o_ffn = put(_sttbf_ffn(fonts, True))
    n_chpx = len(chpx_pages)
    # per-page FC boundaries are those of the pages' first FCs
    def page_fcs(pages, last):
        fcs = [struct.unpack_from("<I", pg, 0)[0] for pg in pages]
        return fcs + [last]
    o_chpx = put(_plcf_bte(page_fcs(chpx_pages, bounds[-1]), range(chpx_first, chpx_first + n_chpx), True))
    o_papx = put(_plcf_bte(page_fcs(papx_pages, pbounds[-1]), range(papx_first, papx_first + len(papx_pages)), True))
    o_lst = o_lfo = (0, 0)
    if lists:
        lst, lfo = _lists(lists)
        o_lst = put(lst); o_lfo = put(lfo)
    # Clx: grpprls then the piece table
    clx = bytearray()
    for g in grpprls:
        clx += bytes([1]) + u16(len(g)) + g
    if pieces is None:
        pieces = [(0, ccp, fc_min | 0, True, 0)]   # cp0, cp1, fc (file offset), compressed, prm
    plc = b"".join(u32(p[0]) for p in pieces) + u32(pieces[-1][1])
    for cp0, cp1, fc, comp, prm in pieces:
        enc = (fc * 2 | 0x40000000) if comp else fc
        plc += u16(0) + u32(enc) + u16(prm)
    clx += bytes([2]) + u32(len(plc)) + plc
    o_clx = put(bytes(clx))
    # the FIB
    fib = bytearray(fc_min)
    struct.pack_into("<HHHH", fib, 0, 0xA5EC, nfib, 0, 0x409)
    struct.pack_into("<H", fib, 10, 0x200 | flags)
    struct.pack_into("<H", fib, 12, 0xBF)
    struct.pack_into("<II", fib, 0x18, fc_min, fc_min + ccp)
    struct.pack_into("<H", fib, 32, 14)
    struct.pack_into("<H", fib, 62, 22)
    struct.pack_into("<I", fib, 0x4C, ccp)
    struct.pack_into("<H", fib, 152, 93)
    for off, (fc, lcb) in ((0xA2, o_stsh), (0xFA, o_chpx), (0x102, o_papx), (0x112, o_ffn), (0x1A2, o_clx), (0x2E2, o_lst), (0x2EA, o_lfo)):
        struct.pack_into("<II", fib, off, fc, lcb)
    wd[:fc_min] = fib
    streams = [("WordDocument", bytes(wd)), ("1Table", bytes(tbl))]
    if data:
        streams.append(("Data", data))
    return cfb(streams)


def picf97(blip_record, mm=0x64, goal=(1440, 720), mx=1000, my=1000):
    """A PICF in the Data stream (68-byte header) followed by an OfficeArt inline shape container and the BLIP."""
    shape = struct.pack("<HHI", 0x000F, 0xF004, 8) + struct.pack("<HHI", 0x0001, 0xF00A, 0)   # SpContainer with an empty Sp record
    # an FBSE holding the BLIP: 36 bytes + the BLIP record
    fbse_body = bytes([6, 6]) + bytes(16) + u16(0) + u32(len(blip_record)) + u32(1) + u32(0) + bytes([0, 0, 0, 0]) + blip_record
    fbse = struct.pack("<HHI", 0x0062, 0xF007, len(fbse_body)) + fbse_body
    body = shape + fbse
    hdr = bytearray(68)
    struct.pack_into("<IHH", hdr, 0, 68 + len(body), 68, mm)
    struct.pack_into("<hh", hdr, 28, goal[0], goal[1])
    struct.pack_into("<HH", hdr, 32, mx, my)
    return bytes(hdr) + body


def blip_record(kind, payload):
    """An OfficeArt BLIP record: header, one UID, the tag byte and the payload (PNG, JPEG or DIB)."""
    typ, inst = {"png": (0xF01E, 0x6E0), "jpeg": (0xF01D, 0x46A), "dib": (0xF01F, 0x7A8)}[kind]
    body = bytes(16) + b"\xff" + payload
    return struct.pack("<HHI", inst << 4, typ, len(body)) + body


# ---- Word 6 ------------------------------------------------------------------------------------------------------------

def word6(paras, styles=None, fonts=None, nfib=101, complex_pieces=None, pictures=b"", ident=0xA5DC):
    """A Word 6 document: everything in the WordDocument stream. `pictures` is appended after the text (its offset is returned
    through paras' sprm 68 operands by the caller: pass bytes and use pic_offset())."""
    fonts = fonts or [("Times New Roman", 1), ("Arial", 2)]
    styles = styles if styles is not None else [{"sgc": 1, "name": "Normal", "sti": 0}]
    fc_min = 0x200
    text = bytearray(); runs = []; pmarks = []
    for p in paras:
        for r, chp in p.runs:
            raw = r.encode("cp1252")
            runs.append((fc_min + len(text), fc_min + len(text) + len(raw), chp)); text += raw
        raw = p.mark.encode("cp1252")
        runs.append((fc_min + len(text), fc_min + len(text) + len(raw), b"")); text += raw
        pmarks.append((fc_min + len(text), u16(p.istd) + p.papx))
    ccp = len(text)
    wd = bytearray(fc_min) + bytearray(text)
    wd += b"\0" * (-len(wd) % 512)
    bounds = [fc_min]; chp_entries = []
    for a, b, chp in runs:
        if a != bounds[-1]: bounds.append(a); chp_entries.append(None)
        bounds.append(b); chp_entries.append(chp or None)
    chpx_pages = _chunk_fkp(bounds, chp_entries, False, False)
    pbounds = [fc_min] + [fc for fc, _ in pmarks]
    papx_pages = _chunk_fkp(pbounds, [pp if (pp[2:] or pp[:2] != b"\0\0") else None for _, pp in pmarks], True, False)
    chpx_first = len(wd) // 512
    for pg in chpx_pages: wd += pg
    papx_first = len(wd) // 512
    for pg in papx_pages: wd += pg
    pic_base = len(wd)
    wd += pictures
    wd += b"\0" * (-len(wd) % 4)
    def put(data):
        off = len(wd); wd.extend(data); wd.extend(b"\0" * (-len(wd) % 4)); return off, len(data)
    o_stsh = put(_stsh(styles, False))
    o_ffn = put(_sttbf_ffn(fonts, False))
    def page_fcs(pages, last): return [struct.unpack_from("<I", pg, 0)[0] for pg in pages] + [last]
    o_chpx = put(_plcf_bte(page_fcs(chpx_pages, bounds[-1]), range(chpx_first, chpx_first + len(chpx_pages)), False))
    o_papx = put(_plcf_bte(page_fcs(papx_pages, pbounds[-1]), range(papx_first, papx_first + len(papx_pages)), False))
    o_clx = (0, 0)
    flags = 0
    if complex_pieces:
        grp, pcs = complex_pieces
        clx = bytearray()
        for g in grp: clx += bytes([1]) + u16(len(g)) + g
        plc = b"".join(u32(c[0]) for c in pcs) + u32(pcs[-1][1])
        for cp0, cp1, fc, prm in pcs: plc += u16(0) + u32(fc) + u16(prm)
        clx += bytes([2]) + u32(len(plc)) + plc
        o_clx = put(bytes(clx)); flags = 4
    fib = bytearray(fc_min)
    struct.pack_into("<HHHH", fib, 0, ident, nfib, 0, 0x409)
    struct.pack_into("<H", fib, 10, flags)
    struct.pack_into("<II", fib, 0x18, fc_min, fc_min + ccp)
    struct.pack_into("<I", fib, 0x34, ccp)
    for off, (fc, lcb) in ((0x60, o_stsh), (184, o_chpx), (192, o_papx), (208, o_ffn), (352, o_clx)):
        struct.pack_into("<II", fib, off, fc, lcb)
    wd[:fc_min] = fib
    return cfb([("WordDocument", bytes(wd))]), pic_base


def pic6(payload, mm=8, goal=(1440, 720), mx=1000, my=1000):
    """A Word 6 PIC (58-byte header) followed by a metafile."""
    hdr = bytearray(58)
    struct.pack_into("<IHH", hdr, 0, 58 + len(payload), 58, mm)
    struct.pack_into("<hh", hdr, 8, 100, 50)
    struct.pack_into("<HH", hdr, 28, goal[0] & 0xFFFF, goal[1] & 0xFFFF)
    struct.pack_into("<HH", hdr, 32, mx, my)
    return bytes(hdr) + payload


# ---- Write ------------------------------------------------------------------------------------------------------------------

def _wri_pages(fods):
    """Pages of FODs [(fcLim, fprop bytes or None)] for the character or paragraph section starting at fc 128."""
    groups = []
    cur = []
    used = 4
    for lim, prop in fods:
        size = 6 + (len(prop) + 1 if prop else 0)
        if cur and used + size > 127:
            groups.append(cur); cur = []; used = 4
        cur.append((lim, prop)); used += size
    groups.append(cur)
    out = []
    first = 128
    for items in groups:
        page = bytearray(128)
        struct.pack_into("<I", page, 0, first)
        base = 4 + 6 * len(items)
        fprops = bytearray()
        offs = []
        for lim, prop in items:
            if prop is None:
                offs.append(0xFFFF)
            else:
                offs.append(base + len(fprops) - 4)
                fprops += bytes([len(prop)]) + prop
        for i, (lim, prop) in enumerate(items):
            struct.pack_into("<IH", page, 4 + 6 * i, lim, offs[i])
        page[base:base + len(fprops)] = fprops
        assert base + len(fprops) <= 127
        page[127] = len(items)
        out.append(bytes(page))
        first = items[-1][0] if items else first
    return out


def wri(paras, fonts=("Arial", "Times New Roman"), ole=False):
    """A Write document. paras: list of (text, chp_runs, pap) where chp_runs is a list of (text, chp-prefix) and pap a PAP prefix;
    a text of bytes is a picture paragraph's raw bytes (header and data)."""
    text = bytearray()
    cfods = []
    pfods = []
    for runs, pap in paras:
        start = len(text)
        if isinstance(runs, bytes):
            text += runs
            pfods.append((128 + len(text), pap))
            cfods.append((128 + len(text), None))
            continue
        for r, chp in runs:
            raw = r.encode("cp1252")
            text += raw
            cfods.append((128 + len(text), chp))
        text += b"\r\n"
        cfods.append((128 + len(text), None))
        pfods.append((128 + len(text), pap))
    fcmac = 128 + len(text)
    body = bytes(text) + b"\0" * (-(len(text)) % 128)
    pn_char = (fcmac + 127) // 128
    cpages = _wri_pages(cfods)
    ppages = _wri_pages(pfods)
    pn_para = pn_char + len(cpages)
    pn_fntb = pn_para + len(ppages)
    fnt = bytearray(128)
    struct.pack_into("<H", fnt, 0, len(fonts))
    o = 2
    for name in fonts:
        raw = bytes([0x20]) + name.encode("latin-1") + b"\0"
        struct.pack_into("<H", fnt, o, len(raw)); o += 2
        fnt[o:o + len(raw)] = raw; o += len(raw)
    pn_mac = pn_fntb + 1
    hdr = bytearray(128)
    struct.pack_into("<HHH", hdr, 0, 0xBE32 if ole else 0xBE31, 0, 0xAB00)
    struct.pack_into("<I", hdr, 14, fcmac)
    struct.pack_into("<HHHHHH", hdr, 18, pn_para, pn_fntb, pn_fntb, pn_fntb, pn_fntb, pn_fntb)
    struct.pack_into("<H", hdr, 28, pn_fntb)
    struct.pack_into("<H", hdr, 96, pn_mac)
    return bytes(hdr) + body + b"".join(cpages) + b"".join(ppages) + bytes(fnt)


def wri_pic_bitmap(width, height, rows, bpp=1, dxa=(1440, 720)):
    """A Write bitmap picture paragraph: 40-byte header and top-down device-dependent rows padded to 16 bits."""
    wbytes = ((width * bpp + 15) // 16) * 2
    data = b"".join(r.ljust(wbytes, b"\0") for r in rows)
    hdr = bytearray(40)
    struct.pack_into("<H", hdr, 0, 0xE3)
    struct.pack_into("<HH", hdr, 10, dxa[0], dxa[1])
    struct.pack_into("<HHHHBB", hdr, 16, 0, width, height, wbytes, 1, bpp)
    struct.pack_into("<H", hdr, 30, 40)
    struct.pack_into("<I", hdr, 32, len(data))
    struct.pack_into("<HH", hdr, 36, 1000, 1000)
    return bytes(hdr) + data


def wri_pic_meta(payload, dxa=(1440, 720)):
    hdr = bytearray(40)
    struct.pack_into("<H", hdr, 0, 0x88)
    struct.pack_into("<HH", hdr, 10, dxa[0], dxa[1])
    struct.pack_into("<H", hdr, 30, 40)
    struct.pack_into("<I", hdr, 32, len(payload))
    struct.pack_into("<HH", hdr, 36, 1000, 1000)
    return bytes(hdr) + payload


def pap(jc=0, left=0, right=0, first=0, tabs=(), graphics=False, header=0):
    """A Write PAP prefix (defaults of the SDK topic: byte 0 = 61, byte 2 = 30, dyaLine = 240)."""
    p = bytearray(79)
    p[0] = 61; p[2] = 30
    struct.pack_into("<H", p, 10, 240)
    p[1] = jc & 3
    struct.pack_into("<HHh", p, 4, right, left, first)
    p[16] = (0x10 if graphics else 0) | (header << 1)
    for i, t in enumerate(tabs):
        struct.pack_into("<H", p, 22 + 4 * i, t)
    n = 22 + 4 * len(tabs) if tabs else 22
    return bytes(p[:max(n, 17)])


def chp(bold=False, italic=False, size=24, ftc=0, underline=False):
    c = bytearray(6)
    c[0] = 1
    c[1] = (1 if bold else 0) | (2 if italic else 0) | ((ftc & 0x3F) << 2)
    c[2] = size
    c[3] = 1 if underline else 0
    c[4] = (ftc >> 6) & 7
    return bytes(c)


# ---- the fixture set ---------------------------------------------------------------------------------------------------------

def png(width, height, rgb):
    import zlib
    raw = b"".join(b"\0" + bytes(rgb) * width for _ in range(height))
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")


def dib24(width, height, rgb):
    pitch = (width * 3 + 3) & ~3
    row = (bytes([rgb[2], rgb[1], rgb[0]]) * width).ljust(pitch, b"\0")
    return struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0, pitch * height, 0, 0, 0, 0) + row * height


PNG = png(16, 8, (200, 30, 30))
WMF = bytes.fromhex("010009000003") + bytes(30)
CHAR_STYLES = [
    {"sgc": 1, "name": "Normal", "sti": 0},
    {"sgc": 1, "name": "Heading 1", "sti": 1, "base": 0, "papx": sp(0x2461, b"\x01"), "chpx": sp(0x0835, b"\x01") + sp(0x4A43, u16(32))},
    {"sgc": 2, "name": "Strong", "sti": 0xFFE, "chpx": sp(0x0835, b"\x01")},
    None,
]


def generate_images(folder):
    """Pictures for the codec test: PNG (small and big), a DIB, and with Pillow a JPEG and a GIF."""
    from pathlib import Path
    out = Path(folder)
    out.mkdir(parents=True, exist_ok=True)
    (out / "small.png").write_bytes(png(8, 4, (10, 20, 30)))
    (out / "big.png").write_bytes(png(3000, 2000, (255, 0, 0)))
    (out / "tiny.dib").write_bytes(dib24(4, 2, (10, 20, 30)))
    try:
        from PIL import Image
        import io
        for name, fmt in (("small.jpg", "JPEG"), ("small.gif", "GIF")):
            buf = io.BytesIO()
            Image.new("RGB", (16, 8), (10, 200, 30)).save(buf, fmt)
            (out / name).write_bytes(buf.getvalue())
    except ImportError:
        pass


def generate(folder):
    from pathlib import Path
    out = Path(folder)
    out.mkdir(parents=True, exist_ok=True)
    fonts = [("Times New Roman", 1), ("Arial", 2), ("Courier New", 3)]
    def w(name, data):
        (out / name).write_bytes(data)
    # Word 97: character formatting, styles, paragraph formatting
    paras = [
        Para(["Title"], istd=1),
        Para(["Plain ", ("bold", sp(0x0835, b"\x01")), " ", ("ital", sp(0x0836, b"\x01")), " ", ("under", sp(0x2A3E, b"\x01")), " ",
              ("strike", sp(0x0837, b"\x01")), " ", ("red", sp(0x2A42, b"\x06")), " ", ("blue", sp(0x6870, bytes([0, 0, 255, 0]))), " ",
              ("sans", sp(0x4A4F, u16(1))), " ", ("big", sp(0x4A43, u16(40))), " ", ("strong", sp(0x4A30, u16(2))), " ", ("CAPS", sp(0x083B, b"\x01"))]),
        Para(["Justified paragraph"], papx=sp(0x2461, b"\x03") + sp(0x845E, i16(720)) + sp(0x8460, i16(-360)) + sp(0x845D, i16(360)) +
             sp(0xC60D, chg_tabs([1440, 2880]))),
        Para(["visible ", ("SECRET", sp(0x083C, b"\x01")), "text\x0bline two\ttab"]),
        Para(["last"]),
    ]
    w("w97-basic.doc", word97(paras, styles=CHAR_STYLES, fonts=fonts))
    # tables, fields
    tparas = [
        Para(["A\x07"], papx=sp(0x2416, b"\x01"), mark=""), Para(["B\x07"], papx=sp(0x2416, b"\x01"), mark=""),
        Para([""], papx=sp(0x2416, b"\x01") + sp(0x2417, b"\x01"), mark="\x07"),
        Para(["Before \x13 HYPERLINK \"http://x\" \x14shown\x15 after"]),
    ]
    w("w97-table-field.doc", word97(tparas, fonts=fonts))
    # lists: a bullet and two numbered items
    lparas = [
        Para(["bullet one"], papx=sp(0x460B, u16(1)) + sp(0x260A, b"\x00")),
        Para(["number one"], papx=sp(0x460B, u16(2)) + sp(0x260A, b"\x00")),
        Para(["number two"], papx=sp(0x460B, u16(2)) + sp(0x260A, b"\x00")),
        Para(["plain"]),
    ]
    lvl_papx = sp(0x845E, i16(720)) + sp(0x8460, i16(-360))
    w("w97-lists.doc", word97(lparas, fonts=fonts, lists=[(111, 23, [0xF0B7], lvl_papx), (222, 0, [0, ord(".")], lvl_papx)]))
    # a Unicode piece after a compressed one
    base = word97([Para(["abc"], mark="\r")], fonts=fonts)
    w("w97-unicode.doc", word97_unicode(fonts))
    # a fast-saved document: the pieces are out of order and one carries a grpprl (bold)
    w("w97-complex.doc", word97_complex(fonts))
    # a picture through the Data stream
    pic = picf97(blip_record("png", PNG))
    pp = [Para(["before ", ("\x01", sp(0x6A03, u32(0)) + sp(0x0855, b"\x01")), " after"])]
    w("w97-picture.doc", word97(pp, fonts=fonts, data=pic))
    pic = picf97(blip_record("dib", dib24(4, 2, (10, 20, 30))))
    w("w97-dib.doc", word97(pp, fonts=fonts, data=pic))
    # damaged structures that name themselves: a character style based on itself through sprmCIstd, a list level that sets its own list
    loop_styles = [{"sgc": 1, "name": "Normal", "sti": 0}, {"sgc": 1, "name": "x", "sti": 1}, {"sgc": 2, "name": "Loop", "sti": 0xFFE, "chpx": sp(0x4A30, u16(2)) + sp(0x0835, b"\x01")}]
    loop = [Para([("looped", sp(0x4A30, u16(2)))]), Para(["item"], papx=sp(0x460B, u16(1)) + sp(0x260A, b"\x00"))]
    w("w97-loops.doc", word97(loop, styles=loop_styles, fonts=fonts, lists=[(111, 23, [0xF0B7], sp(0x460B, u16(1)) + sp(0x845E, i16(720)))]))
    w("w97-encrypted.doc", word97([Para(["secret"])], flags=0x100))
    w("w97-old.doc", word97([Para(["old"])], nfib=100))
    # Word 6
    w6 = [
        Para(["Title"], istd=1),
        Para(["Plain ", ("bold", s1(85, b"\x01")), " ", ("ital", s1(86, b"\x01")), " ", ("under", s1(94, b"\x01")), " ", ("red", s1(98, b"\x06")), " ",
              ("sans", s1(93, u16(1))), " ", ("big", s1(99, bytes([40])))]),
        Para(["Hanging paragraph"], papx=s1(5, b"\x02") + s1(17, i16(720)) + s1(19, i16(-360)) + s1(16, i16(360)) + s1(15, chg_tabs([1440]))),
        Para(["bullet"], papx=s1(12, bytes([23]) + bytes(83))),
        Para(["last"]),
    ]
    st6 = [{"sgc": 1, "name": "Normal", "sti": 0},
           {"sgc": 1, "name": "Heading 1", "sti": 1, "base": 0, "papx": s1(5, b"\x01"), "chpx": s1(85, b"\x01") + s1(99, bytes([32]))}, None]
    d6, _ = word6(w6, styles=st6, fonts=fonts)
    w("w6-basic.doc", d6)
    pic = pic6(WMF)
    d6, base = word6([Para(["pic ", ("\x01", s1(68, u32(0)) + s1(117, b"\x01")), " end"])], fonts=fonts, pictures=pic)
    # the picture offset is only known after layout: rebuild with it
    d6, base2 = word6([Para(["pic ", ("\x01", s1(68, u32(base)) + s1(117, b"\x01")), " end"])], fonts=fonts, pictures=pic)
    assert base2 == base
    w("w6-picture.doc", d6)
    # a Word 6 fast-saved document: two pieces in reverse order
    w("w6-complex.doc", word6_complex(fonts))
    # Write
    wp = [
        ([("Title", chp(bold=True, size=36))], pap(jc=1)),
        ([("Plain ", chp()), ("bold", chp(bold=True)), (" ", chp()), ("italic", chp(italic=True)), (" ", chp()), ("sans", chp(ftc=1, size=20)), (" ", chp()), ("under", chp(underline=True))], pap()),
        ([("Indented\ttab", chp())], pap(jc=3, left=720, right=360, first=-360, tabs=(1440, 2880))),
        ([("last", chp())], pap()),
    ]
    w("write-basic.wri", wri(wp, fonts=("Times New Roman", "Arial")))
    bm = wri_pic_bitmap(10, 2, [bytes([0xFF, 0xC0]), bytes([0x00, 0x00])], 1)
    wp2 = [([("before", chp())], pap()), (bm, pap(graphics=True)), (wri_pic_meta(WMF), pap(graphics=True)), ([("after", chp())], pap())]
    w("write-picture.wri", wri(wp2))
    wp3 = [([("header text", chp())], pap(header=1)), ([("body", chp())], pap())]
    w("write-header.wri", wri(wp3))


def word97_unicode(fonts):
    """The first paragraph as compressed text and the second as UTF-16 (Euro sign and a surrogate pair)."""
    fc_min = 0x800
    first = "abc\r".encode("cp1252")
    second = "café € \U0001F600\r".encode("utf-16-le")
    return word97_pieces(fonts, [(first, True), (second, False)], fc_min)


def word97_pieces(fonts, chunks, fc_min):
    """A document whose pieces are `chunks` (raw bytes, compressed?) stored one after another in order."""
    text = bytearray(); pieces = []; cp = 0
    marks = []
    for data, comp in chunks:
        n = len(data) // (1 if comp else 2)
        pieces.append((cp, cp + n, fc_min + len(text), comp, 0))
        text += data; cp += n
        marks.append(n)
    # one paragraph per piece, the mark is the last character of each piece
    paras = []
    return _word97_raw(fonts, bytes(text), pieces, [(fc_min + len(text),)], fc_min, cp, chunks)


def _word97_raw(fonts, text, pieces, _unused, fc_min, ccp, chunks):
    """Builds the documents of the piece-table tests: FKP boundaries are placed after every paragraph mark."""
    wd = bytearray(fc_min) + bytearray(text)
    wd += b"\0" * (-len(wd) % 512)
    # paragraph marks: the 0x0D characters; character runs: the whole text with default properties
    marks = []
    for cp0, cp1, fc, comp, prm in pieces:
        unit = 1 if comp else 2
        for k in range(cp1 - cp0):
            ch = text[fc - fc_min + k * unit] if comp else text[fc - fc_min + k * 2]
            if ch == 0x0D and (comp or text[fc - fc_min + k * 2 + 1] == 0):
                marks.append(fc + (k + 1) * unit)
    fcs = sorted(set(marks))
    pbounds = [min(p[2] for p in pieces)] + fcs
    papx_pages = _chunk_fkp(pbounds, [None] * len(fcs), True, True)
    cbounds = [pbounds[0], pbounds[-1]]
    chpx_pages = _chunk_fkp(cbounds, [None], False, True)
    chpx_first = len(wd) // 512
    for pg in chpx_pages: wd += pg
    papx_first = len(wd) // 512
    for pg in papx_pages: wd += pg
    tbl = bytearray()
    def put(d):
        o = len(tbl); tbl.extend(d); return o, len(d)
    o_stsh = put(_stsh([{"sgc": 1, "name": "Normal", "sti": 0}], True))
    o_ffn = put(_sttbf_ffn(fonts, True))
    o_chpx = put(_plcf_bte(cbounds, [chpx_first], True))
    o_papx = put(_plcf_bte(pbounds[:1] + [pbounds[-1]] if len(papx_pages) == 1 else [struct.unpack_from("<I", pg, 0)[0] for pg in papx_pages] + [pbounds[-1]],
                           range(papx_first, papx_first + len(papx_pages)), True))
    plc = b"".join(u32(p[0]) for p in pieces) + u32(pieces[-1][1])
    for cp0, cp1, fc, comp, prm in pieces:
        plc += u16(0) + u32((fc * 2 | 0x40000000) if comp else fc) + u16(prm)
    clx = bytes([2]) + u32(len(plc)) + plc
    o_clx = put(clx)
    fib = bytearray(fc_min)
    struct.pack_into("<HHHH", fib, 0, 0xA5EC, 193, 0, 0x409)
    struct.pack_into("<H", fib, 10, 0x200)
    struct.pack_into("<II", fib, 0x18, fc_min, fc_min + len(text))
    struct.pack_into("<H", fib, 32, 14); struct.pack_into("<H", fib, 62, 22)
    struct.pack_into("<I", fib, 0x4C, ccp)
    struct.pack_into("<H", fib, 152, 93)
    for off, (fc, lcb) in ((0xA2, o_stsh), (0xFA, o_chpx), (0x102, o_papx), (0x112, o_ffn), (0x1A2, o_clx)):
        struct.pack_into("<II", fib, off, fc, lcb)
    wd[:fc_min] = fib
    return cfb([("WordDocument", bytes(wd)), ("1Table", bytes(tbl))])


def word97_complex(fonts):
    """Fast-saved: cp 0..6 is stored after cp 6..12, and the first piece carries a grpprl that makes it bold."""
    fc_min = 0x800
    second = b"World\r"       # stored first
    first = b"Hello \r"       # stored second; cp 0..7
    text = second + first
    pieces = [(0, 7, fc_min + len(second), True, (0 << 1) | 1), (7, 13, fc_min, True, 0)]
    return _word97_complex_raw(fonts, text, pieces, [sp(0x0835, b"\x01")], fc_min)


def _word97_complex_raw(fonts, text, pieces, grpprls, fc_min):
    wd = bytearray(fc_min) + bytearray(text)
    wd += b"\0" * (-len(wd) % 512)
    lo, hi = fc_min, fc_min + len(text)
    # paragraph marks at the end of each stored chunk
    marks = [fc_min + len(b"World\r"), hi]
    papx_pages = _chunk_fkp([lo] + marks, [None, None], True, True)
    chpx_pages = _chunk_fkp([lo, hi], [None], False, True)
    chpx_first = len(wd) // 512
    for pg in chpx_pages: wd += pg
    papx_first = len(wd) // 512
    for pg in papx_pages: wd += pg
    tbl = bytearray()
    def put(d):
        o = len(tbl); tbl.extend(d); return o, len(d)
    o_stsh = put(_stsh([{"sgc": 1, "name": "Normal", "sti": 0}], True))
    o_ffn = put(_sttbf_ffn(fonts, True))
    o_chpx = put(_plcf_bte([lo, hi], [chpx_first], True))
    o_papx = put(_plcf_bte([lo, hi], [papx_first], True))
    clx = bytearray()
    for g in grpprls: clx += bytes([1]) + u16(len(g)) + g
    plc = b"".join(u32(p[0]) for p in pieces) + u32(pieces[-1][1])
    for cp0, cp1, fc, comp, prm in pieces:
        plc += u16(0) + u32(fc * 2 | 0x40000000) + u16(prm)
    clx += bytes([2]) + u32(len(plc)) + plc
    o_clx = put(bytes(clx))
    fib = bytearray(fc_min)
    struct.pack_into("<HHHH", fib, 0, 0xA5EC, 193, 0, 0x409)
    struct.pack_into("<H", fib, 10, 0x204)
    struct.pack_into("<II", fib, 0x18, fc_min, hi)
    struct.pack_into("<H", fib, 32, 14); struct.pack_into("<H", fib, 62, 22)
    struct.pack_into("<I", fib, 0x4C, 13)
    struct.pack_into("<H", fib, 152, 93)
    for off, (fc, lcb) in ((0xA2, o_stsh), (0xFA, o_chpx), (0x102, o_papx), (0x112, put(_sttbf_ffn(fonts, True))), (0x1A2, o_clx)):
        struct.pack_into("<II", fib, off, fc, lcb)
    wd[:fc_min] = fib
    return cfb([("WordDocument", bytes(wd)), ("1Table", bytes(tbl))])


def word6_complex(fonts):
    """Word 6 fast-saved (Clx in the main stream): two pieces, the second with a grpprl that makes it italic."""
    fc_min = 0x200
    d, _ = word6([Para(["Hello "]), Para(["World"])], fonts=fonts,
                 complex_pieces=([s1(86, b"\x01")], [(0, 7, fc_min, 0), (7, 13, fc_min + 7, (0 << 1) | 1)]))
    return d
