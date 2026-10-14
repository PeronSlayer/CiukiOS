"""libdeskui and Files host tests (f2-31). Scratch lives under build/host.

The C harnesses compile the production toolkit, model and Files controller
natively with ASan/UBSan. Rendered frames are pinned by SHA-256: a visual
change must be reviewed and re-pinned deliberately.
"""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
TOOLKIT = ["apps/libdeskui/draw.c", "apps/libdeskui/widgets.c", "apps/libdeskui/list.c",
           "apps/libdeskui/menu.c", "apps/libdeskui/dialog.c", "apps/libdeskui/textarea.c", "apps/desktop/compositor.c", "apps/desktop/theme.c",
           "apps/libdeskui/icons.c", "apps/libdeskui/icon_map.c"]

# Reviewed frames (2026-10-11): an unpinned run prints the new digests. Convert
# the harness frames (XRGB8888, 400x300 toolkit, 760x520 Files) to PNG and
# re-pin only after a visual review.
# wt/ui-integ (2026-10-12): Tango store icons (wt/icons) and the ui-look theme, fonts and controls together;
# F2-64 recorded affected transient frames; remaining frames await lead review.
TOOLKIT_FRAMES = {
    "deskui-input.xrgb": "09ed7a619de136347bf0b154bca71a08e48ff79639821715e5164f4009cb4676",
    "deskui-menu.xrgb": "PIN-FROM-FIRST-RUN",
    "deskui-message.xrgb": "482d3e729aba224b751cab6017f4d2b9087f267f4a005c9739598f87017acd0a",
    "deskui-open.xrgb": "PIN-FROM-FIRST-RUN",
    "deskui-popup.xrgb": "PIN-FROM-FIRST-RUN",
    "deskui-progress.xrgb": "f9a3908cfa6f1611710d2d2c06d2d0973d4eed8f76fadf2c316798c4dcf41875",
    "deskui-view0.xrgb": "PIN-FROM-FIRST-RUN",
    "deskui-view1.xrgb": "PIN-FROM-FIRST-RUN",
    "deskui-view2.xrgb": "PIN-FROM-FIRST-RUN",
}
FILES_FRAMES = {
    "files-about.xrgb": "cd6039f092df27ad4491c83886e0799b2291c41a71d641328dbdf4ed29242f4f",
    "files-programs-icons.xrgb": "PIN-FROM-FIRST-RUN",
    "files-confirm-delete.xrgb": "f03cd306204fc6aecdfa9ce422293e3e45997062a4b9595c67b3880f7f870a0d",
    "files-context-background.xrgb": "PIN-FROM-FIRST-RUN",
    "files-context-item.xrgb": "PIN-FROM-FIRST-RUN",
    "files-copy-progress.xrgb": "28aa62a90b5efa83820e0ae472055fb22f2c4791a4ebef2a812053b9483b3e61",
    "files-cp1-system.xrgb": "PIN-FROM-FIRST-RUN",
    "files-cp2-new-folder.xrgb": "PIN-FROM-FIRST-RUN",
    "files-cp3-renamed.xrgb": "PIN-FROM-FIRST-RUN",
    "files-cp4-copied.xrgb": "PIN-FROM-FIRST-RUN",
    "files-cp5-deleted.xrgb": "PIN-FROM-FIRST-RUN",
    "files-help.xrgb": "8b2aaa79ea47ed026ed2bdcf4acc55e78bad68109dd5ab3c75864cdf6c7e67c7",
    "files-menu-file.xrgb": "PIN-FROM-FIRST-RUN",
    "files-menu-view.xrgb": "PIN-FROM-FIRST-RUN",
    "files-multi-select.xrgb": "PIN-FROM-FIRST-RUN",
    "files-properties.xrgb": "2155dd2ad92efd0ad1834a0dcb6e5519f3798738f741eba0dfdbbcd5b778aca6",
    "files-rename-error.xrgb": "113bfc710d30cb2e9bf1a762536621065c80b83ddd04b30d8cdde2668f89a4c7",
    "files-replace-prompt.xrgb": "0c51b9fadaef78448a1d5a55203f00ba1941858c65168a11bee7f99e1a602d4e",
    "files-computer.xrgb": "PIN-FROM-FIRST-RUN",
    "files-c-drive.xrgb": "PIN-FROM-FIRST-RUN",
    "files-root-details.xrgb": "PIN-FROM-FIRST-RUN",
    "files-sort-size-desc.xrgb": "PIN-FROM-FIRST-RUN",
    "files-system-details.xrgb": "PIN-FROM-FIRST-RUN",
    "files-system-icons.xrgb": "PIN-FROM-FIRST-RUN",
    "files-tree.xrgb": "PIN-FROM-FIRST-RUN",
}

