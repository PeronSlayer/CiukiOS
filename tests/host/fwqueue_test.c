/* T0 f1-18: fake-time queue scheduling and production firmware idle checks.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifdef FWQUEUE_FIRMWARE_TEST
/* Reuse the existing VM/physical-memory fakes without changing that suite. */
#define main v86_existing_main
#include "v86_test.c"
#undef main

int main(void)
{
    boot_fixture(true);
    CHECK(!fwinput_adapter_init());
    CHECK(adapter->prio == P_INTERACTIVE);
    unsigned before = enters;
    uint64_t tick = g_ticks;
    switch_fake(vm_thread);
    for (unsigned i = 0; i < 100; i++) {
        CHECK(!biosvm_keyboard_pending() && !biosvm_mouse_pending());
        service_input();
        g_ticks += 10;
    }
    CHECK(g_ticks - tick == 1000 && enters == before);
    switch_fake(&caller);
    CHECK(!fwinput_adapter_step());
    uint64_t reflected = biosvm_input_reflections();
    firmware_byte(1, 0x1E, 1);
    CHECK(biosvm_input_reflections() == reflected + 1 && adapter_ready(0));
    CHECK(fwinput_adapter_step() == 1 && !adapter_ready(0));
    firmware_byte(1, 0x9E, 1);
    CHECK(biosvm_input_reflections() == reflected + 2 && fwinput_adapter_step() == 1);
    firmware_byte(0x21, 0x29, 12);
    CHECK(biosvm_input_reflections() == reflected + 3);
    volatile struct biosvm_mouse_ring *ring = P2V(BIOSVM_SCRATCH + BIOSVM_MOUSE_RING);
    ring->packets[0][0] = 0x29; ring->packets[0][1] = 2; ring->packets[0][2] = 0xFF;
    ring->head = 1;
    CHECK(biosvm_mouse_pending());
    before = enters;
    switch_fake(vm_thread);
    service_input();
    CHECK(!biosvm_mouse_pending() && enters == before && decoder.stats.packets == 1);
    switch_fake(&caller);
    CHECK(fwinput_adapter_step() == 3);
    /* Full callback ring is drained in bounded steps without packet loss. */
    ring->head = ring->tail = 0;
    for (unsigned i = 0; i < BIOSVM_MOUSE_CAP - 1; i++) {
        ring->packets[i][0] = 8; ring->packets[i][1] = 2; ring->packets[i][2] = 1;
    }
    ring->head = BIOSVM_MOUSE_CAP - 1;
    unsigned steps = 0;
    while (biosvm_mouse_pending() && steps++ < BIOSVM_MOUSE_CAP) {
        switch_fake(vm_thread);
        service_input();
        CHECK(decoder.count <= 16);
        switch_fake(&caller);
        CHECK(fwinput_adapter_step() <= 16);
        g_ticks += 10;
    }
    CHECK(!biosvm_mouse_pending() && !decoder.count && !decoder.stats.loss);
    /* BDA check does not invoke INT16; malformed/lost ring state remains work. */
    putword(ram + 0x41C, 2, 0x20);
    before = enters;
    CHECK(biosvm_keyboard_pending() && enters == before);
    putword(ram + 0x41C, 2, 0x1E);
    ring->lost++;
    CHECK(biosvm_mouse_pending());
    switch_fake(vm_thread); service_input(); switch_fake(&caller);
    CHECK(!biosvm_mouse_pending() && fwinput_adapter_step() == 1);
    ring->head = BIOSVM_MOUSE_CAP;
    CHECK(biosvm_mouse_pending());
    switch_fake(vm_thread); service_input(); switch_fake(&caller);
    CHECK(!biosvm_mouse_pending() && fwinput_adapter_step() == 1);
    printf("firmware queue production: %s (idle=1000ms VM entries=0, IRQ1/12, BDA/ring, bounded mouse drain)\n",
           failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
#elif defined(FWQUEUE_SERVICE_TEST)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/biosvm.h>
#include <ciuki/fwinput.h>
#include <ciuki/timing.h>

static unsigned failures, vm_entries, status_calls, consume_calls, words;
static uint64_t now_us, reflected;
static uint32_t call_us;
bool g_cpu_tsc = true;
uint64_t g_tsc_per_ms = 1000;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); failures++; } } while (0)
#define CIUKI_CPU_H
uint64_t ktime_cycles(void) { return now_us; }
bool ktime_elapsed_us(uint64_t start, uint32_t us) { return now_us - start >= us; }
uint64_t deadline_after_ms(uint32_t ms) { return now_us / 1000 + ms; }
void klog(const char *fmt, ...) { (void)fmt; }
int biosvm_init(void) { return 0; }
enum biosvm_backend biosvm_backend_state(void) { return BIOSVM_READY; }
bool biosvm_keyboard_pending(void) { return words != 0; }
bool biosvm_mouse_pending(void) { return false; }
uint64_t biosvm_input_reflections(void) { return reflected; }
void biosvm_set_input_observer(void (*fn)(uint8_t, uint8_t, bool)) { CHECK(fn != 0); }
void biosvm_set_input_service(void (*fn)(void)) { CHECK(fn != 0); }
unsigned biosvm_mouse_packets(uint8_t (*out)[3], unsigned max, unsigned *lost)
{ (void)out; (void)max; *lost = 0; CHECK(false); return 0; }
int biosvm_call(struct biosvm_regs *r, uint32_t ms)
{
    vm_entries++;
    CHECK(ms && ms <= 500);
    if (r->interrupt == 0x15) { r->eax = r->flags = 0; return 0; }
    CHECK(r->interrupt == 0x16 && words);
    now_us += call_us;
    if (r->eax == 0x1100) { status_calls++; r->flags = 0; }
    else { CHECK(r->eax == 0x1000); consume_calls++; words--; r->eax = 0x1E61; }
    return 0;
}
#include "../../src/kernel/vm/fwinput.c"
int main(void)
{
    CHECK(!fwinput_init());
    unsigned setup_entries = vm_entries;
    for (unsigned i = 0; i < 100; i++) { service_input(); now_us += 10000; }
    CHECK(vm_entries == setup_entries && now_us == 1000000);
    /* Each BIOS call exceeds one step budget. Status survives the first
     * step, then consume succeeds on the next without reissuing status. */
    words = 1; call_us = 1500; reflected++;
    fwinput_decode_scan(&decoder, 0x1E, deadline_after_ms(0));
    service_input();
    CHECK(status_calls == 1 && !consume_calls && words == 1 && keyboard_ready);
    service_input();
    CHECK(status_calls == 1 && consume_calls == 1 && !words && !keyboard_ready);
    CHECK(decoder.stats.service_budget_violations == 2 && decoder.stats.max_service_us == 1500);
    struct fwinput_event events[16];
    CHECK(fwinput_poll(events, 16) == 2 && decoder.stats.text_matched == 1 && !decoder.stats.loss);
    service_input();
    CHECK(vm_entries == setup_entries + 2); /* no trailing empty AH=11 */
    /* A fast drain still stops at 16, preserving the remaining BIOS words. */
    words = 20; call_us = 0;
    service_input();
    CHECK(words == 4 && decoder.count == 16 && fwinput_poll(events, 16) == 16);
    service_input();
    CHECK(!words && fwinput_poll(events, 16) == 4 && !decoder.stats.loss);
    CHECK(status_calls == 21 && consume_calls == 21);
    printf("firmware queue service: %s (idle VM entries=0, split AH=11/10 progress, counted overruns, 16-event limit)\n",
           failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <ciuki/kernel.h>
#include <ciuki/task.h>
#include <ciuki/init.h>
#include <ciuki/input.h>
#include <ciuki/fwinput.h>
#include <ciuki/biosvm.h>

static unsigned failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); failures++; } } while (0)
#define CIUKI_CPU_H
static uint32_t fake_flags = 0x200;
static uint32_t read_eflags(void) { return fake_flags; }
struct ciuki_boot_info g_boot;
struct task *g_current;
bool g_cpu_tsc = true;
uint64_t g_tsc_per_ms = 1000;
static struct task queue_task, normal_task, caller;
static uint64_t now_us, reflections, irq_at, end_at, last_dispatch;
static unsigned pending, delivered, waits, event_cost, violations;
static uint64_t max_wake_us;
static struct kwait *reflection_wait;
static void (*queue_main)(void *);
static jmp_buf finished;
static enum biosvm_backend fake_backend = BIOSVM_READY;
static bool fake_disabled;

