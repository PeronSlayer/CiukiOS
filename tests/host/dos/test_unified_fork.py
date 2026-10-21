"""f3-56 fork inputs: pins, strict patch application and the constants the
guest (CIUKIDF.SYS, ciukinit.c) and the native adapter must share. Fast:
no compiler, emulator or image. The build/CPU qualification is
tests/host/dos/run_unified_binding.sh. SPDX-License-Identifier: GPL-2.0-only
"""
import importlib.util
import json
from pathlib import Path
import re
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("unified_fork", ROOT / "scripts/unified_fork.py")
fork = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fork)
u = fork.u
FORK = u.VENDOR / "ciuki/fork"


class ForkInputs(unittest.TestCase):
    def test_manifest_patch_and_added_files_are_pinned(self):
        data, _, patch, added = fork.manifest()
        self.assertEqual(data["base"], u.MODIFICATION)
        self.assertEqual(set(added), {"ciuki/ciukidf.asm", "hdr/ciuki.h", "kernel/ciuki.c", "kernel/ciukinit.c"})
        self.assertEqual(set(data["changed"]), {
            "kernel/entry.asm", "kernel/fcbfns.c", "kernel/int2f.asm", "kernel/inthndlr.c", "kernel/main.c", "kernel/makefile",
            "kernel/nls.c", "kernel/nls_hc.asm", "kernel/proto.h", "kernel/task.c"})
        for blob in added.values():
            self.assertIn(b"SPDX-License-Identifier: GPL-2.0-or-later", blob)
            self.assertIn(b"either version 2, or (at your option) any later version", blob)
        self.assertTrue(patch.startswith(b"--- a/"))

    def test_fork_sources_change_only_declared_files(self):
        data, _, _, added = fork.manifest()
        base = u.patched_sources(u.sources())
        result = fork.fork_sources()
        self.assertEqual(set(result), set(base) | set(added))
        for name, blob in base.items():
            if name in data["changed"]:
                self.assertNotEqual(result[name], blob, name)
                self.assertEqual(fork.sha(result[name]), data["changed"][name])
                # upstream line endings are preserved
                self.assertEqual(b"\r\n" in result[name], b"\r\n" in blob, name)
            else:
                self.assertEqual(result[name], blob, name)

    def test_patch_application_is_exact(self):
        data, _, patch, _ = fork.manifest()
        base = u.patched_sources(u.sources())
        lines = patch.splitlines(keepends=True)
        context = next(i for i, l in enumerate(lines) if l.startswith(b" ") and len(l) > 4)
        broken = b"".join(lines[:context] + [b" changed context\n"] + lines[context + 1:])
        cases = {
            "context mismatch": broken,
            "unexpected target": patch.replace(b"--- a/kernel/task.c\n+++ b/kernel/task.c",
                                               b"--- a/kernel/dsk.c\n+++ b/kernel/dsk.c", 1),
            "invalid file header": b"garbage\n" + patch,
        }
        for error, text in cases.items():
            with self.subTest(error=error), self.assertRaisesRegex(ValueError, error):
                fork.apply_patch(base, text, data["changed"], "patch 0002")
        wrong = dict(data["changed"], **{"kernel/task.c": "0" * 64})
        with self.assertRaisesRegex(ValueError, "postimage hash mismatch"):
            fork.apply_patch(base, patch, wrong, "patch 0002")
        more = dict(data["changed"], **{"kernel/dsk.c": "0" * 64})
        with self.assertRaisesRegex(ValueError, "incomplete patch"):
            fork.apply_patch(base, patch, more, "patch 0002")

    def test_guest_and_native_constants_agree(self):
        header = (ROOT / "src/kernel/vm/dos_unified.h").read_text()
        native = (ROOT / "src/kernel/vm/dos_unified.c").read_text()
        driver = (FORK / "drivers/ciukidf.asm").read_text()
        kernel_h = (FORK / "hdr/ciuki.h").read_text()
        init = (FORK / "kernel/ciukinit.c").read_text()

        def define(name):
            return int(re.search(rf"#define {name} (0x[0-9a-f]+|\d+)u", header).group(1), 0)

        def equ(name):
            return int(re.search(rf"^{name}\s+equ\s+(0x[0-9a-f]+|\d+)", driver, re.M).group(1), 0)
        for asm, c in (("GATE_DIRECT", "DFV2_DIRECT"), ("GATE_LOCAL", "DFV2_LOCAL"), ("GATE_EVENT", "DFV2_EVENT"),
                       ("GATE_CRITICAL", "DFV2_CRITICAL"), ("DESCRIPTOR", "DFV2_DESCRIPTOR"), ("ROM_END", "DFV2_END"),
                       ("FEATURES", "DFV2_FEATURES"), ("CALL_BYTES", "DFV2_CALL_BYTES"),
                       ("BIND_BYTES", "DFV2_BIND_BYTES")):
            self.assertEqual(equ(asm), define(c), asm)
        self.assertEqual(int(re.search(r"#define CIUKI_BIND_BYTES\s+(0x[0-9a-f]+)", kernel_h).group(1), 16),
                         define("DFV2_BIND_BYTES"))
        driver_id = re.search(r"driver_identity db\s+'([^']{16})'", driver).group(1)
        self.assertIn(f'driver_id[17]="{driver_id}"', native)
        kernel_id = re.search(r'fmemcpy\(ciuki_record \+ 0x30, "([^"]{16})", 16\)', init).group(1)
        self.assertIn(f'kernel_id[17]="{kernel_id}"', native)
        self.assertEqual(kernel_id, fork.IDENTITY)
        # v1 and DFV2 ROM ranges are disjoint, inside the protected F1 page.
        v1 = (ROOT / "src/kernel/vm/dos_file_hook.h").read_text()
        v1_end = int(re.search(r"#define DOS_HOOK_END (0x[0-9a-f]+)u", v1).group(1), 16)
        self.assertLessEqual(v1_end, define("DFV2_DIRECT"))
        self.assertLessEqual(define("DFV2_END"), 0x2000)

    def test_f3_57_critical_retry_and_policy_wire(self):
        """CRITICAL status 2, Retry word +2Ah, control block v3 with the
        /LOCAL policy byte, and the CriticalError FS/GS fix in entry.asm."""
        header = (ROOT / "src/kernel/vm/dos_unified.h").read_text()
        native = (ROOT / "src/kernel/vm/dos_unified.c").read_text()
        driver = (FORK / "drivers/ciukidf.asm").read_text()
        kernel_h = (FORK / "hdr/ciuki.h").read_text()
        init = (FORK / "kernel/ciukinit.c").read_text()

        def equ(name):
            return int(re.search(rf"^{name}\s+equ\s+(0x[0-9a-f]+|\d+)", driver, re.M).group(1), 0)
        self.assertEqual(equ("STATUS_CRITICAL"),
                         int(re.search(r"#define DFV2_STATUS_CRITICAL (\d+)u", header).group(1)))
        self.assertEqual(equ("FAIL_ON_INT24"), int(re.search(r"#define DFV2_FAIL_ON_INT24 (\d+)u", header).group(1)))
        self.assertEqual(equ("C_RETRY"), 0x2a)
        self.assertIn(".retry=u16(c+0x2a)", native)
        self.assertIn("for (unsigned i=0x2c;i<DFV2_CALL_BYTES;i++) if (c[i]) return false;", native)
        control = driver[driver.index("control:"):driver.index("control_end:")]
        self.assertEqual(re.search(r"dw\s+(\d+)\n", control).group(1),
                         re.search(r"#define CIUKI_CONTROL_VERSION\s+(\d+)", kernel_h).group(1))
        self.assertEqual(control.count("dw      ") * 2 + 8, int(re.search(r"#define CIUKI_CONTROL_BYTES\s+(\d+)", kernel_h).group(1)))
        self.assertIn("dw      policy", control)
        self.assertIn("(control + 22)", init)
        self.assertIn("r.a.x = 0x0d00;", init)
        entry = fork.fork_sources()["kernel/entry.asm"].decode("latin-1").replace("\r\n", "\n")
        self.assertIn("                push    fs\n                push    gs\n", entry)
        self.assertIn("                pop     gs                      ; CiukiDOS fork: caller's FS/GS\n                pop     fs\n", entry)

    def test_v1_discovery_and_rom_are_not_redefined(self):
        """DFV2 answers CX=2 only; v1 keeps CX=1 and its own descriptor."""
        native = (ROOT / "src/kernel/vm/dos_unified.c").read_text()
        v1 = (ROOT / "src/kernel/vm/dos_file_hook.inc").read_text()
        self.assertIn("f->cx!=DFV2_VERSION", native)
        self.assertIn("f->cx!=1", v1)
        vm = (ROOT / "src/kernel/vm/dos_file_vm.c").read_text()
        self.assertIn("if (v->kernel==2) dos_unified_rom(", vm)
        self.assertIn("if (v->kernel==2) {\n        int unified=unified_interrupt(v,f,vector);", vm)


