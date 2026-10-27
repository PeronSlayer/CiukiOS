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
# new review. wp-font-dialog was re-pinned after G16b added the Font Script combo (reviewed as a screenshot 2026-10-24). A change here is a visible change of WordPad or libdeskui; none of these is guest evidence.
APP_SCREENS = {
    'wp-context.xrgb': '690ddd000344a0191f10174357b37156ee46213142f3d860e1ad49eb1fa6fb8e',
    'wp-empty.xrgb': '33e74cb038c003982298045b806832d7c41b789cad077bafbcd6eba7ddd90772',
    'wp-find.xrgb': '9094dd9780617f53b3e405c5aa4c798102485466870c5f7c9eb434b5429f0d91',
    'wp-font-dialog.xrgb': '6f5aaa53093dab7c60c5502b8c7f447c0289a58f6cba57f8f91faa839dfd5bd8',
    'wp-formatted.xrgb': 'dd80368b287f66ce0140bae4bbf1c0848772c4c7457098da8dd7b284ece5b198',
    'wp-insert-object.xrgb': '08cc56a5835c257b295e8d47745b5864a9ef611c9bf05ea0706266bd7774d37f',
    'wp-justified.xrgb': '6bbe4c85c2805bc91e384b0e5bdf9eb276cb95e3afd57fc7507e1a63a1f75693',
    'wp-nowrap-hscroll.xrgb': 'be741ee26182eb26f1629dafc10ab0b4480ae9e76c6fdc9690feeefecb501b20',
    'wp-opened.xrgb': '1372874ef05de37ca06e7eda8718c0593051d976b77977de9ba2b3af14f2e499',
    'wp-options-dialog.xrgb': '1af33e111f630ae6bfb3dd200b25c07e75d99ce452370baf4922df23a9b2292f',
    'wp-paragraphs.xrgb': 'bd9ecea3feb3b5aa8a2f9479d7eb70b0a9844978baa242d661cf54e7396ed002',
    'wp-pictures-frame.xrgb': '00a50538ea7577c7de624fc534cffcc20eed642124ad45b4f7c6210a71536a8a',
    'wp-pictures.xrgb': '20cf376f4940f34ebddf830551bfa3120a9b5060660fb666bbd1d2122cbb239e',
    'wp-preview-1.xrgb': '8f7b08096501d7feb445019f4357a6be24ff8dafb3579b44dbcf4000c87dc184',
    'wp-replace-bar.xrgb': '38592605d14d9048cf60a9dc61fa3794ddbd904ddfd793a5acdf86f62b218cb2',
    'wp-ruler-bullet.xrgb': '17a2e94dd39f12e4beefe0f5074e4e25dd5d2dc883525f243c2820e994b75515',
    'wp-ruler-drag.xrgb': '52dbad893bad79b105673a32a205703bf0cd2e082f40e4ba64bf638a09908d21',
    'wp-ruler-units.xrgb': '966703ae08cf18ab60459a8a5a89107545274f2cc2122a0a7ef58800d17c1cf9',
    'wp-word-opened.xrgb': '548bf2cc9cd7cfe256fc933e5fff7a9df3fe9bf22c44283bcf7a400437d48149',
    'wp-word-picture.xrgb': '1f86826cd54b1ac182d6ea18e571b54f5ae2da9943e7c00115d2f63f43ba7d57',
}
FLAGS = ["-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-macro-redefined",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections"]
INCLUDES = ["-I", str(ROOT / "apps/wordpad"), "-I", str(ROOT / "apps/libdeskui"), "-I", str(ROOT / "apps/desktop"),
            "-I", str(ROOT / "src/kernel/include")]
TOOLKIT = ["apps/libdeskui/draw.c", "apps/libdeskui/widgets.c", "apps/libdeskui/list.c", "apps/libdeskui/menu.c",
           "apps/libdeskui/dialog.c", "apps/libdeskui/controls.c", "apps/libdeskui/commdlg.c", "apps/libdeskui/textarea.c", "apps/libdeskui/recent.c", "apps/libdeskui/keymap.c",
           "apps/desktop/compositor.c", "apps/desktop/theme.c", "apps/libdeskui/icons.c", "apps/libdeskui/icon_map.c", "apps/libdeskui/iconx.c", "apps/libdeskui/fileicon.c"]
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
        self.assertIn('-lfont"] if name in ("control", "charmap", "wordpad")', (ROOT / "apps/desktop/build_desktop.py").read_text())
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
