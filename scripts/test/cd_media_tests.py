#!/usr/bin/env python3
"""f9-01 host tests: Setup CD media (boot image, ISO, eligibility, manifest)
and the kernel model (cdview, boot-CD identification, Setup-mode storage).

Synthetic, lawful fixtures only: a small canonical-style disk image built
from mbr.asm, ciukldr.asm and mkfs.fat/mtools, pinned stand-in archives and
NTFS/FAT/blank fixture disks. Everything stays in build/host/cd-media (never
/tmp); the fixture boot image is about 300 MB because the loader requires
FAT32 with 8 sectors per cluster (>= 65,525 clusters).
Run: nice -n 19 python3 scripts/test/cd_media_tests.py
"""
# SPDX-License-Identifier: GPL-2.0-only
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / "build/host/cd-media"
EPOCH = 1792590978
IDENTITY_ADDRESS = 0xC0100040
IDENTITY = "CIUKI-BUILD-IDENTITY host-fixture 1792590978"


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


cd = module("cd_media_build_cd", ROOT / "scripts/build_cd.py")
bi = module("cd_media_build_image", ROOT / "scripts/build_image.py")


def sha(data):
    return hashlib.sha256(data).hexdigest()


def env():
    return dict(os.environ, TZ="UTC", LC_ALL="C", SOURCE_DATE_EPOCH=str(EPOCH))


def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, env=env(), cwd=ROOT, **kwargs)


