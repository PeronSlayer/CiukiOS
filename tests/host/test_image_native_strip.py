"""fx-12: the image installs every native ELF payload without debug sections; host fixtures only, no image build."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
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


image = load("image_native_strip_build", ROOT / "scripts/build_image.py")
kernel_tests = load("image_native_strip_fixture", ROOT / "tests/host/test_image_kernel.py")
cd = load("image_native_strip_cd", ROOT / "scripts/build_cd.py")
sha = lambda data: hashlib.sha256(data).hexdigest()
needs_objcopy = unittest.skipUnless(shutil.which("llvm-objcopy"), "requires llvm-objcopy")


def loadable(data):
    """What the loader reads: header fields except the section table, program
    headers and every file-backed segment byte after the 52-byte ELF header."""
    entry, phdrs, _ = image.elf32_layout(data)
    return (data[:32], data[36:46], entry, phdrs,
            [data[max(o, 52):o + filesz] for _k, o, _v, _p, filesz, *_ in phdrs])


class NativePayloadStripTests(unittest.TestCase):
    def setUp(self):
        parent = ROOT / "build/host"
        parent.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="native-strip-", dir=parent)
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.stage = self.work / "stage"
        self.stage.mkdir()

    def write(self, name, data):
        path = self.work / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def sources(self):
        elf = kernel_tests.fixture_kernel(0x6000)
        rel = bytearray(elf)
        struct.pack_into("<H", rel, 16, 1)  # ET_REL: an SDK crt object, not a program
        return {
            "/Programs/lua": self.write("apps/lua", elf),
            "/Programs/alias": self.write("apps/alias", elf + b""),
            "/Programs/other": self.write("apps/other", kernel_tests.fixture_kernel(0x2000)),
            "/System/sdk/crt1.o": self.write("sdk/crt1.o", bytes(rel)),
            "/System/Win32/ntdll.dll": self.write("w32/ntdll.dll", b"MZ" + bytes(range(256)) * 40),
            "/system/tests/ciuki-f2.lua": self.write("lua/ciuki-f2.lua", b"print('x')\n"),
            "/system/assets/odd": self.write("odd", b"\x7fELF placeholder, not a real ELF\n"),
        }

    # Classification --------------------------------------------------------
    def test_only_executable_elf32_files_are_native_elf(self):
        sources = self.sources()
        got = {name: image.is_native_elf(path) for name, path in sources.items()}
        self.assertEqual(got, {"/Programs/lua": True, "/Programs/alias": True, "/Programs/other": True,
                               "/System/sdk/crt1.o": False, "/System/Win32/ntdll.dll": False,
                               "/system/tests/ciuki-f2.lua": False, "/system/assets/odd": False})
        self.assertFalse(image.is_native_elf(self.write("empty", b"")))

    # The installed bytes ---------------------------------------------------
    @needs_objcopy
    def test_stripped_install_equals_the_loadable_bytes_of_the_full_elf(self):
        sources = self.sources()
        full = {name: path.read_bytes() for name, path in sources.items()}
        records = image.strip_payloads(sources, self.stage)
        self.assertEqual([r["path"] for r in records], ["/Programs/alias", "/Programs/lua", "/Programs/other"])
        for record in records:
            name = record["path"]
            installed = sources[name].read_bytes()
            self.assertEqual(loadable(installed), loadable(full[name]), name)
            self.assertNotIn(".debug_info", image.elf32_layout(installed)[2])
            self.assertIn(".text", image.elf32_layout(installed)[2])
            self.assertLess(len(installed), len(full[name]) - 0x1000)
            self.assertEqual((record["full_sha256"], record["full_bytes"]), (sha(full[name]), len(full[name])))
            self.assertEqual((record["installed_sha256"], record["installed_bytes"]), (sha(installed), len(installed)))
            self.assertIn(self.stage, sources[name].parents)

    @needs_objcopy
    def test_the_complete_build_outputs_are_untouched(self):
        sources = self.sources()
        original = {name: Path(path) for name, path in sources.items()}
        before = {name: path.read_bytes() for name, path in original.items()}
        image.strip_payloads(sources, self.stage)
        self.assertEqual({name: path.read_bytes() for name, path in original.items()}, before)

    @needs_objcopy
    def test_non_elf_and_non_executable_payloads_are_untouched(self):
        sources = self.sources()
        keep = {name: sources[name] for name in ("/System/sdk/crt1.o", "/System/Win32/ntdll.dll",
                                                 "/system/tests/ciuki-f2.lua", "/system/assets/odd")}
        image.strip_payloads(sources, self.stage)
        for name, path in keep.items():
            self.assertIs(sources[name], path, name)  # the very same file, not a copy
        self.assertFalse(list(self.stage.glob("*/ntdll.dll")))

    @needs_objcopy
    def test_one_source_under_two_paths_is_stripped_once(self):
        sources = self.sources()
        sources["/Programs/twin"] = sources["/Programs/lua"]
        image.strip_payloads(sources, self.stage)
        self.assertIs(sources["/Programs/twin"], sources["/Programs/lua"])

    @needs_objcopy
    def test_stripping_is_deterministic_and_idempotent(self):
        first_sources, second_sources = self.sources(), self.sources()
        first = image.strip_payloads(first_sources, self.stage)
        other_stage = self.work / "stage2"
        other_stage.mkdir()
        second = image.strip_payloads(second_sources, other_stage)
        self.assertEqual(first, second)
        for name in first_sources:
            self.assertEqual(first_sources[name].read_bytes(), second_sources[name].read_bytes(), name)
        # A second pass over the staged files does nothing (the build runs two passes).
        self.assertEqual(image.strip_payloads(first_sources, self.stage), [])

    @needs_objcopy
    def test_a_corrupting_strip_is_refused_with_the_payload_named(self):
        sources = self.sources()
        real = image.run

        def run_then_corrupt(tool, option, source, target):
            real(tool, option, source, target)
            data = bytearray(Path(target).read_bytes())
            data[0x1010] ^= 0xFF
            Path(target).write_bytes(data)
        with patch.object(image, "run", run_then_corrupt):
            with self.assertRaisesRegex(ValueError, "stripped payload /Programs/alias changed a loaded segment"):
                image.strip_payloads(sources, self.stage)

    @needs_objcopy
    @unittest.skipUnless(shutil.which("clang") and shutil.which("ld.lld"), "requires clang and ld.lld")
    def test_a_real_dwarf_program_keeps_its_loaded_bytes(self):
        source = self.write("hello.c", b"int counter = 3; int _start(void) { for (int i = 0; i < 9; i++) counter += i; return counter; }\n")
        obj, program = self.work / "hello.o", self.work / "hello"
        subprocess.run(["clang", "--target=i386-unknown-none", "-ffreestanding", "-g", "-O0", "-c", str(source), "-o", str(obj)],
                       check=True, capture_output=True)
        subprocess.run(["ld.lld", "-m", "elf_i386", "-static", "-e", "_start", str(obj), "-o", str(program)],
                       check=True, capture_output=True)
        full = program.read_bytes()
        self.assertTrue(any(n.startswith(".debug") for n in image.elf32_layout(full)[2]))
        sources = {"/Programs/hello": program}
        image.strip_payloads(sources, self.stage)
        installed = sources["/Programs/hello"].read_bytes()
        self.assertEqual(loadable(installed), loadable(full))
        self.assertFalse([n for n in image.elf32_layout(installed)[2] if n.startswith(".debug")])
        self.assertLess(len(installed), len(full))

    # Consumers of the installed bytes --------------------------------------
    @needs_objcopy
    def test_payload_records_and_gate_hash_lines_describe_the_installed_files(self):
        sources = self.sources()
        full_lua = sources["/Programs/lua"].read_bytes()
        image.strip_payloads(sources, self.stage)
        installed_lua = sources["/Programs/lua"].read_bytes()
        self.assertNotEqual(sha(installed_lua), sha(full_lua))
        records = {r["path"]: r for r in image.payload_records(sources)}
        self.assertEqual(records["/Programs/lua"]["sha256"], sha(installed_lua))
        self.assertEqual(records["/Programs/lua"]["size"], len(installed_lua))
        pins = json.loads((ROOT / "config/sdk-pins.json").read_text())
        meta = image.application_provenance(sources, {"inputs": {"sdk_manifest_sha256": "0" * 64, "archives": pins["lua"]}},
                                            {"newlib": pins["newlib"]})
        self.assertIn(f"P /Programs/lua {sha(installed_lua)}\n", meta)
        self.assertNotIn(sha(full_lua), meta)

    @needs_objcopy
    def test_read_back_from_a_fat_volume_matches_the_manifest_records(self):
        if not (shutil.which("mkfs.fat") and shutil.which("mcopy") and shutil.which("mmd") and shutil.which("mdir")):
            self.skipTest("requires FAT tools")
        sources = self.sources()
        image.strip_payloads(sources, self.stage)
        volume = self.work / "fixture.fat"
        with volume.open("wb") as stream:
            stream.truncate(16 * 1024 * 1024)
        subprocess.run(["mkfs.fat", str(volume)], check=True, stdout=subprocess.DEVNULL)
        directories = {"/Programs", "/System", "/System/sdk", "/System/Win32", "/System/tests", "/System/assets"}
        image.populate_payloads(volume, sources, directories)
        records = image.payload_records(sources)
        with patch.object(image, "PART_LBA", 0):
            image.check_payloads(volume, records, directories)

    @needs_objcopy
    def test_the_native_record_reaches_the_manifest(self):
        sources = self.sources()
        records = image.strip_payloads(sources, self.stage)
        kernel = self.write("build/VMM.ELF", kernel_tests.fixture_kernel(0x1000))
        (kernel.parent / "build-clock.json").write_text(json.dumps({"utc_epoch": 1, "kernel_sha256": image.sha256(kernel)}))
        disk = kernel.parent / "ciukios.img"
        image.write_manifest(disk, kernel, "image-hash", image.payload_records(sources), {"native_elf": records},
                             directories={"/Programs"})
        manifest = json.loads((kernel.parent / "build-manifest.json").read_text())
        self.assertEqual(manifest["native_elf"], records)
        by_path = {p["path"]: p for p in manifest["payloads"]}
        for record in manifest["native_elf"]:
            self.assertEqual(by_path[record["path"]]["sha256"], record["installed_sha256"])
            self.assertEqual(by_path[record["path"]]["size"], record["installed_bytes"])

    # The builder wiring ----------------------------------------------------
    def test_the_builder_strips_before_the_gate_provenance_and_again_for_late_sources(self):
        text = (ROOT / "scripts/build_image.py").read_text()
        self.assertLess(text.index('strip_payloads(sources, stage) if stage is not None'),
                        text.index('provenance = LUA / "app-gate.meta"'))
        self.assertIn("application_payloads(stage)", text)
        self.assertIn('metadata["native_elf"] += strip_payloads(sources, stage)', text)
        self.assertLess(text.index('metadata["native_elf"] += strip_payloads(sources, stage)'),
                        text.index("payloads = payload_records(sources)"))

    # The Setup CD ----------------------------------------------------------
    @needs_objcopy
    def test_cd_setup_program_is_installed_without_debug_sections(self):
        full = self.write("setup.elf", kernel_tests.fixture_kernel(0x5000))
        bi = cd.module("cd_native_strip_bi", ROOT / "scripts/build_image.py")
        self.assertTrue(bi.is_native_elf(full))
        installed = bi.strip_elf(full, self.work / "cd" / "setup", "payload /Programs/setup")
        self.assertEqual(loadable(installed.read_bytes()), loadable(full.read_bytes()))
        self.assertLess(installed.stat().st_size, full.stat().st_size)
        text = (ROOT / "scripts/build_cd.py").read_text()
        self.assertIn("bi.strip_elf(setup_elf", text)
        # The host fixture's placeholder is not an ELF program and stays as is.
        placeholder = self.write("placeholder", b"\x7fELF placeholder setup program for the host fixture\n")
        self.assertFalse(bi.is_native_elf(placeholder))


if __name__ == "__main__":
    unittest.main()
