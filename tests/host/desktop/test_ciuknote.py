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
import importlib.util as _review_util  # optional PNG review copies (frame_review.py, any import mode)
_review_spec = _review_util.spec_from_file_location("frame_review", __file__.rsplit("/", 1)[0] + "/frame_review.py")
_review = _review_util.module_from_spec(_review_spec); _review_spec.loader.exec_module(_review)
capture = _review.capture

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]

# Rendered screens (sha256 of the raw XRGB array: 320x240 text area harness, 640x440 application),
# produced by the production renderer and reviewed as screenshots on the f2-31 toolkit
# (2026-10-11); a change here is a visible change of CiukNote or of libdeskui.
# wt/ui-integ (2026-10-12): Tango store icons (wt/icons) and the ui-look theme, fonts and controls together;
# F2-64 recorded affected transient frames; remaining frames await lead review.
# 2026-10-15 (main after f2-63 + f2-64): pinned frames re-recorded on the combined tree (FreeType text in the f2-64 dialogs), reviewed against the 9a05ab9f renders.
TEXTAREA_SCREENS = {
    'ta-bold.xrgb': 'c184662e3d21f87b91eaf034a932b750fc4a1baa4a9668d7396b6bc847e25d0e',
    'ta-plain.xrgb': 'fb9a313f1a54d109e21735aa28049a1cd71d3f632c5255bceef2a07351b99cce',
    'ta-scrolled.xrgb': '33d61b4ba794401e2fbc8ae712df3e6466c836e0a5ef57b569b690de48059021',
}
# F4-30 (2026-10-18): reviewed Places in the shared Open/Save dialog;
# only the dialog frames change.
APP_SCREENS = {
    'cn-about.xrgb': 'de423649ddeedb3807983783ba540cb28d6cc872194e40013dccc0fed252e061',
    'cn-bold.xrgb': 'dbb6e00d030ec73460eb09788b033173f87782a6c1075aeeead4aec5d559f976',
    'cn-context.xrgb': 'f94b8ed1d6e07a689eebf6b8e0570fd19558d99c463c63053b5f7a0044359e3c',
    'cn-empty.xrgb': 'b52ffcdab84e440cf0fbfa9bafcd0cdfa9c62d057bd4f843f234ddfe09e00c2b',
    'cn-find-bar.xrgb': 'b1c769eb8d2c9ca2b777dc18afb33ecc42431fe91341e32b2b56680bbce2fdac',
    'cn-format-menu.xrgb': 'd19a5699eda567fe5560bc60dd68caf4c636a78756b2971ffc8c44990c2b6d4c',
    'cn-help.xrgb': 'e89781b5f31fd40b0c90dde5bca927fcb4b4874995b10194b5ce31d57a2b5630',
    'cn-limit.xrgb': '200981da8810de5f08368d9ec8990eb5c544fbb571cd4f8bbd0b001e70bff88d',
    'cn-menu-edit.xrgb': '1e116d6bf0b76ed52d7f28cfd1bfcb01bb61e6685605e46c80f7b1ece428f962',
    'cn-menu-file.xrgb': '8373e57ec17b178ef87538570d77303d42a7265e4afb9aa994e3bd9c3ed58f13',
    'cn-open-root.xrgb': '31c0813f08d9ded09f97c9a41a39a72a2331c5c008547a70da06e2ef5a8e36e1',
    'cn-open-tests.xrgb': 'ae632eeb42e0a5184db2ab11e11232564551b5bc43c3b0b987fa0a926d1e1af0',
    'cn-prompt-changes.xrgb': '1dac23bc0dcb2c31e1361d83e7b411362324f26dae3a7e4f076b07e4a65168a2',
    'cn-replace-bar.xrgb': '95039478d4218b647d6e842cf3c7553e130d8eb2bbcc22a8e8ed7afb2c1da801',
    'cn-save-page.xrgb': 'de5affaa20dc00ffe5d24ae002e467033140c1aea5a4ad5f284f123a0a7081ed',
    'cn-saved.xrgb': 'c31ebaddb8dc2b5f791cee0cfc693a942fafa3e1e12902490bb1ceb7198da3a5',
    'cn-scrolled-bars.xrgb': '06804b6ef253b27c12cf0f48b6ab0ff63faa3d073d9b13cde8a9e938be7ab366',
    'cn-status-hidden.xrgb': '7b12865ddbc38c28518557e9b58151831dc6a27b5dd376c1443217dd7b2380ff',
    'cn-typed.xrgb': '0c9bfefa5c1cf6a2e6fd6c0f2016b8d8431f2fbf87d5f2ec0cb5cfb11ea03992',
    'cn-wordpad-offer.xrgb': 'a4839957925d14584d1ab79d7ec79983fa85aa5753d3c42314c4a473457d05a6',   # G28: the offer to open a large file in WordPad
    'cn-wrapped.xrgb': '945e6aed942540b9910e104d1a0888b9bc2effe444d7f18c0812e017ae3142cf',
}
# Digests the guest prints for the QEMU case (FNV-1a 32 of the 640x440 surface), derived on the
# host from the same script: compare them with the serial log lines "CiukNote checkpoint=...".
CHECKPOINTS = {'opened': 'dcc51b2a', 'ready': 'b861e58b', 'saved': '6d21f27c'}

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
           "apps/libdeskui/dialog.c", "apps/libdeskui/textarea.c", "apps/desktop/compositor.c", "apps/desktop/theme.c",
           "apps/libdeskui/icons.c", "apps/libdeskui/icon_map.c"]
