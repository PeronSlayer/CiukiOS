"""H55 provenance, build failure boundaries, read-back and boot CPU checks.

Synthetic archives/maps are negative-test fixtures, not build/VM evidence.
"""
import copy
import importlib.util
import io
import json
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest
import zipfile
from unittest.mock import patch
import conformance

ROOT = conformance.ROOT
spec = importlib.util.spec_from_file_location("h55_unified", ROOT / "scripts/unified_dos.py")
unified = importlib.util.module_from_spec(spec)
spec.loader.exec_module(unified)


class ProvenanceTests(unittest.TestCase):
    def test_all_actual_sources_and_outer_packages_match_ledger(self):
        for role, count in (("kernel", 181), ("shell", 800), ("share", 2)):
            members = unified.sources(role, ROOT / "third_party/freedos/1.3")
            self.assertEqual(len(members), count)
            self.assertEqual({n: unified.sha(b) for n, b in members.items()}, unified.ledger(role))
        kernel = unified.sources()
        self.assertIn(b"REVISION_SEQ    43", kernel["hdr/version.h"])
        self.assertIn(b"__DATE__", kernel["hdr/version.h"])
        self.assertIn(b"libm.lib ||", kernel["makefile"])

    def archive(self, entries):
        raw = io.BytesIO()
        with zipfile.ZipFile(raw, "w") as archive:
            for name, value in entries:
                archive.writestr(name, value)
        return raw.getvalue()

    def test_archive_names_sizes_hashes_and_atomic_import(self):
        valid = self.archive([("dir/a.c", b"source")])
        expected = {"dir/a.c": unified.sha(b"source")}
        self.assertEqual(unified.zip_members(valid, expected=expected), {"dir/a.c": b"source"})
        for name in ("../escape", "/absolute", "C:/absolute", "dir/../escape", "a\\b", "./a", "a//b"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                unified.zip_members(self.archive([(name, b"bad")]))
        for entries in ([('a', b'1'), ('A', b'2')], [('a', b'1'), ('a/b', b'2')]):
            with self.assertRaises(ValueError):
                unified.zip_members(self.archive(entries))
        link = zipfile.ZipInfo("link")
        link.create_system = 3; link.external_attr = (stat.S_IFLNK | 0o777) << 16
        with self.assertRaises(ValueError):
            unified.zip_members(self.archive([(link, b"target")]))
        for options in (dict(max_member=3), dict(max_total=3), dict(max_count=0),
                        dict(expected={}), dict(expected={"dir/a.c": "0" * 64})):
            with self.subTest(options=options), self.assertRaises(ValueError):
                unified.zip_members(valid, **options)
        with self.assertRaises(zipfile.BadZipFile):
            unified.zip_members(valid[:40])
        parent = ROOT / "build/host/dos"; parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as name:
            dest = Path(name) / "source"
            with self.assertRaises(ValueError):
                unified.extract(unified.zip_members(valid, expected={}), dest)
            self.assertFalse(dest.exists())
            with self.assertRaises(ValueError): unified.extract({"../escape": b"bad"}, dest)
            self.assertFalse(dest.exists())
            unified.extract(unified.zip_members(valid, expected=expected), dest)
            self.assertEqual((dest / "dir/a.c").read_bytes(), b"source")
            with self.assertRaises(ValueError): unified.extract({}, dest)
        with self.assertRaises(ValueError): unified.build_path(ROOT / "third_party/escape")

    def test_corrupt_missing_import_and_ledger_are_rejected(self):
        parent = ROOT / "build/host/dos"; parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as name:
            folder = Path(name)
            with patch.object(unified, "VENDOR", folder):
                with self.assertRaises(FileNotFoundError): unified.sources()
                (folder / unified.SOURCE_PINS["kernel"][0]).write_bytes(b"corrupt")
                with self.assertRaises(ValueError): unified.sources()
        with patch.object(unified, "ledger", return_value={}):
            with self.assertRaises(ValueError): unified.sources()

    def test_outer_missing_corrupt_and_unsafe_pins_are_rejected(self):
        parent = ROOT / "build/host/dos"; parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as name:
            folder = Path(name)
            pin = json.loads((ROOT / "third_party/freedos/1.3/UPSTREAM.json").read_text())
            (folder / "UPSTREAM.json").write_text(json.dumps(pin))
            with self.assertRaises(FileNotFoundError): unified.sources(folder=folder)
            (folder / "kernel.zip").write_bytes(b"corrupt")
            with self.assertRaises(ValueError): unified.sources(folder=folder)
            pin["kernel"]["archive"] = "../kernel.zip"
            (folder / "UPSTREAM.json").write_text(json.dumps(pin))
            with self.assertRaises(ValueError): unified.sources(folder=folder)

    def test_installer_and_each_extracted_tool_pin(self):
        tools, pin = unified.prepare_tools()
        for name, row in pin["tools"].items():
            self.assertEqual(unified.digest(tools / "binl" / name), row["sha256"])
        source = ROOT / pin["source"]["path"]
        self.assertEqual(source.stat().st_size, pin["source"]["size"])
        self.assertEqual(unified.digest(source), pin["source"]["sha256"])
        unified.host_environment()


class BuildTests(unittest.TestCase):
    def test_native_tool_failure_preserves_source_and_evidence(self):
        parent = ROOT / "build/host/dos"; parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as name:
            failure = {n: dict(returncode=-31, output="") for n in ("wmake", "wcc", "wlink", "wlib")}
            with patch.object(unified, "tool_probe", return_value=failure):
                record = unified.build(Path(name) / "attempt", mtime=946684800)
            self.assertEqual(record["status"], "blocked")
            self.assertEqual(record["execution"], failure)
            self.assertEqual(record["source_hashes"], unified.ledger("kernel"))
            self.assertFalse((Path(record["root"]) / "KERNEL.SYS").exists())
            self.assertEqual(json.loads((Path(record["root"]) / "build.json").read_text()), record)

    def test_comparison_requires_raw_bytes_roots_mtimes_dates_and_manifests(self):
        parent = ROOT / "build/host/dos"; parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as name:
            roots = [Path(name) / "a", Path(name) / "longer-b"]
            records = []
            for i, root in enumerate(roots):
                root.mkdir(); (root / "KERNEL.SYS").write_bytes(b"synthetic")
                records.append(dict(status="built-unqualified", root=str(root), input_mtime=i,
                    wall_date=f"2026-10-{21+i}T00:00:00+00:00", kernel_sha256=unified.sha(b"synthetic"), size=9,
                    source_hashes={}, source_archive_sha256="test", config="test", forced_header_sha256="test",
                    host={}, tools={}, linked_objects=[]))
            self.assertTrue(unified.compare(*records))
            for key, value in (("root", records[0]["root"]), ("input_mtime", 0),
                               ("wall_date", records[0]["wall_date"]), ("status", "blocked"),
                               ("kernel_sha256", "bad"), ("source_hashes", {"bad": "source"}),
                               ("tools", {"wcc": "wrong"}), ("linked_objects", ["crt0.obj"])):
                wrong = copy.deepcopy(records[1]); wrong[key] = value
                with self.subTest(key=key), self.assertRaises(ValueError): unified.compare(records[0], wrong)
            (roots[1] / "KERNEL.SYS").write_bytes(b"different")
            with self.assertRaises(ValueError): unified.compare(*records)

    def test_link_map_rejects_opaque_runtime_and_math_library(self):
        parent = ROOT / "build/host/dos"; parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as name:
            work = unified.extract(unified.sources(), Path(name) / "source")
            (work / "lib/libm.lib").write_bytes(b"")
            import re
            kernel_objects = set(re.findall(r"\b([A-Za-z0-9_]+)\.obj\b", (work / "kernel/makefile").read_text()))
            (work / "kernel/kernel.map").write_text(" ".join(n + ".obj" for n in sorted(kernel_objects)) + " floppy.obj device.lib")
            rows = unified.link_audit(work)
            self.assertTrue({"kernel/kernel.asm", "kernel/dosfns.c", "drivers/floppy.asm"} <= {r["source"] for r in rows})
            for text in ("kernel.obj crt0.obj", "kernel.obj clibm.lib", "no modules"):
                (work / "kernel/kernel.map").write_text(text)
                with self.assertRaises(ValueError): unified.link_audit(work)
            (work / "kernel/kernel.map").write_text("kernel.obj")
            (work / "lib/libm.lib").write_bytes(b"opaque")
            with self.assertRaises(ValueError): unified.link_audit(work)


class DistributionTests(unittest.TestCase):
    def test_corresponding_source_is_complete_deterministic_and_read_back(self):
        parent = ROOT / "build/host/dos"; parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as name:
            a = unified.corresponding_source(Path(name) / "one.zip")
            b = unified.corresponding_source(Path(name) / "two.zip")
            self.assertEqual(a["sha256"], b["sha256"])
            with zipfile.ZipFile(Path(name) / "one.zip") as archive:
                for role, (path, _, pin) in unified.SOURCE_PINS.items():
                    self.assertEqual(unified.sha(archive.read("upstream/" + path)), pin)
                for path in ("ciukios/scripts/unified_dos.py", "ciukios/tests/host/dos/reference_mbr.asm",
                             "ciukios/src/boot/full_boot.asm", "ciukios/src/boot/full_stage1_loader.asm",
                             "ciukios/third_party/freedos/1.3/boot16.bin", "build-inputs/config.mak",
                             "build-inputs/ciuki-build-date.h"):
                    self.assertIn(path, archive.namelist())

    def test_notices_profiles_identities_and_corruption_readback(self):
        ref = unified.reference_module()
        files, identity = ref.reference_files()
        identity["profile"] = dict(kernel="unified", schema_value=2, bridge=False, share=True)
        files["/FDCONFIG.SYS"] = ref.CONFIG; files["/FDAUTO.BAT"] = ref.AUTO
        files["/D48/UNI.JSN"] = json.dumps(identity).encode()
        notices, metadata = unified.notices(identity, dict(archive="source.zip", sha256="1" * 64))
        self.assertEqual(metadata["publication"], "pending-lead-verification")
        for name, data in notices.items(): files["/system/licenses/" + name] = data
        self.assertTrue(unified.readback(files.__getitem__, identity, notices))
        for path in list(files):
            if path not in ("/KERNEL.SYS", "/COMMAND.COM", "/SHARE.COM", "/FDCONFIG.SYS", "/FDAUTO.BAT", "/D48/UNI.JSN") and not path.endswith(".LIC") and not path.startswith("/system/licenses/"):
                continue
            saved = files[path]; files[path] = b"corrupt"
            with self.subTest(path=path), self.assertRaises(ValueError): unified.readback(files.__getitem__, identity, notices)
            files[path] = saved


if __name__ == "__main__":
    unittest.main()
