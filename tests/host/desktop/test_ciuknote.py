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
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

# Rendered screens (600x352 XRGB, sha256 of the raw array), produced by the production
# renderer and reviewed as screenshots; a change here is a visible change of CiukNote.
TEXTAREA_SCREENS = {
    "ta-bold.xrgb": "869c3fd6c987c07c824804b17fca4eccb1e2745cca9b0adc970ba782cff8ab06",
    "ta-plain.xrgb": "bb2bb95113791db155a8f85fd8cb81a7ac1010a80528a47e0159f469dbb3211e",
    "ta-scrolled.xrgb": "1288f168fb77a154866c841636ca783b4331930327c50bf00468337f7aa3a0e2",
}
APP_SCREENS = {
    "cn-about.xrgb": "76c40fbc97a0922cab1a691ada5d451158ec7014261379c2700ce6f992430161",
    "cn-bold.xrgb": "17fbe8032c98b91b77248a1a5f23b06dc55c37cd4be60ed3722c404eff057731",
    "cn-context.xrgb": "f79cd71c0e45ab77d363266b0b8607ea37c11d705915cd50fceca31bf86df378",
    "cn-empty.xrgb": "7a972ffc3f9a30b0cb86423abf8ab1c69c49751143f84fdbc80add44d8546e70",
    "cn-find-bar.xrgb": "adf5260d742464041fa10cfb8994bf5ab88d1efa3dc311e5f81973dead27b9b1",
    "cn-format-menu.xrgb": "b5d28d2cadca172d9ccd6c8b6ca3ceeba27142b259386c07ab40f099595c7630",
    "cn-help.xrgb": "260b10c15f82497a11ae1ad83695d1b2973bf2670b81bd21d011f21cf865826d",
    "cn-limit.xrgb": "a9d7fb51744cdf09263c50ae01ddb45324d8fa797269def5484f0bdc62ebedb1",
    "cn-menu-edit.xrgb": "20060fd99165d35c1655d809124c245fccb2a18d90466e8eabb74038761a9102",
    "cn-menu-file.xrgb": "962f654bbbe1a9baa8c579ba2ffd4e64b7b9ea90d3890b071810520b5c853eb5",
    "cn-open-root.xrgb": "34e1cdc5c5085291415bc0f5e9734f1da9b82c93ffdd75fc7f92521e08fbc02e",
    "cn-open-tests.xrgb": "d2868567575dafd33b83c2f13dfb1f9c8bab9728bb13b2a53b92ac96cde1db44",
    "cn-prompt-changes.xrgb": "7288d294dad1f19247099c145a9aeb45ed02c6aaeaa7d88aaada45cfd7eae1ea",
    "cn-replace-bar.xrgb": "9d8fca24779ad40510d152372c80e0fec1af95e380c005703eada83fdf8bd9a4",
    "cn-save-page.xrgb": "40140612f63641b9fa4cb7c406ec8638d13c1e8ee199373cea38317898c8f368",
    "cn-saved.xrgb": "aa266a4aaf94e79d90fc0535be2ed8400d7e6b5fd5da9a01625f35390f2a352b",
    "cn-scrolled-bars.xrgb": "d5b3a2e1b674cee842fe4f09c9192b179c28d16320753ae8f66a8f30184be6a2",
    "cn-status-hidden.xrgb": "964820ca48c59fc34f460b4aca4cb26c2b0fd4cb01e8fb56cd039c52eeefcf86",
    "cn-typed.xrgb": "e78e7c6e89ffab5e2ba668ebb5c8e732b55f5f1ff4ff1e433c67b2e738398d93",
    "cn-wrapped.xrgb": "7f9f7f5265992419774da1e7f4c64964bfbf81b3c1a709b9ba137e3dc6993082",
}
# Digests the guest prints for the QEMU case (FNV-1a 32 of the surface), derived on the host
# from the same script: compare them with the serial log lines "CiukNote checkpoint=...".
CHECKPOINTS = {"ready": "ba7c24ab", "saved": "cab5d60b", "opened": "8e0e8937"}