uint64_t ktime_cycles(void) { return now_us; }
bool ktime_elapsed_us(uint64_t start, uint32_t us) { return now_us - start >= us; }
uint64_t deadline_after_ms(uint32_t ms) { return now_us / 1000 + ms; }
bool deadline_passed(uint64_t d) { return deadline_after_ms(0) - d < (1ull << 63); }
void klog(const char *fmt, ...)
{ if (strstr(fmt, "budget exceeded")) violations++; }
gen_t gen_alloc(void) { return 1; }
struct task *task_create_kernel(const char *name, void (*fn)(void *), void *arg, enum task_prio prio)
{
    CHECK(!strcmp(name, "firmware-queue") && !arg && prio == P_INTERACTIVE);
    queue_main = fn;
    queue_task = (struct task){.state = T_BLOCKED, .prio = prio};
    return &queue_task;
}
void task_start(struct task *t) { CHECK(t->state == T_BLOCKED); t->state = T_READY; }
void task_kill(struct task *t, int code) { (void)code; t->state = T_ZOMBIE; }
void task_reap(struct task *t) { CHECK(t->state == T_ZOMBIE); }
int fwinput_init(void) { return 0; }
bool fwinput_pending(void) { return pending || (!fake_disabled && fake_backend == BIOSVM_DISABLED_BACKEND); }
unsigned fwinput_poll(struct fwinput_event *out, unsigned max)
{
    CHECK(out && max == 1);
    if (!pending) { fake_disabled = fake_backend == BIOSVM_DISABLED_BACKEND; return 0; }
    pending--;
    *out = (struct fwinput_event){FWINPUT_TEXT, 0, 'a', deadline_after_ms(0)};
    return 1;
}
void fwinput_stats(struct fwinput_stats *out) { memset(out, 0, sizeof(*out)); }
enum biosvm_backend biosvm_backend_state(void) { return fake_backend; }
uint64_t biosvm_input_reflections(void) { return reflections; }
void biosvm_set_input_wait(struct kwait *q) { reflection_wait = q; }
bool input_firmware_begin(gen_t gen) { CHECK(gen == 1); return true; }
void input_firmware_event(const struct fwinput_event *e, gen_t gen)
{
    CHECK(e->type == FWINPUT_TEXT && e->value == 'a' && gen == 1);
    delivered++;
    now_us += event_cost;
}
void input_firmware_loss(uint64_t lost) { CHECK(!lost); }
void input_firmware_disable(gen_t gen) { CHECK(gen == 1); }
void kwait_init(struct kwait *q) { q->head = 0; }
void kwait_wake_all(struct kwait *q)
{
    CHECK(q == reflection_wait && queue_task.state == T_BLOCKED);
    task_start(&queue_task);
}
/* Fixed-priority scheduler fake: only READY/RUNNING tasks are eligible,
 * highest priority first. A normal task must win while the queue blocks. */
