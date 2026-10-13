"""Small sequential host suite for the production Savage and desktop presenters."""
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]

class AccelerationTests(unittest.TestCase):
    def test_registers(self):
        out = ROOT / "build/host"
        out.mkdir(parents=True, exist_ok=True)
        flags = ["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined"]
        subprocess.run([*flags, "-I", str(ROOT / "src/kernel/include"), str(ROOT / "tests/host/savage2d_test.c"),
                        "-o", str(out / "savage2d_test")], check=True)
        subprocess.run([str(out / "savage2d_test")], check=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})

    def test_visible_frames(self):
        out = ROOT / "build/host"
        out.mkdir(parents=True, exist_ok=True)
        sources = [ROOT / "apps/desktop" / name for name in
                   ("protocol.c", "client.c", "input.c", "compositor.c", "theme.c", "shell.c", "autoplay.c", "autoplay_content.c", "settings.c", "wallpaper.c", "keymap.c", "folder.c", "../files/model.c", "../files/transfer.c")]
        sources += [ROOT / "apps/libdeskui/draw.c", ROOT / "tests/host/desktop/fake.c", ROOT / "tests/host/desktop/accel_test.c"]
        subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                        "-I", str(ROOT / "src/kernel/include"), "-I", str(ROOT / "apps/desktop"),
                        *map(str, sources), "-o", str(out / "desktop_accel_test")], check=True)
        subprocess.run([str(out / "desktop_accel_test")], check=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})

if __name__ == "__main__":
    unittest.main()
