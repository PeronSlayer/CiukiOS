"""f2-22: production fault controller with a slow output sink and partial delivery.

Intel SDM Vol. 3A sections 2.5, 9.2.1 and Chapter 6's #DE/#NM/#MF/#AC:
https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-3a-part-1-manual.pdf
The control-register policy already matches these requirements. Exercise the
actual controller; fake only payload handshakes, memory, time and record output.
The NASM payload is still validated by the unchanged host script, not executed.
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


HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#include <ciuki/kernel.h>
#include <ciuki/signal.h>
#include <ciuki/probe.h>
#define SIGNAL_RESULT (CIUKI_IMAGE_BASE + 4 * CIUKI_PAGE_SIZE)
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
RESULT_LAYOUT
static struct uaddr memory[2];
static struct process subject = { .pid = 5, .state = PROC_LIVE, .memory = &memory[0] };
static struct process survivor = { .pid = 3, .state = PROC_LIVE, .memory = &memory[1] };
static struct process parent, other = { .pid = 4 };
static struct signal_result result[2];
static unsigned next_index = 1, limit = 7, seq, slow_ms;
static bool fallback, duplicate, early_done;
volatile uint64_t g_ticks;

static void payload_step(void)
{
    if (subject.state != PROC_LIVE) return;
    struct signal_result *r = &result[0];
    if (r->stage == 100 && r->release) { subject.state = PROC_ZOMBIE; return; }
    if (r->stage) return;               /* handler awaits the production ACK */
    if (next_index > limit) {
        if (limit >= 7 || early_done) {
            r->entries = r->returns = 7;
            r->stage = 100;
        }
        return;
    }
    static const unsigned vectors[] = { 14, 14, 6, 0, 13, 17, 16 };
    static const unsigned signals[] = { SIGSEGV, SIGSEGV, SIGILL, SIGFPE, SIGSEGV, SIGBUS, SIGFPE };
    static const unsigned codes[] = { SEGV_MAPERR, SEGV_ACCERR, ILL_ILLOPC, FPE_INTDIV,
                                     SEGV_ACCERR, BUS_ADRALN, CIUKI_SI_X87 };
    unsigned index = duplicate && next_index == 6 ? 5 : next_index > 7 ? 7 : next_index;
    r->stage = 50 + index;
    r->entries = next_index; r->returns = next_index - 1; r->max_depth = 1;
    r->vector = r->expect_vector = fallback && index == 6 ? 14 : vectors[index - 1];
    r->signo = r->expect_signo = fallback && index == 6 ? SIGSEGV : signals[index - 1];
    r->code = r->expect_code = codes[index - 1];
    r->error = r->expect_error = index == 1 || (fallback && index == 6) ? 4 : index == 2 ? 7 : 0;
    r->eip = r->expect_eip = CIUKI_IMAGE_BASE + index;
    r->address = r->expect_address = index == 1 || (fallback && index == 6) ?
        CIUKI_MMAP_LIMIT - CIUKI_PAGE_SIZE : index == 2 ? CIUKI_IMAGE_BASE : index == 6 ? 0 : r->eip;
    r->tid = 4; r->fp_digest = 0xe8671897; r->ac_fallback = fallback;
    subject.fault_vector = r->vector;
}
void task_sleep_ms(uint32_t ms)
{
    g_ticks += ms; result[1].progress++;
    payload_step();
}
int ua_read(const struct uaddr *u, void *dst, uint32_t src, uint32_t bytes)
{
    unsigned which = u == &memory[0] ? 0 : 1;
    CHECK(src >= SIGNAL_RESULT && src - SIGNAL_RESULT + bytes <= sizeof(result[which]));
    memcpy(dst, (char *)&result[which] + src - SIGNAL_RESULT, bytes);
    return 0;
}
int ua_write(const struct uaddr *u, uint32_t dst, const void *src, uint32_t bytes)
{
    CHECK(u == &memory[0] && bytes == 4 && dst >= SIGNAL_RESULT);
    unsigned offset = dst - SIGNAL_RESULT;
    CHECK(offset + bytes <= sizeof(result[0]));
    memcpy((char *)&result[0] + offset, src, bytes);
    if (offset == offsetof(struct signal_result, stage)) next_index++;
    return 0;
}
static struct process *signal_payload(struct process *p, unsigned mode, uint32_t flags)
{
    (void)p; CHECK(!mode && !flags); payload_step(); return &subject;
}
struct proc_thread *proc_thread_find(struct process *p, uint32_t tid)
{
    (void)p; (void)tid; CHECK(false); return 0;
}
int proc_signal_thread_kill(struct process *p, uint32_t tid, uint32_t sig)
{
    (void)p; (void)tid; (void)sig; CHECK(false); return 0;
}
void proc_stop(struct process *p, int code, uint32_t sig)
{
    CHECK(p == &subject && code == 99 && !sig);
    p->status = code << 8; p->state = PROC_ZOMBIE;
}
int proc_reap(struct process *p, struct process *child)
{
    CHECK(p == &parent && child == &subject && child->state == PROC_ZOMBIE); return 0;
}
void rec_emit(const char *probe, const char *event, const char *fmt, ...)
{
    printf("CIUKI_TEST v=1 run=66666666 seq=%06u probe=%s event=%s", ++seq, probe, event);
    if (fmt) { putchar(' '); va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
    putchar('\n');
    g_ticks += slow_ms;                /* actual UART/LFB output consumes PIT time */
}
CONTROLLER
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    slow_ms = 100;
    fallback = !strcmp(argv[1], "tcg");
    duplicate = !strcmp(argv[1], "duplicate");
    early_done = !strcmp(argv[1], "early-done");
    if (!strcmp(argv[1], "partial") || early_done) limit = 5;
    if (!strcmp(argv[1], "overrun")) limit = 8;
    rec_emit("signals-fault", "BEGIN", 0);
    bool pass = signal_case(&parent, &survivor, &other, "fault-repair", 0, 0);
    rec_emit("signals-fault", "END", pass ? "status=PASS" : "status=FAIL reason=signal_contract");
    return 0;
}
'''


def controller_source(source):
    layout = 'struct signal_result {' + source.split('struct signal_result {', 1)[1].split('/* Fresh supervisor', 1)[0]
    controller = 'static bool signal_result_read' + source.split('static bool signal_result_read', 1)[1].split('#endif', 1)[0]
    return HARNESS.replace('RESULT_LAYOUT', layout).replace('CONTROLLER', controller)


class SignalFaultRecordTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch = ROOT / 'build/host'
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix='signal-fault-', dir=scratch)
        cls.addClassCleanup(cls.temp.cleanup)
        scratch = Path(cls.temp.name)
        cls.env = {**os.environ, 'TMPDIR': str(scratch), 'ASAN_OPTIONS': 'detect_leaks=0'}
        source = scratch / 'controller.c'
        source.write_text(controller_source((ROOT / 'src/kernel/probes/f2_probes_signals.c').read_text()))
        cls.program = scratch / 'controller'
        subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-I', str(ROOT / 'src/kernel/include'),
                        str(source), '-o', str(cls.program)], env=cls.env, check=True,
                       capture_output=True, text=True, timeout=60)
        suite = json.loads((ROOT / 'tests/suites/f2-runtime.json').read_text())
        cls.cases = [c for c in suite['cases'] if c['probe'] == 'signals-fault']

    def records(self, mode):
        result = subprocess.run([str(self.program), mode], env=self.env,
                                check=True, capture_output=True, timeout=30)
        self.assertEqual(result.stderr, b'')
        parser = Parser('66666666', 'signals-fault')
        for line in result.stdout.splitlines():
            self.assertLessEqual(len(line), 240)
            parser.feed(line)
        return parser

    def test_slow_output_preserves_seven_hardware_and_tcg_deliveries(self):
        for mode in ('hardware', 'tcg'):
            parser = self.records(mode)
            self.assertEqual(parser.terminal['status'], 'PASS')
            observed = [r for r in parser.records if r.get('part') == 'observed']
            self.assertEqual([int(r['index']) for r in observed], list(range(1, 8)))
            self.assertEqual(observed[5]['vector'], '17' if mode == 'hardware' else '14')
            completion = next(r for r in parser.records if r.get('part') == 'completion')
            self.assertEqual((completion['completed'], completion['zombie'], completion['records']), ('1', '1', '7'))
            for case in self.cases:
                expected = {'terminal': 'END', 'predicates': [p for p in case['expected']['predicates']
                            if p['where'].get('case') == 'fault-repair']}
                with self.subTest(mode=mode, case=case['id']):
                    self.assertTrue(parser.check(expected))

    def test_missing_duplicate_and_early_completion_fail(self):
        for mode in ('partial', 'duplicate', 'overrun', 'early-done'):
            parser = self.records(mode)
            self.assertEqual(parser.terminal['status'], 'FAIL')
            with self.assertRaises(EvidenceError):
                parser.check({'terminal': 'END'})
            if mode in ('partial', 'early-done'):
                observed = {int(r['index']) for r in parser.records if r.get('part') == 'observed'}
                self.assertEqual(set(range(1, 8)) - observed, {6, 7})
                # Neither a zero default status nor claimed final 7/7 counters
                # can replace absent individual delivery records.
                projected = copy.copy(parser)
                projected.terminal = {'event': 'END', 'status': 'PASS'}
                for case in self.cases:
                    predicates = [p for p in case['expected']['predicates']
                                  if p['where'].get('case') == 'fault-repair' and p['where'].get('part') == 'observed']
                    with self.assertRaises(EvidenceError):
                        projected.check({'terminal': 'END', 'predicates': predicates})
            elif mode == 'overrun':
                completion = next(r for r in parser.records if r.get('part') == 'completion')
                self.assertEqual(completion['records'], '7')  # bounded storage, failed order/count


if __name__ == '__main__':
    unittest.main()
