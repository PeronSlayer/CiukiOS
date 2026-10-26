"""Bounded modern CSS parsing and real framebuffer rendering, offline."""
import hashlib
import json
import os
import re
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from test_local import recipe

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent


class ModernColourTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        recipe().build(host=True)
        cls.base = ROOT / "build/external/netsurf-host"
        cls.out = ROOT / "build/host/netsurf-modern"
        cls.out.mkdir(parents=True, exist_ok=True)

    def test_parser(self):
        prefix = self.base / "prefix"
        source = self.base / "source/libcss/src"
        binary = self.out / "css-colours"
        # Instrument the changed translation unit, not just the test driver.
        subprocess.run([
            "clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            # The pinned private stylesheet header ends css_rule with the
            # otherwise undefined declarator _ALIGNED. Do not emit a global
            # variable from each of these two instrumented translation units.
            "-D_ALIGNED=",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-I" + str(prefix / "include"), "-I" + str(source),
            str(HERE / "css_colour_test.c"),
            str(source / "parse/properties/utils.c"),
            "-L" + str(prefix / "lib"), "-lcss", "-lparserutils", "-lwapcaplet",
            "-o", str(binary)
        ], check=True)
        result = subprocess.run([binary], capture_output=True, text=True, timeout=30,
                                env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1",
                                         UBSAN_OPTIONS="halt_on_error=1"))
        print(result.stdout, end="")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_render(self):
        modern = (HERE / "fixtures/modern-colours.html").read_text()
        legacy = modern
        replacements = {
            "rgb(255 0 0)": "rgb(255,0,0)",
            "rgb(0 100% 0)": "rgb(0,255,0)",
            # The old comma grammar truncates its fixed-point channels.
            # These values produce the same byte alpha (128 and 136).
            "rgba(0 0 255 / 50%)": "rgba(0,0,255,0.503)",
            "#f008": "rgba(255,0,0,0.534)",
            "#112233ff": "#112233",
            "light-dark(rgb(255 255 0), #0000)": "#ffff00",
        }
        for original, replacement in replacements.items():
            self.assertIn(original, legacy)
            legacy = legacy.replace(original, replacement)
        captures = []
        with tempfile.TemporaryDirectory(dir=self.out) as directory:
            for name, html in (("modern", modern), ("legacy", legacy)):
                page = Path(directory) / (name + ".html")
                capture = Path(directory) / (name + ".xrgb")
                page.write_text(html)
                result = subprocess.run([
                    self.base / "stage/Programs/netsurf", "-w", "640", "-h", "480",
                    "-f", "ram", "--ciuki-resources=" + str(self.base / "stage/system/netsurf"),
                    "--ciuki-profile=" + str(Path(directory) / "profile"),
                    "--ciuki-capture=" + str(capture), page.as_uri()
                ], capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertNotIn("layout rejected", result.stderr)
                raw = capture.read_bytes()
                self.assertEqual(len(raw), 640 * 480 * 4)
                captures.append(raw)
        self.assertEqual(captures[0], captures[1], "modern colors differ from legacy equivalents")
        pixels = [p[0] & 0xffffff for p in struct.iter_unpack("<I", captures[0])]
        for color in (0xff0000, 0x00ff00, 0x112233, 0xffff00):
            self.assertGreater(pixels.count(color), 500, hex(color))
        digest = hashlib.sha256(captures[0]).hexdigest()
        print("modern-colours sha256", digest)
        golden = json.loads((HERE / "goldens.json").read_text())["modern-colours"]
        self.assertEqual(digest, golden)
        print("PASS modern CSS colors: real framebuffer matches legacy equivalents and golden")


class CustomPropertyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        recipe().build(host=True)
        cls.base = ROOT / "build/external/netsurf-host"
        cls.out = ROOT / "build/host/netsurf-modern"
        cls.out.mkdir(parents=True, exist_ok=True)

    def test_cascade(self):
        css = self.base / "source/libcss"
        prefix = self.base / "prefix"
        driver = self.out / "custom-driver.c"
        # Reuse upstream's DOM/select callbacks without vendoring its driver.
        driver.write_text((css / "test/select.c").read_text().replace(
            "int main(int argc, char **argv)", "int upstream_main(int argc, char **argv)")
            + "\n" + (HERE / "css_custom_test.c").read_text())
        binary = self.out / "css-custom"
        units = ("stylesheet.c", "select/computed.c", "parse/language.c", "parse/important.c",
                 "parse/custom.c", "select/select.c", "select/custom.c",
                 "parse/properties/utils.c", "lex/lex.c", "parse/parse.c")
        subprocess.run([
            "clang", "-std=c17", "-D_DEFAULT_SOURCE", "-D_ALIGNED=", "-O1", "-g",
            "-include", "strings.h", "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all", "-I" + str(css / "src"),
            "-I" + str(css / "test"), "-I" + str(HERE), "-I" + str(prefix / "include"), str(driver),
            *[str(css / "src" / unit) for unit in units],
            "-L" + str(prefix / "lib"), "-lcss", "-lparserutils", "-lwapcaplet",
            "-lm", "-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc", "-o", str(binary)
        ], check=True)
        result = subprocess.run([binary], capture_output=True, text=True, timeout=60,
                                env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1",
                                         UBSAN_OPTIONS="halt_on_error=1"))
        print(result.stdout, end="", flush=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def render(self, html, width=640, height=480, scripts=None):
        with tempfile.TemporaryDirectory(dir=self.out) as directory:
            directory = Path(directory)
            page, capture = directory / "page.html", directory / "page.xrgb"
            page.write_text(html)
            for name, source in (scripts or {}).items():
                (directory / name).write_text(source)
            result = subprocess.run([
                self.base / "stage/Programs/netsurf", "-w", str(width), "-h", str(height),
                "-f", "ram", "--ciuki-resources=" + str(self.base / "stage/system/netsurf"),
                "--ciuki-profile=" + str(directory / "profile"),
                "--ciuki-capture=" + str(capture), page.as_uri()
            ], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn("layout rejected", result.stderr)
            self.assertNotIn("NetSurf error page:", result.stderr)
            raw = capture.read_bytes()
            self.assertEqual(len(raw), width * height * 4)
            return raw, result.stderr

    def test_layout_subset(self):
        html = (HERE / "fixtures/modern-layout.html").read_text()
        raw, log = self.render(html)
        def pixel(x, y):
            return struct.unpack_from("<I", raw, (y * 640 + x) * 4)[0] & 0xffffff
        origin = next(y for y in range(64) if pixel(5, y) == 0x78e6a3)
        for x, y, color in ((5, 5, 0x78e6a3), (45, 5, 0xfafbf8),
                            (53, 5, 0x110e35), (5, 25, 0xfafbf8),
                            (5, 35, 0xddf0e2), (5, 55, 0x78e6a3),
                            (5, 75, 0xfafbf8), (5, 85, 0x110e35)):
            self.assertEqual(pixel(x, y + origin), color, (x, y + origin))
        # Independent legacy color values + explicit block layout for the
        # permitted grid fallback, retaining the unsupported track/span rules.
        equivalent = html.replace("display: grid", "display: block")
        equivalent = equivalent.replace("var(--paper)", "#fafbf8").replace("var(--ink)", "#110e35")
        equivalent = equivalent.replace("var(--accent)", "#78e6a3").replace("var(--space)", "8px")
        equivalent = equivalent.replace("color-mix(in srgb, var(--surface) 84%, #78e6a3)", "#ddf0e2")
        equivalent = equivalent.replace("var(--surface)", "#f0f2ee")
        oracle, _ = self.render(equivalent)
        self.assertEqual(raw, oracle, "grid fallback or computed variable/mix colors differ")
        golden = json.loads((HERE / "goldens.json").read_text())["modern-layout"]
        self.assertEqual(hashlib.sha256(raw).hexdigest(), golden)
        print("PASS variables/mix/flex gaps and readable grid block fallback", flush=True)

    def test_flex_gap_axes(self):
        cases = (
            ("row", "wrap", 100, "auto", 40, ((0, 0), (48, 0), (0, 30))),
            ("row-reverse", "wrap", 100, "auto", 40, ((60, 0), (12, 0), (60, 30))),
            ("row", "wrap-reverse", 100, "50px", 40, ((0, 30), (48, 30), (0, 0))),
            ("column", "nowrap", 40, "100px", 20, ((0, 0), (0, 30), (0, 60))),
            ("column-reverse", "nowrap", 40, "100px", 20, ((0, 80), (0, 50), (0, 20))),
            ("column", "wrap", 88, "50px", 20, ((0, 0), (0, 30), (48, 0))),
        )
        for direction, wrap, width, height, basis, positions in cases:
            with self.subTest(direction=direction, wrap=wrap):
                html = ('<html><head><title>Gaps</title><style>html,body{margin:0;padding:0;background:white}'
                        f'.flex{{display:flex;flex-direction:{direction};flex-wrap:{wrap};'
                        f'width:{width}px;height:{height};gap:10px 8px}}'
                        f'.tile{{flex:0 0 {basis}px;width:40px;height:20px}}'
                        '.a{background:#ff0000}.b{background:#00ff00}.c{background:#0000ff}'
                        '</style></head><body><div class="flex"><div class="tile a"></div>'
                        '<div class="tile b"></div><div class="tile c"></div></div></body></html>')
                raw, _ = self.render(html)
                pixels = [x[0] & 0xffffff for x in struct.iter_unpack("<I", raw)]
                boxes = []
                for color in (0xff0000, 0x00ff00, 0x0000ff):
                    points = [(i % 640, i // 640) for i, p in enumerate(pixels) if p == color]
                    self.assertEqual(len(points), 800)
                    boxes.append((min(p[0] for p in points), min(p[1] for p in points)))
                self.assertEqual(boxes, list(positions), (direction, wrap))
        print("PASS flex gap rows/columns, both reverse directions and wrapping", flush=True)

    def test_module_scripts(self):
        effect = "document.write('<div style=\"height:40px;background:#ff0000\">module</div>');"
        html = ('<title>Module fixture</title><style>html,body{margin:0;background:white}</style>'
                '<script type="module">' + effect + '</script>'
                '<script type="MoDuLe" src="external.js"></script>'
                '<script nomodule>document.write(\'<div style="height:40px;background:#00ff00">classic</div>\');</script>')
        raw, log = self.render(html, scripts={"external.js": effect})
        pixels = [x[0] & 0xffffff for x in struct.iter_unpack("<I", raw)]
        self.assertEqual(pixels.count(0xff0000), 0, "a module executed as classic JavaScript")
        self.assertGreater(pixels.count(0x00ff00), 1000, "classic nomodule script did not execute")
        self.assertNotIn("Uncaught error in JS", log)
        print("PASS inline/external module ignored; classic nomodule script executes", flush=True)

    @unittest.skipUnless(os.environ.get("CIUKI_NETSURF_SITE_SNAPSHOTS"),
                         "requires the lead's untracked site snapshots")
    def test_saved_sites(self):
        source = Path(os.environ["CIUKI_NETSURF_SITE_SNAPSHOTS"])
        originals = {
            "www.alcybercloud.it/index.html": "a9407e925868bd124aa6963df1d61e73964458ca45a7afb7bc406b42e6d4bac6",
            "www.alcybercloud.it/app.css": "147b3b99ad8c8f6206ab85423c38962406473d2c0e4d13884231fba2d7896da7",
            "eduke32logs.alcybercloud.it/index.html": "901c1291502771a854d4654cddf88e3db472e41aa1dfb68169cb5ec493fe6f42",
        }
        for path, digest in originals.items():
            self.assertEqual(hashlib.sha256((source / path).read_bytes()).hexdigest(), digest, path)
        goldens = json.loads((HERE / "goldens.json").read_text())
        for host, colors in (("www.alcybercloud.it", (0x78e6a3, 0x110e35)),
                             ("eduke32logs.alcybercloud.it", (0x140c0a, 0xc2410c))):
            html = (source / host / "index.html").read_text()
            # Offline visual replay of supplied resources only. Scripts/widgets
            # and unavailable media are separate compatibility evidence.
            html = re.sub(r'<script\b[^>]*>.*?</script\s*>', '', html, flags=re.S | re.I)
            html = re.sub(r'\b(src|srcset|poster)\s*=\s*(["\']).*?\2',
                          r'\1="file:///nonexistent-ciuki-replay-resource"', html, flags=re.I | re.S)
            html = re.sub(r'<link\b[^>]*>', lambda m:
                          '<style>' + (source / host / 'app.css').read_text() + '</style>'
                          if 'rel="stylesheet"' in m[0] and (source / host / 'app.css').exists() else '',
                          html, flags=re.I)
            for width, height in ((800, 600), (1280, 800)):
                with self.subTest(host=host, width=width):
                    raw, log = self.render(html, width, height)
                    digest = hashlib.sha256(raw).hexdigest()
                    self.assertEqual(digest, goldens[f"site-{host}-{width}"])
                    pixels = [p[0] & 0xffffff for p in struct.iter_unpack("<I", raw)]
                    for color in colors:
                        self.assertGreater(pixels.count(color), 100, (host, hex(color)))
                    print(host, width, digest, flush=True)
                    print(next(line for line in log.splitlines() if "exit reason=" in line), flush=True)

    @unittest.skipUnless(os.environ.get("CIUKI_NETSURF_SITE_SNAPSHOTS"),
                         "requires the lead's untracked module bundle")
    def test_saved_bundle_syntax(self):
        bundle = Path(os.environ["CIUKI_NETSURF_SITE_SNAPSHOTS"]) / "www.alcybercloud.it/app.js"
        self.assertEqual(hashlib.sha256(bundle.read_bytes()).hexdigest(),
                         "b4ed8c863edb2e463e248da37d7131bea1abdb0b22d3aab602a682af6ce32ebc")
        source = self.base / "source/netsurf"
        engine = source / "content/handlers/javascript/duktape"
        binary = self.out / "js-compile"
        subprocess.run([
            "clang", "-std=c17", "-D_DEFAULT_SOURCE", "-DCIUKI_HOST", "-O1", "-g",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-I" + str(ROOT), "-I" + str(source), "-I" + str(source / "include"),
            "-I" + str(engine), str(HERE / "js_compile_test.c"),
            str(ROOT / "sdk/netsurf/port/js_memory.c"), str(engine / "duktape.c"),
            "-lm", "-o", str(binary)
        ], check=True)
        result = subprocess.run([binary, bundle], capture_output=True, text=True, timeout=60,
                                env=dict(os.environ, ASAN_OPTIONS="detect_leaks=1",
                                         UBSAN_OPTIONS="halt_on_error=1"))
        print(result.stdout, end="", flush=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
