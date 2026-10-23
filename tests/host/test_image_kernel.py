"""fx-6: the image installs the kernel without debug sections; host fixtures only, no image build."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


image = load("image_kernel_build", ROOT / "scripts/build_image.py")
identity = load("image_kernel_identity", ROOT / "scripts/build_identity.py")


def fixture_kernel(debug_bytes=0x4000):
    """A small ET_EXEC i386 ELF: one PT_LOAD at file offset 0x1000, .text, .debug_info, .shstrtab."""
    text = bytes((i * 5 + 1) & 255 for i in range(0x200))
    debug = bytes((i * 11 + 7) & 255 for i in range(debug_bytes))
    names = b"\0.text\0.debug_info\0.shstrtab\0"
    offsets = {n: names.index(n.encode()) for n in (".text", ".debug_info", ".shstrtab")}
    text_off = 0x1000
    debug_off = text_off + len(text)
    names_off = debug_off + len(debug)
    sh_off = (names_off + len(names) + 3) & ~3
    data = bytearray(sh_off + 4 * 40)
    data[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HHIIIIIHHHHHH", data, 16, 2, 3, 1, 0xC0100000, 52, sh_off, 0, 52, 32, 1, 40, 4, 3)
    struct.pack_into("<8I", data, 52, 1, text_off, 0xC0100000, 0x00100000, len(text), len(text) + 64, 5, 0x1000)
    data[text_off:text_off + len(text)] = text
    data[debug_off:debug_off + len(debug)] = debug
    data[names_off:names_off + len(names)] = names
    struct.pack_into("<10I", data, sh_off + 40, offsets[".text"], 1, 6, 0xC0100000, text_off, len(text), 0, 0, 16, 0)
    struct.pack_into("<10I", data, sh_off + 80, offsets[".debug_info"], 1, 0, 0, debug_off, len(debug), 0, 0, 1, 0)
    struct.pack_into("<10I", data, sh_off + 120, offsets[".shstrtab"], 3, 0, 0, names_off, len(names), 0, 0, 1, 0)
    return bytes(data)


class InstalledKernelTests(unittest.TestCase):
    def setUp(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="kernel-", dir=parent)
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)

    def write(self, name, data):
        path = self.work / name
        path.write_bytes(data)
        return path

    # The loader limit ------------------------------------------------------
    def test_loader_limit_is_read_from_the_loader_source(self):
        self.assertEqual(image.loader_kernel_limit(), 0x1000000)
        inc = self.write("elf.inc", b"x:\n    cmp dword [found_size], 52\n    jb elf_error\n"
                                    b"    cmp dword [found_size], 0x200000 ; comment\n    ja elf_error\n")
        self.assertEqual(image.loader_kernel_limit(inc), 0x200000)
        decimal = self.write("elf2.inc", b"    cmp dword [found_size], 4096\n    ja elf_error\n")
        self.assertEqual(image.loader_kernel_limit(decimal), 4096)

    def test_a_loader_source_without_the_limit_fails_the_build(self):
        inc = self.write("elf.inc", b"    cmp dword [found_size], 52\n    jb elf_error\n")
        with self.assertRaisesRegex(ValueError, "limit not found"):
            image.loader_kernel_limit(inc)

    # The 75 % guard --------------------------------------------------------
    def test_budget_guard_accepts_up_to_75_percent_and_refuses_one_byte_more(self):
        self.assertEqual(image.KERNEL_BUDGET_PERCENT, 75)
        ok = self.write("ok", bytes(750))
        self.assertEqual(image.check_kernel_budget(ok, 1000), (750, 1000))
        over = self.write("over", bytes(751))
        with self.assertRaisesRegex(ValueError, "above 75 % of the loader limit"):
            image.check_kernel_budget(over, 1000)

    def test_budget_guard_uses_the_loader_limit_by_default(self):
        limit = image.loader_kernel_limit()
        with patch.object(Path, "stat") as stat:
            stat.return_value.st_size = limit * 75 // 100 + 1
            with self.assertRaises(ValueError):
                image.check_kernel_budget(self.work / "kernel")
            stat.return_value.st_size = limit * 75 // 100
            self.assertEqual(image.check_kernel_budget(self.work / "kernel")[1], limit)

    def test_the_trial_kernel_failed_the_guard_and_its_stripped_form_passes(self):
        # 15,677,484 bytes (full, T23 trial) against the real loader limit.
        limit = image.loader_kernel_limit()
        self.assertGreater(15_677_484 * 100, limit * 75)
        self.assertLess(3_963_384 * 100, limit * 75)

    # Stripping -------------------------------------------------------------
    @unittest.skipUnless(shutil.which("llvm-objcopy"), "requires llvm-objcopy")
    def test_strip_removes_debug_sections_keeps_loaded_segments_and_is_deterministic(self):
        full = self.write("VMM.ELF", fixture_kernel())
        entry, phdrs, names = image.elf32_layout(full.read_bytes())
        self.assertIn(".debug_info", names)
        first = image.strip_kernel(full, self.work / "a.elf")
        second = image.strip_kernel(full, self.work / "b.elf")
        self.assertEqual(first.read_bytes(), second.read_bytes())
        self.assertLess(first.stat().st_size, full.stat().st_size - 0x4000 + 0x400)
        s_entry, s_phdrs, s_names = image.elf32_layout(first.read_bytes())
        self.assertEqual((s_entry, s_phdrs), (entry, phdrs))
        self.assertNotIn(".debug_info", s_names)
        self.assertIn(".text", s_names)
        self.assertEqual(first.read_bytes()[0x1000:0x1200], full.read_bytes()[0x1000:0x1200])
        # The input is untouched.
        self.assertEqual(full.read_bytes(), fixture_kernel())

    @unittest.skipUnless(shutil.which("llvm-objcopy"), "requires llvm-objcopy")
    def test_strip_refuses_a_result_that_changed_a_loaded_segment(self):
        full = self.write("VMM.ELF", fixture_kernel())

        real = image.run

        def run_then_corrupt(tool, option, source, target):
            real(tool, option, source, target)
            data = bytearray(Path(target).read_bytes())
            data[0x1010] ^= 0xFF
            Path(target).write_bytes(data)
        with patch.object(image, "run", run_then_corrupt):
            with self.assertRaisesRegex(ValueError, "changed a loaded segment"):
                image.strip_kernel(full, self.work / "bad.elf")

    def test_strip_refuses_a_missing_tool(self):
        full = self.write("VMM.ELF", fixture_kernel())
        with patch.object(image.shutil, "which", return_value=None):
            with self.assertRaisesRegex(ValueError, "llvm-objcopy"):
                image.strip_kernel(full, self.work / "x.elf")

    def test_layout_rejects_a_non_elf_file(self):
        with self.assertRaises(ValueError):
            image.elf32_layout(b"MZ" + bytes(100))

    # Manifest and BUILD.TXT ------------------------------------------------
    def clock(self, full_bytes):
        record = {"utc_epoch": 1792713600, "kernel_sha256": hashlib.sha256(full_bytes).hexdigest(), "version": "9.9.9 test"}
        (self.work / "build-clock.json").write_text(json.dumps(record))
        return record

    def test_manifest_records_installed_and_full_kernel(self):
        full_bytes = fixture_kernel()
        full = self.write("VMM.ELF", full_bytes)
        clock = self.clock(full_bytes)
        installed = b"installed kernel without debug sections"
        installed_sha = hashlib.sha256(installed).hexdigest()
        records = [{"path": "/system/VMM.ELF", "sha256": installed_sha, "size": len(installed), "attributes": 32}]
        with patch.object(image, "git_identity", return_value=("unknown", "unknown")):
            image.write_manifest(self.work / "disk.img", full, "image-hash", records, {},
                                 installed_kernel_sha256=installed_sha, installed_kernel_bytes=len(installed),
                                 kernel_limit=0x1000000)
        manifest = json.loads((self.work / "build-manifest.json").read_text())
        self.assertEqual(manifest["kernel_sha256"], installed_sha)
        self.assertEqual(manifest["kernel_installed_bytes"], len(installed))
        self.assertEqual(manifest["kernel_full_sha256"], clock["kernel_sha256"])
        self.assertEqual(manifest["kernel_full_bytes"], len(full_bytes))
        self.assertEqual(manifest["kernel_loader_limit"], 0x1000000)
        self.assertEqual(manifest["kernel"], str(full))
        self.assertEqual(manifest["schema_version"], 1)

    def test_manifest_without_installed_arguments_describes_one_kernel(self):
        full_bytes = fixture_kernel()
        full = self.write("VMM.ELF", full_bytes)
        self.clock(full_bytes)
        with patch.object(image, "git_identity", return_value=("unknown", "unknown")):
            image.write_manifest(self.work / "disk.img", full, "image-hash", [], {})
        manifest = json.loads((self.work / "build-manifest.json").read_text())
        self.assertEqual(manifest["kernel_sha256"], manifest["kernel_full_sha256"])
        self.assertEqual(manifest["kernel_installed_bytes"], manifest["kernel_full_bytes"])
        self.assertEqual(manifest["kernel_loader_limit"], image.loader_kernel_limit())

    def test_manifest_refuses_a_payload_record_that_is_not_the_installed_kernel(self):
        full_bytes = fixture_kernel()
        full = self.write("VMM.ELF", full_bytes)
        self.clock(full_bytes)
        records = [{"path": "/system/VMM.ELF", "sha256": hashlib.sha256(full_bytes).hexdigest(),
                    "size": len(full_bytes), "attributes": 32}]
        with patch.object(image, "git_identity", return_value=("unknown", "unknown")):
            with self.assertRaisesRegex(ValueError, "installed kernel differs"):
                image.write_manifest(self.work / "disk.img", full, "image-hash", records, {},
                                     installed_kernel_sha256="0" * 64, installed_kernel_bytes=5)

    def test_manifest_still_binds_the_full_kernel_to_the_build_clock(self):
        full_bytes = fixture_kernel()
        full = self.write("VMM.ELF", full_bytes)
        self.clock(b"another kernel")
        with patch.object(image, "git_identity", return_value=("unknown", "unknown")):
            with self.assertRaisesRegex(ValueError, "build-clock"):
                image.write_manifest(self.work / "disk.img", full, "image-hash", [], {},
                                     installed_kernel_sha256="0" * 64, installed_kernel_bytes=5)

    def test_build_txt_quotes_the_installed_kernel_and_names_the_full_one(self):
        full_bytes = fixture_kernel()
        full = self.write("VMM.ELF", full_bytes)
        clock = self.clock(full_bytes)
        installed = self.write("installed.elf", b"installed kernel without debug sections")
        installed_sha = hashlib.sha256(installed.read_bytes()).hexdigest()
        sources = {"/system/VMM.ELF": installed}
        metadata = {}
        manifest_id = identity.stage_identity(full, self.work / "id", sources, metadata, "a" * 40, False, "t",
                                              installed_kernel=installed)
        text = sources["/System/BUILD.TXT"].read_text()
        lines = text.splitlines()
        self.assertIn("kernel_elf_sha256=" + installed_sha, lines)
        self.assertIn("kernel_full_elf_sha256=" + clock["kernel_sha256"], lines)
        self.assertNotEqual(installed_sha, clock["kernel_sha256"])
        self.assertLess(len(text), 1024)  # HR_BUILD_MAX of the Hardware Report
        self.assertEqual(metadata["build_identity"]["kernel_sha256"], installed_sha)
        self.assertEqual(metadata["build_identity"]["kernel_full_sha256"], clock["kernel_sha256"])
        self.assertEqual(metadata["build_manifest_id"], manifest_id)
        # Deterministic, and the identity depends on the installed bytes.
        self.assertEqual(identity.stage_identity(full, self.work / "id", sources, metadata, "a" * 40, False, "t",
                                                 installed_kernel=installed), manifest_id)
        installed.write_bytes(b"different installed kernel")
        self.assertNotEqual(identity.stage_identity(full, self.work / "id", sources, metadata, "a" * 40, False, "t",
                                                    installed_kernel=installed), manifest_id)

    def test_build_txt_still_refuses_a_full_kernel_that_is_not_the_clock_kernel(self):
        full = self.write("VMM.ELF", fixture_kernel())
        self.clock(b"another kernel")
        installed = self.write("installed.elf", b"x")
        with self.assertRaisesRegex(ValueError, "kernel build-clock hash"):
            identity.stage_identity(full, self.work / "id", {}, {}, "unknown", "unknown", "t", installed_kernel=installed)


if __name__ == "__main__":
    unittest.main()
