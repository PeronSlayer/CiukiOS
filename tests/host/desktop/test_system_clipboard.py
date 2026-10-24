"""G5 production clipboard core and native file/CLP clients; no guest."""
import pathlib
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]

class ClipboardTests(unittest.TestCase):
    def harness(self, name, sources):
        out = ROOT / "build/host/g5"
        out.mkdir(parents=True, exist_ok=True)
        binary = out / name
        subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g",
                        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                        "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections", "-pthread",
                        "-Wl,--gc-sections", "-I", str(ROOT / "src/kernel/include"),
                        "-idirafter", str(ROOT / "sdk/sysroot-overlay/include"),
                        *[str(ROOT / s) for s in sources], "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

    def test_desktop_transport(self):
        self.harness("clipboard-desktop-test", ["apps/desktop/protocol.c", "apps/desktop/clipboard.c",
                     "apps/libdeskui/clipstore.c", "tests/host/desktop/clipboard_desktop_test.c"])

    def test_viewer(self):
        self.harness("clipboard-viewer-test", ["apps/desktop/protocol.c", "apps/libdeskui/clipstore.c",
                     "apps/libdeskui/clipboard.c", "apps/libdeskui/clipfile.c", "tests/host/desktop/clipboard_viewer_test.c"])

    def test_paint_producer(self):
        self.harness("clipboard-paint-test", ["apps/desktop/protocol.c", "apps/libdeskui/clipstore.c",
                     "apps/libdeskui/clipboard.c", "tests/host/desktop/paint_clipboard_test.c"])

    def test_dos_window(self):
        self.harness("clipboard-dos-test", ["apps/desktop/protocol.c", "apps/libdeskui/clipstore.c",
                     "apps/libdeskui/clipboard.c", "tests/host/desktop/dos_clipboard_test.c"])

    def test_store_and_clients(self):
        out = ROOT / "build/host/g5"
        out.mkdir(parents=True, exist_ok=True)
        sources = ["apps/desktop/protocol.c", "apps/libdeskui/clipstore.c",
                   "apps/libdeskui/clipboard.c", "apps/libdeskui/clipfile.c",
                   "apps/files/clipboard.c", "apps/files/model.c",
                   "tests/host/desktop/clipboard_test.c"]
        binary = out / "clipboard-test"
        subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g",
                        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                        "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections",
                        "-Wl,--gc-sections", "-I", str(ROOT / "src/kernel/include"),
                        *[str(ROOT / s) for s in sources], "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == "__main__":
    unittest.main()
