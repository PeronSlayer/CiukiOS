"""F2-88c Amendment 2: production Control Panel job and wait integration."""
# SPDX-License-Identifier: MIT
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value

class ControlJobsTests(unittest.TestCase):
    def test_event_driven_jobs_and_idle_pages(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        icons = module("control_jobs_icons", ROOT / "tests/host/desktop/icon_assets.py")
        ft = module("control_jobs_ft", ROOT / "sdk/freetype/build.py")
        library = ft.host()
        sources = ["tests/host/desktop/control_jobs_test.c", "apps/control/app.c", "apps/control/display.c", "apps/control/fonts.c",
                   "sdk/libfont/font.c", "apps/libdeskui/text_ttf.c", "apps/desktop/protocol.c",
                   *(f"apps/desktop/{n}.c" for n in ("settings", "settings_io", "sounds", "wallpaper", "image", "compositor", "theme")),
                   *(f"apps/libdeskui/{n}.c" for n in ("draw", "widgets", "list", "menu", "dialog", "textarea", "icons", "icon_map"))]
        with tempfile.TemporaryDirectory(prefix="control-jobs-", dir=parent) as tmp:
            work = Path(tmp)
            (work / "include").mkdir()
            (work / "include/ciuki").symlink_to(ROOT / "sdk/sysroot-overlay/include/ciuki", target_is_directory=True)
            binary = work / "control-jobs"
            subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-ffunction-sections", "-fdata-sections", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                            f'-DDOSBOX_SETTINGS_DIR="{work / "settings"}"',
                            *icons.defines(), "-I", str(work / "include"), "-I", str(ROOT / "src/kernel/include"),
                            *ft.includes(), *(str(ROOT / s) for s in sources), str(library),
                            "-Wl,--gc-sections", "-pthread", "-lm", "-o", str(binary)], check=True)
            # Keep leak detection and all caller sanitizer settings unchanged.
            try:
                result = subprocess.run([binary, ROOT / "assets/fonts/liberation"], capture_output=True, text=True,
                                        timeout=30, env={**os.environ, "TMPDIR":tmp})
            except subprocess.TimeoutExpired as error:
                self.fail(f"Control Panel timed out: {error.stdout!r} {error.stderr!r}")
            print(result.stdout, end="", flush=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS Control Panel event-driven jobs", result.stdout)

if __name__ == "__main__":
    unittest.main()
