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
# 2026-10-15 (main after f2-63 + f2-64): re-recorded on the combined tree (FreeType text), reviewed against the 9a05ab9f renders.
GOLDENS = {'balloon': '4db4166909a0b1042ebbd860892a6fc5e39f22f8dc8ada77ce17171a929b2998',
 'error': '310f334a2b31f450caf39691beba785a2fc3b989d24731bcca509449bcdc6d5a',
 'info': '50e827a231ec558500bc59905d529a42ecaca3851d9a7a2c7ec06c0b9a203c53',
 'loading': 'c675bfec0dd20df260c2137464b819e2faaa8624138f6d4ca823bad8eb937af1',
 'notification': '56778009ed9a4259eab689163ddde2679880ba7d6ac4812e18af3c83a6e0a702',
 'question': 'a1ed1a8b878ff15b9f6a251573a41d6f268d33e137d42ceeaea7e4df665c3c06',
 'tooltip': 'df824cb2998e9c2fb263a620afb2aca9c8b7ee8dd79b3df24a97c5a65b2a8d61',
 'tooltip-media': 'd23c96aacda7662b3b7b0730eaf08c4a76b7f507688841fed1cc102003c9a7cf',
 'tooltip-network': 'd8952ce42edcdcda9dea4e651fa0959896883bb0f7785c757670be900294878f',
 'tooltip-task': '0e5dbe0d7229ed371bb504f95416e579a2ea8470b24d65d7167d589e586e140a',
 'tooltip-volume': '7e86b53274a00d2ed44a871fc397508f5aefdb04ba645857093901c23a52df44',
 'warning': '43a94b327cd5807be7f35b7c2aad42f07341c63d8208f757cf6b3000c4951177'}

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
