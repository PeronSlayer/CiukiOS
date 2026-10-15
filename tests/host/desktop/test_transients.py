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
# F2-67: fixed-size captions omit the maximize button; paired task-tip PNGs await lead review.
# F2-72: only balloon, notification and tooltip include the changed DMZ-White overlay.
GOLDENS = {'balloon': '174154fe46feb726e28108835c0e66ae6bc896b6007d92888502c30acfad50c8',
 'error': '310f334a2b31f450caf39691beba785a2fc3b989d24731bcca509449bcdc6d5a',
 'info': '50e827a231ec558500bc59905d529a42ecaca3851d9a7a2c7ec06c0b9a203c53',
 'loading': 'c675bfec0dd20df260c2137464b819e2faaa8624138f6d4ca823bad8eb937af1',
 'notification': 'e1e1e2f589cea7ce0c723ab9f3e4d90ffffb2300cd9c73cf7c870f1c05acfa48',
 'question': 'a1ed1a8b878ff15b9f6a251573a41d6f268d33e137d42ceeaea7e4df665c3c06',
 'tooltip': '1e17b39735a0a17c5a3c479e534355895f425bd0b1a1ebd725e6b751dd9dc405',
 'tooltip-media': 'd23c96aacda7662b3b7b0730eaf08c4a76b7f507688841fed1cc102003c9a7cf',
 'tooltip-network': 'd8952ce42edcdcda9dea4e651fa0959896883bb0f7785c757670be900294878f',
 'tooltip-task': '10829c97cb36660639828ccf63f627ffb046b3df1d1dca8f5f3f9fe43ebec650',
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
