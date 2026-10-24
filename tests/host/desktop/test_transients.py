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
    "balloon": "5c4987636835d1f8bf538dbbed00949998f5a0af37e7fcbd6197e2e78ae723bf",
    "error": "310f334a2b31f450caf39691beba785a2fc3b989d24731bcca509449bcdc6d5a",
    "info": "50e827a231ec558500bc59905d529a42ecaca3851d9a7a2c7ec06c0b9a203c53",
    "loading": "c675bfec0dd20df260c2137464b819e2faaa8624138f6d4ca823bad8eb937af1",
    "notification": "6cd42b0073861b64a5162a82d1d3b96279a29e12357b6810515c307d9b9ee369",
    "question": "a1ed1a8b878ff15b9f6a251573a41d6f268d33e137d42ceeaea7e4df665c3c06",
    "tooltip": "0d05fdfb86398c58398f6b783403a81b208483eb4ef7bd582bde3cbadb77bc20",
    "tooltip-media": "b1f9f70002fbadfe97391eba277600d6b0a59a1ba4ffcfb9e8984280ac7e60b7",
    "tooltip-network": "6ed947eaa1149185c6d52071cbd727969000250f28a77979bc6e44ead0c1b039",
    "tooltip-task": "3cb211651824166cfd6d1478466892e30060727ff2205e513a1af7be5e983c4c",
    "tooltip-volume": "82be7639974371fd0c9d165251035ce1c0e7e684b3a627ffe9b0b9e935b39310",
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
