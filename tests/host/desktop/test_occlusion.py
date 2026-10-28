"""fx-2: windows never repaint over the windows above them (production desktop, four presenters)."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

class OcclusionTests(unittest.TestCase):
    def test_covering_windows_and_z_order(self):
        sources = [ROOT / "apps/desktop" / name for name in
                   ("protocol.c", "client.c", "input.c", "compositor.c", "theme.c", "shell.c", "autoplay.c",
                    "autoplay_content.c", "settings.c", "power_policy.c", "wallpaper.c", "keymap.c", "folder.c", "../files/model.c", "../files/transfer.c")]
        sources += [*(ROOT / "apps/libdeskui" / n for n in ("draw.c", "widgets.c", "list.c", "menu.c", "dialog.c", "controls.c", "commdlg.c", "textarea.c", "icons.c", "icon_map.c", "iconx.c", "fileicon.c")),
                    ROOT / "tests/host/desktop/fake.c", ROOT / "src/kernel/drivers/video/swdib.c", ROOT / "src/kernel/drivers/video/batch.c",
                    ROOT / "tests/host/desktop/occlusion_test.c"]
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="occlusion-", dir=parent) as temp:
            binary = Path(temp) / "occlusion_test"
            subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                            "-include", str(ROOT / "tests/host/desktop/replay_base.h"),
                            "-include", str(ROOT / "tests/host/desktop/paint_probe.h"),
                            "-I", str(ROOT / "src/kernel/include"), "-I", str(ROOT / "apps/desktop"),
                            *map(str, sources), "-o", str(binary)], check=True)
            # Leak detection stays on: every scenario releases what it opened.
            result = subprocess.run([str(binary)], text=True, capture_output=True)
            print(result.stdout, end="")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS fx-2 occlusion", result.stdout)

if __name__ == "__main__":
    unittest.main()
