"""r1: offline release fixtures, real image conversions and publication refusals.

Only Git source-oracle responses and GitHub transport are replaced. mtools,
xorriso and qemu-img operate on a synthetic 40 MiB FAT32 disk and real ISO.
No QEMU process, image build recipe, Git mutation or network is used.
"""

from contextlib import redirect_stdout
from copy import deepcopy
import hashlib
import io
import json
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import release_stable as release


COMMIT = "a" * 40


def run(*args):
    return subprocess.run([str(a) for a in args], check=True, capture_output=True)


def row(path, name=None):
    return {"path": name or path.name, "size": path.stat().st_size, "sha256": release.digest(path)}


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value), encoding="utf-8")


def fixture(root, stage="alpha"):
    directory = root / "build/f0"
    directory.mkdir(parents=True)
    volume = root / "fixture.fat"
    with volume.open("wb") as stream:
        stream.truncate(40 * 1024 * 1024)
    run("mkfs.fat", "--invariant", "-F", "32", "-s", "1", volume)
    payloads = {"/system/VMM.ELF": b"synthetic fixture kernel\n",
                "/system/licenses/WINE-LGPL.txt": b"fixture component notice\n"}
    for name in ("/system", "/system/licenses"):
        run("mmd", "-i", volume, "::" + name)
    source = root / "payload"
    for name, data in payloads.items():
        source.write_bytes(data)
        run("mcopy", "-i", volume, source, "::" + name)
    image = directory / "ciukios.img"
    with image.open("wb") as stream, volume.open("rb") as source_stream:
        mbr = bytearray(512)
        struct.pack_into("<B3sB3sII", mbr, 446, 0x80, b"\0" * 3, 0x0C, b"\0" * 3,
                         2048, volume.stat().st_size // 512)
        mbr[510:] = b"\x55\xaa"
        stream.write(mbr)
        stream.seek(2048 * 512)
        shutil.copyfileobj(source_stream, stream)
    volume.unlink()
    disk = {"schema_version": 1, "version": f"1.0.0 {stage}", "git_revision": COMMIT,
            "git_dirty": False, "image": image.name, "image_sha256": release.digest(image),
            "payloads": [{"path": n, "size": len(b), "sha256": hashlib.sha256(b).hexdigest()}
                         for n, b in payloads.items()]}
    tree = root / "iso-tree"
    (tree / "SOURCES").mkdir(parents=True)
    (tree / "LICENSES").mkdir()
    shutil.copyfile(image, tree / "CIUKBOOT.IMG")
    for name in ("L0001.TXT", "INDEX.TXT", "SOURCES.TXT"):
        (tree / "LICENSES" / name).write_bytes(b"fixture component notice\n")
    wine = tree / "SOURCES/WINE100.TXZ"
    with tarfile.open(wine, "w:xz") as archive:
        data = b"fixture upstream Wine corresponding source\n"
        item = tarfile.TarInfo("wine-10.0/source.c")
        item.size = len(data)
        archive.addfile(item, io.BytesIO(data))
    config = {"source_tree": {"include": ["scripts/", "config/", "third_party/wine/"]},
              "archives": {"CIUKIOS.ZIP": {"kind": "source-tree"},
                           "WINE100.TXZ": {"kind": "pinned", "pin": "config/wine-pin.json", "key": "archive_sha256"}},
              "components": [{"id": "system", "class": "copyleft", "paths": ["/system/"],
                              "archives": ["CIUKIOS.ZIP", "WINE100.TXZ"],
                              "licence_files": ["third_party/wine/COPYING.LIB"]}]}
    blobs = {"config/version.json": json.dumps({"version": "1.0.0", "stage": stage}).encode(),
             "config/cd-components.json": json.dumps(config).encode(),
             "config/wine-pin.json": json.dumps({"archive_sha256": release.digest(wine)}).encode(),
             "config/wine-upper.json": b"{}\n", "scripts/win32/recipe.py": b"# fixture recipe\n",
             "third_party/wine/patches/0001.patch": b"fixture patch\n",
             "third_party/wine/SOURCE-OFFER.txt": b"fixture source offer\n",
             "third_party/wine/COPYING.LIB": b"fixture component notice\n",
             "CHANGELOG.md": f"# Changelog\n\n## 1.0.0 {stage} (unreleased)\n\nFixture release.\n\n## 0.9.2\nOld.\n".encode()}
    source_files = {}
    with zipfile.ZipFile(tree / "SOURCES/CIUKIOS.ZIP", "w") as archive:
        for name, data in blobs.items():
            if name == "CHANGELOG.md":
                continue
            entry = zipfile.ZipInfo("ciukios/" + name)
            entry.create_system = 3
            entry.external_attr = 0o100644 << 16
            archive.writestr(entry, data)
            source_files[entry.filename] = ("100644", hashlib.sha1(f"blob {len(data)}\0".encode() + data).hexdigest())
    # Release text is read from the built commit, outside this fixture's source-tree includes.
    for channel in ("alpha", "beta"):
        blobs[f"docs/release-notes-1.0-{channel}.md"] = f"# Fixture {channel} notes\n\n{channel} limitations.\n".encode()
    for package in release.build_games_disk.PACKAGES:
        blobs[f"third_party/games/{package}/UPSTREAM.json"] = json.dumps(
            {"install": {"files": []}, "license_files": []}).encode()
    iso = directory / "ciukios-setup.iso"
    run("xorriso", "-as", "mkisofs", "-quiet", "-iso-level", "1", "-o", iso, tree)
    cd = {"schema_version": 1,
          "build": {"version": disk["version"], "git_revision": COMMIT, "git_dirty": False,
                    "source_revision": COMMIT, "source_matches_build": True,
                    "disk_image_sha256": disk["image_sha256"]},
          "iso": {"sha256": release.digest(iso), "bytes": iso.stat().st_size},
          "iso_tree": [row(p, p.relative_to(tree).as_posix()) for p in tree.rglob("*") if p.is_file()],
          "boot_image": {"iso_path": "CIUKBOOT.IMG", "files": deepcopy(disk["payloads"])},
          "archives": {p.name: row(p) for p in (tree / "SOURCES").iterdir()},
          "components": {"present": ["system"]}}
    policy = {"main": "suspended", "stable": {
        "requires_owner_approval": True, "owner_approved": True,
        "approval_record": "synthetic owner-approval fixture",
        "approved_image_sha256": disk["image_sha256"], "source_commit": COMMIT,
        "private_roots": list(release.PRIVATE_ROOTS)}}
    write_json(directory / "build-manifest.json", disk)
    write_json(directory / "cd-manifest.json", cd)
    write_json(root / "config/release-policy.json", policy)
    return disk, cd, policy, blobs, source_files


def validate_ovf(test, xml, vmdk, capacity):
    """Independent checks of DSP8023 1.0.0 / CIM schema rules used here.

    Restricted to our generated descriptor, not a replacement for the full
    DMTF schema or an importer. Namespace and sequence checks precede semantic
    device checks so wrong namespaces cannot pass by matching local names.
    """
    ns = {"o": "http://schemas.dmtf.org/ovf/envelope/1",
          "r": "http://schemas.dmtf.org/wbem/wscim/1/cim-schema/2/CIM_ResourceAllocationSettingData",
          "v": "http://schemas.dmtf.org/wbem/wscim/1/cim-schema/2/CIM_VirtualSystemSettingData"}
    attr = lambda name: "{" + ns["o"] + "}" + name
    doc = ET.fromstring(xml)
    test.assertEqual(doc.tag, attr("Envelope"))
    test.assertEqual([n.tag for n in doc], [attr(n) for n in (
        "References", "DiskSection", "NetworkSection", "VirtualSystem")])
    for section in doc.findall(".//o:DiskSection", ns) + doc.findall(".//o:NetworkSection", ns) + \
            doc.findall(".//o:VirtualSystem", ns) + doc.findall(".//o:OperatingSystemSection", ns) + \
            doc.findall(".//o:VirtualHardwareSection", ns):
        test.assertEqual(section[0].tag, attr("Info"))
        test.assertTrue(section[0].text)
    ref = doc.find("o:References/o:File", ns)
    test.assertEqual(set(ref.attrib), {attr("id"), attr("href"), attr("size")})
    test.assertEqual(ref.get(attr("href")), vmdk.name)
    test.assertEqual(int(ref.get(attr("size"))), vmdk.stat().st_size)
    disk = doc.find("o:DiskSection/o:Disk", ns)
    test.assertEqual(disk.get(attr("fileRef")), ref.get(attr("id")))
    test.assertEqual(int(disk.get(attr("capacity"))), capacity)
    test.assertEqual(disk.get(attr("capacityAllocationUnits")), "byte")
    test.assertEqual(disk.get(attr("format")), "http://www.vmware.com/interfaces/specifications/vmdk.html#streamOptimized")
    system = doc.find("o:VirtualSystem", ns)
    test.assertTrue(system.get(attr("id")))
    test.assertEqual(int(system.find("o:OperatingSystemSection", ns).get(attr("id"))), 1)
    hardware = system.find("o:VirtualHardwareSection", ns)
    test.assertEqual([n.tag for n in hardware[:2]], [attr("Info"), attr("System")])
    spec = hardware.find("o:System", ns)
    test.assertEqual([n.tag for n in spec], ["{" + ns["v"] + "}" + n for n in (
        "ElementName", "InstanceID", "VirtualSystemIdentifier", "VirtualSystemType")])
    test.assertEqual(spec.find("v:VirtualSystemType", ns).text, "vmx-07")
    devices = {}
    ids = set()
    allowed = {"Address", "AddressOnParent", "AllocationUnits", "AutomaticAllocation", "Connection",
               "ElementName", "HostResource", "InstanceID", "Parent", "ResourceSubType", "ResourceType", "VirtualQuantity"}
    for item in hardware.findall("o:Item", ns):
        values = {}
        for n in item:
            test.assertTrue(n.tag.startswith("{" + ns["r"] + "}"))
            local = n.tag.split("}")[1]
            test.assertIn(local, allowed)
            test.assertNotIn(local, values)
            values[local] = n.text
        test.assertEqual(list(values), sorted(values))
        for required in ("InstanceID", "ResourceType"):
            test.assertIn(required, values)
        test.assertNotIn(values["InstanceID"], ids)
        ids.add(values["InstanceID"])
        for numeric in ("ResourceType", "VirtualQuantity", "Address", "AddressOnParent"):
            if numeric in values:
                test.assertRegex(values[numeric], r"^[0-9]+$")
        if "AutomaticAllocation" in values:
            test.assertIn(values["AutomaticAllocation"], ("true", "false", "0", "1"))
        kind = int(values["ResourceType"])
        test.assertNotIn(kind, devices)
        devices[kind] = values
    test.assertEqual(set(devices), {3, 4, 5, 10, 17, 24, 35})
    test.assertEqual(devices[3]["VirtualQuantity"], "1")
    test.assertEqual(devices[4]["VirtualQuantity"], "512")
    test.assertEqual(devices[4]["AllocationUnits"], "byte * 2^20")
    test.assertEqual(devices[5]["ResourceSubType"], "PIIX4")
    test.assertEqual(devices[17]["Parent"], devices[5]["InstanceID"])
    test.assertEqual(devices[17]["HostResource"], "ovf:/disk/" + disk.get(attr("diskId")))
    network = doc.find("o:NetworkSection/o:Network", ns)
    test.assertEqual(devices[10]["Connection"], network.get(attr("name")))
    test.assertEqual(devices[10]["ResourceSubType"], "E1000")
    test.assertIn(devices[35]["ResourceSubType"], ("AC97", "ensoniq.es1371"))


@unittest.skipUnless(all(shutil.which(t) for t in ("mkfs.fat", "mmd", "mcopy", "mdir", "xorriso", "qemu-img")),
                     "release fixture tools required")
class StableReleaseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        cls.temporary = tempfile.TemporaryDirectory(dir=parent, prefix="stable-release-test-")
        cls.root = Path(cls.temporary.name)
        cls.disk, cls.cd, cls.policy, cls.blobs, cls.source_files = fixture(cls.root)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def setUp(self):
        self.calls = []
        self.network = []
        original_command = release.command
        def transport(*args, root=release.ROOT):
            self.calls.append(args)
            if args[:2] == ("git", "tag"):
                if "-alpha." in args[-1]:
                    return "v1.0.0-alpha.1\nv0.9.2-alpha.8"
                if "-beta." in args[-1]:
                    return "v1.0.0-beta.1\nv0.9.2-beta.8"
                return "v1.0.0-stable.1\nv0.9.2-stable.8"
            if args[0] == "gh":
                self.network.append(args)
                if args[1] == "api":
                    if "-alpha." in args[-1]:
                        return '[{"ref": "refs/tags/v1.0.0-alpha.2"}]'
                    if "-beta." in args[-1]:
                        return '[{"ref": "refs/tags/v1.0.0-beta.2"}]'
                    return '[{"ref": "refs/tags/v1.0.0-stable.2"}]'
                return "https://example.invalid/synthetic-release"
            return original_command(*args, root=root)
        for name, value in (("command", transport),
                            ("git_bytes", lambda root, commit, name: self.blobs[name]),
                            ("source_files", lambda *args: deepcopy(self.source_files))):
            mock = patch.object(release, name, value)
            mock.start()
            self.addCleanup(mock.stop)
        self.reset_metadata()
        self.addCleanup(self.reset_metadata)
        releases = self.root / "build/releases"
        if releases.exists():
            shutil.rmtree(releases)
        self.addCleanup(lambda: shutil.rmtree(releases, ignore_errors=True))

    def reset_metadata(self):
        write_json(self.root / "config/release-policy.json", self.policy)
        write_json(self.root / "build/f0/build-manifest.json", self.disk)
        write_json(self.root / "build/f0/cd-manifest.json", self.cd)

    def info(self):
        return release.inputs(self.root, "1.0.0", self.disk["image_sha256"])

    def test_inputs_and_refusals(self):
        self.assertEqual(self.info()["commit"], COMMIT)
        cases = [("policy", ("stable", "owner_approved"), False, "approval"),
                 ("policy", ("stable", "requires_owner_approval"), False, "approval"),
                 ("policy", ("stable", "approval_record"), "", "approval"),
                 ("policy", ("stable", "approved_image_sha256"), "f" * 64, "approval"),
                 ("policy", ("stable", "source_commit"), "f" * 40, "source commit"),
                 ("disk", ("git_dirty",), True, "dirty"),
                 ("disk", ("git_dirty",), "0", "dirty"),
                 ("disk", ("git_revision",), "unknown", "source commit"),
                 ("disk", ("image_sha256",), "0" * 64, "approval"),
                 ("disk", ("version",), "0.9.2 alpha", "version"),
                 ("cd", ("build", "git_dirty"), True, "dirty"),
                 ("cd", ("build", "source_revision"), "f" * 40, "source commit"),
                 ("cd", ("build", "git_revision"), "f" * 40, "source commit"),
                 ("cd", ("build", "source_matches_build"), False, "source commit"),
                 ("cd", ("build", "disk_image_sha256"), "f" * 64, "approval"),
                 ("cd", ("iso", "sha256"), "f" * 64, "hash/size"),
                 ("cd", ("iso", "bytes"), 0, "hash/size")]
        for which, keys, value, message in cases:
            with self.subTest(which=which, keys=keys):
                data = deepcopy(getattr(self, which))
                target = data
                for key in keys[:-1]:
                    target = target[key]
                target[keys[-1]] = value
                path = {"policy": "config/release-policy.json", "disk": "build/f0/build-manifest.json",
                        "cd": "build/f0/cd-manifest.json"}[which]
                write_json(self.root / path, data)
                with self.assertRaisesRegex(release.Refusal, message):
                    self.info()
                self.reset_metadata()
        with self.assertRaisesRegex(release.Refusal, "version"):
            release.inputs(self.root, "../1", self.disk["image_sha256"])
        with self.assertRaisesRegex(release.Refusal, "SHA-256"):
            release.inputs(self.root, "1.0.0", "partial")
        self.assertFalse(release.clean(None))
        self.assertTrue(release.clean(0))

    def test_real_audit(self):
        with tempfile.TemporaryDirectory(dir=self.root) as temporary:
            tree, sources, notices = release.audit_inputs(self.info(), Path(temporary))
            self.assertTrue((tree / "CIUKBOOT.IMG").is_file())
            self.assertEqual({p.name for p in sources}, {"CIUKIOS.ZIP", "WINE100.TXZ"})
            self.assertEqual(len(notices), 3)

    def test_dry_run_is_offline_and_does_not_package(self):
        with io.StringIO() as output, redirect_stdout(output), patch.object(release, "package") as package:
            destination = release.release(self.root, "1.0.0", self.disk["image_sha256"], True)
            printed = output.getvalue()
        self.assertFalse(self.network)
        package.assert_not_called()
        self.assertFalse(destination.exists())
        self.assertFalse((self.root / "build/releases").exists())
        self.assertIn("v1.0.0-stable.2", printed)
        for artifact in release.artifacts({"version": "1.0.0"}):
            self.assertIn(artifact, printed)
        self.assertIn("streamOptimized", printed)
        self.assertNotIn("--prerelease", printed)
        self.assertIn("--target " + COMMIT, printed)
        self.assertFalse(list((self.root / "build/codex").glob("stable-audit-*")))

    def test_package_every_conversion_checksums_and_publication(self):
        previous = self.root / "build/releases/v0.9.2-stable.1"
        previous.mkdir(parents=True)
        (previous / "old").write_text("previous release")
        with io.StringIO() as output, redirect_stdout(output):
            destination = release.release(self.root, "1.0.0", self.disk["image_sha256"], False)
        self.assertEqual(destination.name, "v1.0.0-stable.3")
        self.assertFalse(previous.exists())
        self.assertEqual({p.name for p in destination.iterdir()}, set(release.artifacts({"version": "1.0.0"})))
        for line in (destination / "SHA256SUMS").read_text().splitlines():
            sha, name = line.split("  ")
            self.assertEqual(sha, release.digest(destination / name))
        disk_hash = self.disk["image_sha256"]
        stem = "ciukios-1.0.0"
        with zipfile.ZipFile(destination / (stem + "-disk.zip")) as archive:
            self.assertEqual(set(archive.namelist()), {"ciukios.img", "README.txt"})
            with archive.open("ciukios.img") as stream:
                self.assertEqual(hashlib.file_digest(stream, "sha256").hexdigest(), disk_hash)
            self.assertIn(b"CMOV", archive.read("README.txt"))
        with tempfile.TemporaryDirectory(dir=self.root) as temporary:
            temp = Path(temporary)
            for suffix, disk_name, fmt in (("-qemu.zip", "ciukios.qcow2", "qcow2"),
                                            ("-vmware.zip", "ciukios.vmdk", "vmdk"),
                                            ("-virtualbox.zip", "ciukios.vdi", "vdi")):
                with zipfile.ZipFile(destination / (stem + suffix)) as archive:
                    archive.extract(disk_name, temp)
                    self.assertEqual(archive.testzip(), None)
                    if fmt == "qcow2":
                        self.assertTrue(archive.getinfo("run-qemu.sh").external_attr >> 16 & stat.S_IXUSR)
                        launcher = archive.read("run-qemu.sh").decode()
                        self.assertIn("pc-i440fx-9.2", launcher)
                        self.assertIn("-cpu pentium3 -m 512 -smp 1", launcher)
                        self.assertIn("-rtc base=localtime", launcher)
                        self.assertIn("hvf", launcher)
                        self.assertIn(b"whpx", archive.read("run-qemu.cmd"))
                        self.assertNotIn(b"\n", archive.read("run-qemu.cmd").replace(b"\r\n", b""))
                    elif fmt == "vdi":
                        self.assertEqual(set(archive.namelist()), {"ciukios.vdi", "create-vm.sh", "create-vm.cmd", "README.txt"})
                        self.assertTrue(archive.getinfo("create-vm.sh").external_attr >> 16 & stat.S_IXUSR)
                        script = archive.read("create-vm.sh").decode() + archive.read("create-vm.cmd").decode() \
                            + archive.read("README.txt").decode()
                        for key in ("--memory 512", "--ioapic off", "--controller PIIX4", "--nictype1 Am79C973",
                                    "--audiocontroller ac97", "--graphicscontroller vboxvga", "ciukios.vdi"):
                            self.assertIn(key, script)
                        self.assertIn("%VBM%", archive.read("create-vm.cmd").decode())
                    else:
                        self.assertEqual(set(archive.namelist()), {"ciukios.vmdk", "ciukios.vmx", "README.txt"})
                        vmx = archive.read("ciukios.vmx")
                        for setting in (b'virtualDev = "es1371"', b'virtualDev = "e1000"', b'ide0:0.present = "TRUE"'):
                            self.assertIn(setting, vmx)
                info = json.loads(run("qemu-img", "info", "--output=json", "-f", fmt, temp / disk_name).stdout)
                self.assertEqual(info["format"], fmt)
                if fmt == "vmdk":
                    self.assertEqual(info["format-specific"]["data"]["create-type"], "monolithicSparse")
                run("qemu-img", "compare", "-f", "raw", "-F", fmt, self.root / "build/f0/ciukios.img", temp / disk_name)
            ova = destination / (stem + "-vm.ova")
            with tarfile.open(ova) as archive:
                self.assertEqual(archive.getnames(), ["ciukios.ovf", "ciukios.mf", "ciukios-stream.vmdk"])
                xml = archive.extractfile("ciukios.ovf").read()
                for name in archive.getnames():
                    with archive.extractfile(name) as source, (temp / name).open("wb") as target:
                        shutil.copyfileobj(source, target)
                for line in (temp / "ciukios.mf").read_text().splitlines():
                    name, sha = re.fullmatch(r"SHA1\((.+)\)= ([0-9a-f]{40})", line).groups()
                    self.assertEqual(sha, release.digest(temp / name, "sha1"))
            self.assertEqual(ova.read_bytes()[257:263], b"ustar\0")
            validate_ovf(self, xml, temp / "ciukios-stream.vmdk", (self.root / "build/f0/ciukios.img").stat().st_size)
            run("qemu-img", "compare", "-f", "raw", "-F", "vmdk", self.root / "build/f0/ciukios.img", temp / "ciukios-stream.vmdk")
        self.assertEqual(release.digest(destination / (stem + "-setup.iso")), self.cd["iso"]["sha256"])
        with zipfile.ZipFile(destination / (stem + "-sources.zip")) as archive:
            self.assertEqual(set(archive.namelist()), {"SOURCES/CIUKIOS.ZIP", "SOURCES/WINE100.TXZ"})
        with zipfile.ZipFile(destination / (stem + "-notices.zip")) as archive:
            self.assertEqual(len(archive.namelist()), 3)
        notes = (destination / "RELEASE-NOTES.md").read_text()
        for value in (COMMIT, disk_hash, "Fixture release.", "not a hybrid USB", "runtime qualification"):
            self.assertIn(value, notes)
        self.assertNotIn("Old.", notes)
        self.assertEqual(len(self.network), 2)
        publish = self.network[-1]
        self.assertEqual(publish[:4], ("gh", "release", "create", "v1.0.0-stable.3"))
        self.assertNotIn("--prerelease", publish)
        self.assertEqual(publish[publish.index("--target") + 1], COMMIT)

    def test_bad_fat_manifest_and_commercial_nested_payload(self):
        with tempfile.TemporaryDirectory(dir=self.root) as temporary:
            scratch = Path(temporary)
            audit = release.PayloadAudit(self.root, [], set())
            for bad in (self.disk["payloads"][:-1], self.disk["payloads"] + [self.disk["payloads"][0]],
                        [dict(p, sha256="f" * 64) for p in self.disk["payloads"]]):
                with self.assertRaises(release.Refusal):
                    audit.fat(self.root / "build/f0/ciukios.img", scratch, bad)
            nested = scratch / "commercial.zip"
            with zipfile.ZipFile(nested, "w") as archive:
                archive.writestr("games/DOOM.WAD", b"commercial data")
            with self.assertRaisesRegex(release.Refusal, "commercial game"):
                audit.file(nested, "innocent.zip", scratch)

    def test_private_paths_content_and_archive_traversal(self):
        for name in ("legacy/local/a", "ciukios/third_party/mswin98rskt/book", "third_party/drivers/d.bin",
                     "../outside", "C:/drivers/a", "a/../../b", "a\nname"):
            with self.subTest(name=name), self.assertRaises(release.Refusal):
                release.safe_name(name)
        with tempfile.TemporaryDirectory(dir=self.root) as temporary:
            scratch = Path(temporary)
            private = self.root / "third_party/drivers/secret.bin"
            private.parent.mkdir(parents=True, exist_ok=True)
            private.write_bytes(b"private driver pack content" * 10)
            self.addCleanup(lambda: shutil.rmtree(self.root / "third_party", ignore_errors=True))
            renamed = scratch / "renamed.bin"
            renamed.write_bytes(private.read_bytes())
            audit = release.PayloadAudit(self.root, [], set())
            with self.assertRaisesRegex(release.Refusal, "private payload content"):
                audit.file(renamed, "renamed.bin", scratch)
            for name in ("../secret", "third_party/mswin98rskt/renamed"):
                archive_path = scratch / "bad.zip"
                with zipfile.ZipFile(archive_path, "w") as archive:
                    archive.writestr(name, b"payload")
                with self.assertRaises(release.Refusal):
                    audit.file(archive_path, "bad.zip", scratch)

    def test_source_zip_mismatch_is_refused(self):
        source = self.root / "iso-tree/SOURCES/CIUKIOS.ZIP"
        with tempfile.TemporaryDirectory(dir=self.root) as temporary:
            bad = Path(temporary) / "bad.zip"
            with zipfile.ZipFile(source) as original, zipfile.ZipFile(bad, "w") as archive:
                for entry in original.infolist():
                    data = original.read(entry)
                    archive.writestr(entry, data + b"changed" if entry.filename.endswith("wine-upper.json") else data)
            with self.assertRaisesRegex(release.Refusal, "differs from built commit"):
                release.verify_source_zip(bad, self.root, COMMIT, self.info()["config"])

    def test_ovf_schema_rule_checks_reject_invalid_descriptors(self):
        with tempfile.TemporaryDirectory(dir=self.root) as temporary:
            disk = Path(temporary) / "ciukios-stream.vmdk"
            disk.write_bytes(b"descriptor-only fixture")
            xml = release.ovf_descriptor(disk, 4096, "1.0.0")
            validate_ovf(self, xml, disk, 4096)
            for before, after in ((b"/ovf/envelope/1", b"/ovf/envelope/9"),
                                  (b'ovf:fileRef="file1"', b'ovf:fileRef="missing"'),
                                  (b"ovf:/disk/disk1", b"ovf:/disk/missing"),
                                  (b">512<", b">1024<"),
                                  (b">PIIX4<", b">SATA<")):
                with self.subTest(before=before), self.assertRaises((AssertionError, AttributeError)):
                    validate_ovf(self, xml.replace(before, after), disk, 4096)

    def test_tar_links_and_special_files(self):
        with tempfile.TemporaryDirectory(dir=self.root) as temporary:
            scratch = Path(temporary)
            path = scratch / "source.txz"
            audit = release.PayloadAudit(self.root, [], set())
            for target, allowed in (("../common/source.c", True), ("../../../escape", False),
                                     ("/absolute", False)):
                with tarfile.open(path, "w:xz") as archive:
                    entry = tarfile.TarInfo("upstream/tests/link.c")
                    entry.type = tarfile.SYMTYPE
                    entry.linkname = target
                    archive.addfile(entry)
                if allowed:
                    audit.file(path, "source.txz", scratch)
                else:
                    with self.assertRaisesRegex(release.Refusal, "unsafe source tar link"):
                        audit.file(path, "source.txz", scratch)
            with tarfile.open(path, "w:xz") as archive:
                entry = tarfile.TarInfo("upstream/device")
                entry.type = tarfile.CHRTYPE
                archive.addfile(entry)
            with self.assertRaisesRegex(release.Refusal, "special tar member"):
                audit.file(path, "source.txz", scratch)

    def test_accelerator_launcher_fallback_with_stub(self):
        # Execute only the generated shell glue against Python stubs, never QEMU.
        with tempfile.TemporaryDirectory(dir=self.root) as temporary:
            scratch = Path(temporary)
            launcher = scratch / "run-qemu.sh"
            launcher.write_text(release.qemu_launchers()[0])
            uname = scratch / "uname"
            uname.write_text('#!/bin/sh\ncase "$1" in -s) echo Darwin;; *) echo x86_64;; esac\n')
            uname.chmod(0o755)
            emulator = scratch / "emulator-stub"
            emulator.write_text(
                "#!" + sys.executable + "\nimport json, pathlib, sys\n"
                "if sys.argv[1:] == ['-accel', 'help']:\n print('hvf\\ntcg'); sys.exit(0)\n"
                "with pathlib.Path(__file__).with_suffix('.log').open('a') as f:\n"
                " f.write(json.dumps(sys.argv[1:]) + '\\n')\n"
                "sys.exit(1 if sys.argv[2] == 'hvf' else 0)\n")
            emulator.chmod(0o755)
            import os
            environment = dict(os.environ, QEMU=str(emulator), PATH=str(scratch) + os.pathsep + os.environ["PATH"])
            result = subprocess.run(["sh", str(launcher), "-serial", "file:session.log"], env=environment,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            calls = [json.loads(line) for line in emulator.with_suffix(".log").read_text().splitlines()]
            self.assertEqual([args[1] for args in calls], ["hvf", "tcg"])
            self.assertEqual(calls[-1][-2:], ["-serial", "file:session.log"])

    def test_upload_failure_retains_prepared_release(self):
        # The successful preparation path is exercised by the conversion test;
        # here a small stand-in isolates failure after the final atomic rename.
        def prepare(info, directory, scratch, sources, notices):
            (directory / "prepared").write_text("complete preparation stand-in")
        original = release.command
        def fail_upload(*args, **kwargs):
            if args[:3] == ("gh", "release", "create"):
                raise release.Refusal("simulated upload failure")
            return original(*args, **kwargs)
        with patch.object(release, "package", prepare), patch.object(release, "command", fail_upload), \
                io.StringIO() as output, redirect_stdout(output):
            with self.assertRaisesRegex(release.Refusal, "upload failure"):
                release.release(self.root, "1.0.0", self.disk["image_sha256"], False)
        self.assertTrue((self.root / "build/releases/v1.0.0-stable.3/prepared").is_file())

    def test_missing_source_notices_and_unlisted_iso_refused(self):
        info = self.info()
        for case in ("source", "notices", "unlisted", "components", "pin"):
            with self.subTest(case=case), tempfile.TemporaryDirectory(dir=self.root) as temporary:
                current = deepcopy(info)
                if case == "source":
                    del current["cd"]["archives"]["WINE100.TXZ"]
                elif case == "notices":
                    current["config"]["components"][0]["licence_files"].append("missing-notice.txt")
                elif case == "unlisted":
                    current["cd"]["iso_tree"] = current["cd"]["iso_tree"][:-1]
                elif case == "components":
                    current["cd"]["components"]["present"] = []
                else:
                    current["config"]["archives"]["WINE100.TXZ"]["key"] = "wrong-pin"
                with self.assertRaises((release.Refusal, KeyError)):
                    release.audit_inputs(current, Path(temporary))

    def test_failed_preparation_preserves_old_release(self):
        previous = self.root / "build/releases/v0.9.2-stable.1"
        previous.mkdir(parents=True)
        with patch.object(release, "package", side_effect=release.Refusal("conversion failed")), \
                io.StringIO() as output, redirect_stdout(output):
            with self.assertRaisesRegex(release.Refusal, "conversion failed"):
                release.release(self.root, "1.0.0", self.disk["image_sha256"], False)
        self.assertTrue(previous.exists())
        self.assertFalse((previous.parent / "v1.0.0-stable.3").exists())
        self.assertFalse(any(call[1:3] == ("release", "create") for call in self.network))

    def test_unrelated_output_and_existing_destination_refused(self):
        directory = self.root / "build/releases"
        directory.mkdir(parents=True)
        unrelated = directory / "unrelated.zip"
        unrelated.write_text("keep")
        with self.assertRaisesRegex(release.Refusal, "unrelated"):
            release.release(self.root, "1.0.0", self.disk["image_sha256"], True)
        unrelated.unlink()
        (directory / "v1.0.0-stable.2").mkdir()
        with self.assertRaisesRegex(release.Refusal, "already exists"):
            release.release(self.root, "1.0.0", self.disk["image_sha256"], True)

    def test_alpha_channel_dry_run_and_publication(self):
        with io.StringIO() as output, redirect_stdout(output):
            release.release(self.root, "1.0.0", self.disk["image_sha256"], True, "alpha")
            printed = output.getvalue()
        self.assertFalse(self.network)
        self.assertIn('"tag": "v1.0.0-alpha.2"', printed)   # dry run counts local tags only
        self.assertIn('"title": "CiukiOS 1.0.0 alpha 2"', printed)
        self.assertIn("--prerelease", printed)
        with io.StringIO() as output, redirect_stdout(output):
            destination = release.release(self.root, "1.0.0", self.disk["image_sha256"], False, "alpha")
        self.assertEqual(destination.name, "v1.0.0-alpha.3")
        publish = self.network[-1]
        self.assertEqual(publish[3], "v1.0.0-alpha.3")
        self.assertIn("--prerelease", publish)
        self.assertEqual(publish[publish.index("--title") + 1], "CiukiOS 1.0.0 alpha 3")
        self.assertIn("pre-release", (destination / "RELEASE-NOTES.md").read_text())
        self.assertIn("Fixture alpha notes", (destination / "RELEASE-NOTES.md").read_text())
        self.assertNotIn("Fixture beta notes", (destination / "RELEASE-NOTES.md").read_text())

    def test_beta_stage_dry_run_and_publication(self):
        with tempfile.TemporaryDirectory(dir=self.root, prefix="beta-") as temporary:
            root = Path(temporary)
            disk, _, _, blobs, source_files = fixture(root, "beta")
            with patch.object(release, "git_bytes", lambda root, commit, name: blobs[name]), \
                 patch.object(release, "source_files", lambda *args: deepcopy(source_files)):
                with self.assertRaisesRegex(release.Refusal, "not the alpha channel"):
                    release.inputs(root, "1.0.0", disk["image_sha256"], "alpha")
                self.assertIn("Fixture beta notes", release.inputs(
                    root, "1.0.0", disk["image_sha256"], "beta")["channel_notes"])
                with io.StringIO() as output, redirect_stdout(output):
                    release.release(root, "1.0.0", disk["image_sha256"], True, "beta")
                    printed = output.getvalue()
                self.assertFalse(self.network)
                self.assertIn('"tag": "v1.0.0-beta.2"', printed)
                self.assertIn("--prerelease", printed)
                with io.StringIO() as output, redirect_stdout(output):
                    destination = release.release(root, "1.0.0", disk["image_sha256"], False, "beta")
                self.assertEqual(destination.name, "v1.0.0-beta.3")
                self.assertIn("--prerelease", self.network[-1])
                self.assertIn("Fixture beta notes", (destination / "RELEASE-NOTES.md").read_text())
                self.assertNotIn("Fixture alpha notes", (destination / "RELEASE-NOTES.md").read_text())

    def test_notes_come_from_built_commit_and_stable_output_is_unchanged(self):
        with patch.object(release, "git_bytes", side_effect=lambda root, commit, name: self.blobs[name]) as read:
            info = release.inputs(self.root, "1.0.0", self.disk["image_sha256"], "alpha")
            read.assert_any_call(self.root, COMMIT, "docs/release-notes-1.0-alpha.md")
        info.update(title="fixture", tag="v1.0.0-alpha.1")
        self.assertIn("Fixture alpha notes", release.notes(info))
        stable = self.info()
        self.assertEqual(stable["channel_notes"], "")
        stable.update(title="fixture", tag="v1.0.0-stable.1")
        legacy = dict(stable)
        del legacy["channel_notes"]
        self.assertEqual(release.notes(stable), release.notes(legacy))
        self.assertNotIn("Fixture alpha notes", release.notes(stable))
        with self.assertRaisesRegex(release.Refusal, "not the beta channel"):
            release.inputs(self.root, "1.0.0", self.disk["image_sha256"], "beta")
        blobs = dict(self.blobs)
        blobs["docs/release-notes-1.0-alpha.md"] = b" \n"
        with patch.object(release, "git_bytes", lambda root, commit, name: blobs[name]):
            with self.assertRaisesRegex(release.Refusal, "empty alpha release notes"):
                release.inputs(self.root, "1.0.0", self.disk["image_sha256"], "alpha")

    def test_alpha_title_has_no_number_for_first_release(self):
        self.assertEqual(release.release_title("1.0.0", "alpha", 1), "CiukiOS 1.0.0 alpha")
        self.assertEqual(release.release_title("1.0.0", "alpha", 2), "CiukiOS 1.0.0 alpha 2")
        self.assertEqual(release.release_title("1.0.0", "stable", 2), "CiukiOS 1.0.0 \u2014 stable 2")
        self.assertEqual(release.next_number("1.0.0", ["v1.0.0-stable.5", "v1.0.0-alpha.1"], "alpha"), 2)

    def test_channel_refusals(self):
        with self.assertRaisesRegex(release.Refusal, "channel must be"):
            release.inputs(self.root, "1.0.0", self.disk["image_sha256"], "nightly")
        with self.assertRaisesRegex(release.Refusal, "not the beta channel"):
            release.inputs(self.root, "1.0.0", self.disk["image_sha256"], "beta")

    def test_generated_public_files_name_no_pc_models(self):
        destination = None
        with io.StringIO() as output, redirect_stdout(output):
            destination = release.release(self.root, "1.0.0", self.disk["image_sha256"], False, "alpha")
        texts = [destination / "RELEASE-NOTES.md"]
        for suffix in ("-disk.zip", "-qemu.zip", "-vmware.zip", "-virtualbox.zip"):
            with zipfile.ZipFile(destination / ("ciukios-1.0.0" + suffix)) as archive:
                texts += [archive.read(n).decode() for n in archive.namelist() if n.endswith((".txt", ".sh", ".cmd", ".vmx"))]
        texts = [t.read_text() if isinstance(t, Path) else t for t in texts]
        texts.append(release.ovf_descriptor_text() if hasattr(release, "ovf_descriptor_text") else "")
        texts.append(" ".join(release.publication_command(
            {"tag": "v", "commit": COMMIT, "title": "t", "version": "1.0.0", "channel": "alpha"}, destination)))
        for text in texts:
            self.assertIsNone(release.PUBLIC_FORBIDDEN.search(text), text[:80])
        self.assertIsNone(release.PUBLIC_FORBIDDEN.search(release.DISK_README + release.VM_README))
        self.assertIsNotNone(release.PUBLIC_FORBIDDEN.search("a ThinkPad T23"))
        self.assertIsNotNone(release.PUBLIC_FORBIDDEN.search("Armada E500"))

    def test_real_changelog_and_public_doc_name_no_pc_models(self):
        for stage in ("alpha", "beta"):
            section = release.changelog_section((ROOT / "CHANGELOG.md").read_text(), "1.0.1", stage)
            self.assertIn("Known issues", section)
            self.assertIsNone(release.PUBLIC_FORBIDDEN.search(section))
        for channel in ("alpha", "beta"):
            doc = (ROOT / f"docs/release-notes-1.0-{channel}.md").read_text()
            self.assertIsNone(release.PUBLIC_FORBIDDEN.search(doc))

    def test_cli_refuses_without_approval(self):
        result = run(sys.executable, ROOT / "scripts/release_stable.py", "--help")
        self.assertIn(b"--dry-run", result.stdout)
        # Main entry point is the actual tracked default policy, never the fixture's grant.
        result = subprocess.run([sys.executable, str(ROOT / "scripts/release_stable.py"),
                                 "--version", "1.0.0", "--approved-image-sha256", "f" * 64, "--dry-run"],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("explicit owner approval", result.stderr)


class SmallReleaseTests(unittest.TestCase):
    def test_actual_git_source_oracle(self):
        # Read-only Git query against an existing tracked file, no fixture commits.
        commit = release.command("git", "rev-parse", "HEAD", root=ROOT)
        files = release.source_files(ROOT, commit, ["config/version.json"])
        data = release.git_bytes(ROOT, commit, "config/version.json")
        expected = hashlib.sha1(f"blob {len(data)}\0".encode() + data).hexdigest()
        self.assertEqual(files, {"ciukios/config/version.json": ("100644", expected)})

    def test_numbering_and_changelog(self):
        self.assertEqual(release.next_number("1.0.0", []), 1)
        self.assertEqual(release.next_number("1.0.0", ["v1.0.0-stable.1", "v1.0.0-stable.10",
                                                     "v1.0.0-stable.01", "v1.0.1-stable.99"]), 11)
        self.assertEqual(release.changelog_section("## 1.0.0 alpha\nnew\n## 1.0.1\nold", "1.0.0"),
                         "## 1.0.0 alpha\nnew")
        with self.assertRaises(release.Refusal):
            release.changelog_section("## 1.0.01\nwrong", "1.0.0")
        both = "## 1.0.1 beta — unreleased\nb\n## 1.0.1 alpha — 2026-10-25\na"
        self.assertEqual(release.changelog_section(both, "1.0.1", "alpha"), "## 1.0.1 alpha — 2026-10-25\na")
        self.assertEqual(release.changelog_section(both, "1.0.1", "beta"), "## 1.0.1 beta — unreleased\nb")
        with self.assertRaises(release.Refusal):
            release.changelog_section(both, "1.0.1", "stable")

    def test_push_prereleases_remain_suspended(self):
        policy = json.loads((ROOT / "config/release-policy.json").read_text())
        self.assertEqual(policy["main"], "suspended")
        self.assertFalse(policy["stable"]["owner_approved"])
        self.assertIsNone(policy["stable"]["approved_image_sha256"])


if __name__ == "__main__":
    unittest.main()
