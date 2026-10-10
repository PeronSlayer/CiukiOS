"""f2-15: production probe records, capture framing and exact suite predicates."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import runpy
import signal
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts/test"))
import run as runner
from evidence import EvidenceError


class AppGateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch = ROOT / "build/host"
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix="app-gate-", dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        cls.env = dict(os.environ, TMPDIR=str(cls.work), ASAN_OPTIONS="detect_leaks=0")
        supervisor = (ROOT / "src/kernel/proc/supervisor.c").read_text()
        # Compile the actual observer and gate argv function, with fake launch
        # and scheduling boundaries. No evidence format is copied into Python.
        globals_ = supervisor[supervisor.index("static char observed_probe"):supervisor.index("void supervisor_capture_init")]
        captures = supervisor[supervisor.index("void supervisor_capture_init"):supervisor.index("void supervisor_set_io_ops")]
        first = supervisor.index("int supervisor_spawn_gate(")
        gate = supervisor[first:supervisor.index("\n}", first)+3]
        observer = supervisor[supervisor.index("void supervisor_observe("):]
        harness = cls.work / "probe.c"
        harness.write_text('#include <ciuki/kernel.h>\n#include <ciuki/supervisor.h>\n#include <ciuki/probe.h>\n'
                           + globals_ + captures + gate + observer
                           + f'\n#include "{ROOT / "src/kernel/probes/f2_probes_app.c"}"\n'
                           + f'\n#include "{ROOT / "tests/host/proc/app_gate_test.c"}"\n')
        cls.binary = cls.work / "probe"
        subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-DFS_HOST", "-D_POSIX_C_SOURCE=200809L", "-pthread",
                        "-I", str(ROOT / "src/kernel/include"), str(harness), str(ROOT / "src/kernel/lib/sha256.c"),
                        str(ROOT / "src/kernel/lib/fmt.c"), "-o", str(cls.binary)],
                       check=True, env=cls.env, capture_output=True, text=True)
        cls.stack_binary = cls.work / "probe-kernel-stack"
        # Resolve host libc symbols before entering the bounded stack: the
        # dynamic linker's vector-register save frame is not kernel work.
        subprocess.run(["clang", "-std=c17", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fno-omit-frame-pointer", "-DFS_HOST", "-D_GNU_SOURCE", "-DAPP_GATE_KERNEL_STACK_TEST", "-pthread",
                        "-Wl,-z,now",
                        "-I", str(ROOT / "src/kernel/include"), str(harness),
                        str(ROOT / "src/kernel/lib/sha256.c"), str(ROOT / "src/kernel/lib/fmt.c"),
                        "-o", str(cls.stack_binary)],
                       check=True, env=cls.env, capture_output=True, text=True)
        cls.kernel_build = runpy.run_path(str(ROOT / "scripts/build_kernel.py"))
        cls.stack_size = int(re.search(r"#define KSTACK_SIZE (\d+)u",
                             (ROOT / "src/kernel/include/ciuki/mm.h").read_text())[1])
        spec = importlib.util.spec_from_file_location("app_gate_image", ROOT / "scripts/build_image.py")
        cls.image = importlib.util.module_from_spec(spec); spec.loader.exec_module(cls.image)
        cls.suite = json.loads((ROOT / "tests/suites/f2-app.json").read_text())
        cls.expected = cls.suite["cases"][0]["expected"]
        pins = json.loads((ROOT / "config/sdk-pins.json").read_text())
        cls.sources = {}
        for name, data in (("/bin/lua", b"fake SDK ELF"),
                           ("/system/tests/lua-5.4.8-tests/all.lua", b"fake upstream tests"),
                           ("/system/tests/lua-5.4.8-tests/libs/P1/lib1.c", b"fake upstream library"),
                           ("/system/tests/ciuki-f2.lua", (ROOT / "apps/lua/ciuki-f2.lua").read_bytes()),
                           ("/system/tests/lua-5.4.8-tests/ciuki-f2.lua", (ROOT / "apps/lua/ciuki-f2.lua").read_bytes())):
            path = cls.work / name.lstrip("/"); path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data); cls.sources[name] = path
        cls.sdk_hash = hashlib.sha256(b"fake SDK manifest").hexdigest()
        cls.provenance = cls.image.application_provenance(cls.sources,
                            {"inputs":{"sdk_manifest_sha256":cls.sdk_hash,"archives":pins["lua"]}},
                            {"newlib":pins["newlib"]})
        cls.provenance_path = cls.work / "system/tests/app-gate.meta"
        cls.provenance_path.write_text(cls.provenance)
        cls.manifest_path = cls.work / "build-manifest.json"
        cls.manifest_path.write_text(json.dumps({"sdk_manifest_sha256":cls.sdk_hash,
                                                "payloads":cls.image.payload_records(cls.sources),
                                                "lua":{"archives":pins["lua"]}}))

    def output(self, mode=0):
        result = subprocess.run([str(self.binary), str(self.work), str(mode)], capture_output=True,
                                text=True, check=True, env=self.env)
        self.assertEqual(result.stderr, "")
        return result.stdout.splitlines()

    def parse(self, lines):
        parser = runner.Parser("12345678", "app-gate")
        for line in lines:
            self.assertLessEqual(len(line), 240)
            parser.feed(line.encode("ascii"))
        return parser

    def test_probe_on_guarded_kernel_size_stack(self):
        # The bounded binary uses production C without ASan stack inflation;
        # the normal harness retains ASan/UBSan for every controller case.
        for mode in (0, 3, 12, 16, 21, 28, 35):
            with self.subTest(mode=mode):
                result = subprocess.run([str(self.stack_binary), str(self.work), str(mode)],
                                        capture_output=True, text=True, env=self.env, timeout=20)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertRegex(result.stderr, rf"^app-gate stack: usable={self.stack_size} guard=\d+ high_water=\d+\n$")
                high_water = int(result.stderr.split("high_water=")[1])
                self.assertGreater(high_water, 0)
                self.assertLess(high_water, self.stack_size)
                parser = self.parse(result.stdout.splitlines())
                self.assertEqual(parser.outcome, "fail" if mode in (3, 35) else "pass")
                if mode == 0:
                    self.assertTrue(parser.check(self.expected))
                    launches = [r["run_case"] for r in parser.records if r.get("case") == "launch"]
                    self.assertEqual(launches, ["lua-basic", "lua-supplement"])

    def test_bounded_stack_overflow_hits_guard(self):
        result = subprocess.run([str(self.stack_binary), str(self.work), "20"],
                                capture_output=True, text=True, env=self.env, timeout=20)
        self.assertEqual(result.returncode, -signal.SIGSEGV, result.stderr)

    def test_kernel_frame_guard_rejects_large_frames(self):
        build = self.kernel_build
        for source, limit in ((build["SRC"] / "probes/f2_probes_app.c", build["APP_GATE_FRAME_LIMIT"]),
                              (build["SRC"] / "proc/supervisor.c", build["APP_GATE_FRAME_LIMIT"]),
                              (build["SRC"] / "core/output.c", build["KERNEL_FRAME_LIMIT"])):
            with self.subTest(source=source):
                fixture = self.work / "large-frame.c"
                fixture.write_text(f"unsigned large_frame(unsigned index) {{\n"
                                   f"  volatile unsigned char bytes[{limit + 512}];\n"
                                   "  for (unsigned i=0; i<sizeof(bytes); i++) bytes[i]=(unsigned char)i;\n"
                                   "  return bytes[index % sizeof(bytes)];\n}\n")
                result = subprocess.run(["clang", *build["CFLAGS"], *build["frame_flags"](source),
                                         "-c", str(fixture), "-o", str(self.work / "large-frame.o")],
                                        capture_output=True, text=True, env=self.env)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(f"exceeds limit ({limit})", result.stderr)
                self.assertIn("-Werror,-Wframe-larger-than", result.stderr)

    def test_production_records_satisfy_all_three_profiles(self):
        parser = self.parse(self.output())
        for case in self.suite["cases"]:
            self.assertTrue(parser.check(case["expected"]))
        result = {"probe":"app-gate", "outcome":"pass", "build_manifest":{"path":str(self.manifest_path)}}
        runner.record_f2_result(result, parser)
        self.assertEqual(result["missing_f2_fields"], [])
        self.assertEqual(len(result["application_output"]["guest_console_captures"]), 4)
        self.assertTrue(all(item["match"] for item in result["payload_hash_comparisons"]))
        self.assertEqual(result["argv"], ["lua", "-e", "_U=true", "all.lua"])
        self.assertEqual(result["env"], {"LC_ALL":"C", "TZ":"UTC0", "HOME":"/home",
                                      "TMPDIR":"/tmp", "PATH":"/bin"})
        self.assertEqual(result["declared_exclusions"]["complete"], "excluded_by_contract")

    def test_named_identity_growth_is_exactly_attributed(self):
        parser = self.parse(self.output(21))
        for case in self.suite["cases"]:
            self.assertTrue(parser.check(case["expected"]))
        resources = {k: v for r in parser.records if r.get("group") == "resources" for k, v in r.items()}
        self.assertEqual(resources["cache_nodes_before"], "59")
        self.assertEqual(resources["cache_nodes_after"], "60")
        self.assertEqual(resources["identity_bytes_delta"], "1024")
        self.assertEqual(resources["kernel_bytes_delta"], "1024")
        self.assertEqual(resources["kernel_bytes_remainder"], "0")
        self.assertEqual(resources["pages_remainder"], "0")
        self.assertEqual(resources["cache_accounted"], "1")
        metadata = runner.f2_metadata(parser.records)["resource_ledgers"]
        self.assertEqual(metadata["final"]["identities"], [60, 61440])
        self.assertEqual(metadata["final"]["storage"], [13440, 128])

    def test_observed_physical_growth_is_attributed_to_measured_heap_pool(self):
        parser = self.parse(self.output(28))
        resources = {k: v for r in parser.records if r.get("group") == "resources" for k, v in r.items()}
        self.assertEqual(parser.outcome, "pass")
        self.assertEqual(resources["pages_delta"], "32")
        self.assertEqual(resources["heap_pages_before"], "64")
        self.assertEqual(resources["heap_pages_after"], "96")
        self.assertEqual(resources["heap_pages_delta"], "32")
        self.assertEqual(resources["pages_remainder"], "0")
        self.assertEqual(resources["kernel_bytes_delta"], resources["identity_bytes_delta"])
        self.assertEqual(resources["kernel_bytes_remainder"], "0")
        self.assertEqual(resources["cache_accounted"], "1")
        self.assertEqual(resources["cache_bounded"], "1")
        self.assertEqual(resources["cache_pages_delta"], "0")
        for case in self.suite["cases"]:
            self.assertTrue(parser.check(case["expected"]))
        metadata = runner.f2_metadata(parser.records)["resource_ledgers"]
        self.assertEqual(metadata["baseline"]["heap"], [64, 1024, 4096])
        self.assertEqual(metadata["final"]["heap"], [96, 2048, 131072])

    def test_unattributed_pages_still_fail_with_or_without_heap_growth(self):
        for mode, remainder in ((35, 1), (36, 1), (37, -1)):
            with self.subTest(mode=mode):
                parser = self.parse(self.output(mode))
                resources = {k: v for r in parser.records if r.get("group") == "resources" for k, v in r.items()}
                self.assertEqual(parser.outcome, "fail")
                self.assertEqual(int(resources["pages_remainder"]), remainder)
                self.assertEqual(resources["kernel_bytes_remainder"], "0")
                self.assertEqual(resources["cache_accounted"], "0")
                with self.assertRaises(EvidenceError): parser.check(self.expected)

    def test_production_heap_ledger_tracks_refill_reuse_failure_and_peak(self):
        binary = self.work / "heap-ledger"
        result = subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-DAPP_GATE_KHEAP_LEDGER_TEST",
                        "-I", str(ROOT / "src/kernel/include"),
                        str(ROOT / "tests/host/proc/app_gate_test.c"), "-o", str(binary)],
                        capture_output=True, text=True, env=self.env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, env=self.env)
        self.assertEqual(result.stderr, "")
        self.assertEqual(result.stdout, "heap ledger: retained_pages=33 bytes_in_use=0 peak=131072 "
                                      "refill_failure=unchanged reuse=ok\n")

    def test_production_mount_namespace_teardown_releases_named_identities(self):
        binary = self.work / "namespace-teardown"
        result = subprocess.run(["clang", "-std=c17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                        "-DFS_HOST", "-D_POSIX_C_SOURCE=200809L", "-DAPP_GATE_NAMESPACE_TEARDOWN_TEST", "-pthread",
                        "-I", str(ROOT / "src/kernel/include"),
                        str(ROOT / "tests/host/proc/app_gate_test.c"),
                        str(ROOT / "src/kernel/fs/vfs.c"), "-o", str(binary)],
                        capture_output=True, text=True, env=self.env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, env=self.env)
        self.assertEqual(result.stderr, "")
        self.assertEqual(result.stdout, "namespace teardown: gate_nodes_before=5 gate_nodes_after=6 "
                                      "detached_nodes=6 freed_nodes=6 live_allocations=0\n")

    def test_gate_launch_environment_matches_metadata(self):
        source = (ROOT / "src/kernel/proc/supervisor.c").read_text()
        # Execute production argv/env construction up to the private ELF
        # preparation boundary; fake only file lookup and string storage.
        first = source.index("int supervisor_spawn(")
        spawn = source[first:source.index("    /* Prepare privately;", first)]
        first = source.index("int supervisor_spawn_gate(")
        gate = source[first:source.index("\n}", first)+3]
        harness = self.work / "environment.c"
        harness.write_text('''#include <ciuki/kernel.h>
#include <ciuki/supervisor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct process parent;
static struct proc_strings strings;
struct process *proc_supervisor(void) { return &parent; }
static int lookup(void *cwd, const char *path, struct ciuki_file *out) {
    (void)cwd; (void)out;
    if (strcmp(path, "/bin/lua")) abort();
    return 0;
}
const struct ciuki_file_ops *proc_get_file_ops(void) {
    static const struct ciuki_file_ops ops = { lookup }; return &ops;
}
struct proc_strings *proc_strings_new(void) { return &strings; }
int proc_strings_add(struct proc_strings *s, const char *text, uint32_t bytes, bool env) {
    if (s != &strings || bytes != strlen(text)+1) abort();
    printf("%s %s\\n", env ? "env" : "argv", text); return 0;
}
''' + spawn + '    (void)cwd; (void)desktop; (void)out; return err;\n}\n' + gate + '''
int main(void) {
    struct process *p = 0;
    if (supervisor_spawn_gate(false, &p)) return 1;
    puts("supplement");
    return supervisor_spawn_gate(true, &p);
}
''')
        binary = self.work / "environment"
        compiled = subprocess.run(["clang", "-std=c17", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-I", str(ROOT / "src/kernel/include"),
                        str(harness), "-o", str(binary)], env=self.env,
                       capture_output=True, text=True)
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        result = subprocess.run([str(binary)], check=True, env=self.env, capture_output=True, text=True)
        self.assertEqual(result.stderr, "")
        metadata = runner.f2_metadata(self.parse(self.output()).records)
        for lines, argv in zip(result.stdout.split("supplement\n"),
                               (["lua", "-e", "_U=true", "all.lua"], ["lua", "ciuki-f2.lua"])):
            self.assertEqual([s[5:] for s in lines.splitlines() if s.startswith("argv ")], argv)
            assignments = [s[4:].split("=", 1) for s in lines.splitlines() if s.startswith("env ")]
            self.assertEqual(len(assignments), 5)
            self.assertEqual(dict(assignments), metadata["env"])

    def test_each_real_record_predicate_rejects_missing_or_bad_fields(self):
        # Mutate records emitted by production C. No hand-built PASS fixtures.
        lines = self.output()
        parser = self.parse(lines)
        for predicate in self.expected["predicates"]:
            matches = [r for r in parser.records if all(r.get(k)==str(v) for k,v in predicate["where"].items())]
            self.assertTrue(matches, predicate)
            broken = self.parse(lines)
            broken.records = [r for r in broken.records if not all(r.get(k)==str(v) for k,v in predicate["where"].items())]
            with self.subTest(missing=predicate["where"]), self.assertRaises(EvidenceError):
                broken.check(self.expected)
            for field, rule in predicate.get("fields", {}).items():
                broken = self.parse(lines)
                record = next(r for r in broken.records if all(r.get(k)==str(v) for k,v in predicate["where"].items()) and field in r)
                if isinstance(rule, dict):
                    bound = rule.get("ge", rule.get("eq", rule.get("le")))
                    if isinstance(bound,str) and bound.startswith("$"):
                        bound = int(next(r[bound[1:]] for r in broken.records if all(r.get(k)==str(v) for k,v in predicate["where"].items()) and bound[1:] in r))
                    bad = bound-1 if "ge" in rule else bound+1
                    record[field] = f"{bad:08x}" if rule.get("encoding")=="hex" else str(bad)
                else: record[field] = "incorrect"
                with self.subTest(where=predicate["where"], field=field), self.assertRaises(EvidenceError):
                    broken.check(self.expected)
            for field in predicate.get("required_fields", []):
                broken = self.parse(lines)
                for record in broken.records:
                    if all(record.get(k) == str(v) for k, v in predicate["where"].items()):
                        record.pop(field, None)
                with self.subTest(missing_field=field), self.assertRaises(EvidenceError):
                    broken.check(self.expected)
            for relation in predicate.get("relations", []):
                broken = self.parse(lines)
                field = relation["left"]
                record = next(r for r in broken.records if all(r.get(k)==str(v) for k,v in predicate["where"].items()) and field in r)
                bad = int(record[field]) + 1
                if relation["op"] == "ge":
                    right = next(r[relation["right"]] for r in broken.records
                                 if all(r.get(k)==str(v) for k,v in predicate["where"].items()) and relation["right"] in r)
                    bad = int(right) - 1
                record[field] = str(bad)
                with self.subTest(relation=relation), self.assertRaises(EvidenceError):
                    broken.check(self.expected)

    def test_failed_applications_never_qualify(self):
        for mode in (1, 2, 3, 4, 5, 6, 8, 10, 11, 13, 14, 18, 19, 22, 23, 24, 25, 26, 27, 29, 30, 31, 32, 33, 34, 35, 36, 37):
            with self.subTest(mode=mode):
                parser = self.parse(self.output(mode))
                self.assertEqual(parser.outcome, "fail")
                with self.assertRaises(EvidenceError): parser.check(self.expected)
                if mode==3:
                    upstream = next(r for r in parser.records if r.get("case")=="lua-basic")
                    self.assertEqual(upstream["assertion_failures"], "1")
                    capture = parser.bounded_captures[(upstream["pid"], "stdout")]
                    self.assertTrue(capture["truncated"])
                    self.assertLess(capture["retained"], capture["bytes"])

    def test_foreign_output_ignored_and_safe_fallback_uses_ticks(self):
        self.assertTrue(self.parse(self.output(7)).check(self.expected))
        parser = self.parse(self.output(12))
        self.assertEqual(parser.outcome, "pass")
        desktop = next(r for r in parser.records if r.get("case")=="desktop")
        self.assertEqual(desktop["server"], "standin")
        self.assertEqual({r["metric"] for r in parser.records if r.get("group")=="survivor" and "metric" in r}, {"ticks"})
        # These three normal LFB profiles cannot qualify the fallback.
        with self.assertRaises(EvidenceError): parser.check(self.expected)

    def test_capture_offsets_digests_and_descriptors_fail_closed(self):
        lines = self.output()
        for kind in ("offset", "digest", "truncate", "missing"):
            bad = list(lines)
            if kind=="offset":
                index = next(i for i,s in enumerate(bad) if "group=app " in s and "stream=stderr" in s)
                bad[index] = bad[index].replace("offset=0", "offset=1")
            elif kind=="digest":
                index = next(i for i,s in enumerate(bad) if "group=capture_digest " in s)
                bad[index] = bad[index].split("sha256=")[0]+"sha256="+"0"*64
            elif kind=="truncate":
                index = next(i for i,s in enumerate(bad) if "group=capture " in s)
                bad[index] = bad[index].replace("truncated=0", "truncated=1")
            else:
                index = next(i for i,s in enumerate(bad) if "group=app " in s and "stream=stdout" in s)
                del bad[index]
            with self.subTest(kind=kind), self.assertRaises(EvidenceError): self.parse(bad)

    def test_maximum_width_record_formatting_and_truncated_frame_offsets(self):
        parser = self.parse(self.output(16))
        self.assertEqual(parser.outcome, "pass")
        metadata = runner.f2_metadata(parser.records)["resource_ledgers"]
        self.assertEqual(metadata["baseline"], metadata["final"])
        self.assertEqual(metadata["final"]["identities"], [2**32-1, 2**32-1])
        self.assertEqual(metadata["final"]["heap"], [2**32-1]*3)
        capture = parser.bounded_captures[(str(2**32-1), "stdout")]
        self.assertEqual(capture["bytes"], 3072)
        self.assertEqual(capture["retained"], 2048)
        self.assertTrue(capture["truncated"])

    def test_supplement_path_and_provenance_reject_changed_payloads(self):
        spawn = (ROOT / "src/kernel/proc/supervisor.c").read_text()
        self.assertIn('"lua", "ciuki-f2.lua"', spawn)
        self.assertIn('/system/tests/lua-5.4.8-tests', spawn)
        image = (ROOT / "scripts/build_image.py").read_text()
        self.assertIn('f"{TEST_PATH}/ciuki-f2.lua": LUA / "ciuki-f2.lua"', image)
        self.assertEqual(self.sources["/system/tests/ciuki-f2.lua"].read_bytes(),
                         self.sources["/system/tests/lua-5.4.8-tests/ciuki-f2.lua"].read_bytes())
        target = self.sources["/bin/lua"]; original = target.read_bytes()
        try:
            target.write_bytes(b"wrong ELF")
            self.assertEqual(self.parse(self.output()).outcome, "fail")
        finally: target.write_bytes(original)
        for bad in ("", self.provenance.replace("M sdk_manifest", "M bad_manifest", 1),
                    self.provenance+"P /bin/lua CIUKI_TEST\n", self.provenance.rstrip("\n")):
            try:
                self.provenance_path.write_text(bad)
                self.assertEqual(self.parse(self.output()).outcome, "fail")
            finally: self.provenance_path.write_text(self.provenance)

    def test_selectors_and_deadlines(self):
        self.assertEqual({c["profile"] for c in self.suite["cases"]}, {"qemu-t23", "qemu-e500", "qemu-min128"})
        for case in self.suite["cases"]:
            self.assertEqual(case["timeout"], 900)
            self.assertEqual(runner.selector(case["selector"].format(run_id="12345678"))["probe"], "app-gate")
            profile = runner.load(ROOT / "tests/profiles" / (case["profile"]+".json"))
            args, selector = runner.qemu_args("fake", profile, case, "12345678", self.work / "overlay", self.work / "bios")
            self.assertIn("shift=1,sleep=on", args)
            self.assertEqual(runner.selector(selector, "fw_cfg", True)["probe"], "app-gate")
        source = (ROOT / "src/kernel/probes/f2_probes_app.c").read_text()
        self.assertIn("GATE_BUDGET_MS 870000u", source)


if __name__ == "__main__": unittest.main()
