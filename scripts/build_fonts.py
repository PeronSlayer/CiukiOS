#!/usr/bin/env python3
"""Convert the free fonts in assets/fonts/upstream to CiukiOS desktop fonts.

A .CFN file is a 64-byte header followed by the desktop's bitmap font: for
the regular and then the bold style, 95 advance widths (characters 32-126)
and 95 glyphs of 16 rows, each row a little-endian 16-bit word whose bit 15
is the leftmost pixel (src/com/shell_gui_draw.inc, ui_text).

Header (little endian):
  0  'CIUKFNT1'        8  name (32 bytes, zero padded)
  40 version (1)       42 pixel size      44 flags (1: bold style present)
  46 data bytes (6270) 48 licence id (16 bytes, zero padded)

  build_fonts.py                  build every .CFN into assets/fonts/native
  build_fonts.py --check          verify sources and outputs (no network, no Pillow)
  build_fonts.py --convert F.TTF --name "My Font" --output MYFONT.CFN
                                  convert any TrueType/OpenType font
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'assets/fonts'
UP = BASE / 'upstream'
OUT = BASE / 'native'
STYLE_BYTES = 95 + 95 * 32
DATA_BYTES = 2 * STYLE_BYTES

# (8.3 file, display name, directory, regular, bold (None: variable wght),
#  licence id, licence file). None has a Reserved Font Name, so the
# converted fonts keep their family names.
FONTS = [
    ('NOTOSANS.CFN', 'Noto Sans', 'notosans', 'NotoSans[wdth,wght].ttf', None, 'OFL-1.1', 'OFL.txt'),
    ('NOTOSERF.CFN', 'Noto Serif', 'notoserif', 'NotoSerif[wdth,wght].ttf', None, 'OFL-1.1', 'OFL.txt'),
    ('ROBOTO.CFN', 'Roboto', 'roboto', 'Roboto[wdth,wght].ttf', None, 'OFL-1.1', 'OFL.txt'),
    ('OPENSANS.CFN', 'Open Sans', 'opensans', 'OpenSans[wdth,wght].ttf', None, 'OFL-1.1', 'OFL.txt'),
    ('FIRASANS.CFN', 'Fira Sans', 'firasans', 'FiraSans-Regular.ttf', 'FiraSans-Bold.ttf', 'OFL-1.1', 'OFL.txt'),
    ('INTER.CFN', 'Inter', 'inter', 'Inter[opsz,wght].ttf', None, 'OFL-1.1', 'OFL.txt'),
    ('WORKSANS.CFN', 'Work Sans', 'worksans', 'WorkSans[wght].ttf', None, 'OFL-1.1', 'OFL.txt'),
    ('ATKINSON.CFN', 'Atkinson Hyperlegible', 'atkinson', 'AtkinsonHyperlegible-Regular.ttf',
     'AtkinsonHyperlegible-Bold.ttf', 'OFL-1.1', 'OFL.txt'),
    ('INCONSOL.CFN', 'Inconsolata', 'inconsolata', 'Inconsolata[wdth,wght].ttf', None, 'OFL-1.1', 'OFL.txt'),
    ('DEJAVU.CFN', 'DejaVu Sans', 'dejavu', 'DejaVuSans.ttf', 'DejaVuSans-Bold.ttf', 'Bitstream-Vera', 'LICENSE'),
]
# The built-in desktop font (src/com/setup_font.bin, from Liberation Sans:
# "Liberation" is a Reserved Font Name, so it is named for CiukiOS).
DEFAULT = ('CIUKIOS.CFN', 'CiukiOS Sans (default)', 13, 'OFL-1.1')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def load(path, size, bold):
    from PIL import ImageFont
    font = ImageFont.truetype(str(path), size)
    try:
        axes = font.get_variation_axes()
    except (OSError, AttributeError):
        axes = None
    if axes:
        values = []
        for axis in axes:
            name = axis['name'] if isinstance(axis['name'], str) else axis['name'].decode()
            tag = name.lower()
            if tag.startswith('weight'):
                values.append(700 if bold else 400)
            elif tag.startswith('width'):
                values.append(100)
            elif tag.startswith('optical'):
                values.append(max(axis['minimum'], min(axis['maximum'], 14)))
            else:
                values.append(axis['default'])
        font.set_variation_by_axes(values)
    return font


BASELINE = 12          # the built-in font's baseline row in its 16-row cell
ASCII = ''.join(chr(c) for c in range(33, 127))


def extents(font):
    """Rows above and below the baseline that ASCII glyphs use."""
    ascent, _ = font.getmetrics()
    top, bottom = 99, -99
    for ch in ASCII:
        box = font.getbbox(ch)
        top = min(top, box[1] - ascent)
        bottom = max(bottom, box[3] - ascent)
    return -top, bottom


CAP_HEIGHT = 10        # the built-in font's 'H' (Liberation Sans, 13 px)


def fit_size(path):
    """The size whose capitals match the built-in font's, within the cell."""
    best, score = 10, 99
    for size in range(10, 17):
        font = load(path, size, False)
        up, down = extents(font)
        if up > BASELINE or down > 16 - BASELINE:
            continue
        box = font.getbbox('H')
        diff = abs((box[3] - box[1]) - CAP_HEIGHT)
        if diff < score:
            best, score = size, diff
    return best