APP = [*TOOLKIT, "apps/ciuknote/app.c", "apps/ciuknote/doc.c", "tests/host/desktop/ciuknote_test.c"]

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


ICONS = load("ciuknote_icon_assets", ROOT / "tests/host/desktop/icon_assets.py")


def case():
    suite = json.loads((ROOT / "tests/suites/f2-desktop.json").read_text())
    return next(c for c in suite["cases"] if c["id"] == "ciuknote-qemu-t23"), suite


def bin_programs():
    """The desktop menu lists /Programs of the image, sorted by strcmp (the desktop itself is not listed)."""
    return load("ciuknote_image_inventory", ROOT / "tests/host/desktop/shell_menu.py").image_programs(ROOT)


class CiuknoteTests(unittest.TestCase):
    def compile(self, folder, name, sources):
        binary = folder / name
        env = dict(os.environ, TMPDIR=str(folder))
        subprocess.run(["clang", *FLAGS, *INCLUDES, *ICONS.defines(), *(str(ROOT / s) for s in sources), "-o", str(binary)],
                       check=True, env=env)
        return binary

    def run_binary(self, folder, binary, *args):
        env = dict(os.environ, TMPDIR=str(folder), ASAN_OPTIONS="detect_leaks=0")
        result = subprocess.run([str(binary), *args], env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def screens(self, folder):
        for p in folder.glob("*.xrgb"):
            width, height = (320, 240) if p.name.startswith("ta-") else (640, 440)
            capture(p.stem, p.read_bytes(), width, height)
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
                assert_pins(self, actual, TEXTAREA_SCREENS, "TEXTAREA_SCREENS")
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
                assert_pins(self, actual, APP_SCREENS, "APP_SCREENS")
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
        """The script reaches CiukNote through the Ciuki button, Programs and the CiukNote row of the
        Programs submenu (tests/host/desktop/shell_menu.py). Adding or removing a /Programs program can move a
        submenu row; this test names the inventory, and the desktop cases must follow."""
        programs = bin_programs()
        self.assertEqual(programs, ["calc", "charmap", "ciukide", "ciuknote", "clock", "control", "demo", "dosbox", "files", "gfxdemo", "hello", "help", "hwreport", "libc_smoke", "lua", "mixer", "music", "paint", "regedit", "tasks", "tcc", "terminal", "viewer", "wordpad"],
                         "the image's /Programs changed: check the Programs submenu rows of the desktop cases")
        menu = load("note_shell_menu", ROOT / "tests/host/desktop/shell_menu.py")
        c, _ = case()
        x, y, clicks = 512, 384, []   # 1024x768 (QEMU std VGA mode 0x144); the cursor starts at the centre
        for batch in c["actions"][0]["batches"]:
            for event in batch["events"]:
                if event["type"] == "rel":
                    if event["data"]["axis"] == "x": x = min(max(x + event["data"]["value"], 0), 1023)
                    else: y = min(max(y + event["data"]["value"], 0), 767)
                elif event["type"] == "btn" and event["data"]["down"]: clicks.append((x, y))
        menu.check_launch(self, clicks, programs, "ciuknote")
        self.assertEqual(len(clicks), 3, "the case clicks only the Ciuki button, Programs and CiukNote")
        # Apart from the corner clamp (-381 twice, to the top edge) and the approach onto the Ciuki button
        # in the top bar: down to Programs, the CiukNote row offset in the submenu and the move back to the
        # f2-32 pointer position.
        start, programs_y = menu.START_CLICK[1], menu.PROGRAMS_CLICK_Y
        moves = [e["data"]["value"] for b in c["actions"][0]["batches"] for e in b["events"]
                 if e["type"] == "rel" and e["data"]["axis"] == "y"]
        self.assertEqual(moves[:3], [-381, -381, start])
        row = menu.submenu_row_y(programs, "ciuknote")
        self.assertEqual(moves[3:], [programs_y - start, row - programs_y, 470 - row])

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
            assert_pins(self, digests, CHECKPOINTS, "CHECKPOINTS")
        else:
            print("CHECKPOINTS", json.dumps(digests), json.dumps(checkpoints, indent=1))


if __name__ == "__main__":
    unittest.main()
