#!/usr/bin/env python3
"""Gate for CiukPaint (src/apps/paint.c, \\SYSTEM\\APPS\\PAINT.APP).

Boots a copy of the image and drives CiukPaint with QEMU keyboard and mouse
events. The module logs "[PAINT] ..." lines on COM1, among them its geometry
(the picture's screen origin, the tool box, the options box and the first
colour swatch) and the palette indices of its 28 swatches, so the clicks and
the expected pixels follow the real layout.

  attributes  Ctrl+E: the picture becomes 200 x 150
  shapes      solid rectangle, ellipse (outline + fill), line, flood fill,
              text, pencil with the right button (background colour: a drag
              followed by polling), a pencil stroke then Undo, Repeat, Undo
  save        Ctrl+S -> Save As -> C:\\QA\\ART.BMP
  reopen      Ctrl+N, a scribble, Ctrl+O -> "Save changes?" No -> Open;
              the colour picker reads back the pixels
  files       Files opens ART.BMP in CiukPaint (Enter)
After shutdown the BMP is read from the disk image with the FAT16 parser of
the other gates: header, 200 x 150, 8 bits, and pixel colours against the
palette CiukPaint uses (its copy of the portrait and icon palettes must match
src/com/*_palette.inc). QEMU evidence only.
"""
import argparse
import json
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qemu_test_installed_hdd import FAT16                      # noqa: E402
from qemu_test_native_windows import WindowVM                  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
KEYS = {' ': 'spc', '.': 'dot', '\\': 'backslash', '-': 'minus', '/': 'slash',
        ':': 'shift-semicolon', '_': 'shift-minus'}
T_SELECT, T_ERASER, T_FILL, T_PICK, T_ZOOM, T_PENCIL, T_BRUSH, T_AIR, T_TEXT, T_LINE, \
    T_CURVE, T_RECT, T_POLY, T_ELLIPSE, T_RRECT = range(15)
DESK_DEFAULT = [9, 10, 12, 13, 18, 30, 18, 32, 23, 13, 23, 24, 37, 15, 16, 31, 24, 37, 37, 29, 15,
                51, 52, 53, 28, 30, 34, 27, 36, 48, 24, 41, 38, 39, 49, 48, 49, 25, 23, 43, 34, 46,
                57, 47, 27, 61, 61, 60]


def inc_palette(name):
    return [tuple(map(int, line.strip()[3:].split(','))) for line in (ROOT / 'src/com' / name).read_text().splitlines()
            if line.strip().startswith('db ')]


def c_table(src, name):
    body = re.search(r'static const u8 %s\[192\] = \{([^}]*)\}' % name, src).group(1)
    v = [int(x) for x in body.replace('\n', ' ').split(',') if x.strip()]
    return [tuple(v[i:i + 3]) for i in range(0, 192, 3)]


def palette():
    """The 144 colours as 8-bit RGB (the desktop's are the default scheme)."""
    desk = [tuple(DESK_DEFAULT[i:i + 3]) for i in range(0, 48, 3)]
    six = desk + inc_palette('ciuki_logo_palette.inc') + inc_palette('ui_icons_palette.inc')
    return [tuple((v << 2) | (v >> 4) for v in c) for c in six]


