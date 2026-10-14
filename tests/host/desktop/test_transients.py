"""Transient UI production paints, interaction checks and pinned review PNGs."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
GOLDENS = {'balloon': 'a1e54c03ff7cdbb485196d19db2b316aff0b8d945fcce2fbdcd9be3e73fd1f72',
 'error': 'cd0c8d56c896d3c4617527222ff2d99befd1a6d9bebed9fbf24d94b733ab1f56',
 'info': 'a29acea859c8e09b89309d1c7411c8d4b6d5b0f855110a113dc2d3db8498b2cc',
 'loading': '58e99c7c2f72ee8aa0f919b7fcfbe42f546ba2bc91c131abffb1c46e69d432f5',
 'notification': '9a943bc249eace6c3b08ba3a052b2eef35af2cbbdd9481340914d63514d59384',
 'question': '48ef625174e36f163cf0581664f357f8c3e4a64a61a1d28fd0c47b8b6973e2c2',
 'tooltip': 'c98c8f001d7bcb260d1082c0150add3bcfd8479a5d49f10a709688e0c223894f',
 'tooltip-media': '8b2768644838f744c4c79401e70aaa4fda621875f2022bdd2e1a36d5b840891f',
 'tooltip-network': '29983ef059e19c4edc8ccd42e70d8c405d664c89f05eabfa32576f594d83869d',
 'tooltip-task': 'cff08cd83fffebffc373cf97c2ff02ef7f2f1a407d120ae6f8b0dfdc2bee3bed',
 'tooltip-volume': '595659c41a7b3c771fa9e982fef19d53a7f319ac55bf206ca94d423b98aeff8b',
 'warning': '215580166dad778ec2c666060502f4dba311e2b7a6e72bdf62ddbbb35aef2ef8'}

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

class TransientTests(unittest.TestCase):
    def test_transients(self):
        parent = ROOT / "build/host/f2-64"
        out = parent / "after"
        out.mkdir(parents=True, exist_ok=True)
        icons = load("transient_icons", ROOT / "tests/host/desktop/icon_assets.py")
        converter = load("transient_portrait", ROOT / "apps/desktop/convert_portrait.py")
        sources = [f"apps/desktop/{n}.c" for n in
                   ("client", "protocol", "input", "compositor", "theme", "shell", "autoplay",
                    "autoplay_content", "settings", "wallpaper", "keymap", "folder")]
        sources += [f"apps/libdeskui/{n}.c" for n in
                    ("draw", "widgets", "list", "menu", "dialog", "textarea", "icons", "icon_map")]
        sources += ["apps/files/model.c", "apps/files/transfer.c", "tests/host/desktop/fake.c",
                    "tests/host/desktop/transient_test.c"]
        with tempfile.TemporaryDirectory(dir=parent) as temp:
            folder = Path(temp)
            portrait = folder / "portrait.xrgb"
            portrait.write_bytes(converter.convert(ROOT / "assets/brand/ciuki-logo.png"))
            binary = folder / "transients"
            objects = []
            for i, source in enumerate(sources):
                obj = folder / f"{i}.o"
                subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                                *(["-D_POSIX_C_SOURCE=200809L"] if "libdeskui" in source or "transient_test" in source else []),
                                "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                                "-I", str(ROOT / "apps/desktop"), "-I", str(ROOT / "src/kernel/include"),
                                *icons.defines(), "-c", str(ROOT / source), "-o", str(obj)], check=True)
                objects.append(str(obj))
            subprocess.run(["clang", "-fsanitize=address,undefined", *objects, "-o", str(binary)], check=True)
            result = subprocess.run([str(binary), str(out), str(portrait)], capture_output=True, text=True,
                                    env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        from PIL import Image
        digests = {}
        for raw in sorted(out.glob("*.xrgb")):
            data = raw.read_bytes()
            digests[raw.stem] = hashlib.sha256(data).hexdigest()
            Image.frombytes("RGB", (640,480), data, "raw", "BGRX").save(raw.with_suffix(".png"))
            raw.unlink()
        print("TRANSIENT_GOLDENS", digests)
        self.assertEqual(digests, GOLDENS)

if __name__ == "__main__":
    unittest.main()
