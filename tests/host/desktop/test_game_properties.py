"""Production panel/persistence and virtual peripheral launch; no image or VM."""
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

ROOT = Path(__file__).resolve().parents[3]
SANITIZER = "address,undefined"
UPDATE = False

def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result

class GamePropertiesTests(unittest.TestCase):
    def test_vm_ports(self):
        out = ROOT / "build/host/game-ports"
        out.mkdir(parents=True, exist_ok=True)
        binary = out / ("vm-" + SANITIZER.replace(",", "-"))
        subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=" + SANITIZER, "-fno-sanitize-recover=undefined",
                        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                        "-I", str(ROOT / "src/kernel/include"),
                        str(ROOT / "tests/host/game_ports_vm_test.c"), str(ROOT / "src/kernel/lib/fmt.c"),
                        "-o", str(binary)], check=True)
        subprocess.run([binary], check=True)

    def test_properties(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        icons = module("game_icons", ROOT / "tests/host/desktop/icon_assets.py")
        ft = module("game_ft", ROOT / "sdk/freetype/build.py")
        library = ft.host()
        sources = ["tests/host/desktop/game_properties_test.c", "apps/control/app.c", "apps/desktop/power_policy.c", "apps/systools/devctl.c", "apps/systools/profiles.c", "apps/control/driver_pack_ui.c", "apps/control/display.c", "apps/control/fonts.c",
                   "sdk/libfont/font.c", "apps/libdeskui/text_ttf.c", "apps/desktop/protocol.c",
                   *(f"apps/desktop/{n}.c" for n in ("settings", "sounds", "wallpaper", "image", "compositor", "theme")),
                   *(f"apps/libdeskui/{n}.c" for n in ("draw", "widgets", "list", "menu", "dialog", "controls", "commdlg", "textarea", "icons", "icon_map", "iconx", "fileicon"))]
        with tempfile.TemporaryDirectory(prefix="game-properties-", dir=parent) as tmp:
            work = Path(tmp)
            binary = work / "panel"
            subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=" + SANITIZER, "-fno-sanitize-recover=undefined",
                            "-include", str(ROOT / "tests/host/desktop/replay_base.h"), *icons.defines(),
                            f'-DDOSBOX_SETTINGS_DIR="{work / "settings"}"',
                            "-I", str(ROOT / "apps/control"), "-I", str(ROOT / "apps/libdeskui"),
                            "-I", str(ROOT / "src/kernel/include"), "-idirafter", str(ROOT / "sdk/sysroot-overlay/include"),
                            *ft.includes(), *(str(ROOT / s) for s in sources), str(library), "-pthread", "-lm", "-o", str(binary)], check=True)
            frames = work / "frames"
            frames.mkdir()
            result = subprocess.run([binary, frames], text=True, capture_output=True)
            print(result.stdout, end="", flush=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            actual = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(frames.glob("*.xrgb"))}
            pins = ROOT / "tests/host/desktop/game-properties-goldens.json"
            if UPDATE:
                pins.write_text(json.dumps(actual, indent=2, sort_keys=True) + "\n")
            review = module("game_frame_review", ROOT / "tests/host/desktop/frame_review.py")
            for p in frames.glob("*.xrgb"):
                review.capture(p.stem, p.read_bytes(), 560, 436)
            self.assertEqual(actual, json.loads(pins.read_text()))

if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--sanitizer", choices=("address,undefined", "undefined"), default=SANITIZER)
    parser.add_argument("--update-goldens", action="store_true")
    args, remaining = parser.parse_known_args()
    SANITIZER, UPDATE = args.sanitizer, args.update_goldens
    unittest.main(argv=[sys.argv[0], *remaining])
