"""H50 emitted personality layout; assembly/static evidence, no guest claim."""
import hashlib
import importlib.util
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


@unittest.skipUnless(shutil.which("nasm"), "requires NASM")
class HookLayoutTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("hook_layout_builder", ROOT / "scripts/build_dos_image.py")
        cls.builder = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.builder)
        (ROOT / "build/host/dos").mkdir(parents=True, exist_ok=True)
        cls.work = tempfile.TemporaryDirectory(dir=ROOT / "build/host/dos", prefix="layout-")
        labels = ("int21_handler", "dos_hook_legacy_int21", "dos_hook_dispatch", "dos_hook_result",
                  "dos_hook_binding", "dos_hook_binding_end", "dos_hook_expanded", "dos_hook_root_psp",
                  "dos_hook_exec_lookup", "dos_hook_exec_load", "kernel_int21_stacks", "kernel_int21_stacks_end",
                  "dos_hook_children", "dta_off", "dta_seg", "current_psp_seg", "dos_sda", "dos_sda_end")
        source = Path(cls.work.name) / "fixture.asm"
        source.write_text('%include "src/runtime/ciukidos.asm"\ndb "H50MAP"\ndw ' + ",".join(labels) + "\n")
        cls.binary = cls.builder.assemble(str(source), Path(cls.work.name) / "a.bin", {**cls.builder.GEOMETRY, **cls.builder.PERSONALITY})
        again = cls.builder.assemble(str(source), Path(cls.work.name) / "b.bin", {**cls.builder.GEOMETRY, **cls.builder.PERSONALITY})
        assert cls.binary == again
        marker = len(cls.binary) - 6 - 2 * len(labels)
        assert cls.binary[marker:marker+6] == b"H50MAP"
        cls.extent = marker
        cls.labels = dict(zip(labels, struct.unpack_from("<" + "H"*len(labels), cls.binary, marker+6)))

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def test_emitted_handoff_precedes_stack_switch(self):
        start, end = self.labels["int21_handler"], self.labels["dos_hook_legacy_int21"]
        code = self.binary[start:end]
        self.assertEqual(code[0], 0x9c)  # Preserve live FLAGS even on absent binding.
        self.assertIn(b"\x2e\xff\x1e" + struct.pack("<H", self.labels["dos_hook_dispatch"]), code)
        self.assertEqual(code[-1], 0x9d)
        self.assertIn(b"\x9d\xcf", code)  # DONE restores live flags then original IRET.
        self.assertNotIn(b"\xcd\x21", code)
        self.assertEqual(self.binary[end], 0xe9)  # CHAIN jumps exactly once.

    def test_packed_binding_offsets_and_real_extents(self):
        start, end = self.labels["dos_hook_binding"], self.labels["dos_hook_binding_end"]
        binding = self.binary[start:end]
        self.assertEqual(len(binding), 68)
        self.assertEqual(struct.unpack_from("<HH", binding), (1, 68))
        self.assertEqual(struct.unpack_from("<H", binding, 4)[0], self.labels["dos_hook_legacy_int21"])
        self.assertEqual(struct.unpack_from("<H", binding, 8)[0], self.labels["dos_sda"])
        self.assertEqual(struct.unpack_from("<H", binding, 12)[0], self.labels["dos_sda_end"]-self.labels["dos_sda"])
        self.assertEqual(struct.unpack_from("<H", binding, 22)[0], 59)
        self.assertEqual(struct.unpack_from("<H", binding, 26)[0], self.labels["current_psp_seg"])
        self.assertEqual(struct.unpack_from("<H", binding, 30)[0], self.labels["dta_off"])
        self.assertEqual(self.labels["dta_seg"], self.labels["dta_off"]+2)
        self.assertEqual(struct.unpack_from("<H", binding, 34)[0], self.labels["dos_hook_result"])
        self.assertEqual(struct.unpack_from("<H", binding, 58)[0], self.labels["dos_hook_expanded"])
        self.assertEqual(struct.unpack_from("<H", binding, 62)[0], self.labels["kernel_int21_stacks"])
        self.assertEqual(struct.unpack_from("<H", binding, 66)[0], self.labels["kernel_int21_stacks_end"]-self.labels["kernel_int21_stacks"])
        self.assertEqual(self.labels["dos_hook_root_psp"] % 16, 0)
        self.assertLessEqual(self.labels["dos_hook_expanded"]+16*232, self.labels["dos_hook_root_psp"])
        self.assertLessEqual(self.extent, 0xf000)

    def test_rom_reservations_are_separate(self):
        hook = (ROOT / "src/kernel/vm/dos_file_hook.h").read_text()
        value = lambda name: int(re.search(r"#define " + name + r" (0x[0-9a-f]+)u", hook)[1], 16)
        dispatch, end = value("DOS_HOOK_DISPATCH"), value("DOS_HOOK_END")
        self.assertGreaterEqual(dispatch, 0x1400)  # BIOS, mouse, full DPMI real/PM ranges.
        self.assertLessEqual(end, 0x2000)
        intervals = [(value("DOS_HOOK_DISPATCH"), 4), (value("DOS_HOOK_EVENT"), 4),
                     (value("DOS_HOOK_CRITICAL"), 4), (value("DOS_HOOK_DESCRIPTOR"), 20),
                     (value("DOS_HOOK_CASEMAP"), 24), (value("DOS_HOOK_COUNTRY"), 34),
                     (value("DOS_HOOK_UPPER"), 130), (value("DOS_HOOK_LOWER"), 258),
                     (value("DOS_HOOK_TERMINATOR"), 24), (value("DOS_HOOK_COLLATING"), 258),
                     (value("DOS_HOOK_DBCS"), 4)]
        for n, (a, size) in enumerate(intervals):
            for b, other in intervals[:n]:
                self.assertTrue(a+size <= b or b+other <= a)

    def test_bound_exec_uses_internal_description_adapter(self):
        # Bounded source inventory, never read the full old personality here.
        result = subprocess.run(["rg", "-n", "dos_hook_exec_(load|lookup)|dos_hook_unwind|dos_hook_commit", "src/boot/floppy_stage1.asm"],
                                cwd=ROOT, text=True, capture_output=True, check=True).stdout
        self.assertIn("call dos_hook_exec_lookup", result)
        self.assertIn("je dos_hook_exec_load", result)
        self.assertEqual(result.count("call dos_hook_commit"), 2)
        self.assertIn("call dos_hook_unwind", result)
        adapter = (ROOT / "src/runtime/ciukidos_vmm_hook.inc").read_text().split("dos_hook_exec_lookup:", 1)[1]
        self.assertNotRegex(adapter, r"call (?:read|write)_sector")
        self.assertIn("call dos_hook_file_io", adapter)
        self.assertIn("mov ah, 0x3E", adapter)
        self.assertIn("mov ah, 0x3F", adapter)
