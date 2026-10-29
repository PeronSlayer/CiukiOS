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
           "apps/libdeskui/icons.c", "apps/libdeskui/icon_map.c", "apps/libdeskui/iconx.c", "apps/libdeskui/fileicon.c"]

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
# fx-files-nav: Undo toolbar, Back/Forward arrows, address combo and Arrange Icons menu.
# Host frames inspected; the lead reviews these intentional changes at integration.
FILES_FRAMES = {'files-about.xrgb': '800b18f40ad8ed47ae8a908a2e753472d032f88f7f84aac7b37cfb17d74ffce5',
 'files-band-details.xrgb': 'b01c11dc8f7e2767be34a3722b8dfc21d7fba4ce8a7cff509584421f2183f183',
 'files-band-icons.xrgb': '519b24c0041ef52919de4e43dbdd59a379bed57dc0d6585ac9f1a9b89383131a',
 'files-band-list.xrgb': '8978bbe16c6831cb7349f0dfbaaec860a1dab10a042446ba2caf9aea1253cb56',
 'files-c-drive.xrgb': 'fcc338ecb7a0d333d67bd2125c3b509c2e40cd8deafe9406e9b8199a78ce0794',
 'files-computer.xrgb': '86ba274a1b231ddade925ca28395c0c504660ec6d0777918f78e2a35f301eac3',
 'files-confirm-delete.xrgb': '044157eb3315d069753f03d790a96e7a85a9751239c1aee7a978e23e9f32d03f',
 'files-context-background.xrgb': '26327452356e3e4c94bc85c649245ee1c8ca42b9c6dd1c9ac0ae366c0f0dd218',
 'files-context-item.xrgb': '6b046025a8f56afb9ccf6be5408c0f779b6bcc37e8ff2af15765e0c8772ba056',
 'files-copy-progress.xrgb': '8142be483a0821a2f2947d655cda4aa1863a2c84ab7c362e2fefb486302b45fb',
 'files-cp1-system.xrgb': '7591c577be3a291263ffd1a0f225878b7b56587c6d37961b44506cc114d677f7',
 'files-cp2-new-folder.xrgb': '76ac95b80bb4dccef26001d3f9664ce3061a2b450b8b99da6b6a8a3da7a182b2',
 'files-cp3-renamed.xrgb': '5f0e2b69eab59dccee14d7543f4d606000c636dfdb810db9a9e2c4cde377da96',
 'files-cp4-copied.xrgb': 'eb84f5dbb116a5aa63d1ca8cf45e224cde7266be9157800f2d48fe34669653c1',
 'files-cp5-deleted.xrgb': 'a1e1366e930bdd0cb15c85ddf325f9aa41669eb3dd0958737241077a0b60afbc',
 'files-drop-target.xrgb': 'a93867073df0ea64728f905c9e631cee13e86aeedc5aeaf30e92a0c7b5e8c479',
 'files-empty-cd-computer.xrgb': 'f4b22824e56f8f291825f5bfe3f132df6a35d994db101a26f8de760aa74383c5',
 'files-empty-cd-insert.xrgb': 'd6326f5f435dddd76b334373e90633aadc86d83bfa69907e0205126468f95d60',
 'files-help.xrgb': '7bc8962fa02a2d72ca713fbfad11c71fe84fad46ff9e4c631016bf8bc2800692',
 'files-menu-file.xrgb': '5f27384dd725fa1cef90ba2ae0bda7cb678bd2f46e12af753a1828a915accb7b',
 'files-menu-view.xrgb': '5ca28fdb59b3524103db289d708be07c1e82e4a3aeb86f5d348642a2fff59ddd',
 'files-multi-select.xrgb': '9fc0d3182b6ec0c94a2c97cff3e5a28c6e28c22271fc78d479e1ac920abfa06d',
 'files-programs-icons.xrgb': '50bb38083cef65c335a271d47be8674bf04124f4eb52d58d478a394644034169',
 'files-properties.xrgb': '768898a8b4b6c7e54419c4e68c4dd01f90c4bf036159376c2a34cd683d31de34',
 'files-rename-error.xrgb': '3e34a7a84cd4f8d9be202f63895aeaa499ba3470972bcaf3c629900fdd0dbe1a',
 'files-replace-prompt.xrgb': '7071582f8b31d00833993fec6aab603142d5a5b3ce93f9a33801b6ed53ef2526',
 'files-root-details.xrgb': 'a6c1674dfaa31cfc6f13cd5c8e65de8f08f384ddf4630c39a18e71731586c62b',
 'files-sort-size-desc.xrgb': 'c18768c1c11c3dd3fbe197f3e76344324b6b467818afe28c1f35c92c8d9c102c',
 'files-system-details.xrgb': 'a91c9274f1982e1879ddef254f28eca52129579327998d5410bc9b50de410f41',
 'files-system-icons.xrgb': '38c6146562d19bc50031b4b8cb3e13792c1c5255e6b12e3f2a0431bf0f1f708a',
 'files-tree.xrgb': '3784080ef4f387a7111edae3e61dd6dae027103718180f254e2257c50bb2bc86'}

