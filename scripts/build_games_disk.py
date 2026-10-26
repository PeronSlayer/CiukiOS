#!/usr/bin/env python3
"""Build the bundled shareware DOS games disk CIUKIGAM.IMG (owner decisions 2026-10-12/14).

Wolfenstein 3-D, DOOM, Quake, Warcraft II demo and Duke Nukem 3D are pre-installed games
of CiukiOS in their shareware releases, the only ones whose packages may be
redistributed:
Wolfenstein 3-D v1.4 (1wolf14.zip, episode 1), DOOM v1.9 (doom19s.zip,
episode 1), Quake v1.06 (quake106.zip, episode 1) and the Warcraft II: Tides
of Darkness DOS demo v1.20elc (war2demo.exe, Blizzard's electronic release),
and Duke Nukem 3D v1.3d (3dduke13.zip, Episode One).
Registered game data never enters this disk, the image or the repository.

Inputs are only the vendored original archives under third_party/games/
(never the network): each archive's size and SHA-256 and every installed
member's SHA-256 are pinned in its UPSTREAM.json, and a member that is not
installed must be listed as excluded with a reason. Unpacking:

- 1wolf14.zip holds W3DSW14.SHR, Apogee's installer container: a 3Ah-byte
  header, then per file a 0A8h-byte header (name at +0, compressed size at
  +88h) and the data compressed with the PKWARE Data Compression Library
  "implode" method. dcl_explode() is a Python port of Mark Adler's blast.c
  (zlib contrib/blast, zlib license; https://github.com/madler/zlib/blob/develop/contrib/blast/blast.c);
  the container layout follows Hans de Goede's extract.c (RPM Fusion
  wolf3d-shareware, https://github.com/rpmfusion/wolf3d-shareware/blob/master/extract.c).
- doom19s.zip holds DOOMS_19.1 and DOOMS_19.2, which concatenated are one
  ZIP archive (the DEICE installer's split; RPM Fusion doom-shareware and
  AUR doom1-wad unpack it the same way).

- 3dduke13.zip holds DN3DSW13.SHR, a nested ZIP. All 17 original members
  are installed unchanged, including LICENSE.TXT (V.7.16.98).
- quake106.zip holds RESOURCE.1, an MZ LHA self-extractor with 16 -lh5-
  members. The pinned 1945-byte prefix is skipped; every member CRC-16
  and installed SHA-256 is checked, including ID1/PAK0.PAK.

The disk is a DOS VM base image of the DOSBOOT.IMG geometry
(scripts/build_dos_image.write_fat16): the DOS system read back from the
built DOSBOOT.IMG and checked against its manifest
(scripts/test/dos_fixtures.dos_system), \\WOLF3D, \\DOOM and \\QUAKE (with \\QUAKE\\ID1), and the
generated \\DOOM\\CIUKI.CFG of f3-06. fsck.fat and mtools read it back.
Its trailing all-zero sectors are then cut (the kernel reads sectors past
the end of a shorter base as zeros, src/kernel/vm/dosvm_disk.c), so the
128 MiB volume costs only its used sectors in the 512 MiB system image;
the manifest records both the file hash and the full logical image hash.
"""
# SPDX-License-Identifier: GPL-2.0-only
import argparse
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import struct
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
GAMES = "third_party/games"
NAME = "CIUKIGAM.IMG"
# Install order on the disk; each name is a directory under third_party/games.
PACKAGES = ("wolf3d-shareware", "doom-shareware", "quake-shareware", "war2-demo", "duke3d-shareware")
REDISTRIBUTION = "free"
SECTOR = 512


BLIZZARD_MAGIC, BLIZZARD_VERSION = b"WAR\0", 4


