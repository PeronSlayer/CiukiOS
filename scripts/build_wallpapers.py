#!/usr/bin/env python3
"""Build CWP1 wallpaper tiles and a bounded CWC1 catalog.

PNG and BMP inputs are losslessly indexed at their native tile size (1..256
pixels per side, at most 256 distinct colors). Unsupported inputs fail clearly;
no image is silently resized, recolored or excluded. Owner-supplied Windows
artwork remains opt-in. Public builds contain original CC0 tiles.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import unicodedata
from pathlib import Path

from PIL import Image, UnidentifiedImageError

MAX_WALLPAPERS = 99
SUPPORTED_SUFFIXES = {".bmp", ".png"}
PUBLIC_SOURCE = Path(__file__).resolve().parents[1] / "assets/wallpapers/tiles"


def ascii_title(value: str) -> bytes:
    value = unicodedata.normalize("NFKD", value).encode("ascii", "ignore")
    value = bytes(c for c in value if 32 <= c <= 126).strip() or b"Wallpaper"
    return value[:30].ljust(31, b"\0")


def convert(source: Path) -> tuple[bytes, tuple[int, int]]:
    with Image.open(source) as original:
        if original.format not in {"BMP", "PNG"}:
            raise ValueError(f"{source}: only PNG and BMP image files are supported")
        if "A" in original.getbands() or "transparency" in original.info:
            if original.convert("RGBA").getchannel("A").getextrema() != (255, 255):
                raise ValueError(f"{source}: transparency is unsupported; flatten the image first")
        rgb = original.convert("RGB")
        width, height = rgb.size
        if not (1 <= width <= 256 and 1 <= height <= 256):
            raise ValueError(f"{source}: tile dimensions must be 1..256; no resizing is performed")
        palette: dict[tuple[int, int, int], int] = {}
        pixels = bytearray()
        source_pixels = rgb.tobytes()
        for offset in range(0, len(source_pixels), 3):
            color = tuple(source_pixels[offset:offset + 3])
            if color not in palette:
                if len(palette) == 256:
                    raise ValueError(f"{source}: more than 256 colors; cannot convert losslessly")
                palette[color] = len(palette)
            pixels.append(palette[color])
        colors = b"".join(bytes(color) for color in palette).ljust(256 * 3, b"\0")
        header = struct.pack("<4sHHHHI", b"CWP1", width, height, 256, 0, len(pixels))
        return header + colors + pixels, (width, height)


def read_catalog(data: bytes) -> list[tuple[str, bytes]]:
    if len(data) < 8:
        raise ValueError("Wallpaper catalog is truncated")
    magic, count, reserved = struct.unpack_from("<4sHH", data)
    if magic != b"CWC1" or reserved or count > MAX_WALLPAPERS or len(data) < 8+44*count:
        raise ValueError("Invalid wallpaper catalog")
    records = []
    for index in range(count):
        record = data[8+index*44:8+(index+1)*44]
        expected = f"WALL{index+1:02}.CWP"
        if record[:13].split(b"\0", 1)[0] != expected.encode():
            raise ValueError(f"Invalid catalog filename at slot {index+1}")
        title = record[13:].split(b"\0", 1)[0]
        if len(title) > 30 or any(c < 32 or c > 126 for c in title):
            raise ValueError(f"Invalid catalog title at slot {index+1}")
        records.append((expected, title.ljust(31, b"\0")))
    return records


def catalog_bytes(records: list[tuple[str, bytes]]) -> bytes:
    if len(records) > MAX_WALLPAPERS:
        raise ValueError(f"The wallpaper catalog supports at most {MAX_WALLPAPERS} tiles")
    data = struct.pack("<4sHH", b"CWC1", len(records), 0)
    data += b"".join(name.encode("ascii").ljust(13, b"\0") + title for name, title in records)
    return data.ljust((len(data)+511)//512*512, b"\0")


def discover(directory: Path) -> list[Path]:
    if not directory.is_dir():
        raise ValueError(f"Wallpaper source directory does not exist: {directory}")
    inputs = sorted((p for p in directory.iterdir() if p.is_file() and p.suffix.lower() in SUPPORTED_SUFFIXES),
                    key=lambda p: p.name.casefold())
    if not inputs:
        raise ValueError(f"No PNG or BMP inputs found in {directory}")
    return inputs


def build(output: Path, sources: list[Path], *, personal: bool) -> dict:
    if len(sources) > MAX_WALLPAPERS:
        raise ValueError(f"The wallpaper catalog supports at most {MAX_WALLPAPERS} tiles")
    # Validate every input before writing anything: a bad later input must not
    # leave a half-updated catalog or overwrite an earlier working texture.
    converted = [(source, *convert(source)) for source in sources]
    output.mkdir(parents=True, exist_ok=True)
    records = []
    manifest = {"format": "CiukiOS wallpaper CWP1/CWC1", "personal_inputs_enabled": personal,
                "conversion": "lossless RGB palette and unchanged native tile dimensions",
                "wallpapers": []}
    for index, (source, payload, dimensions) in enumerate(converted, 1):
        filename = f"WALL{index:02}.CWP"
        (output / filename).write_bytes(payload)
        records.append((filename, ascii_title(source.stem)))
        manifest["wallpapers"].append({"source": str(source),
            "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "filename": filename, "sha256": hashlib.sha256(payload).hexdigest(),
            "dimensions": dimensions,
            "license": "not supplied; owner-provided image" if personal else "CC0-1.0"})
    (output / "WALLS.DAT").write_bytes(catalog_bytes(records))
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    for index in range(len(sources)+1, MAX_WALLPAPERS+1):
        (output / f"WALL{index:02}.CWP").unlink(missing_ok=True)
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("build/wallpapers"))
    parser.add_argument("--source", type=Path, default=Path("third_party/win_bg"))
    parser.add_argument("--include-user-wallpapers", action="store_true",
                        help="include owner-provided PNG/BMP tiles for a personal build")
    args = parser.parse_args()
    try:
        sources = discover(args.source if args.include_user_wallpapers else PUBLIC_SOURCE)
        build(args.output, sources, personal=args.include_user_wallpapers)
    except (ValueError, OSError, UnidentifiedImageError) as error:
        raise SystemExit(str(error)) from error
    print(f"[wallpapers] {len(sources)} tiles; catalog {args.output / 'WALLS.DAT'}")


if __name__ == "__main__":
    main()
