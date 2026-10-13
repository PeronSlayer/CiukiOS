"""Pixel-exact 1024x768 production scene; --benchmark reports serialized RDTSC.

Run with nice -n 19. Timing is never a unit-test assertion; compare identical
compiler/flags/scene on the same host before and after changing the painter.
"""
# SPDX-License-Identifier: MIT
import argparse
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import tempfile
import tarfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
THEMES = ("Platinum", "Ciuki Blue", "Classic 98")
# Filled from the original production renderer before the optimization.
GOLDENS = {
    "Platinum": "20efa8ab5b7252383bec73ac5ff07b667be3e70689a41e0b6efcbe351a9129fe",
    "Ciuki Blue": "38233d6ca1f5d0758eb541348e60d36106bd3ce3d1104f59e7375b62d7ee7880",
    "Classic 98": "b172a5fc9a7afd162f8c11ab71de6d787e6a78024eb05217b9adb311d31f37c7",
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


def reference_sources(folder, revision, profile):
    """Build a coherent reference: structures and callers must share a revision.

    Keep the small tracked source snapshot under the benchmark's temporary
    folder. No disk image, worktree, git index or repository file is changed.
    f2-48 references already contain the exclusive painter instrumentation.
    """
    snapshot = folder / "reference"
    snapshot.mkdir()
    archive = subprocess.run(["git", "archive", "--format=tar", revision,
                              "apps/desktop", "apps/libdeskui", "apps/files", "tests/host/desktop"],
                             cwd=ROOT, check=True, capture_output=True).stdout
    with tarfile.open(fileobj=io.BytesIO(archive)) as sources:
        sources.extractall(snapshot, filter="data")
    if profile and "compositor_profile_enter" not in (snapshot / "tests/host/desktop/compositor_benchmark.c").read_text():
        raise ValueError("profile reference requires the f2-48 painter harness")
    return {path: snapshot / path for path in SOURCES}, snapshot


def build(folder, sanitize=False, extra=(), rows=False, compiler="clang", profile=False, reference=None):
    binary = folder / "compositor"
    flags = (["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if sanitize else
             ["-O2", "-fno-strict-aliasing", "-fno-builtin", "-mno-sse", "-mno-sse2", "-mno-mmx"])
    sources = SOURCES
    if profile:
        extra = [*extra, "-include", str(ROOT / "tests/host/desktop/compositor_profile.h"),
                 "-Wl,--wrap=du_icon_blit,--wrap=desk_paint_text,--wrap=desk_paint_text_n"]
    if rows:
        sources = [source for source in SOURCES if source != "apps/desktop/compositor.c"]
        sources = [source.replace("compositor_benchmark.c", "compositor_rows_test.c") for source in sources]
        extra = [*extra, "-Wl,--wrap=malloc"]
    replacements, snapshot = reference_sources(folder, reference, profile) if reference else ({}, ROOT)
    subprocess.run([compiler, "-std=c17", *flags, *extra, "-Wall", "-Wextra", "-Werror",
                    "-I", str(snapshot / "apps/desktop"), "-I", str(ROOT / "src/kernel/include"),
                    "-I", str(snapshot / "apps/libdeskui"),
                    *(str(replacements.get(source, ROOT / source)) for source in sources), "-o", str(binary)], check=True)
    return binary


def render(binary, folder, theme, frames=0, reference=False):
    portrait, store = assets()
    target = folder / "scene.xrgb"
    result = subprocess.run([str(binary), str(portrait), str(store), theme, str(target), str(frames)],
                            env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0",
                                 **({"CIUKI_BENCH_REFERENCE": "1"} if reference else {})},
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
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
    parser.add_argument("--profile", action="store_true", help="separate exclusive painter cycle sample")
    parser.add_argument("--reference", help="benchmark a git revision with identical host flags and assets")
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--flag", action="append", default=[], help="extra host compile option (use --flag=-O3)")
    args, remaining = parser.parse_known_args()
    if args.benchmark:
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="compositor-bench-", dir=parent) as temp:
            folder = Path(temp)
            binary = build(folder, extra=args.flag, compiler=args.cc, profile=args.profile, reference=args.reference)
            for theme in THEMES:
                digest, output = render(binary, folder, theme, args.frames, reference=bool(args.reference))
                print(output, end="", flush=True)
                print(f"GOLDEN {theme}: {digest}", flush=True)
    else:
        unittest.main(argv=[__file__, *remaining])
