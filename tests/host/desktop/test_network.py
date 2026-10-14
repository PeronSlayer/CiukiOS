"""Network tray production shell with fake desktop resources; no QEMU."""
# SPDX-License-Identifier: MIT
import os
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class NetworkTrayTests(unittest.TestCase):
    def test_tray(self):
        self.run_tray("network")

    def test_removable_tray(self):
        self.run_tray("media")

    def test_layout_and_damage(self):
        self.run_tray("layout")

    def run_tray(self, kind):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="network-tray-", dir=parent) as temp:
            sources = [ROOT / "apps/desktop" / name for name in
                       ("protocol.c", "client.c", "input.c", "compositor.c", "theme.c", "shell.c", "autoplay.c", "autoplay_content.c",
                        "settings.c", "wallpaper.c", "keymap.c", "folder.c", "../files/model.c", "../files/transfer.c")]
            sources += [ROOT / "apps/libdeskui" / n for n in ("draw.c", "icons.c", "icon_map.c")]
            sources += [ROOT / "tests/host/desktop/fake.c",
                        ROOT / "tests/host/desktop" / ("tray_layout_test.c" if kind == "layout" else f"{kind}_tray_test.c")]
            spec = importlib.util.spec_from_file_location("tray_assets", ROOT / "tests/host/desktop/icon_assets.py")
            assets = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(assets)
            binary = Path(temp) / "network-tray"
            env = {**os.environ, "TMPDIR": temp, "ASAN_OPTIONS": "detect_leaks=0"}
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            *assets.defines(),
                            "-fsanitize=address,undefined", "-Wl,--wrap=du_text",
                            *(["-Wl,--wrap=du_text_wrap"] if kind=="network" else []),
                            "-I", str(ROOT / "src/kernel/include"), "-I", str(ROOT / "apps/desktop"),
                            *map(str, sources), "-o", str(binary)], check=True, env=env)
            subprocess.run([str(binary)], check=True, env=env)


if __name__ == "__main__":
    unittest.main()