def rasterize(font):
    from PIL import Image, ImageDraw
    widths, glyphs = bytearray(), bytearray()
    ascent, _ = font.getmetrics()
    for code in range(32, 127):
        ch = chr(code)
        widths.append(min(16, max(3, round(font.getlength(ch)))))
        im = Image.new('1', (16, 16))
        ImageDraw.Draw(im).text((0, BASELINE - ascent), ch, font=font, fill=1)
        for y in range(16):
            bits = sum(int(bool(im.getpixel((x, y)))) << (15 - x) for x in range(16))
            glyphs += bits.to_bytes(2, 'little')
    return bytes(widths + glyphs)


def header(name, size, licence):
    return struct.pack('<8s32s4H16s', b'CIUKFNT1', name.encode()[:31], 1, size, 1,
                       DATA_BYTES, licence.encode()[:15])


def convert(regular, bold, name, licence, size=None):
    size = size or fit_size(regular)
    data = rasterize(load(regular, size, False)) + rasterize(load(bold or regular, size, True))
    assert len(data) == DATA_BYTES
    return header(name, size, licence) + data, size


def preview(fonts, path):
    """A sheet of every font as the desktop draws it (1-bit glyphs)."""
    from PIL import Image
    sample = 'The quick brown fox jumps over the lazy dog 0123456789'
    sheet = Image.new('RGB', (760, 24 + 44 * len(fonts)), (203, 208, 212))
    for row, (name, blob) in enumerate(fonts):
        y0 = 12 + row * 44
        for style, text in ((0, name), (1, sample)):
            data = blob[64 + style * STYLE_BYTES:]
            x = 12
            for ch in text:
                code = ord(ch) - 32
                for gy in range(16):
                    bits = int.from_bytes(data[95 + code * 32 + gy * 2:95 + code * 32 + gy * 2 + 2], 'little')
                    for gx in range(16):
                        if bits & (0x8000 >> gx) and x + gx < sheet.width:
                            sheet.putpixel((x + gx, y0 + style * 20 + gy), (32, 39, 51))
                x += data[code]
    sheet.save(path)