def blizzard_members(data, offset):
    """{NAME: bytes} of Blizzard's installer container (Warcraft II demo, v1.20elc):
    SETUP.EXE is the executable before `offset`; entries are validated and
    every LHA member's CRC-16 and size are checked."""
    if data[offset:offset + 4] != BLIZZARD_MAGIC:
        raise ValueError("Blizzard installer: container not at the pinned offset")
    count, version = struct.unpack_from("<HH", data, offset + 4)
    if not 2 <= count <= 256 or version != BLIZZARD_VERSION:
        raise ValueError("Blizzard installer: unsupported header")
    at, entries = offset + 12, []
    for _ in range(count):
        if at + 14 > len(data):
            raise ValueError("Blizzard installer: truncated entry")
        word, size, _check = struct.unpack_from("<III", data, at)
        length, flag = word & 0xFFFFFF, word >> 24
        if not 14 <= length <= 80 or at + length > len(data) or flag not in (0, 0x80):
            raise ValueError("Blizzard installer: bad entry record")
        raw = data[at + 12:at + length]
        if not raw.endswith(b"\0") or b"\0" in raw[:-1]:
            raise ValueError("Blizzard installer: bad entry name")
        entries.append((raw[:-1].decode("ascii", "replace").upper().replace("\\", "/"), flag, size))
        at += length
    members = {}
    first, flag, size = entries[0]
    if first != "SETUP.EXE" or flag or size != offset:
        raise ValueError("Blizzard installer: first entry is not the SETUP.EXE stub")
    members[first] = data[:offset]
    total = offset
    for name, flag, size in entries[1:]:
        if size > 32 * 1024 * 1024 or total + size > 64 * 1024 * 1024:
            raise ValueError("Blizzard installer: output too large")
        if not flag:
            if at + size > len(data) or name in members or any(not DOS_NAME.fullmatch(p) for p in name.split("/")):
                raise ValueError("Blizzard installer: bad stored member " + name)
            members[name] = data[at:at + size]
            at += size
        else:
            if at + 27 > len(data):
                raise ValueError("Blizzard installer: truncated LHA member")
            end = at + data[at] + 2 + struct.unpack_from("<I", data, at + 7)[0]
            if end >= len(data) or data[end] != 0:
                raise ValueError("Blizzard installer: LHA member lacks its terminator: " + name)
            (real, packed, usize, crc), = lha_entries(data[at:end + 1])
            output = lh5_decode(packed, usize)
            if usize != size or lha_crc16(output) != crc or real in members or name not in (real[:-1] + "A", real + "A"):
                raise ValueError("Blizzard installer: member differs from its record: " + name)
            members[real] = output
            at = end + 1
        total += size
    if len(data) - at > 4096:
        raise ValueError("Blizzard installer: unexpected data after the last member")
    return members


def _module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# Code always comes from this checkout; `root` below only locates data
# (third_party/games and the owner's local game directories), so host tests
# can point it at a fixture tree.
def _dos_build():
    return _module("build_dos_image", ROOT / "scripts/build_dos_image.py")


def _fixtures():
    return _module("dos_fixtures", ROOT / "scripts/test/dos_fixtures.py")


# ---- PKWARE DCL explode (blast.c port) ----------------------------------

def _huffman(rep):
    """blast.c construct(): compact (count-1, length) bytes -> canonical tables."""
    lengths = []
    for byte in rep:
        lengths += [byte & 15] * ((byte >> 4) + 1)
    count = [0] * 14
    for n in lengths:
        count[n] += 1
    offs = [0] * 15
    for n in range(1, 13):
        offs[n + 1] = offs[n] + count[n]
    symbol = [0] * len(lengths)
    for s, n in enumerate(lengths):
        if n:
            symbol[offs[n]] = s
            offs[n] += 1
    return count, symbol


_LITERAL = _huffman(bytes((
    11, 124, 8, 7, 28, 7, 188, 13, 76, 4, 10, 8, 12, 10, 12, 10, 8, 23, 8,
    9, 7, 6, 7, 8, 7, 6, 55, 8, 23, 24, 12, 11, 7, 9, 11, 12, 6, 7, 22, 5,
    7, 24, 6, 11, 9, 6, 7, 22, 7, 11, 38, 7, 9, 8, 25, 11, 8, 11, 9, 12,
    8, 12, 5, 38, 5, 38, 5, 11, 7, 5, 6, 21, 6, 10, 53, 8, 7, 24, 10, 27,
    44, 253, 253, 253, 252, 252, 252, 13, 12, 45, 12, 45, 12, 61, 12, 45,
    44, 173)))
_LENGTH = _huffman(bytes((2, 35, 36, 53, 38, 23)))
_DISTANCE = _huffman(bytes((2, 20, 53, 230, 247, 151, 248)))
_BASE = (3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264)
_EXTRA = (0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8)


def dcl_explode(data, limit=16 * 1024 * 1024):
    """Decompress one PKWARE DCL stream; returns (output, input bytes used)."""
    pos = buf = cnt = 0

    def bits(need):
        nonlocal pos, buf, cnt
        while cnt < need:
            if pos >= len(data):
                raise ValueError("DCL stream: out of input")
            buf |= data[pos] << cnt
            pos += 1
            cnt += 8
        value = buf & ((1 << need) - 1)
        buf >>= need
        cnt -= need
        return value

    def decode(table):
        count, symbol = table
        code = first = index = 0
        for length in range(1, 14):
            code |= bits(1) ^ 1           # codes are stored inverted, bit by bit
            n = count[length]
            if code < first + n:
                return symbol[index + code - first]
            index += n
            first = (first + n) << 1
            code <<= 1
        raise ValueError("DCL stream: invalid code")

    literal, dictionary = bits(8), bits(8)
    if literal > 1 or not 4 <= dictionary <= 6:
        raise ValueError("DCL stream: bad header")
    out = bytearray()
    while True:
        if bits(1):
            symbol = decode(_LENGTH)
            length = _BASE[symbol] + bits(_EXTRA[symbol])
            if length == 519:             # end code
                break
            shift = 2 if length == 2 else dictionary
            distance = (decode(_DISTANCE) << shift) + bits(shift) + 1
            if distance > len(out):
                raise ValueError("DCL stream: distance too far back")
            start = len(out) - distance
            for i in range(length):       # overlapping copies are allowed
                out.append(out[start + i])
        else:
            out.append(decode(_LITERAL) if literal else bits(8))
        if len(out) > limit:
            raise ValueError("DCL stream: output too large")
    return bytes(out), pos


