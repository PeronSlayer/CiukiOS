"""fx-15: every row of the Windows 9x window-manager model against the production desktop.

Each row of docs/design/desktop-rendering-architecture.md ("fx-15") is a
deterministic scenario, then randomized sequences mix every row; on every
presenter (software upload, 2D engine, engine with the T23 offscreen caches,
command batches at 32 and 16 bpp) each frame must equal a full redraw."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

class WindowModelTests(unittest.TestCase):
    def test_every_row_equals_a_full_redraw(self):
        sources = [*(ROOT / "apps/desktop" / n for n in ("protocol.c", "client.c", "input.c", "compositor.c", "theme.c", "shell.c", "access.c", "access_desktop.c", "autoplay.c", "autoplay_content.c", "autoplay_registry.c", "settings.c", "power_policy.c", "wallpaper.c", "keymap.c", "folder.c", "../files/model.c", "../files/transfer.c"))]
        sources += [*(ROOT / "apps/libdeskui" / n for n in ("draw.c", "widgets.c", "list.c", "menu.c", "dialog.c", "controls.c", "commdlg.c", "textarea.c", "icons.c", "icon_map.c")),
                    ROOT / "tests/host/desktop/fake.c", ROOT / "tests/host/desktop/fake_vfs.c",
                    ROOT / "apps/accessories/mixer_view.c",
                    ROOT / "src/kernel/drivers/video/swdib.c", ROOT / "src/kernel/drivers/video/batch.c",
                    ROOT / "tests/host/desktop/window_model_test.c"]
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="window-model-", dir=parent) as temp:
            binary = Path(temp) / "window_model_test"
            subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                            "-include", str(ROOT / "tests/host/desktop/replay_base.h"),
                            "-include", str(ROOT / "tests/host/desktop/paint_probe.h"),
                            "-I", str(ROOT / "src/kernel/include"), "-I", str(ROOT / "apps/desktop"),
                            *map(str, sources), "-o", str(binary)], check=True)
            runs = os.environ.get("CIUKI_WINDOW_MODEL_RUNS", "6")
            result = subprocess.run([str(binary), runs], text=True, capture_output=True)
            print(result.stdout, end="")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr[-4000:])
            self.assertIn("PASS fx-15 window model", result.stdout)
            for mode in ("software", "engine", "engine+cache", "batch32", "batch16"):
                self.assertIn(f"fx-15 {mode}: every row of the window-manager model identical to a full redraw", result.stdout)

if __name__ == "__main__":
    unittest.main()
