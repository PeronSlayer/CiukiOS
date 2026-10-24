"""G25 accessibility: the input engine, the settings file, the Control Panel applet and the desktop integration.

docs/design/accessibility.md lists the Windows 98 behaviour each test reproduces."""
# SPDX-License-Identifier: MIT
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
CFLAGS = ["-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-D_POSIX_C_SOURCE=200809L",
          "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class AccessTests(unittest.TestCase):
    def scratch(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        return tempfile.TemporaryDirectory(prefix="access-", dir=parent)

    def run_binary(self, command, env):
        result = subprocess.run(command, env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr[-4000:])
        return result.stdout

    def test_engine(self):
        """StickyKeys, FilterKeys, ToggleKeys, MouseKeys, hot keys, time-out, tones and the Magnifier geometry."""
        with self.scratch() as temp:
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0")
            binary = Path(temp) / "access_test"
            subprocess.run(["clang", *CFLAGS, str(ROOT / "apps/desktop/access.c"),
                            str(ROOT / "tests/host/desktop/access_test.c"), "-o", str(binary)], check=True, env=env)
            self.assertIn("PASS g25 accessibility engine", self.run_binary([str(binary)], env))

    def test_settings_and_theme(self):
        """The access_* keys of desktop.cfg: defaults, round trip, rejected values, kept unknown keys, high-contrast theme."""
        with self.scratch() as temp:
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0")
            binary = Path(temp) / "access_settings_test"
            subprocess.run(["clang", *CFLAGS, "-I", str(ROOT / "src/kernel/include"), "-I", str(ROOT / "apps/desktop"),
                            str(ROOT / "apps/desktop/settings.c"), str(ROOT / "apps/desktop/theme.c"),
                            str(ROOT / "tests/host/desktop/access_settings_test.c"), "-lm", "-o", str(binary)], check=True, env=env)
            self.assertIn("PASS g25 settings", self.run_binary([str(binary)], env))

    def test_control_panel(self):
        """Accessibility Options in the Control Panel: tabs, settings pages, what is written to desktop.cfg."""
        with self.scratch() as temp:
            folder = Path(temp)
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0")
            binary = folder / "access_control"
            icons = load("access_icons", ROOT / "tests/host/desktop/icon_assets.py")
            sources = ["tests/host/desktop/access_control_test.c", "apps/control/app.c", "apps/control/driver_pack_ui.c",
                       "apps/control/display.c", "apps/control/fonts.c", "sdk/libfont/font.c", "apps/libdeskui/text_ttf.c",
                       "apps/desktop/protocol.c",
                       *(f"apps/desktop/{n}.c" for n in ("settings", "sounds", "wallpaper", "image", "compositor", "theme")),
                       *(f"apps/libdeskui/{n}.c" for n in ("draw", "widgets", "list", "menu", "dialog", "textarea", "icons", "icon_map"))]
            ft = load("access_freetype", ROOT / "sdk/freetype/build.py")
            library = ft.host()
            subprocess.run(["clang", *CFLAGS, *icons.defines(), "-I", str(ROOT / "src/kernel/include"),
                            "-I", str(ROOT / "apps/control"), "-I", str(ROOT / "tests/host/desktop"),
                            "-idirafter", str(ROOT / "sdk/sysroot-overlay/include"), *ft.includes(), "-pthread",
                            *(str(ROOT / s) for s in sources), str(library), "-lm", "-o", str(binary)], check=True, env=env)
            frames = folder / "frames"
            frames.mkdir()
            out = self.run_binary([str(binary), icons.portrait(), str(frames)], env)
            self.assertIn("PASS accessibility options", out)
            self.assertEqual(len(list(frames.glob("access-*.xrgb"))), 9)
            keep = os.environ.get("CIUKI_ACCESS_FRAMES")   # review copies, never committed
            if keep:
                image = load("access_png", ROOT / "tests/host/desktop/test_desktop.py")
                Path(keep).mkdir(parents=True, exist_ok=True)
                for path in sorted(frames.glob("access-*.xrgb")):
                    width, height = map(int, path.with_suffix(".size").read_text().split())
                    image.write_png(Path(keep) / (path.stem + ".png"), width, height, image.xrgb_to_rgb(path.read_bytes()))


if __name__ == "__main__":
    unittest.main()
