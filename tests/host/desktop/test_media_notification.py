"""F2-74 production media replay; pinned PNGs and full/incremental pixel parity."""
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

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

class MediaNotificationTests(unittest.TestCase):
    def test_replay(self):
        parent = ROOT / "build/host/f2-74"
        out = parent / "after"
        out.mkdir(parents=True, exist_ok=True)
        icons = load("media_icons", ROOT / "tests/host/desktop/icon_assets.py")
        sources = [f"apps/desktop/{n}.c" for n in
                   ("client", "protocol", "input", "compositor", "theme", "shell", "autoplay",
                    "autoplay_content", "settings", "power_policy", "wallpaper", "keymap", "folder")]
        sources += [f"apps/libdeskui/{n}.c" for n in ("draw", "widgets", "list", "menu", "dialog", "controls", "commdlg", "textarea", "icons", "icon_map", "iconx", "fileicon")]
        sources += ["apps/files/model.c", "apps/files/transfer.c", "tests/host/desktop/fake.c",
                    "tests/host/desktop/media_notification_test.c"]
        with tempfile.TemporaryDirectory(dir=parent) as temp:
            binary = Path(temp) / "media"
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                            "-I", str(ROOT / "apps/desktop"), "-I", str(ROOT / "src/kernel/include"),
                            *icons.defines(), *(str(ROOT / s) for s in sources), "-o", str(binary)], check=True)
            subprocess.run([str(binary), str(out), str(icons.portrait())], check=True,
                           env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
        from PIL import Image
        digests = {}
        for raw in sorted(out.glob("*.xrgb")):
            width,height=map(int,re.search(r"-(\d+)x(\d+)-",raw.stem).groups())
            data = raw.read_bytes()
            digests[raw.stem] = hashlib.sha256(data).hexdigest()
            Image.frombytes("RGB", (width,height), data, "raw", "BGRX").save(raw.with_suffix(".png"))
            raw.unlink()
        (parent / "frames.json").write_text(json.dumps(digests,indent=2,sort_keys=True)+"\n")
        golden = ROOT / "tests/host/desktop/media-notification-goldens.json"
        self.assertEqual(digests,json.loads(golden.read_text()))

if __name__ == "__main__":
    unittest.main()