# QMP qcodes used by the scripts and the public set-1 codes the kernel reports (E0 keys | 0x100).
QCODES = {**{c: s for c, s in zip("qwertyuiop", range(0x10, 0x1a))}, **{c: s for c, s in zip("asdfghjkl", range(0x1e, 0x27))},
          **{c: s for c, s in zip("zxcvbnm", range(0x2c, 0x33))}, **{c: s for c, s in zip("1234567890", range(0x02, 0x0c))},
          "spc": 0x39, "ret": 0x1c, "esc": 0x01, "tab": 0x0f, "backspace": 0x0e, "shift": 0x2a, "ctrl": 0x1d, "alt": 0x38,
          "dot": 0x34, "slash": 0x35, "comma": 0x33, "minus": 0x0c, "f5": 0x3f, "f10": 0x44, "f11": 0x57, "f12": 0x58,
          "up": 0x148, "down": 0x150, "left": 0x14b, "right": 0x14d, "home": 0x147, "end": 0x14f, "delete": 0x153}

FLAGS = ["-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-macro-redefined",
         "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections"]
INCLUDES = ["-I", str(ROOT / "apps/libdeskui"), "-I", str(ROOT / "apps/ciuknote"), "-I", str(ROOT / "apps/desktop"),
            "-I", str(ROOT / "src/kernel/include")]
TOOLKIT = ["apps/libdeskui/draw.c", "apps/libdeskui/widgets.c", "apps/libdeskui/textarea.c", "apps/desktop/compositor.c"]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def case():
    suite = json.loads((ROOT / "tests/suites/f2-desktop.json").read_text())
    return next(c for c in suite["cases"] if c["id"] == "ciuknote-qemu-t23"), suite


