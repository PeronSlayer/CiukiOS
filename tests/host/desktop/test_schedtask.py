"""G30 Scheduled Tasks: folder, Add Scheduled Task wizard, properties and Maintenance Wizard
driven with synthetic input over a fake task file and clock; pinned renders. Run with nice -n 19."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import importlib.util as _review_util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

_review_spec = _review_util.spec_from_file_location("frame_review", __file__.rsplit("/", 1)[0] + "/frame_review.py")
_review = _review_util.module_from_spec(_review_spec); _review_spec.loader.exec_module(_review)

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
FLAGS = ["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-D_POSIX_C_SOURCE=200809L",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
         "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections"]
TOOLKIT = ["apps/libdeskui/draw.c", "apps/libdeskui/widgets.c", "apps/libdeskui/list.c", "apps/libdeskui/menu.c",
           "apps/libdeskui/dialog.c", "apps/libdeskui/controls.c", "apps/libdeskui/commdlg.c", "apps/libdeskui/textarea.c", "apps/libdeskui/keymap.c", "apps/desktop/compositor.c",
           "apps/desktop/theme.c", "apps/libdeskui/icons.c", "apps/libdeskui/icon_map.c"]
SOURCES = ["apps/systools/schedtask_ui.c", "apps/systools/sched.c", "apps/systools/service.c", "tests/host/desktop/schedtask_test.c"]
PIN = "PIN-FROM-FIRST-RUN"
# fixd: visually reviewed combined G11/G19/G23/G30 integration frames (2026-10-24).
RENDERS = {
    "folder-empty": "22214bcc3b5e3a5a973139fba0bed5ac54753a494de69ed377bd8f3344665438",
    "folder-tasks": "53609bd9d7d5a3b4dc8c2f69879fe86afabda45b4658072f13a1031a26a51bc3",
    "maint-finish": "9f66c0d90b5735082b0cb946aa3b50c879450bd497e38dec1767290b2b4ab7bc",
    "maint-slot": "763a3111e12b071a046057a868838e6adaac58f227ebaabbba76580eb7c47966",
    "maint-welcome": "b297aa940dbddda220ee23e49866478c08946f6a741352af25742655233f59e9",
    "props-schedule": "820367e516d22ba65e07de14b92b4162d7dca2e10960015b7fa0f9a82b434771",
    "props-settings": "e7be053eca1ae67708908d955821857d71547182f927d655021f4420747f8c8e",
    "props-task": "20b3a86de1d2166f56ef1e2fe794774b612ab8e0f762f8fbc5e85c74e597c879",
    "wizard-finish": "373e0da66c10c02f24abd210062cc1376d50071adad9063ba5a759bf66c3622e",
    "wizard-name": "d14c0861befe5793c5e4b62e301e5fb7b8ed52b0dee89630854189628756cb73",
    "wizard-program": "c67f8edbed12aebdeb5c7dd44d158906dcf39e8aae0e9e511ba08b191196a8f2",
    "wizard-welcome": "aba2b02842ae479e2d1d8f044395a7c69ef0a39cca6d0c1401324ddbf02ebc8e",
    "wizard-when-daily": "48f5783d8d30fbb763fc74a6e678fe8d0a06d37c8ebd8441b8b3734b22adc6d6",
    "wizard-when-weekly": "3347092bb35a91b185a9916b5497c48b21f60f5794b238fbd10dbe1e4b35583c"
}


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class SchedtaskTests(unittest.TestCase):
    def test_windows_and_renders(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="schedtask-", dir=parent) as temp:
            folder = Path(temp)
            renders = folder / "renders"; renders.mkdir()
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0")
            icons = load("schedtask_icon_assets", ROOT / "tests/host/desktop/icon_assets.py")
            binary = folder / "schedtask_test"
            subprocess.run([*FLAGS, *icons.defines(), "-I", str(ROOT / "apps/systools"), "-I", str(ROOT / "apps/libdeskui"),
                            "-I", str(ROOT / "apps/desktop"), "-I", str(ROOT / "src/kernel/include"),
                            *(str(ROOT / s) for s in [*SOURCES, *TOOLKIT]), "-lm", "-o", str(binary)], check=True, env=env)
            result = subprocess.run([str(binary), str(renders)], env=env, capture_output=True, text=True, timeout=300)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS schedtask", result.stdout)
            actual = {}
            for p in sorted(renders.glob("*.xrgb")):
                width, height = map(int, p.with_suffix(".size").read_text().split())
                _review.capture(p.stem, p.read_bytes(), width, height)
                actual[p.stem] = hashlib.sha256(p.read_bytes()).hexdigest()
            self.assertEqual(sorted(actual), sorted(RENDERS))
            fresh = {k: v for k, v in actual.items() if RENDERS[k] == PIN}
            if fresh:
                print("RENDERS", PIN, json.dumps(fresh, indent=1, sort_keys=True))
            if os.environ.get("CIUKI_UPDATE_SCHEDTASK_GOLDENS") == "1":
                import re
                source = Path(__file__)
                updated = re.sub(r'RENDERS = \{.*?\n\}', "RENDERS = " + json.dumps(actual, indent=4, sort_keys=True), source.read_text(), count=1, flags=re.S)
                source.write_text(updated)
                RENDERS.update(actual)
            self.assertEqual({k: v for k, v in actual.items() if RENDERS[k] != PIN},
                             {k: v for k, v in RENDERS.items() if v != PIN})
            # Same input, same pixels.
            again = folder / "again"; again.mkdir()
            subprocess.run([str(binary), str(again)], env=env, check=True, capture_output=True)
            for p in sorted(again.glob("*.xrgb")):
                self.assertEqual(hashlib.sha256(p.read_bytes()).hexdigest(), actual[p.stem], p.stem)


if __name__ == "__main__":
    unittest.main()
