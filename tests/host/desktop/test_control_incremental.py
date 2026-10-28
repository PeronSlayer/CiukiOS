"""Control Panel: every incremental paint equals a full repaint (control_incremental_test.c). Run with nice -n 19."""
# SPDX-License-Identifier: MIT
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


class ControlIncrementalTests(unittest.TestCase):
    def test_incremental_equals_full(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        icons = module("ci_icons", ROOT / "tests/host/desktop/icon_assets.py")
        ft = module("ci_ft", ROOT / "sdk/freetype/build.py")
        library = ft.host()
        sources = ["tests/host/desktop/control_incremental_test.c", "apps/control/app.c", "apps/desktop/power_policy.c", "apps/systools/devctl.c", "apps/systools/profiles.c",
                   "apps/control/driver_pack_ui.c", "apps/control/display.c", "apps/control/fonts.c",
                   "sdk/libfont/font.c", "apps/libdeskui/text_ttf.c", "apps/desktop/protocol.c",
                   *(f"apps/desktop/{n}.c" for n in ("settings", "sounds", "wallpaper", "image", "compositor", "theme")),
                   *(f"apps/libdeskui/{n}.c" for n in ("draw", "widgets", "list", "menu", "dialog", "controls", "commdlg", "textarea", "icons", "icon_map", "iconx", "fileicon"))]
        with tempfile.TemporaryDirectory(prefix="cp-incremental-", dir=parent) as tmp:
            binary = Path(tmp) / "panel"
            subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                            "-include", str(ROOT / "tests/host/desktop/replay_base.h"), *icons.defines(),
                            "-I", str(ROOT / "apps/control"), "-I", str(ROOT / "apps/libdeskui"),
                            "-I", str(ROOT / "src/kernel/include"), "-idirafter", str(ROOT / "sdk/sysroot-overlay/include"),
                            *ft.includes(), *(str(ROOT / s) for s in sources), str(library), "-pthread", "-lm", "-o", str(binary)], check=True)
            result = subprocess.run([binary], text=True, capture_output=True, env={"ASAN_OPTIONS": "detect_leaks=0"})
            print(result.stdout, end="", flush=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS:", result.stdout)


if __name__ == "__main__":
    unittest.main()
