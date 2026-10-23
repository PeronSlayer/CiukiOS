"""About CiukiOS credits page (f2-97): golden frames of the scrollable credits through the real input paths
(End, Page Down, wheel) and the identity header; host only, no QEMU."""
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
W, H = 800, 600
# Pinned on the production compositor with the host fixture (compositor_capture.c, 800x600, version "1.0.0 alpha").
# F2-99 cursor; F1-109 ACPICA and F2-123 hardware-ID credits rows, reviewed in integ-fix1;
# integ-periph touchpad, serial mouse, ATA chipset and audio driver rows, reviewed in integ-periph.
GOLDENS = {
    "about-credits": "d67f22a29f7af79b36c2dcec44d8794927bab79a7db6e652e83f9b7ba3544ee7",
    "about-credits-end": "6fb0e64469d097a15ab521eeec234b6c43562a4d20ef294f50287abbc9da86c8",
    "about-credits-pgdn": "6af924a28fd225ddd670923e5cf999e709d37c1a0e91bd7271b1edb2de720a91",
    "about-credits-wheel": "ead164e1af9b8fdc8037959c55f6071d06426600e2710b3f2990ddec91147849"
}


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class AboutCreditsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix="about-", dir=parent)
        folder = Path(cls.temp.name)
        env = dict(os.environ, TMPDIR=str(folder), ASAN_OPTIONS="detect_leaks=0")
        sources = [ROOT / "apps/desktop" / f"{n}.c" for n in ("client", "protocol", "input", "compositor", "theme", "shell", "autoplay",
                                                           "autoplay_content", "settings", "settings_io", "wallpaper", "keymap", "folder")]
        sources += [ROOT / "apps/files" / n for n in ("model.c", "transfer.c")]
        sources += [ROOT / "apps/libdeskui" / f"{n}.c" for n in ("draw", "widgets", "list", "menu", "dialog", "textarea", "icons", "icon_map")]
        sources += [ROOT / "tests/host/desktop" / n for n in ("fake.c", "compositor_capture.c")]
        binary = folder / "capture"
        subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-include", str(ROOT / "tests/host/desktop/replay_base.h"), "-O1",
                        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections",
                        "-Wl,--gc-sections", "-I", str(ROOT / "apps/desktop"), "-I", str(ROOT / "src/kernel/include"),
                        *map(str, sources), "-o", str(binary)], check=True, env=env)
        portrait = folder / "portrait.xrgb"
        portrait.write_bytes(load("about_portrait", ROOT / "apps/desktop/convert_portrait.py").convert(ROOT / "assets/brand/ciuki-logo.png"))
        cls.frames = {}
        for mode in GOLDENS:
            target = folder / (mode + ".xrgb")
            subprocess.run([str(binary), str(portrait), str(target), mode, f"{W}x{H}"], check=True, env=env)
            cls.frames[mode] = target.read_bytes()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_golden_frames(self):
        digests = {mode: hashlib.sha256(data).hexdigest() for mode, data in self.frames.items()}
        for mode, data in self.frames.items():
            self.assertEqual(len(data), W * H * 4)
            load("about_review", ROOT / "tests/host/desktop/frame_review.py").capture(mode, data, W, H)
        self.assertEqual(digests, GOLDENS)

    def test_scrolling_changes_only_the_list(self):
        top, end, pgdn, wheel = (self.frames[m] for m in ("about-credits", "about-credits-end", "about-credits-pgdn", "about-credits-wheel"))
        self.assertEqual(len({top, end, pgdn, wheel}), 4)
        row = W * 4
        # The identity header (portrait, name, tagline, version) occupies the window's rows 108..170 and never scrolls.
        for frame in (end, pgdn, wheel):
            self.assertEqual(frame[110 * row:170 * row], top[110 * row:170 * row])
        # End shows the last group; the list differs from the top below the header.
        self.assertNotEqual(top[185 * row:410 * row], end[185 * row:410 * row])

    def test_credit_names_are_in_the_dialog_data(self):
        text = (ROOT / "apps/desktop/credits.inc").read_text()
        for name in ("FreeDOS kernel", "Wine reused DLLs", "lwIP", "pixman", "Tango Icon Theme", "Ciuki portrait"):
            self.assertIn(name, text)
        shell = (ROOT / "apps/desktop/shell.c").read_text()
        self.assertIn("Classic computing, one woof at a time.", shell)
        self.assertIn("/system/licenses", shell)
        self.assertIsNone(re.search(r"MIT licence", shell[shell.index("credits_page"):shell.index("credits_page") + 6000]))


if __name__ == "__main__":
    unittest.main()
