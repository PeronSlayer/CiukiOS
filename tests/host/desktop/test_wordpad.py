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
# production renderer. wp-formatted, wp-font-dialog and wp-preview-1 were reviewed as screenshots on 2026-10-24; the others are
# pinned from their first run and still await the lead's visual review. A change here is a visible change of WordPad or libdeskui.
APP_SCREENS = {
    'wp-context.xrgb': '29b1d0410cd0be779de08745e21ed093918e833cb9e9f3cdab1046a5fa47cace',
    'wp-empty.xrgb': '35837779bd6597aaa3103a098d412fcee0d71fe105285e76af95b1bd2795d47f',
    'wp-find.xrgb': '2fafbf33988ad291f70c253fbebff07d04e0bfebf23d0bb860be380f1ed62fe6',
    'wp-font-dialog.xrgb': 'be98b861edda8a12d9a6072601150f86d068844de23ed88966a74c7a9d61e15e',
    'wp-formatted.xrgb': '49569d74571647c173c59742a5b145017e85040adbfdb94aaf6a05d0ae0c5619',
    'wp-opened.xrgb': 'f980de8c3eb240b5fd30f6e8291289696950dbb54ce570043fa0aa1c13ff836c',
    'wp-paragraphs.xrgb': '211378946faf2f643009248f5de6a19622238a3217a8bebbca9193d90a195e87',
    'wp-preview-1.xrgb': 'd293be40bb22befc764d9fb71dd387597bcee56d6ccc1a0160ca7d24a1f68b3f',
    'wp-replace-bar.xrgb': '69f2008e37e8d51c4e822e3fad75093c38536b4635ce55af638ec03fa6fbe595',
    'wp-ruler-bullet.xrgb': 'dda952c18455ed4eba9b3072e2f4070f9552e3f9930f2e52c17062c9d2489d09',
}
FLAGS = ["-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-macro-redefined",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections"]
INCLUDES = ["-I", str(ROOT / "apps/wordpad"), "-I", str(ROOT / "apps/libdeskui"), "-I", str(ROOT / "apps/desktop"),
            "-I", str(ROOT / "src/kernel/include")]
TOOLKIT = ["apps/libdeskui/draw.c", "apps/libdeskui/widgets.c", "apps/libdeskui/list.c", "apps/libdeskui/menu.c",
           "apps/libdeskui/dialog.c", "apps/libdeskui/controls.c", "apps/libdeskui/commdlg.c", "apps/libdeskui/textarea.c", "apps/libdeskui/recent.c", "apps/libdeskui/keymap.c",
           "apps/desktop/compositor.c", "apps/desktop/theme.c", "apps/libdeskui/icons.c", "apps/libdeskui/icon_map.c"]
CORE = ["apps/wordpad/rich.c", "apps/wordpad/rtf.c", "apps/wordpad/layout.c"]


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

    def run_binary(self, folder, binary, *args):
        env = dict(os.environ, TMPDIR=str(folder), ASAN_OPTIONS="detect_leaks=0")
        result = subprocess.run([str(binary), *args], env=env, capture_output=True, text=True, timeout=900)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_model_rtf_text_and_layout(self):
        with self.scratch() as temp:
            folder = Path(temp)
            binary = self.compile(folder, "model", [*CORE, "tests/host/desktop/wordpad_model_test.c"])
            self.assertIn("PASS wordpad model", self.run_binary(folder, binary))

    def test_application_with_fake_file_system(self):
        with self.scratch() as temp:
            folder = Path(temp)
            binary = self.compile(folder, "wordpad", [*CORE, "apps/wordpad/app.c", "apps/ciuknote/doc.c", *TOOLKIT, "tests/host/desktop/wordpad_test.c"])
            out = self.run_binary(folder, binary, str(folder / "wp"))
            self.assertIn("PASS wordpad", out)
            actual = {}
            for p in sorted(folder.glob("*.xrgb")):
                capture(p.stem, p.read_bytes(), 640, 440)
                actual[p.name] = hashlib.sha256(p.read_bytes()).hexdigest()
            self.assertEqual(sorted(actual), sorted(f"wp-{n}.xrgb" for n in (
                "empty", "formatted", "paragraphs", "font-dialog", "find", "opened", "context", "preview-1", "ruler-bullet", "replace-bar")))
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
        self.assertIn("apps/wordpad", (ROOT / "apps/desktop/build_desktop.py").read_text())
        self.assertIn('-lfont"] if name in ("control", "wordpad")', (ROOT / "apps/desktop/build_desktop.py").read_text())
        image = (ROOT / "scripts/build_image.py").read_text()
        self.assertIn('"/Programs/wordpad": DESKTOP / "wordpad"', image)
        catalog = (ROOT / "apps/desktop/catalog.h").read_text()
        self.assertRegex(catalog, r'\{"wordpad", "WordPad", "x-office-document", 1, "rtf"\}')
        shell = (ROOT / "apps/desktop/shell.c").read_text()
        self.assertRegex(shell, r'\{"wordpad", "WordPad", "x-office-document", "accessories", NULL\}')
        self.assertIn('"program:wordpad", "x-office-document"', (ROOT / "apps/libdeskui/icon_map.c").read_text())
        self.assertIn('"viewer","wordpad","lua"', (ROOT / "apps/control/programs.c").read_text())
        client = (ROOT / "apps/desktop/client.c").read_text()
        self.assertIn('c->program == DESK_APP_NOTE && !c->taskman', client)
        self.assertIn('!strcmp(p.title, "wordpad")', client)

    def test_sources_stay_toolkit_independent(self):
        """The model, the RTF code and the layout use no system call and no toolkit header."""
        for name in ("rich.c", "rtf.c", "layout.c", "rich.h", "layout.h"):
            text = (ROOT / "apps/wordpad" / name).read_text()
            self.assertNotRegex(text, r'#include\s+"\.\./libdeskui|#include\s+<(unistd|fcntl|sys/)')


if __name__ == "__main__":
    unittest.main()