# ---- LHA -lh5- (LHa 2.x static Huffman / 8 KiB LZSS) -----------------------
# Independent implementation of the format documented by LHa header.doc,
# src/huf.c and Lhasa lib/lh_new_decoder.c; see docs/design/quake-shareware.md.

def _crc_table():
    table = []
    for n in range(256):
        for _ in range(8):
            n = (n >> 1) ^ (0xA001 if n & 1 else 0)
        table.append(n)
    return tuple(table)


_CRC16 = _crc_table()


def lha_crc16(data):
    crc = 0
    for byte in data:
        crc = (crc >> 8) ^ _CRC16[(crc ^ byte) & 255]
    return crc


def lh5_decode(data, size, limit=32 * 1024 * 1024):
    """Decode exactly size bytes; bounded input, trees, blocks and output."""
    if not 0 <= size <= limit:
        raise ValueError("LH5: output too large")
    pos = buf = count = 0

    def bits(n):
        nonlocal pos, buf, count
        while count < n:
            if pos == len(data):
                raise ValueError("LH5: out of input")
            buf = (buf << 8) | data[pos]
            pos += 1
            count += 8
        count -= n
        value = buf >> count
        buf &= (1 << count) - 1
        return value

    def tree(lengths):
        counts = [lengths.count(n) for n in range(17)]
        if any(n > 16 for n in lengths):
            raise ValueError("LH5: code length too large")
        code = 0
        tables = [{} for _ in range(17)]
        for n in range(1, 17):
            code <<= 1
            if code + counts[n] > 1 << n:
                raise ValueError("LH5: oversubscribed tree")
            for symbol, length in enumerate(lengths):
                if length == n:
                    tables[n][code] = symbol
                    code += 1
        if code != 1 << 16:
            raise ValueError("LH5: incomplete tree")
        return tables

    def decode(t):
        if isinstance(t, int):
            return t
        code = 0
        for n in range(1, 17):
            code = (code << 1) | bits(1)
            if code in t[n]:
                return t[n][code]
        raise ValueError("LH5: invalid code")

    def read_tree(symbols, width, special=False):
        n = bits(width)
        if not n:
            symbol = bits(width)
            if symbol >= symbols:
                raise ValueError("LH5: invalid single symbol")
            return symbol
        if n > symbols:
            raise ValueError("LH5: too many tree lengths")
        lengths = []
        while len(lengths) < n:
            length = bits(3)
            if length == 7:
                while bits(1):
                    length += 1
                    if length > 16:
                        raise ValueError("LH5: code length too large")
            lengths.append(length)
            if special and len(lengths) == 3:
                lengths.extend([0] * bits(2))
            if len(lengths) > n:
                raise ValueError("LH5: tree zero run overflow")
        return tree(lengths + [0] * (symbols - n))

    history = bytearray(b" " * 8192)
    cursor = remaining = 0
    out = bytearray()
    while len(out) < size:
        if not remaining:
            remaining = bits(16)
            if not remaining:
                raise ValueError("LH5: empty block")
            temp = read_tree(19, 5, True)
            n = bits(9)
            if not n:
                codes = bits(9)
                if codes >= 510:
                    raise ValueError("LH5: invalid single symbol")
            else:
                if n > 510:
                    raise ValueError("LH5: too many code lengths")
                lengths = []
                while len(lengths) < n:
                    c = decode(temp)
                    if c <= 2:
                        run = 1 if c == 0 else bits(4) + 3 if c == 1 else bits(9) + 20
                        lengths.extend([0] * run)
                    else:
                        lengths.append(c - 2)
                    if len(lengths) > n:
                        raise ValueError("LH5: code zero run overflow")
                codes = tree(lengths + [0] * (510 - n))
            offsets = read_tree(14, 4)
        remaining -= 1
        c = decode(codes)
        if c < 256:
            out.append(c)
            history[cursor] = c
            cursor = (cursor + 1) & 8191
        else:
            length = c - 253
            if len(out) + length > size:
                raise ValueError("LH5: match exceeds output size")
            p = decode(offsets)
            distance = (1 << (p - 1)) + bits(p - 1) if p else 0
            start = (cursor - distance - 1) & 8191
            for i in range(length):
                byte = history[(start + i) & 8191]
                out.append(byte)
                history[cursor] = byte
                cursor = (cursor + 1) & 8191
    if remaining:
        raise ValueError("LH5: unused block commands")
    return bytes(out)


