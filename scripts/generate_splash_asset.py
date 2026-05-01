#!/usr/bin/env python3
"""Generate a lightweight 160x100 indexed splash asset for CiukiOS full profile.

Output format:
- 768 bytes palette (256 RGB triplets)
- 16000 bytes pixels (160x100 @ 8bpp, 1 pixel per byte)
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    from PIL import Image, ImageEnhance, ImageFilter, ImageOps
except ImportError:
    print(
        "[splash-gen] ERROR: Pillow is required (python3 -m pip install Pillow)",
        file=sys.stderr,
    )
    sys.exit(3)


WIDTH = 160
HEIGHT = 100
PALETTE_COLORS = 256
PALETTE_BYTES = PALETTE_COLORS * 3
PIXEL_BYTES = WIDTH * HEIGHT
PIXEL_INDEX_BYTES = WIDTH * HEIGHT


def _enum_value(namespace: object, attr_name: str, default: int) -> int:
    return int(getattr(namespace, attr_name, default))


def _prepare_source_image(image: Image.Image) -> Image.Image:
    # Normalize alpha and compose over black for deterministic palette reduction.
    opaque_bg = Image.new("RGBA", image.size, (0, 0, 0, 255))
    opaque_bg.alpha_composite(image.convert("RGBA"))
    rgb = opaque_bg.convert("RGB")

    src_w, src_h = rgb.size
    target_ratio = WIDTH / HEIGHT
    if src_h == 0:
        raise ValueError("invalid source image height")

    src_ratio = src_w / src_h
    if src_ratio > target_ratio:
        crop_w = int(src_h * target_ratio)
        left = max(0, (src_w - crop_w) // 2)
        crop_box = (left, 0, left + crop_w, src_h)
    else:
        crop_h = max(1, int(src_w / target_ratio))
        top = max(0, (src_h - crop_h) // 3)
        if top + crop_h > src_h:
            top = src_h - crop_h
        crop_box = (0, top, src_w, top + crop_h)

    cropped = rgb.crop(crop_box)

    resampling = getattr(Image, "Resampling", Image)
    resample_lanczos = _enum_value(resampling, "LANCZOS", getattr(Image, "LANCZOS", 1))

    pre_size = (WIDTH * 2, HEIGHT * 2)
    pre_scaled = cropped.resize(pre_size, resample=resample_lanczos)
    enhanced = ImageOps.autocontrast(pre_scaled, cutoff=1)
    enhanced = ImageEnhance.Contrast(enhanced).enhance(1.14)
    enhanced = ImageEnhance.Color(enhanced).enhance(1.08)
    enhanced = enhanced.filter(ImageFilter.UnsharpMask(radius=1.2, percent=155, threshold=2))
    return enhanced.resize((WIDTH, HEIGHT), resample=resample_lanczos)


def generate_splash_asset(source: Path, output: Path) -> int:
    if not source.is_file():
        print(f"[splash-gen] ERROR: source image not found: {source}", file=sys.stderr)
        return 2

    try:
        image = Image.open(source)
        prepared = _prepare_source_image(image)

        quantize = getattr(Image, "Quantize", Image)
        dither = getattr(Image, "Dither", Image)
        quantize_mediancut = _enum_value(quantize, "MEDIANCUT", getattr(Image, "MEDIANCUT", 0))
        dither_fs = _enum_value(dither, "FLOYDSTEINBERG", getattr(Image, "FLOYDSTEINBERG", 1))

        indexed = prepared.quantize(colors=PALETTE_COLORS - 3, method=quantize_mediancut, dither=dither_fs)

        raw_palette = indexed.getpalette() or []
        palette_list = (raw_palette + [0] * PALETTE_BYTES)[:PALETTE_BYTES]
        # Reserve top 3 palette entries for splash UI chrome colors.
        palette_list[253 * 3 : 253 * 3 + 3] = [48, 96, 208]   # blue banner
        palette_list[254 * 3 : 254 * 3 + 3] = [76, 84, 104] # gray inset
        palette_list[255 * 3 : 255 * 3 + 3] = [146, 88, 186]  # purple blocks
        palette = bytes(palette_list)
        indexed_pixels = indexed.tobytes()

        if any(px >= 253 for px in indexed_pixels):
            print(
                "[splash-gen] ERROR: reserved palette entries 253..255 were used by image quantization",
                file=sys.stderr,
            )
            return 4

        if len(indexed_pixels) != PIXEL_INDEX_BYTES:
            print(
                f"[splash-gen] ERROR: indexed pixel payload is {len(indexed_pixels)} bytes (expected {PIXEL_INDEX_BYTES})",
                file=sys.stderr,
            )
            return 4

        pixels = indexed_pixels

        if len(pixels) != PIXEL_BYTES:
            print(
                f"[splash-gen] ERROR: pixel payload is {len(pixels)} bytes (expected {PIXEL_BYTES})",
                file=sys.stderr,
            )
            return 4

        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(palette + pixels)

        expected_total = PALETTE_BYTES + PIXEL_BYTES
        actual_total = output.stat().st_size
        if actual_total != expected_total:
            print(
                f"[splash-gen] ERROR: output size is {actual_total} bytes (expected {expected_total})",
                file=sys.stderr,
            )
            return 5
    except Exception as exc:  # pragma: no cover - shell integration path
        print(f"[splash-gen] ERROR: failed to generate asset: {exc}", file=sys.stderr)
        return 1

    print(f"[splash-gen] generated {output} ({actual_total} bytes)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate CiukiOS SPLASH.BIN asset")
    parser.add_argument("source", help="Input splash PNG")
    parser.add_argument("output", help="Output SPLASH.BIN path")
    args = parser.parse_args()

    return generate_splash_asset(Path(args.source), Path(args.output))


if __name__ == "__main__":
    raise SystemExit(main())