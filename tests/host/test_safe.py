"""F1-36: safe sweep absence and separately acquired menu evidence."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/test'))
from physical import FIELDS, import_sweep
from evidence import EvidenceError
from run import Parser

SUITE = json.loads((ROOT / 'tests/suites/f1-safe.json').read_text())


def records(probe, events, start=1):
    return ''.join(f'CIUKI_TEST v=1 run=12345678 seq={seq:06d} probe={probe} event={event}\r\n'
                   for seq, event in enumerate(events, start)).encode()


def safe_events(source='menu', selected=True, text=False, hardware=True):
    safe = int(selected)
    optional = 0 if selected else 2
    events = [
        'BEGIN',
        f'DATA case=option safe_mode={safe} fw_cfg={int(source == "fw-cfg")} boot_cfg={int(source == "boot-cfg")} menu={int(source == "menu")} menu_inferred={int(source == "menu")} boot_flags={safe:08x}',
        f'DATA case=storage mounts={int(not selected)} writes=0 gate={"closed" if selected else "mounted"}',
        f'DATA case=activation flag_sequence=1 framebuffer_sequence=2 input_sequence=3 ata_sequence=4 flag_before_activation=1 optional_activations={optional}',
    ]
    events.extend(f'DATA case=disabled device={device} reason=safe_mode'
                  for device in ('audio', 'acceleration', 'network', 'dma', 'power', 'optional_firmware', 'ata'))
    if text: events.append('DATA case=disabled device=framebuffer reason=no_lfb console=text')
    events.extend([
        f'DATA case=required console=1 console_mode={"text" if text else "lfb"} input=1 backend=native active_resources=3 disk_log=unavailable',
        f'DATA group=safe safe_mode={safe} flag_before_activation=1 optional_activations={optional} input_works=1 console_works=1 bios_retries=0 source={source}',
        f'DATA group=metadata subcase=required owner=i8042 generation=23 errors={int(not selected)} gate=safe timing_domain={"hardware" if hardware else "icount"}',
        f'READY console=1 input=1 optional_activations={optional}',
    ])
    if selected:
        events.append('END status=PASS reason=required_devices')
    else:
        events.extend(['DATA subcase=required status=not_run reason=safe_mode_not_selected',
                       'END status=NOT_RUN reason=safe_mode_not_selected'])
    return events


class SafeTests(unittest.TestCase):
    def setUp(self):
        folder = ROOT / 'build/host/safe'; folder.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=folder)
        self.capture = Path(self.temp.name)
        self.hash = hashlib.sha256(b'canonical').hexdigest()
        self.metadata = {k: 'unknown' for k in FIELDS}
        self.metadata.update(model='ThinkPad T23', operator_confirmed=True, build_id='12ab34cd',
                             write_sha256=self.hash, readback_sha256=self.hash,
                             selector='all:sweep run=12345678', selector_source='cfg')

    def tearDown(self):
        self.temp.cleanup()

    def acquire(self, raw, cases, physical_cases=SUITE['physical_cases']):
        (self.capture / 'f0.log').write_bytes(raw)
        (self.capture / 'acquisition.json').write_text(json.dumps(self.metadata))
        return import_sweep(self.capture, self.hash, cases, physical_cases)

    def banner(self, source='cfg'):
        return f'L:CPU signature=000006b1\r\nL:SELECT_SOURCE={source}\r\nCiuki VMM F0 build 12ab34cd - CiukiOS\r\n'.encode()

    def sweep_capture(self):
        events = safe_events(source='none', selected=False)
        events.append('SWEEP step=19 result=not_run reason=safe_mode_not_selected task=probes size=16384 high_water=6196')
        raw = self.banner() + records('sweep', ['BEGIN phase=3 step=19']) + records('safe', events, 2)
        raw += self.banner() + records('boot', ['BEGIN', 'END status=PASS'])
        raw += records('sweep', ['SWEEP_END passed=1 failed=0 not_run=1'], 3)
        return raw

    def test_sweep_safe_cases_are_not_run_without_blocking_later_evidence(self):
        cases = SUITE['cases'] + [{'id': 'later', 'probe': 'boot', 'expected': {'terminal': 'END'}}]
        results = self.acquire(self.sweep_capture(), cases)
        for case, result in zip(SUITE['cases'], results):
            with self.subTest(case=case['id']):
                self.assertEqual(result['outcome'], 'not_run')
                self.assertEqual(result['reason'], 'safe_mode_not_selected')
                self.assertEqual(result['expected'], case['expected'])
                self.assertFalse(result['operator_confirmation_required'])
                self.assertEqual(result['not_run_subcases'], [{'subcase': 'required', 'reason': 'safe_mode_not_selected'}])
                self.assertEqual(result['observed'][-1]['status'], 'NOT_RUN')
                self.assertEqual(result['observed'][-3]['event'], 'READY')
                self.assertEqual(result['sweep_records'][0]['reason'], 'safe_mode_not_selected')
                self.assertEqual(result['sweep_summary'][0]['not_run'], '1')
        self.assertEqual(results[-1]['outcome'], 'pass')

    def test_production_sweep_accounting_retains_reason_and_durable_totals(self):
        # Compile the production accounting block with host record/task seams;
        # the surrounding controller performs privileged reset and disk I/O.
        source = (ROOT / 'src/kernel/probes/probes.c').read_text()
        start = source.index('    const char *outcome = result < 0 ?')
        end = source.index('    e = sweep_checkpoint(storage, &cursor);', start)
        harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define KSTACK_SIZE 16384u
static struct { const char *name; } task = {"probes"}, *g_current = &task;
static unsigned task_stack_high_water(const void *t) { (void)t; return 6196; }
static char record[240];
static void rec_emit(const char *probe, const char *event, const char *fmt, ...)
{
    assert(!strcmp(event, "SWEEP"));
    int n = snprintf(record, sizeof(record), "CIUKI_TEST v=1 run=12345678 seq=000026 probe=%s event=%s ", probe, event);
    va_list ap; va_start(ap, fmt);
    n += vsnprintf(record + n, sizeof(record) - n, fmt, ap); va_end(ap);
    assert(n > 0 && n <= 240);
}
static void check(const char *name, int result, unsigned state, const char *expected)
{
    struct { const char *name; } step = {name};
    struct { unsigned state; } cursor = {0};
    unsigned current = 19, passed = 1, failed = 2, not_run = 3;
''' + source[start:end] + r'''
    assert(cursor.state == state && strstr(record, expected));
}
int main(void)
{
    check("safe", -ECANCELED, 1 | (2 << 6) | (4 << 12), "result=not_run reason=safe_mode_not_selected");
    check("input", -ECANCELED, 1 | (2 << 6) | (4 << 12), "result=not_run reason=operator_absent");
    check("safe", 1, 1 | (3 << 6) | (3 << 12), "result=fail task=");
    check("safe", 0, 2 | (2 << 6) | (3 << 12), "result=pass task=");
    puts("PASS sweep accounting: safe absence, operator absence, pass/fail, durable totals");
}
'''
        src = self.capture / 'accounting.c'; binary = self.capture / 'accounting'
        src.write_text(harness)
        subprocess.run(['clang', '-std=c17', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', str(src), '-o', str(binary)],
                       check=True, env={**os.environ, 'TMPDIR': str(self.capture)})
        subprocess.run([str(binary)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})

    def test_earlier_failed_prerequisite_still_applies(self):
        raw = self.banner() + records('boot', ['BEGIN', 'END status=FAIL']) + self.sweep_capture()
        cases = [{'id': 'earlier', 'probe': 'boot', 'expected': {'terminal': 'END'}}] + SUITE['cases']
        # Use one boot observation for the prerequisite.
        raw = raw[:raw.rfind(self.banner())]
        results = self.acquire(raw, cases)
        self.assertEqual(results[0]['outcome'], 'fail')
        self.assertEqual(results[1]['reason'], 'prerequisite failed: earlier')
        self.assertEqual(results[1]['evidence_reason'], 'safe_mode_not_selected')

    def test_invalid_absence_subcases_and_terminals_are_rejected(self):
        raw = self.sweep_capture()
        for before, after in (
                (b'subcase=required status=not_run', b'subcase=stimulus status=not_run'),
                (b'END status=NOT_RUN reason=safe_mode_not_selected', b'END status=PASS'),
                (b'DATA subcase=required status=not_run reason=safe_mode_not_selected', b'DATA case=other observed=0'),
                (b'safe_mode=0 fw_cfg=0', b'safe_mode=1 fw_cfg=0'),
                (b'source=none', b'source=boot-cfg')):
            with self.subTest(before=before), self.assertRaises(EvidenceError):
                self.acquire(raw.replace(before, after), SUITE['cases'])
        # Duplicate the required subcase, retaining increasing sequence numbers.
        events = safe_events(source='none', selected=False)
        events.insert(-1, events[-2])
        with self.assertRaisesRegex(EvidenceError, 'subcase'):
            self.acquire(self.banner() + records('safe', events), SUITE['cases'])
        self.metadata.update(selector='f1:safe run=12345678', selector_source='menu')
        with self.assertRaisesRegex(EvidenceError, 'subcase'):
            self.acquire(self.banner('menu') + records('safe', safe_events(source='none', selected=False)), SUITE['cases'])

    def test_separate_safe_menu_acquisition_passes_real_target_predicates(self):
        for target in SUITE['physical_cases']:
            with self.subTest(target=target['target']):
                self.metadata.update(model=target['target'], selector='f1:safe run=12345678', selector_source='menu',
                                     case_confirmations={target['id']: True})
                result = self.acquire(self.banner('menu') + records('safe', safe_events()), [target])[0]
                self.assertEqual(result['case'], target['id'])
                self.assertEqual(result['outcome'], 'pass')
                self.assertTrue(result['operator_confirmation_required'])
                self.assertTrue(result['operator_confirmation'])
                self.assertFalse(result['sweep_summary'])

    def test_menu_needs_its_own_acquisition_and_case_confirmation(self):
        target = SUITE['physical_cases'][0]
        result = self.acquire(self.sweep_capture(), [target])[0]
        self.assertEqual(result['outcome'], 'not_run')
        self.metadata.update(selector='f1:safe run=12345678', selector_source='menu')
        raw = self.banner('menu') + records('safe', safe_events())
        result = self.acquire(raw, [target])[0]
        self.assertEqual((result['outcome'], result['reason']), ('not_run', 'operator_absent'))
        self.metadata['case_confirmations'] = {target['id']: True}
        self.metadata['selector_source'] = 'cfg'
        result = self.acquire(raw, [target])[0]
        self.assertEqual(result['outcome'], 'not_run')
        self.metadata['selector_source'] = 'menu'
        for before, after in ((b'source=menu', b'source=boot-cfg'),
                              (b'optional_activations=0', b'optional_activations=2'),
                              (b'mounts=0', b'mounts=1'), (b'writes=0', b'writes=1'),
                              (b'console=1', b'console=0'), (b'input=1', b'input=0')):
            with self.subTest(before=before):
                result = self.acquire(raw.replace(before, after), [target])[0]
                self.assertEqual(result['outcome'], 'fail')

    def test_qemu_safe_cases_keep_pass_fail_and_reject_not_run_terminal(self):
        for case in SUITE['cases']:
            source = 'boot-cfg' if 'boot-cfg' in case['id'] else 'fw-cfg'
            raw = records('safe', safe_events(source=source, text=case['id'] == 'safe-no-lfb', hardware=False))
            with self.subTest(case=case['id']):
                parser = Parser('12345678', 'safe')
                for line in raw.splitlines(keepends=True): parser.feed(line)
                parser.check(case['expected'])
                parser = Parser('12345678', 'safe')
                for line in raw.replace(b'status=PASS', b'status=FAIL').splitlines(keepends=True): parser.feed(line)
                with self.assertRaises(EvidenceError): parser.check(case['expected'])
        parser = Parser('12345678', 'safe')
        with self.assertRaisesRegex(EvidenceError, 'PASS or FAIL'):
            for line in records('safe', safe_events(source='none', selected=False)).splitlines(keepends=True): parser.feed(line)


if __name__ == '__main__': unittest.main()