def licences(record):
    """\\SYSTEM\\FONTS\\LICENSES.TXT: every font's notice and licence text."""
    lines = ['CiukiOS desktop fonts', '=====================', '',
             'Bitmap conversions (scripts/build_fonts.py) of the fonts below. Each keeps',
             'its licence; the conversions are Modified Versions under those licences.',
             'None of the OFL fonts has a Reserved Font Name. The default font is a',
             'conversion of Liberation Sans ("Liberation" is a Reserved Font Name, so',
             'it is called CiukiOS Sans).', '']
    ofl = None
    for font in record:
        lines.append(f"{font['file']:<13} {font['name']}")
        if 'licence_file' in font:
            text = (BASE / font['licence_file']).read_text(encoding='utf-8-sig')
            lines.append('    ' + text.strip().splitlines()[0])
            lines.append(f"    Licence: {font['licence']}")
            if font['licence'] == 'OFL-1.1' and ofl is None:
                ofl = text[text.index('This Font Software is licensed'):]
        else:
            notice = (ROOT / 'src/com/setup_font.LICENSE').read_text().strip().split('\n\n')[0]
            lines += ['    ' + line.strip() for line in notice.splitlines()]
            lines.append('    Licence: OFL-1.1 (Liberation Sans)')
    lines += ['', '-' * 72, 'SIL Open Font License, Version 1.1', '-' * 72, ofl.strip(), '',
              '-' * 72, 'DejaVu Sans: Bitstream Vera Fonts licence and DejaVu changes', '-' * 72,
              (UP / 'dejavu/LICENSE').read_text().strip(), '']
    text = '\r\n'.join(line.rstrip() for line in '\n'.join(lines).splitlines()) + '\r\n'
    (BASE / 'LICENSES.TXT').write_bytes(text.encode('ascii', 'replace'))


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    built, record = [], []
    default = (ROOT / 'src/com/setup_font.bin').read_bytes()
    assert len(default) == DATA_BYTES
    blob = header(DEFAULT[1], DEFAULT[2], DEFAULT[3]) + default
    (OUT / DEFAULT[0]).write_bytes(blob)
    built.append((DEFAULT[1], blob))
    record.append({'file': DEFAULT[0], 'name': DEFAULT[1], 'size': DEFAULT[2],
                   'source': 'src/com/setup_font.bin (Liberation Sans, OFL-1.1)',
                   'sha256': hashlib.sha256(blob).hexdigest()})
    for file, name, folder, regular, bold, licence, licence_file in FONTS:
        blob, size = convert(UP / folder / regular, UP / folder / bold if bold else None, name, licence)
        (OUT / file).write_bytes(blob)
        built.append((name, blob))
        record.append({'file': file, 'name': name, 'size': size, 'licence': licence,
                       'licence_file': f'upstream/{folder}/{licence_file}',
                       'sources': [f'upstream/{folder}/{regular}'] + ([f'upstream/{folder}/{bold}'] if bold else []),
                       'sha256': hashlib.sha256(blob).hexdigest()})
    preview(built, BASE / 'preview.png')
    licences(record)
    sources = sorted(p for p in UP.rglob('*') if p.is_file())
    manifest = {'google_fonts_commit': (UP / 'GOOGLE_FONTS_COMMIT').read_text().strip(),
                'dejavu_release': 'https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.zip',
                'dejavu_zip_sha256': '7576310b219e04159d35ff61dd4a4ec4cdba4f35c00e002a136f00e96a908b0a',
                'sources': {str(p.relative_to(BASE)): sha(p) for p in sources},
                'fonts': record}
    (BASE / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'[fonts] {len(built)} fonts in {OUT}')


def check():
    manifest = json.loads((BASE / 'manifest.json').read_text())
    for name, digest in manifest['sources'].items():
        assert sha(BASE / name) == digest, f'font source changed: {name}'
    for font in manifest['fonts']:
        blob = (OUT / font['file']).read_bytes()
        assert hashlib.sha256(blob).hexdigest() == font['sha256'], f"stale font: {font['file']}"
        assert blob[:8] == b'CIUKFNT1' and len(blob) == 64 + DATA_BYTES, font['file']
    print(f"[fonts] {len(manifest['sources'])} pinned sources and {len(manifest['fonts'])} fonts match")


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--check', action='store_true')
    p.add_argument('--convert', type=Path, help='a TrueType/OpenType font to convert')
    p.add_argument('--bold', type=Path, help='its bold face (default: the same font)')
    p.add_argument('--name', help='the name shown in CiukiOS')
    p.add_argument('--licence', default='see-source')
    p.add_argument('--size', type=int)
    p.add_argument('--output', type=Path)
    a = p.parse_args()
    if a.check:
        check()
    elif a.convert:
        blob, size = convert(a.convert, a.bold, a.name or a.convert.stem, a.licence, a.size)
        out = a.output or Path(a.convert.stem.upper()[:8] + '.CFN')
        out.write_bytes(blob)
        print(f'[fonts] {out}: {a.name or a.convert.stem}, {size} px')
    else:
        build()