class ForkDistribution(unittest.TestCase):
    """f3-56 Amendment 1: licence ledger and corresponding-source manifest."""

    def test_ledger_lists_patch_and_every_fork_file(self):
        rows = [l for l in u.LEDGER.read_text().splitlines() if l.startswith("ciuki-fork\t")]
        self.assertEqual(rows, fork.ledger_rows())
        data = json.loads(fork.FORK.read_text())
        named = {r.split("\t")[1] for r in rows}
        self.assertIn("third_party/freedos/unified/" + data["patch"]["path"], named)
        for item in data["added"].values():
            self.assertIn("third_party/freedos/unified/" + item["source"], named)
        for row in rows:
            self.assertEqual(row.split("\t")[3], "GPL-2.0-or-later")
            self.assertIn("CiukiDOS-unified-kernel-GRANT.md", row)

    def test_bundle_manifest_refuses_an_unlisted_or_absent_fork_file(self):
        files = u.fork_source_files()
        data = json.loads(fork.FORK.read_text())
        self.assertIn("ciuki/fork.json", files)
        self.assertIn(data["patch"]["path"], files)
        for item in data["added"].values():
            self.assertIn(item["source"], files)
        parent = ROOT / "build/host/dos"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as name:
            vendor = Path(name) / "unified"
            shutil.copytree(u.VENDOR / "ciuki", vendor / "ciuki")
            (vendor / "ciuki/fork/kernel/extra.c").write_text("/* not in fork.json */\n")
            with self.assertRaisesRegex(ValueError, "missing from fork.json"):
                u.fork_source_files(vendor)
            (vendor / "ciuki/fork/kernel/extra.c").unlink()
            (vendor / "ciuki/fork/kernel/ciuki.c").unlink()
            with self.assertRaisesRegex(ValueError, "absent fork file"):
                u.fork_source_files(vendor)

    def test_corresponding_source_carries_the_fork(self):
        source = (ROOT / "scripts/unified_dos.py").read_text()
        self.assertIn("for name, data in fork_source_files().items():", source)
        self.assertIn('"scripts/unified_fork.py", "docs/licenses/CiukiDOS-unified-kernel-GRANT.md"', source)


if __name__ == "__main__":
    unittest.main()
