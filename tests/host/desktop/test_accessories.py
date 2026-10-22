"""Accessories recovered from the 0.8 line: engines, windows, renders, registration."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
import importlib.util as _review_util  # optional PNG review copies (frame_review.py, any import mode)
_review_spec = _review_util.spec_from_file_location("frame_review", __file__.rsplit("/", 1)[0] + "/frame_review.py")
_review = _review_util.module_from_spec(_review_spec); _review_spec.loader.exec_module(_review)
capture = _review.capture

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
FLAGS = ["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-D_POSIX_C_SOURCE=200809L",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
         "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections"]
TOOLKIT = ["apps/libdeskui/draw.c", "apps/libdeskui/widgets.c", "apps/libdeskui/list.c", "apps/libdeskui/menu.c",
           "apps/libdeskui/dialog.c", "apps/libdeskui/textarea.c", "apps/libdeskui/recent.c", "apps/libdeskui/keymap.c",
           "apps/desktop/compositor.c", "apps/desktop/theme.c", "apps/libdeskui/icons.c", "apps/libdeskui/icon_map.c"]
INCLUDES = ["-I", str(ROOT / "apps/accessories"), "-I", str(ROOT / "apps/libdeskui"), "-I", str(ROOT / "apps/desktop"),
            "-I", str(ROOT / "src/kernel/include")]
ACC = "apps/accessories/"
TESTS = {
    "calc": [ACC + "calc_model.c", ACC + "calc_view.c"],
    "clock": [ACC + "clock_model.c", ACC + "clock_view.c"],
    "viewer": [ACC + "viewer_model.c", ACC + "viewer_view.c", ACC + "viewer_stb.c"],
    "help": [ACC + "help_topics.c", ACC + "help_view.c"],
    "mixer": [ACC + "mixer_view.c"],
    "charmap": [ACC + "charmap_view.c"],
    "keymap": [],
}
# Renders of the current libdeskui look (f2-31 toolkit plus the paint hook), from
# fixed data in the *_test.c programs. The toolkit restyle (wt/ui-look) changes
# them deliberately: re-pin from the first run on the restyled toolkit.
RENDERS = {
    "calc-grouped-memory.xrgb": "PIN-FROM-FIRST-RUN",
    "calc-initial.xrgb": "PIN-FROM-FIRST-RUN",
    "charmap.xrgb": "PIN-FROM-FIRST-RUN",
    "clock-initial.xrgb": "PIN-FROM-FIRST-RUN",
    "clock-noon.xrgb": "PIN-FROM-FIRST-RUN",
    "help-shortcuts.xrgb": "PIN-FROM-FIRST-RUN",
    "help-welcome.xrgb": "PIN-FROM-FIRST-RUN",
    "mixer-default.xrgb": "PIN-FROM-FIRST-RUN",
    "mixer-muted.xrgb": "PIN-FROM-FIRST-RUN",
    "viewer-empty.xrgb": "PIN-FROM-FIRST-RUN",
    "viewer-jpeg-300.xrgb": "PIN-FROM-FIRST-RUN",
    "viewer-wallpaper-fit.xrgb": "PIN-FROM-FIRST-RUN",
}


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class AccessoriesTests(unittest.TestCase):
    renders = {}

    def run_native(self, name):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=f"acc-{name}-", dir=parent) as temp:
            folder = Path(temp)
            scratch = folder / "scratch"; scratch.mkdir()
            renders = folder / "renders"; renders.mkdir()
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0", HOME=str(scratch))
            binary = folder / name
            sources = [*TESTS[name], *TOOLKIT, f"tests/host/desktop/{name}_test.c"]
            if name != "keymap":
                sources.append(ACC + "acc.c")
            icons = load("acc_icon_assets", ROOT / "tests/host/desktop/icon_assets.py")   # the guest's /system/icons store
            subprocess.run([*FLAGS, *icons.defines(), *INCLUDES, *(str(ROOT / s) for s in sources), "-lm", "-o", str(binary)],
                           check=True, env=env)
            args = {"viewer": [str(scratch), str(ROOT / "misc/CiukiOS_SplashScreen.png"), str(renders)],
                    "help": [str(scratch), str(renders)], "keymap": []}.get(name, [str(renders)])
            result = subprocess.run([str(binary), *args], check=True, env=env, capture_output=True, text=True)
            self.assertIn("PASS", result.stdout)
            # The tests write the recent list only under the scratch HOME.
            self.assertFalse((Path.home() / "recent.lst").exists() and name in ("viewer", "help") and
                             (Path.home() / "recent.lst").stat().st_mtime > binary.stat().st_mtime)
            for p in renders.glob("*.xrgb"):
                width, height = map(int, p.with_suffix(".size").read_text().split())
                capture(p.stem, p.read_bytes(), width, height)
            return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(renders.glob("*.xrgb"))}

    def check_renders(self, actual):
        for name, digest in actual.items():
            self.assertIn(name, RENDERS)
            if RENDERS[name] != "PIN-FROM-FIRST-RUN":
                self.assertEqual(digest, RENDERS[name], name)

    def test_calc(self): self.check_renders(self.run_native("calc"))
    def test_clock(self): self.check_renders(self.run_native("clock"))
    def test_viewer(self): self.check_renders(self.run_native("viewer"))
    def test_help(self): self.check_renders(self.run_native("help"))
    def test_mixer(self): self.check_renders(self.run_native("mixer"))
    def test_charmap(self): self.check_renders(self.run_native("charmap"))
    def test_keymap_and_recent(self): self.run_native("keymap")

    def test_renders_are_deterministic(self):
        self.assertEqual(self.run_native("calc"), self.run_native("calc"))

    def test_catalog_registration(self):
        """Every catalog entry is built, installed in /Programs and has a Tango icon in the theme archive."""
        catalog = (ROOT / "apps/desktop/catalog.h").read_text()
        entries = re.findall(r'\{"([a-z]+)", "([^"]+)", "([a-z-]+)", ([01]),', catalog)
        names = [e[0] for e in entries]
        # Catalogue order selects defaults: Viewer precedes its Paint alternative.
        self.assertEqual(sorted(names), ["calc", "charmap", "clock", "gfxdemo", "help", "hwreport", "mixer", "music", "paint", "regedit", "viewer"])
        build = load("acc_build", ROOT / "apps/desktop/build_desktop.py")
        self.assertEqual(set(build.ACCESSORIES) | {"regedit", "gfxdemo", "hwreport"}, set(names))
        self.assertIn("regedit", build.PROGRAMS)
        image = (ROOT / "scripts/build_image.py").read_text()
        for name in names:
            self.assertIn(f'"/Programs/{name}": DESKTOP / "{name}"', image)
        licenses = load("acc_image_licenses", ROOT / "scripts/build_image.py").desktop_license_payloads()
        self.assertEqual(licenses["/system/licenses/stb_image.txt"], ROOT / "third_party/stb/LICENSE")
        listing = subprocess.run(["tar", "tzf", str(ROOT / "assets/icons/upstream/tango-icon-theme-0.8.90.tar.gz")],
                                 check=True, capture_output=True, text=True).stdout
        for _, _, icon, _ in entries:
            self.assertRegex(listing, rf"/32x32/[a-z]+/{re.escape(icon)}\.png")
        # File-taking accessories are exactly the ones the desktop passes LAUNCH arguments to.
        self.assertEqual({e[0] for e in entries if e[3] == "1"}, {"help", "music", "paint", "viewer"})

    def test_stb_provenance(self):
        data = (ROOT / "third_party/stb/stb_image.h").read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(), "594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3")
        self.assertIn(b"ALTERNATIVE A - MIT License", (ROOT / "third_party/stb/LICENSE").read_bytes())

    def test_image_inventory(self):
        build = load("acc_build_inventory", ROOT / "apps/desktop/build_desktop.py")
        if not (build.OUT / "manifest.json").is_file():
            self.skipTest("run python3 apps/desktop/build_desktop.py first")
        manifest = build.validate_payloads()
        image = load("acc_image", ROOT / "scripts/build_image.py")
        sources, directories, _ = image.desktop_payloads()
        for name in build.ACCESSORIES:
            self.assertIn(name, manifest["elf"])
            self.assertEqual(sources[f"/Programs/{name}"], build.OUT / name)
        self.assertIn("/system/licenses", directories)


if __name__ == "__main__":
    unittest.main()
