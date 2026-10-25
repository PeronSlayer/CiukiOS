"""DOS Box memory profile (2026-10-25, docs/design/dos-dpmi-contract.md
"DOS Box memory: UMBs linked, SHARE high, the EMMXXXX0 device"). The
kernel=unified FDCONFIG.SYS links the VM's upper memory blocks with the
kernel kept low and loads the EMMXXXX0 device (CIUKIEMM.SYS) low; FDAUTO.BAT
loads SHARE high; the f3-48 reference profile is unchanged. Host-only: no
build/ links, QEMU or image (tests/host/dos/run_dos_memory.sh boots it)."""
import importlib.util, re, struct, subprocess, sys, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("unified_dos_m", ROOT / "scripts/unified_dos.py")
unified = importlib.util.module_from_spec(spec); sys.modules["unified_dos_m"] = unified; spec.loader.exec_module(unified)


class DosBoxMemoryTests(unittest.TestCase):
    def setUp(self):
        self.ref = unified.reference_module()

    def test_config_links_umbs_and_keeps_every_kernel_structure_low(self):
        lines = unified.fork_config(self.ref).split(b"\r\n")
        self.assertEqual(lines, [b"DOS=UMB,LOW", b"FILES=40", b"BUFFERS=10", b"LASTDRIVE=Z",
                                 b"DEVICE=C:\\CIUKIEMM.SYS", b"DEVICE=C:\\CIUKIDF.SYS",
                                 b"SHELL=C:\\CIUKISH.COM", b""])
        # FreeDOS 2043 config.c Dosmem() stops at the first token other than
        # UMB/HIGH: UMB must come first; HIGH (kernel text in the HMA) and the
        # *HIGH/DOSDATA lines (SFT, CDS, stacks, device headers in UMBs) would
        # break the DFV2 final bind (f3-56 Amendment 1).
        config = unified.fork_config(self.ref)
        for forbidden in (b"DEVICEHIGH", b"FILESHIGH", b"LASTDRIVEHIGH", b"STACKSHIGH", b"DOSDATA=UMB", b"INSTALLHIGH"):
            self.assertNotIn(forbidden, config)
        self.assertNotIn(b"HIGH", config)
        self.assertNotIn(b"DOSDATA", config)

    def test_autoexec_loads_share_high_and_the_reference_profile_is_unchanged(self):
        auto = unified.fork_auto(self.ref)
        self.assertEqual(auto, self.ref.AUTO.replace(b"\r\nC:\\SHARE.COM\r\n", b"\r\nLH C:\\SHARE.COM\r\n"))
        self.assertEqual(auto.count(b"LH "), 1)
        self.assertTrue(self.ref.CONFIG.startswith(b"DOS=LOW\r\n"))
        self.assertNotIn(b"CIUKIEMM", self.ref.CONFIG)
        self.assertIn(b"\r\nC:\\SHARE.COM\r\n", self.ref.AUTO)
        self.assertNotIn(b"LH ", self.ref.AUTO)

    def test_emmxxxx0_device_header_and_answers(self):
        with tempfile.TemporaryDirectory() as t:
            listing = Path(t) / "E.lst"
            subprocess.run(["nasm", "-f", "bin", "-l", str(listing), str(ROOT / "src/com/dosemm.asm"),
                            "-o", str(Path(t) / "E.SYS")], check=True)
            image = (Path(t) / "E.SYS").read_bytes()
            text = listing.read_text()
        self.assertEqual(image, unified.ems_device())
        self.assertLess(len(image), 512)
        nxt, attr, strategy, interrupt = struct.unpack_from("<IHHH", image)
        self.assertEqual((nxt, attr, image[10:18]), (0xFFFFFFFF, 0xC000, b"EMMXXXX0"))
        after = text[text.index("init:"):]           # INIT's first instruction: the resident end
        resident = int(re.search(r"^\s*\d+\s+([0-9A-F]{8})\s", after, re.M).group(1), 16)
        self.assertLessEqual(resident, 0x60)        # 6 paragraphs stay resident
        self.assertLess(max(strategy, interrupt), resident)
        # Commands 1-14: 10 output status, 13/14 open/close done; 8/9/11 write
        # fault; 3 IOCTL read fault (no EMM386 private API); others unknown.
        self.assertIn(bytes([3, 3, 0x0B, 3, 3, 3, 3, 0x0A, 0x0A, 0, 0x0A, 3, 0, 0]), image[:resident])
        # INIT: EMMXXXX0 at offset 0Ah of the INT 67h segment (read from the
        # IVT, no DOS call), then EMS status AH=40h through INT 67h.
        self.assertIn(bytes.fromhex("268e069e01"), image[resident:])
        self.assertIn(bytes.fromhex("b440cd67"), image[resident:])

    def test_profile_is_installed_and_read_back(self):
        source = (ROOT / "scripts/unified_dos.py").read_text()
        self.assertIn('files["/CIUKIEMM.SYS"] = ems_device()', source)
        self.assertIn('files["/FDAUTO.BAT"] = auto', source)
        self.assertIn('read("/CIUKIEMM.SYS") != ems_device()', source)
        self.assertIn('"src/com/dossh.asm", "src/com/dosemm.asm"', source)
        builder = (ROOT / "scripts/dos_conformance.py").read_text()
        self.assertIn('files["/FDAUTO.BAT"]=reference["/FDAUTO.BAT"] if guest is not None', builder)


if __name__ == "__main__":
    unittest.main()
