"""f2-15: production probe records, capture framing and exact suite predicates."""
# SPDX-License-Identifier: MIT
import hashlib
import importlib.util
import json
import os
from pathlib import Path
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
                        "-fsanitize=address,undefined", "-I", str(ROOT / "src/kernel/include"),
                        str(harness), str(ROOT / "src/kernel/lib/sha256.c"), "-o", str(cls.binary)],
                       check=True, env=cls.env, capture_output=True, text=True)
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
        self.assertEqual(result["declared_exclusions"]["complete"], "excluded_by_contract")

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
                    bound = rule.get("ge",rule.get("eq"))
                    if isinstance(bound,str) and bound.startswith("$"):
                        bound = int(next(r[bound[1:]] for r in broken.records if all(r.get(k)==str(v) for k,v in predicate["where"].items()) and bound[1:] in r))
                    bad = bound-1 if "ge" in rule else bound+1
                    record[field] = f"{bad:08x}" if rule.get("encoding")=="hex" else str(bad)
                else: record[field] = "incorrect"
                with self.subTest(where=predicate["where"], field=field), self.assertRaises(EvidenceError):
                    broken.check(self.expected)

    def test_failed_applications_never_qualify(self):
        for mode in (1, 2, 3, 4, 5, 6, 8, 10, 11, 13, 14, 18, 19):
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
