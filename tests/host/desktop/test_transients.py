"""Transient UI production paints, interaction checks and pinned review PNGs."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

# F2-81: intentional two-bar shell/theme changes; docs/design/desktop-shell.md.

ROOT = Path(__file__).resolve().parents[3]
# 2026-10-15 (main after f2-63 + f2-64): re-recorded on the combined tree (FreeType text), reviewed against the 9a05ab9f renders.
# F2-67: fixed-size captions omit the maximize button; paired task-tip PNGs await lead review.
# F2-72: only balloon, notification and tooltip include the changed DMZ-White overlay.
# F2-74: balloon/notification include the explicit close affordance; media replay is pinned separately.
# F2-82: orb changes in frames containing desktop chrome; dialogs stay pinned.
GOLDENS = {
    "balloon": "9fa57eba71861fae519da8c89d4e472d1e0e4fdd8cd806b15a361c2555f626df",
    "error": "310f334a2b31f450caf39691beba785a2fc3b989d24731bcca509449bcdc6d5a",
    "info": "50e827a231ec558500bc59905d529a42ecaca3851d9a7a2c7ec06c0b9a203c53",
    "loading": "c675bfec0dd20df260c2137464b819e2faaa8624138f6d4ca823bad8eb937af1",
    "notification": "60af77232670729bcdafc7619fa48a71f38961b9e058aa85ce98dad33339b27c",
    "question": "a1ed1a8b878ff15b9f6a251573a41d6f268d33e137d42ceeaea7e4df665c3c06",
    "tooltip": "76b851afa53d3f6c0c19eb8ae3eda960a191bb453fa90d866f57c0bf5a611b9d",
    "tooltip-media": "33475de8acdab984dd3d9f574d3f1d2d8c3f24577d0a67a423fcbc5928e02be6",
    "tooltip-network": "3f46a7922fc218b479aecf3ef520e356a6fd8b5b1098b931bbe5516309fa7298",
    "tooltip-task": "990426e00196181d7cf6daae096216bde03fd96e7d2939584e1dc09a9382687a",
    "tooltip-volume": "9f6ddc3d8c7adf2f7fdd673b28b74554494ef856d42f196e699880bdcc4316b3",
    "warning": "43a94b327cd5807be7f35b7c2aad42f07341c63d8208f757cf6b3000c4951177"
}


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

class TransientTests(unittest.TestCase):
    def test_transients(self):
        parent = ROOT / "build/host/f2-74/transients"
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