# ui-look (2026-10-12): the new theme engine, fonts and controls changed every
# rendered frame. Entries set to PIN-FROM-FIRST-RUN print their new digest and
# are re-pinned by the lead after a visual review; pinned entries still compare.
PIN = "PIN-FROM-FIRST-RUN"


def assert_pins(test, actual, pinned, label):
    test.assertEqual(sorted(actual), sorted(pinned))
    fresh = {k: v for k, v in actual.items() if pinned[k] == PIN}
    if fresh:
        print(label, PIN, json.dumps(fresh, indent=1, sort_keys=True))
    test.assertEqual({k: v for k, v in actual.items() if pinned[k] != PIN}, {k: v for k, v in pinned.items() if v != PIN})


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ICONS = load("deskui_icon_assets", ROOT / "tests/host/desktop/icon_assets.py")


class DeskuiTests(unittest.TestCase):
    portrait = None

    @classmethod
    def setUpClass(cls):
        cls.parent = ROOT / "build/host"
        cls.parent.mkdir(parents=True, exist_ok=True)

    def run_harness(self, name, sources, *args, frames=False):
        with tempfile.TemporaryDirectory(prefix=name + "-", dir=self.parent) as temp:
            folder = Path(temp)
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0")
            binary = folder / name
            include = folder / "include/ciuki"
            include.mkdir(parents=True)
            for header in ("channel.h", "surface.h"):
                shutil.copy2(ROOT / "sdk/sysroot-overlay/include/ciuki" / header, include / header)
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-D_POSIX_C_SOURCE=200809L",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                            "-I", str(ROOT / "apps/libdeskui"), "-I", str(ROOT / "apps/files"), "-I", str(ROOT / "src/kernel/include"),
                            "-I", str(folder / "include"),
                            *ICONS.defines(), *(str(ROOT / s) for s in sources), "-o", str(binary)], check=True, env=env)
            output = folder / "frames"
            output.mkdir()
            extra = [str(a(folder)) if callable(a) else str(a) for a in args]
            result = subprocess.run([str(binary), *([str(output)] if frames else []), *extra],
                                    check=True, env=env, capture_output=True, text=True)
            print(result.stdout.strip())
            if name == "files-ui":
                from PIL import Image
                review = ROOT / "build/f2-36"
                review.mkdir(parents=True, exist_ok=True)
                for stem in ("computer", "c-drive"):
                    raw = (output / f"files-{stem}.xrgb").read_bytes()
                    Image.frombytes("RGB", (760, 520), raw, "raw", "BGRX").save(review / f"files-{stem}.png")
            digests = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(output.glob("*.xrgb"))}
            return result.stdout, digests

    def check_frames(self, actual, pinned, label):
        if not pinned:
            print(label + "_FRAMES", json.dumps(actual, indent=1, sort_keys=True))
            self.fail(label + " frames are not pinned")
        assert_pins(self, actual, pinned, label + "_FRAMES")

    def test_toolkit_widgets_and_frames(self):
        _, digests = self.run_harness("deskui", ["tests/host/desktop/deskui_test.c", *TOOLKIT,
            "apps/desktop/protocol.c", "tests/host/desktop/resize_transport.c",
            "tests/host/desktop/resize_client.c", "tests/host/desktop/resize_client_test.c"], frames=True)
        self.assertEqual(len(digests), 9)
        self.check_frames(digests, TOOLKIT_FRAMES, "TOOLKIT")

    def test_files_model(self):
        stdout, _ = self.run_harness("files-model", ["tests/host/desktop/files_model_test.c", "tests/host/desktop/fake_vfs.c",
                                                     "apps/files/model.c"])
        self.assertIn("PASS Files model", stdout)

    def files_ui(self):
        if DeskuiTests.portrait is None:
            converter = load("deskui_portrait", ROOT / "apps/desktop/convert_portrait.py")
            DeskuiTests.portrait = converter.convert(ROOT / "assets/brand/ciuki-logo.png")
        def portrait(folder):
            path = folder / "ciuki-portrait.xrgb"
            path.write_bytes(DeskuiTests.portrait)
            return path
        return self.run_harness("files-ui", ["tests/host/desktop/files_ui_test.c", "tests/host/desktop/fake_vfs.c",
                                             "apps/files/app.c", "apps/files/model.c", "apps/files/transfer.c", *TOOLKIT], portrait, frames=True)

    def test_files_ui_script_and_frames(self):
        stdout, digests = self.files_ui()
        self.assertIn("PASS Files UI", stdout)
        self.assertEqual(len(digests), 25)
        self.check_frames(digests, FILES_FRAMES, "FILES")

    def test_files_qemu_script_targets(self):
        """files-qemu-t23 relative moves land on the harness's layout targets."""
        stdout, _ = self.files_ui()
        targets = dict((k, tuple(map(int, v.split(",")))) for k, v in re.findall(r"(\w+)=(-?\d+,-?\d+)", stdout.split("SCRIPT", 1)[1].splitlines()[0]))
        sys.path.insert(0, str(ROOT / "scripts/test"))
        try:
            runner = load("deskui_runner", ROOT / "scripts/test/run.py")
        finally:
            sys.path.remove(str(ROOT / "scripts/test"))
        suite = json.loads((ROOT / "tests/suites/f2-desktop.json").read_text())
        case = next(c for c in suite["cases"] if c["id"] == "files-qemu-t23")
        runner.Actions(case["actions"])
        self.assertEqual(case["profile"], "qemu-t23")
        self.assertEqual(case["checks"]["listing_contains"], ["CIUKI"])
        # 1024x768 (QEMU std VGA mode 0x144); the cursor starts at the centre.
        x, y, clicks, keys = 512, 384, [], []
        for batch in case["actions"][0]["batches"]:
            for event in batch["events"]:
                if event["type"] == "rel":
                    if event["data"]["axis"] == "x": x = min(max(x + event["data"]["value"], 0), 1023)
                    else: y = min(max(y + event["data"]["value"], 0), 767)
                elif event["type"] == "btn" and event["data"]["down"]: clicks.append((x, y))
                elif event["type"] == "key": keys.append((event["data"]["key"]["data"], event["data"]["down"]))
        # Ciuki button, Programs, then the Files row of the Programs submenu (tests/host/desktop/shell_menu.py).
        programs = sorted(set(re.findall(r'"/Programs/([A-Za-z0-9_.-]+)"', (ROOT / "scripts/build_image.py").read_text())) - {"desktop"})
        menu = load("deskui_shell_menu", ROOT / "tests/host/desktop/shell_menu.py")
        menu.check_launch(self, clicks, programs, "files")
        client = (4 + 3, 36 + 24)   # Files moves its frame to 4,36; content below the 24 px title
        expected = [targets[k] for k in ("view_title", "list_item", "places_system", "file_title", "new_folder")]
        self.assertEqual([(cx - client[0], cy - client[1]) for cx, cy in clicks[3:]], expected)
        self.assertEqual(keys[:2], [("f11", True), ("f11", False)])
        self.assertEqual(keys[-2:], [("a", True), ("a", False)])
        self.assertNotIn("a", [k for k, _ in keys[2:-2]])
        after = next(p for p in case["expected"]["predicates"] if p["where"].get("stage") == "after")
        self.assertEqual(after["fields"]["keys"], {"eq": 4})
        digests = [p for p in case["expected"]["predicates"] if p["where"].get("case") == "files-checkpoint"]
        self.assertEqual(sorted(p["where"]["checkpoint"] for p in digests), [1, 2, 3, 4, 5])
        self.assertGreater(sum(b["pause_ms"] for b in case["actions"][0]["batches"]) / 1000, 10)

    def test_icon_manifest(self):
        icons = load("deskui_icons", ROOT / "apps/desktop/convert_icons.py")
        record = icons.check()
        header = (ROOT / "apps/desktop/shell_icons.h").read_text()
        for name in icons.MEMBERS:
            size = icons.size_of(name)
            self.assertIn(f"desk_icon_{name}[{size * size}]", header)
            self.assertTrue(record["icons"][name]["source"].startswith(f"tango-icon-theme-0.8.90/{size}x{size}/"))


if __name__ == "__main__":
    unittest.main()
