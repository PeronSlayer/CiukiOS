"""G4: taskbar menu, window arrangement and the Windows-key shortcuts of the production desktop."""
# SPDX-License-Identifier: MIT
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class TaskbarTests(unittest.TestCase):
    def test_taskbar_arrangement_and_windows_keys(self):
        sources = [ROOT / f"apps/desktop/{n}.c" for n in
                   ("client", "input", "protocol", "compositor", "theme", "shell", "autoplay", "autoplay_content",
                    "settings", "wallpaper", "keymap", "folder", "power_policy")]
        sources += [ROOT / f"apps/libdeskui/{n}.c" for n in
                    ("draw", "widgets", "controls", "list", "menu", "dialog", "textarea", "icons", "icon_map")]
        sources += [ROOT / "apps/files/model.c", ROOT / "apps/files/transfer.c", ROOT / "tests/host/desktop/fake.c",
                    ROOT / "tests/host/desktop/taskbar_test.c"]
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0"}
        with tempfile.TemporaryDirectory(prefix="taskbar-", dir=parent) as temp:
            folder = Path(temp)
            objects = []
            for index, path in enumerate(sources):
                obj = folder / f"{index}.o"
                subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                                *(["-D_POSIX_C_SOURCE=200809L"] if "libdeskui" in str(path) else []),
                                "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                                "-I", str(ROOT / "apps/desktop"), "-I", str(ROOT / "apps/libdeskui"),
                                "-I", str(ROOT / "src/kernel/include"), "-c", str(path), "-o", str(obj)],
                               check=True, env=env)
                objects.append(str(obj))
            binary = folder / "taskbar"
            subprocess.run(["clang", "-fsanitize=address,undefined", *objects, "-o", str(binary)], check=True, env=env)
            result = subprocess.run([str(binary)], env=env, text=True, capture_output=True)
            print(result.stdout, end="")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr[-4000:])
            self.assertIn("PASS G4 taskbar", result.stdout)


if __name__ == "__main__":
    unittest.main()
