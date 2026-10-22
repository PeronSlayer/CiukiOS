"""f2-22/f2-23: production signal controller, timed waits and slow output.

Intel SDM Vol. 3A sections 2.5, 9.2.1 and Chapter 6's #DE/#NM/#MF/#AC:
https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-3a-part-1-manual.pdf
The control-register policy already matches these requirements. Exercise the
actual controller; fake only payload handshakes, memory, time and record output.
The NASM payload is still validated by the unchanged host script, not executed.
F2-23 replays production sched_tick() and the final-release signal boundary;
payload/syscall completion is modeled here, not claimed as guest execution.
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
#include <ciuki/cpustat.h>
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
static struct task subject_task;
static struct proc_thread subject_thread = { .tid = 4, .process = &subject, .task = &subject_task };
static unsigned payload_mode, signal_posts;
static int post_error;
static bool missing_thread, missing_task, preentry, release_done, ready_poll;
struct task *g_current;
static struct task *idle_task;
static bool device_cpu_refresh;
static uint32_t slice_left;
/* f3-14: the focused DOS window's tick budget lives in the scheduler block */
static const struct task *focus_task;
static uint32_t focus_ticks, ordinary_ticks, device_used;
static uint16_t device_history;
static struct sched_stats scheduler_stats;
#define DEVICE_BUDGET 4u
#define FOCUS_PERIOD 10u
#define FOCUS_BUDGET 8u
uint32_t g_starvation_boosts;
uint64_t g_idle_ticks;   /* f2-33: the extracted sched_tick counts idle ticks */
volatile bool g_need_resched;
static unsigned queued;
void file_clock_tick(void) { }
static bool rq_any(void) { return queued != 0; }
static void rq_push(struct task *t) { CHECK(t == &subject_task); queued++; }
static void rq_remove(struct task *t) { (void)t; CHECK(false); }
/* This fixture verifies signal expiry states; full priority behavior uses
 * production helpers in scheduler_priorities_test.c. */
static void priority_update(struct task *t) { (void)t; (void)rq_remove; }
static void device_cpu_advance(void) { (void)device_cpu_refresh; }
static bool device_throttled(void) { return device_used>=DEVICE_BUDGET; }
static bool device_urgent(const struct task *t) { (void)t;return false; }
/* f1-90 helpers of the extracted sched_tick; attribution itself is covered
 * by the production scheduler fixtures. */
static bool device_attribution;
static unsigned audio_over;
static void device_tick_sample(struct task *t) { (void)t; }
static bool device_demote(const struct task *t) { return device_throttled() && !device_urgent(t); }
static void device_overrun_record(struct task *t) { (void)t; scheduler_stats.budget_exhausted++; }
static void device_audio_poll(void) { }
/* f1-101 helpers of the extracted sched_tick; the producer boost is covered
 * by t23_audio_producer_test.c. */
static bool producer_over;
static bool producer_turn(const struct task *t) { (void)t; return false; }
static void producer_advance(void) { }

void task_start(struct task *t) { t->state=T_READY;rq_push(t); }
#include <ciuki/work.h>
static kwork_fn signal_work;
static void *signal_arg;
bool kwork_queue(kwork_fn fn,void *arg) { signal_work=fn;signal_arg=arg;return true; }
static uint32_t read_eflags(void) { return 0x200; }
static uint32_t irq_save(void) { return 0x200; }
static void irq_restore(uint32_t f) { (void)f; }
uint64_t deadline_after_ms(uint32_t ms) { return g_ticks+ms; }
TIMERS
SCHED_TICK
RELEASE_INJECTION

static void payload_step(void)
{
    if (subject.state != PROC_LIVE) return;
    struct signal_result *r = &result[0];
    if (r->stage == 100 && r->release) { subject.state = PROC_ZOMBIE; return; }
    if (payload_mode >= 15) {
        if (payload_mode == 15) {
            if (preentry && g_ticks >= 2) {
                subject_thread.in_syscall = true;
                subject_thread.operation = r;
            }
            if (ready_poll && !signal_posts) {
                /* Replay the production timer's expiry before each poll.
                 * The receive operation is still outstanding at READY. */
                subject_task.state = T_BLOCKED;
                subject_task.generation=1; ktimer_wait_task(&subject_task,g_ticks);
                sched_tick();
                for(unsigned n=0;signal_work && n<1024;n++) { kwork_fn fn=signal_work;signal_work=0;fn(signal_arg); }
                CHECK(subject_task.state == T_READY && !subject_task.wake_tick);
            }
            if (!signal_posts || post_error) return;
            r->sleep_result = -EINTR;
            r->saved_eax = r->resumed_eax = (uint32_t)-EINTR;
        } else {
            if (release_done) return;
            release_done = true;
            signal_release_drop(&release_objects[0].object);
            r->release_count = r->completion = r->handler_completion = 1;
            r->sleep_result = payload_mode == 16 ? 0 : 8;
            r->saved_eax = r->resumed_eax = r->sleep_result;
        }
        r->entries = r->returns = 1;
        r->stage = 100;
        subject_thread.in_syscall = false;
        subject_thread.operation = 0;
        return;
    }
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
    (void)p; CHECK(mode == payload_mode && !flags);
    if (mode >= 15) {
        result[0].tid = subject_thread.tid;
        result[0].stage = 2;
        subject_thread.in_syscall = !preentry;
        subject_thread.operation = preentry ? 0 : &result[0];
        release_objects[0] = (struct signal_release){
            .object = { signal_release_retain, signal_release_drop, true },
            .references = 1, .inject = true };
        if (ready_poll) payload_step();
        return &subject;
    }
    payload_step(); return &subject;
}
struct proc_thread *proc_thread_find(struct process *p, uint32_t tid)
{
    CHECK(p == &subject && tid == subject_thread.tid);
    return missing_thread ? 0 : &subject_thread;
}
struct proc_thread *proc_thread_for(const struct task *t)
{
    CHECK(t == &subject_task); return &subject_thread;
}
int proc_signal_thread_kill(struct process *p, uint32_t tid, uint32_t sig)
{
    CHECK(p == &subject && tid == subject_thread.tid && sig == SIGUSR1);
    CHECK(subject_thread.in_syscall && subject_thread.operation);
    signal_posts++;
    return post_error;
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
    if (!strcmp(argv[1], "predicate")) {
        struct signal_result r = { .stage = 2, .tid = 4 };
        subject_thread.in_syscall = true; subject_thread.operation = &r;
        for (unsigned state = T_READY; state <= T_DEAD; state++) {
            subject_task.state = state;
            CHECK(signal_channel_waiting(&r, &subject_thread) == (state == T_READY || state == T_BLOCKED));
        }
        subject_task.state = T_READY;
        CHECK(!signal_channel_waiting(&r, 0));
        subject_thread.task = 0;
        CHECK(!signal_channel_waiting(&r, &subject_thread));
        subject_thread.task = &subject_task;
        subject_thread.in_syscall = false;
        CHECK(!signal_channel_waiting(&r, &subject_thread));
        subject_thread.in_syscall = true; subject_thread.operation = 0;
        CHECK(!signal_channel_waiting(&r, &subject_thread));
        subject_thread.operation = &r;
        r.errors = 1; CHECK(!signal_channel_waiting(&r, &subject_thread)); r.errors = 0;
        r.tid = 0; CHECK(!signal_channel_waiting(&r, &subject_thread)); r.tid = 4;
        for (unsigned stage = 0; stage <= 100; stage++) {
            r.stage = stage;
            CHECK(signal_channel_waiting(&r, &subject_thread) == (stage == 2));
        }
        return 0;
    }
    if (!strncmp(argv[1], "channel-", 8) || !strncmp(argv[1], "close", 5) || !strncmp(argv[1], "dup2", 4)) {
        payload_mode = !strncmp(argv[1], "close", 5) ? 16 : !strncmp(argv[1], "dup2", 4) ? 17 : 15;
        missing_thread = !strcmp(argv[1], "channel-missing-thread");
        missing_task = !strcmp(argv[1], "channel-missing-task");
        preentry = !strcmp(argv[1], "channel-preentry");
        ready_poll = !strcmp(argv[1], "channel-ready");
        release_done = !strcmp(argv[1], "close-missing") || !strcmp(argv[1], "dup2-missing");
        if (missing_task) subject_thread.task = 0;
        subject_task.state = !strcmp(argv[1], "channel-running") ? T_RUNNING :
            !strcmp(argv[1], "channel-zombie") ? T_ZOMBIE :
            !strcmp(argv[1], "channel-dead") ? T_DEAD : T_BLOCKED;
        if (!strcmp(argv[1], "channel-errors")) result[0].errors = 1;
        if (strstr(argv[1], "failed-post")) post_error = -ESRCH;
        g_current = &subject_task;
        slow_ms = 100;                  /* records must not spend the wait deadline */
        rec_emit("signals-fault", "BEGIN", 0);
        const char *name = payload_mode == 15 ? "channel-eintr" : payload_mode == 16 ? "close-deferred" : "dup2-deferred";
        bool pass = signal_case(&parent, &survivor, &other, name, payload_mode, 0);
        if (ready_poll) CHECK(queued > 0);
        rec_emit("signals-fault", "END", pass ? "status=PASS" : "status=FAIL reason=signal_contract");
        return 0;
    }
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
    release = 'static struct signal_release' + source.split('static struct signal_release', 1)[1].split('static int signal_image_read', 1)[0]
    scheduler = (ROOT / 'src/kernel/core/task.c').read_text()
    vm_work = scheduler[scheduler.index('static void sched_vm_tick_work('):scheduler.index('void sched_tick(void)')]
    timers = (ROOT / 'src/kernel/core/sync.c').read_text()
    timers = timers[timers.index('/* F0-46 timers:'):timers.index('/* End F0-46 timers. */')]
    tick = 'void sched_tick(void)' + scheduler.split('void sched_tick(void)', 1)[1].split('struct task *sched_tasks(void)', 1)[0]
    # f3-00 added optional DOS VM hooks to the production scheduler as weak symbols.
    tick = ('extern void dosvm_tick(void) __attribute__((weak));\n'
            'extern void serial_tick(void) __attribute__((weak));\n' + vm_work + tick)
    return (HARNESS.replace('RESULT_LAYOUT', layout).replace('CONTROLLER', controller)
            .replace('RELEASE_INJECTION', release).replace('TIMERS', timers).replace('SCHED_TICK', tick))


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
        compiled = subprocess.run(['clang', '-std=c17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-I', str(ROOT / 'src/kernel/include'),
                        str(source), '-o', str(cls.program)], env=cls.env,
                       capture_output=True, text=True, timeout=60)
        if compiled.returncode:
            raise RuntimeError(compiled.stderr)
        suite = json.loads((ROOT / 'tests/suites/f2-runtime.json').read_text())
        cls.cases = [c for c in suite['cases'] if c['probe'] == 'signals-fault']

    def records(self, mode):
        result = subprocess.run([str(self.program), mode], env=self.env,
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
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

    def test_channel_predicate_checks_every_task_state_and_precondition(self):
        result = subprocess.run([str(self.program), 'predicate'], env=self.env,
                                check=True, capture_output=True, timeout=30)
        self.assertEqual(result.stdout, b'')
        self.assertEqual(result.stderr, b'')

    def test_channel_blocked_and_timer_ready_inject_with_final_eintr_context(self):
        for mode in ('channel-blocked', 'channel-ready', 'channel-preentry'):
            with self.subTest(mode=mode):
                parser = self.records(mode)
                self.assertEqual(parser.terminal['status'], 'PASS')
                parser.check({'terminal': 'END', 'predicates': [
                    {'where': {'case': 'channel-eintr', 'part': 'inject'}, 'exact_count': 1,
                     'fields': {'pid': {'eq': 5}, 'tid': {'eq': 4}, 'signal': {'eq': 10},
                                'mask': {'encoding': 'hex', 'eq': 0}, 'attempted': {'eq': 1},
                                'expected': {'encoding': 'signed', 'eq': 0},
                                'observed': {'encoding': 'signed', 'eq': 0}}},
                    {'where': {'case': 'channel-eintr', 'part': 'handlers'},
                     'exact_count': 1, 'fields': {'entries': {'eq': 1}, 'returns': {'eq': 1}}},
                    {'where': {'case': 'syscall-interruption', 'syscall': 'channel_recv'},
                     'exact_count': 1, 'fields': {'result': {'encoding': 'signed', 'eq': -4},
                        'saved_eax': {'encoding': 'hex', 'eq': 0xfffffffc},
                        'resumed_eax': {'encoding': 'hex', 'eq': 0xfffffffc}}},
                ]})
                self.assertFalse(any(r.get('part') == 'wait' for r in parser.records))

    def test_timeout_reports_last_state_without_claiming_an_injection(self):
        for mode, state in (('channel-running', '1'), ('channel-zombie', '3'),
                            ('channel-dead', '4'), ('channel-missing-thread', '-1'),
                            ('channel-missing-task', '-1'), ('channel-errors', '2')):
            with self.subTest(mode=mode):
                parser = self.records(mode)
                self.assertEqual(parser.terminal['status'], 'FAIL')
                wait = [r for r in parser.records if r.get('part') == 'wait']
                self.assertEqual(len(wait), 1)
                self.assertEqual((wait[0]['stage'], wait[0]['tid'], wait[0]['state'], wait[0]['ticks']),
                                 ('2', '4', state, '2000'))
                injection = next(r for r in parser.records if r.get('part') == 'inject')
                self.assertEqual((injection['tid'], injection['attempted'], injection['observed']),
                                 ('0', '0', '-3'))

    def test_failed_injection_retains_error_and_timeout(self):
        parser = self.records('channel-failed-post')
        self.assertEqual(parser.terminal['status'], 'FAIL')
        injection = next(r for r in parser.records if r.get('part') == 'inject')
        self.assertEqual((injection['tid'], injection['attempted'], injection['observed']), ('4', '1', '-3'))
        self.assertEqual(len([r for r in parser.records if r.get('part') == 'wait']), 1)

    def test_close_and_dup2_record_the_production_release_injection(self):
        for mode in ('close', 'dup2'):
            with self.subTest(mode=mode):
                parser = self.records(mode)
                self.assertEqual(parser.terminal['status'], 'PASS')
                injection = next(r for r in parser.records if r.get('part') == 'inject')
                self.assertEqual((injection['tid'], injection['signal'], injection['attempted'], injection['observed']),
                                 ('4', '10', '1', '0'))
                result = next(r for r in parser.records if r.get('syscall') == mode)
                self.assertEqual((result['result'], result['releases'], result['handler_completion']),
                                 ('0' if mode == 'close' else '8', '1', '1'))

    def test_close_and_dup2_missing_or_failed_injection_remains_a_failure(self):
        for mode in ('close', 'dup2'):
            with self.subTest(mode=mode):
                parser = self.records(mode + '-missing')
                self.assertEqual(parser.terminal['status'], 'FAIL')
                wait = [r for r in parser.records if r.get('part') == 'wait']
                self.assertEqual(len(wait), 1)
                self.assertEqual((wait[0]['stage'], wait[0]['tid'], wait[0]['state'], wait[0]['ticks']),
                                 ('2', '4', '2', '2000'))
                injection = next(r for r in parser.records if r.get('part') == 'inject')
                self.assertEqual((injection['attempted'], injection['observed']), ('0', '-3'))
                parser = self.records(mode + '-failed-post')
                self.assertEqual(parser.terminal['status'], 'FAIL')
                injection = next(r for r in parser.records if r.get('part') == 'inject')
                self.assertEqual((injection['tid'], injection['attempted'], injection['observed']), ('4', '1', '-3'))


if __name__ == '__main__':
    unittest.main()