def lha_entries(data):
    """Yield validated (DOS path, packed bytes, size, CRC16), header levels 0..2."""
    at = 0
    seen = set()
    total = 0
    while at < len(data):
        if len(data) - at == 1 and data[at] == 0:
            return
        if len(data) - at < 24:
            raise ValueError("LHA: truncated header")
        level = data[at + 20]
        if level not in (0, 1, 2):
            raise ValueError("LHA: unsupported header level")
        header_size = struct.unpack_from("<H", data, at)[0] if level == 2 else data[at] + 2
        end = at + header_size
        minimum = 26 if level == 2 else 27 if level == 1 else 24
        if header_size < minimum or end > len(data):
            raise ValueError("LHA: truncated header")
        h = bytearray(data[at:end])
        if level < 2 and sum(h[2:]) & 255 != h[1]:
            raise ValueError("LHA: header checksum mismatch")
        if h[2:7] != b"-lh5-":
            raise ValueError("LHA: unsupported method")
        packed, size = struct.unpack_from("<II", h, 7)
        if level < 2:
            n = h[21]
            if 24 + n + (3 if level == 1 else 0) > len(h):
                raise ValueError("LHA: truncated filename/CRC")
            filename = bytes(h[22:22 + n])
            crc = struct.unpack_from("<H", h, 22 + n)[0]
            ext = len(h) - 2 if level == 1 else None
        else:
            filename = b""
            crc = struct.unpack_from("<H", h, 21)[0]
            ext = 24
        directory = b""
        common_crc = None
        while ext is not None:
            if ext + 2 > len(h):
                raise ValueError("LHA: truncated extended header")
            n = struct.unpack_from("<H", h, ext)[0]
            if not n:
                break
            if n < 3:
                raise ValueError("LHA: invalid extended header size")
            if level == 1:
                if n > packed or end + n > len(data):
                    raise ValueError("LHA: truncated extended header")
                h.extend(data[end:end + n])
                end += n
                packed -= n  # level 1 skip size includes extended headers
            if ext + n + 2 > len(h):
                raise ValueError("LHA: truncated extended header")
            kind, body = h[ext + 2], bytes(h[ext + 3:ext + n])
            if kind == 1:
                filename = body
            elif kind == 2:
                directory = body.replace(b"\xff", b"/")
            elif kind == 0:
                if len(body) < 2 or common_crc is not None:
                    raise ValueError("LHA: invalid common header")
                common_crc = struct.unpack_from("<H", body)[0]
                h[ext + 3:ext + 5] = b"\0\0"
            ext += n
        if common_crc is not None and lha_crc16(h) != common_crc:
            raise ValueError("LHA: header CRC mismatch")
        try:
            name = (directory + filename).decode("ascii").replace("\\", "/").upper()
        except UnicodeDecodeError as error:
            raise ValueError("LHA: non-ASCII path") from error
        if not name or any(not DOS_NAME.fullmatch(p) for p in name.split("/")) or name in seen:
            raise ValueError("LHA: bad or duplicate member path " + name)
        seen.add(name)
        total += size
        if size > 32 * 1024 * 1024 or total > 64 * 1024 * 1024:
            raise ValueError("LHA: output too large")
        if end + packed > len(data):
            raise ValueError("LHA: truncated member " + name)
        yield name, data[end:end + packed], size, crc
        at = end + packed
    raise ValueError("LHA: missing archive terminator")


def lha_members(data):
    members = {}
    for name, packed, size, crc in lha_entries(data):
        output = lh5_decode(packed, size)
        if lha_crc16(output) != crc:
            raise ValueError("LHA: member CRC mismatch: " + name)
        members[name] = output
    return members


# ---- package containers ---------------------------------------------------

DOS_NAME = re.compile(r"[A-Z0-9_\-]{1,8}(\.[A-Z0-9_\-]{1,3})?")
SHR_HEADER, SHR_ENTRY, SHR_SIZE = 0x3A, 0xA8, 0x88


def shr_members(data):
    """{NAME: bytes} of an Apogee .SHR installer container, in order."""
    members, at = {}, SHR_HEADER
    if len(data) < SHR_HEADER:
        raise ValueError("SHR container: short header")
    while at < len(data):
        if at + SHR_ENTRY > len(data):
            raise ValueError("SHR container: truncated entry header")
        raw = data[at:at + 16].split(b"\0", 1)[0]
        name = raw.decode("ascii", "replace").upper()
        if not DOS_NAME.fullmatch(name) or name in members:
            raise ValueError(f"SHR container: bad member name {raw!r}")
        size = struct.unpack_from("<I", data, at + SHR_SIZE)[0]
        body = data[at + SHR_ENTRY:at + SHR_ENTRY + size]
        if len(body) != size:
            raise ValueError("SHR container: truncated member " + name)
        output, used = dcl_explode(body)
        if used != size:
            raise ValueError("SHR container: trailing data in " + name)
        members[name] = output
        at += SHR_ENTRY + size
    return members


