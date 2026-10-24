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
import importlib.util as _review_util  # optional PNG review copies (frame_review.py, any import mode)
_review_spec = _review_util.spec_from_file_location("frame_review", __file__.rsplit("/", 1)[0] + "/frame_review.py")
_review = _review_util.module_from_spec(_review_spec); _review_spec.loader.exec_module(_review)
capture = _review.capture

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
TOOLKIT = ["apps/libdeskui/draw.c", "apps/libdeskui/widgets.c", "apps/libdeskui/list.c",
           "apps/libdeskui/menu.c", "apps/libdeskui/dialog.c", "apps/libdeskui/controls.c", "apps/libdeskui/commdlg.c", "apps/libdeskui/textarea.c", "apps/desktop/compositor.c", "apps/desktop/theme.c",
           "apps/libdeskui/icons.c", "apps/libdeskui/icon_map.c"]

# Reviewed frames (2026-10-11): an unpinned run prints the new digests. Convert
# the harness frames (XRGB8888, 400x300 toolkit, 760x520 Files) to PNG and
# re-pin only after a visual review.
# wt/ui-integ (2026-10-12): Tango store icons (wt/icons) and the ui-look theme, fonts and controls together;
# F2-64 recorded affected transient frames; remaining frames await lead review.
# 2026-10-15 (main after f2-63 + f2-64): pinned frames re-recorded on the combined tree (FreeType text in the f2-64 dialogs), reviewed against the 9a05ab9f renders.
# G16 (2026-10-24): deskui-open and the four Places frames re-pinned for the toolkit Open/Save As (New Folder, List/Details, Files of type).
# F4-30 (2026-10-18): only deskui-open changes for the shared Places pane;
# the four dialog-places-goldens.json frames cover mounted media and Save.
TOOLKIT_FRAMES = {
    'deskui-input.xrgb': '72e8a91f02bdb8b2b5ef0bafe23a9dc1332152aef2280790299cfb5951c16da6',
    'deskui-menu.xrgb': 'a9ffa04ca2ea5565a6a7ca8a99b32dc3c3b3d7e0edba6646be7d7ee31946cea9',
    'deskui-message.xrgb': '23bca4045904f0bdc3311b8578b025328a1baa30b2b202b2f872ec8242f36f41',
    'deskui-open.xrgb': 'dfc6242164e8cc9c9dc4405d3b5f71bf8ffbf83d919eda20f20bd7315889ce15',
    'deskui-popup.xrgb': '0d67abb32369b6a755ed7f5247dea1389049a56297cef185087485cd8ec8035c',
    'deskui-progress.xrgb': 'af6ac5d3e8525d71376d9d0768b53a9d45c4b0403e0289a1d2aab3837e7c9b76',
    'deskui-view0.xrgb': '9269da40f4bd59ba2b6331b8382dcd432b1dd1089456a608fbeef7986f823c05',
    'deskui-view1.xrgb': '26cc77326b0cc117215cd2389e275852a86b5a4a34109881bb8bb99e5e05758b',
    'deskui-view2.xrgb': 'dc03906560ba1b1543e63a6d5959f7541d75651eecde32496f6c21eb0d2fc8d0',
}
FILES_FRAMES = {
    'files-drop-target.xrgb': 'fa3e0f26a2db9c30dd775cd57bdf5925669e1d72872289e5c9e0dff9a778a977',
    'files-band-details.xrgb': '4080b44e53eef99b7c25e4662588fe20350324862aa8a6900b9a72860ce94ece',
    'files-band-list.xrgb': '3e462501fda2ce487945b057fa78a88e85ac92ad40650dde027e23fba3c6368e',
    'files-band-icons.xrgb': 'aa131944918d592a600592c10d13d5fa0c05aaf9e4972c3d8dbc99400d621276',
    'files-about.xrgb': '1a93a771a447517d1872b950e12418dac641d4f1f0c7c21841a1e5115bdc68f6',
    'files-programs-icons.xrgb': 'ba85b84b556aeaeba0165ed54fcbc5762f2dc8af5a911922e6652400fbbbad1c',
    'files-confirm-delete.xrgb': '68240aed5186a408d746eeb88692d3bf5121b5444a40d4d0b4fbcc02641adf37',
    'files-context-background.xrgb': '36025408efb1545e3d5e9b4349f821430f5a1f999d8c39458f6bc1456bd7bdac',
    'files-context-item.xrgb': '80ccd71c58c45eeea0471a01335970bc2ffc05f9b43ca2dfb971f5654572307f',
    'files-copy-progress.xrgb': '2542fade6958be2ef613a5567c27b6b21358541a6c98c741bdbb8d0e738ffa3e',
    'files-cp1-system.xrgb': '6bf866276d2d492a36c76dd37952f7cdf8eafc69a9daba60f6ee749c15fdf379',
    'files-cp2-new-folder.xrgb': 'a0206dad2edeaf99be51337eb51186e3ce8b20fcf6c22ec8fbc59996b92ed6d6',
    'files-cp3-renamed.xrgb': 'db328fb767d988aab0783feb17115dd24b45c82fb230d4a68b88321b96d77f9c',
    'files-cp4-copied.xrgb': 'eac0df61e307c53e214aa7e34c1c33726ae7570d2c19653ab0c74f2e8909147a',
    'files-cp5-deleted.xrgb': '75122973988f67620bd112bc3b89efbd39e16acf1cc8e7e6013dd49156d1beb6',
    'files-help.xrgb': '4268a86667b2c80bddb6305cbc95e97a62b49d3d6295f83142cbf5301cf669b7',
    'files-menu-file.xrgb': 'fa3bbac3d311703fbbc92762a3fe6a5171b7ae731e99c8c7176a1131a170fe89',
    'files-menu-view.xrgb': '18cb22210b89067d6a9195eddcc261b5c8336a1ea5878482261a433663f5d078',
    'files-multi-select.xrgb': 'ddbec86351caf942f8658ea586bc859121e5c2cdd34af1eaa250920f2f9f211d',
    'files-properties.xrgb': '8308013ed6c5cf7372d0d415e9f32ac9de578f9d8970c65ed75f4c24de7f7798',
    'files-rename-error.xrgb': '2a28adc4ea67265026e8fb2c779dfecc04204e9a167e9811754f50188bb3c52f',
    'files-replace-prompt.xrgb': '8e66a2b9a7c423e2678dfffd1f12e86083f59df196243b3dfde95cb466dac85e',
    'files-computer.xrgb': '506bb594021dc02a639b7551b5d076d67610323859d387e55a583a2d1e2f8e14',
    'files-c-drive.xrgb': 'f7f83a31eb52f51ae109a81a4e50dc60a27c238521451adc77e5e038c2dc2b8d',
    'files-root-details.xrgb': '113608285b83719291c9f25f492f077bdb9567e3a379424927c51105a9ed4eb7',
    'files-sort-size-desc.xrgb': '98a9556f0422749d1e8aa9a483df84ef328b86db81cb1e137c2d85c5aa31eb48',
    'files-system-details.xrgb': '3505f30e9aa8ad5859330877045665d135f2a7c8fd15725e3b9ed7daf3082121',
    'files-system-icons.xrgb': '70cfa16a5528554805141a9d71d2658ebdbad4cb2ee9545a2fd1e3f70489a64f',
    'files-tree.xrgb': '625a8187d886448fe7ab0356df520e6449805b1d65526f9b546182e84e618c5c',
}

