#!/usr/bin/env python3
"""Generate deterministic image fixtures and a manifest for webimg_host_asan."""
from __future__ import annotations

import argparse
import random
import struct
import zlib
from pathlib import Path

from PIL import Image


def rgb_pattern(width: int, height: int) -> bytes:
    return bytes(c for y in range(height) for x in range(width) for c in (
        (x * 17 + y * 3) & 255, (x * 5 + y * 19) & 255,
        (x * 11 + y * 7 + 31) & 255))


def chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)


def png_first_deflate_type(payload: bytes) -> int:
    pos = 8
    while pos + 12 <= len(payload):
        length = int.from_bytes(payload[pos:pos + 4], "big")
        if payload[pos + 4:pos + 8] == b"IDAT":
            return (payload[pos + 10] >> 1) & 3  # zlib header is two bytes
        pos += 12 + length
    raise ValueError("PNG has no IDAT chunk")


def gif_has_interlaced_image(payload: bytes) -> bool:
    if payload[:6] not in (b"GIF87a", b"GIF89a") or len(payload) < 13:
        return False
    pos = 13
    screen_flags = payload[10]
    if screen_flags & 0x80:
        pos += 3 * (1 << ((screen_flags & 7) + 1))
    while pos < len(payload):
        marker = payload[pos]
        pos += 1
        if marker == 0x3B:
            return False
        if marker == 0x21:  # extension: label followed by size-prefixed subblocks
            if pos >= len(payload):
                return False
            pos += 1
            while pos < len(payload):
                length = payload[pos]
                pos += 1
                if length == 0:
                    break
                pos += length
        elif marker == 0x2C:  # image descriptor; its packed byte is byte 9
            if pos + 9 > len(payload):
                return False
            flags = payload[pos + 8]
            return bool(flags & 0x40)
        else:
            return False
    return False


def filter_row(row: bytes, previous: bytes, kind: int, bpp: int) -> bytes:
    out = bytearray(len(row))
    for i, value in enumerate(row):
        left = row[i - bpp] if i >= bpp else 0
        up = previous[i] if previous else 0
        upper_left = previous[i - bpp] if previous and i >= bpp else 0
        if kind == 0:
            predictor = 0
        elif kind == 1:
            predictor = left
        elif kind == 2:
            predictor = up
        elif kind == 3:
            predictor = (left + up) // 2
        else:
            p = left + up - upper_left
            pa, pb, pc = abs(p - left), abs(p - up), abs(p - upper_left)
            predictor = left if pa <= pb and pa <= pc else up if pb <= pc else upper_left
        out[i] = (value - predictor) & 255
    return bytes(out)


def png_bytes(width: int, height: int, rows: list[bytes], *, color_type: int = 2,
              bpp: int = 3, depth: int = 8, filters: list[int] | None = None,
              palette: bytes | None = None, transparency: bytes | None = None,
              strategy: int = zlib.Z_DEFAULT_STRATEGY, level: int = 6,
              split_idat: bool = False) -> bytes:
    previous = b""
    packed = bytearray()
    filters = filters or [0] * height
    for y, row in enumerate(rows):
        kind = filters[y % len(filters)]
        packed.append(kind)
        packed.extend(filter_row(row, previous, kind, bpp))
        previous = row
    compressor = zlib.compressobj(level, zlib.DEFLATED, 15, 8, strategy)
    compressed = compressor.compress(bytes(packed)) + compressor.flush()
    ihdr = struct.pack(">IIBBBBB", width, height, depth, color_type, 0, 0, 0)
    result = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
    if palette is not None:
        result += chunk(b"PLTE", palette)
    if transparency is not None:
        result += chunk(b"tRNS", transparency)
    if split_idat and len(compressed) > 2:
        cut = len(compressed) // 2
        result += chunk(b"IDAT", compressed[:cut]) + chunk(b"IDAT", compressed[cut:])
    else:
        result += chunk(b"IDAT", compressed)
    return result + chunk(b"IEND", b"")


def write_rgb(outdir: Path, name: str, width: int, height: int,
              pixels: bytes, tolerance: int = 0) -> Path:
    path = outdir / f"{name}.rgb"
    path.write_bytes(pixels)
    return path


