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
        self.assertEqual(current["number"], 1)
        self.assertRegex(current["core"], r"^[0-9]+\.[0-9]+\.[0-9]+$")
        self.assertEqual(current["version"], "1.0.0-beta.1")
        self.assertEqual(current["human_name"], "CiukiOS 1.0.0 Beta 1")

    def test_malformed_files_are_refused(self):
        for text in ('{"version": "0.9", "stage": "pre-alpha"}',
                     '{"version": "0.9.0", "stage": "Pre Alpha"}',
                     '{"version": "0.9.0", "stage": "a\'b"}',
                     '{"version": "1.0.0-pre-alpha"}', '{"version": "01.0.0"}',
                     '{"version": "1.0.0-beta.01"}', '{"version": "1.0.0-beta.0"}', '{}'):
            path = self.work / "version.json"
            path.write_text(text)
            with self.subTest(text=text), self.assertRaises(SystemExit):
                version.load(path)

    def test_hand_written_files_agree_with_the_source(self):
        current = version.load()
        self.assertEqual((ROOT / "VERSION").read_text(), f"CiukiOS {current['display']}\n")
        headings = [line for line in (ROOT / "CHANGELOG.md").read_text().splitlines() if line.startswith("## ")]
        self.assertTrue(any(current["version"] in h for h in headings))
        self.assertIn(current["version"], (ROOT / "README.md").read_text())

    def test_documentation_carries_the_current_version_and_identity(self):
        """f2-97: the docs that describe the image and the About credits agree with config/version.json."""
        current = version.load()
        notes = (ROOT / f"docs/release-notes-1.0-{current['stage']}.md").read_text()
        self.assertIn(current["human_name"], notes)
        for name in ("README.md", "docs/build-and-run.md"):
            self.assertIn(current["version"], (ROOT / name).read_text(), name)
        # No stale pre-alpha version in the sections that describe the build.
        self.assertNotIn("0.9.2 pre-alpha)", (ROOT / "docs/build-and-run.md").read_text())
        credits = (ROOT / "docs/credits.md").read_text()
        self.assertIn("Classic computing, one woof at a time.", credits)
        # About shows the kernel's uname version, which is this file's display string (build_kernel passes it).
        shell = (ROOT / "apps/desktop/shell.c").read_text()
        self.assertIn('"Dedicated to Ciuki.  Version %s", human_version', shell)

    def test_kernel_and_image_builders_read_the_same_source(self):
        self.assertEqual(kernel_build.read_version(), version.load())
        self.assertEqual(image.read_version(), version.load())
        uname = (ROOT / "src/kernel/proc/syscalls_file.c").read_text()
        self.assertIn(".release = CIUKI_BUILD_ID, .version = CIUKI_VERSION", uname)
        self.assertIn('klog("CiukiOS %s", CIUKI_VERSION_FULL)', (ROOT / "src/kernel/core/main.c").read_text())
        setup_build = (ROOT / "apps/setup/build_setup.py").read_text()
        self.assertIn('ROOT / "scripts/ciuki_version.py"', setup_build)
        self.assertIn('version["human_name"]', setup_build)
        self.assertIn('"config/version.json"', setup_build)

    def test_manifest_records_the_kernel_build_version(self):
        kernel = self.work / "VMM.ELF"
        kernel.write_bytes(b"\x7fELF" + bytes(60))
        record = {"utc_epoch": 1, "kernel_sha256": image.sha256(kernel), "version": "9.9.9-beta.1", "version_full": "9.9.9-beta.1+build.123.gabc"}
        (self.work / "build-clock.json").write_text(json.dumps(record))
        with patch.object(image, "git_identity", return_value=("unknown", "unknown")):
            image.write_manifest(self.work / "disk.img", kernel, "image-hash", [], {})
        manifest = json.loads((self.work / "build-manifest.json").read_text())
        self.assertEqual(manifest["version"], "9.9.9-beta.1")
        self.assertEqual(manifest["version_full"], "9.9.9-beta.1+build.123.gabc")
        self.assertEqual(manifest["schema_version"], 1)
        # A record from a kernel built before the version existed falls back to the source.
        del record["version"]
        (self.work / "build-clock.json").write_text(json.dumps(record))
        with patch.object(image, "git_identity", return_value=("unknown", "unknown")):
            image.write_manifest(self.work / "disk.img", kernel, "image-hash", [], {})
        manifest = json.loads((self.work / "build-manifest.json").read_text())
        self.assertEqual(manifest["version"], version.load()["version"])

    def test_precedence(self):
        ordered = ['1.0.0-alpha', '1.0.0-alpha.1', '1.0.0-alpha.beta',
                   '1.0.0-beta', '1.0.0-beta.2', '1.0.0-beta.11',
                   '1.0.0-rc.1', '1.0.0', '1.0.1', '1.1.0', '2.0.0', '10.0.0']
        for i, left in enumerate(ordered):
            for j, right in enumerate(ordered):
                self.assertEqual(version.compare(left, right), (i > j) - (i < j))
        self.assertEqual(version.compare('1.0.0+build.1.ga', '1.0.0+build.2.gb'), 0)
        self.assertLess(version.compare('1.0.0-1', '1.0.0-a'), 0)
        self.assertEqual(version.with_build('1.0.0-beta.1', 'abc-dirty', 'abc'),
                         '1.0.0-beta.1+build.abc-dirty.gabc')

    def test_human_names(self):
        for value, expected in [('0.9.2', 'CiukiOS 0.9.2'), ('1.0.0', 'CiukiOS 1.0.0'),
                                ('1.0.0-alpha.2', 'CiukiOS 1.0.0 Alpha 2'),
                                ('1.0.0-rc.1', 'CiukiOS 1.0.0 Release Candidate 1')]:
            self.assertEqual(version.identity(value)['human_name'], expected)
        for value in ('1.0.0-beta', '1.0.0+build.1', '1.0.0-beta.1\n', '1.0.٠'):
            with self.assertRaises(ValueError):
                version.identity(value)

    @unittest.skipUnless(shutil.which('clang'), 'requires clang')
    def test_runtime_human_names(self):
        source = self.work / 'version_name.c'
        source.write_text('''#include "apps/desktop/version_name.h"
#include <assert.h>
int main(void) {
    char out[96];
    ciuki_version_name(out, sizeof(out), "1.0.0-beta.1");
    assert(!strcmp(out, "1.0.0 Beta 1"));
    ciuki_version_name(out, sizeof(out), "1.0.0-rc.10");
    assert(!strcmp(out, "1.0.0 Release Candidate 10"));
    ciuki_version_name(out, sizeof(out), "1.0.0");
    assert(!strcmp(out, "1.0.0"));
    ciuki_version_name(out, 4, "1.0.0-alpha.2");
    assert(!strcmp(out, "1.0"));
    ciuki_version_name(out, sizeof(out), "");
    assert(!strcmp(out, "unknown"));
    return 0;
}
''')
        binary = self.work / 'version_name'
        subprocess.run(['clang', '-std=c17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-fno-sanitize-recover=undefined', '-I', str(ROOT), str(source),
                        '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

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