def zip_members(data):
    """{NAME: bytes} of a ZIP archive (CRC-32 checked by zipfile)."""
    members = {}
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        for info in archive.infolist():
            name = info.filename.upper()
            if info.is_dir() or not DOS_NAME.fullmatch(name) or name in members:
                raise ValueError("ZIP archive: unexpected member " + info.filename)
            members[name] = archive.read(info)
    return members


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def upstream(package, root=ROOT):
    path = Path(root) / GAMES / package / "UPSTREAM.json"
    record = json.loads(path.read_text())
    for key in ("archive", "size", "sha256", "unpack", "install", "license_files", "redistribution"):
        if key not in record:
            raise ValueError(f"{path}: missing {key}")
    return record


def archive(package, record, root=ROOT):
    """The vendored original archive, refused unless size and SHA-256 match the pin."""
    path = Path(root) / GAMES / package / record["archive"]
    data = path.read_bytes()
    if len(data) != record["size"] or sha256(data) != record["sha256"]:
        raise ValueError(f"{path}: differs from the pinned size/SHA-256 in UPSTREAM.json")
    return data


def unpack(record, data):
    """(outer members, installable members) as UPSTREAM.json "unpack" declares."""
    spec = record["unpack"]
    if spec.get("format") == "blizzard-install":   # the archive is the installer executable itself
        return {}, blizzard_members(data, spec["offset"])
    outer = zip_members(data)
    if spec.get("format") == "zip-split":          # DOOMS_19.1 + DOOMS_19.2 = one ZIP
        inner = zip_members(b"".join(outer[name] for name in spec["parts"]))
    elif spec.get("format") == "apogee-shr":       # W3DSW14.SHR, DCL imploded members
        inner = shr_members(outer[spec["member"]])
    elif spec.get("format") == "lha-sfx":
        inner = lha_members(outer[spec["member"]][spec["offset"]:])
    elif spec.get("format") == "zip-sfx":         # DN3DSW13.SHR is a ZIP, not Wolf's DCL container
        inner = zip_members(outer[spec["member"]])
    else:
        raise ValueError("unknown unpack format: " + str(spec.get("format")))
    return outer, inner


def package_files(package, root=ROOT):
    """Pinned files of one package: [(disk path, bytes, record)], plus the licence
    documents [(name, bytes)] checked against the archive's own members."""
    record = upstream(package, root)
    outer, inner = unpack(record, archive(package, record, root))
    install = record["install"]
    directory = install["directory"]
    if not DOS_NAME.fullmatch(directory):
        raise ValueError("bad install directory: " + directory)
    pinned = {f["name"]: f for f in install["files"]}
    excluded = install.get("excluded", {})
    unknown = set(inner) - set(pinned) - set(excluded)
    missing = (set(pinned) | set(excluded)) - set(inner)
    if unknown or missing:
        raise ValueError(f"{package}: members not pinned {sorted(unknown)}, pinned but absent {sorted(missing)}")
    files = []
    for name, pin in sorted(pinned.items()):
        data = inner[name]
        if len(data) != pin["size"] or sha256(data) != pin["sha256"]:
            raise ValueError(f"{package}: member {name} differs from its pin")
        files.append((f"/{directory}/{name}", data, {"package": package, "role": pin.get("role", "data"),
                                                      "redistribution": REDISTRIBUTION}))
    licenses = []
    for item in record["license_files"]:
        path = Path(root) / GAMES / package / item["name"]
        data = path.read_bytes()
        source = (outer if item["from"] == "archive" else inner).get(item["name"])
        if data != source or sha256(data) != item["sha256"]:
            raise ValueError(f"{path}: not the byte-identical member of {record['archive']}")
        licenses.append((item["name"], data))
    return record, files, licenses


def free_hashes(root=ROOT):
    """SHA-256 of every file pinned in a vendored shareware package (installed
    members and licence documents). Only these may equal one of the owner's
    local files (the shareware and registered DOOM 1.9 share SETUP.EXE, the
    network tools and the documents) or carry a reserved name (DOOM.EXE,
    DOOM1.WAD, WOLF3D.EXE). The pins alone; build() proves them against the
    archives."""
    hashes = set()
    for package in PACKAGES:
        record = upstream(package, root)
        hashes.update(f["sha256"] for f in record["install"]["files"])
        hashes.update(f["sha256"] for f in record["license_files"])
    return hashes


