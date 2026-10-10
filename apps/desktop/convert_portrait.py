#!/usr/bin/env python3
"""Deterministic conversion of the exact approved Ciuki PNG, without Pillow.

PNG filter/alpha rules: https://www.w3.org/TR/png/ . Keep encoded RGB samples;
nearest-neighbour resizing and integer alpha flattening are format conversion.
No palette, colour-space, pose, shape or artwork substitution is performed.
"""
# SPDX-License-Identifier: MIT
import argparse
import hashlib
from pathlib import Path
import struct
import zlib

SOURCE_SHA256 = "17210ac9cfe07c54d6b0069a1502a036d52cb6340715ad47bdd54fa4dd386620"
ARRAY_SHA256 = "1c63236be6380187c5fde01a9e91a69cadd581a68910eaf0055762a4c3e96839"
SIZE = 256
BACKGROUND = (0x37, 0x55, 0x64)


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
        crc = struct.unpack_from(">I", data, pos+8+length)[0]
        if zlib.crc32(kind + payload) != crc:
            raise ValueError("PNG CRC")
        if kind == b"IHDR":
            if header is not None or length != 13:
                raise ValueError("PNG IHDR")
            header = struct.unpack(">IIBBBBB", payload)
        elif kind == b"IDAT":
            compressed.extend(payload)
        elif kind == b"IEND":
            ended = True
            if length or end != len(data):
                raise ValueError("PNG IEND")
        elif kind[0] & 32 == 0 and kind != b"PLTE":
            raise ValueError("unsupported critical PNG chunk")
        pos = end
    if not ended or header != (1254, 1254, 8, 6, 0, 0, 0):
        raise ValueError("expected approved 1254x1254 noninterlaced RGBA8 PNG")
    w, h = header[:2]
    stride = w * 4
    # Bound inflate output before allocation; the source digest is also pinned.
    inflater = zlib.decompressobj()
    raw = inflater.decompress(compressed, (stride+1)*h+1)
    if len(raw) != (stride+1)*h or not inflater.eof or inflater.unused_data:
        raise ValueError("PNG inflated length")
    pixels, previous = bytearray(), bytearray(stride)
    for y in range(h):
        off = y * (stride+1)
        f = raw[off]
        row = bytearray(raw[off+1:off+1+stride])
        if f > 4:
            raise ValueError("PNG filter")
        for x in range(stride):
            a = row[x-4] if x >= 4 else 0
            b = previous[x]
            c = previous[x-4] if x >= 4 else 0
            if f == 1: predictor = a
            elif f == 2: predictor = b
            elif f == 3: predictor = (a+b)//2
            elif f == 4:
                p = a+b-c
                pa, pb, pc = abs(p-a), abs(p-b), abs(p-c)
                predictor = a if pa <= pb and pa <= pc else b if pb <= pc else c
            else: predictor = 0
            row[x] = (row[x] + predictor) & 255
        pixels.extend(row)
        previous = row
    return w, h, pixels


def convert(source):
    data = Path(source).read_bytes()
    if hashlib.sha256(data).hexdigest() != SOURCE_SHA256:
        raise ValueError("approved Ciuki source SHA-256 mismatch")
    w, h, pixels = decode(data)
    out = bytearray()
    for y in range(SIZE):
        for x in range(SIZE):
            off = ((y*h//SIZE)*w + x*w//SIZE)*4
            r, g, b, a = pixels[off:off+4]
            rgb = [(v*a + bg*(255-a) + 127)//255 for v, bg in zip((r,g,b), BACKGROUND)]
            out.extend((rgb[2], rgb[1], rgb[0], 0))
    return bytes(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parents[2]
    parser.add_argument("--source", type=Path, default=root / "assets/brand/ciuki-logo.png")
    parser.add_argument("--out", type=Path, default=root / "build/apps/desktop/ciuki-portrait.xrgb")
    args = parser.parse_args()
    data = convert(args.source)
    sha = hashlib.sha256(data).hexdigest()
    if sha != ARRAY_SHA256:
        parser.error(f"converted portrait hash differs from recorded hash: {sha}")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(data)
    print(f"SHA256 {sha}  {args.out} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
