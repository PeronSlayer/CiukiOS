"""Quake III Arena demo installer (owner decision 2026-10-25): pin, desktop folder layout,
notices, no built-in Start-menu entry and Setup CD assignment. Host only."""
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
image = load("q3a_demo_image", ROOT / "scripts/build_image.py")
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
        # sec-user-02 (user-security-contract.md 8.3, A1-5): the installer and its
        # documents live in the protected store; the desktop folder holds only
        # shortcuts to the exact store files.
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as tmp:
            sources, directories, metadata, free = q3a.payloads(tmp)
            self.assertEqual(set(sources), {
                "/Programs/Installers/Quake III Arena Demo/Q3ADemo.exe",
                "/Programs/Installers/Quake III Arena Demo/Q3A_EULA.txt",
                "/Programs/Installers/Quake III Arena Demo/README.txt",
                "/Documents/Desktop/Quake III Arena Demo/Install Quake III Arena Demo.lnk",
                "/Documents/Desktop/Quake III Arena Demo/Licence Agreement.lnk",
                "/Documents/Desktop/Quake III Arena Demo/Read Me.lnk",
                "/system/licenses/q3a-demo-installer-UPSTREAM.json",
                "/system/licenses/q3a-demo-installer-Q3A_EULA.txt",
                "/system/licenses/q3a-demo-installer-README-linux-demo.txt"})
            self.assertEqual(free, {SHA})
            self.assertEqual(directories, {"/Programs", "/Programs/Installers", "/Programs/Installers/Quake III Arena Demo",
                                           "/Documents", "/Documents/Desktop", "/Documents/Desktop/Quake III Arena Demo"})
            self.assertEqual(q3a.sha256(sources["/Programs/Installers/Quake III Arena Demo/Q3ADemo.exe"]), SHA)
            # No executable or script in shared /Documents (the builder's static check).
            image.refuse_shared_executables(sources)
            for name in q3a.LINKS:
                data = Path(sources[f"{q3a.DESKTOP_FOLDER}/{name}"]).read_bytes()
                self.assertEqual(data[:8], b"CIUKLNK3")
                show, icon, *lengths, hotkey = struct.unpack_from("<Ii5II", data, 8)
                fields, at = [], 40
                for n in lengths:
                    fields.append(data[at:at + n].decode()); at += n
                self.assertEqual((show, icon, hotkey, at), (1, 0, 0, len(data)))
                label, target, arguments, directory, icon_path = fields
                self.assertEqual((label, arguments, icon_path), (q3a.LINKS[name][0], "", ""))
                self.assertEqual(target, "C:\\Programs\\Installers\\Quake III Arena Demo\\" + q3a.LINKS[name][1])
                self.assertEqual(directory, "C:\\Programs\\Installers\\Quake III Arena Demo")
                self.assertEqual(metadata["q3a_demo"]["links"][name]["target"], target)
            self.assertFalse([p for p in list(sources) + list(directories) if p.lower().startswith("/windows/start menu")])
            self.assertEqual(metadata["q3a_demo"]["sha256"], SHA)
            for source in sources.values():
                self.assertTrue(Path(source).is_file())
            eula = Path(sources["/system/licenses/q3a-demo-installer-Q3A_EULA.txt"]).read_text(errors="replace")
            self.assertIn("LIMITED USE SOFTWARE LICENSE AGREEMENT", eula)
            # The local-payload guard accepts the pinned installer and nothing else is reserved.
            dos.refuse_local_payloads({p: q3a.sha256(s) for p, s in sources.items()}, free=free)

    def test_desktop_folder_no_menu_entry(self):
        # The installer sits in a desktop folder; Start has no built-in record for it (the installer's own shortcuts merge in).
        record = json.loads((PACKAGE / "UPSTREAM.json").read_text())["install"]
        self.assertEqual(record["windows_path"], q3a.TARGET)
        self.assertNotIn("menu_entry", record)
        self.assertEqual(q3a.INSTALLER_PATH, q3a.STORE_FOLDER + "/Q3ADemo.exe")
        self.assertEqual(q3a.TARGET, "C:\\Programs\\Installers\\Quake III Arena Demo\\Q3ADemo.exe")
        self.assertFalse(hasattr(q3a, "LABEL"))
        for name in ("apps/desktop/shell.c", "apps/libdeskui/icon_map.c"):
            self.assertNotIn("q3demo", (ROOT / name).read_text(), name)
        self.assertNotIn("Install Quake III", (ROOT / "apps/desktop/shell.c").read_text())

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
            self.assertEqual(assignment["/Programs/Installers/Quake III Arena Demo/Q3ADemo.exe"], "q3a-demo")
            self.assertEqual(assignment["/Games/CIUKIGAM.IMG"], "games")
            self.assertIn("q3a-demo", present)


if __name__ == "__main__":
    unittest.main()