# Warcraft II data containers (*.WAR, *.SUD, *.CUD): only the pinned demo's own
# members may carry these names (the retail game's files use the same names).
WAR2_DATA = re.compile(r"[^/]*\.(WAR|SUD|CUD)")
WAR2_DIR = "/WAR2"
# The Miles Audio Interface Library V3.03 files WAR2's sound setup writes
# (SETUP.EXE strings: DEVICE, DRIVER, IO_ADDR %Xh, IRQ, DMA_8_BIT, DMA_16_BIT),
# for the DOS VM's Sound Blaster 16 (FDAUTO.BAT: BLASTER=A220 I7 D1 H5 T6): the
# aildrvr.lst entries "Sound Blaster 16/AWE32" (SB16.DIG) and "Creative Labs
# Sound Blaster 16" (SBPRO2.MDI, the FM music driver). They live beside the
# drivers: WAR2.EXE changes to its DRIVERS directory, opens MDI.INI and DIG.INI
# there and changes back (strings "drivers", "MDI.INI", "DIG.INI"; host-runner
# INT 21h trace 2026-10-25: AH=3Bh c:\war2\drivers, 3D00h MDI.INI, AH=3Bh c:\war2).
WAR2_SOUND = (("DIG.INI", "Sound Blaster 16/AWE32", "SB16.DIG"),
              ("MDI.INI", "Creative Labs Sound Blaster 16", "SBPRO2.MDI"))
WAR2_SOUND_DIR = WAR2_DIR + "/DRIVERS"


def war2_files():
    """Generated [(disk path, bytes)]: DRIVERS/DIG.INI, DRIVERS/MDI.INI and
    WAR2.BAT. WAR2.EXE looks for a VESA BIOS first (INT 10h 4F00h/4F01h, then
    4F02h 101h windowed and the 4F0Ah protected-mode table) and only without
    one for UNIVBE.DRV, which UVCONFIG.EXE writes. The DOS VM's VBE BIOS serves
    every display since war2vbe (2026-10-25, host-runner INT 10h trace), so the
    launcher starts WAR2.EXE directly: UVCONFIG's interactive dialog detected
    only "Generic VGA, 256 KB" in the VM and is not needed. UVCONFIG.EXE stays
    installed with the demo."""
    files = []
    for name, device, driver in WAR2_SOUND:
        text = (";Miles Design Audio Interface Library V3.03 of 18-Jun-95\r\n"
                f"DEVICE      {device}\r\nDRIVER      {driver}\r\nIO_ADDR     220h\r\n"
                "IRQ         7\r\nDMA_8_BIT   1\r\nDMA_16_BIT  5\r\n")
        files.append((f"{WAR2_SOUND_DIR}/{name}", text.encode("ascii")))
    files.append((f"{WAR2_DIR}/WAR2.BAT", b"@ECHO OFF\r\nWAR2.EXE\r\n"))
    return files


def duke3d_files():
    """Original release has no CFG; CONFIG.C/SNDCARDS.H define this format.
    VGA 320x200, SB16 PCM and SoundBlaster OPL music (device 0), keyboard
    and mouse. DUKESB selects IRQ 5 in the VM's existing CT1745 mixer;
    changing BLASTER alone would leave the virtual card on IRQ 7."""
    text = """;CiukiOS Duke Nukem 3D shareware defaults
[Screen Setup]
ScreenMode = 0
ScreenWidth = 320
ScreenHeight = 200
ScreenSize = 8
ScreenGamma = 0
Shadows = 1
Detail = 1
Tilt = 1
Messages = 1
Out = 0
Password = ""
[Sound Setup]
FXDevice = 0
MusicDevice = 0
FXVolume = 192
MusicVolume = 160
SoundToggle = 1
MusicToggle = 1
VoiceToggle = 1
AmbienceToggle = 1
NumVoices = 8
NumChannels = 2
NumBits = 16
MixRate = 22000
MidiPort = 0x330
BlasterAddress = 0x220
BlasterType = 6
BlasterInterrupt = 5
BlasterDma8 = 1
BlasterDma16 = 5
BlasterEmu = 0x620
ReverseStereo = 0
[Controls]
ControllerType = 1
MouseSensitivity = 32768
MouseAiming = 0
MouseButton0 = "Fire"
MouseButton1 = "Open"
MouseButtonClicked0 = ""
MouseButtonClicked1 = ""
MouseAnalogAxes0 = "analog_turning"
MouseAnalogAxes1 = "analog_moving"
MouseAnalogScale0 = 65536
MouseAnalogScale1 = 65536
[KeyDefinitions]
Move_Forward = "Up" "W"
Move_Backward = "Down" "S"
Turn_Left = "Left" ""
Turn_Right = "Right" ""
Strafe_Left = "A" ""
Strafe_Right = "D" ""
Strafe = "LAlt" "RAlt"
Fire = "LCtrl" "RCtrl"
Open = "Space" "E"
Run = "LShift" "RShift"
AutoRun = "CapLck" ""
Jump = "X" ""
Crouch = "C" ""
Look_Up = "PgUp" ""
Look_Down = "PgDn" ""
Aim_Up = "Home" ""
Aim_Down = "End" ""
Weapon_1 = "1" ""
Weapon_2 = "2" ""
Weapon_3 = "3" ""
Weapon_4 = "4" ""
Weapon_5 = "5" ""
Weapon_6 = "6" ""
Weapon_7 = "7" ""
Weapon_8 = "8" ""
Weapon_9 = "9" ""
Weapon_10 = "0" ""
Inventory = "Enter" ""
Inventory_Left = "[" ""
Inventory_Right = "]" ""
Map = "Tab" ""
Shrink_Screen = "-" ""
Enlarge_Screen = "=" ""
Center_View = "Kpad5" ""
[Comm Setup]
PlayerName = "Duke"
RTSName = "DUKE.RTS"
[Misc]
Executions = 0
RunMode = 0
Crosshairs = 1
"""
    # NASM is already a DOS payload build dependency; keep machine code in
    # its source file rather than an opaque embedded byte string.
    with tempfile.TemporaryDirectory(prefix="dukesb-") as temp:
        helper = _dos_build().assemble("src/com/dukesb.asm", Path(temp) / "DUKESB.COM")
    launcher = (b"@ECHO OFF\r\nDUKESB.COM 5\r\nSET BLASTER=A220 I5 D1 H5 T6\r\n"
                b"DUKE3D.EXE\r\nDUKESB.COM 7\r\nSET BLASTER=A220 I7 D1 H5 T6\r\n")
    return [("/DUKE3D/DUKE3D.CFG", text.replace("\n", "\r\n").encode("ascii")),
            ("/DUKE3D/DUKE3D.BAT", launcher), ("/DUKE3D/DUKESB.COM", helper)]


