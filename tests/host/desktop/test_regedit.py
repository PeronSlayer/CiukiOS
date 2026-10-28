"""Production Registry Editor through the desktop's libdeskui harness."""
# SPDX-License-Identifier: MIT
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[3]

class RegeditTests(unittest.TestCase):
    def test_editor(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="regedit-", dir=parent) as scratch:
            work = Path(scratch)
            (work / "include").mkdir()
            (work / "include/ciuki").symlink_to(ROOT / "sdk/sysroot-overlay/include/ciuki", target_is_directory=True)
            directory = work / "registry"
            directory.mkdir()
            env = dict(os.environ, TMPDIR=str(work), ASAN_OPTIONS="detect_leaks=0")
            sources = [ROOT / "tests/host/desktop/regedit_test.c", ROOT / "apps/regedit/app.c",
                       *sorted((ROOT / "sdk/libreg").glob("*.c")),
                       *(ROOT / "apps/libdeskui" / (s + ".c") for s in ("draw", "widgets", "list", "menu", "dialog", "controls", "commdlg", "textarea", "icons", "icon_map", "iconx", "fileicon")),
                       ROOT / "apps/desktop/compositor.c", ROOT / "apps/desktop/theme.c"]
            binary = work / "editor"
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-macro-redefined",
                            "-D_POSIX_C_SOURCE=200809L", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                            "-I", str(work / "include"), "-I", str(ROOT / "src/kernel/include"), *map(str, sources),
                            "-o", str(binary)], check=True, env=env)
            subprocess.run([str(binary), str(directory)], check=True, env=env)

if __name__ == "__main__":
    unittest.main()
