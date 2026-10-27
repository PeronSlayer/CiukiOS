"""F2-83 bar depth, RGB565 bands, label contrast and production review frames."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
GOLDENS = ROOT / "tests/host/desktop/bars-goldens.json"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class BarTests(unittest.TestCase):
    def test_surfaces_contrast_banding_and_frames(self):
        desktop = load("bar_desktop", ROOT / "tests/host/desktop/test_desktop.py")
        assets = load("bar_assets", ROOT / "tests/host/desktop/icon_assets.py")
        contrast = load("bar_contrast", ROOT / "tests/host/desktop/test_theme.py")
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        dest = ROOT / "build/codex/f2-83-frames"
        dest.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="bars-", dir=parent) as temp:
            folder = Path(temp)
            binary = folder / "bars"
            sources = [ROOT / "apps/desktop" / f"{n}.c" for n in
                       ("client", "protocol", "input", "compositor", "theme")]
            sources += desktop.SHELL_SOURCES
            sources += [ROOT / p for p in ("apps/libdeskui/icons.c", "apps/libdeskui/icon_map.c", "apps/libdeskui/iconx.c", "apps/libdeskui/fileicon.c",
                                          "tests/host/desktop/fake.c", "tests/host/desktop/bars_test.c")]
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined",
                            "-fno-sanitize-recover=undefined", "-ffunction-sections", "-fdata-sections",
                            "-Wl,--gc-sections", "-I", str(ROOT / "apps/desktop"),
                            "-I", str(ROOT / "src/kernel/include"), *assets.defines(),
                            *map(str, sources), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary), str(assets.portrait()), str(folder)],
                                    capture_output=True, text=True,
                                    env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
            self.assertEqual(result.returncode, 0, result.stdout[-1000:] + result.stderr)
            minimum = {32: 100, 16: 100}
            for line in result.stdout.splitlines():
                if not line.startswith("PAIR "):
                    continue
                _, role, ink, face = line.split()
                for bpp in (32, 16):
                    rgb = [contrast.rgb565(int(c, 16)) if bpp == 16 else
                           tuple((int(c, 16) >> s) & 255 for s in (16, 8, 0)) for c in (ink, face)]
                    lo, hi = sorted(map(contrast.luminance, rgb))
                    ratio = (hi + 0.05) / (lo + 0.05)
                    minimum[bpp] = min(minimum[bpp], ratio)
                    self.assertGreaterEqual(ratio, 4.5, (role, bpp, ink, face, ratio))
            from PIL import Image
            digests = {}
            for raw in sorted(folder.glob("*.xrgb")):
                width, height, bpp = map(int, re.search(r"-(\d+)x(\d+)-(\d+)bpp", raw.stem).groups())
                pixels = raw.read_bytes()
                digests[raw.stem] = hashlib.sha256(pixels).hexdigest()
                image = Image.frombytes("RGB", (width, height), pixels, "raw", "BGRX")
                if bpp == 16:
                    # Display the actual quantized result, including text/icons.
                    rgb = bytearray(image.tobytes())
                    for channel, bits in enumerate((3, 2, 3)):
                        rgb[channel::3] = bytes((v >> bits) * 255 // (255 >> bits) for v in rgb[channel::3])
                    image = Image.frombytes("RGB", (width, height), bytes(rgb))
                image.save(dest / (raw.stem + ".png"))
                for name, box in (("orb", (0, 0, 54, 44)),
                                  ("three-tasks", (0, height-32, 488, height))):
                    crop = image.crop(box)
                    crop.resize((crop.width*2, crop.height*2), Image.Resampling.NEAREST).save(
                        dest / (raw.stem + "-" + name + "-2x.png"))
            self.assertEqual(len(digests), 8)
            if os.environ.get("CIUKI_RECORD_BAR_GOLDENS") == "1":
                GOLDENS.write_text(json.dumps(digests, indent=2, sort_keys=True) + "\n")
            self.assertEqual(digests, json.loads(GOLDENS.read_text()))
            print("PASS f2-83 surfaces/frames; minimum label contrast:",
                  {bpp: round(value, 3) for bpp, value in minimum.items()})


if __name__ == "__main__":
    unittest.main()
