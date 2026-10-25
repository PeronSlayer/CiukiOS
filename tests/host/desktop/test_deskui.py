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
    'deskui-open.xrgb': 'f61f579f436666299669297bff40d933716f316fc9a0e04c2acc2aeee214caae',
    'deskui-popup.xrgb': '0d67abb32369b6a755ed7f5247dea1389049a56297cef185087485cd8ec8035c',
    'deskui-progress.xrgb': 'af6ac5d3e8525d71376d9d0768b53a9d45c4b0403e0289a1d2aab3837e7c9b76',
    'deskui-view0.xrgb': '9269da40f4bd59ba2b6331b8382dcd432b1dd1089456a608fbeef7986f823c05',
    'deskui-view1.xrgb': '26cc77326b0cc117215cd2389e275852a86b5a4a34109881bb8bb99e5e05758b',
    'deskui-view2.xrgb': 'dc03906560ba1b1543e63a6d5959f7541d75651eecde32496f6c21eb0d2fc8d0',
}
# G15 help widget frames.
HELP_TOOLKIT_FRAMES = {
    'help-dialog-button.xrgb': 'a6cd36f668060afff27e9250c7a4e377218f18e36ecf56d63ce149afa6b18aab',
    'help-dialog-whats-this.xrgb': '8dcf17399af218f5a5a0aca0d848b3326aff9fce0539db6eb8dea032523a56a2',
    'help-tooltip.xrgb': '08b3246e79f06ba9f6dbcc6e8b27624291927698ce530a64e62c85d95b6f8b42',
    'help-whats-this.xrgb': 'aec0b87a3abead1bf4a49be6f8b4a5e08138d1386dfd1b151db65d96994c04ea',
}
# Fix A (2026-10-24): reviewed combined G8 menus, G24 regional text and integrated Files frames.
FILES_FRAMES = {
    'files-drop-target.xrgb': '9e7f4e9ed7defb992f5d31e75b99eafff69700b134240f9044411bb66f0ac6fe',
    'files-band-details.xrgb': '29250bb717a0dbcd69c59c8a5dd093478d7b579fad0e408e25d684b547004d95',
    'files-band-list.xrgb': 'aa56db61a2335e894fc082b3aed6959b70b0587ee103e207108a11afec810674',
    'files-band-icons.xrgb': '3b6a79757c6c2b9bf144eddaddd64755aa14ea87405ac2ea7d24f4536fe60ba6',
    'files-about.xrgb': 'd2be5ff3c2ea96f2d8e6c8cea07a86ef0103b0a68e4d659350ea0e3ad4b063c3',
    'files-programs-icons.xrgb': '524e5bf7a3262dfe1105f89a328f3b8c5469fd1763237c251e509b5fb4ddcb23',
    'files-confirm-delete.xrgb': 'ec7bae5c28c2be4bc1687032d852a67a988a7608943918d57727a566d474275a',
    'files-context-background.xrgb': 'c15ed6b4941a646eb0d72442c800dfc7cd9047c5d30267b213d048d7619d29bd',
    'files-context-item.xrgb': 'f735bb8f96bca642495ed7398118748fe4e0d530d586158895f8a22fe0c226f9',
    'files-copy-progress.xrgb': 'c12141f698e7383fa905955f930f586aa2f0303f5a136c3b4073e51228384a5a',
    'files-cp1-system.xrgb': 'a6f44a5cb0ab43b3588c3fc950dacda1674e639eb0265fabd95195d94b5e0752',
    'files-cp2-new-folder.xrgb': '951c57be3937ec3a271d6bb5247201ef31cb651b9bc62e5e6589a6d4f5c2abc5',
    'files-cp3-renamed.xrgb': '5998d04289e24d052c0a60a0f45ac7330412c7b32d93f9d5b0b716c2539548a9',
    'files-cp4-copied.xrgb': '62d9185bb769b21eca79bed9462b62f542f4e919da295936590655da90266a2b',
    'files-cp5-deleted.xrgb': 'ed659d7058677f71b0aa8a44463fc60ea6f2889f6afdf21ada6978894ec2a27a',
    'files-help.xrgb': 'c21c60de2dece820a7285593923a37e7c116d39377a2a53b854697cc9f485f04',
    'files-menu-file.xrgb': '84ab26d3e014f4d1968408913b675c6882731f82fb030740e052384227c74794',
    'files-menu-view.xrgb': '70f8fde96b769013f7a86198d108508c4c5eec10e0ff7469208a0954ad0c072d',
    'files-multi-select.xrgb': '69ed87d4a59a5fa07a32e6dbd9d91722f41a2a6a59f6cf38686d77a19aa3ff5d',
    'files-properties.xrgb': '3a164995eef139ff1967477c2b9f5aaed698a85ad2dc9317bb7ae8bfbb2e15f8',
    'files-rename-error.xrgb': '6cdd24b8827fa7ca852ed8262d17a52a6c0241ebfa84673518d34e3d9bc6311e',
    'files-replace-prompt.xrgb': 'cec02d03e9d335d0a30d388a723b1dd96bca646d4a1c9cfb425c5653eea76819',
    'files-computer.xrgb': 'b1ae97064dec9bc510c53263f622d91e22239a4fc6c0f031744611ddd8c7a916',
    'files-c-drive.xrgb': '4d1cea3e20a476875979d6a6f5bd3bb1e1b8cbac70dc12ab57cb67e357fff174',
    'files-root-details.xrgb': 'a805bd6fd7b4574c2b87c4cb46cd2bb4f4e59616f81e043127b3cd68d413be25',
    'files-sort-size-desc.xrgb': 'f1099a836f08f9d8ac97b566077a1a4cce53f46ae1e07a4c9ba3e83895c6c09e',
    'files-system-details.xrgb': '0e5c7f15200e8ad92bf69a53531890d4a64b528138f477e29a94feb84a63050b',
    'files-system-icons.xrgb': '07650519226969dc8248f7f647b8315d9e941297039cf231ee025e77adc7dc22',
    'files-tree.xrgb': '9b15462e628320282b7d416c6ef7b8d46e751c580d1e1895cfb76be75c13c898',
}

