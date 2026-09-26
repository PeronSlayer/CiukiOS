#!/usr/bin/env python3
"""Extract a bounded Doom v1.9 timedemo from an IWAD without shipping it."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


def wad_lump(data: bytes, wanted: str) -> bytes:
    if len(data) < 12 or data[:4] not in (b"IWAD", b"PWAD"):
        raise ValueError("input is not a Doom WAD")
    count, directory = struct.unpack_from("<II", data, 4)
    if directory > len(data) or count > (len(data) - directory) // 16:
        raise ValueError("invalid WAD directory")
    for index in range(count):
        offset = directory + index * 16
        filepos, size = struct.unpack_from("<II", data, offset)
        name = data[offset + 8 : offset + 16].rstrip(b"\0").decode("ascii")
        if name.upper() == wanted.upper():
            if filepos > len(data) or size > len(data) - filepos:
                raise ValueError(f"invalid {wanted} lump bounds")
            return data[filepos : filepos + size]
    raise ValueError(f"WAD lump not found: {wanted}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("wad", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--lump", default="DEMO1")
    parser.add_argument("--tics", type=int, default=350)
    args = parser.parse_args()

    if not 1 <= args.tics <= 100000:
        parser.error("--tics must be between 1 and 100000")
    demo = wad_lump(args.wad.read_bytes(), args.lump)
    header_size = 13
    needed = header_size + args.tics * 4
    if len(demo) < needed or demo[0] != 109:
        raise SystemExit("DEMO1 is not a sufficiently long Doom v1.9 demo")
    args.output.write_bytes(demo[:needed] + b"\x80")
    print(f"[doom-demo] wrote {args.output} tics={args.tics} bytes={needed + 1}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
