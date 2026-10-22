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
    map_fixture = ("linker timestamp and absolute paths omitted from section evidence\n"
                   "Segment                Class          Group          Address         Size\n"
                   "=======                =====          =====          =======         ====\n"
                   "HMA_TEXT               CODE           DGROUP         0000:0020       00001000\n"
                   "\n                        +--------+\n| next map section |\n")

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
        required = {"binl/wlink.lnk", "binl/wlsystem.lnk",
                    "h/stdlib.h", "h/dos.h", "h/ctype.h", "h/fcntl.h", "h/sys/stat.h",
                    "h/mem.h", "h/memory.h", "h/string.h", "h/stdio.h", "h/stddef.h",
                    "h/io.h", "h/unistd.h", "h/errno.h", "h/utime.h", "h/_comdef.h",
                    "lib286/dos/clibs.lib", "lib286/dos/doslfns.lib", "lib286/dos/cstart_t.obj"}
        self.assertTrue(required <= pins["support_files"].keys())
        self.assertIn("wcl", pins["tools"])
        for name, row in pins["support_files"].items():
            self.assertEqual((tools / name).stat().st_size, row["size"])
            self.assertEqual(unified.digest(tools / name), row["sha256"])
        self.assertIn(b"@%WATCOM%/binl/wlsystem.lnk", (tools / "binl/wlink.lnk").read_bytes())
        system = (tools / "binl/wlsystem.lnk").read_bytes()
        self.assertIn(b"system begin com", system)
        self.assertIn(b"libfile cstart_t.obj", system)
        parent = ROOT / "build/host/dos"; parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as directory:
            out = Path(directory) / "tools"
            for missing in ("h/dos.h", "lib286/dos/clibs.lib", "binl/wlink.lnk", "binl/wlsystem.lnk"):
                bad = copy.deepcopy(pins); del bad["support_files"][missing]
                with patch.object(unified.json, "loads", return_value=bad), self.assertRaises(ValueError):
                    unified.prepare_tools(out)
                self.assertFalse(out.exists())
            for corrupt in ("h/stdlib.h", "binl/wlink.lnk", "binl/wlsystem.lnk"):
                bad = copy.deepcopy(pins); bad["support_files"][corrupt]["sha256"] = "0" * 64
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
            for name in ("binl/wlink.lnk", "binl/wlsystem.lnk"):
                path = out / name
                raw = path.read_bytes()
                path.unlink()
                with self.assertRaises(FileNotFoundError):
                    unified.prepare_tools(out)
                self.assertFalse(path.exists())
                path.write_bytes(b"corrupt")
                with self.assertRaisesRegex(ValueError, "modified"):
                    unified.prepare_tools(out)
                self.assertEqual(path.read_bytes(), b"corrupt")
                path.write_bytes(raw)
            (out / "h/stdlib.h").write_bytes(b"corrupt")
            with self.assertRaisesRegex(ValueError, "modified"):
                unified.prepare_tools(out)

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
        self.assertEqual((manifest["date"], manifest["time"], manifest["epoch"]),
                         ("May 14 2021", "00:00:00", 1620950400))
        self.assertIn('ALLCFLAGS=' + ' '.join(unified.date_flags()) + ' \n', unified.build_config(manifest))
        self.assertTrue(all(not any(c.isspace() for c in flag) for flag in unified.date_flags()))
        self.assertNotIn('CIUKI_BUILD_DATE=', unified.build_config(manifest))
        self.assertNotIn('__DATE__', unified.date_header(manifest))
        self.assertNotIn('__TIME__', unified.date_header(manifest))
        make = next(line[5:] for line in unified.build_config(manifest).splitlines() if line.startswith("MAKE="))
        # Preserve the pinned SYS flags while supplying the date there too;
        # sys/makefile does not consume ALLCFLAGS.
        upstream_flags = re.search(rb'^CFLAGST=(.+)', original["mkfiles/watcom.mak"], re.M)[1].strip().decode()
        self.assertEqual(shlex.split(make), ["wmake", "-ms", "-h",
            "CFLAGST=-fo=.obj " + upstream_flags + " " + " ".join(unified.date_flags())])
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
            forced = Path(name) / "ciuki-build-date.h"
            forced.write_text(unified.date_header(manifest))
            fixed = preprocess(changed["hdr/version.h"], ["-include", str(forced)])
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
        kernel = b'FreeDOS kernel - GIT (build 2043 OEM:0xfd) [compiled May 14 2021]\n' + b'\0' * 1024
        execution = {n: dict(returncode=0, output="synthetic native probe") for n in ("wmake", "wcc", "wlink", "wlib")}
        with tempfile.TemporaryDirectory(dir=parent) as name:
            for failure in (None, "probe", "source", "header", "config", "layout", "audit"):
                with self.subTest(failure=failure):
                    calls = []
                    def run(command, **kwargs):
                        calls.append(command)
                        work = Path(kwargs["cwd"])
                        env = kwargs["env"]
                        self.assertEqual(env["WATCOM"], "/synthetic-ow")
                        self.assertEqual(env["INCLUDE"], "/synthetic-ow/h")
                        self.assertEqual(env["LIB"], "/synthetic-ow/lib286:/synthetic-ow/lib286/dos")
                        self.assertEqual(env["WLINK_LNK"], "/synthetic-ow/binl/wlink.lnk")
                        self.assertEqual(env["PATH"], "/synthetic-ow/binl:/usr/bin:/bin")
                        if Path(command[0]).name == "wcc":
                            self.assertEqual(command[3:4], unified.date_flags())
                            self.assertEqual((work / "../ciuki-build-date.h").read_text(), unified.date_header(manifest))
                            probe = Path(command[-1])
                            self.assertNotIn("__DATE__", probe.read_text())
                            value = b"wrong" if failure == "probe" else (manifest["date"] + "|" + manifest["time"]).encode()
                            Path(command[-2].removeprefix("-fo=")).write_bytes(value)
                        else:
                            self.assertEqual(command, ["make", "all"])
                            self.assertEqual((work / "config.mak").read_text(), unified.build_config(manifest))
                            self.assertEqual((work / "ciuki-build-date.h").read_text(), unified.date_header(manifest))
                            (work / "bin/kernel.sys").write_bytes(kernel)
                            (work / "kernel/kernel.sys").write_bytes(kernel)
                            (work / "kernel/kernel.map").write_text("bad map" if failure == "layout" else
                                self.map_fixture + self.link_map(work) +
                                ("Module: crt0.obj(cstart.asm)\n" if failure == "audit" else ""))
                            (work / "lib/libm.lib").write_bytes(b"")
                            if failure == "source":
                                (work / "hdr/version.h").write_bytes(b"unexpected build edit")
                            if failure in ("header", "config"):
                                (work / ("ciuki-build-date.h" if failure == "header" else "config.mak")).write_text("corrupt")
                        return subprocess.CompletedProcess(command, 0)
                    with patch.object(unified, "prepare_tools", return_value=(Path("/synthetic-ow"), {"tools": {}, "support_files": {}})), \
                            patch.object(unified, "host_environment", return_value={}), \
                            patch.object(unified, "tool_probe", return_value=execution), \
                            patch.object(unified.subprocess, "run", side_effect=run):
                        record = unified.build(Path(name) / str(failure), mtime=946684800)
                    self.assertEqual(record["status"], "ok" if failure is None else "blocked")
                    self.assertEqual(len(calls), 1 if failure == "probe" else 2)
                    self.assertEqual((Path(record["root"]) / "KERNEL.SYS").exists(), failure is None)
                    if failure is None:
                        self.assertEqual(record["kernel_provenance"]["behavioural_equivalence"]["status"], "pending-lead-f3-48")
                        self.assertNotIn("pristine_comparison", record)
                    else:
                        if failure == "audit":
                            self.assertEqual(json.loads((Path(record["root"]) / "build.json").read_text()), record)
                            self.assertTrue(any(r["object"] == "crt0.obj" and r["classification"] == "opaque"
                                                for r in record["linked_objects"]))
                        message = {"probe": "macro probe failed", "source": "beyond patch 0001",
                                   "header": "external build inputs", "config": "external build inputs",
                                   "layout": "module-segment table", "audit": "crt0.obj"}[failure]
                        self.assertIn(message, record["error"])

    def test_comparison_requires_raw_bytes_roots_host_dates_patch_and_layout(self):
        parent = ROOT / "build/host/dos"; parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as name:
            roots = [Path(name) / "a", Path(name) / "longer-b"]
            records = []
            manifest, raw, _ = unified.reproducible_inputs()
            kernel = b'code\0FreeDOS kernel - GIT (build 2043 OEM:0xfd) [compiled May 14 2021]\n\0data'
            hashes = {n: unified.sha(b) for n, b in unified.patched_sources(unified.sources()).items()}
            for i, root in enumerate(roots):
                root.mkdir(); (root / "KERNEL.SYS").write_bytes(kernel)
                (root / "source/kernel").mkdir(parents=True)
                (root / "source/kernel/kernel.map").write_text(self.map_fixture)
                records.append(dict(schema=3, status="ok", root=str(root),
                    input_mtime=(946684800, 1609459200)[i], wall_date=f"2026-10-{21+i}T00:00:00+00:00",
                    unmodified=False, modification=unified.MODIFICATION, build_inputs=manifest,
                    build_manifest_sha256=unified.sha(raw), kernel_sha256=unified.sha(kernel), size=len(kernel),
                    source_hashes=hashes, pristine_source_hashes=unified.ledger("kernel"),
                    source_archive_sha256=unified.SOURCE_PINS["kernel"][2], config=unified.build_config(manifest),
                    date_header=unified.date_header(manifest),
                    kernel_provenance=unified.kernel_provenance(root / "source", kernel),
                    host={}, tools={}, support_files={}, linked_objects=[]))
            self.assertTrue(unified.compare(*records))
            self.assertEqual(records[0]["kernel_provenance"]["behavioural_equivalence"]["status"], "pending-lead-f3-48")
            for key, value in (("root", records[0]["root"]),
                               ("status", "blocked"), ("input_mtime", 946684801),
                               ("wall_date", records[0]["wall_date"]), ("date_header", "wrong"),
                               ("schema", 2), ("kernel_provenance", {}),
                               ("kernel_sha256", "bad"), ("source_hashes", {"bad": "source"}),
                               ("build_inputs", {}), ("build_manifest_sha256", "bad"), ("unmodified", True),
                               ("tools", {"wcc": "wrong"}), ("support_files", {"h/dos.h": "wrong"}),
                               ("linked_objects", ["crt0.obj"])):
                wrong = copy.deepcopy(records[1]); wrong[key] = value
                with self.subTest(key=key), self.assertRaises(ValueError): unified.compare_rebuilds(records[0], wrong)
            (roots[1] / "KERNEL.SYS").write_bytes(b"different")
            with self.assertRaises(ValueError): unified.compare_rebuilds(*records)

    def test_kernel_layout_and_packed_reference_provenance(self):
        parent = ROOT / "build/host/dos"; parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as name:
            work = Path(name); (work / "kernel").mkdir()
            mapping = work / "kernel/kernel.map"
            mapping.write_text(self.map_fixture)
            kernel = b"raw uncompressed kernel"
            record = unified.kernel_provenance(work, kernel)
            self.assertEqual(record["rebuilt"]["sha256"], unified.sha(kernel))
            self.assertEqual(record["rebuilt"]["size"], len(kernel))
            self.assertEqual(record["rebuilt"]["sections"], ["HMA_TEXT CODE DGROUP 0000:0020 00001000"])
            self.assertEqual(record["reference"]["size"], 46256)
            self.assertEqual(record["reference"]["sha256"],
                             "932c0c155701eddb7b902f7269a1b2ce31f5c82a6dc195172f2336d18a74e1fb")
            self.assertEqual(record["vendor_byte_comparison"]["status"], "replaced-by-amendment-3")
            self.assertEqual(record["behavioural_equivalence"]["status"], "pending-lead-f3-48")
            self.assertNotIn("pristine_comparison", record)
            for bad in ("no segment table", "Segment Class Group Address Size\n(no rows)\n"):
                mapping.write_text(bad)
                with self.assertRaisesRegex(ValueError, "section layout"):
                    unified.kernel_provenance(work, kernel)

    def link_map(self, work):
        # Wlink 1.9's actual forms: C absolute source, NASM THEADR name,
        # library THEADR member, and sources with segments but no publics.
        return ("Module: kernel.obj(kernel.asm)\n"
                f"Module: dosfns.obj({work}/kernel/dosfns.c)\n" +
                "".join(f"Module: ../lib/device.lib({n}.asm)\n"
                        for n in ("floppy", "rdpcclk", "wrpcclk", "wratclk")) +
                "Module          Segment               Class           Address         Size\n"
                "======          =======               =====           =======         ====\n"
                "apisupt.asm     PSP                   PSP            0000:0100        00000000\n"
                f"{work}/kernel/strings.c\n"
                "                HMA_TEXT              CODE           0209:9d99        00000000\n"
                "+--------------------+\nLibraries Used\n../lib/device.lib\n")

    def test_link_map_classifies_nasm_library_and_objects_without_publics(self):
        parent = ROOT / "build/host/dos"; parent.mkdir(parents=True, exist_ok=True)
        audits = []
        with tempfile.TemporaryDirectory(dir=parent) as name:
            for directory in ("a", "longer-root"):
                work = unified.extract(unified.patched_sources(unified.sources()), Path(name) / directory)
                (work / "lib/libm.lib").write_bytes(b"")
                (work / "kernel/kernel.map").write_text(self.link_map(work))
                rows = unified.link_audit(work); audits.append(rows)
                by_object = {r["object"]: r for r in rows}
                self.assertEqual(len(rows), 55)  # 51 direct objects + 4 library members
                self.assertEqual({r["classification"] for r in rows}, {"known"})
                self.assertEqual({r["license"] for r in rows}, {"GPL-2.0-or-later"})
                for obj, src, tool in (("kernel", "kernel/kernel.asm", "nasm 3.02"),
                                      ("dosfns", "kernel/dosfns.c", "Open Watcom 1.9 wcc"),
                                      ("floppy", "drivers/floppy.asm", "nasm 3.02")):
                    row = by_object[obj + ".obj"]
                    self.assertEqual(row["source"], src)
                    self.assertEqual(row["sha256"], unified.digest(work / src))
                    self.assertEqual(row["origin"]["tool"], tool)
                    self.assertEqual(row["map_visibility"], "public-symbols")
                self.assertIn("wlib", by_object["floppy.obj"]["origin"]["library"])
                self.assertEqual(by_object["apisupt.obj"]["map_visibility"], "module-segments")
                self.assertEqual(by_object["strings.obj"]["map_visibility"], "module-segments")
                self.assertEqual(by_object["iasmsupt.obj"]["source"], "kernel/asmsupt.asm")
                self.assertEqual(by_object["iasmsupt.obj"]["origin"]["rule"], "kernel/makefile:iasmsupt.obj")
                self.assertEqual(by_object["iprf.obj"]["origin"]["rule"], "kernel/makefile:iprf.obj")
            self.assertEqual(*audits)  # no absolute-root differences in closure

    def test_link_map_rejects_named_opaque_inputs_and_keeps_complete_diagnostics(self):
        parent = ROOT / "build/host/dos"; parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=parent) as name:
            work = unified.extract(unified.patched_sources(unified.sources()), Path(name) / "source")
            (work / "lib/libm.lib").write_bytes(b"")
            good = self.link_map(work)
            bad = ((good + "Module: crt0.obj(cstart.asm)\n", "crt0.obj"),
                   (good + "Module: ../lib/device.lib(opaque.asm)\n", "opaque.asm"),
                   (good + "clibm.lib\n", "clibm.lib"),
                   (good + "Module: /external/kernel.obj(kernel.asm)\n", "/external/kernel.obj"),
                   (good + "Module: /external/device.lib(floppy.asm)\n", "/external/device.lib"),
                   (good + "Module: kernel.obj(/external/kernel.asm)\n", "/external/kernel.asm"),
                   (good.replace("apisupt.asm     PSP", "unknown.asm     PSP"), "unknown.asm"),
                   (good.replace("apisupt.asm     PSP", "opaque_runtime     PSP"), "opaque_runtime"),
                   (good + "Module: malformed\n", "malformed"),
                   ("no modules", "no Wlink module records"))
            for text, reason in bad:
                with self.subTest(reason=reason):
                    (work / "kernel/kernel.map").write_text(text)
                    with self.assertRaises(unified.LinkAuditError) as caught:
                        unified.link_audit(work)
                    error = caught.exception
                    self.assertIn(reason, str(error))
                    self.assertIn("source=", str(error)); self.assertIn("origin=", str(error))
                    self.assertTrue(any(r["classification"] == "opaque" for r in error.rows))
                    self.assertTrue(any(r["object"] == "dosfns.obj" for r in error.rows))
            (work / "kernel/kernel.map").write_text(good)
            (work / "lib/libm.lib").write_bytes(b"opaque")
            with self.assertRaisesRegex(unified.LinkAuditError, "nonempty libm.lib"):
                unified.link_audit(work)
            (work / "lib/libm.lib").write_bytes(b"")
            for path in ("kernel/kernel.asm", "drivers/floppy.asm", "mkfiles/owlinux.mak"):
                original = (work / path).read_bytes()
                (work / path).write_bytes(original + b"\nchanged")
                with self.subTest(path=path), self.assertRaisesRegex(unified.LinkAuditError, "differs from pinned inputs"):
                    unified.link_audit(work)
                (work / path).write_bytes(original)


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
                             "ciukios/third_party/freedos/unified/ciuki/build.json",
                             "ciukios/third_party/freedos/unified/ciuki/patches/0001-reproducible-date.patch",
                             "ciukios/third_party/freedos/unified/ciuki/fork.json",
                             "ciukios/third_party/freedos/unified/ciuki/patches/0002-ciukidos-dfv2-binding.patch",
                             "ciukios/third_party/freedos/unified/ciuki/fork/drivers/ciukidf.asm",
                             "ciukios/scripts/unified_fork.py",
                             "ciukios/docs/licenses/CiukiDOS-unified-kernel-GRANT.md"):
                    self.assertIn(path, archive.namelist())
                manifest, raw, date_patch = unified.reproducible_inputs()
                prefix = "ciukios/third_party/freedos/unified/"
                self.assertEqual(archive.read(prefix + "ciuki/build.json"), raw)
                self.assertEqual(archive.read(prefix + manifest["patch"]["path"]), date_patch)
                self.assertEqual(archive.read("build-inputs/config.mak"), unified.build_config(manifest).encode())
                self.assertEqual(archive.read("build-inputs/ciuki-build-date.h"), unified.date_header(manifest).encode())
                self.assertNotIn("ciukios/config/upx.json", archive.namelist())

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
