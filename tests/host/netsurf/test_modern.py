"""Bounded modern CSS parsing and real framebuffer rendering, offline."""
import hashlib
import json
import os
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


if __name__ == "__main__":
    unittest.main()
