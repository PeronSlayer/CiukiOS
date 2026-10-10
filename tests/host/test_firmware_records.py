"""Validate f1-23 predicates with records emitted by the production input probe.

Research: Intel SDM Vol. 1, 20.5.2 specifies V86 I/O bitmap protection:
https://cdrdv2-public.intel.com/843827/253665-sdm-vol-1-dec-24.pdf
Decision: preserve device-firmware-ownership.md's monitor and existing synthetic
self-test. Only expose its report through the probe; never add live BIOS calls.
v86_test.c verifies the real self-test at fake CPU/port/scheduler boundaries.
"""
import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
from evidence import EvidenceError, Parser


class FirmwareRecordTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent = ROOT / 'build/host'
        parent.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='firmware-records-', dir=parent)
        cls.addClassCleanup(cls.temp.cleanup)
        scratch = Path(cls.temp.name)
        source = (ROOT / 'tests/host/i8042_test.c').read_text()
        # Same legacy adapter-boundary adaptation as host_kernel_tests.sh.
        old = '!strcmp(name, "firmware-queue") && fn && !arg && prio == P_DEVICE'
        assert source.count(old) == 1
        source = source.replace(old, old.replace('P_DEVICE', 'P_INTERACTIVE'))
        source = source.replace('"../../src/', '"' + str(ROOT / 'src') + '/')
        source += '''
bool fwinput_pending(void) { return fw_count || fake_fw_state == BIOSVM_DISABLED_BACKEND; }
uint64_t biosvm_input_reflections(void) { return 0; }
void biosvm_set_input_wait(struct kwait *q) { CHECK(q != 0); }
void biosvm_input_snapshot(struct biosvm_input_diag *out) { memset(out, 0, sizeof(*out)); }
'''
        fixture = scratch / 'probe.c'
        fixture.write_text(source)
        cls.program = scratch / 'probe'
        cls.env = {**os.environ, 'TMPDIR': str(scratch), 'ASAN_OPTIONS': 'detect_leaks=0'}
        result = subprocess.run([
            'clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', '-I', str(ROOT / 'src/kernel/include'),
            str(fixture), '-o', str(cls.program),
        ], env=cls.env, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        suite = json.loads((ROOT / 'tests/suites/f1-input.json').read_text())
        cls.cases = [c for c in suite['cases'] if
                     c['id'].startswith(('firmware_overrun', 'disallowed_io'))]

    def records(self, mode='firmware-records'):
        result = subprocess.run([str(self.program), mode], env=self.env,
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout.decode() + result.stderr.decode())
        self.assertEqual(result.stderr, b'')
        parser = Parser('12ab34cd', 'input-fault')
        for line in result.stdout.splitlines():
            if line.startswith(b'CIUKI_TEST '):
                self.assertLessEqual(len(line), 240)
                parser.feed(line)
        self.assertIsNotNone(parser.terminal)
        return parser

    def test_five_profiles_require_both_synthetic_subcases(self):
        profiles = {'qemu-min128', 'qemu-t23', 'qemu-e500',
                    'qemu-desktop-1998', 'qemu-desktop-2002'}
        for name in ('firmware_overrun', 'disallowed_io'):
            cases = [c for c in self.cases if c['id'].startswith(name)]
            self.assertEqual(len(cases), 5)
            self.assertEqual({c['profile'] for c in cases}, profiles)
            for case in cases:
                self.assertEqual(case['probe'], 'input-fault')
                self.assertIn(' platform=e500', case['selector'])
                self.assertNotIn('not_run_subcases', case['expected'])

    def test_actual_probe_records_and_every_predicate_violation(self):
        parser = self.records()
        for case in self.cases:
            expected = case['expected']
            with self.subTest(case=case['id']):
                self.assertTrue(parser.check(expected))
            for predicate in expected['predicates']:
                matches = [r for r in parser.records if all(
                    r.get(k) == str(v) for k, v in predicate['where'].items())]
                self.assertTrue(matches)
                for field in predicate.get('fields', {}):
                    broken = copy.copy(parser)
                    broken.records = copy.deepcopy(parser.records)
                    record = next(r for r in broken.records if field in r and all(
                        r.get(k) == str(v) for k, v in predicate['where'].items()))
                    record[field] = 'invalid'
                    with self.subTest(case=case['id'], field=field), self.assertRaises(EvidenceError):
                        broken.check(expected)
                broken = copy.copy(parser)
                broken.records = copy.deepcopy(parser.records)
                broken.records = [r for r in broken.records if r not in matches]
                with self.subTest(case=case['id'], missing=predicate['where']), self.assertRaises(EvidenceError):
                    broken.check(expected)

    def test_changed_controller_state_and_zero_survivor_progress_fail(self):
        parser = self.records()
        for case in self.cases:
            fields = ('survivor_ticks', 'survivor_progress', 'survivor_ok')
            if case['id'].startswith('disallowed_io'):
                fields += ('pic_after', 'pit_after')
            name = case['id'].split('-qemu', 1)[0]
            for field in fields:
                broken = copy.copy(parser)
                broken.records = copy.deepcopy(parser.records)
                record = next(r for r in broken.records if r.get('case') == name and field in r)
                record[field] = {'pic_after': '0000', 'pit_after': '00'}.get(field, '0')
                with self.subTest(case=case['id'], field=field), self.assertRaises(EvidenceError):
                    broken.check(case['expected'])

    def test_absent_backend_is_explicit_and_cannot_pass_firmware_cases(self):
        parser = self.records('firmware-absent')
        self.assertEqual(parser.terminal['status'], 'PASS')
        absent = [r for r in parser.records if r.get('reason') == 'firmware_backend_absent']
        self.assertEqual({r['case'] for r in absent}, {'firmware_overrun', 'disallowed_io'})
        self.assertEqual(len(absent), 2)
        self.assertTrue(all(r['status'] == 'not_run' for r in absent))
        for case in self.cases:
            with self.subTest(case=case['id']), self.assertRaises(EvidenceError):
                parser.check(case['expected'])

    def test_qemu_only_selftest_refusal_is_not_run_and_survivors_continue(self):
        parser = self.records('firmware-refused')
        self.assertEqual(parser.terminal['status'], 'PASS')
        refused = [r for r in parser.records
                   if r.get('reason') == 'firmware_selftest_qemu_only']
        self.assertEqual({r['case'] for r in refused}, {'firmware_overrun', 'disallowed_io'})
        self.assertEqual(len(refused), 2)
        self.assertTrue(all(r['status'] == 'not_run' for r in refused))
        survivors = [r for r in parser.records if 'survivor_progress' in r and
                     r.get('case') in {'firmware_overrun', 'disallowed_io'}]
        self.assertEqual({r['case'] for r in survivors},
                         {'firmware_overrun', 'disallowed_io'})
        self.assertTrue(all(int(r['survivor_progress']) > 0 and r['survivor_ok'] == '1'
                            for r in survivors))
        for case in self.cases:
            with self.subTest(case=case['id']), self.assertRaises(EvidenceError):
                parser.check(case['expected'])

    def test_selftest_failure_reaches_probe_verdict(self):
        parser = self.records('firmware-failed')
        self.assertEqual(parser.terminal['status'], 'FAIL')
        self.assertEqual(parser.terminal['reason'], 'fault_or_survivor')
        for case in self.cases:
            with self.subTest(case=case['id']), self.assertRaises(EvidenceError):
                parser.check(case['expected'])


if __name__ == '__main__':
    unittest.main()
