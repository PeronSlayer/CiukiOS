#!/usr/bin/env python3
"""Compile CiukiOS's official Tango/Ciuki icons into a bounded native resource.

No network access at build time. Upstream archive and approved portrait are pinned.
--check verifies checked-in hashes using only the Python standard library.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import tarfile

from build_ciuki_logo import UI_DAC

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'assets/icons'
ARCHIVE = BASE / 'upstream/tango-icon-theme-0.8.90.tar.gz'
ARCHIVE_SHA = '6e98d8032d57d818acc907ec47e6a718851ff251ae7c29aafb868743eb65c88e'
LOGO = ROOT / 'assets/brand/ciuki-logo.png'
LOGO_SHA = '17210ac9cfe07c54d6b0069a1502a036d52cb6340715ad47bdd54fa4dd386620'
OUTPUT = BASE / 'native'
PALETTE = ROOT / 'src/com/ui_icons_palette.inc'
# IDs are the stable shell ui_icon ABI, including reserved hard-disk ID 3.
ICONS = [
    ('Computer', 'devices/computer', (16, 14, 14)),
    ('Files', 'places/folder', None),
    ('Editor', 'apps/accessories-text-editor', None),
    ('Hard disk', 'devices/drive-harddisk', None),
    ('DOS', 'apps/utilities-terminal', None),
    ('Run', 'mimetypes/application-x-executable', None),
    ('Doom', 'devices/input-gaming', None),
    ('Games', 'categories/applications-games', None),
    ('Display', 'devices/video-display', None),
    ('Sound', 'status/audio-volume-high', None),
    ('Floppy', 'devices/media-floppy', None),
    ('About CiukiOS', None, (24, 4, 4)),
    ('Power', 'actions/system-shutdown', None),
    ('Windows', 'apps/preferences-system-windows', None),
    ('Costa', 'places/user-desktop', None),
    ('USB', 'devices/drive-removable-media', None),
    ('CD-ROM', 'devices/media-optical', None),
    ('Install CiukiOS', 'apps/system-installer', (16, 0, 16)),
]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check():
    m = json.loads((OUTPUT / 'manifest.json').read_text())
    assert sha(ARCHIVE) == ARCHIVE_SHA, 'Upstream archive changed'
    assert sha(LOGO) == LOGO_SHA, 'Approved Ciuki portrait changed'
    for name, expected in m['files'].items():
        assert sha(ROOT / name) == expected, f'Stale or modified icon asset: {name}'
    data = (OUTPUT / 'ICONS.DAT').read_bytes()
    assert data[:16] == struct.pack('<8s4H', b'CIUKICO1', 18, 32, 32, 0)
    assert len(data) == 16 + 18 * 1024
    assert all(x < 144 or x == 255 for x in data[16:])
    assert len((OUTPUT / 'fallback.bin').read_bytes()) == 128
    print('[ui-icons] pinned Tango archive, Ciuki master and 18 native sprites match')


def generate():
    from PIL import Image, ImageDraw, __version__ as pillow_version
    assert sha(ARCHIVE) == ARCHIVE_SHA
    assert sha(LOGO) == LOGO_SHA
    OUTPUT.mkdir(parents=True, exist_ok=True)
    sources = BASE / 'sources'
    sources.mkdir(exist_ok=True)
    files = [ARCHIVE, LOGO, Path(__file__).resolve(), ROOT / 'scripts/build_ciuki_logo.py',
             BASE / 'upstream/COPYING', BASE / 'upstream/AUTHORS']
    images = []
    records = []
    with tarfile.open(ARCHIVE) as archive:
        for number, (name, source, badge) in enumerate(ICONS):
            if source:
                member = 'tango-icon-theme-0.8.90/32x32/' + source + '.png'
                blob = archive.extractfile(member).read()
                path = sources / (source.replace('/', '-') + '.png')
                path.write_bytes(blob)
                files.append(path)
                im = Image.open(io.BytesIO(blob)).convert('RGBA')
                assert im.size == (32, 32), member
            else:
                member = None
                im = Image.new('RGBA', (32, 32))
            images.append(im)
            records.append({'id': number, 'name': name, 'upstream_member': member,
                            'ciuki_overlay': badge})
    def pixels(im):
        return getattr(im, 'get_flattened_data', im.getdata)()
    opaque = [p[:3] for im in images for p in pixels(im) if p[3] >= 128]
    sample = Image.new('RGB', (len(opaque), 1))
    sample.putdata(opaque)
    q = sample.quantize(colors=64, method=Image.Quantize.MEDIANCUT,
                        dither=Image.Dither.NONE)
    raw = q.getpalette()[:192]
    dac = [tuple(round(c * 63 / 255) for c in raw[i:i+3]) for i in range(0, 192, 3)]
    rgb = [tuple((v << 2) | (v >> 4) for v in c) for c in dac]
    logo_palette = []
    logo_inc = ROOT / 'src/com/ciuki_logo_palette.inc'
    files.append(logo_inc)
    for line in logo_inc.read_text().splitlines():
        if line.strip().startswith('db '):
            logo_palette.append(tuple(map(int, line.strip()[3:].split(','))))
    assert len(logo_palette) == 64
    colors = [tuple((v << 2) | (v >> 4) for v in c)
              for c in list(UI_DAC) + logo_palette + dac]
    payload = bytearray(struct.pack('<8s4H', b'CIUKICO1', 18, 32, 32, 0))
    previews = []
    for n, im in enumerate(images):
        data = bytearray(255 if p[3] < 128 else 80 + min(range(64), key=lambda i:
                         sum((p[c]-rgb[i][c])**2 for c in range(3))) for p in pixels(im))
        badge = ICONS[n][2]
        if badge:
            size, x, y = badge
            path = ROOT / f'assets/brand/native/ciuki-{size}.bin'
            files.append(path)
            logo = path.read_bytes()
            assert len(logo) == size * size
            for row in range(size):
                for col in range(size):
                    value = logo[row * size + col]
                    if value != 255:
                        data[(y + row) * 32 + x + col] = value
        payload.extend(data)
        preview = Image.new('RGBA', (32, 32))
        preview.putdata([(*colors[p], 255) if p != 255 else (0, 0, 0, 0) for p in data])
        path = OUTPUT / f'{n:02d}.png'
        preview.save(path)
        files.append(path)
        previews.append(preview)
    path = OUTPUT / 'ICONS.DAT'
    path.write_bytes(payload)
    files.append(path)
    fallback = bytes(min(range(16), key=lambda i: sum((color[c]-UI_DAC[i][c])**2
                     for c in range(3))) for color in logo_palette + dac)
    path = OUTPUT / 'fallback.bin'
    path.write_bytes(fallback)
    files.append(path)
    lines = ['; Generated by scripts/build_ui_icons.py; Tango 0.8.90 Public Domain.',
             '%ifndef UI_ICONS_PALETTE_INCLUDED', '%define UI_ICONS_PALETTE_INCLUDED 1',
             '%define UI_ICONS_COLOR_COUNT 64', '%macro UI_ICONS_PALETTE 0']
    lines += ['    db ' + ','.join(map(str, color)) for color in dac]
    lines += ['%endmacro', '%endif', '']
    PALETTE.write_text('\n'.join(lines))
    files.append(PALETTE)
    sheet = Image.new('RGB', (864, 364), '#cbd0d4')
    draw = ImageDraw.Draw(sheet)
    draw.text((18, 12), 'CiukiOS / Official icons / Tango + Ciuki', fill='#202733')
    for i, im in enumerate(previews):
        x, y = 144 * (i % 6), 34 + 108 * (i // 6)
        bigger = im.resize((64, 64), Image.Resampling.NEAREST)
        sheet.paste(bigger, (x + 40, y + 5), bigger)
        draw.text((x + 12, y + 78), ICONS[i][0], fill='#202733')
    path = BASE / 'preview.png'
    sheet.save(path)
    files.append(path)
    manifest = {'library': 'Tango 0.8.90', 'license': 'Public Domain (upstream COPYING)',
                'source_url': 'https://tango.freedesktop.org/releases/tango-icon-theme-0.8.90.tar.gz',
                'archive_sha256': ARCHIVE_SHA, 'ciuki_master_sha256': LOGO_SHA,
                'size': [32, 32], 'count': len(ICONS), 'palette_indices': [80, 143],
                'ciuki_palette_indices': [16, 79], 'transparent_index': 255,
                'alpha_threshold': 128, 'pillow_version': pillow_version,
                'icons': records,
                'files': {str(p.relative_to(ROOT)): sha(p) for p in dict.fromkeys(files)}}
    (OUTPUT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    check()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    check() if args.check else generate()
