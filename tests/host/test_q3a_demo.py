"""Quake III Arena demo installer (owner request 2026-10-25): pin, layout, notices,
Games menu entry and Setup CD assignment. Host only."""
# SPDX-License-Identifier: GPL-2.0-only
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SHA = "6af96cb1b5c97535c9923d8855ca85ec21b21b65b48738b2c5e674e17c65835d"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


q3a = load("q3a_demo_build", ROOT / "scripts/q3a_demo.py")
cd = load("q3a_demo_cd", ROOT / "scripts/build_cd.py")
dos = load("q3a_demo_dos", ROOT / "scripts/build_dos_image.py")
PACKAGE = ROOT / "third_party/games/q3a-demo-installer"


class Q3ADemoTests(unittest.TestCase):
    def test_pin(self):
        record = q3a.verify()
        self.assertEqual((record["archive"], record["size"], record["sha256"]), ("Q3ADemo.exe", 48907862, SHA))
        data = (PACKAGE / "Q3ADemo.exe").read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(), SHA)
        self.assertEqual(data[:2], b"MZ")
        self.assertIn("ftp.gwdg.de/pub/misc/ftp.idsoftware.com/idstuff/quake3/win32/Q3ADemo.exe", record["sources"][0])
        self.assertTrue(record["original_location"].startswith("ftp://ftp.idsoftware.com/"))

    def test_pin_detects_change(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as tmp:
            folder = Path(tmp) / "third_party/games/q3a-demo-installer"
            folder.mkdir(parents=True)
            for name in ("UPSTREAM.json", "Q3A_EULA.txt", "README-linux-demo.txt"):
                (folder / name).write_bytes((PACKAGE / name).read_bytes())
            (folder / "Q3ADemo.exe").write_bytes(b"MZ not the installer")
            with self.assertRaises(SystemExit):
                q3a.verify(tmp)

    def test_layout_and_licences(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as tmp:
            sources, directories, metadata, free = q3a.payloads(tmp)
            self.assertEqual(set(sources), {
                "/Games/Quake III Arena Demo Setup/Q3ADemo.exe",
                "/system/licenses/q3a-demo-installer-UPSTREAM.json",
                "/system/licenses/q3a-demo-installer-Q3A_EULA.txt",
                "/system/licenses/q3a-demo-installer-README-linux-demo.txt"})
            self.assertEqual(free, {SHA})
            self.assertEqual(directories, {"/Games", "/Games/Quake III Arena Demo Setup"})
            self.assertFalse([p for p in list(sources) + list(directories) if p.lower().startswith("/windows/start menu")])
            self.assertEqual(metadata["q3a_demo"]["sha256"], SHA)
            for source in sources.values():
                self.assertTrue(Path(source).is_file())
            eula = Path(sources["/system/licenses/q3a-demo-installer-Q3A_EULA.txt"]).read_text(errors="replace")
            self.assertIn("LIMITED USE SOFTWARE LICENSE AGREEMENT", eula)
            # The local-payload guard accepts the pinned installer and nothing else is reserved.
            dos.refuse_local_payloads({p: q3a.sha256(s) for p, s in sources.items()}, free=free)

    def test_games_menu_entry(self):
        # Programs > Games lists a built-in record (shell.c programs[], Win32 launch path), not a .lnk in a Start Menu tree.
        record = json.loads((PACKAGE / "UPSTREAM.json").read_text())["install"]
        entry = record["menu_entry"]
        self.assertEqual((entry["label"], entry["target"]), (q3a.LABEL, q3a.TARGET))
        self.assertEqual(record["windows_path"], q3a.TARGET)
        self.assertFalse(hasattr(q3a, "SHORTCUT_PATH"))
        shell = (ROOT / "apps/desktop/shell.c").read_text()
        row = [l for l in shell.splitlines() if l.strip().startswith('{"q3demo"')]
        self.assertEqual(len(row), 1)
        self.assertIn('"Install Quake III Arena Demo"', row[0])
        self.assertIn('"games"', row[0])
        self.assertIn(q3a.TARGET.replace("\\", "\\\\"), row[0])

    def test_cd_component(self):
        config = json.loads((ROOT / "config/cd-components.json").read_text())
        row = next(c for c in config["components"] if c["id"] == "q3a-demo")
        self.assertEqual((row["class"], row["package"]), ("bundled-shareware", "q3a-demo-installer"))
        self.assertTrue(row["conditions"].strip())
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as tmp:
            sources, _, _, _ = q3a.payloads(tmp)
            games = ["q3a-demo-installer"]
            assignment, present, _ = cd.eligibility(config, sorted(sources) + ["/Games/CIUKIGAM.IMG", "/system/VMM.ELF"], games)
            for path in sources:
                self.assertEqual(assignment[path], "q3a-demo", path)
            self.assertEqual(assignment["/Games/CIUKIGAM.IMG"], "games")
            self.assertIn("q3a-demo", present)


if __name__ == "__main__":
    unittest.main()
