"""Native production desktop tests with fake surfaces/channels; never boots QEMU."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


class DesktopTests(unittest.TestCase):
    def test_native_production_model(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="desktop-", dir=parent) as temp:
            # This sandbox runs under ptrace, which LeakSanitizer cannot inspect.
            # ASan/UBSan stay enabled; fake live-fd/map counters check lifetimes.
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0")
            out = Path(temp) / "desktop-test"
            sources = [ROOT / "apps/desktop" / n for n in
                       ("protocol.c", "client.c", "input.c", "compositor.c")]
            sources += [ROOT / "tests/host/desktop" / n for n in ("fake.c", "desktop_test.c")]
            subprocess.run(["cc", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                            "-I", str(ROOT / "apps/desktop"), "-I", str(ROOT / "src/kernel/include"),
                            *(str(s) for s in sources), "-o", str(out)], check=True, env=env)
            subprocess.run([str(out)], check=True, env=env)

    def test_approved_portrait_determinism(self):
        c = load("desktop_conversion_test", ROOT / "apps/desktop/convert_portrait.py")
        source = ROOT / "assets/brand/ciuki-logo.png"
        before = hashlib.sha256(source.read_bytes()).hexdigest()
        a, b = c.convert(source), c.convert(source)
        self.assertEqual(a, b)
        self.assertEqual(len(a), 256*256*4)
        self.assertEqual(hashlib.sha256(a).hexdigest(), c.ARRAY_SHA256)
        self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(), before)
        self.assertEqual(a[:4], bytes((0x64,0x55,0x37,0)))
        self.assertTrue(all(a[i] == 0 for i in range(3, len(a), 4)))
        damaged = bytearray(source.read_bytes()); damaged[16] ^= 1
        with patch.object(Path, "read_bytes", return_value=bytes(damaged)):
            with self.assertRaisesRegex(ValueError, "source SHA-256"):
                c.convert(source)
        with self.assertRaisesRegex(ValueError, "PNG CRC"):
            c.decode(bytes(damaged))
        print("PASS portrait: pinned source/array SHA-256, repeat determinism, XRGB bytes, corrupt rejection")

    def test_build_provenance_and_payloads(self):
        build = load("desktop_provenance_test", ROOT / "apps/desktop/build_desktop.py")
        if not (build.OUT / "manifest.json").is_file():
            self.skipTest("run python3 apps/desktop/build_desktop.py first")
        m = build.validate_payloads()
        self.assertEqual(set(m["files"]), {"desktop", "demo", "ciuki-portrait.xrgb"})
        expected = build.inputs()
        self.assertTrue(build.current_build(expected))
        expected["cflags"] = []
        self.assertFalse(build.current_build(expected))
        image = load("desktop_image_test", ROOT / "scripts/build_image.py")
        sources, directories, metadata = image.desktop_payloads()
        self.assertEqual(set(sources), {"/bin/desktop", "/bin/demo", "/system/assets/ciuki-portrait.xrgb"})
        self.assertIn("/system/assets", directories)
        self.assertEqual(metadata["portrait"]["array_sha256"], m["files"]["ciuki-portrait.xrgb"])
        records = image.payload_records(sources)
        self.assertEqual(len(records), 3)
        for r in records:
            self.assertEqual(r["sha256"], m["files"][Path(r["path"]).name])
        print("PASS desktop/demo ELF, SDK/source/output provenance, payload size/SHA-256 inventory")

    @unittest.skipUnless(all(shutil.which(t) for t in ("mkfs.fat", "mcopy", "mmd", "mdir")),
                         "requires mtools and mkfs.fat")
    def test_actual_desktop_payload_fat_readback(self):
        image = load("desktop_fat_test", ROOT / "scripts/build_image.py")
        if not (image.DESKTOP / "manifest.json").is_file():
            self.skipTest("build desktop first")
        sources, directories, _ = image.desktop_payloads()
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="desktop-fat-", dir=parent) as temp:
            env = dict(os.environ, TMPDIR=temp)
            volume = Path(temp) / "fixture.fat"
            with volume.open("wb") as f:
                f.truncate(8*1024*1024)
            subprocess.run(["mkfs.fat", str(volume)], check=True, env=env, stdout=subprocess.DEVNULL)
            image.populate_payloads(volume, sources, directories)
            with patch.object(image, "PART_LBA", 0):
                image.check_payloads(volume, image.payload_records(sources), directories)
        print("PASS real FAT read-back: desktop, demo, portrait sizes/SHA-256")

    def test_stale_output_rejected(self):
        build = load("desktop_stale_test", ROOT / "apps/desktop/build_desktop.py")
        if not (build.OUT / "manifest.json").is_file():
            self.skipTest("build desktop first")
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="desktop-stale-", dir=parent) as temp:
            dest = Path(temp)
            for name in ("manifest.json", "desktop", "demo", "ciuki-portrait.xrgb"):
                shutil.copy2(build.OUT / name, dest / name)
            with patch.object(build, "OUT", dest):
                self.assertTrue(build.current_build(build.inputs()))
                (dest / "ciuki-portrait.xrgb").write_bytes(b"wrong portrait")
                with self.assertRaisesRegex(ValueError, "stale desktop"):
                    build.validate_payloads()
        print("PASS stale/tampered portrait rejected before image payload acceptance")


if __name__ == "__main__":
    unittest.main()
