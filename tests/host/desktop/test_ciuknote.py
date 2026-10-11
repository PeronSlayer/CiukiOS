"""f2-32 CiukNote host tests: text area model, application logic with a fake file system,
rendering digests and a replay of the QEMU case's key script. Scratch lives under build/host."""
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

# Rendered screens (sha256 of the raw XRGB array: 320x240 text area harness, 640x440 application),
# produced by the production renderer and reviewed as screenshots on the f2-31 toolkit
# (2026-10-11); a change here is a visible change of CiukNote or of libdeskui.
# Re-pinned 2026-10-12 after review: cn-open-tests shows text-file icons from the Tango store.
TEXTAREA_SCREENS = {
    "ta-bold.xrgb": "7805646b995636522bddc91f90e12a5a63923b0dfa083bcb68782c2f50d68f3d",
    "ta-plain.xrgb": "d832afc19218ae89624ee09457a701af1087c644edc79f22b4c5d2b59c2112d3",
    "ta-scrolled.xrgb": "9da85efa4fffc16a6db2f0a5b421b73b1d4c97186da4278e51f85188c8e3b0c1",
}
APP_SCREENS = {
    "cn-about.xrgb": "89c450cde4c55e38e924276a308437934db63e75eb5ed8c28b4fb460cc6bf606",
    "cn-bold.xrgb": "c09b6806cf1c593914b0afd73cd8c565e9a89e04f32898960178ebbbb07e3c16",
    "cn-context.xrgb": "2ab3f5690ab352e21f5e58b60080846de9fe89bfaa83a7c5e2792d79f5a30477",
    "cn-empty.xrgb": "844b1be57b4f845c50921712915e0b977476ffbf5e0bea68308d4ed2ccfaba09",
    "cn-find-bar.xrgb": "e2d612979f6f465024587da77bcb90daff5f15f9b4940809fb73f7bfe6273ff5",
    "cn-format-menu.xrgb": "20c5ed5449f2f00b7fcb5ee672e6d92c1f4a23b3cd0fe96c12aae094615452e4",
    "cn-help.xrgb": "581aa1a95923938d639825f9fece789a76b26460f889b79212cabe5b7e4a5ef0",
    "cn-limit.xrgb": "8e3b78d13945e4f53c13ca12f39173df6f8b8918c936464b938852837dfa6ac1",
    "cn-menu-edit.xrgb": "6872c1ccb6d46ef59fb4e6547b0e7fd386849e00cbf4deffedebc004c9ba24e1",
    "cn-menu-file.xrgb": "69f3df0b2d8f1a72284aa9dd6086622c15e93900217ccd391aeb5c5fe6424bf5",
    "cn-open-root.xrgb": "e5e4c4fabb866f7e9d1accb65ca0fb6565dc51f14f89cc03c7cdac95bb54fb6d",
    "cn-open-tests.xrgb": "e6136565acf6f8f05dd56b7af2872bcdfbb5cfea7c23bb0da2cd4236f9c4e33f",
    "cn-prompt-changes.xrgb": "b77da3d38ab87a0c1f04affa70c7870f044681687b51dd8d11af64015a17a632",
    "cn-replace-bar.xrgb": "4e9ff509a253865e28b293a4e3829a3765174279dd7f56a5df6d5d9f97eb0ccd",
    "cn-save-page.xrgb": "cc86442ce05aeffe194670da6a52ea6b6a8ed11f369547aac553591fdd648c08",
    "cn-saved.xrgb": "f3e546ddcebd4c8ddc6e94c0328c5f22a082e3017b7d0c26847deb31da7f275e",
    "cn-scrolled-bars.xrgb": "6e2941be09961be39b046dec50104d51d6db4cd0442a00d9dd2be00bd84609e1",
    "cn-status-hidden.xrgb": "8c7282da19f6e4f425122d0a8e9316260a2bff76548a91a91ed41683caedbc67",
    "cn-typed.xrgb": "6cecb1e48db81fc020128d7185e58807fd596bc63e80a81df6589c18428a28cd",
    "cn-wrapped.xrgb": "309ea0f0d6f7084244db438fe1108a76a9bcf0358121a88f569b17b59670548c",
}
# Digests the guest prints for the QEMU case (FNV-1a 32 of the 640x440 surface), derived on the
# host from the same script: compare them with the serial log lines "CiukNote checkpoint=...".
CHECKPOINTS = {"ready": "780ef981", "saved": "7f605aaf", "opened": "033a8c51"}

