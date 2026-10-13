"""Network tray production shell with fake desktop resources; no QEMU."""
# SPDX-License-Identifier: MIT
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class NetworkTrayTests(unittest.TestCase):
    def test_tray(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="network-tray-", dir=parent) as temp:
            sources = [ROOT / "apps/desktop" / name for name in
                       ("protocol.c", "client.c", "input.c", "compositor.c", "theme.c", "shell.c",
                        "settings.c", "wallpaper.c", "keymap.c", "folder.c", "../files/model.c", "../files/transfer.c")]
            sources += [ROOT / "apps/libdeskui/draw.c", ROOT / "tests/host/desktop/fake.c",
                        ROOT / "tests/host/desktop/network_tray_test.c"]
            binary = Path(temp) / "network-tray"
            env = {**os.environ, "TMPDIR": temp, "ASAN_OPTIONS": "detect_leaks=0"}
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-Wl,--wrap=du_text",
                            "-I", str(ROOT / "src/kernel/include"), "-I", str(ROOT / "apps/desktop"),
                            *map(str, sources), "-o", str(binary)], check=True, env=env)
            subprocess.run([str(binary)], check=True, env=env)


if __name__ == "__main__":
    unittest.main()