# ui-look (2026-10-12): the new theme engine, fonts and controls changed every
# rendered frame. Entries set to PIN-FROM-FIRST-RUN print their new digest and
# are re-pinned by the lead after a visual review; pinned entries still compare.
PIN = "PIN-FROM-FIRST-RUN"

# G6: reviewed property sheets, drive pie/counting, Version and Attributes.
G6_FRAMES = {
    "g6-attributes.xrgb": "4ec32dfe992e81b6410a27d69633378555a5ba2ace43694a2f3b01d43df7c3a5",
    "g6-counting.xrgb": "42b3305b9c527cd7f17357243d80e9a0b41881ddcf48236cdd018197e02054e4",
    "g6-drive.xrgb": "498d8688f28d739299d4b71bc2567cc7ff1e98784a8172e9c07e3cd5417e2648",
    "g6-file.xrgb": "5bfaba21d7f7629d31aa78b3edfbc71d472e207f632032aca0596d060cd2c165",
    "g6-folder.xrgb": "c7cde320492cac790ffb8e74c6ece5f449c6e88659de20cf4e58f3173d3e58d1",
    "g6-tools.xrgb": "eee7fead585b45aa113857a8f7f27184b2e9b50a1759813e99be6fa420159b2f",
    "g6-version.xrgb": "96976f0bb3f2a2100d69c50ed0f6819ce2a3cc7043ac7d28d58327e1751c6526"
}


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

    def run_harness(self, name, sources, *args, frames=False, cflags=()):
        with tempfile.TemporaryDirectory(prefix=name + "-", dir=self.parent) as temp:
            folder = Path(temp)
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0")
            binary = folder / name
            include = folder / "include/ciuki"
            include.mkdir(parents=True)
            for header in ("channel.h", "surface.h"):
                shutil.copy2(ROOT / "sdk/sysroot-overlay/include/ciuki" / header, include / header)
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-D_POSIX_C_SOURCE=200809L",
                            *cflags, "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                            "-I", str(ROOT / "apps/libdeskui"), "-I", str(ROOT / "apps/files"), "-I", str(ROOT / "src/kernel/include"),
                            "-I", str(folder / "include"),
                            *ICONS.defines(), *(str(ROOT / s) for s in sources), "-o", str(binary)], check=True, env=env)
            output = folder / "frames"
            output.mkdir()
            extra = [str(a(folder)) if callable(a) else str(a) for a in args]
            result = subprocess.run([str(binary), *([str(output)] if frames else []), *extra],
                                    env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            stdout = result.stdout + (result.stderr if name == "files-transfer" else "")
            print(stdout.strip())
            if name == "files-ui":
                from PIL import Image
                review = ROOT / "build/f2-36"
                review.mkdir(parents=True, exist_ok=True)
                for stem in ("computer", "c-drive"):
                    raw = (output / f"files-{stem}.xrgb").read_bytes()
                    Image.frombytes("RGB", (760, 520), raw, "raw", "BGRX").save(review / f"files-{stem}.png")
            digests = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(output.glob("*.xrgb"))}
            for p in output.glob("*.xrgb"):
                width, height = (760, 520) if name in ("files-ui", "files-transfer") else (400, 300)
                if p.with_suffix('.size').exists():
                    width, height = map(int, p.with_suffix('.size').read_text().split())
                capture(p.stem, p.read_bytes(), width, height)
            return stdout, digests

    def check_frames(self, actual, pinned, label):
        if not pinned:
            print(label + "_FRAMES", json.dumps(actual, indent=1, sort_keys=True))
            self.fail(label + " frames are not pinned")
        assert_pins(self, actual, pinned, label + "_FRAMES")

    def test_update_regions_and_scroll(self):
        self.run_harness("update-regions", ["tests/host/desktop/update_regions_test.c", *TOOLKIT,
            "apps/desktop/protocol.c", "tests/host/desktop/resize_transport.c", "tests/host/desktop/resize_client.c"],
            cflags=("-include", str(ROOT / "tests/host/desktop/replay_base.h"), "-pthread"))

    def test_toolkit_widgets_and_frames(self):
        _, digests = self.run_harness("deskui", ["tests/host/desktop/deskui_test.c", *TOOLKIT,
            "apps/desktop/protocol.c", "tests/host/desktop/resize_transport.c",
            "tests/host/desktop/resize_client.c", "tests/host/desktop/resize_client_test.c"], frames=True)
        self.assertEqual(len(digests), 9)
        self.check_frames(digests, TOOLKIT_FRAMES, "TOOLKIT")

    def test_dialog_places(self):
        sources = ['tests/host/desktop/dialog_places_test.c',
                   *(s for s in TOOLKIT if s != 'apps/libdeskui/dialog.c')]
        _, digests = self.run_harness('places', sources, frames=True)
        expected = json.loads((ROOT/'tests/host/desktop/dialog-places-goldens.json').read_text())
        self.check_frames(digests, expected, 'PLACES')

    def test_common_controls_and_dialogs(self):
        """G16: combo box, trackbar, up-down, tooltips, edit undo, Open/Save As, Font, Color, Browse for Folder, Find and Replace."""
        stdout, digests = self.run_harness("common-dialogs", ["tests/host/desktop/common_dialogs_test.c", *TOOLKIT], frames=True)
        self.assertIn("PASS G16 common controls and dialogs", stdout)
        expected = json.loads((ROOT / "tests/host/desktop/common-dialogs-goldens.json").read_text())
        self.check_frames(digests, expected, "G16")

    def test_files_slow_listing_responsive(self):
        stdout, _ = self.run_harness("files-async", ["tests/host/desktop/files_async_test.c",
            "apps/files/app.c", "apps/files/model.c", "apps/files/transfer.c", *TOOLKIT,
            "apps/desktop/protocol.c", "tests/host/desktop/resize_transport.c", "tests/host/desktop/resize_client.c"])
        self.assertIn("PASS Files worker", stdout)

    def test_files_transfer_worker(self):
        stdout, digests = self.run_harness("files-transfer", ["tests/host/desktop/files_transfer_test.c",
            "apps/files/model.c", "apps/files/transfer.c", *TOOLKIT,
            "apps/desktop/protocol.c", "tests/host/desktop/resize_transport.c", "tests/host/desktop/resize_client.c",
            *(f"apps/desktop/{n}.c" for n in ("client", "input", "shell", "autoplay", "autoplay_content",
                "settings", "settings_io", "wallpaper", "keymap", "folder"))], frames=True,
            cflags=("-include", str(ROOT / "tests/host/desktop/replay_base.h")))
        self.assertIn("PASS Files transfer worker", stdout)
        self.assertIn("[files] job=copy progress items=0 bytes=0", stdout)
        self.assertIn("[files] job=copy cancel", stdout)
        self.assertRegex(stdout, r"\[files\] job=copy done ms=\d+ bytes=0 rate_kbs=0 errors=1")
        expected = json.loads((ROOT / "tests/host/desktop/transfers-goldens.json").read_text())
        self.check_frames(digests, expected, "TRANSFERS")

    def test_files_external_writer_idle(self):
        def directory(folder):
            path=folder / "writer"; path.mkdir(); return path
        stdout, _ = self.run_harness("files-idle", ["tests/host/desktop/files_idle_test.c",
            "apps/files/model.c", "apps/files/transfer.c", "apps/files/vfs.c", *TOOLKIT,
            "apps/desktop/protocol.c", "tests/host/desktop/resize_transport.c", "tests/host/desktop/resize_client.c"], directory)
        self.assertIn("PASS Files idle", stdout)

    def test_files_version(self):
        stdout, _ = self.run_harness("files-version", ["tests/host/desktop/files_version_test.c", "apps/files/vfs.c", "apps/files/model.c"])
        self.assertIn("PASS G6 PE Version", stdout)

    def test_files_properties(self):
        stdout, digests = self.run_harness("g6-properties", ["tests/host/desktop/files_properties_test.c",
            "tests/host/desktop/fake_vfs.c", "apps/files/app.c", "apps/files/model.c",
            "apps/files/transfer.c", *TOOLKIT], frames=True)
        self.assertIn("PASS G6 Properties", stdout)
        self.check_frames(digests, G6_FRAMES, "G6")

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
        self.assertEqual(len(digests), 29)
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
