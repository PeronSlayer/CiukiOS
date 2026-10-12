"""Bundled shareware games disk (owner decision 2026-10-12): vendored packages,
their pins and licence documents, the DCL decoder, the disk and the guard that
keeps the owner's registered game data out of it. Host only; never boots QEMU."""
# SPDX-License-Identifier: GPL-2.0-only
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PACKAGES = ("wolf3d-shareware", "doom-shareware")


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


games = load("games_disk_build", ROOT / "scripts/build_games_disk.py")
dos = load("games_dos_build", ROOT / "scripts/build_dos_image.py")
DOSBOOT = ROOT / "build/f0/dos/DOSBOOT.IMG"
ENV = dict(os.environ, MTOOLS_SKIP_CHECK="1", LC_ALL="C")


def sha(data):
    return hashlib.sha256(data).hexdigest()


class PackageTests(unittest.TestCase):
    def test_archives_match_pins_and_two_references(self):
        for package in PACKAGES:
            with self.subTest(package=package):
                record = games.upstream(package)
                data = games.archive(package, record)
                self.assertEqual(len(data), record["size"])
                digests = {"sha256": sha(data), "sha1": hashlib.sha1(data).hexdigest(), "md5": hashlib.md5(data).hexdigest()}
                self.assertEqual({k: record[k] for k in digests}, digests)
                # At least two independent published references, each agreeing on every hash it gives.
                published = [r for r in record["hash_references"] if any(k in r for k in digests)]
                self.assertGreaterEqual(len(published), 2)
                for reference in published:
                    self.assertTrue(reference["url"].startswith("https://"))
                    for k in digests:
                        if k in reference:
                            self.assertEqual(reference[k], digests[k], reference["reference"])
                self.assertTrue(any("sha256" in r for r in published))
                self.assertEqual(record["redistribution"], "free (shareware, unmodified package)")
                self.assertEqual(len(record["sources"]), 2)
                # A changed archive is refused before anything is unpacked.
                root = Path(self.tmp) / package
                folder = root / games.GAMES / package
                shutil.copytree(ROOT / games.GAMES / package, folder)
                (folder / record["archive"]).write_bytes(data[:-1] + bytes([data[-1] ^ 1]))
                with self.assertRaisesRegex(ValueError, "differs from the pinned"):
                    games.archive(package, record, root)
                with self.assertRaisesRegex(ValueError, "differs from the pinned"):
                    games.package_files(package, root)

    def setUp(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        self._temp = tempfile.TemporaryDirectory(prefix="games-", dir=parent)
        self.addCleanup(self._temp.cleanup)
        self.tmp = self._temp.name

    def test_members_pinned_and_independently_known(self):
        wolf, wolf_files, wolf_docs = games.package_files("wolf3d-shareware")
        doom, doom_files, doom_docs = games.package_files("doom-shareware")
        md5 = {path.rsplit("/", 1)[1]: hashlib.md5(data).hexdigest() for path, data, _ in wolf_files + doom_files}
        # game-data-packager data/wolf3d.yaml, v1.4 shareware; doomwiki.org Doom files, 1.9 shareware.
        self.assertEqual({k: md5[k] for k in ("AUDIOHED.WL1", "AUDIOT.WL1", "GAMEMAPS.WL1", "MAPHEAD.WL1",
                                               "VGADICT.WL1", "VGAGRAPH.WL1", "VGAHEAD.WL1", "VSWAP.WL1")},
                         {"AUDIOHED.WL1": "58aa1b9892d5adfa725fab343d9446f8", "AUDIOT.WL1": "4b6109e957b584e4ad7f376961f3887e",
                          "GAMEMAPS.WL1": "30fecd7cce6bc70402651ec922d2da3d", "MAPHEAD.WL1": "7b6dd4e55c33c33a41d1600be5df3228",
                          "VGADICT.WL1": "76a6128f3c0dd9b77939ce8313992746", "VGAGRAPH.WL1": "74decb641b1a4faed173e10ab744bff0",
                          "VGAHEAD.WL1": "61bf1616e78367853c91f2c04e2c1cb7", "VSWAP.WL1": "6efa079414b817c97db779cecfb081c9"})
        self.assertEqual(md5["DOOM.EXE"], "e2382b7dc47ae2433d26b6e6bc312999")
        self.assertEqual(md5["DOOM1.WAD"], "f0cefca49926d00903cf57551d901abe")
        # VENDOR.DOC [3][A][1]: every file of the release is installed unmodified.
        vendor = dict(wolf_docs)["VENDOR.DOC"].decode("cp437")
        names = {path.rsplit("/", 1)[1] for path, _, _ in wolf_files}
        for line in vendor.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[0] == "■":
                self.assertIn(f"{parts[1]}.{parts[2]}".upper(), names)
        self.assertEqual(set(wolf["install"]["excluded"]), set())
        self.assertEqual(set(doom["install"]["excluded"]), {f"DMFAQ66{c}.TXT" for c in "ABCD"})
        self.assertNotIn("DOS4GW.EXE", {path.rsplit("/", 1)[1] for path, _, _ in doom_files})   # bound into DOOM.EXE
        exe = next(data for path, data, _ in doom_files if path == "/DOOM/DOOM.EXE")
        self.assertEqual(games._fixtures().extender(exe), "dos4gw-professional-1.95-bound")

    def test_licence_documents_and_verbatim_clauses(self):
        for package in PACKAGES:
            with self.subTest(package=package):
                record, _, documents = games.package_files(package)
                self.assertEqual({n for n, _ in documents}, {f["name"] for f in record["license_files"]})
                for name, data in documents:
                    self.assertEqual((ROOT / games.GAMES / package / name).read_bytes(), data)
                texts = [data.decode("cp437").replace("\r\n", "\n") for _, data in documents]
                if package == "doom-shareware":
                    texts.append(self.endoom())
                    _, inner = games.unpack(record, games.archive(package, record))
                    texts.append(inner["DMFAQ66A.TXT"].decode("cp437").replace("\r\n", "\n"))
                for clause in record["license_clauses"]:
                    if "url" in clause:          # quoted from an external record (Carmack, Debian)
                        continue
                    self.assertTrue(any(clause["text"] in t for t in texts), clause["source"])
                self.assertIn("bundling", record)
        wolf = games.upstream("wolf3d-shareware")
        self.assertTrue(any("[C] ADDITIONAL CONDITIONS FOR BUNDLES" in c["text"] for c in wolf["license_clauses"]))
        self.assertIn("PRIOR signed written permission", wolf["bundling"])
        attributes = (ROOT / games.GAMES / ".gitattributes").read_text()
        self.assertIn("* -text", attributes)

    def endoom(self):
        record = games.upstream("doom-shareware")
        _, inner = games.unpack(record, games.archive("doom-shareware", record))
        wad = inner["DOOM1.WAD"]
        count, table = struct.unpack_from("<II", wad, 4)
        for i in range(count):
            at, size, name = struct.unpack_from("<II8s", wad, table + 16 * i)
            if name.rstrip(b"\0") == b"ENDOOM":
                cells = wad[at:at + size]
                return "\n".join("".join(chr(cells[(r * 80 + c) * 2]) for c in range(80)).rstrip() for r in range(25))
        self.fail("no ENDOOM lump")


class DecoderTests(unittest.TestCase):
    def test_dcl_explode(self):
        # Ben Rudiak-Gould's example as corrected in blast.c's header comment.
        self.assertEqual(games.dcl_explode(bytes.fromhex("00048224258f807f")), (b"AIAIAIAIAIAIA", 8))
        for bad, reason in ((bytes.fromhex("0204"), "bad header"), (bytes.fromhex("0007"), "bad header"),
                            (bytes.fromhex("000482"), "out of input"), (b"", "out of input")):
            with self.assertRaisesRegex(ValueError, reason):
                games.dcl_explode(bad)

    def test_shr_container(self):
        member = bytes.fromhex("00048224258f807f")
        entry = bytearray(games.SHR_ENTRY)
        entry[:7] = b"AI.TXT\0"
        struct.pack_into("<I", entry, games.SHR_SIZE, len(member))
        data = bytes(games.SHR_HEADER) + bytes(entry) + member
        self.assertEqual(games.shr_members(data), {"AI.TXT": b"AIAIAIAIAIAIA"})
        with self.assertRaisesRegex(ValueError, "truncated"):
            games.shr_members(data[:-1])
        bad = bytearray(data)
        bad[games.SHR_HEADER:games.SHR_HEADER + 7] = b"../X\0\0\0"
        with self.assertRaisesRegex(ValueError, "bad member name"):
            games.shr_members(bytes(bad))


class GuardTests(unittest.TestCase):
    """The owner's registered game data (DOOM.WAD, *.WL6) never enters the games disk or the image."""

    def setUp(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        self._temp = tempfile.TemporaryDirectory(prefix="games-guard-", dir=parent)
        self.addCleanup(self._temp.cleanup)
        self.root = Path(self._temp.name)
        shutil.copytree(ROOT / games.GAMES, self.root / games.GAMES)
        # Fake local registered copies, as the owner keeps them (ignored by git).
        record = games.upstream("doom-shareware")
        _, inner = games.unpack(record, games.archive("doom-shareware", record))
        self.local = {"third_party/Doom/DOOM.WAD": b"IWAD" + b"registered doom" * 64,
                      "third_party/Doom/DOOM.EXE": b"MZ" + b"registered exe" * 64,
                      "third_party/Doom/SETUP.EXE": inner["SETUP.EXE"],          # identical in both releases
                      "third_party/WOLF3D/VSWAP.WL6": b"registered wolf" * 64,
                      "third_party/WOLF3D/WOLF3D.EXE": b"MZ" + b"registered wolf exe" * 64}
        for path, data in self.local.items():
            (self.root / path).parent.mkdir(parents=True, exist_ok=True)
            (self.root / path).write_bytes(data)

    def test_registered_names_never_pass(self):
        free = games.free_hashes(self.root)
        for name in ("/DOOM/DOOM.WAD", "/WOLF3D/VSWAP.WL6", "/GAMES/DOOM2.WAD", "/X/SPEAR.SOD"):
            with self.subTest(name=name), self.assertRaisesRegex(SystemExit, "registered game data"):
                dos.refuse_local_payloads({name: "0" * 64}, self.root, free=free | {"0" * 64})
            with self.assertRaisesRegex(ValueError, "registered game data"):
                games.refuse_registered([(name, b"shareware?" * 10, {"package": "doom-shareware"})], self.root)

    def test_local_files_need_a_pin(self):
        free = games.free_hashes(self.root)
        exe = sha(self.local["third_party/Doom/DOOM.EXE"])
        with self.assertRaisesRegex(SystemExit, "local commercial payload"):
            dos.refuse_local_payloads({"/DOOM/GAME.BIN": exe}, self.root, free=free)
        with self.assertRaisesRegex(SystemExit, "local commercial payload"):
            games.refuse_registered([("/DOOM/GAME.BIN", self.local["third_party/Doom/DOOM.EXE"], {"role": "dos_system"})], self.root)
        with self.assertRaisesRegex(ValueError, "unpinned shareware member"):
            games.refuse_registered([("/DOOM/EXTRA.EXE", b"MZ" * 40, {"package": "doom-shareware"})], self.root)
        # A pinned shareware member may equal a local file (SETUP.EXE) or use a reserved name (DOOM.EXE).
        setup = sha(self.local["third_party/Doom/SETUP.EXE"])
        dos.refuse_local_payloads({"/DOOM/SETUP.EXE": setup}, self.root, free=free)
        with self.assertRaisesRegex(SystemExit, "local commercial payload"):
            dos.refuse_local_payloads({"/DOOM/SETUP.EXE": setup}, self.root)
        pinned = {f["name"]: f["sha256"] for f in games.upstream("doom-shareware", self.root)["install"]["files"]}
        dos.refuse_local_payloads({"/DOOM/DOOM.EXE": pinned["DOOM.EXE"]}, self.root, free=free)
        with self.assertRaisesRegex(SystemExit, "reserved name"):
            dos.refuse_local_payloads({"/DOOM/DOOM.EXE": pinned["DOOM.EXE"]}, self.root)

    def test_pins_cannot_admit_registered_data(self):
        path = self.root / games.GAMES / "doom-shareware/UPSTREAM.json"
        record = json.loads(path.read_text())
        record["install"]["files"].append({"name": "DOOM.WAD", "size": 1, "sha256": "0" * 64, "role": "data"})
        path.write_text(json.dumps(record))
        with self.assertRaisesRegex(ValueError, "pinned but absent"):
            games.package_files("doom-shareware", self.root)

    @unittest.skipUnless(DOSBOOT.is_file() and DOSBOOT.with_suffix(".json").is_file(), "run python3 scripts/build_dos_image.py first")
    def test_disk_holds_only_shareware(self):
        out = self.root / "out" / games.NAME
        image, manifest = games.build(out, DOSBOOT, self.root)
        listing = subprocess.run(["mdir", "-/", "-b", "-i", str(image), "::/"], check=True, capture_output=True,
                                 text=True, env=ENV).stdout.upper().split()
        for name in listing:
            self.assertIsNone(dos.REGISTERED_DATA.fullmatch(name.rsplit("/", 1)[-1]), name)
        local = {sha(data) for path, data in self.local.items() if not path.endswith("SETUP.EXE")}
        on_disk = set()
        for name in listing:
            if name.endswith("/"):
                continue
            data = subprocess.run(["mcopy", "-n", "-i", str(image), "::" + name.split("::", 1)[-1], "-"],
                                  check=True, capture_output=True, env=ENV).stdout
            on_disk.add(sha(data))
        self.assertFalse(on_disk & local)
        self.assertEqual(len(on_disk), len({f["sha256"] for f in manifest["files"]}))
        self.assertTrue(all(f["redistribution"] == "free" for f in manifest["files"] if f["role"] != "dos_system"))


@unittest.skipUnless(DOSBOOT.is_file() and DOSBOOT.with_suffix(".json").is_file(), "run python3 scripts/build_dos_image.py first")
class DiskTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        cls._temp = tempfile.TemporaryDirectory(prefix="games-disk-", dir=parent)
        cls.out = Path(cls._temp.name) / games.NAME
        cls.image, cls.manifest = games.build(cls.out, DOSBOOT)

    @classmethod
    def tearDownClass(cls):
        cls._temp.cleanup()

    def test_manifest_hashes_every_file(self):
        m = self.manifest
        self.assertEqual(games.validate(self.out), m)
        self.assertEqual(m["redistribution"], "free")
        self.assertEqual([p["name"] for p in m["packages"]], list(PACKAGES))
        game = [f for f in m["files"] if f["role"] != "dos_system"]
        self.assertEqual({f["path"].rsplit("/", 1)[0] for f in game}, {"/WOLF3D", "/DOOM"})
        self.assertTrue(all(f["redistribution"] == "free" and len(f["sha256"]) == 64 for f in game))
        self.assertIn("/DOOM/CIUKI.CFG", {f["path"] for f in game})
        self.assertEqual({f["path"] for f in game if f["role"] == "executable"}, {"/WOLF3D/WOLF3D.EXE", "/DOOM/DOOM.EXE"})
        system = {f["path"]: f["sha256"] for f in m["files"] if f["role"] == "dos_system"}
        dos_manifest = json.loads(DOSBOOT.with_suffix(".json").read_text())
        self.assertEqual(system, {p["path"]: p["sha256"] for p in dos_manifest["payloads"]})
        self.assertEqual(m["dos_image_sha256"], dos_manifest["image_sha256"])
        for record in m["licenses"]:
            self.assertEqual(sha((ROOT / record["path"]).read_bytes()), record["sha256"])

    def test_cut_image_reads_back_and_extends_to_the_logical_image(self):
        m = self.manifest
        size = self.out.stat().st_size
        self.assertEqual(size, m["size"])
        self.assertEqual(size % 512, 0)
        self.assertLess(size, 16 * 1024 * 1024)                 # 128 MiB volume, a few MiB used
        self.assertEqual(m["logical_size"], 128 * 1024 * 1024)
        for f in m["files"]:
            data = subprocess.run(["mcopy", "-n", "-i", str(self.out), "::" + f["path"], "-"], check=True,
                                  capture_output=True, env=ENV).stdout
            self.assertEqual(sha(data), f["sha256"], f["path"])
        full = self.out.with_name("full.img")
        shutil.copyfile(self.out, full)
        with full.open("r+b") as stream:
            stream.truncate(m["logical_size"])                  # sparse zero tail: the kernel's view
        self.assertEqual(games._fixtures().sha(full), m["logical_sha256"])
        checked = subprocess.run(["fsck.fat", "-n", str(full)], capture_output=True, text=True)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        self.assertGreaterEqual(size // 512, 361)                # DOSVM_BASE_MIN_SECTORS
        full.unlink()

    def test_deterministic(self):
        again = self.out.with_name("again.img")
        _, manifest = games.build(again, DOSBOOT)
        self.assertEqual(manifest["image_sha256"], self.manifest["image_sha256"])
        self.assertEqual(manifest["logical_sha256"], self.manifest["logical_sha256"])


if __name__ == "__main__":
    unittest.main()
