"""G30 Control Panel: Device Manager actions, Add New Hardware, Performance and Hardware Profiles
(control_system_tools_test.c) with pinned renders. Run with nice -n 19."""
# SPDX-License-Identifier: MIT
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
SANITIZER = "address,undefined"
UPDATE = False


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


class ControlSystemToolsTests(unittest.TestCase):
    def test_panel(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        icons = module("g30c_icons", ROOT / "tests/host/desktop/icon_assets.py")
        ft = module("g30c_ft", ROOT / "sdk/freetype/build.py")
        library = ft.host()
        sources = ["tests/host/desktop/control_system_tools_test.c", "apps/control/app.c", "apps/desktop/power_policy.c", "apps/systools/devctl.c", "apps/systools/profiles.c",
                   "apps/control/driver_pack_ui.c", "apps/control/display.c", "apps/control/fonts.c",
                   "sdk/libfont/font.c", "apps/libdeskui/text_ttf.c", "apps/desktop/protocol.c",
                   *(f"apps/desktop/{n}.c" for n in ("settings", "sounds", "wallpaper", "image", "compositor", "theme")),
                   *(f"apps/libdeskui/{n}.c" for n in ("draw", "widgets", "list", "menu", "dialog", "controls", "commdlg", "textarea", "icons", "icon_map"))]
        with tempfile.TemporaryDirectory(prefix="g30-control-", dir=parent) as tmp:
            work = Path(tmp)
            binary = work / "panel"
            subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=" + SANITIZER, "-fno-sanitize-recover=undefined",
                            "-include", str(ROOT / "tests/host/desktop/replay_base.h"), *icons.defines(),
                            "-I", str(ROOT / "apps/control"), "-I", str(ROOT / "apps/libdeskui"),
                            "-I", str(ROOT / "src/kernel/include"), "-idirafter", str(ROOT / "sdk/sysroot-overlay/include"),
                            *ft.includes(), *(str(ROOT / s) for s in sources), str(library), "-pthread", "-lm", "-o", str(binary)], check=True)
            frames = work / "frames"
            frames.mkdir()
            result = subprocess.run([binary, frames], text=True, capture_output=True, env={"ASAN_OPTIONS": "detect_leaks=0"})
            print(result.stdout, end="", flush=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS G30 control", result.stdout)
            actual = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(frames.glob("*.xrgb"))}
            pins = ROOT / "tests/host/desktop/control-system-tools-goldens.json"
            if UPDATE:
                pins.write_text(json.dumps(actual, indent=2, sort_keys=True) + "\n")
            self.assertEqual(actual, json.loads(pins.read_text()))
            review = module("g30c_frame_review", ROOT / "tests/host/desktop/frame_review.py")
            for p in frames.glob("*.xrgb"):
                review.capture(p.stem, p.read_bytes(), 560, 420)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--update-goldens", action="store_true")
    args, remaining = parser.parse_known_args()
    UPDATE = args.update_goldens
    unittest.main(argv=[sys.argv[0], *remaining])