class CiuknoteTests(unittest.TestCase):
    def compile(self, folder, name, sources):
        binary = folder / name
        env = dict(os.environ, TMPDIR=str(folder))
        subprocess.run(["clang", *FLAGS, *INCLUDES, *(str(ROOT / s) for s in sources), "-o", str(binary)], check=True, env=env)
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
            binary = self.compile(folder, "ciuknote",
                                  [*TOOLKIT, "apps/ciuknote/app.c", "apps/ciuknote/doc.c", "tests/host/desktop/ciuknote_test.c"])
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
        runner_path = ROOT / "scripts/test"
        import sys
        sys.path.insert(0, str(runner_path))
        try:
            runner = load("ciuknote_runner_test", runner_path / "run.py")
            runner.Actions(c["actions"])
        finally:
            sys.path.remove(str(runner_path))
        shell = next(x for x in suite["cases"] if x["id"] == "desktop-shell-qemu-t23")
        self.assertEqual(c["selector"], shell["selector"])
        self.assertEqual(c["expected"], shell["expected"])
        self.assertTrue(c["desktop_screen"])
        self.assertTrue(all(a["after"] == {"event": "ARM", "action": "post_fault_input"} for a in c["actions"]))
        total = sum(b["pause_ms"] for a in c["actions"] for _ in range(a.get("repeat", 1)) for b in a["batches"])
        # The kernel's post-fault interaction window is 15 s from ARM; leave a margin for QMP latency.
        self.assertLessEqual(total, 11000)
        keys = [e["data"] for a in c["actions"] if a["note"] == "finish-key-a" for b in a["batches"] for e in b["events"]]
        self.assertEqual([(k["key"]["data"], k["down"]) for k in keys], [("a", True), ("a", False)])
        f11 = [e["data"] for e in c["actions"][0]["batches"][0]["events"] + c["actions"][0]["batches"][1]["events"]]
        self.assertEqual([(k["key"]["data"], k["down"]) for k in f11], [("f11", True), ("f11", False)])
        self.assertEqual(shell["expected"]["predicates"][-1]["fields"]["keys"], {"eq": 4})

    def test_menu_row_arithmetic_follows_the_image_inventory(self):
        """The scripts address Ciuki menu rows by distance from the bottom of the screen.
        Rows are 28 px; the menu lists /bin (without desktop) plus About and Restart. Adding or
        removing a /bin program moves them: this test then names the new offset."""
        text = (ROOT / "scripts/build_image.py").read_text()
        programs = sorted(set(re.findall(r'"/bin/([A-Za-z0-9_.-]+)"', text)) - {"desktop"})
        self.assertEqual(programs, ["ciuknote", "demo", "files", "hello", "libc_smoke", "lua"],
                         "the image's /bin changed: update the Ciuki menu offsets of the desktop cases")
        rows = len(programs) + 2

        def distance(name):  # cursor starts 17 px above the bottom after opening the menu
            return 32 + 28 * rows - 28 * programs.index(name) - 14 - 17
        c, suite = case()
        launch = next(a for a in c["actions"] if a["note"].startswith("launch-first-menu-row"))
        y = [e["data"]["value"] for e in launch["batches"][0]["events"] if e["data"]["axis"] == "y"]
        self.assertEqual(programs.index("ciuknote"), 0)
        self.assertEqual(y, [-distance("ciuknote")])
        for shell in (x for x in suite["cases"] if x["id"].startswith("desktop-shell-")):
            moves = [e["data"]["value"] for b in shell["actions"][0]["batches"] for e in b["events"]
                     if e["type"] == "rel" and e["data"]["axis"] == "y" and e["data"]["value"] in (-distance("demo"),)]
            self.assertEqual(moves, [-distance("demo")], shell["id"])

    def test_qemu_case_script_replays_on_the_host(self):
        """The committed QMP key script drives the production application with the kernel's
        key-to-text rules; the saved file must be exactly the one the host checker digests."""
        c, _ = case()
        lines = ["S"]  # the guest renders and reports the empty window before the first key arrives
        for a in c["actions"]:
            if not a["note"].startswith("ciuknote-"):
                continue
            for _ in range(a.get("repeat", 1)):
                for b in a["batches"]:
                    for e in b["events"]:
                        if e["type"] == "key":
                            lines.append(f"K {QCODES[e['data']['key']['data']]} {int(e['data']['down'])}")
                    lines.append("S")
        with self.scratch() as temp:
            folder = Path(temp)
            binary = self.compile(folder, "ciuknote",
                                  [*TOOLKIT, "apps/ciuknote/app.c", "apps/ciuknote/doc.c", "tests/host/desktop/ciuknote_test.c"])
            script = folder / "script.txt"
            script.write_text("\n".join(lines) + "\n")
            out = self.run_binary(folder, binary, "replay", str(script), "/system/tests/note.txt")
        fields = {}
        checkpoints = []
        for line in out.splitlines():
            if line.startswith("CHECKPOINT "):
                checkpoints.append(line[len("CHECKPOINT "):])
            else:
                key, _, value = line.partition(" ")
                fields[key] = value
        expected = (b"Hello CiukNote\r\nCiuki says woof.\r\n9:41 AM 10/11/2026")
        path, _, hexdata = fields["FILE"].partition(" ")
        self.assertEqual(path, "/system/tests/note.txt")
        self.assertEqual(bytes.fromhex(hexdata), expected)
        self.assertEqual(hashlib.sha256(expected).hexdigest(), c["checks"]["files"][0]["sha256"])
        self.assertEqual(fields["QUIT"], "1")                  # File > Exit closed the window
        self.assertEqual(fields["PAGE"], "0")                  # back on the editor page
        self.assertEqual(fields["TEXT"], str(len("Hello CiukNote\nCiuki says woof.\n9:41 AM 10/11/2026")))
        self.assertEqual([x.split()[1] for x in checkpoints], ["checkpoint=1", "checkpoint=2", "checkpoint=3"])
        self.assertEqual(len(checkpoints), 3)
        digests = {x.split()[2].split("=")[1]: x.split("digest=")[1] for x in checkpoints}
        if CHECKPOINTS:
            self.assertEqual(digests, CHECKPOINTS)
        else:
            print("CHECKPOINTS", json.dumps(digests), json.dumps(checkpoints, indent=1))


if __name__ == "__main__":
    unittest.main()
