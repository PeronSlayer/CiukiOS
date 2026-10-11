#!/usr/bin/env python3
"""Boot splash for the kernel's quiet console: the original Ciuki photograph.

Deterministic format conversion of misc/CiukiOS_SplashScreen.png (owner
requirement, AGENTS.md: keep the original Ciuki photograph in the boot
splash), without Pillow: PNG decoding per https://www.w3.org/TR/png/ (RGBA8,
non-interlaced, filters 0-4), exact 2:1 nearest-neighbour reduction to
512x384, alpha over black, RGB565 little endian. Nothing is redrawn.
"""
# SPDX-License-Identifier: MIT
import argparse
import hashlib
from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "misc/CiukiOS_SplashScreen.png"
SOURCE_SHA256 = "7bb975fa0f9b1870b87f0922b3ca385e6c2eabcce2bdf07f2759c8504af4ae8e"
WIDTH, HEIGHT = 512, 384
OUTPUT_SHA256 = "958841634565cfb36d07eb54c7f31566037e28869fc2dbadf8491cecffe3d444"


def decode(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("PNG signature")
    pos, compressed, header, ended = 8, bytearray(), None, False
    while pos < len(data):
        if pos + 12 > len(data):
            raise ValueError("truncated PNG chunk")
        length, kind = struct.unpack_from(">I4s", data, pos)
        end = pos + 12 + length
        if end > len(data):
            raise ValueError("truncated PNG data")
        payload = data[pos+8:pos+8+length]
        if zlib.crc32(kind + payload) != struct.unpack_from(">I", data, pos+8+length)[0]:
            raise ValueError("PNG CRC")
        if kind == b"IHDR":
            if header is not None or length != 13:
                raise ValueError("PNG IHDR")
            header = struct.unpack(">IIBBBBB", payload)
        elif kind == b"IDAT":
            compressed.extend(payload)
        elif kind == b"IEND":
            ended = True
        elif kind[0] & 32 == 0 and kind != b"PLTE":
            raise ValueError("unsupported critical PNG chunk")
        pos = end
    if not ended or header != (2 * WIDTH, 2 * HEIGHT, 8, 6, 0, 0, 0):
        raise ValueError("expected the 1024x768 non-interlaced RGBA8 splash PNG")
    w, h = header[:2]
    stride = w * 4
    inflater = zlib.decompressobj()
    raw = inflater.decompress(compressed, (stride + 1) * h + 1)
    if len(raw) != (stride + 1) * h or not inflater.eof:
        raise ValueError("PNG inflated length")
    rows, previous = [], bytearray(stride)
    for y in range(h):
        off = y * (stride + 1)
        f = raw[off]
        row = bytearray(raw[off+1:off+1+stride])
        if f == 1:
            for x in range(4, stride): row[x] = (row[x] + row[x-4]) & 255
        elif f == 2:
            for x in range(stride): row[x] = (row[x] + previous[x]) & 255
        elif f == 3:
            for x in range(stride):
                a = row[x-4] if x >= 4 else 0
                row[x] = (row[x] + (a + previous[x]) // 2) & 255
        elif f == 4:
            for x in range(stride):
                a = row[x-4] if x >= 4 else 0
                b = previous[x]
                c = previous[x-4] if x >= 4 else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                row[x] = (row[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        elif f:
            raise ValueError("PNG filter")
        rows.append(row)
        previous = row
    return w, h, rows


def convert(source=SOURCE):
    data = Path(source).read_bytes()
    if hashlib.sha256(data).hexdigest() != SOURCE_SHA256:
        raise ValueError("splash source SHA-256 mismatch")
    _, _, rows = decode(data)
    out = bytearray()
    for y in range(HEIGHT):
        row = rows[2 * y]
        for x in range(WIDTH):
            r, g, b, a = row[8*x:8*x+4]
            r, g, b = ((v * a + 127) // 255 for v in (r, g, b))
            out += struct.pack("<H", (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3))
    return bytes(out)


def build(out):
    """Write `out` unless it already holds this source's conversion."""
    out = Path(out)
    stamp = out.with_suffix(".sha256")
    if out.is_file() and stamp.is_file() and stamp.read_text().split() == [
            SOURCE_SHA256, hashlib.sha256(out.read_bytes()).hexdigest()]:
        return out
    data = convert()
    digest = hashlib.sha256(data).hexdigest()
    if digest != OUTPUT_SHA256:
        raise ValueError(f"converted splash differs from the recorded SHA-256: {digest}")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    stamp.write_text(f"{SOURCE_SHA256} {digest}\n")
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "build/f0/splash.rgb565")
    args = parser.parse_args()
    path = build(args.out)
    print(f"SHA256 {hashlib.sha256(path.read_bytes()).hexdigest()}  {path} ({path.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
