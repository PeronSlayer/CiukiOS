"""Host checks of probe assembly and evidence rejection; never guest evidence."""
import json
from pathlib import Path
import shutil
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import conformance as battery
import inventory
import qemu_reference as reference
from cases import CASES


class EvidenceTests(unittest.TestCase):
    def transcript(self, status="PASS", detail="0000"):
        return "\n".join(f"{tag} {status} {detail}" for tag in battery.tags())

    def test_complete_identical_success(self):
        rows = battery.records(self.transcript())
        table, ok = battery.compare(rows, rows)
        self.assertTrue(ok)
        self.assertEqual(table.count("| PASS |"), len(CASES))

    def test_identical_failures_never_pass(self):
        rows = battery.records(self.transcript("FAIL", "0001"))
        self.assertFalse(battery.compare(rows, rows)[1])

    def test_missing_or_unknown_never_pass(self):
        good = battery.records(self.transcript())
        bad = dict(good)
        bad.pop(next(iter(bad)))
        with self.assertRaises(ValueError):
            battery.compare(good, bad)
        for text in ("D48999 PASS 0000", "D48001 PASS 0001", "D48001 FAIL 0000", "D48001 PASS"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                battery.records(text)

    def test_duplicate_rejected_even_if_identical(self):
        with self.assertRaises(ValueError):
            battery.records(self.transcript() + "\nD48001 PASS 0000")

    def test_existing_vmm_and_host_transport(self):
        text = "D48001 PASS 0000"
        self.assertEqual(battery.records("  guest: " + text), battery.records(text))
        record = "CIUKI_TEST event=DATA group=guest vm=1 generation=7 part=1 parts=1 data=" + text.encode().hex()
        self.assertEqual(battery.records(record), battery.records(text))
        with self.assertRaises(ValueError):
            battery.records(record.replace("parts=1", "parts=2"))
        with self.assertRaises(ValueError):
            battery.records(record.split("data=", 1)[0] + "data=")

    def test_one_regression_changes_verdict(self):
        good = battery.records(self.transcript())
        bad = dict(good)
        bad["D48001"] = ("FAIL", "FFFF")
        table, ok = battery.compare(good, bad)
        self.assertFalse(ok)
        self.assertIn("FAIL FFFF", table)

    def test_static_candidates_are_not_runtime_claims(self):
        rows = inventory.sites(bytes.fromhex("b43fcd21b8001acd2fb44890cd21"))
        self.assertEqual([(r["offset"], r["vector"], r["ax"], r["al_known"]) for r in rows],
                         [(0, "21", "3F00", False), (4, "2F", "1A00", True)])
        self.assertTrue(all(r["evidence"] == "unvalidated-static-candidate" for r in rows))

    @unittest.skipUnless(shutil.which("nasm"), "requires SDK nasm")
    def test_all_com_programs_assemble_and_are_reproducible(self):
        with tempfile.TemporaryDirectory() as folder:
            a = battery.build(Path(folder) / "a")
            b = battery.build(Path(folder) / "b")
            self.assertEqual(a, b)
            self.assertEqual(len(a["cases"]), len(CASES))
            for row in a["cases"]:
                data = (Path(folder) / "a" / row["binary"]).read_bytes()
                self.assertIn((row["tag"] + " PASS ").encode(), data)
                self.assertIn((row["tag"] + " FAIL ").encode(), data)
                self.assertIn(b"\xcd\x21", data)

    def test_cases_cover_mandatory_file_calls(self):
        covered = {fn for c in CASES for fn in c.functions}
        required = [*range(0x39, 0x48), 0x4e, 0x4f, 0x56, 0x57, 0x59, 0x5a, 0x5b, 0x5c, 0x67, 0x68, 0x6c]
        self.assertFalse({f"21:{fn:02X}" for fn in required} - covered)
        self.assertEqual(len({c.id for c in CASES}), len(CASES))

    def test_reference_runner_command_uses_overlay_and_runtime_selector(self):
        profile = json.loads((battery.ROOT / "tests/profiles/qemu-t23.json").read_text())
        argv = reference.command("qemu-unused", Path("private.qcow2"), "f3:dos-conformance run=12345678 guest=freedos", profile)
        self.assertIn("file=private.qcow2,format=qcow2,if=ide,index=0", argv)
        self.assertIn("name=opt/it.alcybercloud.ciukios/test,string=f3:dos-conformance run=12345678 guest=freedos", argv)
        self.assertNotIn("build-full", argv)

    def test_reference_requires_matching_qualified_image_and_probes(self):
        with tempfile.TemporaryDirectory() as folder:
            image, probes = Path(folder) / "small-input", Path(folder) / "probes"
            image.write_bytes(b"not a disk image; hash fixture only")
            probes.write_text(json.dumps(battery.build(Path(folder)/"coms")))
            manifest = dict(schema_version=1, dos_conformance_adapter=1, image_sha256=battery.digest(image),
                            probes_sha256=battery.digest(probes), freedos_release="1.3",
                            freedos_kernel_sha256="a" * 64, freedos_shell_sha256="b" * 64,
                            freedos_share_sha256="c" * 64, freedos_profile_sha256="d" * 64,
                            freedos_kernel=dict(version="2043",sha256="a"*64),
                            freedos_shell=dict(version="0.85a",sha256="b"*64),
                            freedos_share=dict(version="release",sha256="c"*64),
                            profile=dict(share=True, ansi=False, redirector=False,dpmi=True,xms=True,
                                         freedos_bridge=False,ciukidos_bridge=True,hardware="qemu-t23"),
                            read_back="canonical-image",payloads=[dict(path="fixture")])
            import sys
            sys.path.insert(0,str(battery.ROOT/"scripts"))
            import dos_conformance
            with patch.object(dos_conformance,"readback",return_value=manifest):
                reference.qualified(image, manifest, probes)
            for key, value in (("dos_conformance_adapter", 0), ("image_sha256", "b" * 64),
                               ("probes_sha256", "c" * 64), ("freedos_release", "master"),
                               ("freedos_kernel_sha256", "unknown")):
                wrong = dict(manifest, **{key: value})
                with self.subTest(key=key), self.assertRaises(ValueError):
                    reference.qualified(image, wrong, probes)

    def simulated_capture(self, data, timeout=False):
        # Popen, selector and read are all mocks: this never launches QEMU
        # or any subprocess. Assert the owned-process cleanup on every path.
        proc, selector = Mock(), Mock()
        proc.poll.return_value = None
        selector.select.return_value = [(SimpleNamespace(fileobj=proc.stdout), None)]
        clock = [0, 1000] if timeout else [0, 0, 0]
        with tempfile.TemporaryDirectory() as folder, \
                patch.object(reference.subprocess, "Popen", return_value=proc) as popen, \
                patch.object(reference.selectors, "DefaultSelector", return_value=selector), \
                patch.object(reference.os, "read", return_value=data), \
                patch.object(reference.time, "monotonic", side_effect=clock):
            path = Path(folder) / "serial.log"
            try:
                return reference.capture(["never-executed"], path, "12345678", "freedos", 900)
            finally:
                proc.terminate.assert_called_once()
                proc.wait.assert_called()
                proc.stdout.close.assert_called_once()
                selector.close.assert_called_once()
                self.assertEqual(path.read_bytes(), b"" if timeout else data)
                popen.assert_called_once()

    def test_reference_capture_framing_and_reap(self):
        data = ("D48 BEGIN run=12345678 guest=freedos\n" + self.transcript() +
                "\nD48 CLEAN run=12345678 guest=freedos handles=0 searches=0 locks=0 vms=0 dropped=0" +
                f"\nD48 END run=12345678 guest=freedos count={len(CASES)}\n").encode()
        self.assertEqual(len(self.simulated_capture(data)), len(CASES))
        with self.assertRaises(ValueError):
            self.simulated_capture(data.replace(b"count=", b"count=0"))

    def test_reference_timeout_and_log_cap_reap(self):
        with self.assertRaises(ValueError):
            self.simulated_capture(b"", timeout=True)
        with patch.object(reference.res, "LOG_CAP", 4), self.assertRaises(ValueError):
            self.simulated_capture(b"oversized")

    def test_dedicated_suite_cannot_pass_as_empty_generic_suite(self):
        suite = json.loads(reference.SUITE.read_text())
        self.assertNotEqual(suite["schema_version"], 1)
        self.assertEqual(suite["runner"], "tests/host/dos/run_unified_file.sh")
        self.assertEqual(suite["differential"]["guests"], ["ciukidos"])

    def test_pinned_audit_contains_no_false_runtime_claim(self):
        audit = json.loads((battery.ROOT / "docs/validation/f3-48-static-candidates.json").read_text())
        self.assertEqual(audit["runtime_trace"], "not-collected")
        self.assertEqual(len(audit["files"]), 12)
        self.assertEqual(sum(len(f["candidates"]) for f in audit["files"]), 263)
        for f in audit["files"]:
            self.assertEqual(len(f["sha256"]), 64)
            self.assertTrue(all(s["evidence"] == "unvalidated-static-candidate" for s in f["candidates"]))


if __name__ == "__main__":
    unittest.main()
