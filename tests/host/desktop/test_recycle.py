"""G2 Recycle Bin host tests: the service model, Files and the shell, with
ASan/UBSan against the fake FAT-like tree. No QEMU, no image build.

The Files frames of the bin (details, menus, dialogs) are pinned by SHA-256
in recycle-goldens.json; a visual change must be reviewed and re-pinned.
"""
# SPDX-License-Identifier: MIT
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import importlib.util as _util
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
_spec = _util.spec_from_file_location("recycle_icon_assets", ROOT / "tests/host/desktop/icon_assets.py")
ICONS = _util.module_from_spec(_spec); _spec.loader.exec_module(ICONS)
_review_spec = _util.spec_from_file_location("frame_review", ROOT / "tests/host/desktop/frame_review.py")
_review = _util.module_from_spec(_review_spec); _review_spec.loader.exec_module(_review)

TOOLKIT = ["apps/libdeskui/draw.c", "apps/libdeskui/widgets.c", "apps/libdeskui/list.c",
           "apps/libdeskui/menu.c", "apps/libdeskui/dialog.c", "apps/libdeskui/controls.c", "apps/libdeskui/commdlg.c", "apps/libdeskui/textarea.c", "apps/desktop/compositor.c", "apps/desktop/theme.c",
           "apps/libdeskui/icons.c", "apps/libdeskui/icon_map.c"]
GOLDENS = ROOT / "tests/host/desktop/recycle-goldens.json"


class RecycleTests(unittest.TestCase):
    def build_and_run(self, name, sources, frames=False):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=name + "-", dir=parent) as temp:
            folder = Path(temp)
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0")
            include = folder / "include/ciuki"
            include.mkdir(parents=True)
            for header in ("channel.h", "surface.h"):
                shutil.copy2(ROOT / "sdk/sysroot-overlay/include/ciuki" / header, include / header)
            binary = folder / name
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-D_POSIX_C_SOURCE=200809L",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                            "-I", str(ROOT / "apps/libdeskui"), "-I", str(ROOT / "apps/files"), "-I", str(ROOT / "src/kernel/include"),
                            "-I", str(folder / "include"), *ICONS.defines(), *(str(ROOT / s) for s in sources), "-pthread",
                            "-o", str(binary)], check=True, env=env)
            output = folder / "frames"
            output.mkdir()
            result = subprocess.run([str(binary), *([str(output)] if frames else [])], env=env, capture_output=True, text=True, timeout=300)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            stdout = result.stdout
            print("\n".join(l for l in stdout.splitlines() if l.startswith("PASS")))
            digests = {}
            for p in sorted(output.glob("*.xrgb")):
                digests[p.name] = hashlib.sha256(p.read_bytes()).hexdigest()
                _review.capture(p.stem, p.read_bytes(), 760, 520)
            return stdout, digests

    def test_service_model(self):
        stdout, _ = self.build_and_run("recycle-model", ["tests/host/desktop/recycle_model_test.c", "tests/host/desktop/fake_vfs.c",
                                                         "apps/files/model.c"])
        self.assertIn("PASS Recycle Bin model", stdout)

    def test_files_ui(self):
        stdout, digests = self.build_and_run("recycle-ui", ["tests/host/desktop/recycle_ui_test.c", "tests/host/desktop/fake_vfs.c",
                                                            "apps/files/app.c", "apps/files/model.c", "apps/files/transfer.c", *TOOLKIT], frames=True)
        self.assertIn("PASS Files Recycle Bin UI", stdout)
        pinned = json.loads(GOLDENS.read_text()) if GOLDENS.exists() else {}
        if digests != pinned:
            print("RECYCLE_FRAMES", json.dumps(digests, indent=2, sort_keys=True))
        self.assertEqual(digests, pinned)


if __name__ == "__main__":
    unittest.main()
