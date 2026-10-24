"""G18 installed-menu and shortcut-sheet behaviour on five production presenters."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

class G18ShellTests(unittest.TestCase):
    def test_shortcut_sheet_and_installed_menu_tree(self):
        sources = [ROOT / "apps/desktop" / name for name in
                   ("protocol.c", "client.c", "input.c", "compositor.c", "theme.c", "shell.c", "autoplay.c",
                    "autoplay_content.c", "settings.c", "wallpaper.c", "keymap.c", "folder.c", "../files/model.c", "../files/transfer.c")]
        sources += [*(ROOT / "apps/libdeskui" / n for n in ("draw.c", "widgets.c", "list.c", "menu.c", "dialog.c", "textarea.c", "icons.c", "icon_map.c")),
                    ROOT / "tests/host/desktop/fake.c", ROOT / "src/kernel/drivers/video/swdib.c", ROOT / "src/kernel/drivers/video/batch.c",
                    ROOT / "tests/host/desktop/g18_shell_test.c"]
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="g18-shell-", dir=parent) as temp:
            binary = Path(temp) / "g18_shell_test"
            subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                            "-include", str(ROOT / "tests/host/desktop/replay_base.h"),
                            "-include", str(ROOT / "tests/host/desktop/paint_probe.h"),
                            "-I", str(ROOT / "src/kernel/include"), "-I", str(ROOT / "apps/desktop"),
                            *map(str, sources), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], text=True, capture_output=True)
            print(result.stdout, end="")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr[-4000:])
            self.assertIn("PASS G18 shell", result.stdout)

if __name__ == "__main__":
    unittest.main()
