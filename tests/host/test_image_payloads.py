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
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


image = load("image_payload_build", ROOT / "scripts/build_image.py")
lua = load("lua_payload_build", ROOT / "apps/lua/build_lua.py")


class ImagePayloadTests(unittest.TestCase):
    def setUp(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="payloads-", dir=parent)
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)

    def test_records_and_manifest_include_every_payload(self):
        a, b = self.work / "binary", self.work / "empty"
        a.write_bytes(bytes(range(256)) * 256)
        b.write_bytes(b"")
        sources = {"/system/tests/long-name.lua": a, "/bin/empty": b}
        records = image.payload_records(sources)
        self.assertEqual([p["path"] for p in records], sorted(sources))
        self.assertEqual(records[1], {"path": "/system/tests/long-name.lua",
                                     "sha256": hashlib.sha256(a.read_bytes()).hexdigest(),
                                     "size": 65536})
        disk = self.work / "disk.img"
        with patch.object(image, "git_identity", return_value=("unknown", "unknown")):
            image.write_manifest(disk, a, "image-hash", records, {"sdk_manifest_sha256": "sdk-hash"})
        manifest = json.loads((self.work / "build-manifest.json").read_text())
        self.assertEqual(manifest["payloads"], records)
        self.assertEqual(manifest["sdk_manifest_sha256"], "sdk-hash")
        self.assertEqual(manifest["image_sha256"], "image-hash")
        self.assertEqual(image.IMAGE_BYTES, 512 * 1024 * 1024)
        self.assertEqual(image.PART_LBA, 2048)

    @unittest.skipUnless(all(shutil.which(t) for t in ("mkfs.fat", "mmd", "mcopy", "mdir")),
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
        directories = {"/bin", "/tmp", "/home", "/system", "/system/tests",
                       "/system/tests/lua-5.4.8-tests", "/system/tests/lua-5.4.8-tests/libs",
                       "/system/tests/lua-5.4.8-tests/libs/P1"}
        sources = {"/system/tests/lua-5.4.8-tests/long-name.lua": source, "/bin/empty": empty}
        image.populate_payloads(volume, sources, directories)
        records = image.payload_records(sources)
        with patch.object(image, "PART_LBA", 0):
            image.check_payloads(volume, records, directories)
            wrong_size = [dict(records[1], size=records[1]["size"] + 1)]
            with self.assertRaisesRegex(ValueError, "payload size"):
                image.check_payloads(volume, wrong_size, directories)
            source.write_bytes(b"X" * records[1]["size"])
            subprocess.run(["mcopy", "-o", "-i", str(volume), str(source),
                            "::" + records[1]["path"]], check=True)
            with self.assertRaisesRegex(ValueError, "payload SHA-256"):
                image.check_payloads(volume, records, directories)
            with self.assertRaises(subprocess.CalledProcessError):
                image.check_payloads(volume, [{"path": "/bin/missing", "size": 0,
                                              "sha256": records[0]["sha256"]}], directories)

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
        expected = {image.TEST_PATH + "/" + n for n in manifest["test_files"]}
        self.assertEqual({n for n in sources if n.startswith(image.TEST_PATH + "/")}, expected)
        self.assertTrue({"/bin", "/tmp", "/home", "/system",
                         image.TEST_PATH + "/libs/P1"}.issubset(directories))
        self.assertEqual(metadata["lua"]["archives"],
                         json.loads((ROOT / "config/sdk-pins.json").read_text())["lua"])
        for name in ("/bin/lua", "/bin/hello", "/bin/libc_smoke", "/system/tests/ciuki-f2.lua"):
            self.assertIn(name, sources)
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
