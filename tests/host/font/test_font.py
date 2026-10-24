"""F2-49: production libfont, independent sfnt references, desktop harness.

Run this suite on its own with nice -n 19; no QEMU/image rebuilds.
Re-pin reviewed coverage with --prepare-goldens after an intentional change.
"""
# SPDX-License-Identifier: MIT
import base64
import concurrent.futures
import ctypes as C
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/host/desktop"))
from frame_review import capture
FONTS = ROOT / "assets/fonts/liberation"
GOLDENS = Path(__file__).with_name("coverage-goldens.json")

class Metrics(C.Structure):
    _fields_ = [(x, C.c_int) for x in ("ascent", "descent", "line_gap")] + [(x, C.c_uint) for x in ("units_per_em", "glyph_count")] + \
               [(x, C.c_int) for x in ("has_os2", "typo_ascent", "typo_descent", "typo_line_gap")] + \
               [(x, C.c_uint) for x in ("win_ascent", "win_descent", "weight", "selection")] + [(x, C.c_char * 128) for x in ("family", "style")]

class Bitmap(C.Structure):
    _fields_ = [(x, C.c_uint) for x in ("width", "height", "stride")] + [(x, C.c_int) for x in ("left", "top", "advance")] + [("coverage", C.POINTER(C.c_uint8))]

class Stats(C.Structure):
    _fields_ = [("glyphs", C.c_uint), ("bytes", C.c_size_t)] + [(x, C.c_uint64) for x in ("hits", "misses", "evictions")]

class SizeMetrics(C.Structure):
    _fields_ = [(x, C.c_int32) for x in ("ascent", "descent", "line_gap", "max_advance")]

class Rect(C.Structure):
    _fields_ = [(x, C.c_int) for x in ("x", "y", "w", "h")]

class Canvas(C.Structure):
    _fields_ = [("pixels", C.POINTER(C.c_uint32)), ("width", C.c_int), ("height", C.c_int), ("clip", Rect)]

Painter = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_int, C.c_int, C.POINTER(Bitmap))

def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m

def tables(path):
    data = Path(path).read_bytes()
    n, = struct.unpack_from(">H", data, 4)
    result = {}
    for i in range(n):
        tag, _, off, length = struct.unpack_from(">4sIII", data, 12 + 16 * i)
        result[tag.decode()] = data[off:off + length]
    return result

def u16(data, off): return struct.unpack_from(">H", data, off)[0]
def s16(data, off): return struct.unpack_from(">h", data, off)[0]
def u32(data, off): return struct.unpack_from(">I", data, off)[0]

def name(t, id):
    records = []
    for i in range(u16(t, 2)):
        platform, enc, lang, nid, length, off = struct.unpack_from(">6H", t, 6 + i * 12)
        if nid == id and (platform == 0 or platform == 3 and enc in (1, 10)):
            score = 3 if platform == 3 and lang == 0x409 else 2 if platform == 3 else 1
            records.append((score, t[u16(t, 4) + off:u16(t, 4) + off + length].decode("utf-16-be")))
    return max(records)[1]

def cmap(t, cp):
    offsets = []
    for i in range(u16(t, 2)):
        platform, encoding, off = struct.unpack_from(">HHI", t, 4 + i * 8)
        if platform == 0 or platform == 3 and encoding in (1, 10): offsets.append(off)
    off = offsets[-1]; fmt = u16(t, off)
    if fmt == 12:
        for i in range(u32(t, off + 12)):
            first, last, start = struct.unpack_from(">III", t, off + 16 + i * 12)
            if first <= cp <= last: return start + cp - first
        return 0
    assert fmt == 4
    if cp > 0xffff: return 0
    count = u16(t, off + 6) // 2
    for i in range(count):
        end = u16(t, off + 14 + i * 2); start = u16(t, off + 16 + count * 2 + i * 2)
        if start <= cp <= end:
            delta = s16(t, off + 16 + count * 4 + i * 2)
            at = off + 16 + count * 6 + i * 2; offset = u16(t, at)
            if not offset: return (cp + delta) & 0xffff
            glyph = u16(t, at + offset + (cp - start) * 2)
            return ((glyph + delta) & 0xffff) if glyph else 0
    return 0

