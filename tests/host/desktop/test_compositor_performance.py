"""Pixel-exact 1024x768 production scene; --benchmark reports serialized RDTSC.

Run with nice -n 19. Timing is never a unit-test assertion; compare identical
compiler/flags/scene on the same host before and after changing the painter.
"""
# SPDX-License-Identifier: MIT
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
THEMES = ("Platinum", "Ciuki Blue", "Classic 98")
# Filled from the original production renderer before the optimization.
GOLDENS = {
    "Platinum": "3ec2fcf41abc5791c0c389cbfe54cf9b3f65cd345f6643ed7363756470791769",
    "Ciuki Blue": "155c42a34fc381e8d9e77915612b57950317a2ca2b9643a0b63a8f2da67e885c",
    "Classic 98": "43586a63bafc8ea4c46153a2f0af37cba9a8cdaac47b61afae3c720ad0d464cb",
}
SOURCES = ["apps/desktop/" + n + ".c" for n in
           ("client", "protocol", "input", "compositor", "theme", "shell", "settings",
            "settings_io", "wallpaper", "keymap", "folder")]
SOURCES += ["apps/files/model.c", "apps/files/transfer.c", "apps/libdeskui/draw.c",
            "apps/libdeskui/icons.c", "apps/libdeskui/icon_map.c", "tests/host/desktop/fake.c",
            "tests/host/desktop/compositor_benchmark.c"]


def assets():
    path = ROOT / "tests/host/desktop/icon_assets.py"
    spec = importlib.util.spec_from_file_location("compositor_assets", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.portrait(), module.store()


def build(folder, sanitize=False, extra=(), rows=False, compiler="clang"):
    binary = folder / "compositor"
    flags = (["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if sanitize else
             ["-O2", "-fno-strict-aliasing", "-fno-builtin", "-mno-sse", "-mno-sse2", "-mno-mmx"])
    sources = SOURCES
    if rows:
        sources = [source for source in SOURCES if source != "apps/desktop/compositor.c"]
        sources = [source.replace("compositor_benchmark.c", "compositor_rows_test.c") for source in sources]
        extra = [*extra, "-Wl,--wrap=malloc"]
    subprocess.run([compiler, "-std=c17", *flags, *extra, "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "apps/desktop"), "-I", str(ROOT / "src/kernel/include"),
                    *map(str, (ROOT / source for source in sources)), "-o", str(binary)], check=True)
    return binary


def render(binary, folder, theme, frames=0):
    portrait, store = assets()
    target = folder / "scene.xrgb"
    result = subprocess.run([str(binary), str(portrait), str(store), theme, str(target), str(frames)],
                            env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"},
                            check=True, capture_output=True, text=True)
    pixels = target.read_bytes()
    assert len(pixels) == 1024 * 768 * 4
    target.unlink()
    return hashlib.sha256(pixels).hexdigest(), result.stdout


class CompositorSceneTests(unittest.TestCase):
    def test_row_painters_against_original_theme(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="compositor-rows-", dir=parent) as temp:
            binary = build(Path(temp), sanitize=True, rows=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                                    env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS compositor rows", result.stdout)

    def test_original_scene_goldens_and_clips(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="compositor-scene-", dir=parent) as temp:
            folder = Path(temp)
            binary = build(folder, sanitize=True)
            for theme in THEMES:
                with self.subTest(theme=theme):
                    digest, output = render(binary, folder, theme)
                    self.assertEqual(digest, GOLDENS[theme])
                    self.assertIn("PASS compositor scene", output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--benchmark", action="store_true")
    parser.add_argument("--frames", type=int, default=32)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--flag", action="append", default=[], help="extra host compile option (use --flag=-O3)")
    args, remaining = parser.parse_known_args()
    if args.benchmark:
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="compositor-bench-", dir=parent) as temp:
            folder = Path(temp)
            binary = build(folder, extra=args.flag, compiler=args.cc)
            for theme in THEMES:
                digest, output = render(binary, folder, theme, args.frames)
                print(output, end="", flush=True)
                print(f"GOLDEN {theme}: {digest}", flush=True)
    else:
        unittest.main(argv=[__file__, *remaining])
