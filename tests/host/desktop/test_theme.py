"""Blue default and foreground/background contrast, including RGB565 loss."""
# SPDX-License-Identifier: MIT
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


def rgb565(colour):
    channels = [(colour >> shift) & 255 for shift in (16, 8, 0)]
    return tuple((v >> bits) * 255 / (255 >> bits) for v, bits in zip(channels, (3, 2, 3)))


def luminance(channels):
    def linear(v):
        v /= 255
        return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4
    return sum(weight * linear(v) for weight, v in zip((0.2126, 0.7152, 0.0722), channels))


class ThemeTests(unittest.TestCase):
    def test_defaults_persistence_and_readable_blue_in_32_and_16_bpp(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="theme-", dir=parent) as folder:
            binary = Path(folder) / "theme"
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                            "-I", str(ROOT / "apps/desktop"),
                            *(str(ROOT / p) for p in ("apps/desktop/theme.c", "apps/desktop/settings.c",
                                                      "tests/host/desktop/theme_test.c")),
                            "-o", str(binary)], check=True)
            result = subprocess.run([str(binary), str(Path(folder) / "desktop.cfg")],
                                    capture_output=True, text=True,
                                    env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS Blue default", result.stdout)
            minima = {}
            for line in result.stdout.splitlines():
                if not line.startswith("PAIR "):
                    continue
                _, role, ink, face = line.split()
                colours = [int(c, 16) for c in (ink, face)]
                for bpp in (32, 16):
                    rgb = [rgb565(c) if bpp == 16 else tuple((c >> s) & 255 for s in (16, 8, 0))
                           for c in colours]
                    lo, hi = sorted(map(luminance, rgb))
                    ratio = (hi + 0.05) / (lo + 0.05)
                    minima[bpp] = min(minima.get(bpp, ratio), ratio)
                    self.assertGreaterEqual(ratio, 4.5, (role, bpp, ink, face, ratio))
            print("BLUE_CONTRAST minimum:", {bpp: round(v, 3) for bpp, v in minima.items()})


if __name__ == "__main__":
    unittest.main()