def hmetrics(t, glyph):
    count = u16(t["hhea"], 34); hmtx = t["hmtx"]
    if glyph < count: return u16(hmtx, glyph * 4), s16(hmtx, glyph * 4 + 2)
    return u16(hmtx, (count - 1) * 4), s16(hmtx, count * 4 + (glyph - count) * 2)

def kerning(t, left, right):
    kern = t.get("kern")
    if not kern or u16(kern, 2) == 0 or u16(kern, 8) != 1: return 0
    for i in range(u16(kern, 10)):
        a, b, value = struct.unpack_from(">HHh", kern, 18 + i * 6)
        if (a, b) == (left, right): return value
    return 0

def scale(units, size, upem):
    value = units * size
    return (1 if value >= 0 else -1) * ((abs(value) + upem // 2) // upem)

class FontTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent = ROOT / "build/host"; parent.mkdir(parents=True, exist_ok=True)
        (parent / "f2-63").mkdir(exist_ok=True)
        (ROOT / "build/codex").mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix="font-", dir=parent)
        cls.folder = Path(cls.temp.name)
        cls.env = dict(os.environ, TMPDIR=cls.temp.name, ASAN_OPTIONS="detect_leaks=0")
        cls.ft = module("font_freetype", ROOT / "sdk/freetype/build.py")
        cls.ft_library = cls.ft.host()
        binary = cls.folder / "libfont.so"
        subprocess.run(["clang", "-std=c17", "-O2", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", "-pthread",
                        "-idirafter", str(ROOT / "sdk/sysroot-overlay/include"), "-I", str(ROOT / "src/kernel/include"),
                        *cls.ft.includes(), str(ROOT / "sdk/libfont/font.c"), str(ROOT / "apps/libdeskui/text_ttf.c"), str(cls.ft_library), "-lm", "-o", str(binary)], check=True, env=cls.env)
        cls.lib = C.CDLL(str(binary), use_errno=True)
        definitions = {
            "open": [C.c_char_p, C.POINTER(C.c_void_p)], "close": [C.c_void_p],
            "get_metrics": [C.c_void_p, C.POINTER(Metrics)], "lookup": [C.c_void_p, C.c_uint32, C.POINTER(C.c_uint)],
            "get_size_metrics": [C.c_void_p, C.c_uint, C.POINTER(SizeMetrics)],
            "hmetrics": [C.c_void_p, C.c_uint, C.POINTER(C.c_int), C.POINTER(C.c_int)],
            "raster": [C.c_void_p, C.c_uint, C.c_uint, C.POINTER(Bitmap)], "bitmap_free": [C.POINTER(Bitmap)],
            "measure": [C.c_void_p, C.c_uint, C.c_char_p, C.c_size_t, C.POINTER(C.c_int32)],
            "paint": [C.c_void_p, C.c_uint, C.c_char_p, C.c_size_t, C.c_int, C.c_int, Painter, C.c_void_p, C.POINTER(C.c_int32)],
            "cache_info": [C.c_void_p, C.POINTER(Stats)], "cache_clear": [C.c_void_p]}
        for function, args in definitions.items():
            f = getattr(cls.lib, "ciuki_font_" + function); f.argtypes = args
            f.restype = None if function in ("close", "bitmap_free", "cache_clear") else C.c_int
        cls.lib.du_text_face.argtypes = [C.POINTER(Canvas), C.c_void_p, C.c_uint, C.c_int, C.c_int, C.c_char_p, C.c_uint32]
        cls.lib.du_text_ttf.argtypes = [C.POINTER(Canvas), C.c_char_p, C.c_uint, C.c_int, C.c_int, C.c_char_p, C.c_uint32]

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def face(self, path):
        face = C.c_void_p()
        self.assertEqual(self.lib.ciuki_font_open(os.fsencode(path), C.byref(face)), 0)
        self.addCleanup(self.lib.ciuki_font_close, face)
        return face

    def glyph(self, face, cp):
        result = C.c_uint(); self.assertEqual(self.lib.ciuki_font_lookup(face, cp, C.byref(result)), 0); return result.value

    def raster(self, face, cp, size):
        b = Bitmap(); self.assertEqual(self.lib.ciuki_font_raster(face, self.glyph(face, cp), size, C.byref(b)), 0)
        try:
            return {"width": b.width, "height": b.height, "left": b.left, "top": b.top, "advance": b.advance}, C.string_at(b.coverage, b.width * b.height)
        finally: self.lib.ciuki_font_bitmap_free(C.byref(b))

    def measure(self, face, text, size=853):
        advance = C.c_int32(); self.assertEqual(self.lib.ciuki_font_measure(face, size, text, len(text), C.byref(advance)), 0); return advance.value

    def stats(self, face):
        s = Stats(); self.assertEqual(self.lib.ciuki_font_cache_info(face, C.byref(s)), 0); return s

    def test_metrics_names_cmap_hmtx_against_tables(self):
        for file in sorted(FONTS.glob("*.ttf")):
            with self.subTest(file=file.name):
                t = tables(file); face = self.face(file); m = Metrics()
                self.assertEqual(self.lib.ciuki_font_get_metrics(face, C.byref(m)), 0)
                self.assertEqual((m.ascent, m.descent, m.line_gap), struct.unpack_from(">3h", t["hhea"], 4))
                self.assertEqual(m.units_per_em, u16(t["head"], 18)); self.assertEqual(m.glyph_count, u16(t["maxp"], 4))
                self.assertEqual(m.family.decode(), name(t["name"], 1)); self.assertEqual(m.style.decode(), name(t["name"], 2))
                self.assertIn("2.1.5", name(t["name"], 5)); self.assertTrue(m.has_os2)
                self.assertEqual((m.typo_ascent, m.typo_descent, m.typo_line_gap, m.win_ascent, m.win_descent), struct.unpack_from(">3h2H", t["OS/2"], 68))
                self.assertEqual((m.weight, m.selection), (u16(t["OS/2"], 4), u16(t["OS/2"], 62)))
                for cp in (0, 32, 65, 86, 0xe9, 0xc5, 0x2026, 0x20ac, 0x1f600, 0x10ffff):
                    g = self.glyph(face, cp); self.assertEqual(g, cmap(t["cmap"], cp))
                    advance, bearing = C.c_int(), C.c_int()
                    self.assertEqual(self.lib.ciuki_font_hmetrics(face, g, C.byref(advance), C.byref(bearing)), 0)
                    self.assertEqual((advance.value, bearing.value), hmetrics(t, g))
                advance, bearing = C.c_int(), C.c_int()
                self.assertEqual(self.lib.ciuki_font_hmetrics(face, m.glyph_count - 1, C.byref(advance), C.byref(bearing)), 0)
                self.assertEqual((advance.value, bearing.value), hmetrics(t, m.glyph_count - 1))

    def test_kerning_measure_and_paint_agree(self):
        observed_nonzero = False
        for file in sorted(FONTS.glob("*.ttf")):
            face = self.face(file); t = tables(file); upem = u16(t["head"], 18)
            text = "AVATAR To Wa café Åé €"; glyphs = [cmap(t["cmap"], ord(c)) for c in text]
            pairs = [kerning(t, a, b) for a, b in zip(glyphs, glyphs[1:])]
            observed_nonzero |= any(pairs)
            advances = [self.raster(face, ord(c), 853)[0]["advance"] for c in text]
            expected = sum(advances) + sum(scale(k, 853, upem) for k in pairs)
            self.assertEqual(self.measure(face, text.encode()), expected)
            painted = []
            @Painter
            def painter(ctx, x, y, bitmap):
                b = bitmap.contents; painted.append((x, y, b.left, b.top)); return 0
            out = C.c_int32(); data = text.encode()
            self.assertEqual(self.lib.ciuki_font_paint(face, 853, data, len(data), 5, 30, painter, None, C.byref(out)), 0)
            self.assertEqual(out.value, expected); pen = 0; positions = []
            for i, g in enumerate(glyphs):
                if i: pen += scale(pairs[i - 1], 853, upem)
                positions.append(5 + scale(pen, 1, 64)); pen += advances[i]
            # Space produces advance but no callback.
            self.assertEqual([x - left for x, y, left, top in painted], [x for c, x in zip(text, positions) if c != " "])
            self.assertTrue(all(y - top == 30 for x, y, left, top in painted))
        self.assertTrue(observed_nonzero)
        source = FONTS / "LiberationSans-Regular.ttf"; data = bytearray(source.read_bytes())
        for i in range(u16(data, 4)):
            at = 12 + i * 16
            if data[at:at + 4] == b"kern": data[at:at + 4] = b"NONE"
        file = self.folder / "no-kern.ttf"; file.write_bytes(data); face = self.face(file); t = tables(file)
        self.assertEqual(self.measure(face, b"AV"), sum(self.raster(face, cp, 853)[0]["advance"] for cp in b"AV"))

    def test_utf8_edges_and_missing_glyph(self):
        face = self.face(FONTS / "LiberationSans-Regular.ttf")
        replacement = self.measure(face, "\ufffd".encode())
        for bad, count in [(b"\x80", 1), (b"\xc0\xaf", 2), (b"\xe2\x82", 2), (b"\xed\xa0\x80", 3),
                           (b"\xf4\x90\x80\x80", 4), (b"\xf0\x80\x80\xaf", 4), (b"\xff", 1), (b"\xc2", 1)]:
            self.assertEqual(self.measure(face, bad), replacement * count)
        self.assertEqual(self.measure(face, b"\xe2A"), replacement + self.measure(face, b"A"))
        self.assertEqual(self.measure(face, b"A\0V"), sum(self.measure(face, bytes([c])) for c in b"A\0V"))
        self.assertEqual(self.measure(face, b""), 0)
        self.assertEqual(self.glyph(face, 0x10ffff), 0)
        for cp in (0xd800, 0xdfff, 0x110000, 0xffffffff):
            out = C.c_uint(); self.assertEqual(self.lib.ciuki_font_lookup(face, cp, C.byref(out)), -1)
        for size in (0, 63, 8193, 0xffffffff):
            b = Bitmap(); self.assertEqual(self.lib.ciuki_font_raster(face, 1, size, C.byref(b)), -1)
        self.assertEqual(self.lib.ciuki_font_open(None, C.byref(C.c_void_p())), -1)
        corrupt = self.folder / "broken.ttf"; corrupt.write_bytes(b"\0" * 12)
        self.assertEqual(self.lib.ciuki_font_open(os.fsencode(corrupt), C.byref(C.c_void_p())), -1)
        data = bytearray((FONTS / "LiberationSans-Regular.ttf").read_bytes()); struct.pack_into(">I", data, 20, 0xffffffff)
        corrupt.write_bytes(data); self.assertEqual(self.lib.ciuki_font_open(os.fsencode(corrupt), C.byref(C.c_void_p())), -1)

    def test_coverage_goldens(self):
        record = json.loads(GOLDENS.read_text())
        self.assertEqual(record["per_pixel_tolerance"], 1)
        faces = {}
        for item in record["glyphs"]:
            file = item["font"]
            if file not in faces:
                self.assertEqual(hashlib.sha256((FONTS / file).read_bytes()).hexdigest(), record["fonts"][file])
                faces[file] = self.face(FONTS / file)
            actual, coverage = self.raster(faces[file], item["codepoint"], item["em_26_6"])
            self.assertEqual(actual, item["metrics"])
            expected = zlib.decompress(base64.b64decode(item["coverage_zlib_base64"]))
            self.assertEqual(len(coverage), len(expected))
            self.assertLessEqual(max((abs(a - b) for a, b in zip(coverage, expected)), default=0), 1, item)

    def test_scaled_metrics_and_hinted_advances_at_8_through_16_pixels(self):
        for file in sorted(FONTS.glob("*.ttf")):
            face = self.face(file); t = tables(file); upem = u16(t["head"], 18)
            for px in range(8, 17):
                with self.subTest(font=file.name, px=px):
                    m = SizeMetrics()
                    self.assertEqual(self.lib.ciuki_font_get_size_metrics(face, px * 64, C.byref(m)), 0)
                    self.assertEqual((m.ascent, m.descent, m.line_gap), tuple(scale(v, px * 64, upem) for v in struct.unpack_from(">3h", t["hhea"], 4)))
                    self.assertEqual(m.max_advance, scale(u16(t["hhea"], 10), px * 64, upem))
                    for cp in range(32, 127):
                        metrics, _ = self.raster(face, cp, px * 64)
                        self.assertEqual(metrics["advance"] % 64, 0)
                        # v35 phantom-point fitting can move narrow/wide glyphs
                        # by more than 2 px; the measured maximum is 164/64 px.
                        self.assertLessEqual(abs(metrics["advance"] - scale(hmetrics(t, cmap(t["cmap"], cp))[0], px * 64, upem)), 3 * 64)
            m = SizeMetrics()
            self.assertEqual(self.lib.ciuki_font_get_size_metrics(face, 853, C.byref(m)), 0)
            self.assertEqual(m.ascent, scale(s16(t["hhea"], 4), 853, upem))
            self.assertEqual(self.lib.ciuki_font_get_size_metrics(face, 63, C.byref(m)), -1)

    def test_hinting_difference_budget_against_stb_baseline(self):
        record = json.loads(GOLDENS.with_name("coverage-stb-baseline.json").read_text())
        faces = {}; observed = []
        for item in record["glyphs"]:
            file = item["font"]
            self.assertEqual(hashlib.sha256((FONTS / file).read_bytes()).hexdigest(), record["fonts"][file])
            if file not in faces: faces[file] = self.face(FONTS / file)
            new, coverage = self.raster(faces[file], item["codepoint"], item["em_26_6"])
            old = item["metrics"]; before = zlib.decompress(base64.b64decode(item["coverage_zlib_base64"]))
            self.assertLessEqual(abs(new["advance"] - old["advance"]), 96, item)
            for field in ("left", "top", "width", "height"):
                self.assertLessEqual(abs(new[field] - old[field]), 3, (field, item))
            x0 = min(old["left"], new["left"]); y0 = min(old["top"], new["top"])
            x1 = max(old["left"] + old["width"], new["left"] + new["width"])
            y1 = max(old["top"] + old["height"], new["top"] + new["height"])
            def pixel(m, data, x, y):
                x -= m["left"]; y -= m["top"]
                return data[y * m["width"] + x] if 0 <= x < m["width"] and 0 <= y < m["height"] else 0
            error = sum(abs(pixel(old, before, x, y) - pixel(new, coverage, x, y)) for y in range(y0, y1) for x in range(x0, x1))
            mean = error / max(1, (x1 - x0) * (y1 - y0))
            # 25% of the full coverage range, individually per baseline-aligned
            # glyph. Hinting intentionally shifts entire stems by a pixel.
            self.assertLessEqual(mean, 63.75, item)
            observed.append(dict(font=file, cp=item["codepoint"], size=item["em_26_6"], mean_error=round(mean, 4),
                                 advance_delta=abs(new["advance"] - old["advance"]),
                                 box_delta=max(abs(new[k] - old[k]) for k in ("left", "top", "width", "height"))))
        (ROOT / "build/host/f2-63/parity.json").write_text(json.dumps(observed, indent=2) + "\n")

    def test_cache_bounds_owned_snapshots_and_threads(self):
        face = self.face(FONTS / "LiberationSerif-Italic.ttf")
        b = Bitmap(); self.assertEqual(self.lib.ciuki_font_raster(face, self.glyph(face, ord("A")), 853, C.byref(b)), 0)
        original = C.string_at(b.coverage, b.width * b.height)
        for cp in range(32, 232): self.raster(face, cp, 853)
        s = self.stats(face); self.assertEqual(s.glyphs, 128); self.assertGreater(s.evictions, 0); self.assertLessEqual(s.bytes, 256 * 1024)
        self.lib.ciuki_font_cache_clear(face); self.assertEqual(C.string_at(b.coverage, b.width * b.height), original)
        self.lib.ciuki_font_bitmap_free(C.byref(b)); self.assertEqual(self.stats(face).bytes, 0)
        for cp in range(33, 160):
            self.raster(face, cp, 8192)
            self.assertLessEqual(self.stats(face).bytes, 256 * 1024)
        self.assertLess(self.stats(face).glyphs, 128)  # byte limit evicts before entry limit
        self.lib.ciuki_font_cache_clear(face)
        def by_id(glyph):
            bitmap = Bitmap()
            self.assertEqual(self.lib.ciuki_font_raster(face, glyph, 853, C.byref(bitmap)), 0)
            self.lib.ciuki_font_bitmap_free(C.byref(bitmap))
        for glyph in range(128): by_id(glyph)
        by_id(0); by_id(128)  # the used entry survives, glyph 1 is least recent
        misses = self.stats(face).misses
        by_id(0); self.assertEqual(self.stats(face).misses, misses)
        by_id(1); self.assertEqual(self.stats(face).misses, misses + 1)
        self.lib.ciuki_font_cache_clear(face)
        other = self.face(FONTS / "LiberationSerif-Italic.ttf")
        def work(index):
            for i in range(100): self.raster(face, 65 + i % 26, 853 if index & 1 else 1195)
            return self.measure(face, b"Thread safe face")
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            widths = list(pool.map(work, range(4)))
        expected = {(self.glyph(face, cp), size) for cp in range(65, 91) for size in (853, 1195)}
        expected.update((self.glyph(face, cp), 853) for cp in b"Thread safe face")
        self.assertEqual(len(set(widths)), 1); self.assertEqual(self.stats(face).glyphs, len(expected)); self.assertEqual(self.stats(other).glyphs, 0)

    def test_integer_blend_and_canvas_clip(self):
        file = FONTS / "LiberationSans-Regular.ttf"; face = self.face(file)
        geometry, coverage = self.raster(face, 65, 853)
        width, height = 48, 40; background = 0xa52f7391; foreground = 0xabcdef03
        pixels = (C.c_uint32 * (width * height))(*([background] * (width * height)))
        c = Canvas(pixels, width, height, Rect(2, 4, 19, 17))
        self.assertEqual(self.lib.du_text_face(C.byref(c), face, 853, -2, 23, b"A", foreground), 0)
        for y in range(height):
            for x in range(width):
                gx, gy = x - (-2 + geometry["left"]), y - (23 + geometry["top"])
                a = coverage[gy * geometry["width"] + gx] if 2 <= x < 21 and 4 <= y < 21 and 0 <= gx < geometry["width"] and 0 <= gy < geometry["height"] else 0
                expected = sum(((((foreground >> s) & 255) * a + ((background >> s) & 255) * (255 - a) + 127) // 255) << s for s in (0, 8, 16)) if a else background
                self.assertEqual(pixels[y * width + x], expected)
        before = list(pixels); c.clip = Rect(-10, -10, 100, 100)
        self.assertEqual(self.lib.du_text_ttf(C.byref(c), os.fsencode(file), 853, 0x7fffffff, -0x7fffffff, b"A", foreground), -1)
        self.assertEqual(list(pixels), before)
        c.clip = Rect(0, 0, width, height)
        self.assertEqual(self.lib.du_text_ttf(C.byref(c), os.fsencode(file), 853, 20, 25, b"A", foreground), 0)

    def test_installed_inventory_hashes_and_alias_contract(self):
        image = module("font_image_inventory", ROOT / "scripts/build_image.py")
        sources, metadata = image.font_payloads(); record = json.loads((FONTS / "UPSTREAM.json").read_text())
        self.assertEqual(len(sources), 15); self.assertEqual(len(record["files"]), 12)
        for family in ("Sans", "Serif", "Mono"):
            for style in ("Regular", "Bold", "Italic", "BoldItalic"):
                filename = f"Liberation{family}-{style}.ttf"
                self.assertEqual(metadata["files"]["/System/fonts/" + filename], record["files"][filename])
        self.assertEqual(metadata["bytes"], sum(p.stat().st_size for p in sources.values()))
        payloads = image.payload_records(sources)
        for entry in payloads: self.assertEqual(entry["sha256"], metadata["files"][entry["path"]])
        contract = (ROOT / "docs/design/fonts-contract.md").read_text()
        for alias, family in (("Arial", "Sans"), ("Times New Roman", "Serif"), ("Courier New", "Mono")):
            self.assertIn(f"| {alias} | Liberation {family} |", contract)
        self.assertIn("SIL OPEN FONT LICENSE", (FONTS / "LICENSE").read_text())

    def test_control_panel_through_desktop_harness(self):
        toolkit = module("fonts_test_toolkit", ROOT / "tests/host/desktop/test_deskui.py")
        icons = module("fonts_test_icons", ROOT / "tests/host/desktop/icon_assets.py")
        sources = ["tests/host/desktop/fonts_control_test.c", "apps/control/app.c", "apps/desktop/power_policy.c", "apps/control/driver_pack_ui.c", "apps/control/display.c", "apps/control/fonts.c",
                   "sdk/libfont/font.c", "apps/libdeskui/text_ttf.c", "apps/desktop/protocol.c", "apps/desktop/settings.c",
                   "apps/desktop/sounds.c", "apps/desktop/wallpaper.c", "apps/desktop/image.c", *toolkit.TOOLKIT]
        binary = self.folder / "fonts-control"
        subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-D_POSIX_C_SOURCE=200809L",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-pthread", *icons.defines(),
                        "-I", str(ROOT / "apps/control"), "-I", str(ROOT / "src/kernel/include"),
                        "-idirafter", str(ROOT / "sdk/sysroot-overlay/include"), *self.ft.includes(), *(str(ROOT / s) for s in sources), str(self.ft_library), "-lm", "-o", str(binary)], check=True, env=self.env)
        frame = self.folder / "fonts.xrgb"
        result = subprocess.run([binary, FONTS, frame], check=True, capture_output=True, text=True, env=self.env)
        self.assertIn("PASS control fonts", result.stdout)
        self.assertEqual(frame.stat().st_size, 560 * 420 * 4)
        capture("control-fonts", frame.read_bytes(), 560, 420)
        from PIL import Image
        Image.frombytes("RGB", (560, 420), frame.read_bytes(), "raw", "BGRX").save(ROOT / "build/host/f2-63/control-fonts.png")

    def test_host_benchmark(self):
        binary = self.folder / "benchmark"
        subprocess.run(["clang", "-std=c17", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread", "-idirafter", str(ROOT / "sdk/sysroot-overlay/include"),
                        *self.ft.includes(), str(ROOT / "tests/host/font/benchmark.c"), str(ROOT / "sdk/libfont/font.c"), str(self.ft_library), "-lm", "-o", str(binary)], check=True, env=self.env)
        result = subprocess.run([binary, FONTS / "LiberationSans-Regular.ttf"], check=True, text=True, capture_output=True, env=self.env)
        data = json.loads(result.stdout); data.update(host=platform.machine(), measurement="host CPU, no display blending, not Pentium III evidence")
        self.assertGreater(data["cold_us_per_glyph"], 0); self.assertGreater(data["warm_us_per_glyph"], 0)
        (ROOT / "build/codex/f2-63-benchmark.json").write_text(json.dumps(data, indent=2) + "\n")
        print("FONT_BENCHMARK", json.dumps(data, sort_keys=True))

    def test_open_reads_face_in_one_request(self):
        """fx-11, T23 trial 423b803a: NetSurf's first layout spent 2,117,000 of
        2,175,000 us in two ciuki_font_open calls (Sans Regular and Bold).
        Through the guest's newlib stdio (st_blksize = 4 KiB FAT32 cluster) the
        old open made one read(2) per cluster: 101 + 102 = 203 storage
        requests, about 10.4 ms each on the T23. The face must arrive in one
        request per 1 MiB (CIUKI_IO_MAX), without stdio."""
        binary = self.folder / "open-io"
        subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                        "-idirafter", str(ROOT / "sdk/sysroot-overlay/include"), *self.ft.includes(),
                        str(ROOT / "tests/host/font/open_io.c"), str(ROOT / "sdk/libfont/font.c"), str(self.ft_library),
                        "-Wl,--wrap=read,--wrap=fread", "-lm", "-o", str(binary)], check=True, env=self.env)
        faces = [FONTS / "LiberationSans-Regular.ttf", FONTS / "LiberationSans-Bold.ttf"]
        result = subprocess.run([binary, *faces], text=True, capture_output=True, env=self.env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = json.loads(result.stdout)
        guest_requests = sum(-(-path.stat().st_size // data["guest_blksize"]) for path in faces)
        self.assertEqual(guest_requests, 203)  # the old stdio pattern on the guest
        for face, path in zip(data["faces"], faces):
            size = path.stat().st_size
            with self.subTest(face=path.name):
                self.assertEqual(face["stdio_freads"], 0, face)
                self.assertEqual(face["bytes_read"], size, face)
                self.assertLessEqual(face["reads"], -(-size // (1024 * 1024)), face)
                self.assertGreaterEqual(face["largest_read"], size, face)
        print("FONT_OPEN_IO", json.dumps(dict(data, old_guest_requests=guest_requests,
              t23_us_per_old_request=round(2117000 / guest_requests))))

    def test_heap_budget_allocation_failures_and_warm_paint(self):
        binary = self.folder / "memory"
        subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                        "-idirafter", str(ROOT / "sdk/sysroot-overlay/include"), *self.ft.includes(),
                        str(ROOT / "tests/host/font/memory.c"), str(ROOT / "sdk/libfont/font.c"), str(self.ft_library),
                        "-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free", "-o", str(binary)], check=True, env=self.env)
        result = subprocess.run([binary, FONTS / "LiberationSans-Regular.ttf"], text=True, capture_output=True, env=self.env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = json.loads(result.stdout)
        self.assertEqual(data["leaked_bytes"], 0); self.assertEqual(data["warm_paint_allocations"], 0)
        self.assertLess(data["peak_live_bytes"], 2 * 1024 * 1024)  # trusted Liberation face + interpreter + coverage + owned snapshot
        (ROOT / "build/host/f2-63/memory.json").write_text(json.dumps(data, indent=2) + "\n")

def prepare():
    FontTests.setUpClass(); test = FontTests()
    try:
        glyphs = []; hashes = {}
        for file in sorted(FONTS.glob("*.ttf")):
            face = test.face(file); hashes[file.name] = hashlib.sha256(file.read_bytes()).hexdigest()
            for size in (683, 853, 1024, 1195):
                for cp in (ord("A"), ord("g"), ord("j"), 0xc5, 0xe9, 0x10ffff):
                    metrics, coverage = test.raster(face, cp, size)
                    glyphs.append(dict(font=file.name, codepoint=cp, em_26_6=size, metrics=metrics,
                                       coverage_zlib_base64=base64.b64encode(zlib.compress(coverage)).decode()))
        GOLDENS.write_text(json.dumps(dict(rasterizer="FreeType 2.13.3 normal grey v35", per_pixel_tolerance=1, fonts=hashes, glyphs=glyphs), indent=2) + "\n")
    finally:
        test.doCleanups(); FontTests.tearDownClass()

if __name__ == "__main__":
    (ROOT / "build/codex").mkdir(parents=True, exist_ok=True)
    if sys.argv[1:] == ["--prepare-goldens"]: prepare()
    else: unittest.main()
