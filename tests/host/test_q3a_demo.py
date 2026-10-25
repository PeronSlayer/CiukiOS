"""Quake III Arena demo installer (owner request 2026-10-25): pin, layout, notices,
shortcut bytes, Games menu entry and Setup CD assignment. Host only."""
# SPDX-License-Identifier: GPL-2.0-only
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
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
                "/WINDOWS/Start Menu/Programs/Games/Install Quake III Arena Demo.lnk",
                "/system/licenses/q3a-demo-installer-UPSTREAM.json",
                "/system/licenses/q3a-demo-installer-Q3A_EULA.txt",
                "/system/licenses/q3a-demo-installer-README-linux-demo.txt"})
            self.assertEqual(free, {SHA})
            self.assertTrue({"/Games", "/Games/Quake III Arena Demo Setup", "/WINDOWS/Start Menu/Programs/Games"} <= directories)
            self.assertEqual(metadata["q3a_demo"]["sha256"], SHA)
            for source in sources.values():
                self.assertTrue(Path(source).is_file())
            eula = Path(sources["/system/licenses/q3a-demo-installer-Q3A_EULA.txt"]).read_text(errors="replace")
            self.assertIn("LIMITED USE SOFTWARE LICENSE AGREEMENT", eula)
            # The local-payload guard accepts the pinned installer and nothing else is reserved.
            dos.refuse_local_payloads({p: q3a.sha256(s) for p, s in sources.items()}, free=free)

    def test_shortcut_bytes(self):
        data = q3a.encode_link(q3a.LABEL, q3a.TARGET, directory=q3a.WORKDIR)
        self.assertEqual(data[:8], b"CIUKLNK3")
        show, icon = struct.unpack_from("<Ii", data, 8)
        lengths = struct.unpack_from("<5I", data, 16)
        self.assertEqual((show, icon, struct.unpack_from("<I", data, 36)[0]), (1, 0, 0))
        self.assertEqual(40 + sum(lengths), len(data))
        fields, at = [], 40
        for n in lengths:
            fields.append(data[at:at + n].decode()); at += n
        self.assertEqual(fields, ["Install Quake III Arena Demo", "C:\\Games\\Quake III Arena Demo Setup\\Q3ADemo.exe", "",
                                  "C:\\Games\\Quake III Arena Demo Setup", ""])
        record = json.loads((PACKAGE / "UPSTREAM.json").read_text())["install"]
        self.assertEqual(record["windows_path"], fields[1])
        self.assertEqual(record["shortcut"]["label"], fields[0])

    def test_games_menu_folder(self):
        # Programs > Games is the folder the Start menu scan lists.
        self.assertEqual(Path(q3a.SHORTCUT_PATH).parent.as_posix(), "/WINDOWS/Start Menu/Programs/Games")
        self.assertTrue(q3a.SHORTCUT_PATH.endswith(q3a.LABEL + ".lnk"))

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
