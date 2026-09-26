#!/usr/bin/env python3
"""Rasterize the OFL-licensed Liberation Sans UI font for 16-color VGA."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
import argparse
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, default=Path('src/com/setup_font.bin'))
    args = p.parse_args()
    data = bytearray()
    for face in ('Regular', 'Bold'):
        font_path = subprocess.check_output(['fc-match', '-f', '%{file}', f'Liberation Sans:style={face}'], text=True)
        font = ImageFont.truetype(font_path, 13)
        widths, glyphs = bytearray(), bytearray()
        for code in range(32, 127):
            ch = chr(code)
            widths.append(min(16, max(3, round(font.getlength(ch)))))
            im = Image.new('1', (16, 16))
            ImageDraw.Draw(im).text((0, 0), ch, font=font, fill=1)
            for y in range(16):
                bits = sum(int(bool(im.getpixel((x, y)))) << (15-x) for x in range(16))
                glyphs += bits.to_bytes(2, 'little')
        data += widths + glyphs
    args.output.write_bytes(data)


if __name__ == '__main__':
    main()