class Gate:
    def __init__(self, vm):
        self.vm = vm
        self.steps = []
        self.geo = None

    def serial(self, offset=0):
        return subprocess.check_output(['scripts/serial_log_normalize.py', '--offset', str(offset),
                                        str(self.vm.serial)]).decode('cp437', 'replace')

    def type(self, text):
        for ch in text:
            key = KEYS.get(ch, ch)
            if ch.isupper():
                key = 'shift-' + ch.lower()
            self.vm.key(key)

    def act(self, name, keys, expect, timeout=30):
        offset = self.vm.offset()
        if callable(keys):
            keys()
        else:
            for k in keys:
                if k.startswith('type:'):
                    self.type(k[5:])
                else:
                    self.vm.key(k)
        for marker in ([expect] if isinstance(expect, str) else expect):
            self.vm.wait(marker, offset, timeout)
        time.sleep(0.4)
        self.steps.append(name)
        print(f'[ciukpaint] PASS {name}', flush=True)
        return self.serial(offset)

    # ---- pointer ----
    def screen(self, x, y):
        return self.geo['ox'] + x, self.geo['oy'] + y

    def click(self, sx, sy, button=1):
        self.vm.position(sx, sy)
        self.vm.hmp(f'mouse_button {button}')
        time.sleep(0.2)
        self.vm.hmp('mouse_button 0')
        time.sleep(0.4)

    def tool(self, t):
        x = self.geo['tbx'] + 4 + (t & 1) * 26 + 12
        y = self.geo['tby'] + 4 + (t >> 1) * 26 + 12
        return lambda: self.click(x, y)

    def option(self, row, height):
        x = self.geo['optx'] + 26
        y = self.geo['opty'] + 2 + row * height + height // 2
        return lambda: self.click(x, y)

    def swatch(self, i, button=1):
        x = self.geo['swx'] + (i % 14) * 17 + 8
        y = self.geo['swy'] + (i // 14) * 18 + 8
        return lambda: self.click(x, y, button)

    def drag(self, points, button=1):
        def run():
            self.vm.position(*self.screen(*points[0]))
            self.vm.hmp(f'mouse_button {button}')
            time.sleep(0.3)
            for p in points[1:]:
                self.vm.position(*self.screen(*p))
                time.sleep(0.2)
            time.sleep(0.3)
            self.vm.hmp('mouse_button 0')
            time.sleep(0.3)
        return run

    def canvas_click(self, x, y, button=1):
        return lambda: self.click(*self.screen(x, y), button)


def coords(text, label):
    m = re.findall(r'\[PAINT\] draw %s (\d+),(\d+)-(\d+),(\d+)' % label, text)
    return tuple(map(int, m[-1]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--image', type=Path, default=Path('build/full/ciukios-full.img'))
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    disk = out / 'disk.img'
    shutil.copyfile(args.image, disk)
    subprocess.run(['mmd', '-i', f'{disk}@@{FAT16(disk).start}', '::QA'], check=True)
    report = {'passed': False, 'physical_hardware_qualified': False}
    src = (ROOT / 'src/apps/paint.c').read_text()
    pal_checks = {
        'portrait_palette_current': c_table(src, 'logo_pal') == inc_palette('ciuki_logo_palette.inc'),
        'icon_palette_current': c_table(src, 'icon_pal') == inc_palette('ui_icons_palette.inc'),
    }
    vm = WindowVM(disk, out, 'std', memory=128, palette='platinum')
    g = Gate(vm)
    marks = {}
    try:
        vm.ready()
        text = g.act('Win+R paint opens CiukPaint', ['meta_l-r', 'type:paint', 'ret'],
                     ['[DESKTOP] WINDOW 16 OPEN', '[PAINT] ready', '[PAINT] geometry'])
        swatches = [int(v) for v in re.search(r'\[PAINT\] swatches ([\d ]+)', text).group(1).split()]
        vm.shot('open')
        g.act('Attributes: 200 x 150', ['ctrl-e', 'type:200', 'tab', 'type:150', 'ret'],
              '[PAINT] attributes 200x150')
        text = g.serial(0)
        vals = [int(v) for v in re.findall(r'\[PAINT\] geometry ([\d ]+)', text)[-1].split()]
        g.geo = dict(zip(['ox', 'oy', 'tbx', 'tby', 'optx', 'opty', 'swx', 'swy'], vals))
        report['geometry'] = g.geo
        red, yellow, green, blue, purple, rose = (swatches[2], swatches[17], swatches[4], swatches[20],
                                                   swatches[7], swatches[23])
        marks['colours'] = dict(black=swatches[0], red=red, yellow=yellow, green=green, blue=blue, purple=purple, rose=rose)
        # ---- shapes ----
        g.act('Rectangle tool', g.tool(T_RECT), '[PAINT] tool Rectangle')
        g.act('Solid style', g.option(2, 25), '[PAINT] option line 1 fill 2')
        g.act('Red', g.swatch(2), f'[PAINT] colour fg {red}')
        marks['rect'] = coords(g.act('Draw a solid rectangle', g.drag([(20, 20), (50, 40), (80, 60)]),
                                     '[PAINT] draw Rectangle'), 'Rectangle')
        g.act('Ellipse tool', g.tool(T_ELLIPSE), '[PAINT] tool Ellipse')
        g.act('Outline and fill style', g.option(1, 25), '[PAINT] option line 1 fill 1')
        g.act('Blue', g.swatch(20), f'[PAINT] colour fg {blue}')
        marks['ellipse'] = coords(g.act('Draw an ellipse', g.drag([(100, 10), (140, 40), (180, 70)]),
                                        '[PAINT] draw Ellipse'), 'Ellipse')
        g.act('Fill tool', g.tool(T_FILL), '[PAINT] tool Fill')
        g.act('Yellow', g.swatch(17), f'[PAINT] colour fg {yellow}')
        g.act('Fill the ellipse', g.canvas_click(140, 40), '[PAINT] fill')
        g.act('Line tool', g.tool(T_LINE), '[PAINT] tool Line')
        g.act('Line width 3', g.option(2, 15), '[PAINT] option line 3 fill 1')
        g.act('Green', g.swatch(4), f'[PAINT] colour fg {green}')
        marks['line'] = coords(g.act('Draw a line', g.drag([(10, 135), (100, 135), (190, 135)]),
                                     '[PAINT] draw Line'), 'Line')
        g.act('Text tool', g.tool(T_TEXT), '[PAINT] tool Text')
        g.act('Black', g.swatch(0), f'[PAINT] colour fg {swatches[0]}')
        g.act('Place text', g.canvas_click(20, 80), [], 5)
        g.act('Type and place the text', lambda: (g.type('CiukPaint'), time.sleep(1), g.click(*g.screen(190, 20))),
              '[PAINT] text CiukPaint')
        vm.shot('drawn')
        g.act('Pencil tool', g.tool(T_PENCIL), '[PAINT] tool Pencil')
        g.act('Rose background (right click)', g.swatch(23, 2), f'[PAINT] colour bg {rose}')
        marks['right'] = coords(g.act('Right-button pencil stroke', g.drag([(110, 110), (140, 112), (170, 115)], 2),
                                      '[PAINT] draw Pencil'), 'Pencil')
        g.act('Purple', g.swatch(7), f'[PAINT] colour fg {purple}')
        def live_stroke():
            before = Image.open(vm.shot('stroke-before')).convert('RGB')
            vm.position(*g.screen(5, 100))
            vm.hmp('mouse_button 1')
            time.sleep(0.2)
            vm.position(*g.screen(100, 100))
            time.sleep(0.25)
            held = Image.open(vm.shot('stroke-held')).convert('RGB')
            # Sample the middle of the drawn line, away from the pointer.
            sx, sy = g.screen(50, 100)
            report['live_stroke_visible'] = any(
                before.getpixel((sx + dx, sy + dy)) != held.getpixel((sx + dx, sy + dy))
                for dx in range(-3, 4) for dy in range(-2, 3))
            vm.position(*g.screen(195, 100))
            time.sleep(0.2)
            vm.hmp('mouse_button 0')
            time.sleep(0.3)
            assert report['live_stroke_visible'], 'pencil stroke did not appear while button was held'
        marks['undone'] = coords(g.act('Pencil stroke visible before release', live_stroke,
                                       '[PAINT] draw Pencil'), 'Pencil')
        g.act('Undo', ['ctrl-z'], '[PAINT] undo')
        g.act('Repeat', ['ctrl-y'], '[PAINT] redo')
        g.act('Undo again', ['ctrl-z'], '[PAINT] undo')
        vm.shot('before-save')
        # ---- save, new, reopen ----
        g.act('Ctrl+S asks for a name', ['ctrl-s'], '[PAINT] dialog Save As')
        g.act('Save as C:\\QA\\ART.BMP', ['type:C:\\QA\\ART.BMP', 'ret'], '[PAINT] saved C:\\QA\\ART.BMP')
        g.act('New', ['ctrl-n'], '[PAINT] new 200x150')
        g.act('Rectangle tool again', g.tool(T_RECT), '[PAINT] tool Rectangle')
        g.act('A scribble', g.drag([(30, 30), (60, 60)]), '[PAINT] draw Rectangle')
        g.act('Open asks to save', ['ctrl-o'], '[PAINT] dialog save changes')
        g.act('No: the Open dialog', ['n'], '[PAINT] dialog Open')
        g.act('Open C:\\QA\\ART.BMP', ['type:C:\\QA\\ART.BMP', 'ret'],
              ['[PAINT] opened C:\\QA\\ART.BMP', '[PAINT] size 200x150'])
        vm.shot('reopened')
        rx0, ry0, rx1, ry1 = marks['rect']
        g.act('Pick colour tool', g.tool(T_PICK), '[PAINT] tool Pick Colour')
        g.act('The rectangle is red', g.canvas_click((rx0 + rx1) // 2, (ry0 + ry1) // 2),
              f'[PAINT] picked fg {red}')
        g.act('Pick colour tool again', g.tool(T_PICK), '[PAINT] tool Pick Colour')
        ex0, ey0, ex1, ey1 = marks['ellipse']
        g.act('The ellipse is filled yellow', g.canvas_click((ex0 + ex1) // 2, (ey0 + ey1) // 2),
              f'[PAINT] picked fg {yellow}')
        g.act('Alt+F4 closes CiukPaint', ['alt-f4'], '[PAINT] closed')
        # ---- Files ----
        g.act('Files on C:\\QA', ['meta_l-r', 'type:explorer c:\\qa', 'ret'], '[FILES] list C:\\QA 1')
        g.act('Enter opens ART.BMP in CiukPaint', ['type:a', 'ret'],
              ['[FILES] open C:\\QA\\ART.BMP', '[PAINT] opened C:\\QA\\ART.BMP'])
        vm.shot('from-files')
        g.act('Alt+F4 closes CiukPaint again', ['alt-f4'], '[PAINT] closed')
        report['steps'] = g.steps
    except Exception as error:
        report['error'] = repr(error)
        try:
            vm.shot('failure')
            report['serial_tail'] = g.serial(0)[-3000:]
        except Exception as diagnostic:
            report['diagnostic_error'] = repr(diagnostic)
    finally:
        vm.close()
    report['marks'] = marks
    checks = dict(pal_checks)
    checks['live_stroke_visible'] = report.get('live_stroke_visible', False)
    if 'error' not in report:
        data = FAT16(disk).read('QA/ART.BMP')
        (out / 'ART.BMP').write_bytes(data)
        off, = struct.unpack_from('<I', data, 10)
        w, h, planes, bpp, comp = struct.unpack_from('<iiHHI', data, 18)
        ncol, = struct.unpack_from('<I', data, 46)
        pal = palette()
        table = [tuple(data[54 + i * 4:54 + i * 4 + 3][::-1]) for i in range(ncol)]
        stride = (w + 3) & ~3

        def rgb(x, y):
            return table[data[off + (h - 1 - y) * stride + x]]

        def near(x, y, colour, r=1):
            return any(rgb(x + dx, y + dy) == pal[colour] for dx in range(-r, r + 1) for dy in range(-r, r + 1)
                       if 0 <= x + dx < w and 0 <= y + dy < h)
        c = marks['colours']
        rx0, ry0, rx1, ry1 = marks['rect']
        ex0, ey0, ex1, ey1 = marks['ellipse']
        lx0, ly0, lx1, ly1 = marks['line']
        ux0, uy0, ux1, uy1 = marks['undone']
        px0, py0, px1, py1 = marks['right']
        text_px = sum(rgb(x, y) == pal[c['black']] for x in range(18, 110) for y in range(78, 98))
        checks.update({
            'bmp_header': data[:2] == b'BM' and planes == 1 and bpp == 8 and comp == 0 and 0 < ncol <= 256,
            'size_200x150': (w, h) == (200, 150),
            'file_size': len(data) == off + stride * h,
            'rectangle_red': rgb((rx0 + rx1) // 2, (ry0 + ry1) // 2) == pal[c['red']]
                             and rgb(min(rx0, rx1) + 1, min(ry0, ry1) + 1) == pal[c['red']],
            'ellipse_outline_blue': near(min(ex0, ex1), (ey0 + ey1) // 2, c['blue'])
                                    and near((ex0 + ex1) // 2, min(ey0, ey1), c['blue']),
            'ellipse_filled_yellow': rgb((ex0 + ex1) // 2, (ey0 + ey1) // 2) == pal[c['yellow']],
            'line_green': near((lx0 + lx1) // 2, (ly0 + ly1) // 2, c['green']),
            'text_black': text_px > 40,
            'right_button_rose': near((px0 + px1) // 2, (py0 + py1) // 2, c['rose'], 2),
            'undone_stroke_absent': not any(rgb(x, y) == pal[c['purple']] for x in range(w) for y in range(h)),
            'corner_white': rgb(199, 0) == pal[80],
        })
        report['text_pixels'] = text_px
    report['checks'] = checks
    report['passed'] = 'error' not in report and all(checks.values())
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'error': report.get('error'),
                      'checks': checks}, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
