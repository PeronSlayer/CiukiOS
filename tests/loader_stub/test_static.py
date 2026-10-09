"""T0: real assembler outputs and the frozen C/NASM handoff agree."""
import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("build_image", ROOT / "scripts/build_image.py")
BUILDER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILDER)
SERIAL_SPEC = importlib.util.spec_from_file_location(
    "check_serial", ROOT / "tests/loader_stub/check_serial.py")
SERIAL = importlib.util.module_from_spec(SERIAL_SPEC)
SERIAL_SPEC.loader.exec_module(SERIAL)
OFFSETS = {
    "magic": 0x000, "version": 0x004, "size": 0x006, "flags": 0x008,
    "boot_drive": 0x00C, "memmap_source": 0x00D, "input_policy": 0x00E,
    "reserved0": 0x00F, "partition_lba": 0x010, "loader_start": 0x014,
    "loader_end": 0x018, "kernel_start": 0x01C, "kernel_end": 0x020,
    "kernel_entry_phys": 0x024, "e820_count": 0x028, "e801_low_kib": 0x02C,
    "e801_high_64k": 0x030, "int88_kib": 0x034, "uart_base": 0x038,
    "uart_divisor": 0x03C, "pci_bios": 0x040, "apm": 0x044,
    "acpi_rsdp_phys": 0x048, "smbios_entry_phys": 0x04C, "fb_phys": 0x050,
    "fb_pitch": 0x054, "fb_width": 0x058, "fb_height": 0x05A, "fb_bpp": 0x05C,
    "fb_red_size": 0x05D, "fb_red_pos": 0x05E, "fb_green_size": 0x05F,
    "fb_green_pos": 0x060, "fb_blue_size": 0x061, "fb_blue_pos": 0x062,
    "fb_rsvd_size": 0x063, "vbe_mode": 0x064, "test_request_len": 0x066,
    "test_request": 0x068, "options": 0x0A8, "edid": 0x128, "edd": 0x1A8,
    "reserved1": 0x1EA, "vbe_ctrl": 0x1F0, "vbe_mode_info": 0x3F0, "e820": 0x4F0,
}


def run(*args):
    subprocess.run([str(a) for a in args], cwd=ROOT, check=True, capture_output=True)


class StaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch_root = ROOT / "build/tools"
        scratch_root.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix="loader-t0-", dir=scratch_root)
        cls.scratch = Path(cls.temp.name)
        cls.mbr = cls.scratch / "mbr.bin"
        cls.loader = cls.scratch / "ciukldr.bin"
        run("nasm", "-f", "bin", "src/boot/mbr.asm", "-o", cls.mbr)
        run("nasm", "-f", "bin", "src/boot/ciukldr.asm", "-o", cls.loader)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_mbr_size_signature_and_empty_partition_table(self):
        data = self.mbr.read_bytes()
        self.assertEqual(len(data), 512)
        self.assertEqual(data[510:], b"\x55\xaa")
        self.assertEqual(data[440:446], bytes(6), "disk signature is separate from code")
        self.assertEqual(data[446:510], bytes(64))

    def test_loader_header_crc_and_single_byte_corruption(self):
        loader = BUILDER.prepare_loader(self.loader.read_bytes())
        magic, version, sectors, crc, entry, length = BUILDER.HEADER.unpack_from(loader)
        self.assertEqual((magic, version, length), (b"CLDR", 1, 16))
        self.assertLessEqual(sectors, 1023)
        self.assertEqual(sectors * 512, len(loader))
        self.assertGreaterEqual(entry, length)
        self.assertLess(entry, len(loader))
        self.assertEqual(crc, zlib.crc32(loader[length:]))
        loader[entry] ^= 1
        self.assertNotEqual(crc, zlib.crc32(loader[length:]))

    def test_reject_bad_loader_header_size_and_entry(self):
        original = self.loader.read_bytes()
        for raw in (b"", original[:-1], original + bytes(1024*512)):
            with self.assertRaises(ValueError):
                BUILDER.prepare_loader(raw)
        for offset, replacement in ((0, b"BAD!"), (4, b"\x02\0"),
                                    (12, b"\0\0"), (14, b"\x20\0")):
            broken = bytearray(original)
            broken[offset:offset+len(replacement)] = replacement
            with self.assertRaises(ValueError):
                BUILDER.prepare_loader(broken)

    def test_every_frozen_handoff_offset(self):
        source = self.scratch / "offsets.asm"
        binary = self.scratch / "offsets.bin"
        source.write_text('%include "src/boot/boot_info.inc"\n' +
                          "\n".join("dw ciuki_boot_info." + f for f in OFFSETS) +
                          "\ndw ciuki_boot_info_size, ciuki_e820_size\n")
        run("nasm", "-f", "bin", source, "-o", binary)
        actual = struct.unpack("<" + "H"*(len(OFFSETS)+2), binary.read_bytes())
        self.assertEqual(actual, tuple(OFFSETS.values()) + (0x10F0, 24))
        cc = shutil.which("cc")
        self.assertIsNotNone(cc, "a C compiler is needed for C/NASM ABI agreement")
        source_c = self.scratch / "offsets.c"
        source_c.write_text('#include "src/kernel/include/ciuki/boot_info.h"\n' +
                            "\n".join(f'CBI_ASSERT_OFF({f}, {o});' for f, o in OFFSETS.items()))
        run(cc, "-std=c17", "-fsyntax-only", "-I", ROOT, source_c)

    def test_stub_virtual_entry_physical_translation_and_bss(self):
        obj = self.scratch / "stub.o"
        elf = self.scratch / "VMM.ELF"
        run("nasm", "-f", "elf32", "tests/loader_stub/stub.asm", "-o", obj)
        run("ld.lld", "-m", "elf_i386", "-T", "tests/loader_stub/link.ld", "-o", elf, obj)
        data = elf.read_bytes()
        self.assertEqual(data[:7], b"\x7fELF\x01\x01\x01")
        entry, offset = struct.unpack_from("<II", data, 24)
        size, count = struct.unpack_from("<HH", data, 42)
        self.assertEqual((entry, size, count), (0xC0100000, 32, 2))
        segments = [struct.unpack_from("<8I", data, offset+i*size) for i in range(count)]
        executable = [p for p in segments if p[0] == 1 and p[6] & 1
                      and p[2] <= entry < p[2] + p[5]]
        self.assertEqual(len(executable), 1)
        p = executable[0]
        self.assertEqual(p[3] + entry - p[2], 0x100000)
        self.assertTrue(any(p[5] > p[4] for p in segments), "stub must test BSS")
        for p in segments:
            self.assertGreaterEqual(p[3], 0x100000)
            self.assertLessEqual(p[3] + p[5], 0x1000000)

    def test_serial_checker_accepts_fixtures_and_rejects_missing_evidence(self):
        # Parser fixtures are not boot evidence.
        events = [
            "BEGIN", "DATA flags=0000012a e820_count=6 input_policy=0",
            "DATA video=lfb width=1024 height=768 bpp=32 pitch=4096",
            "DATA request_len=0 request_hex=-", "DATA entry_phys=00100000",
            "END status=PASS"]
        raw = "".join(f"CIUKI_TEST v=1 run=00000000 seq={i:06d} probe=loader event={e}\r\n"
                      for i, e in enumerate(events, 1)).encode()
        self.assertEqual(len(SERIAL.check(raw)), 6)
        for broken in (b"", raw.replace(b"seq=000003", b"seq=000002"),
                       raw.replace(b"status=PASS", b"status=FAIL"),
                       raw.replace(b"input_policy=0", b"input_policy=1"),
                       raw.replace(b"entry_phys=00100000", b"entry_phys=c0100000")):
            with self.assertRaises(ValueError):
                SERIAL.check(broken)
        SERIAL.check(b"C", crc_error=True)
        with self.assertRaises(ValueError):
            SERIAL.check(b"H", crc_error=True)


if __name__ == "__main__":
    unittest.main()