static struct task *dispatch(void)
{
    struct task *next = &normal_task;
    if ((queue_task.state == T_READY || queue_task.state == T_RUNNING) &&
        queue_task.prio < next->prio) next = &queue_task;
    next->dispatches++;
    next->state = T_RUNNING;
    g_current = next;
    return next;
}
bool kwait_wait_until(struct kwait *q, kwait_cond_fn cond, void *arg, uint64_t d)
{
    CHECK(q == reflection_wait && g_current == &queue_task && d <= deadline_after_ms(10));
    CHECK(!cond(arg)); /* a leftover batch must block, rather than spin */
    waits++;
    CHECK(queue_task.dispatches > last_dispatch);
    last_dispatch = queue_task.dispatches;
    queue_task.state = T_BLOCKED;
    CHECK(dispatch() == &normal_task && normal_task.prio == P_NORMAL);
    uint64_t next = d * 1000;
    if (irq_at && irq_at < next) {
        now_us = irq_at;
        reflections++;
        pending = 20;
        kwait_wake_all(q);
        CHECK(cond(arg));
        uint64_t wake_us = now_us - irq_at;
        if (wake_us > max_wake_us) max_wake_us = wake_us;
        irq_at = 0;
    } else {
        now_us = next;
        task_start(&queue_task);
    }
    if (now_us >= end_at) longjmp(finished, 1);
    normal_task.state = T_READY;
    CHECK(dispatch() == &queue_task);
    return cond(arg);
}
#include "../../src/kernel/drivers/fwinput_adapter.c"

static void run_until(uint64_t end, uint64_t irq)
{
    end_at = end; irq_at = irq;
    queue_task.state = T_READY; normal_task.state = T_READY;
    last_dispatch = queue_task.dispatches;
    CHECK(dispatch() == &queue_task);
    if (!setjmp(finished)) queue_main(0);
}
int main(void)
{
    g_boot.input_policy = CBI_INPUT_FIRMWARE;
    g_current = &caller;
    normal_task = (struct task){.state = T_READY, .prio = P_NORMAL, .user = true};
    CHECK(!fwinput_adapter_init() && queue_main && reflection_wait);
    run_until(1000000, 0);
    CHECK(waits == 100 && normal_task.dispatches == 100 && !delivered);
    run_until(1030000, 1003000);
    CHECK(max_wake_us < 10000 && delivered == 20 && !pending);
    CHECK(normal_task.dispatches == waits); /* dispatched between every step */
    g_current = &queue_task;
    pending = 40;
    CHECK(fwinput_adapter_step() == 16 && pending == 24);
    event_cost = 600;
    CHECK(fwinput_adapter_step() == 2 && pending == 22 && budget_violations == 1 && violations == 1);
    /* PIT fallback also stops after expiry when TSC is unavailable. */
    g_cpu_tsc = false;
    event_cost = 2000;
    CHECK(fwinput_adapter_step() == 1 && pending == 21 && budget_violations == 2);
    fake_flags = 0;
    CHECK(!fwinput_adapter_step() && pending == 21);
    fake_flags = 0x200;
    g_current = 0;
    CHECK(!fwinput_adapter_step() && pending == 21);
    printf("firmware queue scheduler: %s (idle=1000ms, IRQ wake=%lluus, normal dispatches=%u, 16-event/1ms budgets)\n",
           failures ? "FAIL" : "PASS", (unsigned long long)max_wake_us, normal_task.dispatches);
    return failures ? 1 : 0;
}
#endif