# ui-look (2026-10-12): the new theme engine, fonts and controls changed every
# rendered frame. Entries set to PIN-FROM-FIRST-RUN print their new digest and
# are re-pinned by the lead after a visual review; pinned entries still compare.
PIN = "PIN-FROM-FIRST-RUN"

# fx-files-nav: same property sheets over the updated Files toolbar/address band.
# G6: reviewed property sheets, drive pie/counting, Version and Attributes;
# fx-dosfull: a DOS program's Screen page (Usage).
G6_FRAMES = {'g6-attributes.xrgb': '9d70e8a00b9e6f7aff86307be9925b264f7c03388a3dfdf75b8bf31856884ae8',
 'g6-counting.xrgb': 'db51a0eeea6c806b67e20a4e8c479c853ee8f05d7099d0292b0864c25abb11ea',
 'g6-dos-screen.xrgb': '7217896688951323a2252801c9af2dfa04edadcaa077d023df7360c88ecccbf0',
 'g6-drive.xrgb': '6001754a151ef7a480273425d355a78d6c15dd59fbde7046cd74b8da6469a743',
 'g6-file.xrgb': '61b87bf9240289282bd2a66b95b48ebd2d87735f0d0e44d3adaea6731a5b22ca',
 'g6-folder.xrgb': 'f8a5441da9bd3ba6ffcef0a0eea333ec25ea25a95a4187d577429574bb0f260e',
 'g6-tools.xrgb': '6b17515a51cc347aaa0b8defa2df5e392884156770a88299f8ba8c02f2dd69be',
 'g6-version.xrgb': 'f30f2bd8330d6483198ec1551deece438c4be4ad1ef54af8595ae0ae409e86b6'}


# fx-files-nav: keyboard/mouse popup and warning frames, inspected before pinning.
NAVIGATION_FRAMES = {'navigation-arrange.xrgb': '0813f88912975bbfa43b097a7459c7177131d80a650675e38692edec919310b6',
 'navigation-back-history.xrgb': '47ab5989d5651d0d85e167abf664578379208e03a691dee6d3c3df16db5eb8fc',
 'navigation-completion.xrgb': '9442abf28131b99669e097b164b3426f4185c99d2789fb8f507af9e7fef63808',
 'navigation-extension-warning.xrgb': '0f41e72ca67e9aefa987736fee00e2555fe4232dd8c84066ab27d3db547e86bd',
 'navigation-manual-icons.xrgb': '7291ac8fd1c3d3dc8b71134f6895ab1a84a9b57319f05779ab0a5535fb011a3b'}


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

    def test_files_navigation(self):
        stdout, digests = self.run_harness("files-navigation", ["tests/host/desktop/files_navigation_test.c",
            "tests/host/desktop/fake_vfs.c", "apps/files/app.c", "apps/files/model.c",
            "apps/files/transfer.c", *TOOLKIT], frames=True)
        self.assertIn("PASS navigation model", stdout)
        self.assertIn("PASS navigation UI", stdout)
        self.check_frames(digests, NAVIGATION_FRAMES, "NAVIGATION")

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
        self.assertEqual(len(digests), 31)
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