def fake_kernel():
    """ELF32 with one PT_LOAD at 0xC0100000 (file offset 0x1000) holding the identity."""
    body = bytearray(0x200)
    text = (IDENTITY + "\n").encode()
    body[0x40:0x40 + len(text)] = text
    header = bytearray(52)
    header[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HHIIIIIHHHHHH", header, 16, 2, 3, 1, 0xC0100000, 52, 0, 0, 52, 32, 1, 40, 0, 0)
    ph = struct.pack("<IIIIIIII", 1, 0x1000, 0xC0100000, 0x00100000, len(body), len(body), 5, 0x1000)
    data = bytearray(0x1000 + len(body))
    data[:52] = header
    data[52:84] = ph
    data[0x1000:] = body
    return bytes(data)


PAYLOAD = {
    "/system/VMM.ELF": fake_kernel(),
    "/system/BOOT.CFG": b"safe=0 serial=1\n",
    "/Programs/hello": bytes((i * 7 + 3) & 255 for i in range(70000)),
    "/Programs/netsurf": b"netsurf placeholder\n" * 100,
    "/System/Win32/kernel32.dll": bytes(range(256)) * 20,
    "/System/sdk/include/stdio.h": b"/* fixture */\n",
    "/system/licenses/WINE-LICENSE": b"Wine LGPL fixture notice\n",
    "/system/licenses/NetSurf-SOURCE.txt": b"NetSurf GPL-2.0 fixture\n",
    "/system/licenses/zlib.txt": b"zlib fixture\n",
    "/system/licenses/wolf3d-shareware-VENDOR.DOC": b"Apogee fixture\n",
    "/system/licenses/doom-shareware-README.TXT": b"id fixture\n",
    "/system/licenses/quake-shareware-SLICNSE.TXT": b"id fixture\n",
    "/Games/CIUKIGAM.IMG": bytes(10240),
    "/system/dos/DOSBOOT.IMG": bytes([0xEB, 0x3C, 0x90]) + bytes(8189),
    "/WINDOWS/FONTS/LiberationMono-Regular.ttf": b"font fixture" * 50,
    "/WINDOWS/SYSTEM/soundfonts.json": b"{}\n",
    "/Documents/empty.txt": b"",
    "/Documents/A long Mixed Case name.txt": b"long name\n",
}
DIRECTORIES = ["/Documents", "/Documents/Desktop", "/Games", "/Program Files", "/Program Files/Common Files",
               "/Programs", "/System", "/System/Win32", "/System/licenses", "/System/dos", "/System/sdk",
               "/System/sdk/include", "/WINDOWS", "/WINDOWS/FONTS", "/WINDOWS/SYSTEM"]


def assemble(work):
    mbr = work / "mbr.bin"
    loader = work / "ciukldr.bin"
    run("nasm", "-f", "bin", ROOT / "src/boot/mbr.asm", "-o", mbr)
    run("nasm", "-f", "bin", "-dCIUKI_VERSION='host fixture'", ROOT / "src/boot/ciukldr.asm", "-o", loader)
    return mbr.read_bytes(), bytes(bi.prepare_loader(loader.read_bytes()))


def make_disk_image(work):
    """A canonical-style disk image: MBR + signature, loader at LBA 1, FAT32 at 2048."""
    directory = work / "disk"
    directory.mkdir(parents=True)
    mbr_code, loader = assemble(work)
    volume = work / "disk.fat"
    sectors = 40 * 1024 * 2
    with volume.open("wb") as f:
        f.truncate(sectors * 512)
    run("mkfs.fat", "--invariant", "-F", "32", "-s", "1", "-n", "CIUKIOS", volume, stdout=subprocess.DEVNULL)
    for d in DIRECTORIES:
        run("mmd", "-i", volume, "::" + d)
    staging = work / "payload"
    for path, data in sorted(PAYLOAD.items()):
        source = staging / path.lstrip("/")
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_bytes(data)
        run("mcopy", "-i", volume, source, "::" + path)
    run("mattrib", "-i", volume, "+r", "::/WINDOWS/FONTS/LiberationMono-Regular.ttf")
    mbr = bytearray(mbr_code)
    struct.pack_into("<I", mbr, 440, 0x4B554943)
    struct.pack_into("<B3sB3sII", mbr, 446, 0x80, b"\x20\x21\0", 0x0C, b"\xfe\xff\xff", 2048, sectors)
    image = directory / "ciukios.img"
    with image.open("wb") as f:
        f.write(mbr)
        f.write(loader)
        f.seek(2048 * 512)
        f.write(volume.read_bytes())
    head = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    version = module("cd_media_version", ROOT / "scripts/ciuki_version.py").load()["display"]
    kernel = sha(PAYLOAD["/system/VMM.ELF"])
    manifest = {"schema_version": 1, "version": version, "git_revision": head, "git_dirty": True,
                "image": "ciukios.img", "image_sha256": sha(image.read_bytes()), "kernel_sha256": kernel,
                "build_epoch": EPOCH, "payloads": [{"path": p, "sha256": sha(d), "size": len(d)} for p, d in sorted(PAYLOAD.items())],
                "games": {"packages": [{"name": n} for n in ("wolf3d-shareware", "doom-shareware", "quake-shareware")]}}
    (directory / "build-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (directory / "build-clock.json").write_text(json.dumps({"utc_epoch": EPOCH, "kernel_sha256": kernel, "version": version}) + "\n")
    volume.unlink()
    return image, mbr_code, loader


def make_config(work):
    """The real component table; stand-in archives with their own pins; a
    narrow source tree so the fixture stays small."""
    config = json.loads(cd.COMPONENTS.read_text())
    sources = work / "sources"
    sources.mkdir()
    pins = {}
    for name in ("WINE100.TXZ", "NETSURF.TGZ", "JENT370.TGZ", "FDKERNEL.ZIP", "FDCOM.ZIP", "FDSHARE.ZIP"):
        data = f"{name} stand-in archive for the host fixture\n".encode()
        (sources / name).write_bytes(data)
        pins[name] = sha(data)
        config["archives"][name].update({"file": str((sources / name).relative_to(ROOT)), "pin": str((sources / "pins.json").relative_to(ROOT)),
                                         "key": name.replace(".", "_")})
    (sources / "pins.json").write_text(json.dumps({k.replace(".", "_"): v for k, v in pins.items()}))
    config["source_tree"]["include"] = ["Makefile", "LICENSE", "VERSION"]
    path = work / "components.json"
    path.write_text(json.dumps(config, indent=2))
    return path, config


def make_fixture_disks(work):
    blank = work / "blank.img"
    with blank.open("wb") as f:
        f.truncate(8 * 1024 * 1024)
    def wrap(volume, kind, out):
        mbr = bytearray(512)
        count = volume.stat().st_size // 512
        struct.pack_into("<B3sB3sII", mbr, 446, 0x00, b"\x20\x21\0", kind, b"\xfe\xff\xff", 2048, count)
        mbr[510:] = b"\x55\xaa"
        with out.open("wb") as f:
            f.write(mbr)
            f.seek(2048 * 512)
            f.write(volume.read_bytes())
        volume.unlink()
    fat = work / "fat.part"
    with fat.open("wb") as f:
        f.truncate(16 * 1024 * 1024)
    run("mkfs.fat", "--invariant", "-F", "16", "-n", "OCCUPIED", fat, stdout=subprocess.DEVNULL)
    note = work / "OCCUPIED.TXT"
    note.write_bytes(b"existing data that Setup must not touch\n")
    run("mcopy", "-i", fat, note, "::/OCCUPIED.TXT")
    wrap(fat, 0x06, work / "fat.img")
    ntfs = work / "ntfs.part"
    with ntfs.open("wb") as f:
        f.truncate(16 * 1024 * 1024)
    run("mkntfs", "-q", "-Q", "-F", "-s", "512", "-p", "2048", "-L", "OCCUPIED", ntfs, stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL)
    wrap(ntfs, 0x07, work / "ntfs.img")
    return blank, work / "fat.img", work / "ntfs.img"


class Fixture:
    built = None

    @classmethod
    def get(cls):
        if cls.built:
            return cls.built
        if WORK.exists():
            shutil.rmtree(WORK)
        WORK.mkdir(parents=True)
        image, mbr_code, loader = make_disk_image(WORK)
        config_path, config = make_config(WORK)
        setup = WORK / "setup.elf"
        setup.write_bytes(b"\x7fELF placeholder setup program for the host fixture\n")
        outputs = {}
        for name in ("a", "b"):
            outputs[name] = cd.build(image, WORK / name, setup, config_path=config_path, allow_source_mismatch=True)
        cls.built = {"image": image, "mbr_code": mbr_code, "loader": loader, "config_path": config_path, "config": config,
                     "setup": setup, "manifests": outputs, "a": WORK / "a", "b": WORK / "b"}
        return cls.built


def chs(lba):
    c, rest = divmod(lba, 255 * 63)
    h, s = divmod(rest, 63)
    return bytes((h, (s + 1) | ((c >> 8) & 3) << 6, c & 255))


class EligibilityTests(unittest.TestCase):
    def setUp(self):
        self.config = json.loads(cd.COMPONENTS.read_text())
        self.games = ["wolf3d-shareware", "doom-shareware", "quake-shareware"]
        self.paths = sorted({*PAYLOAD, "/Programs/setup", "/system/RELEASE.JSON"})

    def test_every_fixture_path_has_a_row(self):
        assignment, present, required = cd.eligibility(self.config, self.paths, self.games)
        self.assertEqual(assignment["/Programs/netsurf"], "netsurf")
        self.assertEqual(assignment["/system/licenses/WINE-LICENSE"], "windows")
        self.assertEqual(assignment["/system/licenses/wolf3d-shareware-VENDOR.DOC"], "wolf3d")
        self.assertEqual(assignment["/WINDOWS/SYSTEM/soundfonts.json"], "soundfont")
        self.assertIn("WINE100.TXZ", required)
        self.assertIn("NETSURF.TGZ", required)
        self.assertIn("CIUKIOS.ZIP", required)

    def test_path_without_component_fails(self):
        with self.assertRaisesRegex(ValueError, "payload path without a component: /Unlisted/file"):
            cd.eligibility(self.config, [*self.paths, "/Unlisted/file"], self.games)
        with self.assertRaisesRegex(ValueError, "without a component: /system/OTHER.BIN"):
            cd.eligibility(self.config, [*self.paths, "/system/OTHER.BIN"], self.games)

    def test_copyleft_without_archive_fails(self):
        config = copy.deepcopy(self.config)
        next(c for c in config["components"] if c["id"] == "netsurf")["archives"] = []
        with self.assertRaisesRegex(ValueError, "copyleft component netsurf lists no source archive"):
            cd.eligibility(config, self.paths, self.games)
        # A present copyleft component whose archive cannot be produced fails the build.
        assignment, present, required = cd.eligibility(self.config, [*self.paths, "/system/licenses/CIUKIDOS.TXT"], self.games)
        self.assertIn("dos-unified", present)
        self.assertIn("UNIDOS.ZIP", required)
        target = WORK / "eligibility"
        shutil.rmtree(target, ignore_errors=True)
        with self.assertRaisesRegex(ValueError, r"UNIDOS.ZIP is required but not produced \(f3-55: ciukidos-unified-source.zip absent\)"):
            cd.build_archives(self.config, ["UNIDOS.ZIP"], target, EPOCH, WORK, {})
        # Produced beside the image, it must match the image build's manifest.
        produced = WORK / "unified-fixture"
        shutil.rmtree(produced, ignore_errors=True)
        produced.mkdir(parents=True)
        (produced / "ciukidos-unified-source.zip").write_bytes(b"unified source fixture")
        record = {"unified_dos": {"corresponding_source": {"sha256": sha(b"unified source fixture")}}}
        archives = cd.build_archives(self.config, ["UNIDOS.ZIP"], target, EPOCH, produced, record)
        self.assertEqual(archives["UNIDOS.ZIP"]["sha256"], sha(b"unified source fixture"))
        record["unified_dos"]["corresponding_source"]["sha256"] = sha(b"other")
        with self.assertRaisesRegex(ValueError, "UNIDOS.ZIP: SHA-256 differs from build-manifest.json"):
            cd.build_archives(self.config, ["UNIDOS.ZIP"], target, EPOCH, produced, record)
        shutil.rmtree(produced, ignore_errors=True)
        for marker in ("/system/dos/UNIFIED.IMG", "/system/licenses/SHARE.TXT"):
            self.assertIn("UNIDOS.ZIP", cd.eligibility(self.config, [*self.paths, marker], self.games)[2])
        config = copy.deepcopy(self.config)
        config["archives"]["NETSURF.TGZ"]["file"] = "Makefile"
        with self.assertRaisesRegex(ValueError, "NETSURF.TGZ: SHA-256 differs"):
            cd.build_archives(config, ["NETSURF.TGZ"], target, EPOCH)
        shutil.rmtree(target, ignore_errors=True)

    def test_shareware_conditions(self):
        wolf = next(c for c in self.config["components"] if c["id"] == "wolf3d")
        self.assertEqual(wolf["class"], "bundled-shareware")
        self.assertIn("Apogee permission for bundles/CD-ROM (VENDOR.DOC [3][C]) not on file", wolf["conditions"])
        config = copy.deepcopy(self.config)
        next(c for c in config["components"] if c["id"] == "quake")["conditions"] = " "
        with self.assertRaisesRegex(ValueError, "bundled-shareware component quake lacks its conditions text"):
            cd.eligibility(config, self.paths, self.games)
        with self.assertRaisesRegex(ValueError, "games package heretic-shareware has no component row"):
            cd.eligibility(self.config, self.paths, [*self.games, "heretic-shareware"])

    def test_canonical_payload_is_covered(self):
        for candidate in (ROOT / "build/f0/build-manifest.json", ROOT / "build/f0-main/build-manifest.json"):
            if candidate.is_file():
                manifest = json.loads(candidate.read_text())
                paths = [p["path"] for p in manifest["payloads"]] + ["/Programs/setup", "/system/RELEASE.JSON"]
                games = [p["name"] for p in manifest.get("games", {}).get("packages", [])]
                cd.eligibility(self.config, paths, games)
                self.assertFalse(any("DMFAQ66" in p.upper() for p in paths))
                return
        self.skipTest("no canonical build-manifest.json in this tree")


class MediaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.f = Fixture.get()
        cls.a = cls.f["a"]
        cls.manifest = json.loads((cls.a / "cd-manifest.json").read_text())
        cls.boot = cls.a / "ciukios-setup-boot.img"
        cls.iso = cls.a / "ciukios-setup.iso"

    def read(self, path, offset, size):
        with path.open("rb") as f:
            f.seek(offset)
            return f.read(size)

    def test_boot_image_mbr_loader_and_chs(self):
        mbr = self.read(self.boot, 0, 512)
        self.assertEqual(mbr[:440], self.f["mbr_code"][:440])           # mbr.asm build
        self.assertEqual(struct.unpack_from("<I", mbr, 440)[0], 0x4B554943)
        entry = mbr[446:462]
        sectors = self.boot.stat().st_size // 512
        self.assertEqual((entry[0], entry[4]), (0x80, 0x0C))
        self.assertEqual(struct.unpack_from("<II", entry, 8), (2048, sectors - 2048))
        self.assertEqual(entry[1:4], chs(2048))
        self.assertEqual(entry[5:8], chs(sectors - 1))
        self.assertEqual((entry[5], entry[6] & 63), (254, 63))              # SeaBIOS: 255 heads x 63 sectors
        cylinders = ((entry[6] << 2) & 0x300) + entry[7] + 1
        self.assertEqual(cylinders * 255 * 63, sectors)
        self.assertEqual(mbr[462:510], bytes(48))
        self.assertEqual(mbr[510:], b"\x55\xaa")
        loader = self.f["loader"]
        self.assertEqual(self.read(self.boot, 512, len(loader)), loader)
        self.assertEqual((self.a / "ciukldr.bin").read_bytes(), loader)
        self.assertEqual(self.read(self.f["image"], 512, len(loader)), loader)   # the disk image's loader
        gap = self.read(self.boot, 512 + len(loader), 2047 * 512 - len(loader))
        self.assertEqual(gap, bytes(len(gap)))
        self.assertEqual(sectors % 4, 0)

    def test_boot_image_bpb_matches_every_loader_pin(self):
        source = (ROOT / "src/boot/ciukldr/disk.inc").read_text()
        reserved = int(re.search(r"%define CIUKI_FAT_RESERVED_SECTORS (\d+)", source).group(1))
        body = source.split(".bpb:", 1)[1].split("call cluster_validate", 1)[0]
        pins = re.findall(r"cmp (byte|word) \[sector\+(\d+)\], (\w+)", body)
        self.assertGreaterEqual(len(pins), 12)
        bpb = self.read(self.boot, 2048 * 512, 512)
        for width, offset, value in pins:
            expected = reserved if value == "CIUKI_FAT_RESERVED_SECTORS" else int(value, 0)
            actual = bpb[int(offset)] if width == "byte" else struct.unpack_from("<H", bpb, int(offset))[0]
            self.assertEqual(actual, expected, f"disk.inc pin {width} [sector+{offset}]")
        total, fat = struct.unpack_from("<II", bpb, 32)
        mbr = self.read(self.boot, 0, 512)
        self.assertLessEqual(total, struct.unpack_from("<I", mbr, 458)[0])
        clusters = (total - reserved - 2 * fat) // 8
        self.assertTrue(65525 <= clusters <= 0x0FFFFFF5 and fat * 128 >= clusters + 2)
        self.assertEqual(clusters, self.manifest["boot_image"]["clusters"])
        self.assertEqual(struct.unpack_from("<I", bpb, 44)[0], 2)
        descriptor = self.read(self.boot, (2048 + 8) * 512, 512)
        self.assertEqual(struct.unpack_from("<4sIII", descriptor), (b"CBB1", 1, 32, 128))
        self.assertEqual(self.read(self.boot, (2048 + 32) * 512, 128 * 512), bytes(128 * 512))
        self.assertEqual(self.read(self.boot, (2048 + 6) * 512, 1024), self.read(self.boot, 2048 * 512, 1024))

    def test_boot_image_fsck_and_files(self):
        partition = WORK / "fsck.part"
        with self.boot.open("rb") as source, partition.open("wb") as dest:
            size = self.boot.stat().st_size - 2048 * 512
            dest.truncate(size)
            cd.copy_extents(source, 2048 * 512, dest, 0, size)
        try:
            result = subprocess.run(["fsck.fat", "-n", str(partition)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        finally:
            partition.unlink()
        volume = f"{self.boot}@@{2048 * 512}"
        cfg = run("mcopy", "-i", volume, "::/system/BOOT.CFG", "-", capture_output=True).stdout
        self.assertEqual(cfg, b"safe=0 serial=1 setup=1\n")
        release = json.loads(run("mcopy", "-i", volume, "::/system/RELEASE.JSON", "-", capture_output=True).stdout)
        disk = json.loads((self.f["image"].parent / "build-manifest.json").read_text())
        self.assertEqual(release["disk_image_sha256"], disk["image_sha256"])
        self.assertEqual(release["git_revision"], disk["git_revision"])
        self.assertEqual(release["version"], disk["version"])
        self.assertEqual(release["boot_options"], cfg.decode())
        self.assertEqual(release["payload_files"], len(PAYLOAD) + 2)          # + setup, RELEASE.JSON
        self.assertEqual(release["content_digest"], self.manifest["content_digest"])
        boot = self.manifest["boot_image"]
        self.assertEqual(boot["boot_cfg_template"].encode("ascii"), PAYLOAD["/system/BOOT.CFG"])
        self.assertEqual(boot["boot_cfg_sha256"], sha(PAYLOAD["/system/BOOT.CFG"]))
        self.assertIn("/Program Files/Common Files", [r["path"] for r in boot["directories"]])
        self.assertTrue(all(r["attributes"] & 16 for r in boot["directories"]))
        self.assertTrue(all(isinstance(r["attributes"], int) for r in boot["files"]))
        records = {r["path"]: r for r in boot["files"]}
        for path, data in PAYLOAD.items():
            if path == "/system/BOOT.CFG":
                continue
            self.assertEqual(records[path]["sha256"], sha(data))
            self.assertEqual(run("mcopy", "-i", volume, "::" + path, "-", capture_output=True).stdout, data)
        self.assertEqual(records["/Programs/setup"]["sha256"], sha(self.f["setup"].read_bytes()))
        listing = run("mattrib", "-i", volume, "::/WINDOWS/FONTS/LiberationMono-Regular.ttf", capture_output=True, text=True).stdout
        self.assertIn("R", listing.split("::")[0])
        tree = run("mattrib", "-/", "-i", volume, "::/", capture_output=True, text=True).stdout
        for directory in ("::/Program Files/Common Files", "::/Documents/Desktop"):   # empty directories kept
            self.assertIn(directory, tree)

    def test_iso_descriptors_path_table_and_names(self):
        with self.iso.open("rb") as f:
            def block(n):
                f.seek(n * 2048)
                return f.read(2048)
            pvd = block(16)
            self.assertEqual(pvd[:7], b"\x01CD001\x01")
            self.assertEqual(pvd[40:72].rstrip(b" "), b"CIUKIOS_SETUP")
            space = struct.unpack_from("<I", pvd, 80)[0]
            self.assertEqual(space, struct.unpack_from(">I", pvd, 84)[0])
            self.assertEqual(space * 2048, self.iso.stat().st_size)
            self.assertEqual(struct.unpack_from("<H", pvd, 128)[0], 2048)
            types = []
            n = 16
            while True:
                d = block(n)
                types.append(d[0])
                if d[0] == 255:
                    break
                n += 1
            self.assertEqual(types, [1, 0, 255])                            # PVD, boot record, terminator; no Joliet SVD
            boot = block(17)
            self.assertEqual(boot[:7], b"\x00CD001\x01")
            self.assertEqual(boot[7:39], b"EL TORITO SPECIFICATION".ljust(32, b"\0"))
            catalog = struct.unpack_from("<I", boot, 0x47)[0]
            size = struct.unpack_from("<I", pvd, 132)[0]
            table = block(struct.unpack_from("<I", pvd, 140)[0])[:size]
            names, at = [], 0
            while at < size:
                length = table[at]
                names.append(table[at + 8:at + 8 + length])
                at += 8 + length + (length & 1)
            self.assertEqual(names, [b"\0", b"LICENSES", b"SOURCES"])
            root = pvd[156:190]
            files = {}
            def walk(extent, length, prefix):
                data = b"".join(block(extent + i) for i in range(-(-length // 2048)))[:length]
                i = 0
                while i < length:
                    rl = data[i]
                    if not rl:
                        i = (i // 2048 + 1) * 2048
                        continue
                    rec = data[i:i + rl]
                    name = rec[33:33 + rec[32]]
                    i += rl
                    if name in (b"\0", b"\1"):
                        continue
                    self.assertEqual(rl, (33 + rec[32] + 1) & ~1, "no Rock Ridge system use area")
                    text = name.decode()
                    if rec[25] & 2:
                        self.assertRegex(text, r"^[A-Z0-9_]{1,8}$")
                        walk(struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0], prefix + text + "/")
                    else:
                        self.assertRegex(text, r"^[A-Z0-9_]{1,8}\.[A-Z0-9_]{0,3};1$")
                        files[prefix + text[:-2]] = (struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0])
            walk(struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0], "")
            cat = block(catalog)
            v, e = cat[:32], cat[32:64]
            self.assertEqual((v[0], v[1], v[2:4], v[30:32]), (1, 0, b"\0\0", b"\x55\xaa"))
            self.assertEqual(sum(struct.unpack("<16H", v)) & 0xFFFF, 0)       # checksum recomputed
            self.assertEqual((e[0], e[1]), (0x88, 4))
            self.assertEqual(struct.unpack_from("<H", e, 2)[0], 0)
            self.assertEqual((e[4], e[5]), (0x0C, 0))
            self.assertEqual(struct.unpack_from("<H", e, 6)[0], 1)
            rba = struct.unpack_from("<I", e, 8)[0]
            self.assertEqual(e[12:], bytes(20))
            self.assertEqual(files["BOOT.CAT"][0], catalog)
            extent, length = files["CIUKBOOT.IMG"]
            self.assertEqual(extent, rba)
            self.assertEqual(length, self.boot.stat().st_size)
            f.seek(extent * 2048)
            self.assertEqual(sha(f.read(length)), sha(self.boot.read_bytes()))
        self.assertEqual(self.manifest["iso"]["catalog"]["validation_entry_hex"], v.hex())
        self.assertEqual(self.manifest["iso"]["catalog"]["initial_entry_hex"], e.hex())
        expected = {"BOOT.CAT", "CDMANIFE.JSN", "CIUKBOOT.IMG", "SETUPINF.TXT", "LICENSES/INDEX.TXT", "LICENSES/SOURCES.TXT",
                    "SOURCES/CIUKIOS.ZIP", "SOURCES/WINE100.TXZ", "SOURCES/NETSURF.TGZ"}
        self.assertTrue(expected <= set(files), sorted(set(files)))
        self.assertEqual(set(files), {r["path"] for r in self.manifest["iso_tree"]})

    def test_iso_independent_extraction_matches_manifest(self):
        out = WORK / "extract"
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir()
        try:
            run("xorriso", "-osirrox", "on", "-indev", self.iso, "-extract", "/", out, capture_output=True)
            found = {p.relative_to(out).as_posix(): p for p in out.rglob("*") if p.is_file()}
            records = {r["path"]: r for r in self.manifest["iso_tree"]}
            self.assertEqual(set(found), set(records))
            for path, record in records.items():
                data = found[path].read_bytes()
                self.assertEqual((len(data), sha(data)), (record["size"], record["sha256"]), path)
            sources = (out / "LICENSES/SOURCES.TXT").read_text()
            self.assertIn("section 3(a)", sources)
            for name, record in self.manifest["archives"].items():
                self.assertIn(f"\\SOURCES\\{name}  SHA-256 {record['sha256']}", sources)
            index = (out / "LICENSES/INDEX.TXT").read_text()
            self.assertIn("payload:/system/licenses/wolf3d-shareware-VENDOR.DOC", index)
            self.assertIn("repo:LICENSE", index)
            with __import__("zipfile").ZipFile(out / "SOURCES/CIUKIOS.ZIP") as archive:
                self.assertEqual(sorted(archive.namelist()), ["ciukios/LICENSE", "ciukios/Makefile", "ciukios/VERSION"])
                self.assertEqual(archive.read("ciukios/Makefile"), (ROOT / "Makefile").read_bytes())
            sample = (out / "SETUPINF.TXT").read_text()
            self.assertIn("[Setup]\nVersion=1\n", sample)
            copy_ = json.loads((out / "CDMANIFE.JSN").read_text())
            self.assertEqual(copy_["boot_image"]["sha256"], self.manifest["boot_image"]["sha256"])
            self.assertNotIn("iso", copy_)
        finally:
            for p in [out, *out.rglob("*")]:
                if p.is_dir():
                    p.chmod(0o700)
            shutil.rmtree(out)

    def test_two_builds_are_byte_identical(self):
        b = self.f["b"]
        for name in ("ciukios-setup.iso", "ciukios-setup-boot.img", "cd-manifest.json"):
            self.assertEqual(sha((self.a / name).read_bytes()), sha((b / name).read_bytes()), name)

    def test_manifest_and_size_figures(self):
        iso = self.manifest["iso"]
        self.assertEqual(iso["sha256"], sha(self.iso.read_bytes()))
        self.assertEqual(iso["sectors"], -(-self.iso.stat().st_size // 2048))
        self.assertEqual(iso["limit_sectors"], 333000)
        self.assertEqual(iso["headroom_sectors"], 333000 - iso["sectors"])
        self.assertEqual(self.manifest["boot_image"]["sha256"], sha(self.boot.read_bytes()))
        self.assertTrue(self.manifest["boot_image"]["files"])
        self.assertIn("system", self.manifest["components"]["present"])
        # Keep the exact field evidence after the large fixtures are deleted.
        catalog = iso["catalog"]
        print(f"[cd-test] catalog block={catalog['block']} validation={catalog['validation_entry_hex']} "
              f"initial={catalog['initial_entry_hex']}")
        boot = self.manifest["boot_image"]
        print("[cd-test] boot " + json.dumps({k: boot[k] for k in (
            "image_sectors", "partition_sectors", "clusters", "end_chs", "bpb")}, sort_keys=True))

    def test_oversize_iso_fails_on_the_completed_iso(self):
        limit = self.manifest["iso"]["sectors"] - 1
        target = WORK / "oversize"
        with self.assertRaisesRegex(ValueError, rf"over the {limit}-sector CD-R limit"):
            cd.build(self.f["image"], target, self.f["setup"], config_path=self.f["config_path"],
                     limit_sectors=limit, allow_source_mismatch=True)
        self.assertFalse((target / "ciukios-setup.iso").exists())
        shutil.rmtree(target, ignore_errors=True)


class KernelModelTests(unittest.TestCase):
    def test_cdview_identification_and_setup_storage(self):
        f = Fixture.get()
        disks = WORK / "disks"
        shutil.rmtree(disks, ignore_errors=True)
        disks.mkdir()
        blank, fat, ntfs = make_fixture_disks(disks)
        fs = ROOT / "src/kernel/fs"
        sources = [fs / n for n in ("fs_port.c", "blkreq.c", "cache.c", "partition.c", "path.c", "fat.c", "vfs.c", "fs_ro.c",
                                    "iso9660.c", "ntfs_fs.c", "ntfs_ro.c", "mount.c", "cdview.c")]
        sources += [ROOT / "src/kernel/core/bootlog.c", ROOT / "src/kernel/core/blackbox.c", ROOT / "src/kernel/drivers/blkpart.c",
                    ROOT / "src/kernel/lib/sha256.c", ROOT / "src/kernel/lib/fmt.c", ROOT / "tests/host/fs/fake.c",
                    ROOT / "apps/setup/setup_info.c", ROOT / "tests/host/cd_media_test.c"]
        binary = WORK / "cd_media_test"
        subprocess.run(["nice", "-n", "19", "clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                        "-DFS_HOST", "-D_POSIX_C_SOURCE=200809L", "-pthread", "-I", str(ROOT / "src/kernel/include"),
                        "-I", str(fs), *map(str, sources), "-o", str(binary)], check=True, cwd=ROOT)
        manifest = json.loads((f["a"] / "cd-manifest.json").read_text())
        disk = json.loads((f["image"].parent / "build-manifest.json").read_text())
        files = len(manifest["boot_image"]["files"])
        record = (f"[setup] media ok iso={manifest['content_digest']} image={disk['image_sha256']} "
                  f"payload={files} mode=placeholder")
        result = subprocess.run(["nice", "-n", "19", str(binary), str(f["a"] / "ciukios-setup.iso"), "safe=0 serial=1 setup=1\n",
                                 hex(IDENTITY_ADDRESS), IDENTITY, str(blank), str(fat), str(ntfs), record],
                                capture_output=True, text=True, env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
        sys.stdout.write(result.stdout)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for disk_number in range(3):
            self.assertIn(f"[setup] writes disk={disk_number} count=0", result.stdout)
        self.assertIn(", 0 failures", result.stdout)


class SuiteDefinitionTests(unittest.TestCase):
    def test_f9_cd_smoke_case_definitions(self):
        suite = json.loads((ROOT / "tests/suites/f9-cd-smoke.json").read_text())
        self.assertEqual(suite["image"], "setup-cd")
        cases = {c["id"]: c for c in suite["cases"]}
        for profile in ("qemu-t23", "qemu-desktop-1998", "qemu-desktop-2002"):
            case = cases[f"setup-cd-{profile}"]
            self.assertEqual((case["profile"], case["boot"]), (profile, "cdrom"))
            self.assertEqual([d["fixture"] for d in case["disks"]], ["blank", "occupied-fat"])
            serial = " ".join(case["expected"]["serial_contains"])
            for text in ("[storage] setup_mode=1 writable=0", "[setup] media ok iso=", "[setup] writes disk=0 count=0",
                         "[setup] writes disk=1 count=0", "L:"):
                self.assertIn(text, serial)
        refused = cases["setup-cd-memory-64"]
        self.assertEqual(refused["ram_mib"], 64)
        self.assertIn("[setup] refused reason=memory usable_kib=", " ".join(refused["expected"]["serial_contains"]))
        self.assertIn("[setup] media ok", " ".join(refused["expected"]["serial_absent"]))


def tearDownModule():
    # The fixture media are large; keep them only on request (diagnosis).
    if Fixture.built and not os.environ.get("CIUKI_KEEP_CD_FIXTURES"):
        shutil.rmtree(WORK, ignore_errors=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
