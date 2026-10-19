"""f4-04 T0: the production NTFS read-only mount path versus independent oracles.

Disk image (MBR, partition type 07h) -> storage -> shared cache -> ntfs_fs
adapter -> ntfs_ro library -> fs_ro views -> VFS, all compiled from the
production sources under ASan/UBSan. Names and hashes are compared with the
ntfs-3g tools' manifest (ntfsls/ntfscat), never with the parser's own output.

SPDX-License-Identifier: GPL-2.0-only
"""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build/host/ntfs"
WORK = ROOT / "build/host/ntfs-vfs"
SPEC = importlib.util.spec_from_file_location("ntfs_fixtures", ROOT / "scripts/test/ntfs_fixtures.py")
FIX = importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(FIX)

SOURCES = ["tests/host/fs/test_ntfs.c", "src/kernel/fs/fs_port.c", "src/kernel/fs/blkreq.c", "src/kernel/fs/cache.c",
           "src/kernel/fs/partition.c", "src/kernel/fs/path.c", "src/kernel/fs/fat.c",
           "src/kernel/fs/vfs.c", "src/kernel/fs/fs_ro.c", "src/kernel/fs/iso9660.c",
           "src/kernel/fs/ntfs_fs.c", "src/kernel/fs/ntfs_ro.c", "src/kernel/fs/mount.c",
           "src/kernel/core/bootlog.c", "src/kernel/drivers/blkpart.c",
           "src/kernel/lib/sha256.c", "src/kernel/lib/fmt.c"]
# Mount refusal (manifest reason -> diagnostic, error) and the VFS name model
# are shared with the QEMU evidence checker in scripts/test/ntfs_fixtures.py.
REFUSALS = {code: (error, diagnostic) for code, (diagnostic, error) in FIX.REFUSAL.items()}


