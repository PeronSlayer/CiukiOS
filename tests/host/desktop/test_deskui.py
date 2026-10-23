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
           "apps/libdeskui/menu.c", "apps/libdeskui/dialog.c", "apps/libdeskui/textarea.c", "apps/desktop/compositor.c", "apps/desktop/theme.c",
           "apps/libdeskui/icons.c", "apps/libdeskui/icon_map.c"]

# Reviewed frames (2026-10-11): an unpinned run prints the new digests. Convert
# the harness frames (XRGB8888, 400x300 toolkit, 760x520 Files) to PNG and
# re-pin only after a visual review.
# wt/ui-integ (2026-10-12): Tango store icons (wt/icons) and the ui-look theme, fonts and controls together;
# F2-64 recorded affected transient frames; remaining frames await lead review.
# 2026-10-15 (main after f2-63 + f2-64): pinned frames re-recorded on the combined tree (FreeType text in the f2-64 dialogs), reviewed against the 9a05ab9f renders.
# F4-30 (2026-10-18): only deskui-open changes for the shared Places pane;
# the four dialog-places-goldens.json frames cover mounted media and Save.
TOOLKIT_FRAMES = {
    'deskui-input.xrgb': '72e8a91f02bdb8b2b5ef0bafe23a9dc1332152aef2280790299cfb5951c16da6',
    'deskui-menu.xrgb': 'a9ffa04ca2ea5565a6a7ca8a99b32dc3c3b3d7e0edba6646be7d7ee31946cea9',
    'deskui-message.xrgb': '23bca4045904f0bdc3311b8578b025328a1baa30b2b202b2f872ec8242f36f41',
    'deskui-open.xrgb': 'fc912b457c774d912452593b94c8dbea73c0f0222c7fb4de97f441a86f06552e',
    'deskui-popup.xrgb': '0d67abb32369b6a755ed7f5247dea1389049a56297cef185087485cd8ec8035c',
    'deskui-progress.xrgb': 'af6ac5d3e8525d71376d9d0768b53a9d45c4b0403e0289a1d2aab3837e7c9b76',
    'deskui-view0.xrgb': '9269da40f4bd59ba2b6331b8382dcd432b1dd1089456a608fbeef7986f823c05',
    'deskui-view1.xrgb': '26cc77326b0cc117215cd2389e275852a86b5a4a34109881bb8bb99e5e05758b',
    'deskui-view2.xrgb': 'dc03906560ba1b1543e63a6d5959f7541d75651eecde32496f6c21eb0d2fc8d0',
}
# G7: baseline/current pixel comparison confines changes to menu rows 0..21
# (Tools and shifted Help); Computer and transfer frames visually reviewed.
FILES_FRAMES = {
    'files-drop-target.xrgb': 'a7d38c3d24a14a9239c5bc41ead1a3e986f4e236d69676343574808fe689cdf5',
    'files-band-details.xrgb': 'b3381c753edeb2606ddd8c56194f54d0883418073a0ba96cfbed6d0fffc726bd',
    'files-band-list.xrgb': 'aa56db61a2335e894fc082b3aed6959b70b0587ee103e207108a11afec810674',
    'files-band-icons.xrgb': '3b6a79757c6c2b9bf144eddaddd64755aa14ea87405ac2ea7d24f4536fe60ba6',
    'files-about.xrgb': '7919bbabc53c41facdc45e040098a451905b11ce6d96774fe6cb09cbec161294',
    'files-programs-icons.xrgb': 'ed6c39b2d7cac1194eb318f74fe5854957f5bf84dd8c2664d4c280c8aa091431',
    'files-confirm-delete.xrgb': 'ec7bae5c28c2be4bc1687032d852a67a988a7608943918d57727a566d474275a',
    'files-context-background.xrgb': 'e9950374afdb9c00ad17687921b0fd715ae23d8a974aa7da404d25adc97d167e',
    'files-context-item.xrgb': '7a2131f1a793078cd3ec7d9ae1123eda203ab3e01bab4b49a15188131d0b8da9',
    'files-copy-progress.xrgb': 'c12141f698e7383fa905955f930f586aa2f0303f5a136c3b4073e51228384a5a',
    'files-cp1-system.xrgb': 'a6f44a5cb0ab43b3588c3fc950dacda1674e639eb0265fabd95195d94b5e0752',
    'files-cp2-new-folder.xrgb': '951c57be3937ec3a271d6bb5247201ef31cb651b9bc62e5e6589a6d4f5c2abc5',
    'files-cp3-renamed.xrgb': '4e166c757661aa57efbbd84188688fdf7a13a87db08564ab166b8bf3abb0ea76',
    'files-cp4-copied.xrgb': '62d9185bb769b21eca79bed9462b62f542f4e919da295936590655da90266a2b',
    'files-cp5-deleted.xrgb': 'ed659d7058677f71b0aa8a44463fc60ea6f2889f6afdf21ada6978894ec2a27a',
    'files-help.xrgb': '47b03017520501f4da307630ab107ff1e25634e324686fd1c459db5d3896aafe',
    'files-menu-file.xrgb': '12b98a7539389a9623f7a4a84fcff184790ab9977b8f71d3858b9dd98e0bd26b',
    'files-menu-view.xrgb': 'e9e490afa2382da569a4d41bea2caecd3de27a89ac6a86f881b28c339ebdecfc',
    'files-multi-select.xrgb': '2ea91b736404a69c10a1e1afb2ec447cd80fb2df22fbcf4bbf0d7a5ad9f71099',
    'files-properties.xrgb': 'b0839edf5ef4fc396742e07c6622de4b28aa5c9eb96da2d65735ed51c996b02b',
    'files-rename-error.xrgb': '6cdd24b8827fa7ca852ed8262d17a52a6c0241ebfa84673518d34e3d9bc6311e',
    'files-replace-prompt.xrgb': '88b8607ff074335d0ac07debfc56fbce46e93fd1209ae60881b625ffd7ee06ec',
    'files-computer.xrgb': 'b9d0a700cd185eb710c61c52848f78f21852ca285a74d9bf0601159403c87529',
    'files-c-drive.xrgb': '12e4bff88d436c0bac238b294a403436f407601404b9b2bc98b6fc30422b78fb',
    'files-root-details.xrgb': '0dc36aae9c2b51d2114e41f8e841495887a2c94abda069c358591f10e0c5a9e8',
    'files-sort-size-desc.xrgb': '412914fa5cdff431f858269d460ab694dc06e15d6431a1292bcd6530a59b7640',
    'files-system-details.xrgb': 'aaad7e93c59bb4ba3d95245ccb35f97adff40f938635a3e615d17158d9dd53ea',
    'files-system-icons.xrgb': '07650519226969dc8248f7f647b8315d9e941297039cf231ee025e77adc7dc22',
    'files-tree.xrgb': '9b15462e628320282b7d416c6ef7b8d46e751c580d1e1895cfb76be75c13c898',
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

    def test_find_files(self):
        stdout, _ = self.run_harness("files-search", ["tests/host/desktop/files_search_test.c",
            "tests/host/desktop/fake_vfs.c", "apps/files/model.c", "apps/files/transfer.c", *TOOLKIT,
            "apps/desktop/protocol.c", "tests/host/desktop/resize_transport.c", "tests/host/desktop/resize_client.c"])
        self.assertIn("PASS G7 model", stdout)
        self.assertIn("PASS G7 UI", stdout)

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
