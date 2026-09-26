#!/usr/bin/env python3
"""Compile the approved Ciuki portrait into native UI sprites; never redraw it.

The full approved PNG stays byte-identical. Conversion happens at build time:
fixed-size RGBA resampling, a shared 64-colour palette, and transparent pixels.
The OS reads embedded byte indices, not PNG files. --check verifies recorded
hashes without importing an image library or regenerating approved artwork.
"""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'assets/brand/ciuki-logo.png'
OUTPUT = ROOT / 'assets/brand/native'
PALETTE = ROOT / 'src/com/ciuki_logo_palette.inc'
SIZES = (16, 24, 64)
COLORS = 64
# Existing shared UI palette is intentionally unchanged.
UI_DAC = ((9,10,12),(13,18,30),(18,32,23),(13,23,24),
          (37,15,16),(31,24,37),(37,29,15),(51,52,53),
          (28,30,34),(27,36,48),(24,41,38),(39,49,48),
          (49,25,23),(43,34,46),(57,47,27),(61,61,60))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check():
    manifest = json.loads((OUTPUT / 'manifest.json').read_text())
    assert manifest['source_sha256'] == digest(SOURCE), 'Approved logo changed; rebuild native assets explicitly'
    for name, expected in manifest['files'].items():
        assert digest(ROOT / name) == expected, f'Stale or modified logo asset: {name}'
    print('[ciuki-logo] approved source and compiled assets match')


def generate():
    from PIL import Image, __version__ as pillow_version
    OUTPUT.mkdir(parents=True, exist_ok=True)
    original = Image.open(SOURCE).convert('RGBA')
    assert original.size[0] == original.size[1], 'Approved master must retain its square canvas'
    # Preserve the complete approved canvas and pose, with no crop or redraw.
    resized = {size: original.resize((size, size), Image.Resampling.LANCZOS)
               for size in SIZES}
    def pixels(image):
        return getattr(image, 'get_flattened_data', image.getdata)()
    opaque = [pixel[:3] for image in resized.values()
              for pixel in pixels(image) if pixel[3] >= 128]
    sample = Image.new('RGB', (len(opaque), 1))
    sample.putdata(opaque)
    quantized = sample.quantize(colors=COLORS, method=Image.Quantize.MEDIANCUT,
                               dither=Image.Dither.NONE)
    raw_palette = quantized.getpalette()[:COLORS * 3]
    dac = [tuple(round(channel * 63 / 255) for channel in raw_palette[i:i+3])
           for i in range(0, len(raw_palette), 3)]
    assert len(dac) == COLORS
    rgb = [tuple(round(channel * 255 / 63) for channel in color) for color in dac]
    fallback = bytes(min(range(16), key=lambda i: sum((color[c]-UI_DAC[i][c])**2 for c in range(3)))
                     for color in dac)
    files = []
    for size, image in resized.items():
        data = bytearray()
        preview = []
        for red, green, blue, alpha in pixels(image):
            if alpha < 128:
                data.append(255)
                preview.append((0, 0, 0, 0))
                continue
            pixel = (red, green, blue)
            index = min(range(COLORS), key=lambda i: sum((pixel[c]-rgb[i][c])**2 for c in range(3)))
            data.append(index + 16)
            preview.append((*rgb[index], 255))
        path = OUTPUT / f'ciuki-{size}.bin'
        path.write_bytes(data)
        files.append(path)
        # Reviewable rendering of the exact runtime indices, not a separate design.
        visual = Image.new('RGBA', (size, size))
        visual.putdata(preview)
        path = OUTPUT / f'ciuki-{size}.png'
        visual.save(path)
        files.append(path)
    path = OUTPUT / 'fallback.bin'
    path.write_bytes(fallback)
    files.append(path)
    lines = ['; Generated from the approved Ciuki portrait by scripts/build_ciuki_logo.py.',
             f'; Source SHA-256: {digest(SOURCE)}',
             '%ifndef CIUKI_LOGO_PALETTE_INCLUDED', '%define CIUKI_LOGO_PALETTE_INCLUDED 1',
             f'%define CIUKI_LOGO_COLOR_COUNT {COLORS}', '%macro CIUKI_LOGO_PALETTE 0']
    lines += ['    db ' + ','.join(map(str, color)) for color in dac]
    lines += ['%endmacro', '%endif', '']
    PALETTE.write_text('\n'.join(lines))
    files.append(PALETTE)
    manifest = {'source': str(SOURCE.relative_to(ROOT)), 'source_sha256': digest(SOURCE),
                'source_dimensions': original.size, 'sizes': SIZES, 'logo_colors': COLORS,
                'palette_indices': [16, 79], 'transparent_index': 255,
                'alpha_threshold': 128, 'resampling': 'Pillow LANCZOS, complete original canvas',
                'pillow_version': pillow_version,
                'fallback': 'Same scaled pixels mapped to nearest existing 16 UI colours only for planar VGA',
                'files': {str(path.relative_to(ROOT)): digest(path) for path in files}}
    (OUTPUT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    check()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    check() if args.check else generate()