def generated_files(fixtures=None):
    """Every generated (not pinned) game file, [(disk path, bytes, record)]:
    DOOM's CIUKI.CFG, Warcraft II's sound setup and Duke's preset/launchers. The
    games disk and the DOS image (scripts/build_dos_image.py) both install
    exactly this list, so a generated file cannot reach only one of them."""
    fixtures = fixtures or _fixtures()
    # f3-06 DOOM defaults (dos_fixtures.DOOM_CONFIG): SB16 at 220h/IRQ 7/DMA 1, no mouse or joystick.
    files = [("/DOOM/CIUKI.CFG", fixtures.DOOM_CONFIG, {"package": None, "role": "config",
                                                        "redistribution": REDISTRIBUTION, "generated": True})]
    for path, data in war2_files():
        files.append((path, data, {"package": None, "role": "config" if path.endswith(".INI") else "launcher",
                                   "redistribution": REDISTRIBUTION, "generated": True}))
    for path, data in duke3d_files():
        files.append((path, data, {"package": None, "role": "config" if path.endswith(".CFG") else "launcher",
                                  "redistribution": REDISTRIBUTION, "generated": True}))
    return files


def refuse_registered(files, root=ROOT):
    """No registered game data (DOOM.WAD, *.WL6, ...) and no other local
    commercial file: the DOS system and generated files go through the f3-06
    guard; shareware members must be pinned (free_hashes)."""
    dos = _dos_build()
    free = free_hashes(root)
    for path, data, info in files:
        name = path.rsplit("/", 1)[-1].upper()
        if dos.REGISTERED_DATA.fullmatch(name):
            raise ValueError(f"refusing registered game data {path}")
        if info.get("package") and sha256(data) not in free:
            raise ValueError(f"refusing unpinned shareware member {path}")
        if (WAR2_DATA.fullmatch(name) or path.upper() == WAR2_DIR + "/WAR2.EXE") and (
                info.get("package") != "war2-demo" or sha256(data) not in free):
            raise ValueError(f"refusing Warcraft II data that is not the pinned demo's {path}")
        if name in ("DUKE3D.EXE", "DUKE3D.GRP"):
            pins = upstream("duke3d-shareware", root)["install"]["files"]
            expected = next(f["sha256"] for f in pins if f["name"] == name)
            if info.get("package") != "duke3d-shareware" or sha256(data) != expected:
                raise ValueError(f"refusing Duke 3D data that is not the pinned shareware's {path}")
    dos.refuse_local_payloads({path: sha256(data) for path, data, _ in files}, root, free=free)


# ---- the disk -------------------------------------------------------------

def last_used_sector(path):
    """Index of the last sector holding a nonzero byte (allocated extents only)."""
    size = Path(path).stat().st_size
    last = -1
    fd = os.open(path, os.O_RDONLY)
    try:
        cursor = 0
        while cursor < size:
            try:
                start = os.lseek(fd, cursor, os.SEEK_DATA)
            except OSError:
                break
            end = min(os.lseek(fd, start, os.SEEK_HOLE), size)
            at = start
            while at < end:
                block = os.pread(fd, min(1 << 20, end - at), at)
                if not block:
                    raise ValueError("short image read")
                stripped = block.rstrip(b"\0")
                if stripped:
                    last = (at + len(stripped) - 1) // SECTOR
                at += len(block)
            cursor = end
    finally:
        os.close(fd)
    return last


