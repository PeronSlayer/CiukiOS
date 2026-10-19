"""F2-82 production orb, incremental parity, pixel goldens and review PNGs."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
GOLDENS = ROOT / "tests/host/desktop/start-orb-goldens.json"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class StartOrbTests(unittest.TestCase):
    def test_generated_shared_geometry(self):
        converter = load("orb_geometry", ROOT / "apps/desktop/convert_portrait.py")
        converter.check_geometry()
        geometry = json.loads(converter.geometry_files()["start-orb.json"])
        self.assertEqual(geometry["orb_box"], [6, 0, 38, 38])
        self.assertEqual(geometry["bar_height"], 32)
        self.assertEqual(geometry["well_diameter"], 42)
        self.assertEqual(geometry["shadow"], 1)
        self.assertEqual(geometry["portrait"], {"size": 24, "offset": [7, 7], "pressed_shift": 1})

    def test_render_hits_layering_damage_and_goldens(self):
        desktop = load("orb_desktop", ROOT / "tests/host/desktop/test_desktop.py")
        assets = load("orb_assets", ROOT / "tests/host/desktop/icon_assets.py")
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        dest = ROOT / "build/codex/f2-82-frames"
        dest.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="orb-", dir=parent) as temp:
            folder = Path(temp)
            binary = folder / "orb"
            sources = [ROOT / "apps/desktop" / f"{n}.c" for n in
                       ("client", "protocol", "input", "compositor", "theme", "image")]
            sources += desktop.SHELL_SOURCES
            sources += [ROOT / "apps/libdeskui" / n for n in ("icons.c", "icon_map.c")]
            sources += [ROOT / "tests/host/desktop" / n for n in ("fake.c", "start_orb_test.c")]
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-I", str(ROOT / "apps/desktop"), "-I", str(ROOT / "src/kernel/include"),
                            *map(str, sources), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary), str(assets.portrait()), str(folder),
                                     str(ROOT / "assets/wallpapers/ciuki/Ciuk2.png")], capture_output=True, text=True,
                                    env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout, end="")
            from PIL import Image
            digests = {}
            for raw in sorted(folder.glob("*.xrgb")):
                width, height = map(int, re.search(r"-(\d+)x(\d+)-", raw.stem).groups())
                pixels = raw.read_bytes()
                digests[raw.stem] = hashlib.sha256(pixels).hexdigest()
                image = Image.frombytes("RGB", (width, height), pixels, "raw", "BGRX")
                image.save(dest / (raw.stem + ".png"))
                image.crop((0, 0, 96, 72)).resize((192, 144), Image.Resampling.NEAREST).save(
                    dest / (raw.stem + "-orb-2x.png"))
            self.assertEqual(len(digests), 20)
            if os.environ.get("CIUKI_RECORD_ORB_GOLDENS") == "1":
                GOLDENS.write_text(json.dumps(digests, indent=2, sort_keys=True)+"\n")
            self.assertEqual(digests, json.loads(GOLDENS.read_text()))


if __name__ == "__main__":
    unittest.main()
