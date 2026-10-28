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
# new review. wp-font-dialog was re-pinned after G16b added the Font Script combo (reviewed as a screenshot 2026-10-24).
# fx-deskdebt re-pinned every frame but preview-1 after fx-icons (4358d62b5) mapped program:clock, the Date/Time toolbar
# button, to the Haiku clock (reviewed as a zoomed old/new comparison 2026-10-27; no other pixel changed).
# A change here is a visible change of WordPad or libdeskui; none of these is guest evidence.
APP_SCREENS = {
    'wp-context.xrgb': '3f284a2051d0395ece8222185dd5c81fcbdee83089a50ed7bd48d1e5112a4b09',
    'wp-empty.xrgb': 'b9d9701b0ce3b658898916c4e6f5781cd9ea5b4e39ebd701ba169c63c95e4cc1',
    'wp-find.xrgb': '0ca3e0075f64155684b32c113e85c407813e19fd8ed9d7966b486c9b7b8d4faf',
    'wp-font-dialog.xrgb': '272abef4bd72faeab2e8c3e54671497acd3a0f684424ac265458934b8735ee42',
    'wp-formatted.xrgb': '2158c55796be56483c422213643ec2ca193142321a793189a3f134000ee48f49',
    'wp-insert-object.xrgb': 'bce9b06b8f817bfa18878b6b52e71255c72b3af1c92b945f6892ea317e48d381',
    'wp-justified.xrgb': 'bbeafdb2ea6d4020a4d7602ed73a3d60370202a93ebbbdc6f4f85544158c5885',
    'wp-nowrap-hscroll.xrgb': '158483991ed8ca49ba48142ab2675df8b55c4d92ff265ff6c431ebcdab292b6b',
    'wp-opened.xrgb': '925c2c56e7270c543e0f6e09a9cca0573ff8761ae922cf6bb71dc0181f662f43',
    'wp-options-dialog.xrgb': '0fb79f7834b898a976c7169370974130b4cac94d162c0c6259a139e1640282ba',
    'wp-paragraphs.xrgb': 'cb88677f795d8f3c6ac7447b4144e6a541e6c67676ab7cfd88c8cc171b3375c9',
    'wp-pictures-frame.xrgb': 'dc23a0b61b354c65ff031ac932114a5e257e9ec23a2dd66bc1733186361aa0fd',
    'wp-pictures.xrgb': 'b49640ab3f64a1bd76f2c41d2eaf254bc571eb466533f61691a8f995bf6ec4c2',
    'wp-preview-1.xrgb': '8f7b08096501d7feb445019f4357a6be24ff8dafb3579b44dbcf4000c87dc184',
    'wp-replace-bar.xrgb': 'fb42b76b438665529f8e961e778254061bd9becf5f0451d6ddf0b7155ea93ecc',
    'wp-ruler-bullet.xrgb': '4bce0c7a82aa10e6817c4f96c119ef26d63c204eb54a2441e2513b0cdfc5e24c',
    'wp-ruler-drag.xrgb': '7d0d2c5b39d5c32afc4b4007d414de5d76f258e71fb2647bbf13e0e8bc173918',
    'wp-ruler-units.xrgb': 'a4fa5d8a2ed190f0618d613780673d767bdea3a8dc4cba592cb571cef4d4f687',
    'wp-word-opened.xrgb': '85d0da24da0c78fdde79c539d0a956a2bc62eb9b0a3d3e45144f05f52db37d01',
    'wp-word-picture.xrgb': 'fb6a86c6e0a962cac13ec02f2ae1cf8956e052e3a8773764dee1a8612b21bfb3',
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