def build(out=None, dos_image=None, root=ROOT):
    root = Path(root)
    out = Path(out or root / "build/f0/games" / NAME)
    dos_image = Path(dos_image or root / "build/f0/dos/UNIFIED.IMG")
    for forbidden in ("third_party",):
        if out.resolve().is_relative_to((root / forbidden).resolve()):
            raise ValueError("the games disk is a build output, never under " + forbidden)
    dos, fixtures = _dos_build(), _fixtures()
    boot, loader, system, dos_sha = fixtures.dos_system(dos_image)
    files = [(path, data, {"role": "dos_system", "redistribution": "ciukios"}) for path, data in sorted(system.items())]
    packages, licenses = [], []
    # The DOS VM payloads keep their nested directories (/SYSTEM, /D48 since f3-51).
    directories = sorted({path.rsplit("/", 1)[0] for path in system if path.count("/") > 1})
    for package in PACKAGES:
        record, members, documents = package_files(package, root)
        directories.append("/" + record["install"]["directory"])
        directories += sorted({path.rsplit("/", 1)[0] for path, _, _ in members
                               if path.count("/") > 2})
        files += members
        licenses += [(package, name, data) for name, data in documents]
        packages.append({"name": package, "archive": record["archive"], "size": record["size"],
                         "sha256": record["sha256"], "directory": "/" + record["install"]["directory"],
                         "redistribution": record["redistribution"], "bundling": record.get("bundling", ""),
                         "license_files": [f"{GAMES}/{package}/{item['name']}" for item in record["license_files"]]})
    files += generated_files(fixtures)
    refuse_registered(files, root)
    paths = [path for path, _, _ in files]
    if len(set(p.upper() for p in paths)) != len(paths):
        raise ValueError("duplicate path on the games disk")
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".games-", dir=out.parent) as temporary:
        full = Path(temporary) / "full.img"
        # The boot sector's BPB names CIUKIOSFULL: keep the root label equal (fsck.fat compares them).
        checked = fixtures.write_guest(full,boot,loader,{path: data for path, data, _ in files},directories)
        for path, data, _ in files:
            if fixtures.run(["mcopy", "-n", "-i", fixtures.mtools_image(full), "::" + path, "-"]).stdout != data:
                raise ValueError("games disk read-back mismatch: " + path)
        listing = fixtures.run(["mdir", "-/", "-a", "-i", fixtures.mtools_image(full), "::/"]).stdout.decode(errors="replace")
        logical_sha = fixtures.sha(full)
        used = last_used_sector(full) + 1
        size = max(used, dos.DATA_LBA + dos.SPC) * SECTOR
        candidate = Path(temporary) / NAME
        with full.open("rb") as source, candidate.open("wb") as dest:
            for at in range(0, size, 1 << 20):
                dest.write(source.read(min(1 << 20, size - at)))
        # The cut image extended with zeros is the logical image again.
        h = hashlib.sha256(candidate.read_bytes())
        zero = bytes(1 << 20)
        remaining = dos.SECTORS * SECTOR - size
        while remaining:
            h.update(zero[:min(len(zero), remaining)])
            remaining -= min(len(zero), remaining)
        if h.hexdigest() != logical_sha:
            raise ValueError("cut games disk does not extend to the logical image")
        candidate.replace(out)
    manifest = {"schema_version": 1, "name": NAME, "image_sha256": sha256(out.read_bytes()), "size": size,
                "logical_size": dos.SECTORS * SECTOR, "logical_sha256": logical_sha, "dos_image_sha256": dos_sha,
                "geometry": dos.GEOMETRY, "redistribution": REDISTRIBUTION, "packages": packages,
                "files": [{"path": path, "size": len(data), "sha256": sha256(data), **info} for path, data, info in files],
                "licenses": [{"package": package, "name": name, "path": f"{GAMES}/{package}/{name}",
                              "size": len(data), "sha256": sha256(data)} for package, name, data in licenses],
                "checkers": {"fsck": checked, "listing": listing}}
    out.with_suffix(".json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    games = sum(1 for _, _, info in files if info.get("package") or info.get("generated"))
    print(f"T1 GAMES PASS: FAT16 {NAME}, {games} game files from {len(packages)} pinned shareware packages, "
          f"{len(files) - games} DOS system files read back; {size} of {dos.SECTORS * SECTOR} bytes kept; "
          f"SHA256 {manifest['image_sha256']}")
    return out, manifest


def validate(out, root=ROOT):
    """The built disk and its manifest agree (build_image.py, host tests)."""
    out = Path(out)
    manifest = json.loads(out.with_suffix(".json").read_text())
    data = out.read_bytes()
    if len(data) != manifest["size"] or sha256(data) != manifest["image_sha256"]:
        raise ValueError(f"{out} differs from its manifest; rebuild it")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", type=Path, default=ROOT / "build/f0/games" / NAME)
    parser.add_argument("--dos-image", type=Path, default=ROOT / "build/f0/dos/UNIFIED.IMG")
    args = parser.parse_args()
    build(args.out, args.dos_image)
    return 0


if __name__ == "__main__":
    sys.exit(main())
