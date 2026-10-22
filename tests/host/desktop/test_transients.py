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
    "balloon": "908136ec7b45610918fe082dd807552f29032b7a92ea9f80175e83145e85edcf",
    "error": "310f334a2b31f450caf39691beba785a2fc3b989d24731bcca509449bcdc6d5a",
    "info": "50e827a231ec558500bc59905d529a42ecaca3851d9a7a2c7ec06c0b9a203c53",
    "loading": "c675bfec0dd20df260c2137464b819e2faaa8624138f6d4ca823bad8eb937af1",
    "notification": "45dad9289e17ef9aa84aa05fc671c6c1cf3740da76cb253cb8c7571b5e3f61af",
    "question": "a1ed1a8b878ff15b9f6a251573a41d6f268d33e137d42ceeaea7e4df665c3c06",
    "tooltip": "138ea973f5a273457dd088f5bd4f546301db138d1b71ad5d00df6a8100c841dd",
    "tooltip-media": "30f60a4a05266e870b4c44e4bb51fb4cf9e17b37ad76f8128bb23933a4599141",
    "tooltip-network": "a2978376035d6af7ea973748346082db8017a5050d8843370a38247244d3c406",
    "tooltip-task": "204f7fd3bc9c2a266fdbd422ae8bd43420f2c3cd3433ccf01231ba674365f788",
    "tooltip-volume": "ce0789616d6adc38fca93074fe4d8d59b33c9fa6e3e1c15d5e04ea55fa698cbb",
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
