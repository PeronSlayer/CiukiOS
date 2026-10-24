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
import importlib.util as _review_util  # optional PNG review copies (frame_review.py, any import mode)
_review_spec = _review_util.spec_from_file_location("frame_review", __file__.rsplit("/", 1)[0] + "/frame_review.py")
_review = _review_util.module_from_spec(_review_spec); _review_spec.loader.exec_module(_review)
capture = _review.capture

# F2-81: intentional two-bar shell/theme changes; docs/design/desktop-shell.md.

ROOT = Path(__file__).resolve().parents[3]
THEMES = ("Platinum", "Ciuki Blue")
# F2-62: Blue polish; explicitly selected Platinum was unchanged.
# F2-63: re-recorded for the pinned FreeType UI atlases (before/after PNGs in build/host/f2-63).
# F2-67: recorded caption controls and owner-centred shell messages; paired PNGs await lead review.
# F2-72: re-recorded with --benchmark --frames 2 for the DMZ-White cursor overlay.
# F2-82: shell scene includes the larger orb and well.
# F2-83: deliberately re-recorded for the shared shaded bar surfaces/tasks.
# F2-99: original cursor silhouette and software shadow replace DMZ-White.
# fx-13: cursor set scaled to the classic 13x19 arrow.
# G4: the shell menus underline their access keys (SM-9).
# fixd: visually reviewed combined G11/G19/G23/G30 integration frames (2026-10-24).
GOLDENS = {
    "Ciuki Blue": "434841a387286672d2f1c6eb58658bf13e1f8491fdcfd4833a14938f6753e375",
    "Platinum": "2032042044b174bbfc4bc7c6ceccf7382b8f5340128a6813ae9e52ba18a669a1"
}
SOURCES = ["apps/desktop/" + n + ".c" for n in
           ("client", "protocol", "input", "compositor", "theme", "shell", "autoplay", "autoplay_content", "settings", "power_policy",
            "settings_io", "wallpaper", "keymap", "folder")]
SOURCES += ["apps/files/model.c", "apps/files/transfer.c", "apps/libdeskui/draw.c","apps/libdeskui/widgets.c","apps/libdeskui/list.c","apps/libdeskui/menu.c","apps/libdeskui/dialog.c", "apps/libdeskui/controls.c", "apps/libdeskui/commdlg.c","apps/libdeskui/textarea.c",
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
    capture("compositor-" + theme, pixels, 1024, 768)
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