def add_image(records: list[str], image: Path, ref: Path,
              width: int, height: int, tolerance: int = 0) -> None:
    records.append(f"image {image.resolve()} {ref.resolve()} {width} {height} {tolerance}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("outdir", type=Path)
    args = parser.parse_args()
    out = args.outdir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    records = ["# kind image reference width height tolerance; error rows use '-' fields"]

    w, h = 19, 13
    rgb = rgb_pattern(w, h)
    rgb_image = Image.frombytes("RGB", (w, h), rgb)

    # All five PNG row filters appear in one image; IDAT is split across chunks.
    png = out / "filters-split.png"
    rows = [rgb[y * w * 3:(y + 1) * w * 3] for y in range(h)]
    png.write_bytes(png_bytes(w, h, rows, filters=[0, 1, 2, 3, 4], split_idat=True))
    add_image(records, png, write_rgb(out, "filters-split-reference", w, h, rgb), w, h)

    # DEFLATE stored and fixed-Huffman block modes exercise separate decoder paths.
    fixed_rgb = bytes((42, 96, 154)) * (w * h)
    fixed_rows = [fixed_rgb[y * w * 3:(y + 1) * w * 3] for y in range(h)]
    for name, strategy, level, source_rows, reference in (
            ("stored", zlib.Z_DEFAULT_STRATEGY, 0, rows, rgb),
            ("fixed", zlib.Z_FIXED, 6, fixed_rows, fixed_rgb)):
        image = out / f"deflate-{name}.png"
        encoded = png_bytes(w, h, source_rows, strategy=strategy, level=level)
        expected_type = 0 if name == "stored" else 1
        if png_first_deflate_type(encoded) != expected_type:
            raise ValueError(f"zlib did not generate requested {name} DEFLATE block")
        image.write_bytes(encoded)
        add_image(records, image, write_rgb(out, f"deflate-{name}-reference", w, h, reference), w, h)

    # Default zlib strategy on a large, skewed alphabet gives the dynamic-tree path.
    dw, dh = 192, 64
    rng = random.Random(20261004)
    alphabet = [(0, 0, 0), (255, 255, 255), (16, 80, 160), (220, 30, 90)]
    dynamic = bytes(c for _ in range(dw * dh)
                    for c in (alphabet[0] if rng.randrange(10) < 8 else rng.choice(alphabet[1:])))
    dynamic_rows = [dynamic[y * dw * 3:(y + 1) * dw * 3] for y in range(dh)]
    image = out / "deflate-default.png"
    encoded = png_bytes(dw, dh, dynamic_rows, level=9)
    if png_first_deflate_type(encoded) != 2:
        raise ValueError("zlib did not generate a dynamic-Huffman DEFLATE block")
    image.write_bytes(encoded)
    add_image(records, image, write_rgb(out, "deflate-default-reference", dw, dh, dynamic), dw, dh)

    # Indexed alpha and RGBA exercise palette lookup and compositing over white.
    palette = bytes((240, 20, 40, 20, 180, 60, 20, 40, 230, 0, 0, 0))
    alpha = bytes((255, 160, 0, 255))
    index_rows = [bytes((x + y) % 4 for x in range(w)) for y in range(h)]
    palette_ref = bytearray()
    colors = [tuple(palette[i:i + 3]) for i in range(0, len(palette), 3)]
    for y in range(h):
        for x in range(w):
            i = index_rows[y][x]
            a = alpha[i]
            palette_ref.extend((colors[i][c] * a + 255 * (255 - a) + 127) // 255 for c in range(3))
    image = out / "palette-alpha.png"
    image.write_bytes(png_bytes(w, h, index_rows, color_type=3, bpp=1, palette=palette,
                                transparency=alpha))
    add_image(records, image, write_rgb(out, "palette-alpha-reference", w, h, bytes(palette_ref)), w, h)

    rgba_rows, rgba_ref = [], bytearray()
    for y in range(h):
        row = bytearray()
        for x in range(w):
            r, g, b = ((x * 17 + y * 3) & 255, (x * 5 + y * 19) & 255,
                       (x * 11 + y * 7 + 31) & 255)
            a = (x * 29 + y * 13) & 255
            row.extend((r, g, b, a))
            rgba_ref.extend(((r * a + 255 * (255 - a) + 127) // 255,
                             (g * a + 255 * (255 - a) + 127) // 255,
                             (b * a + 255 * (255 - a) + 127) // 255))
        rgba_rows.append(bytes(row))
    image = out / "rgba.png"
    image.write_bytes(png_bytes(w, h, rgba_rows, color_type=6, bpp=4))
    add_image(records, image, write_rgb(out, "rgba-reference", w, h, bytes(rgba_ref)), w, h)

    # JPEG with non-MCU-aligned dimensions and restart markers, plus progressive rejection.
    jw, jh = 37, 19
    jpeg_rgb = Image.frombytes("RGB", (jw, jh), rgb_pattern(jw, jh))
    image = out / "jpeg-restart-edge.jpg"
    jpeg_rgb.save(image, format="JPEG", quality=93, subsampling=0,
                  restart_marker_blocks=2, optimize=False)
    jpeg_data = image.read_bytes()
    if not any(bytes((0xff, 0xd0 + marker)) in jpeg_data for marker in range(8)):
        raise ValueError("Pillow did not emit JPEG restart markers")
    with Image.open(image) as decoded:
        ref = write_rgb(out, "jpeg-restart-edge-reference", jw, jh, decoded.convert("RGB").tobytes())
    add_image(records, image, ref, jw, jh, 20)
    image = out / "jpeg-progressive.jpg"
    jpeg_rgb.save(image, format="JPEG", quality=90, progressive=True)
    if b"\xff\xc2" not in image.read_bytes():
        raise ValueError("Pillow did not emit progressive JPEG SOF2")
    records.append(f"open-error {image.resolve()} - 0 0 0")

    # Ordinary and interlaced transparent GIFs, expected transparency composited on white.
    ordinary = out / "fixture-gif.gif"
    rgb_image.quantize(colors=32, method=Image.Quantize.MEDIANCUT).save(ordinary, format="GIF")
    with Image.open(ordinary) as decoded:
        ref = write_rgb(out, "fixture-gif-reference", w, h, decoded.convert("RGB").tobytes())
    add_image(records, ordinary, ref, w, h)
    gw, gh = 19, 17  # both dimensions >= 16 so the encoder preserves interlace
    gif = Image.new("P", (gw, gh))
    colors = [(240, 20, 40), (20, 180, 60), (20, 40, 230), (0, 0, 0)]
    gif.putpalette([c for color in colors for c in color] + [0] * (768 - 12))
    gif.putdata([(x + 2 * y) % 4 for y in range(gh) for x in range(gw)])
    image = out / "gif-interlaced-transparent.gif"
    gif.save(image, format="GIF", interlace=True, transparency=3)
    if not gif_has_interlaced_image(image.read_bytes()):
        raise ValueError("Pillow did not emit an interlaced GIF")
    expected = bytearray()
    for y in range(gh):
        for x in range(gw):
            color = colors[(x + 2 * y) % 4]
            expected.extend((255, 255, 255) if (x + 2 * y) % 4 == 3 else color)
    add_image(records, image, write_rgb(out, "gif-interlaced-reference", gw, gh,
                                        bytes(expected)), gw, gh)

    # Malformed input and bounded-dimension failures.
    png_bytes_good = png.read_bytes()
    bad_crc = bytearray(png_bytes_good)
    pos = 8
    while pos + 12 <= len(bad_crc):
        length = int.from_bytes(bad_crc[pos:pos + 4], "big")
        if bad_crc[pos + 4:pos + 8] == b"IDAT":
            bad_crc[pos + 11 + length] ^= 1
            break
        pos += 12 + length
    bad = out / "bad-crc.png"
    bad.write_bytes(bad_crc)
    records.append(f"stream-error {bad.resolve()} - 0 0 0")
    pos = 8
    while pos + 12 <= len(png_bytes_good):
        length = int.from_bytes(png_bytes_good[pos:pos + 4], "big")
        if png_bytes_good[pos + 4:pos + 8] == b"IDAT":
            cut = pos + 8 + max(2, length // 2)
            break
        pos += 12 + length
    truncated = out / "truncated.png"
    truncated.write_bytes(png_bytes_good[:cut])
    records.append(f"stream-error {truncated.resolve()} - 0 0 0")
    oversized = out / "oversized.png"
    Image.new("RGB", (2049, 1), (1, 2, 3)).save(oversized, format="PNG")
    records.append(f"open-error {oversized.resolve()} - 0 0 0")

    (out / "manifest.txt").write_text("\n".join(records) + "\n", encoding="ascii")


if __name__ == "__main__":
    main()
