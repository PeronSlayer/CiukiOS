#!/usr/bin/env python3
"""Reject screenshots that are not a restored VGA 80x25 shell screen."""

from __future__ import annotations

import argparse
from pathlib import Path


def read_token(data: bytes, position: int) -> tuple[bytes, int]:
    while position < len(data):
        if data[position] in b" \t\r\n":
            position += 1
            continue
        if data[position] == ord("#"):
            while position < len(data) and data[position] not in b"\r\n":
                position += 1
            continue
        break
    start = position
    while position < len(data) and data[position] not in b" \t\r\n#":
        position += 1
    if start == position:
        raise ValueError("missing PPM token")
    return data[start:position], position


def load_ppm(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    position = 0
    values: list[bytes] = []
    for _ in range(4):
        value, position = read_token(data, position)
        values.append(value)
    if values[0] != b"P6" or values[3] != b"255":
        raise ValueError("expected an 8-bit P6 PPM")
    width, height = int(values[1]), int(values[2])
    if position >= len(data) or data[position] not in b" \t\r\n":
        raise ValueError("missing raster separator")
    pixels = data[position + 1 : position + 1 + width * height * 3]
    if len(pixels) != width * height * 3:
        raise ValueError("truncated raster")
    return width, height, pixels


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("ppm", type=Path)
    parser.add_argument("--label", default="textmode-screen")
    args = parser.parse_args()

    try:
        width, height, pixels = load_ppm(args.ppm)
    except (OSError, ValueError) as exc:
        print(f"[{args.label}] FAIL {exc}")
        return 1

    # QEMU's VGA BIOS renders mode 03h as a 720x400 raster (9x16 glyph cell).
    # Requiring the exact raster distinguishes a real text-mode restore from a
    # shell prompt painted over a stale 320x200 graphics mode.
    if (width, height) != (720, 400):
        print(f"[{args.label}] FAIL expected VGA mode-03 raster 720x400, got {width}x{height}")
        return 1

    def brightness_at(x: int, y: int) -> int:
        offset = (y * width + x) * 3
        return max(pixels[offset : offset + 3])

    title_samples = [brightness_at(x, y) for y in range(16) for x in range(width)]
    body_samples = [
        brightness_at(x, y)
        for y in range(16, height)
        for x in range(0, width, 4)
    ]
    title_lit = sum(value >= 80 for value in title_samples) / len(title_samples)
    body_dark = sum(value < 30 for value in body_samples) / len(body_samples)

    if title_lit < 0.60:
        print(f"[{args.label}] FAIL title bar is not restored: lit_ratio={title_lit:.3f}")
        return 1
    if body_dark < 0.75:
        print(f"[{args.label}] FAIL stale graphics remain below title bar: dark_ratio={body_dark:.3f}")
        return 1

    print(
        f"[{args.label}] PASS VGA mode 03h 720x400 "
        f"title_lit_ratio={title_lit:.3f} body_dark_ratio={body_dark:.3f}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
