"""Production AutoPlay shell and persistent preferences; no QEMU."""
# SPDX-License-Identifier: MIT
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[3]

class AutoPlayTests(unittest.TestCase):
    def test_media_dialogs_and_preferences(self):
        parent = ROOT / "build/host"; parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="autoplay-", dir=parent) as temp:
            work = Path(temp); (work / "include").mkdir()
            (work / "include/ciuki").symlink_to(ROOT / "sdk/sysroot-overlay/include/ciuki", target_is_directory=True)
            registry = work / "registry"; registry.mkdir()
            common = ["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-macro-redefined",
                      "-D_POSIX_C_SOURCE=200809L", "-include", str(ROOT / "tests/host/desktop/replay_base.h"),
                      "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                      "-I", str(work / "include"), "-I", str(ROOT / "apps/desktop"), "-I", str(ROOT / "src/kernel/include")]
            sources = ["tests/host/desktop/autoplay_test.c", "tests/host/desktop/fake.c", "tests/host/desktop/fake_vfs.c",
                       *("apps/desktop/" + n + ".c" for n in ("client", "protocol", "input", "compositor", "theme", "shell", "autoplay", "autoplay_content", "autoplay_registry", "settings", "power_policy", "wallpaper", "keymap", "folder")),
                       "apps/files/model.c", "apps/files/transfer.c", "apps/libdeskui/draw.c","apps/libdeskui/widgets.c","apps/libdeskui/list.c","apps/libdeskui/menu.c","apps/libdeskui/dialog.c","apps/libdeskui/textarea.c", "apps/libdeskui/icons.c", "apps/libdeskui/icon_map.c",
                       *(str(p.relative_to(ROOT)) for p in sorted((ROOT / "sdk/libreg").glob("*.c")))]
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0")
            objects = []
            for i, source in enumerate(sources):
                target = work / f"{i}.o"
                subprocess.run([*common, *(["-Dfake_reset=fake_tree_reset"] if source.endswith("fake_vfs.c") else []),
                                "-c", str(ROOT / source), "-o", str(target)], check=True, env=env)
                objects.append(target)
            binary = work / "autoplay"
            subprocess.run(["clang", "-fsanitize=address,undefined", *map(str, objects), "-o", str(binary)], check=True, env=env)
            subprocess.run([str(binary), str(registry)], check=True, env=env)

if __name__ == "__main__":
    unittest.main()
