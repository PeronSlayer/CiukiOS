"""f4-04: the canonical runner's NTFS support, without QEMU.

Fixture disk allocation, QEMU drive arguments, the generated MBR disk, the
independent evidence checker (scripts/test/ntfs_fixtures.py verify_records) and
the f4-media suite predicates, driven by records shaped exactly like those of
the kernel's f4:ntfs-ro probe (src/kernel/probes/f4_probes_media.c).

SPDX-License-Identifier: GPL-2.0-only
"""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts/test"))
import run as runner
SPEC = importlib.util.spec_from_file_location("ntfs_fixtures", ROOT / "scripts/test/ntfs_fixtures.py")
FIX = importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(FIX)
OUT = ROOT / "build/host/ntfs"


def sha(text): return hashlib.sha256(text.encode()).hexdigest()


def probe_records(kind, row, mutations=None):
    """What a correct f4:ntfs-ro run records for this oracle row."""
    records = [dict(event="BEGIN")]
    if "reason" in row:
        diagnostic, error = FIX.REFUSAL[row["reason"]]
        return records + [
            dict(event="DATA", case="device", slot=0, disk=1, part=1, sectors=65536, result=error, reason=diagnostic, drive="-"),
            dict(event="DATA", case="refusal", slot=0, result=error, reason=diagnostic, letter_absent=1, writes=0),
            dict(event="END", status="PASS")]
    view, omitted = FIX.vfs_view(row); statuses = FIX.EXPECT_STATUS.get(kind, {})
    handles = any(statuses.get(path, 0) == 0 and oracle != "directory" for path, oracle in view.items())
    records.append(dict(event="DATA", case="device", slot=0, disk=2, part=1, sectors=65536, result=0, reason="ntfs", drive="D"))
    records.append(dict(event="DATA", case="peer", stage="before", result=1))
    for cycle in (1, 2):
        for number, (path, oracle) in enumerate(sorted(view.items()), 1):
            status = statuses.get(path, 0)
            records.append(dict(event="DATA", case="entry", cycle=cycle, id=number, kind="directory" if oracle == "directory" else "file",
                                attr=49 if oracle == "directory" else 33, status=status, plen=len(path.encode()), path_sha256=sha(path)))
            if oracle == "directory":
                continue
            if status:
                records.append(dict(event="DATA", case="status", cycle=cycle, id=number, open=status, stat=status))
            elif oracle["size"] > 0xffffffff:
                records.append(dict(event="DATA", case="large", cycle=cycle, id=number, size=oracle["size"], open=-27))
            else:
                records.append(dict(event="DATA", case="file", cycle=cycle, id=number, size=oracle["size"], sha256=oracle["sha256"]))
        records.append(dict(event="DATA", case="names", cycle=cycle, omitted=omitted))
        if cycle == 1:
            records.append(dict(event="DATA", case="cycle", cycle=1, entries=len(view), result=0))
            for op in range(mutations if mutations is not None else (11 if handles else 9)):
                records.append(dict(event="DATA", case="mutation", operation=op, result=-30))
            records.append(dict(event="DATA", case="ads", result=-95))
            if handles: records.append(dict(event="DATA", case="unmount", stage="busy", result=-16, mounted=1))
            records.append(dict(event="DATA", case="peer", stage="after", result=1))
            records.append(dict(event="DATA", case="unmount", stage="clean", result=0, letter_absent=1,
                                generation_before=1, generation_after=2, cwd_expired=1))
            records.append(dict(event="DATA", case="gone", stat=-2))
            records.append(dict(event="DATA", case="remount", result=3, reason="ntfs", generation=3))
    records.append(dict(event="DATA", case="cycle", cycle=2, entries=len(view), result=0))
    records.append(dict(event="DATA", case="counters", reads=9000, view_writes=0, view_flushes=0, cache_writes=0))
    records.append(dict(event="END", status="PASS"))
    return records


def parsed(records):
    parser = runner.Parser("12345678", "ntfs-ro")
    for sequence, row in enumerate(records, 1):
        parser.feed((f"CIUKI_TEST v=1 run=12345678 seq={sequence:06d} probe=ntfs-ro " +
                     " ".join(f"{key}={value}" for key, value in row.items())).encode())
    return parser


