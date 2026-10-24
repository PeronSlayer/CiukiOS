"""G8 real file jobs and HKCR service, ASan/UBSan with leak detection."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

def run_ui_replay(test):
    parent = ROOT / "build/host"
    parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="g8-ui-", dir=parent) as tmp:
        sources = ["tests/host/desktop/file_operations_ui_test.c", "tests/host/desktop/fake_vfs.c",
                   "apps/files/model.c", "apps/files/app.c", "apps/files/transfer.c", "apps/desktop/autoplay_content.c"]
        sources += ["apps/w32srv/" + n + ".c" for n in ("registry", "storage", "defaults", "directory", "protocol", "posix_io")]
        sources += ["apps/libdeskui/" + n + ".c" for n in ("draw", "widgets", "list", "menu", "dialog", "controls", "commdlg", "textarea", "icons", "icon_map")]
        sources += ["apps/desktop/compositor.c", "apps/desktop/theme.c"]
        binary = Path(tmp) / "ui"
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1", TMPDIR=tmp)
        from . import icon_assets
        subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I" + str(ROOT / "src/kernel/include"),
                        "-I" + str(ROOT / "sdk/win32/include"), *icon_assets.defines(),
                        *[str(ROOT / s) for s in sources], "-pthread", "-o", str(binary)], check=True, env=env)
        result = subprocess.run([str(binary), tmp], capture_output=True, text=True, env=env, timeout=60)
        from . import frame_review
        for frame in Path(tmp).glob("g8-dialog-*.xrgb"):
            frame_review.capture(frame.stem, frame.read_bytes(), 760, 520)
        test.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout, end="")

class FileOperationsTests(unittest.TestCase):
    def test_ui_replay(self):
        run_ui_replay(self)
    def test_operations_and_shared_hkcr(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="g8-operations-", dir=parent) as tmp:
            sources = ["tests/host/desktop/file_operations_test.c", "tests/host/desktop/fake_vfs.c", "apps/files/model.c"]
            sources += ["apps/w32srv/" + n + ".c" for n in ("registry", "storage", "defaults", "directory", "protocol", "posix_io")]
            binary = Path(tmp) / "operations"
            env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1", TMPDIR=tmp)
            subprocess.run(["clang", "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I" + str(ROOT / "src/kernel/include"),
                            "-I" + str(ROOT / "sdk/win32/include"), *[str(ROOT / s) for s in sources], "-o", str(binary)], check=True, env=env)
            result = subprocess.run([str(binary), tmp], capture_output=True, text=True, env=env, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout, end="")
