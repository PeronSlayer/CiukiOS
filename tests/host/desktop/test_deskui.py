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
           "apps/libdeskui/menu.c", "apps/libdeskui/dialog.c", "apps/libdeskui/textarea.c", "apps/desktop/compositor.c",
           "apps/libdeskui/icons.c", "apps/libdeskui/icon_map.c"]

# Reviewed frames (2026-10-11): an unpinned run prints the new digests. Convert
# the harness frames (XRGB8888, 400x300 toolkit, 760x520 Files) to PNG and
# re-pin only after a visual review.
# Re-pinned 2026-10-12 after review: Tango store icons (libdeskui du_icon) in the
# Files places/tree/bin/type icons and the open dialog's file-type icons.
TOOLKIT_FRAMES = {
    "deskui-input.xrgb": "cd7aed148d6c9ae54c23e13eb17563a2ae8488f4d40664d1fe81fe99170251c8",
    "deskui-menu.xrgb": "6a3b7c78ec5576f2f237e4abf344d850a49f92c281e29a86880f0f41c13aabe0",
    "deskui-message.xrgb": "e652a4f707dd38bfc4486703d669a9bb5a0a3e591f062aa96313b6ac9c1972a8",
    "deskui-open.xrgb": "66ef7ebe0f2b563c54c56333bcd3db4578ff901021b602667b955dc0b66741b2",
    "deskui-popup.xrgb": "448045458ae3a455eeceff6e6629169ce00810984182c38c797ac3b36754c185",
    "deskui-progress.xrgb": "6915fefe563a3a461bc144867bc3b7926cdd5c41dcf5ddbda76f0209d46f8dac",
    "deskui-view0.xrgb": "b7b45a87702afd0c8a4fea32a8bd87a0728f75743b6a4b938c1c50f886dd1fcc",
    "deskui-view1.xrgb": "5379566410cd35344722e958d9f382afebd24d5fee6ab14afbd47d345bceeede",
    "deskui-view2.xrgb": "0bc25854712e0ea58ce7c8ed9dfb779b5951ad2ded3832ac75587d2ce726f51b",
}
FILES_FRAMES = {
    "files-about.xrgb": "4e9a6df7bccc68089d1330c9aab409f535dc55c82e9123f8de05d9e4b129a821",
    "files-bin-icons.xrgb": "45d901bbcb4bafe96763bcc3a0ec3bcb45992f88224d5d3bfaeacc8ea032178d",
    "files-confirm-delete.xrgb": "838513dfbc9fa394fa0b1527dd81e8f27d9b6a27d6d52e19d3921ffc012cdc64",
    "files-context-background.xrgb": "913bbfc0fdad940f6c615357810782254b312094de23fdf3521fcf03857e54a9",
    "files-context-item.xrgb": "ef79f6a5bdc0b1d24a2c0b0c25ecc4d27e0d0dd5b7c0d261a36e7135fff79482",
    "files-copy-progress.xrgb": "c2cfd6335896196969bef87114e720d4b1a40107f5c5203bae71bde6b1fa4e37",
    "files-cp1-system.xrgb": "e3a5e0fced0541b2d05317fea6b38c02608a5e5831cda8bf31d509072fd3ea9b",
    "files-cp2-new-folder.xrgb": "788bac2a40c1d63df676377c22d384241f12d66f4fe6d173c6c3c21e00f6b58b",
    "files-cp3-renamed.xrgb": "6a95eaee30d7094c5f2b19fbb9f8cc0c0a2ba9ad20c8b70733c756d0f644381d",
    "files-cp4-copied.xrgb": "3f78909f9dd88bbf60f6ea6d67787c8756244e1f9ec62320857ec61a26110be8",
    "files-cp5-deleted.xrgb": "92e1ea862278f9bd7fd8dd4517a90dcc28aac821330ef8c36c2450464be7e1d7",
    "files-help.xrgb": "1736a56b35cdcb1d55c515915ced93303301754fab88223d4cfa5726fe0fbd00",
    "files-menu-file.xrgb": "8e7cb258ae8339b909323c42377c4fe13e7036a30759abe16d86f57986423d00",
    "files-menu-view.xrgb": "18260f7cf633da2209fbfcb728b0ef12137a5e188e6adef3e028cc0cafafdde5",
    "files-multi-select.xrgb": "70407eb59478acb9903db53c63b204308db3218b109727a7ec6aa265a9568256",
    "files-properties.xrgb": "bee3f324de75e47546a40025ab000783ac0d919e651e43ebb30e5dc46e1831f5",
    "files-rename-error.xrgb": "04e896519e40cb106b558988b8231048f2dfe195df6cdc01dbfac57fb41b9b63",
    "files-replace-prompt.xrgb": "9fefe01913eff1b88db45bdd018bcd3da3ea56f2a8d4d9ceecfdc360f1e01ed6",
    "files-root-details.xrgb": "07670c675c49e9c31b11700b7bb88dbf671ff94fd2ce4ca6d5e6a72e2eaca80d",
    "files-sort-size-desc.xrgb": "de6d3b701fb83d2ba896b0eb0578a39a776c534ad4f9272c7dca7202423b178c",
    "files-system-details.xrgb": "c5552601f63c2267ca76389c14774b0d00079fb3d91148f02b01f9e172d74089",
    "files-system-icons.xrgb": "7c7e55fdff5fce2315ab1d8b1f34cb5a974aff413742f3e0c3681f92e1d92e20",
    "files-tree.xrgb": "5e5c80a147ddf098c88b3d49cb05572ff6f5d11e2010af3ccf928875777d01b2",
}


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
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-D_POSIX_C_SOURCE=200809L",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                            "-I", str(ROOT / "apps/libdeskui"), "-I", str(ROOT / "apps/files"), "-I", str(ROOT / "src/kernel/include"),
                            *ICONS.defines(), *(str(ROOT / s) for s in sources), "-o", str(binary)], check=True, env=env)
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
        # Ciuki button, Programs, then the Files row of the Programs submenu (tests/host/desktop/shell_menu.py).
        programs = sorted(set(re.findall(r'"/bin/([A-Za-z0-9_.-]+)"', (ROOT / "scripts/build_image.py").read_text())) - {"desktop"})
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
