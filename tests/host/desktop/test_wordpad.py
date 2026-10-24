"""G28 WordPad host tests: document model, RTF and text files, layout and pagination, the application against a fake
file system with fixed-advance fonts (pinned frames), the real Liberation faces through libfont, and the registration of
the program in the build, the image, the launcher catalogue and the shell. Scratch lives under build/host."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import json
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

PIN = "PIN-FROM-FIRST-RUN"
# Frames of the application test (640x440 XRGB, fixed-advance fonts: bars instead of glyphs), produced by the
# production renderer. wp-formatted, wp-font-dialog and wp-preview-1 were reviewed as screenshots on 2026-10-24 (G28). G28b changed
# the ruler (units, tab stops, right marker) and the preview bar (a Print button): wp-font-dialog, wp-find, wp-paragraphs,
# wp-ruler-bullet and wp-preview-1 were reviewed again as screenshots (CIUKI_FONT_REVIEW=<folder> writes PNG copies), as were the new
# frames justified, options-dialog, ruler-units, nowrap-hscroll, ruler-drag, pictures, pictures-frame, insert-object, word-opened
# and word-picture; context, empty, formatted, opened and replace-bar differ from G28's only by the ruler and are pinned without a
# new review. A change here is a visible change of WordPad or libdeskui; none of these is guest evidence.
APP_SCREENS = {
    'wp-context.xrgb': '7c3774294348b7bafa8b7a73d0ddf249e0634082a854076d8dbc08797a7db50f',
    'wp-empty.xrgb': '32666e1497a07675f2ca95074cdf9eb90ca1a83734e97e4c6ab1f913cf6495df',
    'wp-find.xrgb': 'b8b548bf648955f1aa510a0587943597c029e3fb1bc5828d87ada722b23f7ef7',
    'wp-font-dialog.xrgb': 'aae56f839590714091c9f7892adf08c3186cd0b663f6d4a4d69ad05ed8d5003c',
    'wp-formatted.xrgb': 'e5958b1ba1ac40c190b84bb7546dab3606dfcc37cb3548f3c4f68674eeba96ab',
    'wp-opened.xrgb': '04618571576664f6e9fcd4fac82130c8cd8023166afbbcc1c3e0e1aa27f1c3a7',
    'wp-paragraphs.xrgb': '2df78fab3fb20a253ab371773451152a4e1ac42b066e45e8da2e0afae2cde225',
    'wp-preview-1.xrgb': '8f7b08096501d7feb445019f4357a6be24ff8dafb3579b44dbcf4000c87dc184',
    'wp-justified.xrgb': 'a4500688ac279d5a3bad9c012d60e03e3f3921271c13c0856b64239f13fcbeea', 'wp-options-dialog.xrgb': '60bfe236ea8ca78b42aa1eb88b32f14e5672eb16643d8020340489c6e90b25dc', 'wp-ruler-units.xrgb': '0dee146990d22fd77f36b37d19e1fcc67148b5414d39d7f812ffda8eceff3619', 'wp-nowrap-hscroll.xrgb': '151457981ec39a0a9204778b850cac28d760949926c96529b79b9db432676156', 'wp-ruler-drag.xrgb': '8633e6443f8a236e329d309e219717fbd6db7fbb3d1707032094bef6ef57d5a3',
    'wp-pictures.xrgb': '8e2db8794d6f8a493b70985b6a22b3fe7e90a6e3c081334d5654b499fd2f26a7', 'wp-pictures-frame.xrgb': '3644e10d69d361da48d03fc1c7c62e2fcf047b1635c30b136fbdcf31c7f6f524', 'wp-insert-object.xrgb': 'a48db366fc149bf225829d10a4101a860325de3ea340ed16ee93fae891b4842d', 'wp-word-opened.xrgb': '094253d03dc99156b3dbb3bce2f5c31df1b107d760b2f4ca0ce55d9017fbf0ea', 'wp-word-picture.xrgb': '8ec44bce6935a1ef3c06e1be8c501dc256f9418638d5986662eb15ec81d18fe3',
    'wp-replace-bar.xrgb': 'e79bf9152508bfb29e80dff6a07a3335d0de8f964737ba5cacb72c24f50e405a',
    'wp-ruler-bullet.xrgb': '77698a4afb66eb4e7f6503443cd14a3303beee310b2f984398a9742c9c4cbd7a',
}
FLAGS = ["-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-macro-redefined",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections"]
INCLUDES = ["-I", str(ROOT / "apps/wordpad"), "-I", str(ROOT / "apps/libdeskui"), "-I", str(ROOT / "apps/desktop"),
            "-I", str(ROOT / "src/kernel/include")]
TOOLKIT = ["apps/libdeskui/draw.c", "apps/libdeskui/widgets.c", "apps/libdeskui/list.c", "apps/libdeskui/menu.c",
           "apps/libdeskui/dialog.c", "apps/libdeskui/textarea.c", "apps/libdeskui/recent.c", "apps/libdeskui/keymap.c",
           "apps/desktop/compositor.c", "apps/desktop/theme.c", "apps/libdeskui/icons.c", "apps/libdeskui/icon_map.c"]
CORE = ["apps/wordpad/rich.c", "apps/wordpad/rtf.c", "apps/wordpad/layout.c"]
IMPORT = ["apps/wordpad/ole.c", "apps/wordpad/msword.c", "apps/wordpad/write.c"]
CLIPBOARD = ["apps/libdeskui/clipboard.c", "apps/libdeskui/clipstore.c", "apps/desktop/protocol.c"]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ICONS = load("wordpad_icon_assets", ROOT / "tests/host/desktop/icon_assets.py")


class WordpadTests(unittest.TestCase):
    def scratch(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        return tempfile.TemporaryDirectory(prefix="wordpad-", dir=parent)

    def compile(self, folder, name, sources, extra=(), libs=()):
        binary = folder / name
        env = dict(os.environ, TMPDIR=str(folder))
        subprocess.run(["clang", *FLAGS, *INCLUDES, *ICONS.defines(), *extra, *(str(ROOT / s) for s in sources), *libs, "-lm", "-pthread", "-o", str(binary)],
                       check=True, env=env)
        return binary

    def run_binary(self, folder, binary, *args, **extra):
        env = dict(os.environ, TMPDIR=str(folder), ASAN_OPTIONS="detect_leaks=0", **extra)
        result = subprocess.run([str(binary), *args], env=env, capture_output=True, text=True, timeout=900)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_model_rtf_text_and_layout(self):
        with self.scratch() as temp:
            folder = Path(temp)
            binary = self.compile(folder, "model", [*CORE, "tests/host/desktop/wordpad_model_test.c"])
            self.assertIn("PASS wordpad model", self.run_binary(folder, binary))

    def test_import_word_and_write(self):
        """OLE2 reader, Word 6 and 97 documents and Write files: built fixtures, a LibreOffice file, damaged files."""
        fixtures = load("wordpad_fixtures", ROOT / "tests/host/desktop/wordpad_fixtures.py")
        with self.scratch() as temp:
            folder = Path(temp)
            fixtures.generate(folder / "fx")
            binary = self.compile(folder, "import", [*CORE, *IMPORT, "tests/host/desktop/wordpad_import_test.c"])
            out = self.run_binary(folder, binary, str(folder / "fx"), str(ROOT / "tests/host/desktop/fixtures"))
            self.assertIn("PASS wordpad import", out)

    def test_picture_codec(self):
        """stb_image decoding (PNG, JPEG, GIF, DIB), the reduction of big pictures and PNG encoding."""
        fixtures = load("wordpad_fixtures", ROOT / "tests/host/desktop/wordpad_fixtures.py")
        with self.scratch() as temp:
            folder = Path(temp)
            fixtures.generate_images(folder / "img")
            binary = self.compile(folder, "image", [*CORE, "apps/wordpad/image.c", "tests/host/desktop/wordpad_image_test.c"])
            self.assertIn("PASS wordpad image", self.run_binary(folder, binary, str(folder / "img")))

    def test_application_with_fake_file_system(self):
        with self.scratch() as temp:
            folder = Path(temp)
            fixtures = load("wordpad_fixtures", ROOT / "tests/host/desktop/wordpad_fixtures.py")
            fixtures.generate(folder / "fx")
            binary = self.compile(folder, "wordpad", [*CORE, *IMPORT, "apps/wordpad/app.c", "apps/ciuknote/doc.c", *TOOLKIT, *CLIPBOARD,
                                                      "apps/libprint/print.c", "apps/libprint/render.c", "tests/host/desktop/wordpad_test.c"],
                                  extra=["-DWP_UNDO_BYTES=(1u<<20)"])   # a small undo budget, to test the cap
            out = self.run_binary(folder, binary, str(folder / "wp"), WORDPAD_FIXTURES=str(folder / "fx"))
            self.assertIn("PASS wordpad", out)
            actual = {}
            for p in sorted(folder.glob("*.xrgb")):
                capture(p.stem, p.read_bytes(), 640, 440)
                actual[p.name] = hashlib.sha256(p.read_bytes()).hexdigest()
            self.assertEqual(sorted(actual), sorted(f"wp-{n}.xrgb" for n in (
                "empty", "formatted", "paragraphs", "font-dialog", "find", "opened", "context", "preview-1", "ruler-bullet", "replace-bar",
                "justified", "options-dialog", "ruler-units", "nowrap-hscroll", "ruler-drag", "pictures", "pictures-frame", "insert-object", "word-opened", "word-picture")))
            fresh = {k: v for k, v in actual.items() if APP_SCREENS[k] == PIN}
            if fresh:
                print("APP_SCREENS", PIN, json.dumps(fresh, indent=1, sort_keys=True))
            self.assertEqual({k: v for k, v in actual.items() if APP_SCREENS[k] != PIN}, {k: v for k, v in APP_SCREENS.items() if v != PIN})

    def test_real_fonts(self):
        ft = load("wordpad_ft", ROOT / "sdk/freetype/build.py")
        with self.scratch() as temp:
            folder = Path(temp)
            sources = ["apps/wordpad/fonts.c", *CORE, "apps/libdeskui/text_ttf.c", "sdk/libfont/font.c", *TOOLKIT, "tests/host/desktop/wordpad_fonts_test.c"]
            binary = self.compile(folder, "fonts", sources, [*ft.includes(), "-idirafter", str(ROOT / "sdk/sysroot-overlay/include")], [str(ft.host())])
            out = self.run_binary(folder, binary, str(ROOT / "assets/fonts/liberation") + "/")
            self.assertIn("PASS wordpad fonts", out)

    def test_registration(self):
        """The program is built, installed in /Programs, listed in the launcher and the Accessories menu, associated with
        .rtf and reachable from CiukNote."""
        build = load("wordpad_build", ROOT / "apps/desktop/build_desktop.py")
        self.assertIn("wordpad", build.ACCESSORIES)
        self.assertIn("wordpad", build.PROGRAMS)
        for source in build.ACCESSORIES["wordpad"]:
            self.assertTrue((ROOT / "apps/accessories" / source).resolve().is_file(), source)
        for source in ("ole.c", "msword.c", "write.c", "image.c"):   # G28b: the importers and the picture codec are part of the program
            self.assertIn(f"../wordpad/{source}", build.ACCESSORIES["wordpad"])
        builder = (ROOT / "apps/desktop/build_desktop.py").read_text()
        self.assertIn('for name in ("ciuknote", "paint", "viewer", "wordpad"):', builder)   # and so is the shared printing code (G31)
        self.assertIn("apps/wordpad", (ROOT / "apps/desktop/build_desktop.py").read_text())
        self.assertIn('-lfont"] if name in ("control", "wordpad")', (ROOT / "apps/desktop/build_desktop.py").read_text())
        image = (ROOT / "scripts/build_image.py").read_text()
        self.assertIn('"/Programs/wordpad": DESKTOP / "wordpad"', image)
        catalog = (ROOT / "apps/desktop/catalog.h").read_text()
        self.assertRegex(catalog, r'\{"wordpad", "WordPad", "x-office-document", 1, "rtf\|doc\|wri"\}')
        shell = (ROOT / "apps/desktop/shell.c").read_text()
        self.assertRegex(shell, r'\{"wordpad", "WordPad", "x-office-document", "accessories", NULL\}')
        self.assertIn('"program:wordpad", "x-office-document"', (ROOT / "apps/libdeskui/icon_map.c").read_text())
        self.assertIn('"viewer","wordpad","lua"', (ROOT / "apps/control/programs.c").read_text())
        self.assertIn("rtf|doc|wri", catalog)
        topics = (ROOT / "apps/accessories/help_topics.c").read_text()
        for topic in ("wordpad", "wordpad-formatting", "wordpad-ruler", "wordpad-pictures", "wordpad-files", "wordpad-printing"):
            self.assertIn(f'{{"{topic}", ', topics)
        client = (ROOT / "apps/desktop/client.c").read_text()
        self.assertIn('c->program == DESK_APP_NOTE && !c->taskman', client)
        self.assertIn('!strcmp(p.title, "wordpad")', client)

    def test_sources_stay_toolkit_independent(self):
        """The model, the RTF code and the layout use no system call and no toolkit header."""
        for name in ("rich.c", "rtf.c", "layout.c", "rich.h", "layout.h", "ole.c", "ole.h", "msword.c", "write.c"):
            text = (ROOT / "apps/wordpad" / name).read_text()
            self.assertNotRegex(text, r'#include\s+"\.\./libdeskui|#include\s+<(unistd|fcntl|sys/)')


if __name__ == "__main__":
    unittest.main()
