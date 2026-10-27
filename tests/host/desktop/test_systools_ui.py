"""G30 system tools windows: System Information, System Monitor, Resource Meter on the production toolkit."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
import importlib.util as _review_util
_review_spec = _review_util.spec_from_file_location("frame_review", __file__.rsplit("/", 1)[0] + "/frame_review.py")
_review = _review_util.module_from_spec(_review_spec); _review_spec.loader.exec_module(_review)

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
FLAGS = ["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-D_POSIX_C_SOURCE=200809L",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
         "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections"]
TOOLKIT = ["apps/libdeskui/draw.c", "apps/libdeskui/widgets.c", "apps/libdeskui/list.c",
           "apps/libdeskui/menu.c", "apps/libdeskui/dialog.c", "apps/libdeskui/controls.c", "apps/libdeskui/commdlg.c", "apps/libdeskui/textarea.c", "apps/desktop/compositor.c", "apps/desktop/theme.c",
           "apps/libdeskui/icons.c", "apps/libdeskui/icon_map.c", "apps/libdeskui/iconx.c", "apps/libdeskui/fileicon.c"]
SYSTOOLS = ["apps/systools/sysinfo.c", "apps/systools/resmeter.c", "apps/systools/monitor.c",
            "apps/systools/sysinfo_view.c", "apps/systools/sysmon_view.c", "apps/systools/resmeter_view.c"]
INCLUDES = ["-I", str(ROOT / "apps/systools"), "-I", str(ROOT / "apps/libdeskui"), "-I", str(ROOT / "apps/desktop"),
            "-I", str(ROOT / "src/kernel/include")]
SIZES = {"sysinfo": (640, 480), "sysmon": (560, 420), "resmeter": (320, 180)}
PIN = "PIN-FROM-FIRST-RUN"
# Renders at the toolkit of 2026-10-24 from fixed synthetic data (systools_ui_test.c).
RENDERS = {
    "resmeter-critical.xrgb": "bad77672988ad77463282ff9aaad5cd3c987dab69b5606eb1a4ce21e30db1132",
    "resmeter-ok.xrgb": "afab56d2bffce0aa510e2601fb0e23416abf1e7a02d875efae86533374abb730",
    "resmeter-unknown.xrgb": "42c91e7fff065eec5ef6fc20f887a21aced56a034f309f90eb8c8774a10fa48d",
    "sysinfo-conflicts.xrgb": "abccf2ace8e690d88a66a430082508246b7f28f3ef9d1ce956c3b50b9f70f722",
    "sysinfo-launch-error.xrgb": "c7ffeb21e482eff718ac794a5b441d96d4f279744170ebd3b561fab3ca5fb53d",
    "sysinfo-summary.xrgb": "44dcdc20f1631961a22b5426b19e17624f2f96a8ec1125e896ebb8b7c154d513",
    "sysmon-add.xrgb": "9c4094f40dd16fdaf1d462e359369d171267f6a207fe5f4c9ef29b3807273069",
    "sysmon-bar.xrgb": "f7c8528bc2c58978ea3d88aff59b228c9726918f729e9c83c09962ae3ffdc050",
    "sysmon-edit.xrgb": "dfdc3d798cc27d66602fef4816c91bec3b7eaf5f9abb683662acfe1ced260970",
    "sysmon-line.xrgb": "e0b97dedd56eb2bfaa199c4cc643d9220b513217e5a381eaca8fc03a7baf97d2",
    "sysmon-numeric.xrgb": "12d7d4e0f852f9ef4e9ba0c382d4c3c22956d065b44191ddb8e9e24c3de9e7f9"
}


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ICONS = load("systools_ui_icon_assets", ROOT / "tests/host/desktop/icon_assets.py")


class SystoolsUiTests(unittest.TestCase):
    def test_windows_and_renders(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="systools-ui-", dir=parent) as temp:
            folder = Path(temp)
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0")
            binary = folder / "systools-ui"
            subprocess.run([*FLAGS, *INCLUDES, *ICONS.defines(), *(str(ROOT / s) for s in [*SYSTOOLS, *TOOLKIT]),
                            str(ROOT / "tests/host/desktop/systools_ui_test.c"), "-o", str(binary)], check=True, env=env)
            out = subprocess.run([str(binary), str(folder)], capture_output=True, text=True, env=env, timeout=300)
            self.assertEqual(out.returncode, 0, out.stdout + out.stderr)
            for name in ("System Information window", "System Monitor window", "Resource Meter window"):
                self.assertIn("PASS G30 " + name, out.stdout)
            print(out.stdout, end="")
            actual = {}
            for p in sorted(folder.glob("*.xrgb")):
                w, h = SIZES[p.stem.split("-")[0]]
                _review.capture(p.stem, p.read_bytes(), w, h)
                actual[p.name] = hashlib.sha256(p.read_bytes()).hexdigest()
        self.assertEqual(sorted(actual), sorted(RENDERS))
        fresh = {k: v for k, v in actual.items() if RENDERS[k] == PIN}
        if fresh:
            print("RENDERS", PIN, json.dumps(fresh, indent=1, sort_keys=True))
        if os.environ.get("CIUKI_UPDATE_SYSTOOLS_GOLDENS") == "1":
            import re
            source = Path(__file__)
            source.write_text(re.sub(r'RENDERS = \{.*?\n\}', "RENDERS = " + json.dumps(actual, indent=4, sort_keys=True), source.read_text(), count=1, flags=re.S))
            RENDERS.update(actual)
        self.assertEqual({k: v for k, v in actual.items() if RENDERS[k] != PIN}, {k: v for k, v in RENDERS.items() if v != PIN})


if __name__ == "__main__":
    unittest.main()
