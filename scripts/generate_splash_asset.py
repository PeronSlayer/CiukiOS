#!/usr/bin/env python3
"""Generate a lightweight 80x50 indexed splash asset for CiukiOS full profile.

Output format:
- 96 bytes palette (32 RGB triplets)
- 4000 bytes indexed pixels (80x50)
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print(
        "[splash-gen] ERROR: Pillow is required (python3 -m pip install Pillow)",
        file=sys.stderr,
    )
    sys.exit(3)


WIDTH = 80
HEIGHT = 50
PALETTE_COLORS = 32
PALETTE_BYTES = PALETTE_COLORS * 3
PIXEL_BYTES = WIDTH * HEIGHT


def _enum_value(namespace: object, attr_name: str, default: int) -> int:
    return int(getattr(namespace, attr_name, default))


def generate_splash_asset(source: Path, output: Path) -> int:
    if not source.is_file():
        print(f"[splash-gen] ERROR: source image not found: {source}", file=sys.stderr)
        return 2

    try:
        image = Image.open(source).convert("RGBA")

        # Make alpha handling explicit and deterministic before palette reduction.
        opaque_bg = Image.new("RGBA", image.size, (0, 0, 0, 255))
        opaque_bg.alpha_composite(image)
        rgb = opaque_bg.convert("RGB")

        resampling = getattr(Image, "Resampling", Image)
        resample_lanczos = _enum_value(resampling, "LANCZOS", getattr(Image, "LANCZOS", 1))
        resized = rgb.resize((WIDTH, HEIGHT), resample=resample_lanczos)

        quantize = getattr(Image, "Quantize", Image)
        dither = getattr(Image, "Dither", Image)
        quantize_mediancut = _enum_value(quantize, "MEDIANCUT", getattr(Image, "MEDIANCUT", 0))
        dither_fs = _enum_value(dither, "FLOYDSTEINBERG", getattr(Image, "FLOYDSTEINBERG", 1))

        indexed = resized.quantize(colors=PALETTE_COLORS, method=quantize_mediancut, dither=dither_fs)

        raw_palette = indexed.getpalette() or []
        palette = bytes((raw_palette + [0] * PALETTE_BYTES)[:PALETTE_BYTES])
        pixels = indexed.tobytes()

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