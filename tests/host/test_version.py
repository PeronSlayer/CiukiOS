"""T0 version identity: config/version.json is the single source; no QEMU, no image build."""
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


version = load("ciuki_version_test", ROOT / "scripts/ciuki_version.py")
kernel_build = load("kernel_build_version_test", ROOT / "scripts/build_kernel.py")
image = load("image_build_version_test", ROOT / "scripts/build_image.py")


class VersionTests(unittest.TestCase):
    def setUp(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="version-", dir=parent)
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)

    def test_source_file_and_derived_string(self):
        current = version.load()
        self.assertRegex(current["number"], r"^[0-9]+\.[0-9]+\.[0-9]+$")
        self.assertEqual(current["display"], f"{current['number']} {current['stage']}")

    def test_malformed_files_are_refused(self):
        for text in ('{"version": "0.9", "stage": "pre-alpha"}',
                     '{"version": "0.9.0", "stage": "Pre Alpha"}',
                     '{"version": "0.9.0", "stage": "a\'b"}',
                     '{"version": "0.9.0"}', '{}'):
            path = self.work / "version.json"
            path.write_text(text)
            with self.subTest(text=text), self.assertRaises(SystemExit):
                version.load(path)

    def test_hand_written_files_agree_with_the_source(self):
        current = version.load()
        self.assertEqual((ROOT / "VERSION").read_text(), f"CiukiOS {current['display']}\n")
        headings = [line for line in (ROOT / "CHANGELOG.md").read_text().splitlines() if line.startswith("## ")]
        self.assertIn(current["display"], headings[0])
        self.assertIn(current["display"], (ROOT / "README.md").read_text())

    def test_documentation_carries_the_current_version_and_identity(self):
        """f2-97: the docs that describe the image and the About credits agree with config/version.json."""
        current = version.load()
        notes = (ROOT / f"docs/release-notes-1.0-{current['stage']}.md").read_text()
        self.assertIn(f"{current['number'].rsplit('.', 1)[0]} {current['stage'].title()}", notes)
        for name in ("README.md", "docs/build-and-run.md"):
            self.assertIn(current["display"], (ROOT / name).read_text(), name)
        # No stale pre-alpha version in the sections that describe the build.
        self.assertNotIn("0.9.2 pre-alpha)", (ROOT / "docs/build-and-run.md").read_text())
        credits = (ROOT / "docs/credits.md").read_text()
        self.assertIn("Classic computing, one woof at a time.", credits)
        # About shows the kernel's uname version, which is this file's display string (build_kernel passes it).
        shell = (ROOT / "apps/desktop/shell.c").read_text()
        self.assertIn('"Dedicated to Ciuki.  Version %s", d->version', shell)

    def test_kernel_and_image_builders_read_the_same_source(self):
        self.assertEqual(kernel_build.read_version(), version.load())
        self.assertEqual(image.read_version(), version.load())

    def test_manifest_records_the_kernel_build_version(self):
        kernel = self.work / "VMM.ELF"
        kernel.write_bytes(b"\x7fELF" + bytes(60))
        record = {"utc_epoch": 1, "kernel_sha256": image.sha256(kernel), "version": "9.9.9 test"}
        (self.work / "build-clock.json").write_text(json.dumps(record))
        with patch.object(image, "git_identity", return_value=("unknown", "unknown")):
            image.write_manifest(self.work / "disk.img", kernel, "image-hash", [], {})
        manifest = json.loads((self.work / "build-manifest.json").read_text())
        self.assertEqual(manifest["version"], "9.9.9 test")
        self.assertEqual(manifest["schema_version"], 1)
        # A record from a kernel built before the version existed falls back to the source.
        del record["version"]
        (self.work / "build-clock.json").write_text(json.dumps(record))
        with patch.object(image, "git_identity", return_value=("unknown", "unknown")):
            image.write_manifest(self.work / "disk.img", kernel, "image-hash", [], {})
        manifest = json.loads((self.work / "build-manifest.json").read_text())
        self.assertEqual(manifest["version"], version.load()["display"])

    @unittest.skipUnless(shutil.which("nasm"), "requires nasm")
    def test_loader_menu_title_carries_the_define_and_fits_80_columns(self):
        for define, shown in ((f"-dCIUKI_VERSION='{version.load()['display']}'", version.load()["display"]),
                              (None, "unknown")):
            binary = self.work / "loader.bin"
            command = ["nasm", "-f", "bin", *([define] if define else []),
                       str(ROOT / "src/boot/ciukldr.asm"), "-o", str(binary)]
            subprocess.run(command, cwd=ROOT, check=True)
            data = binary.read_bytes()
            self.assertEqual(len(data) % 512, 0)
            match = re.search(rb"CiukiOS ([^:\r\n]+): N Normal / S Safe / L Log / H Hardware / P Probe \(3 s\)\r\n\0", data)
            with self.subTest(version=shown):
                self.assertIsNotNone(match)
                self.assertEqual(match.group(1).decode(), shown)
                self.assertLessEqual(len(match.group(0)) - 3, 79)


if __name__ == "__main__":
    unittest.main()