class NtfsRunner(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not (OUT / "manifest.json").exists():
            raise unittest.SkipTest("NTFS fixtures not generated (tests/host/test_ntfs_ro.py builds them)")
        cls.rows = {r["kind"]: r for r in json.loads((OUT / "manifest.json").read_text())["fixtures"]}
        cls.suite = runner.load(ROOT / "tests/suites/f4-media.json")
        cls.cases = [c for c in cls.suite["cases"] if c["probe"] == "ntfs-ro"]

    def test_suite_has_both_profiles_and_the_directive_cases(self):
        kinds = {profile: sorted(c["fixtures"][-1]["kind"] for c in self.cases if c["profile"] == profile)
                 for profile in ("qemu-t23", "qemu-desktop-2002")}
        self.assertEqual(kinds["qemu-t23"], sorted(("clean", "fragmented", "names", "compressed", "dirty", "hibernated")))
        self.assertEqual(kinds["qemu-t23"], kinds["qemu-desktop-2002"])
        self.assertEqual(len({c["id"] for c in self.cases}), len(self.cases))

    def test_ide_slots_and_drive_arguments(self):
        with self.assertRaises(runner.res.Refusal):
            runner.fixture_disks({"fixtures": [{"generator": "mkntfs"}, {"generator": "mkntfs"}]})
        with self.assertRaises(runner.res.Refusal):
            runner.fixture_disks({"fixtures": [{"generator": "mkfs.fat"}, {"generator": "mkntfs"}, {"generator": "xorriso"},
                                               {"generator": "mkfs.fat"}]})
        self.assertEqual(runner.fixture_disks({"fixtures": [{"generator": "mkfs.fat"}, {"generator": "mkntfs"}]}), [(0, 1), (1, 2)])
        self.assertEqual(runner.fixture_disks({"fixtures": [{"generator": "mkntfs"}]}), [(0, 1)])
        for case in self.cases:
            profile = runner.load(ROOT / f"tests/profiles/{case['profile']}.json")
            args, request = runner.qemu_args("fixture", profile, case, "12345678", Path("overlay"), Path("bios"))
            self.assertEqual(request, "f4:ntfs-ro run=12345678")
            drives = [a for a in args if a.startswith("file=fixture-")]
            ntfs = [a for a in drives if "id=ciuki-ntfs" in a]
            self.assertEqual(len(ntfs), 1, case["id"])
            peers = len(case["fixtures"]) - 1
            self.assertIn(f"index={peers + 1},", ntfs[0] + ",")
            self.assertTrue(ntfs[0].startswith(f"file=fixture-{peers}.img,format=raw,if=ide"))
            self.assertEqual(len(drives), len(case["fixtures"]))

    def test_generated_disk_has_one_type_07_partition_with_the_volume_verbatim(self):
        with tempfile.TemporaryDirectory(dir=ROOT / "build/host") as folder:
            item = FIX.qemu_fixture({"generator": "mkntfs", "kind": "clean", "seed": 1}, Path(folder), 0)
            disk = Path(item["path"]).read_bytes(); mbr = disk[:512]
            self.assertEqual((mbr[510:512], mbr[450]), (b"\x55\xaa", 7))
            start, count = struct.unpack_from("<II", mbr, 454)
            self.assertEqual((start, count * 512), (2048, self.rows["clean"]["image_bytes"]))
            self.assertEqual(hashlib.sha256(disk[start * 512:(start + count) * 512]).hexdigest(), self.rows["clean"]["image_sha256"])
            self.assertEqual(item["oracle"]["image_sha256"], self.rows["clean"]["image_sha256"])
            self.assertEqual(item["volume_sha256"], self.rows["clean"]["image_sha256"])
            self.assertEqual(sorted(p.name for p in Path(folder).iterdir()), ["fixture-0.img"])
            with self.assertRaises(ValueError): FIX.qemu_fixture({"kind": "sector-4096", "seed": 1}, Path(folder), 1)
            with self.assertRaises(ValueError): FIX.qemu_fixture({"kind": "clean", "seed": 2}, Path(folder), 1)

    def test_suite_predicates_and_oracle_accept_a_correct_run(self):
        for case in self.cases:
            kind = case["fixtures"][-1]["kind"]; row = self.rows[kind]
            records = probe_records(kind, row)
            with self.subTest(case=case["id"]):
                self.assertTrue(parsed(records).check(case["expected"]))
                FIX.verify_records({"kind": kind, "oracle": row}, parsed(records).records)

    def test_oracle_rejects_every_kind_of_deviation(self):
        row = self.rows["clean"]; item = {"kind": "clean", "oracle": row}
        base = probe_records("clean", row)
        def mutate(change):
            records = copy.deepcopy(base); change(records)
            with self.assertRaises(ValueError): FIX.verify_records(item, parsed(records).records)
        files = [i for i, r in enumerate(base) if r.get("case") == "file"]
        entries = [i for i, r in enumerate(base) if r.get("case") == "entry"]
        mutate(lambda r: r[files[0]].update(sha256="0" * 64))
        mutate(lambda r: r[files[0]].update(size=1))
        mutate(lambda r: r.pop(files[-1]))
        mutate(lambda r: r.pop(entries[0]))
        mutate(lambda r: r[entries[0]].update(path_sha256=sha("/invented")))
        mutate(lambda r: r[entries[0]].update(status=-95))
        mutate(lambda r: r[entries[0]].update(plen=3))
        mutate(lambda r: r.insert(3, dict(event="DATA", case="walkerr", cycle=1, result=-5, path_sha256="0" * 64)))
        mutate(lambda r: [x.update(omitted=0) for x in r if x.get("case") == "names"])
        mutate(lambda r: [x.update(reason="ntfs_dirty") for x in r if x.get("case") == "device"])
        mutate(lambda r: [x.update(result=-5) for x in r if x.get("case") == "device"])
        # The second cycle is checked as strictly as the first.
        second = [i for i, r in enumerate(base) if r.get("case") == "file" and r["cycle"] == 2]
        mutate(lambda r: r[second[0]].update(sha256="f" * 64))
        # Suite predicates reject writes, a skipped mutation and a lost unmount.
        expected = [c for c in self.cases if c["id"] == "ntfs-clean-qemu-t23"][0]["expected"]
        for change in (lambda r: [x.update(view_writes=1) for x in r if x.get("case") == "counters"],
                       lambda r: [x.update(result=0) for x in r if x.get("case") == "mutation" and x["operation"] == 4],
                       lambda r: [x.update(letter_absent=0) for x in r if x.get("stage") == "clean"],
                       lambda r: [x.update(result=0) for x in r if x.get("stage") == "busy"]):
            records = copy.deepcopy(base); change(records)
            with self.assertRaises(runner.EvidenceError): parsed(records).check(expected)

    def test_refusals_and_unsupported_content(self):
        dirty = self.rows["dirty"]; item = {"kind": "dirty", "oracle": dirty}
        FIX.verify_records(item, parsed(probe_records("dirty", dirty)).records)
        for index in (1, 2):
            records = probe_records("dirty", dirty); records[index]["reason"] = "ntfs_malformed"
            with self.assertRaises(ValueError): FIX.verify_records(item, parsed(records).records)
        records = probe_records("dirty", dirty); records.insert(3, dict(event="DATA", case="entry", cycle=1, id=1, kind="file", attr=33, status=0, plen=1, path_sha256="0" * 64))
        with self.assertRaises(ValueError): FIX.verify_records(item, parsed(records).records)
        compressed = self.rows["compressed"]
        records = probe_records("compressed", compressed)
        FIX.verify_records({"kind": "compressed", "oracle": compressed}, parsed(records).records)
        for r in records:
            if r.get("case") == "status": r["open"] = 0
        with self.assertRaises(ValueError): FIX.verify_records({"kind": "compressed", "oracle": compressed}, parsed(records).records)

    def test_missing_fixture_is_not_run_never_pass(self):
        for probe, reason in (("ntfs-ro", "fixtures_absent"), ("optical", "device_absent")):
            parser = runner.Parser("12345678", probe)
            for sequence, row in enumerate((dict(event="BEGIN"), dict(event="DATA", status="not_run", reason=reason),
                                            dict(event="END", status="NOT_RUN", reason=reason)), 1):
                parser.feed((f"CIUKI_TEST v=1 run=12345678 seq={sequence:06d} probe={probe} " +
                             " ".join(f"{key}={value}" for key, value in row.items())).encode())
            self.assertEqual((parser.outcome, parser.terminal["reason"]), ("not_run", reason))
            with self.assertRaises(runner.EvidenceError): parser.check(self.cases[0]["expected"])
        for probe, status, reason in (("ntfs-ro", "NOT_RUN", "device_absent"), ("ntfs-ro", "NOT_RUN", "other"),
                                      ("fat-read", "NOT_RUN", "fixtures_absent")):
            parser = runner.Parser("12345678", probe)
            parser.feed(f"CIUKI_TEST v=1 run=12345678 seq=000001 probe={probe} event=BEGIN".encode())
            with self.assertRaises(runner.EvidenceError):
                parser.feed(f"CIUKI_TEST v=1 run=12345678 seq=000002 probe={probe} event=END status={status} reason={reason}".encode())

    def test_names_fixture_expectation_uses_dos_aliases(self):
        view, omitted = FIX.vfs_view(self.rows["names"])
        self.assertEqual(sorted(view), ["/SMILE~1.TXT", "/Sub dir", "/Sub dir/Mixed Case.TXT", "/Sub dir/deeper",
                                        "/Sub dir/deeper/leaf.txt", "/Sub dir/inner.txt", "/plain ascii.txt", "/\u1f80.txt"])
        self.assertEqual({p for p, v in view.items() if v == "directory"}, {"/Sub dir", "/Sub dir/deeper"})
        self.assertEqual(omitted, 1)
        self.assertEqual(view["/SMILE~1.TXT"]["size"], 21)


if __name__ == "__main__": unittest.main()
