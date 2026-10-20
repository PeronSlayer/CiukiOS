"""f5-10C dialogs, controls, menus, message boxes and Win32 font selection.

The production user32 and gdi32 sources run in one host process over the
production kernel queues (the user32 harness), the real gdi32 engine and the
pinned FreeType 2.13.3 backend with the Liberation 2.1.5 faces. Frames are
host renders of the classic look, not Windows captures.
"""
# SPDX-License-Identifier: LGPL-2.1-or-later
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT))
from sdk.freetype import build as freetype
from scripts.win32 import gdi32, user32

HERE = Path(__file__).resolve().parent
EXERCISE = ROOT/'src/probes/win32_dialogs'
OUT = ROOT/'build/host/win32/dialogs'
FRAMES = HERE/'frames'
REVIEW = ROOT/'build/codex/f5-10c-frames'
FONTS = ROOT/'assets/fonts/liberation'


def parse_res(raw):
    """Entries of a compiled .res file: type, name, language, data."""
    out, at = [], 0
    def name(at):
        if struct.unpack_from('<H', raw, at)[0] == 0xffff:
            return struct.unpack_from('<H', raw, at+2)[0], at+4
        end = at
        while struct.unpack_from('<H', raw, end)[0]:
            end += 2
        return raw[at:end].decode('utf-16-le'), end+2
    while at + 32 <= len(raw):
        size, header = struct.unpack_from('<II', raw, at)
        kind, p = name(at+8)
        ident, p = name(p)
        p = (p + 3) & ~3
        language = struct.unpack_from('<H', raw, p+6)[0]
        data = raw[at+header:at+header+size]
        if size or kind != 0:
            out.append((kind, ident, language, data))
        at = (at + header + size + 3) & ~3
    return [e for e in out if e[0] != 0]


def resources_header(entries):
    lines = ['/* Generated from tests/host/win32/fixtures/resources.rc. */',
             'struct fixture_resource { unsigned type, id, language; const char *name; unsigned size; const unsigned char *data; };']
    for i, (kind, ident, language, data) in enumerate(entries):
        lines.append('static const unsigned char resource_%d[%d] = {%s};' % (
            i, max(len(data), 1), ','.join(str(b) for b in data) or '0'))
    lines.append('static const struct fixture_resource fixture_resources[] = {')
    for i, (kind, ident, language, data) in enumerate(entries):
        if isinstance(kind, str):
            continue
        name = '"%s"' % ident if isinstance(ident, str) else '0'
        number = ident if isinstance(ident, int) else 0
        lines.append('  {%d, %d, %d, %s, %d, resource_%d},' % (kind, number, language, name, len(data), i))
    lines.append('};')
    return '\n'.join(lines) + '\n'


def png(width, height, bgrx):
    rgb = bytearray(width*height*3)
    rgb[0::3], rgb[1::3], rgb[2::3] = bgrx[2::4], bgrx[1::4], bgrx[0::4]
    rows = b''.join(b'\0' + bytes(rgb[y*width*3:(y+1)*width*3]) for y in range(height))
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))


class DialogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, TMPDIR=str(OUT))
        cls.fontlib = freetype.build(OUT/'freetype', flags=['-O1', '-fPIC'], env=env)
        res = OUT/'resources.res'
        subprocess.run(['llvm-rc', '/fo', str(res), str(ROOT/'tests/host/win32/fixtures/resources.rc')], check=True)
        # f5-10C2: the w32-dialogs guest fixture's own resources (IDs 600/601).
        exercise_res = OUT/'exercise.res'
        subprocess.run(['llvm-rc', '/fo', str(exercise_res), str(EXERCISE/'exercise.rc')], check=True)
        (OUT/'resources.h').write_text(resources_header(parse_res(res.read_bytes()) +
                                                        parse_res(exercise_res.read_bytes())))
        session = json.loads((ROOT/'tests/host/win32/user32/session.json').read_text())
        vectors = []
        for layout in ('us', 'it'):
            rows = [','.join(map(str, row[:-1]))+','+str(int(row[-1], 16)) for row in session[layout]]
            vectors.append('static const unsigned '+layout+'[][6]={'+','.join('{'+r+'}' for r in rows)+'};')
        for key in ('scans', 'us', 'it'):
            vectors.append('static const unsigned vk_'+key+'[]={'+','.join(map(str, session['virtual_keys'][key]))+'};')
        (OUT/'session.h').write_text('\n'.join(vectors)+'\n')
        user32.icon_header(OUT)
        flags = ['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fno-pie',
                 '-ffunction-sections', '-fdata-sections', '-fsanitize=address,undefined',
                 '-fno-sanitize-recover=all', '-I'+str(ROOT/'src/kernel/include'),
                 # USER reaches GDI through GetProcAddress, as across two PE
                 # images: C function-type identity does not exist there.
                 '-fno-sanitize=function']
        objects = []
        bridge = OUT/'kernel.o'
        subprocess.run([*flags, '-c', str(ROOT/'tests/host/win32/user32/kernel_bridge.c'), '-o', str(bridge)], check=True)
        objects.append(bridge)
        user_sources = sorted((ROOT/'sdk/win32/user32').glob('*.c')) + [
            ROOT/'apps/libdeskui/v3.c', ROOT/'apps/desktop/protocol.c', ROOT/'apps/desktop/keymap.c',
            ROOT/'sdk/win32/services/bytes.c', HERE/'harness.c', EXERCISE/'exercise.c',
            *[ROOT/'apps/w32srv'/n for n in ('registry.c', 'storage.c', 'directory.c', 'defaults.c', 'protocol.c', 'posix_io.c')]]
        for i, source in enumerate(user_sources):
            obj = OUT/f'u{i}.o'
            subprocess.run([*flags, '-DW32_HOST_MODEL', '-DW32_DIALOGS_HOST', '-I'+str(ROOT/'sdk/win32/include'), '-I'+str(OUT),
                            '-I'+str(ROOT/'tests/host/win32/user32'), '-c', str(source), '-o', str(obj)], check=True)
            objects.append(obj)
        # gdi32 shares the process; its FillRect/FrameRect helpers are renamed
        # so USER's exports keep their names, as in two separate PE images.
        gdi_flags = [*flags, '-D_GNU_SOURCE', '-DW32_HOST_MODEL', '-DFillRect=gdi32_FillRect',
                     '-DFrameRect=gdi32_FrameRect', '-DInvertRect=gdi32_InvertRect',
                     '-I'+str(ROOT/'sdk/win32/include'),
                     '-idirafter', str(ROOT/'sdk/sysroot-overlay/include'), *freetype.includes()]
        gdi_sources = [ROOT/'sdk/win32/gdi32'/n for n in gdi32.SOURCES if n not in ('compiler.c', 'compiler.S')]
        gdi_sources += [ROOT/'sdk/win32/gdi32/native_font.c', HERE/'gdi_host.c']
        names = set()
        for i, source in enumerate(gdi_sources):
            obj = OUT/f'g{i}.o'
            subprocess.run([*gdi_flags, '-c', str(source), '-o', str(obj)], check=True)
            objects.append(obj)
            if i < len(gdi_sources) - 2:
                nm = subprocess.run(['llvm-nm', '--defined-only', str(obj)], capture_output=True,
                                    text=True, check=True).stdout
                names |= set(re.findall(r'^\S+ T ((?:[A-Z]|w32_gdi_)[A-Za-z0-9_]*)$', nm, re.M))
        font_obj = OUT/'libfont.o'
        subprocess.run([*gdi_flags, '-c', str(ROOT/'sdk/libfont/font.c'), '-o', str(font_obj)], check=True)
        objects.append(font_obj)
        # f5-10C2: the production w32srv font service behind the bridge.
        service_obj = OUT/'w32srv_fonts.o'
        subprocess.run([*gdi_flags, '-c', str(ROOT/'apps/w32srv/fonts.c'), '-o', str(service_obj)], check=True)
        objects.append(service_obj)
        # Name table for GetProcAddress("gdi32.dll", ...), from the sources.
        names = sorted(names - {'GdiDllMain'})
        table = OUT/'gdi_symbols.c'
        table.write_text('/* Generated: gdi32 export lookup for the dialogs harness. */\n#include <string.h>\n' +
                         ''.join(f'extern void {n}(void);\n' for n in names) +
                         'void *gdi_symbol(const char *name) {\n' +
                         ''.join(f'  if (!strcmp(name, "{n}")) return (void *){n};\n' for n in names) +
                         '  return 0;\n}\n')
        obj = OUT/'gdi_symbols.o'
        subprocess.run(['clang', '-std=c17', '-O1', '-w', '-c', str(table), '-o', str(obj)], check=True)
        objects.append(obj)
        cls.program = OUT/'dialogs'
        subprocess.run([*flags, '-no-pie', *map(str, objects), str(ROOT/'src/kernel/fs/path.c'),
                        str(ROOT/'tests/host/proc/signal_legacy.c'), str(ROOT/'tests/host/proc/desktop_legacy.c'),
                        str(cls.fontlib), '-pthread', '-o', str(cls.program)], check=True)
        cls.env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:detect_stack_use_after_return=0',
                       CIUKI_FONTS=str(FONTS), CIUKI_OUT=str(OUT))

    def execute(self, mode):
        (OUT/'frames').mkdir(exist_ok=True)
        log = OUT/(mode+'.log')
        with log.open('w') as f:
            p = subprocess.run([str(self.program), mode], stdout=f, stderr=subprocess.STDOUT,
                               env=self.env, timeout=300)
        text = log.read_text()
        self.assertEqual(p.returncode, 0, text[-6000:])
        self.assertIn('PASS '+mode, text)
        return text

    def test_font_selection_metrics_aliases_resources_and_budget(self):
        text = self.execute('fonts')
        stats = re.search(r'FONTSTATS (.*)', text).group(1)
        values = dict(item.split('=') for item in stats.split())
        self.assertLessEqual(int(values['total_peak']), 2*1024*1024)
        self.assertGreater(int(values['evictions']), 0)
        print(text[text.find('FONT'):][:4000])

    def test_font_parser_bounds(self):
        self.execute('parser')

    def test_dialog_units_templates_and_hostile_input(self):
        self.execute('templates')

    def test_modal_loop_keyboard_navigation_and_default_buttons(self):
        self.execute('keyboard')

    def test_edit_caret_selection_and_undo(self):
        self.execute('edit')

    def test_list_combo_and_scroll_bars(self):
        self.execute('lists')

    def test_menus_accelerators_and_system_menu(self):
        self.execute('menus')

    def test_message_boxes(self):
        self.execute('msgbox')

    def test_installer_style_pages(self):
        self.execute('installer')

    def test_f5_20b_upper_user(self):
        """f5-20B: the Unicode USER layer and the remaining USER entry points
        the reused Wine DLLs import (directive f5-20, part B)."""
        text = self.execute('upper-user')
        for part in ('unicode classes windows messages subclassing controls SPI',
                     'unicode dialogs DefDlgProcW DWL_DLGPROC items modal',
                     'unicode menus MENUINFO item rectangles accelerators resources',
                     'text Char* IsChar* OEM wsprintf',
                     'DDEML instances string handles data handles no-partner connect',
                     'desktop ancestors hit testing window info deferred positions',
                     'clipboard formats icons extraction DlgDirList'):
            self.assertIn('PASS f5-20B ' + part, text)

    def test_f5_20b_upper_gdi(self):
        """f5-20B: the GDI entry points, Uniscribe subset and metafiles."""
        text = self.execute('upper-gdi')
        for part in ('GDI regions pens polylines alpha', 'Uniscribe items shaping',
                     'metafiles recording playback'):
            self.assertIn('PASS f5-20B ' + part, text)

    def test_pinned_frames(self):
        text = self.execute('frames')
        manifest = {}
        REVIEW.mkdir(parents=True, exist_ok=True)
        for name, width, height in re.findall(r'^FRAME (\S+) (\d+) (\d+)$', text, re.M):
            raw = (OUT/'frames'/(name+'.bin')).read_bytes()
            width, height = int(width), int(height)
            self.assertEqual(len(raw), width*height*4)
            manifest[name] = {'width': width, 'height': height,
                              'sha256': hashlib.sha256(raw).hexdigest()}
            (REVIEW/(name+'.png')).write_bytes(png(width, height, raw))
        record = {'schema': 1,
                  'provenance': 'CiukiOS production USER/GDI host renders (classic look from primitives, '
                                'FreeType 2.13.3 v35, Liberation 2.1.5); not Windows captures',
                  'fonts': {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                            for p in sorted(FONTS.glob('*.ttf'))},
                  'frames': manifest}
        pin = FRAMES/'manifest.json'
        if os.environ.get('CIUKI_DIALOG_PIN_FRAMES') == '1':
            pin.write_text(json.dumps(record, indent=2, sort_keys=True)+'\n')
            for name in manifest:
                (FRAMES/(name+'.png')).write_bytes((REVIEW/(name+'.png')).read_bytes())
        self.assertEqual(record, json.loads(pin.read_text()))
        for name in manifest:
            self.assertEqual((FRAMES/(name+'.png')).read_bytes(), (REVIEW/(name+'.png')).read_bytes())
        self.assertGreaterEqual(len(manifest), 20)

    def test_font_bridge_equality_cache_budget_eviction_and_refusals(self):
        """f5-10C2: production gdi32 bridge client -> w32srv packet protocol ->
        production font service; glyphs byte-identical to the direct backend."""
        text = self.execute('bridge')
        for line in re.findall(r'^BRIDGE .*$', text, re.M):
            print(line)
        self.assertRegex(text, r'BRIDGE miss packets=[23]\b')
        self.assertRegex(text, r'BRIDGE cache hits=[1-9][0-9]* misses=0 transactions=0')

    def test_pinned_dialog_frames_through_the_bridge(self):
        """The f5-10C frames rendered with the glyphs crossing the bridge are
        the pinned frames: the guest path draws what the host frames pin."""
        text = self.execute('frames-bridge')
        pinned = json.loads((FRAMES/'manifest.json').read_text())['frames']
        review = ROOT/'build/codex/f5-10c2-frames/f5-10c-bridge'
        review.mkdir(parents=True, exist_ok=True)
        seen = 0
        for name, width, height in re.findall(r'^FRAME (\S+) (\d+) (\d+)$', text, re.M):
            raw = (OUT/'frames'/(name+'.bin')).read_bytes()
            self.assertEqual(hashlib.sha256(raw).hexdigest(), pinned[name]['sha256'], name)
            (review/(name+'.png')).write_bytes(png(int(width), int(height), raw))
            seen += 1
        self.assertEqual(seen, len(pinned))
        print(re.search(r'^BRIDGE frames .*$', text, re.M).group(0))

    def test_w32_dialogs_fixture_dry_run_against_suite_predicates(self):
        """The guest fixture's logic on the host harness, published with the
        kernel probe's record grammar (controller.h) and checked by the runner's
        evidence parser against the w32-dialogs-qemu-t23 predicates."""
        sys.path.insert(0, str(ROOT/'scripts/test'))
        import evidence
        text = self.execute('exercise')
        records = re.findall(r'^RECORD (\S+) (\S+) (.*)$', text, re.M)
        self.assertEqual(len(records), 8, text[-3000:])
        case = next(c for c in json.loads((ROOT/'tests/suites/f5-core.json').read_text())['cases']
                    if c['id'] == 'w32-dialogs-qemu-t23')
        run = '12345678'
        body = [('BEGIN', ''), ('DATA', 'case=fixture path=/System/Win32/Fixtures/dialogs-exercise.exe pid=7 host_dry_run=1')]
        body += [(event, fields) for probe, event, fields in records if probe == 'w32-dialogs']
        body.append(('END', 'status=PASS'))
        parser = evidence.Parser(run, 'w32-dialogs')
        lines = []
        for seq, (event, fields) in enumerate(body, 1):
            line = f'CIUKI_TEST v=1 run={run} seq={seq:06d} probe=w32-dialogs event={event}' + (' '+fields if fields else '')
            self.assertLessEqual(len(line), 240, line)
            lines.append(line)
            parser.feed(line.encode())
        parser.check(case['expected'])
        # The predicates discriminate: one failed keyboard step, a leaked
        # window or a service peak above its budget each fail the case.
        for marker, good, bad in (('case=dialog_keyboard', 'errors=0', 'errors=1'),
                                  ('case=dialog_teardown', 'live_windows=0', 'live_windows=1'),
                                  ('case=font_budget', 'service_budget=1572864', 'service_budget=1024')):
            self.assertTrue(any(marker in line and good in line for line in lines), marker)
            negative = evidence.Parser(run, 'w32-dialogs')
            for line in lines:
                negative.feed((line.replace(good, bad) if marker in line else line).encode())
            with self.assertRaises(evidence.EvidenceError):
                negative.check(case['expected'])
        dry = ROOT/'build/codex/f5-10c2-dialogs-dry-run.log'
        dry.write_text('\n'.join(lines)+'\n')
        print('\n'.join(lines))


if __name__ == '__main__':
    unittest.main()