# QMP qcodes used by the scripts and the public set-1 codes the kernel reports (E0 keys | 0x100).
QCODES = {**{c: s for c, s in zip("qwertyuiop", range(0x10, 0x1a))}, **{c: s for c, s in zip("asdfghjkl", range(0x1e, 0x27))},
          **{c: s for c, s in zip("zxcvbnm", range(0x2c, 0x33))}, **{c: s for c, s in zip("1234567890", range(0x02, 0x0c))},
          "spc": 0x39, "ret": 0x1c, "esc": 0x01, "tab": 0x0f, "backspace": 0x0e, "shift": 0x2a, "ctrl": 0x1d, "alt": 0x38,
          "dot": 0x34, "slash": 0x35, "comma": 0x33, "minus": 0x0c, "f4": 0x3e, "f5": 0x3f, "f9": 0x43, "f10": 0x44, "f11": 0x57,
          "f12": 0x58, "up": 0x148, "down": 0x150, "left": 0x14b, "right": 0x14d, "home": 0x147, "end": 0x14f, "delete": 0x153}

FLAGS = ["-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-macro-redefined",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections"]
INCLUDES = ["-I", str(ROOT / "apps/libdeskui"), "-I", str(ROOT / "apps/ciuknote"), "-I", str(ROOT / "apps/desktop"),
            "-I", str(ROOT / "src/kernel/include")]
TOOLKIT = ["apps/libdeskui/draw.c", "apps/libdeskui/widgets.c", "apps/libdeskui/list.c", "apps/libdeskui/menu.c",
           "apps/libdeskui/dialog.c", "apps/libdeskui/textarea.c", "apps/desktop/compositor.c",
           "apps/libdeskui/icons.c", "apps/libdeskui/icon_map.c"]
APP = [*TOOLKIT, "apps/ciuknote/app.c", "apps/ciuknote/doc.c", "tests/host/desktop/ciuknote_test.c"]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ICONS = load("ciuknote_icon_assets", ROOT / "tests/host/desktop/icon_assets.py")


def case():
    suite = json.loads((ROOT / "tests/suites/f2-desktop.json").read_text())
    return next(c for c in suite["cases"] if c["id"] == "ciuknote-qemu-t23"), suite


def bin_programs():
    """The desktop menu lists /bin of the image, sorted by strcmp (the desktop itself is not listed)."""
    text = (ROOT / "scripts/build_image.py").read_text()
    return sorted(set(re.findall(r'"/bin/([A-Za-z0-9_.-]+)"', text)) - {"desktop"})


def menu_distance(programs, name):
    """Pointer travel from the Ciuki button (cursor 16 px above the corner) to the middle of a menu row:
    dy = -1 - 28 * (n + 2 - k) for the 28 px rows anchored to the bottom bar (see docs/desktop.md)."""
    return -1 - 28 * (len(programs) + 2 - programs.index(name))


class CiuknoteTests(unittest.TestCase):
    def compile(self, folder, name, sources):
        binary = folder / name
        env = dict(os.environ, TMPDIR=str(folder))
        subprocess.run(["clang", *FLAGS, *INCLUDES, *ICONS.defines(), *(str(ROOT / s) for s in sources), "-o", str(binary)],
                       check=True, env=env)
        return binary

    def run_binary(self, folder, binary, *args):
        env = dict(os.environ, TMPDIR=str(folder), ASAN_OPTIONS="detect_leaks=0")
        return subprocess.run([str(binary), *args], check=True, env=env, capture_output=True, text=True).stdout

    def screens(self, folder):
        return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(folder.glob("*.xrgb"))}

    def scratch(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        return tempfile.TemporaryDirectory(prefix="ciuknote-", dir=parent)

    def test_textarea_model_layout_pointer_and_rendering(self):
        with self.scratch() as temp:
            folder = Path(temp)
            binary = self.compile(folder, "textarea", [*TOOLKIT, "tests/host/desktop/textarea_test.c"])
            out = self.run_binary(folder, binary, str(folder / "ta"))
            self.assertIn("PASS textarea", out)
            actual = self.screens(folder)
            self.assertEqual(sorted(actual), sorted(f"ta-{n}.xrgb" for n in ("plain", "bold", "scrolled")))
            if TEXTAREA_SCREENS:
                self.assertEqual(actual, TEXTAREA_SCREENS)
            else:
                print("TEXTAREA_SCREENS", json.dumps(actual, indent=1))

    def test_application_with_fake_file_system(self):
        with self.scratch() as temp:
            folder = Path(temp)
            binary = self.compile(folder, "ciuknote", APP)
            out = self.run_binary(folder, binary, str(folder / "cn"))
            self.assertIn("PASS ciuknote", out)
            actual = self.screens(folder)
            if APP_SCREENS:
                self.assertEqual(actual, APP_SCREENS)
            else:
                print("APP_SCREENS", json.dumps(actual, indent=1))
            self.assertGreaterEqual(len(actual), 20)

    def test_qemu_case_is_well_formed(self):
        c, suite = case()
        files = next(x for x in suite["cases"] if x["id"] == "files-qemu-t23")
        runner_path = ROOT / "scripts/test"
        sys.path.insert(0, str(runner_path))
        try:
            runner = load("ciuknote_runner_test", runner_path / "run.py")
            runner.Actions(c["actions"])
        finally:
            sys.path.remove(str(runner_path))
        self.assertEqual((c["profile"], c["selector"], c["timeout"]), (files["profile"], files["selector"], files["timeout"]))
        self.assertTrue(c["desktop_screen"])
        self.assertEqual(len(c["actions"]), 1)
        self.assertEqual(c["actions"][0]["after"], {"event": "ARM", "action": "post_fault_input"})
        batches = c["actions"][0]["batches"]
        keys = [(e["data"]["key"]["data"], e["data"]["down"]) for b in batches for e in b["events"] if e["type"] == "key"]
        # F10 selects the CiukNote scenario (F9 Task Manager, F11 Files, F12 shell); two A edges end the case.
        self.assertEqual(keys[:2], [("f10", True), ("f10", False)])
        self.assertEqual(keys[-2:], [("a", True), ("a", False)])
        # The desktop counts A edges only once the last checkpoint is reached: none may follow File > Exit
        # (Alt+F, X: the only Alt chord of the script) but the pair.
        self.assertNotIn("a", [k for k, _ in keys[keys.index(("alt", True)):-2]])
        after = next(p for p in c["expected"]["predicates"] if p["where"].get("stage") == "after")
        self.assertEqual(after["fields"]["keys"], {"eq": 4})
        # The same controller predicates as the Files case, with the note-* records: write gate,
        # three checkpoints (ready, saved, opened) and the durable flush.
        def names(case_):
            return [(p["where"].get("case"), p["where"].get("checkpoint")) for p in case_["expected"]["predicates"]]
        expected = [(n.replace("files-", "note-") if n else n, k) for n, k in names(files) if not (k and k > 3)]
        self.assertEqual(names(c), expected)
        self.assertEqual(sorted(k for n, k in names(c) if n == "note-checkpoint"), [1, 2, 3])
        gate = next(p for p in c["expected"]["predicates"] if p["where"].get("case") == "note-write-gate")
        self.assertEqual(gate["fields"], {"expected": {"eq": 0}, "observed": {"eq": 0}})
        # The kernel's interaction window is 40 s for the app cases; leave a margin for QMP latency.
        total = sum(b["pause_ms"] for b in batches)
        self.assertLessEqual(total, 30000)
        self.assertGreater(total, 10000)
        self.assertEqual(c["checks"]["files"][0]["path"], "::/system/tests/note.txt")
        self.assertEqual(c["checks"]["files"][0]["sha256"], hashlib.sha256(
            b"Hello CiukNote\r\nCiuki says woof.\r\n9:41 AM 10/11/2026").hexdigest())
        self.assertEqual(c["checks"]["fsck_exit_codes"], [0])

    def test_menu_row_arithmetic_follows_the_image_inventory(self):
        """The scripts address Ciuki menu rows by distance from the bottom of the screen: rows are
        28 px, the menu lists /bin (without desktop) plus About and Restart, and the pointer starts
        on the Ciuki button, 16 px above the corner. Adding or removing a /bin program moves a row's
        distance; this test names the new one, and the desktop cases must follow."""
        programs = bin_programs()
        self.assertEqual(programs, ["ciuknote", "demo", "dosbox", "files", "hello", "libc_smoke", "lua", "tasks"],
                         "the image's /bin changed: recompute dy = -1 - 28 * (n + 2 - k) for the desktop cases")
        c, _ = case()
        x, y, clicks = 512, 384, []   # 1024x768 (QEMU std VGA mode 0x144); the cursor starts at the centre
        for batch in c["actions"][0]["batches"]:
            for event in batch["events"]:
                if event["type"] == "rel":
                    if event["data"]["axis"] == "x": x = min(max(x + event["data"]["value"], 0), 1023)
                    else: y = min(max(y + event["data"]["value"], 0), 767)
                elif event["type"] == "btn" and event["data"]["down"]: clicks.append((x, y))
        self.assertTrue(4 <= clicks[0][0] < 80 and 739 <= clicks[0][1] < 765, "the first click is on the Ciuki button")
        menu_top = 768 - 32 - 28 * (len(programs) + 2)
        self.assertEqual((clicks[1][1] - menu_top) // 28, programs.index("ciuknote"))
        self.assertEqual(clicks[1][1] - clicks[0][1], menu_distance(programs, "ciuknote"))
        # Apart from the corner clamp (+381 twice, three packets each) and the button approach (-16), the only
        # vertical move is the row distance.
        moves = [e["data"]["value"] for b in c["actions"][0]["batches"] for e in b["events"]
                 if e["type"] == "rel" and e["data"]["axis"] == "y" and e["data"]["value"] not in (381, -16)]
        self.assertEqual(moves, [menu_distance(programs, "ciuknote")])

    def key_script(self):
        """The case's keys after the launch click, as the replay harness reads them: K <code> <down> per key
        event and S after every batch (the guest renders and may report a checkpoint). The final A pair is
        not CiukNote's: it follows File > Exit, when the window is gone (the desktop counts it)."""
        c, _ = case()
        batches = c["actions"][0]["batches"]
        total = sum(1 for b in batches for e in b["events"] if e["type"] == "key")
        lines, buttons, seen = ["S"], 0, 0   # the guest renders and reports the empty window before the first key
        for b in batches:
            started = buttons >= 4
            for e in b["events"]:
                if e["type"] == "btn":
                    buttons += 1
                    continue
                if e["type"] != "key":
                    continue
                seen += 1
                if started and seen <= total - 2:
                    lines.append(f"K {QCODES[e['data']['key']['data']]} {int(e['data']['down'])}")
            if started:
                lines.append("S")
        return lines

    def test_qemu_case_script_replays_on_the_host(self):
        """The committed QMP key script drives the production application with the kernel's
        key-to-text rules; the saved file must be exactly the one the host checker digests."""
        c, _ = case()
        lines = self.key_script()
        with self.scratch() as temp:
            folder = Path(temp)
            binary = self.compile(folder, "ciuknote", APP)
            script = folder / "script.txt"
            script.write_text("\n".join(lines) + "\n")
            out = self.run_binary(folder, binary, "replay", str(script), "/system/tests/note.txt")
        fields, checkpoints = {}, []
        for line in out.splitlines():
            if line.startswith("CHECKPOINT "):
                checkpoints.append(line[len("CHECKPOINT "):])
            else:
                key, _, value = line.partition(" ")
                fields[key] = value
        expected = b"Hello CiukNote\r\nCiuki says woof.\r\n9:41 AM 10/11/2026"
        path, _, hexdata = fields["FILE"].partition(" ")
        self.assertEqual(path, "/system/tests/note.txt")
        self.assertEqual(bytes.fromhex(hexdata), expected)
        self.assertEqual(hashlib.sha256(expected).hexdigest(), c["checks"]["files"][0]["sha256"])
        self.assertEqual(fields["QUIT"], "1")                  # File > Exit on the clean document closes the window
        self.assertEqual(fields["DIALOG"], "0")                # no prompt is left open
        self.assertEqual(fields["TEXT"], str(len("Hello CiukNote\nCiuki says woof.\n9:41 AM 10/11/2026")))
        self.assertEqual([x.split()[1] for x in checkpoints], ["checkpoint=1", "checkpoint=2", "checkpoint=3"])
        self.assertEqual([x.split()[2] for x in checkpoints], ["event=ready", "event=saved", "event=opened"])
        self.assertIn("path=/system/tests/note.txt", checkpoints[2])
        digests = {x.split()[2].split("=")[1]: x.split("digest=")[1] for x in checkpoints}
        if CHECKPOINTS:
            self.assertEqual(digests, CHECKPOINTS)
        else:
            print("CHECKPOINTS", json.dumps(digests), json.dumps(checkpoints, indent=1))


if __name__ == "__main__":
    unittest.main()
