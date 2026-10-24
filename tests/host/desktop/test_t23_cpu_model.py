"""f2-95 T23 CPU model of the desktop (music player idle, window drag);
fx-26 timed drags and window operations (frame cost on the virtual clock).

The production compositor runs over a fake display/2D engine in virtual
time; operations are counted from the production code and charged at the
T23 rates stated in t23_cpu_model_test.c. Run with nice -n 19.
"""
# SPDX-License-Identifier: MIT
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]


class T23CpuModelTests(unittest.TestCase):
    def test_budget(self):
        out = ROOT / "build/host"
        out.mkdir(parents=True, exist_ok=True)
        sources = [ROOT / "apps/desktop" / name for name in
                   ("protocol.c", "client.c", "input.c", "compositor.c", "theme.c", "shell.c", "autoplay.c",
                    "autoplay_content.c", "settings.c", "power_policy.c", "wallpaper.c", "keymap.c", "folder.c",
                    "../files/model.c", "../files/transfer.c")]
        sources += [*(ROOT / "apps/libdeskui" / n for n in ("draw.c", "widgets.c", "list.c", "menu.c", "dialog.c",
                                                            "textarea.c", "icons.c", "icon_map.c")),
                    ROOT / "tests/host/desktop/fake.c", ROOT / "tests/host/desktop/t23_cpu_model_test.c"]
        binary = out / "t23_cpu_model_test"
        subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-builtin-memcmp", "-Wl,--wrap=memcmp",
                        "-include", str(ROOT / "tests/host/desktop/replay_base.h"),
                        "-I", str(ROOT / "src/kernel/include"), "-I", str(ROOT / "apps/desktop"),
                        *map(str, sources), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True,
                                env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"}, timeout=300)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS F2-95 T23 CPU model", result.stdout)
        self.assertIn("PASS F2-98b T23 budgets", result.stdout)
        # fx-26: drags keep the frame rate while other windows draw; no lost
        # slots after a late frame start; window operations measured.
        self.assertIn("PASS fx-26 T23 window drag keeps the frame rate", result.stdout)
        self.assertIn("PASS fx-26 T23 timed drags", result.stdout)
        self.assertIn("PASS fx-26 T23 window operations", result.stdout)
        print(result.stdout, end="")


if __name__ == "__main__":
    unittest.main()