class NtfsVfs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        absent = [t for t in (*FIX.TOOLS, "clang") if not shutil.which(t)]
        if absent: raise unittest.SkipTest("NTFS fixture tools missing: " + ", ".join(absent))
        script_hash = FIX.sha(ROOT / "scripts/test/ntfs_fixtures.py")
        marker = OUT / "generator.sha256"
        if not marker.exists() or marker.read_text().strip() != script_hash:
            cls.manifest = FIX.build(OUT)
            marker.write_text(script_hash + "\n")
        else: cls.manifest = json.loads((OUT / "manifest.json").read_text())
        cls.fixtures = {r["kind"]: r for r in cls.manifest["fixtures"]}
        WORK.mkdir(parents=True, exist_ok=True)
        cls.binary = WORK / "test_ntfs"
        subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections",
                        "-Wl,--gc-sections", "-DFS_HOST", "-D_POSIX_C_SOURCE=200809L", "-pthread",
                        "-I", str(ROOT / "src/kernel/include"), "-I", str(ROOT / "src/kernel/fs"),
                        *[str(ROOT / s) for s in SOURCES], "-o", str(cls.binary)], check=True, capture_output=True)

    def disk(self, kind):
        image = OUT / self.fixtures[kind]["image"]
        self.assertEqual(FIX.sha(image), self.fixtures[kind]["image_sha256"], "fixture changed")
        path = WORK / (kind + ".disk")
        if not path.exists() or path.stat().st_mtime < image.stat().st_mtime: FIX.disk_image(image, path)
        return path

    def run_mode(self, kind, *mode, expect=0):
        disk = self.disk(kind); before = (FIX.sha(disk), FIX.sha(OUT / self.fixtures[kind]["image"]))
        result = subprocess.run([str(self.binary), str(disk), *mode], capture_output=True, text=True,
                                env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0:halt_on_error=1",
                                     "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, expect, result.stdout + "\n" + result.stderr)
        self.assertEqual(before, (FIX.sha(disk), FIX.sha(OUT / self.fixtures[kind]["image"])), "read-only image changed")
        return result.stdout

    def tree(self, kind):
        out = self.run_mode(kind, "tree"); rows = {"M": None, "D": {}, "F": {}, "L": {}, "S": {}, "E": {}, "C": None}
        for line in out.splitlines():
            f = line.split("\t")
            if f[0] == "M": rows["M"] = dict(error=int(f[1]), diagnostic=f[2], drive=int(f[3]), omitted=int(f[4]))
            elif f[0] == "D": rows["D"][f[1]] = dict(attr=int(f[2]), alias=f[3])
            elif f[0] == "F": rows["F"][f[1]] = dict(size=int(f[2]), sha256=f[3], attr=int(f[4]), alias=f[5])
            elif f[0] == "L": rows["L"][f[1]] = int(f[2])
            elif f[0] == "S": rows["S"][f[1]] = int(f[2])
            elif f[0] == "E": rows["E"][f[1]] = int(f[2])
            elif f[0] == "C": rows["C"] = dict(kv.split("=") for kv in f[1].split())
        return rows

    def test_trees_hashes_and_listings_match_independent_manifest(self):
        for kind in ("clean", "fragmented", "negative", "mft-fragmented", "tail", "names", "attributes", "cluster-512"):
            with self.subTest(kind=kind):
                actual = self.tree(kind); self.assertEqual((actual["M"]["error"], actual["M"]["diagnostic"]), (0, "ntfs"))
                expected, omitted = FIX.vfs_view(self.fixtures[kind])
                directories = sorted(path for path, oracle in expected.items() if oracle == "directory")
                expected = {path: oracle for path, oracle in expected.items() if oracle != "directory"}
                self.assertEqual(sorted(actual["F"]), sorted(expected))
                self.assertEqual(sorted(actual["D"]), directories)
                for path, oracle in expected.items():
                    if oracle is None: continue
                    self.assertEqual({"size": actual["F"][path]["size"], "sha256": actual["F"][path]["sha256"]}, oracle, path)
                self.assertEqual((actual["S"], actual["E"], actual["L"]), ({}, {}, {}))
                c = actual["C"]
                self.assertEqual(int(c["omitted"]), omitted)
                self.assertEqual((c["writes"], c["flushes"], c["io_writes"], c["io_flushes"]), ("0", "0", "0", "0"))
                self.assertGreater(int(c["reads"]), 0)

    def test_dos_aliases_are_the_stored_names(self):
        actual = self.tree("clean")
        self.assertEqual(actual["F"]["/Long filename with spaces.txt"]["alias"], "LONGFI~1.TXT")
        # Entries without a stored DOS name get deterministic generated aliases.
        self.assertEqual(actual["F"]["/resident.txt"]["alias"], "RESIDENT.TXT")
        self.assertTrue(actual["F"]["/" + "n" * 240 + ".txt"]["alias"].startswith("NNNNNN~"))

    def test_unsupported_and_damaged_files_are_listed_but_never_opened(self):
        for kind, name, status in (("compressed", "/compressed.bin", -95), ("encrypted", "/allocated.bin", -95),
                                   ("reparse", "/allocated.bin", -95), ("runlist", "/allocated.bin", -5),
                                   ("attribute-list", "/fragment.bin", -5)):
            with self.subTest(kind=kind):
                actual = self.tree(kind); self.assertEqual(actual["M"]["error"], 0)
                self.assertEqual(actual["S"].get(name), status)
                self.assertNotIn(name, actual["F"])

    def test_sparse_file_over_4_gib_is_listed_as_large(self):
        actual = self.tree("sparse")
        self.assertEqual(actual["L"]["/huge.bin"], (1 << 32) + 65537)

    def test_large_directory_exceeds_view_bound_explicitly(self):
        actual = self.tree("large")
        self.assertEqual(actual["E"]["/many"], -28)           # ENOSPC, never a truncated listing
        self.assertEqual(actual["M"]["error"], 0)

    def test_behaviors(self):
        for kind in ("clean", "names", "attributes", "compressed", "encrypted", "reparse", "runlist",
                     "attribute-list", "sparse", "large", "index-cycle"):
            with self.subTest(kind=kind):
                self.assertIn("PASS behavior " + kind, self.run_mode(kind, "behavior", kind))

    def test_mount_refusals_carry_distinct_reasons_and_never_attach(self):
        for kind in ("dirty", "recovery", "hibernated", "fixup", "index", "sequence", "bitmap",
                     "log-recovery", "log-hibernated", "log-fixup", "index-fixup", "index-vcn"):
            with self.subTest(kind=kind):
                out = self.run_mode(kind, "refusal")
                self.assertIn("R\t%d\t%s" % REFUSALS[self.fixtures[kind]["reason"]], out.splitlines())

    def test_unmount_remount_stale_handles_and_generations(self):
        self.assertIn("PASS lifecycle", self.run_mode("clean", "lifecycle"))

    def test_failure_injection_at_every_read_and_allocation(self):
        self.assertIn("PASS faults", self.run_mode("clean", "faults"))


if __name__ == "__main__": unittest.main()
