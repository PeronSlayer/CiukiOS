"""H55 provenance, build failure boundaries, read-back and boot CPU checks.

Synthetic archives/maps are negative-test fixtures, not build/VM evidence.
"""
import copy
import importlib.util
import io
import json
from pathlib import Path
import re
import shlex
import stat
import subprocess
import tarfile
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
    def test_reference_is_versioned_without_provisioning_symlink(self):
        ref = unified.reference_module()
        self.assertEqual(ref.RELEASE, ROOT / "third_party/freedos/1.3")
        self.assertFalse((ROOT / "third_party/freedos/freedos").is_symlink())
        with tempfile.TemporaryDirectory() as directory, self.assertRaisesRegex(ValueError, "reference absent"):
            ref.reference_files(Path(directory))

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

    def test_sys_dependencies_complete_and_pinned_before_extraction(self):
        tools, pins = unified.prepare_tools()
        required = {"h/stdlib.h", "h/dos.h", "h/ctype.h", "h/fcntl.h", "h/sys/stat.h",
                    "h/mem.h", "h/memory.h", "h/string.h", "h/stdio.h", "h/stddef.h",
                    "h/io.h", "h/unistd.h", "h/errno.h", "h/utime.h", "h/_comdef.h",
                    "lib286/dos/clibs.lib", "lib286/dos/doslfns.lib", "lib286/dos/cstart_t.obj"}
        self.assertTrue(required <= pins["support_files"].keys())
        self.assertIn("wcl", pins["tools"])
        for name, row in pins["support_files"].items():
            self.assertEqual((tools / name).stat().st_size, row["size"])
            self.assertEqual(unified.digest(tools / name), row["sha256"])
        self.assertIn(b"libfile cstart_t.obj", (tools / "binl/wlsystem.lnk").read_bytes())
        parent = ROOT / "build/host/dos"; parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as directory:
            out = Path(directory) / "tools"
            for missing in ("h/dos.h", "lib286/dos/clibs.lib"):
                bad = copy.deepcopy(pins); del bad["support_files"][missing]
                with patch.object(unified.json, "loads", return_value=bad), self.assertRaises(ValueError):
                    unified.prepare_tools(out)
                self.assertFalse(out.exists())
            bad = copy.deepcopy(pins); bad["support_files"]["h/stdlib.h"]["sha256"] = "0" * 64
            with patch.object(unified.json, "loads", return_value=bad), self.assertRaises(ValueError):
                unified.prepare_tools(out)
            self.assertFalse(out.exists())
            # Existing binl-only installations gain the missing trees without
            # replacing the already verified native tools.
            unified.extract({str(p.relative_to(tools)): p.read_bytes()
                             for p in (tools / "binl").iterdir() if p.is_file()} |
                            {"license.txt": (tools / "license.txt").read_bytes()}, out)
            before = (out / "binl/wcc").stat().st_ino
            unified.prepare_tools(out)
            self.assertEqual((out / "binl/wcc").stat().st_ino, before)
            (out / "h/stdlib.h").write_bytes(b"corrupt")
            with self.assertRaisesRegex(ValueError, "modified"):
                unified.prepare_tools(out)

    def test_upx_archive_tools_and_vendor_failure_preserve_packed_pin(self):
        executable, pins = unified.prepare_upx()
        self.assertEqual(unified.digest(executable), pins["members"][pins["executable"]]["sha256"])
        packed = unified.reference_module().reference_files()[0]["/KERNEL.SYS"]
        if pins["vendor"]["unpacked"]["sha256"] is None:
            with self.assertRaisesRegex(ValueError, "NotPackedException"):
                unified.unpacked_vendor()
        else:
            raw, provenance = unified.unpacked_vendor()
            self.assertEqual(unified.sha(raw), pins["vendor"]["unpacked"]["sha256"])
            self.assertEqual(provenance["vendor"], pins["vendor"])
        self.assertEqual(packed, unified.reference_module().reference_files()[0]["/KERNEL.SYS"])
        bad = copy.deepcopy(pins); bad["archive"]["sha256"] = "0" * 64
        with patch.object(unified.json, "loads", return_value=bad), self.assertRaisesRegex(ValueError, "archive pin"):
            unified.prepare_upx()

    def test_upx_comparison_uses_only_verified_unpacked_representation(self):
        kernel = b'code\0FreeDOS kernel - GIT (build 2043 OEM:0xfd) [compiled Oct 21 2026]\n\0data'
        raw = kernel.replace(b'Oct 21 2026', b'May 14 2021')
        packed = b'packed synthetic vendor, not runtime evidence'
        _, original = unified.prepare_upx()
        pins = copy.deepcopy(original)
        pins["vendor"]["packed"] = dict(size=len(packed), sha256=unified.sha(packed))
        pins["vendor"]["unpacked"] = dict(size=len(raw), sha256=unified.sha(raw))
        def unpack(command, **kwargs):
            self.assertEqual(command[1], "-d")
            target = Path(command[2]); self.assertEqual(target.read_bytes(), packed)
            target.write_bytes(raw)
            return subprocess.CompletedProcess(command, 0, "synthetic unpack", "")
        with tempfile.TemporaryDirectory(dir=ROOT / "build/codex") as directory, \
             patch.object(unified, "UPX_EVIDENCE", Path(directory) / "synthetic.json"), \
             patch.object(unified, "prepare_upx", return_value=(Path('/synthetic/upx'), pins)), \
             patch.object(unified, "reference_module") as ref, \
             patch.object(unified.subprocess, "run", side_effect=unpack):
            ref.return_value.reference_files.return_value = ({"/KERNEL.SYS": packed}, {})
            result = unified.pristine_comparison(kernel)
            self.assertEqual(result["reference_sha256"], unified.sha(raw))
            self.assertEqual(result["reference_representation"]["vendor"], pins["vendor"])
            pins["vendor"]["unpacked"]["sha256"] = "0" * 64
            with self.assertRaisesRegex(ValueError, "unpacked vendor kernel pin"):
                unified.pristine_comparison(kernel)
            pins["vendor"]["packed"]["sha256"] = "0" * 64
            with self.assertRaisesRegex(ValueError, "packed vendor kernel pin"):
                unified.pristine_comparison(kernel)

    def test_upx_unsafe_incomplete_and_corrupt_tar_rejected_before_import(self):
        _, original = unified.prepare_upx()
        parent = ROOT / "build/host/dos"; parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as directory:
            folder = Path(directory); archive = folder / "upx.tar.xz"; pinfile = folder / "pin.json"
            good = tarfile.TarInfo("tools/upx"); good.size = 4
            link = tarfile.TarInfo("tools/link"); link.type = tarfile.SYMTYPE; link.linkname = "/outside"
            bomb = tarfile.TarInfo("tools/bomb"); bomb.size = 5 * 1024 * 1024
            cases = [[(tarfile.TarInfo(name), b"")] for name in ("../escape", "/absolute", "a/../b", "a\\b")]
            cases += [[(good, b"upx!"), (good, b"upx!")], [(link, b"")], [], [(good, b"bad!")],
                      [(bomb, b"0" * bomb.size)]]
            for entries in cases:
                with self.subTest(names=[i.name for i, _ in entries]):
                    with tarfile.open(archive, "w:xz") as tar:
                        for info, data in entries: tar.addfile(info, io.BytesIO(data))
                    pins = copy.deepcopy(original)
                    pins["archive"].update(path=str(archive.relative_to(ROOT)), size=archive.stat().st_size,
                                           sha256=unified.digest(archive))
                    pins["members"] = {"tools/upx": dict(size=4, sha256=unified.sha(b"upx!"))}
                    pins["executable"] = "tools/upx"
                    pinfile.write_text(json.dumps(pins))
                    with patch.object(unified, "UPX_PIN", pinfile), self.assertRaises(ValueError):
                        unified.prepare_upx(folder / "out")
                    self.assertFalse((folder / "out").exists())


