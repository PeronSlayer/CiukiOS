#!/usr/bin/env python3
"""Build CWP1 tiles, owner CWP2 photos and a bounded CWC1 catalog.

PNG and BMP inputs are losslessly indexed at their native tile size (1..256
pixels per side, at most 256 distinct colors). Unsupported inputs fail clearly;
no image is silently resized, recolored or excluded. Owner-supplied Windows
artwork remains opt-in. The three project-owned Ciuki photos are always
appended after the selected CWP1 set.
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
WP_FILL = 0
WP_FIT = 1
SUPPORTED_SUFFIXES = {".bmp", ".png"}
PUBLIC_SOURCE = Path(__file__).resolve().parents[1] / "assets/wallpapers/tiles"
PHOTO_SOURCE = Path(__file__).resolve().parents[1] / "misc/ciukios_bg"
PHOTO_NAMES = ("Ciuk1.png", "Ciuk2.png", "Ciuk3.png")
PHOTO_MAX_WIDTH = 2048
PHOTO_MAX_HEIGHT = 1536


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


def convert_photo(source: Path) -> tuple[bytes, tuple[int, int], int]:
    """Encode a project photo as exact row-major RGB888 without resampling."""
    with Image.open(source) as original:
        if original.format != "PNG" or original.mode != "RGB":
            raise ValueError(f"{source}: project photos must be RGB PNGs")
        width, height = original.size
        if not (1 <= width <= PHOTO_MAX_WIDTH and 1 <= height <= PHOTO_MAX_HEIGHT):
            raise ValueError(f"{source}: photo dimensions must be 1..{PHOTO_MAX_WIDTH} by "
                             f"1..{PHOTO_MAX_HEIGHT}; no resizing is performed")
        rgb = original.tobytes()
    if len(rgb) != width * height * 3:
        raise ValueError(f"{source}: decoded RGB payload has an unexpected size")
    row_bytes = width * 3
    stride = (row_bytes + 1) & ~1
    pixels = rgb if stride == row_bytes else b"".join(
        rgb[start:start + row_bytes] + b"\0" for start in range(0, len(rgb), row_bytes))
    payload_bytes = stride * height
    header = struct.pack("<4sHHHBBI", b"CWP2", width, height, stride, 1, 0, payload_bytes)
    payload = header + pixels
    if len(payload) != 16 + payload_bytes:
        raise ValueError(f"{source}: encoded CWP2 size is invalid")
    return payload, (width, height), len(pixels)


def select_photos(directory: Path) -> tuple[list[Path], bool]:
    originals = [directory / name for name in PHOTO_NAMES]
    missing = [path for path in originals if not path.is_file()]
    if missing:
        raise ValueError("Missing required project photo(s): " + ", ".join(map(str, missing)))
    adaptive = [directory / "adaptive" / name for name in PHOTO_NAMES]
    present = [path.is_file() for path in adaptive]
    if any(present) and not all(present):
        raise ValueError("Incomplete adaptive photo set: " + ", ".join(
            str(path) for path, exists in zip(adaptive, present) if not exists))
    return (adaptive, True) if all(present) else (originals, False)


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


def build(output: Path, sources: list[Path], *, personal: bool,
          photo_sources: list[Path] | None = None, adaptive_photos: bool = False) -> dict:
    photo_sources = photo_sources or []
    if len(sources) + len(photo_sources) > MAX_WALLPAPERS:
        raise ValueError(f"The wallpaper catalog supports at most {MAX_WALLPAPERS} entries")
    # Validate every input before writing anything: a bad later input must not
    # leave a half-updated catalog or overwrite an earlier working texture.
    converted = [(source, *convert(source)) for source in sources]
    converted_photos = [(source, *convert_photo(source)) for source in photo_sources]
    original_inputs = {}
    if adaptive_photos:
        for source in photo_sources:
            original = source.parent.parent / source.name
            original_inputs[source] = (original, hashlib.sha256(original.read_bytes()).hexdigest())
    output.mkdir(parents=True, exist_ok=True)
    records = []
    manifest = {"format": "CiukiOS wallpaper CWP1/CWP2/CWC1", "personal_inputs_enabled": personal,
                "adaptive_photos_enabled": adaptive_photos,
                "conversion": "CWP1 lossless RGB palette; CWP2 exact native RGB888 PNG samples",
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
    for index, (source, payload, dimensions, rgb_bytes) in enumerate(
            converted_photos, len(records) + 1):
        filename = f"WALL{index:02}.CWP"
        (output / filename).write_bytes(payload)
        records.append((filename, ascii_title(source.stem)))
        manifest["wallpapers"].append({
            "source": str(source), "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "filename": filename, "sha256": hashlib.sha256(payload).hexdigest(),
            "format": "CWP2", "dimensions": dimensions,
            "stride": struct.unpack_from("<H", payload, 8)[0],
            "header_bytes": 16, "rgb_payload_bytes": rgb_bytes,
            "file_bytes": len(payload), "rgb_exact_match": True,
            "license": "owner-provided CiukiOS project asset"})
        if adaptive_photos:
            original, original_hash = original_inputs[source]
            manifest["wallpapers"][-1].update({
                "original_source": str(original),
                "original_source_sha256": original_hash,
                "adaptation": "extended scenery; centered aspect-preserving Fill"})
    photo_default = next((i for i, (_, title) in enumerate(records, 1)
                          if title.split(b"\0", 1)[0] == b"Ciuk1"), None)
    if photo_default is not None:
        default_style = WP_FILL if adaptive_photos else WP_FIT
        (output / "WALL.CFG").write_bytes(bytes([photo_default, default_style]))
        manifest["default_selection"] = {"file": "WALL.CFG", "entry": photo_default,
                                         "title": "Ciuk1", "position": "Fill" if adaptive_photos else "Fit",
                                         "position_id": default_style}
    else:
        (output / "WALL.CFG").unlink(missing_ok=True)
    (output / "WALLS.DAT").write_bytes(catalog_bytes(records))
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    for index in range(len(records)+1, MAX_WALLPAPERS+1):
        (output / f"WALL{index:02}.CWP").unlink(missing_ok=True)
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("build/wallpapers"))
    parser.add_argument("--source", type=Path, default=Path("third_party/win_bg"))
    parser.add_argument("--photo-source", type=Path, default=PHOTO_SOURCE,
                        help="directory containing Ciuk1.png, Ciuk2.png and Ciuk3.png")
    parser.add_argument("--include-user-wallpapers", action="store_true",
                        help="include owner-provided PNG/BMP tiles for a personal build")
    args = parser.parse_args()
    try:
        sources = discover(args.source if args.include_user_wallpapers else PUBLIC_SOURCE)
        photos, adaptive = select_photos(args.photo_source)
        manifest = build(args.output, sources, personal=args.include_user_wallpapers,
                         photo_sources=photos, adaptive_photos=adaptive)
    except (ValueError, OSError, UnidentifiedImageError) as error:
        raise SystemExit(str(error)) from error
    print(f"[wallpapers] {len(manifest['wallpapers'])} entries; "
          f"default {manifest.get('default_selection', {}).get('entry')}; "
          f"catalog {args.output / 'WALLS.DAT'}")


if __name__ == "__main__":
    main()
