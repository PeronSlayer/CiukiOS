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
# F2-83: shaded bars and notification surfaces; dialog-only hashes stay pinned.
# F2-99: balloon, notification and tooltip contain the new arrow/shadow; other pins unchanged.
GOLDENS = {
    "balloon": "1a50f4e6299ac74365b4b31213613843c589ff636176c09ed7a971d81540ebb0",
    "error": "310f334a2b31f450caf39691beba785a2fc3b989d24731bcca509449bcdc6d5a",
    "info": "50e827a231ec558500bc59905d529a42ecaca3851d9a7a2c7ec06c0b9a203c53",
    "loading": "c675bfec0dd20df260c2137464b819e2faaa8624138f6d4ca823bad8eb937af1",
    "notification": "0024c26dccc0cb0b4548fd993a36d9e9f079e942b0ed99adf2a686c5cabd021d",
    "question": "a1ed1a8b878ff15b9f6a251573a41d6f268d33e137d42ceeaea7e4df665c3c06",
    "tooltip": "8eca608ff18dfe326cb590d6b17398be90ee0f615a805474feb02105a5cb7ebd",
    "tooltip-media": "cc021a1c313df85129207d682aa7c0512fdc4a52ea7abff0b6ffff8ae5c3d36a",
    "tooltip-network": "6366dfe19076cbe6cae6715f89e6837ee657ec023aeed48237b44f363d0b0231",
    "tooltip-task": "1a2eaa5b220946c63f8a1a5efae411c4037f0af574f62ec6c06011f6cd20ac2d",
    "tooltip-volume": "461b1f47a3b78607795b90cfc57c5b2982047b2f1352e50956a13cdaaf2ab6ef",
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
                    "autoplay_content", "settings", "power_policy", "wallpaper", "keymap", "folder")]
        sources += [f"apps/libdeskui/{n}.c" for n in
                    ("draw", "widgets", "list", "menu", "dialog", "controls", "commdlg", "textarea", "icons", "icon_map")]
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