# ui-look (2026-10-12): the new theme engine, fonts and controls changed every
# rendered frame. Entries set to PIN-FROM-FIRST-RUN print their new digest and
# are re-pinned by the lead after a visual review; pinned entries still compare.
PIN = "PIN-FROM-FIRST-RUN"

# Fix A (2026-10-24): reviewed regional text and combined property-sheet frames.
# G6: reviewed property sheets, drive pie/counting, Version and Attributes.
G6_FRAMES = {
    "g6-attributes.xrgb": "36c4b31cf29863de56400ecf085ed32903fd69f2cfc9eb64683fbdf24568fd40",
    "g6-counting.xrgb": "ee3d760c0609ee2cbeaff3cd8845687e3a61c7f216256a1f0e82901fac2f97d2",
    "g6-drive.xrgb": "44e70c1c971f2e8cc9caf125425b222b54eb8efc5c6705cc4cd5ee29ce915923",
    "g6-file.xrgb": "78f6614dd348b74028bed3ae0c3d247d27d6dc762a2a61b41683dd302538e2f7",
    "g6-folder.xrgb": "bfb0324a30b2e80c88d939ea5455cda68e60b14c71adffbd06223f9c9198b9f5",
    "g6-tools.xrgb": "9015ae71bb47c53f390ba462a3717303d3c1ee9895ec7b72f7d9308402419539",
    "g6-version.xrgb": "4310c84be5d30c6671ff51bf763a98306408c7eb928fc3918e288334ff83bdcd"
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
            "apps/desktop/protocol.c", "tests/host/desktop/resize_transport.c", "apps/libdeskui/clipboard.c", "apps/libdeskui/clipstore.c", "tests/host/desktop/resize_client.c"],
            cflags=("-include", str(ROOT / "tests/host/desktop/replay_base.h"), "-pthread"))

    def test_toolkit_widgets_and_frames(self):
        _, digests = self.run_harness("deskui", ["tests/host/desktop/deskui_test.c", *TOOLKIT,
            "apps/desktop/protocol.c", "tests/host/desktop/resize_transport.c",
            "apps/libdeskui/clipboard.c", "apps/libdeskui/clipstore.c", "tests/host/desktop/resize_client.c", "tests/host/desktop/resize_client_test.c"], frames=True)
        self.assertEqual(len(digests), 9)
        self.check_frames(digests, TOOLKIT_FRAMES, "TOOLKIT")

    def test_help_toolkit(self):
        """G15: tooltips, F1 WM_HELP, What's This?, the dialog ? button and the LAUNCH of CiukiOS Help."""
        stdout, digests = self.run_harness("help-toolkit", ["tests/host/desktop/help_toolkit_test.c", *TOOLKIT,
            "apps/desktop/protocol.c", "tests/host/desktop/resize_transport.c", "apps/libdeskui/clipboard.c",
            "apps/libdeskui/clipstore.c", "tests/host/desktop/resize_client.c"],
            frames=True, cflags=("-include", str(ROOT / "tests/host/desktop/replay_base.h"), "-pthread"))
        self.assertIn("PASS G15 libdeskui help", stdout)
        self.check_frames(digests, HELP_TOOLKIT_FRAMES, "HELP_TOOLKIT")

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
            "apps/desktop/protocol.c", "tests/host/desktop/resize_transport.c", "apps/libdeskui/clipboard.c", "apps/libdeskui/clipstore.c", "tests/host/desktop/resize_client.c"])
        self.assertIn("PASS Files worker", stdout)

    def test_files_transfer_worker(self):
        stdout, digests = self.run_harness("files-transfer", ["tests/host/desktop/files_transfer_test.c",
            "apps/files/model.c", "apps/files/transfer.c", *TOOLKIT,
            "apps/desktop/protocol.c", "tests/host/desktop/resize_transport.c", "apps/libdeskui/clipboard.c", "apps/libdeskui/clipstore.c", "tests/host/desktop/resize_client.c",
            *(f"apps/desktop/{n}.c" for n in ("client", "input", "shell", "autoplay", "autoplay_content",
                "settings", "power_policy", "settings_io", "wallpaper", "keymap", "folder"))], frames=True,
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
            "apps/desktop/protocol.c", "tests/host/desktop/resize_transport.c", "apps/libdeskui/clipboard.c", "apps/libdeskui/clipstore.c", "tests/host/desktop/resize_client.c"], directory)
        self.assertIn("PASS Files idle", stdout)

    def test_find_files(self):
        stdout, _ = self.run_harness("files-search", ["tests/host/desktop/files_search_test.c",
            "tests/host/desktop/fake_vfs.c", "apps/files/model.c", "apps/files/transfer.c", *TOOLKIT,
            "apps/desktop/protocol.c", "tests/host/desktop/resize_transport.c", "apps/libdeskui/clipboard.c", "apps/libdeskui/clipstore.c", "tests/host/desktop/resize_client.c"])
        self.assertIn("PASS G7 model", stdout)
        self.assertIn("PASS G7 UI", stdout)

    def test_files_version(self):
        stdout, _ = self.run_harness("files-version", ["tests/host/desktop/files_version_test.c", "apps/files/vfs.c", "apps/files/model.c"])
        self.assertIn("PASS G6 PE Version", stdout)

    def test_files_properties(self):
        stdout, digests = self.run_harness("g6-properties", ["tests/host/desktop/files_properties_test.c",
            "tests/host/desktop/fake_vfs.c", "apps/files/app.c", "apps/files/model.c",
            "apps/files/transfer.c", *TOOLKIT], frames=True)
        self.assertIn("PASS G6 Properties", stdout)
        self.check_frames(digests, G6_FRAMES, "G6")

    def test_files_cleanup(self):
        stdout, _ = self.run_harness("files-cleanup", ["tests/host/desktop/files_cleanup_test.c",
            "tests/host/desktop/fake_vfs.c", "apps/files/model.c", "apps/files/vfs.c"])
        self.assertIn("PASS G29 Disk Cleanup model", stdout)

    def test_files_model(self):
        stdout, _ = self.run_harness("files-model", ["tests/host/desktop/files_model_test.c", "tests/host/desktop/fake_vfs.c",
                                                     "apps/files/model.c"])
        self.assertIn("PASS Files model", stdout)

    def test_partition_tool(self):
        stdout, _ = self.run_harness("partition-tool", ["tests/host/partition_tool_test.c",
            "sdk/libdisk/disk.c", "sdk/libdisk/format.c", "sdk/libdisk/sha256.c"],
            lambda folder: folder / "disk.img", cflags=("-I", str(ROOT / "sdk/libdisk/include")))
        self.assertIn("PASS FDISK", stdout)

    def test_backup(self):
        stdout, _ = self.run_harness("backup", ["tests/host/desktop/backup_test.c",
            "apps/files/backup.c", "apps/files/vfs.c", "apps/files/model.c"],
            lambda folder: folder, cflags=("-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections"))
        self.assertIn("PASS Backup", stdout)

    def test_files_disk_ui(self):
        stdout, _ = self.run_harness("files-disk-ui", ["tests/host/desktop/files_disk_ui_test.c",
            "tests/host/desktop/fake_vfs.c", "apps/files/app.c", "apps/files/model.c",
            "apps/files/transfer.c", *TOOLKIT], frames=True)
        self.assertIn("PASS G29 disk dialogs", stdout)

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
