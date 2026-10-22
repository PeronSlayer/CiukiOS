# SPDX-License-Identifier: MIT
"""Sequential native CiukPaint harness; no guest, full image or variants."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
import importlib.util as _review_util  # optional PNG review copies (frame_review.py, any import mode)
_review_spec = _review_util.spec_from_file_location("frame_review", __file__.rsplit("/", 1)[0] + "/frame_review.py")
_review = _review_util.module_from_spec(_review_spec); _review_spec.loader.exec_module(_review)
capture = _review.capture

ROOT = Path(__file__).resolve().parents[3]
TOOLKIT = ["draw", "widgets", "list", "menu", "dialog", "textarea", "icons", "icon_map"]
# F2-99: nine generic-pointer goldens updated in integ-fix1; all other frames
# and all pixels outside the pointer footprints retain their former pins.


class PaintTests(unittest.TestCase):
    def test_application(self):
        parent = ROOT / "build/host"; parent.mkdir(parents=True, exist_ok=True)
        renders = ROOT / "build/f2-40"; renders.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="paint-", dir=parent) as temp:
            spec = importlib.util.spec_from_file_location("paint_icons", ROOT / "tests/host/desktop/icon_assets.py")
            icons = importlib.util.module_from_spec(spec); spec.loader.exec_module(icons)
            sources = [*(f"apps/libdeskui/{s}.c" for s in TOOLKIT), "apps/desktop/compositor.c", "apps/desktop/theme.c",
                       "apps/paint/paint.c", "apps/paint/io.c", "apps/paint/stb.c", "tests/host/desktop/paint_test.c"]
            binary = Path(temp) / "paint"
            flags = ["clang", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-D_POSIX_C_SOURCE=200809L",
                     "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-ffunction-sections", "-fdata-sections",
                     "-Wl,--gc-sections", "-I", str(ROOT / "src/kernel/include"), *icons.defines()]
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0")
            subprocess.run([*flags, *(str(ROOT / s) for s in sources), "-lm", "-o", str(binary)], check=True, env=env)
            one, two = Path(temp) / "one", Path(temp) / "two"
            one.mkdir(); two.mkdir()
            result = subprocess.run([str(binary), str(one), str(renders)], capture_output=True, text=True, env=env)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("paint: PASS", result.stdout)
            print(result.stdout.strip())
            first = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in renders.glob("paint-*.png")}
            for p in renders.glob("paint-*.png"):
                capture(p.stem, p.read_bytes())
            self.assertEqual(len(first), 7 + 15 + 8 + 3)
            self.assertEqual(first, json.loads((ROOT / "tests/host/desktop/paint-goldens.json").read_text()))
            result = subprocess.run([str(binary), str(two), str(renders)], capture_output=True, text=True, env=env)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("paint: PASS", result.stdout)
            self.assertEqual(first, {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in renders.glob("paint-*.png")})
            (renders / "renders.sha256").write_text("".join(f"{digest}  {name}\n" for name, digest in sorted(first.items())))

    def test_font_and_writer_provenance(self):
        data = (ROOT / "third_party/stb/stb_image_write.h").read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(), "cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05")
        self.assertIn(b"ALTERNATIVE A - MIT License", data)
        header = (ROOT / "apps/paint/default_font.h").read_text().split("= {", 1)[1]
        native = bytes(int(n) for n in re.findall(r"\d+", header))
        self.assertEqual(native, (ROOT / "assets/fonts/native/CIUKIOS.CFN").read_bytes()[64:])


if __name__ == "__main__":
    unittest.main()