class BuildTests(unittest.TestCase):
    def test_patch_changes_only_two_date_sites_and_preserves_upstream_default(self):
        original = unified.sources()
        changed = unified.patched_sources(original)
        manifest, _, _ = unified.reproducible_inputs()
        self.assertEqual({n for n in original if original[n] != changed[n]}, {"hdr/version.h", "sys/sys.c"})
        for name, data in changed.items():
            for line in data.splitlines():
                if re.search(rb'\b__(?:DATE|TIME|TIMESTAMP)__\b', line):
                    self.assertEqual(line, b'#define CIUKI_BUILD_DATE __DATE__', name)
        self.assertEqual({n: unified.sha(changed[n]) for n in manifest["source_hashes"]}, manifest["source_hashes"])
        self.assertIn('ALLCFLAGS=' + ' '.join(unified.date_flags(manifest)) + ' \n', unified.build_config(manifest))
        self.assertNotIn('-fi', unified.build_config(manifest))
        make = next(line[5:] for line in unified.build_config(manifest).splitlines() if line.startswith("MAKE="))
        # Preserve the pinned SYS flags while supplying the date there too;
        # sys/makefile does not consume ALLCFLAGS.
        upstream_flags = re.search(rb'^CFLAGST=(.+)', original["mkfiles/watcom.mak"], re.M)[1].strip().decode()
        self.assertEqual(shlex.split(make), ["wmake", "-ms", "-h",
            "CFLAGST=-fo=.obj " + upstream_flags + " " + " ".join(unified.date_flags(manifest))])
        # Real host preprocessing checks fallback semantics, not OW execution.
        parent = ROOT / "build/host/dos"; parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as name:
            header = Path(name) / "version.h"
            source = b'#include "version.h"\nconst char version[] = KERNEL_VERSION_STRING;\n'
            def preprocess(data, flags=()):
                header.write_bytes(data)
                return subprocess.check_output(["gcc", "-E", "-P", "-x", "c", "-I", name, *flags, "-"], input=source)
            upstream = preprocess(original["hdr/version.h"])
            self.assertEqual(preprocess(changed["hdr/version.h"]), upstream)
            fixed = preprocess(changed["hdr/version.h"], ["-DCIUKI_BUILD_DATE=\"" + manifest["date"] + "\""])
            self.assertIn(('"' + manifest["date"] + '"').encode(), fixed)

    def test_patch_pin_context_postimage_and_manifest_are_checked(self):
        manifest, _, raw = unified.reproducible_inputs()
        members = unified.sources()
        bad = dict(members); bad["hdr/version.h"] += b"\n"
        with self.assertRaisesRegex(ValueError, "pristine"):
            unified.patched_sources(bad)
        parent = ROOT / "build/host/dos"; parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as name:
            folder = Path(name); path = folder / manifest["patch"]["path"]
            path.parent.mkdir(parents=True)
            pin = folder / "ciuki/build.json"
            with patch.object(unified, "VENDOR", folder), patch.object(unified, "BUILD_MANIFEST", pin):
                for data, update_hash, error in (
                    (raw + b"bad", False, "SHA-256"),
                    (raw.replace(b"-#define KVS(v,s,o)", b"-#define WRONG(v,s,o)", 1), True, "context"),
                    (raw.replace(b"+#define CIUKI_BUILD_DATE __DATE__", b"+#define CIUKI_BUILD_DATE WRONG"), True, "postimage"),
                ):
                    current = copy.deepcopy(manifest)
                    if update_hash: current["patch"]["sha256"] = unified.sha(data)
                    pin.write_text(json.dumps(current)); path.write_bytes(data)
                    with self.subTest(error=error), self.assertRaisesRegex(ValueError, error):
                        unified.patched_sources(members)
                manifest["date"] = "Oct 22 2026"
                pin.write_text(json.dumps(manifest)); path.write_bytes(raw)
                with self.assertRaisesRegex(ValueError, "manifest"):
                    unified.patched_sources(members)

    def test_native_tool_failure_preserves_source_and_evidence(self):
        parent = ROOT / "build/host/dos"; parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as name:
            failure = {n: dict(returncode=-31, output="") for n in ("wmake", "wcc", "wlink", "wlib")}
            with patch.object(unified, "tool_probe", return_value=failure):
                record = unified.build(Path(name) / "attempt", mtime=946684800)
            self.assertEqual(record["status"], "blocked")
            self.assertEqual(record["execution"], failure)
            self.assertEqual(record["pristine_source_hashes"], unified.ledger("kernel"))
            self.assertEqual(record["source_hashes"], {n: unified.sha(b) for n, b in unified.patched_sources(unified.sources()).items()})
            self.assertFalse(record["unmodified"])
            self.assertEqual(record["modification"], unified.MODIFICATION)
            self.assertFalse((Path(record["root"]) / "KERNEL.SYS").exists())
            self.assertEqual(json.loads((Path(record["root"]) / "build.json").read_text()), record)

    def test_build_uses_manifest_macros_and_checks_postbuild_sources(self):
        parent = ROOT / "build/host/dos"; parent.mkdir(parents=True, exist_ok=True)
        manifest, _, _ = unified.reproducible_inputs()
        kernel = b'FreeDOS kernel - GIT (build 2043 OEM:0xfd) [compiled Oct 21 2026]\n' + b'\0' * 1024
        execution = {n: dict(returncode=0, output="synthetic native probe") for n in ("wmake", "wcc", "wlink", "wlib")}
        with tempfile.TemporaryDirectory(dir=parent) as name:
            for failure in (None, "probe", "source"):
                with self.subTest(failure=failure):
                    calls = []
                    def run(command, **kwargs):
                        calls.append(command)
                        work = Path(kwargs["cwd"])
                        if Path(command[0]).name == "wcc":
                            self.assertEqual(command[3:5], unified.date_flags(manifest))
                            self.assertNotIn("__DATE__", (work / "date-probe.c").read_text())
                            value = b"wrong" if failure == "probe" else (manifest["date"] + "|" + manifest["time"]).encode()
                            (work / "date-probe.obj").write_bytes(value)
                        else:
                            self.assertEqual(command, ["make", "all"])
                            self.assertEqual((work / "config.mak").read_text(), unified.build_config(manifest))
                            self.assertFalse((work / "ciuki-build-date.h").exists())
                            (work / "bin/kernel.sys").write_bytes(kernel)
                            (work / "kernel/kernel.sys").write_bytes(kernel)
                            (work / "kernel/kernel.map").write_text("synthetic map")
                            if failure == "source":
                                (work / "hdr/version.h").write_bytes(b"unexpected build edit")
                        return subprocess.CompletedProcess(command, 0)
                    with patch.object(unified, "prepare_tools", return_value=(Path("/synthetic-ow"), {"tools": {}, "support_files": {}})), \
                            patch.object(unified, "host_environment", return_value={}), \
                            patch.object(unified, "tool_probe", return_value=execution), \
                            patch.object(unified.subprocess, "run", side_effect=run), \
                            patch.object(unified, "pristine_comparison", side_effect=ValueError("synthetic vendor unavailable")), \
                            patch.object(unified, "link_audit", return_value=[]):
                        record = unified.build(Path(name) / str(failure), mtime=946684800)
                    self.assertEqual(record["status"], "built-unqualified" if failure is None else "blocked")
                    self.assertEqual(len(calls), 1 if failure == "probe" else 2)
                    self.assertEqual((Path(record["root"]) / "KERNEL.SYS").exists(), failure is None)
                    if failure is None:
                        self.assertEqual(record["pristine_comparison"]["status"], "blocked")
                        self.assertIn("synthetic vendor unavailable", record["pristine_comparison"]["error"])
                    else:
                        self.assertIn("macro probe failed" if failure == "probe" else "beyond patch 0001", record["error"])

    def test_comparison_requires_raw_bytes_roots_input_dates_patch_and_pristine(self):
        parent = ROOT / "build/host/dos"; parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as name:
            roots = [Path(name) / "a", Path(name) / "longer-b"]
            records = []
            manifest, raw, _ = unified.reproducible_inputs()
            kernel = b'code\0FreeDOS kernel - GIT (build 2043 OEM:0xfd) [compiled Oct 21 2026]\n\0data'
            pristine = kernel.replace(b'Oct 21 2026', b'May 14 2021')
            comparison = unified.masked_pristine_compare(kernel, pristine)
            comparison["reference_representation"] = {"synthetic": True}
            hashes = {n: unified.sha(b) for n, b in unified.patched_sources(unified.sources()).items()}
            for i, root in enumerate(roots):
                root.mkdir(); (root / "KERNEL.SYS").write_bytes(kernel)
                records.append(dict(schema=2, status="built-unqualified", root=str(root),
                    input_mtime=(946684800, 1609459200)[i], wall_date="2026-10-21T00:00:00+00:00",
                    unmodified=False, modification=unified.MODIFICATION, build_inputs=manifest,
                    build_manifest_sha256=unified.sha(raw), kernel_sha256=unified.sha(kernel), size=len(kernel),
                    source_hashes=hashes, pristine_source_hashes=unified.ledger("kernel"),
                    source_archive_sha256=unified.SOURCE_PINS["kernel"][2], config=unified.build_config(manifest),
                    host={}, tools={}, support_files={}, linked_objects=[], pristine_comparison=comparison))
            with patch.object(unified, "unpacked_vendor", return_value=(pristine, {"synthetic": True})):
                self.assertTrue(unified.compare(*records))
                wrong = copy.deepcopy(records[1]); wrong["pristine_comparison"] = {"status": "PASS"}
                with self.assertRaisesRegex(ValueError, "evidence"):
                    unified.compare(records[0], wrong)
            # Real packed vendor bytes cannot qualify these synthetic binaries.
            with self.assertRaises(ValueError):
                unified.compare(*records)
            for key, value in (("root", records[0]["root"]),
                               ("status", "blocked"), ("input_mtime", 946684801),
                               ("kernel_sha256", "bad"), ("source_hashes", {"bad": "source"}),
                               ("build_inputs", {}), ("build_manifest_sha256", "bad"), ("unmodified", True),
                               ("tools", {"wcc": "wrong"}), ("support_files", {"h/dos.h": "wrong"}),
                               ("linked_objects", ["crt0.obj"])):
                wrong = copy.deepcopy(records[1]); wrong[key] = value
                with self.subTest(key=key), self.assertRaises(ValueError): unified.compare_rebuilds(records[0], wrong)
            (roots[1] / "KERNEL.SYS").write_bytes(b"different")
            with self.assertRaises(ValueError): unified.compare_rebuilds(*records)

    def test_pristine_comparison_masks_only_the_unique_fixed_date(self):
        kernel = b'code\0FreeDOS kernel - GIT (build 2043 OEM:0xfd) [compiled Oct 21 2026]\n\0data'
        pristine = kernel.replace(b'Oct 21 2026', b'May 14 2021')
        result = unified.masked_pristine_compare(kernel, pristine)
        self.assertEqual(result["date_size"], 11)
        self.assertEqual(result["compared_bytes"], len(kernel) - 11)
        self.assertEqual(result["reference_sha256"], unified.sha(pristine))
        for bad in (pristine + b'\0', pristine.replace(b'code', b'Code'), pristine.replace(b'data', b'datA'),
                    pristine.replace(b'2043', b'2044'), pristine.replace(b'May', b'Bad'), b'UPX!packed',
                    pristine + pristine, b'X' + pristine[:-1]):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                unified.masked_pristine_compare(kernel, bad)
        with self.assertRaisesRegex(ValueError, "pinned build date"):
            unified.masked_pristine_compare(pristine, pristine)
        vendor = unified.reference_module().reference_files()[0]["/KERNEL.SYS"]
        self.assertEqual(len(vendor), 46256)
        self.assertEqual(vendor.find(b'UPX!'), 85)
        with self.assertRaisesRegex(ValueError, "UPX marker"):
            unified.masked_pristine_compare(kernel, vendor)

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
                for path in ("ciukios/scripts/unified_dos.py", "ciukios/config/upx.json", "ciukios/tests/host/dos/reference_mbr.asm",
                             "ciukios/src/boot/full_boot.asm", "ciukios/src/boot/full_stage1_loader.asm",
                             "ciukios/third_party/freedos/1.3/boot16.bin", "build-inputs/config.mak",
                             "ciukios/third_party/freedos/unified/ciuki/build.json",
                             "ciukios/third_party/freedos/unified/ciuki/patches/0001-reproducible-date.patch"):
                    self.assertIn(path, archive.namelist())
                manifest, raw, date_patch = unified.reproducible_inputs()
                prefix = "ciukios/third_party/freedos/unified/"
                self.assertEqual(archive.read(prefix + "ciuki/build.json"), raw)
                self.assertEqual(archive.read(prefix + manifest["patch"]["path"]), date_patch)
                self.assertEqual(archive.read("build-inputs/config.mak"), unified.build_config(manifest).encode())
                self.assertNotIn("build-inputs/ciuki-build-date.h", archive.namelist())

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
