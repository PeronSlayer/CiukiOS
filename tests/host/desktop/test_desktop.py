"""Native production desktop tests with fake surfaces/channels; never boots QEMU."""
# SPDX-License-Identifier: MIT
import hashlib
import json
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
    def test_shell_suite_uses_existing_runner(self):
        path = str(ROOT / "scripts/test")
        sys.path.insert(0, path)
        try:
            runner = load("desktop_shell_runner_test", ROOT / "scripts/test/run.py")
            suite = json.loads((ROOT / "tests/suites/f2-desktop.json").read_text())
            shell = [c for c in suite["cases"] if c["id"].startswith("desktop-shell-")]
            self.assertEqual({c["profile"] for c in shell}, {"qemu-t23", "qemu-e500"})
            self.assertEqual(len(shell), 2)
            for case in shell:
                self.assertTrue(case["desktop_screen"])
                runner.Actions(case["actions"])
                batches = case["actions"][0]["batches"]
                keys = [e["data"] for b in batches for e in b["events"] if e["type"] == "key"]
                self.assertEqual([(k["key"]["data"], k["down"]) for k in keys],
                                 [("f12", True), ("f12", False), ("a", True), ("a", False)])
                predicate = next(p for p in case["expected"]["predicates"] if p.get("where", {}).get("stage") == "after")
                self.assertEqual(predicate["fields"]["keys"], {"eq": 4})
        finally:
            sys.path.remove(path)

    def test_plain_screen_digest_and_shell_portrait(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="desktop-screen-", dir=parent) as temp:
            folder = Path(temp)
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0")
            binary = folder / "capture"
            sources = [ROOT / "apps/desktop" / n for n in ("client.c", "protocol.c", "input.c", "compositor.c")]
            sources += [ROOT / "tests/host/desktop" / n for n in ("fake.c", "compositor_capture.c")]
            subprocess.run(["clang", "-std=c17", "-O1", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-I", str(ROOT / "apps/desktop"), "-I", str(ROOT / "src/kernel/include"),
                            *(str(p) for p in sources), "-o", str(binary)], check=True, env=env)
            c = load("capture_portrait", ROOT / "apps/desktop/convert_portrait.py")
            portrait = c.convert(ROOT / "assets/brand/ciuki-logo.png")
            source = folder / "portrait.xrgb"
            source.write_bytes(portrait)
            for mode in ("plain", "shell", "about"):
                target = folder / (mode + ".xrgb")
                subprocess.run([str(binary), str(source), str(target), mode], check=True, env=env)
                pixels = target.read_bytes()
                self.assertEqual(len(pixels), 640*480*4)
                if mode == "plain":
                    # Independently compared with HEAD's f2-08 compositor before
                    # recording this fixture; unchanged font/clock/cursor/portrait.
                    self.assertEqual(hashlib.sha256(pixels).hexdigest(),
                                     "6755f9a40f4b0ee69c1253145fe53ebd75f5f1948c2429c70da1a90923f82426")
                else:
                    for y in range(20):
                        for x in range(20):
                            offset = ((454+y)*640+8+x)*4
                            src = ((y*256//20)*256+x*256//20)*4
                            self.assertEqual(pixels[offset:offset+4], portrait[src:src+4])

    def test_shell_launcher(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="desktop-launch-", dir=parent) as temp:
            folder = Path(temp)
            inc = folder / "include/ciuki"
            inc.mkdir(parents=True)
            for name in ("channel.h", "surface.h", "spawn.h"):
                shutil.copy2(ROOT / "sdk/sysroot-overlay/include/ciuki" / name, inc / name)
            for name in ("runtime.h", "raw.h"):
                (inc / name).write_text("/* supplied by host harness */\n")
            env = dict(os.environ, TMPDIR=temp, ASAN_OPTIONS="detect_leaks=0")
            binary = folder / "launch-test"
            sources = [ROOT / "apps/desktop" / n for n in ("client.c", "protocol.c", "input.c", "compositor.c")]
            sources += [ROOT / "tests/host/desktop" / n for n in ("fake.c", "shell_launch_test.c")]
            subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                            "-I", str(folder / "include"), "-I", str(ROOT / "apps/desktop"),
                            "-I", str(ROOT / "src/kernel/include"), *(str(p) for p in sources), "-o", str(binary)],
                           check=True, env=env)
            subprocess.run([str(binary)], check=True, env=env)

    def test_tango_icon_provenance(self):
        c = load("shell_icon_test", ROOT / "apps/desktop/convert_icons.py")
        record = c.check()
        self.assertEqual(set(record["icons"]), set(c.MEMBERS))
        self.assertEqual(record["archive_sha256"], c.ARCHIVE_SHA)

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
