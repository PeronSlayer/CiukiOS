"""T0 payload provenance and real mtools read-back, without building/booting an image."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


image = load("image_payload_build", ROOT / "scripts/build_image.py")
dos = load("image_dos_build", ROOT / "scripts/build_dos_image.py")
lua = load("lua_payload_build", ROOT / "apps/lua/build_lua.py")


class ImagePayloadTests(unittest.TestCase):
    def setUp(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="payloads-", dir=parent)
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)

    @unittest.skipUnless(shutil.which("mkfs.fat") and shutil.which("fsck.fat"), "requires FAT tools")
    def test_raw_blackbox_reservation(self):
        import re
        loader = (ROOT / "src/boot/ciukldr/disk.inc").read_text()
        reserved = int(re.search(r"%define CIUKI_FAT_RESERVED_SECTORS (\d+)", loader)[1])
        self.assertEqual(reserved, image.FAT_RESERVED)
        self.assertIn("cmp word [sector+14], CIUKI_FAT_RESERVED_SECTORS", loader)
        self.assertEqual(loader.count("add eax, CIUKI_FAT_RESERVED_SECTORS"), 2)
        if shutil.which("nasm"):
            binary = self.work / "loader.bin"
            subprocess.run(["nasm", "-f", "bin", "-I", str(ROOT) + "/",
                            str(ROOT / "src/boot/ciukldr.asm"), "-o", str(binary)], check=True, cwd=ROOT)
            prepared = image.prepare_loader(binary.read_bytes())
            self.assertEqual(prepared[:4], b"CLDR")
            self.assertEqual(len(prepared) % 512, 0)
        volume = self.work / "raw.fat"
        with volume.open("wb") as stream:
            stream.truncate(40 * 1024 * 1024)
        subprocess.run(["mkfs.fat", "--invariant", "-a", "-F", "32", "-s", "1",
                        "-R", str(image.FAT_RESERVED), "-b", "6", str(volume)], check=True)
        with volume.open("rb") as stream:
            before = stream.read(image.FAT_RESERVED * 512)
        image.initialize_blackbox(volume)
        with volume.open("rb") as stream:
            after = stream.read(image.FAT_RESERVED * 512)
        import struct, zlib
        self.assertEqual(before[:8*512], after[:8*512])
        self.assertEqual(before[9*512:32*512], after[9*512:32*512])
        desc = after[8*512:9*512]
        self.assertEqual(struct.unpack_from("<4sIII", desc), (b"CBB1", 1, 32, 128))
        self.assertEqual(struct.unpack_from("<I", desc, 508)[0], zlib.crc32(desc[:508]))
        self.assertEqual(after[32*512:], bytes(65536))
        subprocess.run(["fsck.fat", "-n", str(volume)], check=True)
        with volume.open("r+b") as stream:
            stream.seek(14); stream.write(struct.pack("<H", 32))
        with self.assertRaisesRegex(ValueError, "blackbox reserved geometry"):
            image.initialize_blackbox(volume)

    def test_registry_seed_and_payload_readback(self):
        with patch.object(image, "git_identity", return_value=("abc123", "clean")):
            sources, directories = image.registry_payloads(self.work / "registry")
        self.assertEqual(set(sources), {"/System/Registry/System.reg"})
        self.assertEqual(directories, {"/System", "/System/Registry"})
        text = sources["/System/Registry/System.reg"].read_text(encoding="utf-8")
        self.assertIn("; ciuki-registry generation=1", text)
        self.assertIn("[System\\Version]", text)
        self.assertIn('name = "CiukiOS"', text)
        self.assertIn('version = ' + json.dumps(image.read_version()["display"]), text)
        self.assertIn('build = "abc123"', text)
        self.assertIn("[System\\Identity]", text)
        self.assertIn('Tagline = "Classic computing, one woof at a time."', text)
        records = image.payload_records(sources)
        self.assertEqual(records[0]["size"], len(text.encode("utf-8")))
        self.assertEqual(records[0]["sha256"], hashlib.sha256(text.encode("utf-8")).hexdigest())
        if all(shutil.which(t) for t in ("mkfs.fat", "mmd", "mcopy", "mdir")):
            volume = self.work / "registry.fat"
            with volume.open("wb") as stream:
                stream.truncate(8 * 1024 * 1024)
            subprocess.run(["mkfs.fat", str(volume)], check=True, stdout=subprocess.DEVNULL)
            image.populate_payloads(volume, sources, directories)
            with patch.object(image, "PART_LBA", 0):
                image.check_payloads(volume, records, directories)

    def test_records_and_manifest_include_every_payload(self):
        a, b = self.work / "binary", self.work / "empty"
        a.write_bytes(bytes(range(256)) * 256)
        b.write_bytes(b"")
        sources = {"/system/tests/long-name.lua": a, "/Programs/empty": b}
        records = image.payload_records(sources)
        self.assertEqual([p["path"] for p in records], sorted(sources))
        self.assertEqual(records[1], {"path": "/system/tests/long-name.lua",
                                     "sha256": hashlib.sha256(a.read_bytes()).hexdigest(),
                                     "size": 65536, "attributes": 32})
        disk = self.work / "disk.img"
        # The kernel build records its UTC epoch and binds it to the ELF hash;
        # the image builder consumes that record rather than choosing a new time.
        clock_record = {"utc_epoch": int(time.time()), "kernel_sha256": image.sha256(a)}
        (self.work / "build-clock.json").write_text(json.dumps(clock_record, indent=2) + "\n")
        with patch.object(image, "git_identity", return_value=("unknown", "unknown")):
            image.write_manifest(disk, a, "image-hash", records, {"sdk_manifest_sha256": "sdk-hash"},
                                 directories={"/Documents", "/Documents/Empty"}, boot_cfg=b"safe=0 serial=1  \r\n")
        manifest = json.loads((self.work / "build-manifest.json").read_text())
        self.assertEqual(manifest["payloads"], records)
        self.assertEqual(manifest["boot_cfg_template"], "safe=0 serial=1  \r\n")
        self.assertEqual(manifest["boot_cfg_sha256"], hashlib.sha256(b"safe=0 serial=1  \r\n").hexdigest())
        self.assertEqual(manifest["directories"], [{"path": p, "attributes": 16} for p in ("/Documents", "/Documents/Empty")])
        self.assertEqual(manifest["sdk_manifest_sha256"], "sdk-hash")
        self.assertEqual(manifest["image_sha256"], "image-hash")
        self.assertEqual(manifest["build_epoch"], clock_record["utc_epoch"])
        self.assertEqual(manifest["kernel_sha256"], clock_record["kernel_sha256"])
        self.assertEqual(image.IMAGE_BYTES, 512 * 1024 * 1024)
        self.assertEqual(image.PART_LBA, 2048)

    @unittest.skipUnless(all(shutil.which(t) for t in ("mkfs.fat", "mmd", "mcopy", "mdir", "mrd")),
                         "requires mtools and mkfs.fat")
    def test_real_fat_readback_rejects_changed_missing_and_wrong_size(self):
        # Small host FAT fixture, not a canonical image build or a copied image.
        volume = self.work / "fixture.fat"
        with volume.open("wb") as stream:
            stream.truncate(8 * 1024 * 1024)
        subprocess.run(["mkfs.fat", str(volume)], check=True, stdout=subprocess.DEVNULL)
        source = self.work / "data"
        source.write_bytes(bytes(range(256)) * 256 + b"\0\r\n\xff")
        empty = self.work / "empty"
        empty.write_bytes(b"")
        directories = {"/Programs", "/tmp", "/Documents", "/System", "/Games", "/system/tests",
                       "/system/tests/lua-5.4.8-tests", "/system/tests/lua-5.4.8-tests/libs",
                       "/system/tests/lua-5.4.8-tests/libs/P1"}
        sources = {"/system/tests/lua-5.4.8-tests/long-name.lua": source, "/Programs/empty": empty}
        image.populate_payloads(volume, sources, directories)
        records = image.payload_records(sources)
        with patch.object(image, "PART_LBA", 0):
            image.check_payloads(volume, records, directories)
            # Preserve disk spelling while lowercase System paths and the
            # kernel's uppercase GAMES path still resolve.
            image.check_payloads(volume, records, {"/programs", "/documents", "/system", "/GAMES"})
            wrong_size = [dict(records[1], size=records[1]["size"] + 1)]
            with self.assertRaisesRegex(ValueError, "payload size"):
                image.check_payloads(volume, wrong_size, directories)
            source.write_bytes(b"X" * records[1]["size"])
            subprocess.run(["mcopy", "-o", "-i", str(volume), str(source),
                            "::" + records[1]["path"]], check=True)
            with self.assertRaisesRegex(ValueError, "payload SHA-256"):
                image.check_payloads(volume, records, directories)
            with self.assertRaises(subprocess.CalledProcessError):
                image.check_payloads(volume, [{"path": "/Programs/missing", "size": 0,
                                              "sha256": records[0]["sha256"]}], directories)
            # Case-insensitive existence is insufficient for the display contract.
            subprocess.run(["mrd", "-i", str(volume), "::/Games"], check=True)
            subprocess.run(["mmd", "-i", str(volume), "::/GAMES"], check=True)
            with self.assertRaisesRegex(ValueError, "directory display name: Games"):
                image.check_payloads(volume, [], directories)

    @unittest.skipUnless((ROOT / "build/f0/dos/UNIFIED.IMG").is_file() and
                         all(shutil.which(t) for t in ("mkfs.fat", "mmd", "mcopy", "mdir")),
                         "run python3 scripts/build_dos_image.py first; requires mtools")
    def test_games_disk_payloads(self):
        """Owner decision 2026-10-12: the shareware games disk at /Games/CIUKIGAM.IMG with the
        packages' licence documents and provenance; the guard accepts only pinned shareware files."""
        sources, directories, metadata, free = image.games_payloads(ROOT / "build/f0/dos/UNIFIED.IMG",
                                                                    self.work / "games/CIUKIGAM.IMG")
        licences = {f"/system/licenses/{p}-{n}" for p, n in (("wolf3d-shareware", "VENDOR.DOC"),
                    ("wolf3d-shareware", "FILE_ID.DIZ"), ("wolf3d-shareware", "UPSTREAM.json"),
                    ("doom-shareware", "README.TXT"), ("doom-shareware", "DOOMS_19.DAT"), ("doom-shareware", "UPSTREAM.json"), ("quake-shareware", "SLICNSE.TXT"),
                    ("quake-shareware", "LICINFO.TXT"), ("quake-shareware", "UPSTREAM.json"))}
        self.assertEqual(set(sources), {"/Games/CIUKIGAM.IMG"} | licences)
        self.assertTrue({"/Games", "/system/licenses"} <= directories)
        self.assertNotIn("checkers", metadata)
        records = image.payload_records(sources)
        disk = next(r for r in records if r["path"] == "/Games/CIUKIGAM.IMG")
        self.assertEqual((disk["sha256"], disk["size"]), (metadata["image_sha256"], metadata["size"]))
        self.assertLess(disk["size"], 128 * 1024 * 1024)
        self.assertTrue(all(f["redistribution"] in ("free", "ciukios") for f in metadata["files"]))
        names = {f["path"].rsplit("/", 1)[1] for f in metadata["files"]}
        self.assertFalse({n for n in names if dos.REGISTERED_DATA.fullmatch(n)})
        # The shareware README.TXT equals the registered release's: only the pin admits it.
        local = self.work / "third_party/Doom"
        local.mkdir(parents=True)
        shutil.copyfile(sources["/system/licenses/doom-shareware-README.TXT"], local / "README.TXT")
        (local / "DOOM.WAD").write_bytes(b"IWAD" + bytes(100))
        payloads = {r["path"]: r["sha256"] for r in records}
        dos.refuse_local_payloads(payloads, self.work, free=free)
        with self.assertRaisesRegex(SystemExit, "local commercial payload"):
            dos.refuse_local_payloads(payloads, self.work)
        with self.assertRaisesRegex(SystemExit, "registered game data"):
            dos.refuse_local_payloads({**payloads, "/Games/DOOM.WAD": hashlib.sha256((local / "DOOM.WAD").read_bytes()).hexdigest()},
                                      self.work, free=free)
        # The FAT32 read-back the image build performs.
        volume = self.work / "games.fat"
        with volume.open("wb") as stream:
            stream.truncate(128 * 1024 * 1024)
        subprocess.run(["mkfs.fat", str(volume)], check=True, stdout=subprocess.DEVNULL)
        image.populate_payloads(volume, sources, {"/System"} | directories)
        with patch.object(image, "PART_LBA", 0):
            image.check_payloads(volume, records, directories)
        # build() installs it and hands the same pin set to the guard.
        text = (ROOT / "scripts/build_image.py").read_text()
        self.assertIn("dos_payloads(out)", text)
        self.assertIn("refuse_local_payloads({p[\"path\"]: p[\"sha256\"] for p in payloads}, free=free)", text)

    def test_archive_rejects_traversal_and_links(self):
        for name, kind in (("lua-5.4.8-tests/../../escape", tarfile.REGTYPE),
                           ("lua-5.4.8-tests/link", tarfile.SYMTYPE)):
            archive = self.work / "bad.tar"
            with tarfile.open(archive, "w") as out:
                entry = tarfile.TarInfo(name)
                entry.type = kind
                if kind == tarfile.SYMTYPE:
                    entry.linkname = "../../escape"
                    out.addfile(entry)
                else:
                    entry.size = 1
                    out.addfile(entry, io.BytesIO(b"x"))
            with self.assertRaisesRegex(ValueError, "unexpected archive member"):
                lua.extract(archive, self.work / "extracted", lua.TEST_DIR)
        self.assertFalse((self.work / "escape").exists())

    def test_bad_archive_fails_before_replacing_output(self):
        archive = self.work / "bad.tar.gz"
        archive.write_bytes(b"wrong archive")
        sentinel = self.work / "existing/manifest.json"
        sentinel.parent.mkdir()
        sentinel.write_bytes(b"previous build")
        with patch.object(lua, "OUT", sentinel.parent):
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                lua.build(archive, archive, 1)
        self.assertEqual(sentinel.read_bytes(), b"previous build")

    def test_built_payloads_match_complete_unmodified_archive_inventory(self):
        for label, directory in (("Lua SDK", lua.OUT), ("SDK", image.SDK),
                                 ("Desktop payload", image.DESKTOP)):
            if not (directory / "manifest.json").is_file():
                self.skipTest(f"{label} build not present")
        sources, directories, metadata = image.application_payloads()
        manifest = json.loads((lua.OUT / "manifest.json").read_text())
        expected = {image.TEST_PATH + "/" + n for n in manifest["test_files"]} | {image.TEST_PATH + "/ciuki-f2.lua"}
        self.assertEqual({n for n in sources if n.startswith(image.TEST_PATH + "/")}, expected)
        self.assertTrue({"/Programs", "/tmp", "/Documents", "/System",
                         image.TEST_PATH + "/libs/P1"}.issubset(directories))
        self.assertEqual(metadata["lua"]["archives"],
                         json.loads((ROOT / "config/sdk-pins.json").read_text())["lua"])
        for name in ("/Programs/lua", "/Programs/hello", "/Programs/libc_smoke", "/system/tests/ciuki-f2.lua"):
            self.assertIn(name, sources)
        self.assertIn(image.TEST_PATH + "/ciuki-f2.lua", sources)
        self.assertIn("/system/tests/app-gate.meta", sources)
        # An extra edited upstream test must not be silently copied onto the volume.
        extra = lua.OUT / lua.TEST_DIR / "unexpected.lua"
        extra.write_text("error('unexpected')\n")
        try:
            with self.assertRaisesRegex(ValueError, "stale Lua"):
                image.application_payloads()
        finally:
            extra.unlink()

    def test_unbuilt_payloads_skip_before_inventory_validation(self):
        lua_out, sdk_out, desktop_out = (self.work / name for name in ("lua", "sdk", "desktop"))
        for directory in (lua_out, sdk_out, desktop_out):
            directory.mkdir()
            (directory / "manifest.json").write_text("{}")
        for label, directory in (("Lua SDK", lua_out), ("SDK", sdk_out),
                                 ("Desktop payload", desktop_out)):
            with self.subTest(payload=label):
                manifest = directory / "manifest.json"
                manifest.unlink()
                with patch.object(lua, "OUT", lua_out), patch.object(image, "SDK", sdk_out), \
                        patch.object(image, "DESKTOP", desktop_out), \
                        patch.object(image, "application_payloads") as inventory:
                    with self.assertRaisesRegex(unittest.SkipTest, f"{label} build not present"):
                        self.test_built_payloads_match_complete_unmodified_archive_inventory()
                    inventory.assert_not_called()
                manifest.write_text("{}")


if __name__ == "__main__":
    unittest.main()
