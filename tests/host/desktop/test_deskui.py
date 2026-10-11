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
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
TOOLKIT = ["apps/libdeskui/draw.c", "apps/libdeskui/widgets.c", "apps/libdeskui/list.c",
           "apps/libdeskui/menu.c", "apps/libdeskui/dialog.c", "apps/libdeskui/textarea.c", "apps/desktop/compositor.c"]

# Reviewed frames (2026-10-11): an unpinned run prints the new digests. Convert
# the harness frames (XRGB8888, 400x300 toolkit, 760x520 Files) to PNG and
# re-pin only after a visual review.
TOOLKIT_FRAMES = {
    "deskui-input.xrgb": "cd7aed148d6c9ae54c23e13eb17563a2ae8488f4d40664d1fe81fe99170251c8",
    "deskui-menu.xrgb": "6a3b7c78ec5576f2f237e4abf344d850a49f92c281e29a86880f0f41c13aabe0",
    "deskui-message.xrgb": "e652a4f707dd38bfc4486703d669a9bb5a0a3e591f062aa96313b6ac9c1972a8",
    "deskui-open.xrgb": "d0a12aba4db4070909d712af782db5002c329e94cc47e23a94d68e26849f6423",
    "deskui-popup.xrgb": "448045458ae3a455eeceff6e6629169ce00810984182c38c797ac3b36754c185",
    "deskui-progress.xrgb": "6915fefe563a3a461bc144867bc3b7926cdd5c41dcf5ddbda76f0209d46f8dac",
    "deskui-view0.xrgb": "b7b45a87702afd0c8a4fea32a8bd87a0728f75743b6a4b938c1c50f886dd1fcc",
    "deskui-view1.xrgb": "5379566410cd35344722e958d9f382afebd24d5fee6ab14afbd47d345bceeede",
    "deskui-view2.xrgb": "0bc25854712e0ea58ce7c8ed9dfb779b5951ad2ded3832ac75587d2ce726f51b",
}
FILES_FRAMES = {
    "files-about.xrgb": "848608e756eb362f7bc4d1a7786691b3e5952f63a56a5442092a74c3b8311bd3",
    "files-bin-icons.xrgb": "4c6a87ed369387027741cfcc76fb05de2927e2b9c55df6974f1f7c6804919443",
    "files-confirm-delete.xrgb": "fd47eff318c5f996920050cdf2e32501250161be33b940644a52b7d9af065cf2",
    "files-context-background.xrgb": "7d035a9c7900d4ff94e58d8d4f9af4b3695f0eda5ea2eb5fae90f94eba3d4a6f",
    "files-context-item.xrgb": "4822dd79e1a4e1310bb7e5139c5aa501c15a71b39e06e7dee6e0da9132ec6a63",
    "files-copy-progress.xrgb": "2e971b17357a0dd9fc22b9b50eaa2535c67f5d7d68c28f5958bb3a69413d269a",
    "files-cp1-system.xrgb": "74a71b778af7b88181d83121c9b329add96db6125d3f057e27cfff657aa55164",
    "files-cp2-new-folder.xrgb": "4c35e8cc64e74e01f4133b5fdfbc39c9601c247ec65997879a6acd60f3bf4b24",
    "files-cp3-renamed.xrgb": "84323e077af4dfd29de63bea8c66bb7684deed81fa132b6e27b347e173ba4259",
    "files-cp4-copied.xrgb": "d08e6b1bc00856b2afc78e177d8b7733bbc532f8535d4fa50219860b38b5b1af",
    "files-cp5-deleted.xrgb": "d77bdadc036e25f432d0e7e73f7ae3e4e00ef2538c26a3fe2c2b18235be13b5a",
    "files-help.xrgb": "677e8a2507dd53d7c8bc3ab11b3767c61aae0731394f3fdf6d0c6eacddcf4fa7",
    "files-menu-file.xrgb": "95ef19921b9407be6851e298e0fcd084adbe64a10b71af0597ace371aad2639f",
    "files-menu-view.xrgb": "ebc24ad44c9ee83bb70531e58131fa9dc6d1c789a4b30490f197ba0c16f1eef2",
    "files-multi-select.xrgb": "0ff6ca9e82ed0594a174e5c32dd1cf9f1b5d221bdc7b4b151196bbe60fb2335a",
    "files-properties.xrgb": "080576a0f1e700b7f0b80aa4e4c93435c167e76184ab59d727e2a07141a4650a",
    "files-rename-error.xrgb": "8266209136693d75a8e81899fb0545eb74689f4c5da7434791c1a4ac5658d8f7",
    "files-replace-prompt.xrgb": "16a93e3305f6b3d6a1e3f4d7661888dd267b9cda5c1ddaecb27276c7e9d31f4f",
    "files-root-details.xrgb": "5282f23b1cb7fd8c73f6746bfeaf246b319884195927e6296978100e0573f690",
    "files-sort-size-desc.xrgb": "6b99f6c3fbaaaee919a6493bd26cad3eb3f87ff69bb4cf90fd9f9e5cdf91996b",
    "files-system-details.xrgb": "cfe19bb7248aae935bf752e6eec247d5b442c5c23e4c354ab829e6d7ec06d0af",
    "files-system-icons.xrgb": "c6135eb087a4c61af8949da49c7aefcf1f326283bd9463297ac73cef7f60544e",
    "files-tree.xrgb": "fb19be1f01ca4f871d0d27bac8addb7e5c87d398de8c8e174a6cc841986ffcdd",
}


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


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
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-D_POSIX_C_SOURCE=200809L",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                            "-I", str(ROOT / "apps/libdeskui"), "-I", str(ROOT / "apps/files"), "-I", str(ROOT / "src/kernel/include"),
                            *(str(ROOT / s) for s in sources), "-o", str(binary)], check=True, env=env)
            output = folder / "frames"
            output.mkdir()
            extra = [str(a(folder)) if callable(a) else str(a) for a in args]
            result = subprocess.run([str(binary), *([str(output)] if frames else []), *extra],
                                    check=True, env=env, capture_output=True, text=True)
            print(result.stdout.strip())
            digests = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(output.glob("*.xrgb"))}
            return result.stdout, digests

    def check_frames(self, actual, pinned, label):
        if not pinned:
            print(label + "_FRAMES", json.dumps(actual, indent=1, sort_keys=True))
            self.fail(label + " frames are not pinned")
        self.assertEqual(actual, pinned)

    def test_toolkit_widgets_and_frames(self):
        _, digests = self.run_harness("deskui", ["tests/host/desktop/deskui_test.c", *TOOLKIT], frames=True)
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
                                             "apps/files/app.c", "apps/files/model.c", *TOOLKIT], portrait, frames=True)

    def test_files_ui_script_and_frames(self):
        stdout, digests = self.files_ui()
        self.assertIn("PASS Files UI", stdout)
        self.assertEqual(len(digests), 23)
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
        # Ciuki button; menu row 1 of demo, files, hello, libc_smoke, lua (28 px rows above the bar).
        self.assertTrue(4 <= clicks[0][0] < 80 and 739 <= clicks[0][1] < 765)
        menu_top = 768 - 32 - 28 * 7
        self.assertEqual((clicks[1][1] - menu_top) // 28, 1)
        client = (4 + 3, 36 + 24)   # Files moves its frame to 4,36; content below the 24 px title
        expected = [targets[k] for k in ("view_title", "list_item", "places_system", "file_title", "new_folder")]
        self.assertEqual([(cx - client[0], cy - client[1]) for cx, cy in clicks[2:]], expected)
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
