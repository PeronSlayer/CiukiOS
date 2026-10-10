"""Validate f1-23/f1-27 predicates with the production input probe's records.

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


# Keep the canonical QEMU stimulus predicates unchanged. These additional
# requirements consume the probe's real setup output, including boot replay,
# rather than the runner's older handwritten stimulus-only record fixtures.
INIT_STAGES = (
    ('flush', 0xa7), ('config_read', 0x20), ('config_write', 0x60),
    ('self_test', 0xaa), ('config_write', 0x60), ('iface_kbd', 0xab),
    ('iface_aux', 0xa9), ('enable', 0xa8), ('reset_kbd', 0x02),
    ('reset_aux', 0xe6), ('config_write', 0x60), ('enable', 0x20),
    ('reset_kbd', 0xf4), ('reset_aux', 0xf4), ('flush', 0x00),
)


def native_setup_predicates():
    predicates = [{
        'where': {'event': 'DATA', 'case': 'setup'},
        'exact_count': len(INIT_STAGES), 'unique': 'index',
        'required_fields': ['step', 'reply', 'first'],
        'fields': {
            **{field: {'encoding': 'hex', 'width': 2, 'ge': 0}
               for field in ('command', 'status_before', 'status_after', 'status_reply')},
            'index': {'ge': 1, 'le': len(INIT_STAGES)},
            'elapsed_ms': {'ge': 0, 'le': 500},
            'result': {'encoding': 'signed', 'eq': 0}, 'bytes': {'ge': 0},
        },
    }]
    for index, (step, command) in enumerate(INIT_STAGES, 1):
        fields = {'command': {'encoding': 'hex', 'width': 2, 'eq': command}}
        if step not in ('flush', 'enable') and index != 3:
            fields.update({
                'reply': {'encoding': 'hex', 'width': 2, 'ge': 0},
                'first': {'encoding': 'hex', 'width': 2, 'ge': 0}, 'bytes': {'ge': 1},
            })
        if step == 'self_test':
            fields['reply']['eq'] = 0x55
            fields['elapsed_ms'] = {'ge': 0, 'le': 200}
        if step == 'iface_kbd': fields['reply']['eq'] = 0
        if step in ('reset_kbd', 'reset_aux'): fields['reply']['eq'] = 0xfa
        predicates.append({
            'where': {'event': 'DATA', 'case': 'setup', 'step': step, 'index': str(index)},
            'exact_count': 1, 'fields': fields,
        })
    return predicates


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
        cls.input_cases = [c for c in suite['cases'] if c['probe'] == 'input' and
                           c['profile'] != 'qemu-e500']
        for case in cls.input_cases:
            case['expected']['predicates'].extend(native_setup_predicates())

    def records(self, mode='firmware-records'):
        result = subprocess.run([str(self.program), mode], env=self.env,
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout.decode() + result.stderr.decode())
        self.assertEqual(result.stderr, b'')
        parser = Parser('12ab34cd', 'input' if mode.startswith('input-') else 'input-fault')
        for line in result.stdout.splitlines():
            if line.startswith(b'CIUKI_TEST '):
                self.assertLessEqual(len(line), 240)
                parser.feed(line)
        self.assertIsNotNone(parser.terminal)
        return parser

    def test_native_setup_records_match_boot_logs_and_predicates(self):
        for mode in ('input-records', 'input-selftest-delayed', 'input-aux-quirk'):
            parser = self.records(mode)
            steps = [r for r in parser.records if r.get('case') == 'setup' and 'step' in r]
            self.assertEqual(len(steps), 15)
            self.assertEqual([int(r['index']) for r in steps], list(range(1, 16)))
            self.assertEqual({r['step'] for r in steps}, {
                'self_test', 'iface_kbd', 'iface_aux', 'config_read', 'config_write',
                'flush', 'enable', 'reset_kbd', 'reset_aux'})
            self.assertTrue(all(r['result'] == '0' for r in steps))
            result = subprocess.run([str(self.program), mode], env=self.env,
                                    capture_output=True, timeout=30)
            boot = [line.removeprefix(b'[i8042] ') for line in result.stdout.splitlines()
                    if line.startswith(b'[i8042] step=')]
            probe = [line.split(b'case=setup ', 1)[1] for line in result.stdout.splitlines()
                     if line.startswith(b'CIUKI_TEST ') and b'case=setup step=' in line]
            self.assertEqual(boot, probe)
            for case in self.input_cases:
                with self.subTest(mode=mode, case=case['id']):
                    self.assertTrue(parser.check(case['expected']))
            if mode == 'input-selftest-delayed':
                self.assertEqual(steps[3]['elapsed_ms'], '199')
                self.assertEqual(steps[3]['reply'], '55')
            if mode == 'input-aux-quirk':
                self.assertEqual((steps[6]['first'], steps[6]['reply'], steps[6]['bytes']),
                                 ('03', '5a', '2'))

    def test_each_native_setup_predicate_rejects_changed_or_missing_evidence(self):
        parser = self.records('input-records')
        for case in self.input_cases:
            expected = case['expected']
            for predicate in expected['predicates']:
                if predicate['where'].get('case') != 'setup':
                    continue
                matches = [r for r in parser.records if all(
                    r.get(k) == str(v) for k, v in predicate['where'].items())]
                self.assertTrue(matches)
                for field in predicate.get('fields', {}):
                    broken = copy.copy(parser)
                    broken.records = copy.deepcopy(parser.records)
                    record = next(r for r in broken.records if field in r and all(
                        r.get(k) == str(v) for k, v in predicate['where'].items()))
                    record[field] = 'invalid'
                    with self.subTest(case=case['id'], step=predicate['where'], field=field), \
                            self.assertRaises(EvidenceError):
                        broken.check(expected)
                for field in predicate.get('required_fields', []):
                    broken = copy.copy(parser)
                    broken.records = copy.deepcopy(parser.records)
                    record = next(r for r in broken.records if all(
                        r.get(k) == str(v) for k, v in predicate['where'].items()))
                    del record[field]
                    with self.subTest(case=case['id'], missing_field=field), \
                            self.assertRaises(EvidenceError):
                        broken.check(expected)
                for change in ('missing', 'duplicate'):
                    broken = copy.copy(parser)
                    broken.records = copy.deepcopy(parser.records)
                    if change == 'missing':
                        broken.records = [r for r in broken.records if r not in matches]
                    else:
                        broken.records.extend(copy.deepcopy(matches))
                    with self.subTest(case=case['id'], step=predicate['where'], change=change), \
                            self.assertRaises(EvidenceError):
                        broken.check(expected)

    def test_native_failure_records_name_the_original_quarantined_step(self):
        modes = {
            'input-translation': ('config_write', '-5', '60', None),
            'input-selftest-late': ('self_test', '-110', 'aa', 'none'),
            'input-aux-badloop': ('iface_aux', '-5', 'a9', '00'),
            'input-aux-missing': ('reset_aux', '-110', 'f5', 'none'),
            'input-stuck-obf': ('flush', '-110', 'ad', '00'),
            'input-final-obf': ('flush', '-110', '00', '00'),
            'input-enable-failed': ('enable', '-5', '20', None),
        }
        for mode, (step, error, command, reply) in modes.items():
            parser = self.records(mode)
            self.assertEqual(parser.terminal['status'], 'FAIL')
            self.assertEqual(parser.terminal['reason'], 'setup')
            steps = [r for r in parser.records if 'step' in r]
            self.assertTrue(all(r['result'] == '0' for r in steps[:-1]))
            failure = steps[-1]
            self.assertEqual((failure['step'], failure['result'], failure['command']),
                             (step, error, command))
            if reply is not None:
                self.assertEqual(failure['reply'], reply)
            summary = next(r for r in parser.records if 'failed_step' in r)
            self.assertEqual(summary['failed_step'], step)
            # Re-entry returns EIO; retained evidence keeps the original error.
            self.assertEqual(summary['error'], '-5')
            self.assertFalse(any(r['event'] == 'READY' for r in parser.records))
            for case in self.input_cases:
                with self.subTest(mode=mode, case=case['id']), self.assertRaises(EvidenceError):
                    parser.check(case['expected'])

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
