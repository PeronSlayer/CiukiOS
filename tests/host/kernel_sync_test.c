/* T0: production F1 services with fake scheduler and PIC boundaries.
 * Build: scripts/test/host_kernel_tests.sh (ASan/UBSan).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <ciuki/kernel.h>
#include <ciuki/task.h>
#include <ciuki/sync.h>
#include <ciuki/work.h>
#include <ciuki/registry.h>
#include <ciuki/init.h>
#define SEL_KCODE 0x08

static unsigned failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* cpu.h contains privileged i386 inline assembly. Supply its boundary,
 * rather than a second implementation of any service being tested. */
#define CIUKI_CPU_H
static uint32_t flags = 0x200;
static unsigned writes, masks, unmasks, eois, starts, schedules, yields, creates;
static bool masked[16];
static irq_handler_t handlers[16];
static char last_log[256];
static bool expect_panic;
static jmp_buf panic_env;
static struct task tasks[4];
static void (*on_schedule)(void);
static void (*worker_entry)(void *);
static void *worker_arg;

struct task *g_current;
volatile uint64_t g_ticks;
volatile bool g_need_resched;
uint64_t g_tsc_per_ms;
bool g_cpu_tsc;
struct ciuki_boot_info g_boot;

static uint32_t read_eflags(void) { return flags; }
static uint32_t irq_save(void) { uint32_t f = flags; flags = 0; return f; }
static void irq_restore(uint32_t f) { flags = f; }
static void outb(uint16_t port, uint8_t v) { (void)port; (void)v; writes++; }
static void outl(uint16_t port, uint32_t v) { (void)port; (void)v; writes++; }
static uint32_t inl(uint16_t port) { (void)port; return UINT32_MAX; }
void *kzalloc(size_t n) { return calloc(1, n); }
void kfree(void *p) { free(p); }

void klog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
    va_end(ap);
}

__attribute__((noreturn)) void panic(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
    va_end(ap);
    if (expect_panic)
        longjmp(panic_env, 1);
    fprintf(stderr, "unexpected panic: %s\n", fmt);
    exit(2);
}

void task_start(struct task *t)
{
    CHECK(t->state == T_BLOCKED);
    t->state = T_READY;
    starts++;
}

void schedule(void)
{
    CHECK(!flags && g_current->state == T_BLOCKED);
    schedules++;
    if (on_schedule)
        on_schedule();
    else {
        g_ticks = g_current->wake_tick;
        g_current->state = T_READY;
    }
    CHECK(g_current->state == T_READY);
    g_current->state = T_RUNNING;
}

void task_yield(void) { CHECK(flags & 0x200); yields++; }

struct task *task_create_kernel(const char *name, void (*fn)(void *), void *arg, enum task_prio prio)
{
    CHECK(strcmp(name, "device-work") == 0 && prio == P_DEVICE);
    creates++;
    tasks[3] = (struct task){ .id = 4, .generation = 1, .state = T_BLOCKED, .prio = prio };
    worker_entry = fn;
    worker_arg = arg;
    return &tasks[3];
}

void pic_mask(unsigned irq) { CHECK(irq < 16); masked[irq] = true; masks++; writes++; }
void pic_unmask(unsigned irq) { CHECK(irq < 16); masked[irq] = false; unmasks++; writes++; }
void irq_set_handler(unsigned irq, irq_handler_t h) { CHECK(irq < 16); handlers[irq] = h; }

#include "../../src/kernel/core/sync.c"
#include "../../src/kernel/core/work.c"
#include "../../src/kernel/lib/stackprot.c"
#include "../../src/kernel/core/registry.c"

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap) { return vsnprintf(buf, size, fmt, ap); }
static unsigned probe_records;
void rec_emit(const char *probe, const char *event, const char *fmt, ...)
{
    char extra[512] = {0};
    va_list ap;
    va_start(ap, fmt);
    if (fmt) vsnprintf(extra, sizeof(extra), fmt, ap);
    va_end(ap);
    CHECK(55 + strlen(probe) + strlen(event) + strlen(extra) <= 240);
    if (55 + strlen(probe) + strlen(event) + strlen(extra) > 240) printf("oversized %s %s %s\n", probe, event, extra);
    if (!strcmp(event, "END")) CHECK(strstr(extra, "status=PASS"));
    probe_records++;
}
#include "../../src/kernel/probes/registry_probe.c"

static void current(unsigned n)
{
    g_current = &tasks[n];
    g_current->state = T_RUNNING;
    flags = 0x200;
}

static void drain_work(void)
{
    struct task *old = g_current;
    current(3);
    unsigned bound = 0;
    while (kwork_run_one())
        CHECK(++bound < 10000);
    g_current = old;
}

static bool idle_ok(int h, gen_t g)
{
    CHECK(flags & 0x200);
    CHECK(registry_get((unsigned)h)->state == RS_QUIESCING);
    CHECK(registry_release(h, g) == -EINVAL);  /* proof has not returned */
    CHECK(registry_activate(h, g) == -EINVAL);
    CHECK(registry_quiesce(h, g, idle_ok) == -EINVAL);
    return true;
}

static bool idle_bad(int h, gen_t g)
{
    CHECK(flags & 0x200);
    CHECK(registry_valid(h, g));
    return false;
}

static void release_active(int h)
{
    gen_t g = registry_get((unsigned)h)->generation;
    CHECK(registry_quiesce(h, g, idle_ok) == 0);
    CHECK(registry_release(h, g) == 0);
}

static void test_boot_reservations(void)
{
    g_boot.fb_phys = 0xE0000000u;
    g_boot.fb_pitch = 2560;
    g_boot.fb_height = 480;
    registry_init();
    unsigned input_ports = 0, framebuffers = 0;
    for (unsigned i = 0; i < registry_count(); i++) {
        const struct resource *r = registry_get(i);
        if (!strcmp(r->owner, "input") || !strcmp(r->owner, "boot-framebuffer")) {
            CHECK(r->state == RS_FIRMWARE);
            if (!strcmp(r->owner, "input")) input_ports++;
            else framebuffers++;
            gen_t old = r->generation;
            CHECK(!registry_claim_reserved((int)i, old, "boot-fixture"));
            CHECK(r->state == RS_CLAIMED && r->generation != old);
            CHECK(!registry_activate((int)i, r->generation));
            release_active((int)i);
        }
    }
    CHECK(input_ports == 2 && framebuffers == 1);
    memset(&g_boot, 0, sizeof(g_boot));
    printf("boot reservations: PASS (input/framebuffer firmware leases transfer with fresh generations)\n");
}

static void test_registry(void)
{
    registry_init();
    unsigned baseline = registry_count();
    struct registry_stats before, after;
    registry_snapshot(&before);
    CHECK(before.live == baseline && baseline >= 15);
    CHECK(registry_activate(-1, 1) == -EINVAL);
    CHECK(registry_claim(RES_MMIO, 10, 10, "bad", false) == -EINVAL);
    CHECK(registry_claim(RES_PORT, UINT32_MAX - 1, 3, "bad", false) == -EINVAL);
    CHECK(registry_claim(RES_IRQ, 16, 17, "bad", true) == -EINVAL);
    CHECK(registry_claim(RES_PORT, 0x500, 0x510, "bad", true) == -EINVAL);
    CHECK(registry_claim(RES_MMIO, 10, 20, 0, false) == -EINVAL);
    gen_t last = 0;
    for (unsigned i = 0; i < 100; i++) {
        unsigned w = writes;
        int h = registry_claim(RES_PORT, 0x500, 0x510, "driver-a", false);
        CHECK(h == (int)baseline);
        gen_t g = registry_get((unsigned)h)->generation;
        CHECK(g > last && !registry_valid(h, last) && registry_valid(h, g));
        CHECK(registry_release(h, g) == -EINVAL);
        CHECK(registry_quiesce(h, g, idle_ok) == -EINVAL);
        CHECK(registry_activate(h, g + 1) == -EINVAL);
        CHECK(registry_activate(h, g) == 0);
        CHECK(registry_activate(h, g) == -EINVAL);
        struct resource saved = *registry_get((unsigned)h);
        CHECK(registry_claim(RES_PORT, 0x508, 0x518, "driver-b", false) == -EINVAL);
        CHECK(memcmp(&saved, registry_get((unsigned)h), sizeof(saved)) == 0);
        CHECK(strstr(last_log, "driver-a") && strstr(last_log, "driver-b"));
        CHECK(registry_release(h, last) == -EINVAL);
        CHECK(registry_quiesce(h, last, idle_ok) == -EINVAL);
        CHECK(registry_quarantine(h, last) == -EINVAL);
        release_active(h);
        CHECK(!registry_valid(h, g));
        CHECK(registry_release(h, g) == -EINVAL);
        CHECK(registry_activate(h, g) == -EINVAL);
        CHECK(registry_quiesce(h, g, idle_ok) == -EINVAL);
        CHECK(writes == w);
        last = g;
    }
    registry_snapshot(&after);
    CHECK(after.live == before.live && after.claims - before.claims == 100);
    CHECK(after.releases - before.releases == 100 && after.conflicts - before.conflicts == 100);
    CHECK(after.quarantines == before.quarantines && registry_count() == baseline + 1);

    int h = registry_discover(RES_MMIO, 0x10000000, 0x10001000, "discovery", false);
    gen_t g = registry_get((unsigned)h)->generation;
    CHECK(registry_get((unsigned)h)->state == RS_DISCOVERED);
    CHECK(registry_release(h, g) == -EINVAL && registry_activate(h, g) == -EINVAL);
    CHECK(registry_quiesce(h, g, idle_ok) == -EINVAL);
    CHECK(registry_claim_reserved(h, g + 1, "display") == -EINVAL);
    CHECK(registry_claim_reserved(h, g, "display") == 0);
    CHECK(!registry_valid(h, g));
    g = registry_get((unsigned)h)->generation;
    CHECK(registry_claim_reserved(h, g, "other") == -EINVAL);
    CHECK(registry_activate(h, g) == 0);
    CHECK(registry_quiesce(h, g, 0) == -EINVAL);
    flags = 0;
    CHECK(registry_quiesce(h, g, idle_ok) == -EINVAL);
    flags = 0x200;
    unsigned w = writes;
    CHECK(registry_quiesce(h, g, idle_bad) == -EFAULT);
    struct resource retained = *registry_get((unsigned)h);
    CHECK(retained.state == RS_QUARANTINED && !registry_valid(h, g));
    CHECK(registry_release(h, g) == -EINVAL && registry_activate(h, g) == -EINVAL);
    CHECK(registry_claim_reserved(h, g, "other") == -EINVAL);
    CHECK(registry_quiesce(h, g, idle_ok) == -EINVAL);
    CHECK(registry_claim(RES_MMIO, retained.start, retained.end, "intruder", false) == -EINVAL);
    CHECK(memcmp(&retained, registry_get((unsigned)h), sizeof(retained)) == 0 && writes == w);
    registry_snapshot(&before);
    CHECK(registry_quarantine(h, g) == 0);
    registry_snapshot(&after);
    CHECK(before.quarantines == after.quarantines);

    /* Explicit firmware lease transfer changes the generation. */
    g = registry_get(0)->generation;
    CHECK(registry_get(0)->state == RS_FIRMWARE);
    CHECK(registry_activate(0, g) == -EINVAL && registry_release(0, g) == -EINVAL);
    CHECK(registry_quiesce(0, g, idle_ok) == -EINVAL);
    CHECK(registry_claim_reserved(0, g, "pic-service") == 0);
    CHECK(!registry_valid(0, g));
    CHECK(registry_activate(0, registry_get(0)->generation) == 0);
    printf("kernel services registry: PASS (100 cycles, stale/conflict/quarantine/lease)\n");
}

static void test_deadlines(void)
{
    g_ticks = UINT64_MAX - 2;
    uint64_t d = deadline_after_ms(5);
    CHECK(d == 2 && !deadline_passed(d));
    CHECK(deadline_passed(deadline_after_ms(0)));
    g_ticks = UINT64_MAX;
    CHECK(!deadline_passed(d));
    g_ticks = 0;
    CHECK(!deadline_passed(d) && deadline_passed(UINT64_MAX));
    g_ticks = 2;
    CHECK(deadline_passed(d));
    flags = 0;
    CHECK(deadline_after_ms(1) == 3 && !flags);
    CHECK(udelay(51) == -EINVAL);
    flags = 0x200;
    CHECK(udelay(1001) == -EINVAL && udelay(0) == 0);
    g_cpu_tsc = false;
    CHECK(udelay(1) == -ENOSYS);
    g_cpu_tsc = true;
    g_tsc_per_ms = 0;
    CHECK(udelay(1) == -ENOSYS);
    g_tsc_per_ms = 1;
    flags = 0;
    CHECK(udelay(50) == 0 && !flags);
    flags = 0x200;
    g_tsc_per_ms = 1ull << 40;     /* prevent host scheduling noise in IRQ budget tests */
    printf("kernel services deadlines: PASS (64-bit wrap, pacing bounds)\n");
}

static struct kwait test_wait;
static bool condition;
static struct kmutex mutexes[2];
static bool ready(void *arg) { CHECK(!flags); return *(bool *)arg; }

static void wake_irq(void)
{
    unsigned n = starts;
    condition = true;
    kwait_wake_all(&test_wait);
    kwait_wake_all(&test_wait);
    CHECK(starts == n + 1 && !flags && g_current->wake_tick == 0);
}

static unsigned wake_attempts;
static void spurious_wake(void)
{
    condition = ++wake_attempts == 2;
    kwait_wake_all(&test_wait);
}

static void timer_then_irq(void)
{
    g_ticks = g_current->wake_tick;
    g_current->wake_tick = 0;
    g_current->state = T_READY;
    unsigned n = starts;
    kwait_wake_all(&test_wait);
    CHECK(starts == n);                  /* timer already queued it */
    condition = true;                   /* condition wins at the deadline */
}

static void unlock_owner(void)
{
    struct task *sleeping = g_current;
    current(0);
    kmutex_unlock(&mutexes[0]);
    CHECK(sleeping->state == T_READY);
    g_current = sleeping;
    flags = 0;
}

#define PANICS(stmt) do { expect_panic = true; if (!setjmp(panic_env)) { stmt; CHECK(false); } \
    expect_panic = false; flags = 0x200; } while (0)

static void test_sync(void)
{
    current(0);
    kwait_init(&test_wait);
    condition = true;
    unsigned n = schedules;
    CHECK(kwait_wait_until(&test_wait, ready, &condition, deadline_after_ms(5)));
    CHECK(schedules == n);
    condition = false;
    on_schedule = wake_irq;
    CHECK(kwait_wait_until(&test_wait, ready, &condition, deadline_after_ms(5)));
    CHECK(schedules == n + 1 && !test_wait.head && !waiters);
    condition = false;
    on_schedule = spurious_wake;
    CHECK(kwait_wait_until(&test_wait, ready, &condition, deadline_after_ms(5)));
    CHECK(wake_attempts == 2);
    condition = false;
    on_schedule = timer_then_irq;
    CHECK(kwait_wait_until(&test_wait, ready, &condition, deadline_after_ms(5)));
    on_schedule = 0;
    condition = false;
    CHECK(!kwait_wait_until(&test_wait, ready, &condition, deadline_after_ms(5)));
    CHECK(!test_wait.head && !waiters && !g_current->wake_tick);
    kmutex_init(&mutexes[0]);
    kmutex_init(&mutexes[1]);
    kmutex_lock(&mutexes[0]);
    PANICS(kmutex_lock(&mutexes[0]));
    PANICS(ksync_task_exit(g_current));
    kmutex_lock(&mutexes[1]);
    PANICS(kmutex_unlock(&mutexes[0]));
    kmutex_unlock(&mutexes[1]);
    current(1);
    PANICS(kmutex_unlock(&mutexes[0]));
    on_schedule = unlock_owner;
    kmutex_lock(&mutexes[0]);
    CHECK(mutexes[0].owner == g_current);
    kmutex_unlock(&mutexes[0]);
    on_schedule = 0;
    flags = 0;
    PANICS(kmutex_lock(&mutexes[0]));
    CHECK(!held && !waiters);
    /* A terminating sleeper is unlinked before its stack/task is freed. */
    struct kwaiter w = { .task = &tasks[2], .queue = &test_wait };
    test_wait.head = waiters = &w;
    tasks[2].wake_tick = 99;
    ksync_task_exit(&tasks[2]);
    CHECK(!test_wait.head && !waiters && !tasks[2].wake_tick);
    printf("kernel services sync: PASS (fake schedule, IRQ wake, mutex/exit checks)\n");
}

static unsigned work_seen, work_expected;
static void count_work(void *arg)
{
    CHECK(flags & 0x200);
    CHECK((unsigned)(uintptr_t)arg == work_expected++);
    work_seen++;
}
static void tick_work(void *arg) { (void)arg; g_ticks++; }

static void test_work(void)
{
    current(0);
    CHECK(!kwork_queue(count_work, 0));
    CHECK(kwork_init() == 0 && kwork_init() == 0);
    CHECK(creates == 1 && worker_entry && !worker_arg);
    struct kwork_stats s;
    kwork_snapshot(&s);
    CHECK(s.lost == 1);
    for (unsigned round = 0; round < 3; round++) {
        flags = 0;                       /* enqueue from IRQ */
        for (unsigned i = 0; i < KWORK_CAPACITY; i++)
            CHECK(kwork_queue(count_work, (void *)(uintptr_t)(round * KWORK_CAPACITY + i)));
        CHECK(!kwork_queue(count_work, 0) && !flags);
        flags = 0x200;
        kwork_snapshot(&s);
        CHECK(s.pending == KWORK_CAPACITY && s.lost == round + 2);
        drain_work();
        CHECK(work_seen == (round + 1) * KWORK_CAPACITY);
        CHECK(strstr(last_log, "lost="));
    }
    unsigned n = yields;
    CHECK(kwork_queue(tick_work, 0));
    drain_work();
    CHECK(yields > n);
    kwork_snapshot(&s);
    CHECK(!s.pending && s.completed == 3 * KWORK_CAPACITY + 1 && s.queued == s.completed);
    printf("kernel services work: PASS (IRQ enqueue, FIFO wrap, counted overflow, yield)\n");
}

static unsigned calls_a, calls_b;
static bool source_a, source_b;
static bool irq_a(unsigned irq, const char *owner)
{
    CHECK(masked[irq] && strcmp(owner, "irq-a") == 0);
    calls_a++;
    return source_a;
}
static bool irq_b(unsigned irq, const char *owner)
{
    CHECK(masked[irq] && strcmp(owner, "irq-b") == 0);
    calls_b++;
    return source_b;
}

static void fake_irq(unsigned irq)
{
    CHECK(handlers[irq] && !masked[irq]);
    struct trap_frame tf = { .vector = 0x20 + irq };
    flags = 0;
    handlers[irq](&tf);
    CHECK(masked[irq]);                  /* no unmask before dispatcher EOI */
    eois++;
    flags = 0x200;
}

static int irq_claim(unsigned irq, const char *owner)
{
    int h = registry_claim(RES_IRQ, irq, irq + 1, owner, true);
    CHECK(h >= 0);
    CHECK(registry_activate(h, registry_get((unsigned)h)->generation) == 0);
    return h;
}

static void test_irq_chain(void)
{
    current(0);
    int a = irq_claim(11, "irq-a"), b = irq_claim(11, "irq-b");
    unsigned w = writes;
    CHECK(irq_chain_add(0, irq_a, "irq-a") == -EINVAL);
    CHECK(irq_chain_add(11, irq_a, "missing") == -EINVAL && writes == w);
    CHECK(irq_chain_add(11, irq_a, "irq-a") == 0);
    CHECK(irq_chain_add(11, irq_b, "irq-b") == 0);
    CHECK(irq_chain_add(11, irq_a, "irq-a") == -EINVAL);
    source_a = source_b = true;
    fake_irq(11);
    CHECK(calls_a == 1 && calls_b == 1 && eois == 1);
    drain_work();
    CHECK(!masked[11]);
    source_a = false;
    fake_irq(11);
    CHECK(calls_a == 2 && calls_b == 2);
    gen_t ag = registry_get((unsigned)a)->generation;
    CHECK(registry_quiesce(a, ag, idle_ok) == 0);
    CHECK(registry_release(a, ag) == -EINVAL); /* handler must be detached */
    drain_work();
    fake_irq(11);                         /* quiescing owner can still drain */
    CHECK(calls_a == 3 && calls_b == 3);
    CHECK(irq_chain_remove(11, irq_a, "irq-a") == 0); /* pending rearm */
    CHECK(registry_release(a, ag) == 0);
    drain_work();
    fake_irq(11);
    drain_work();
    CHECK(calls_a == 3 && calls_b == 4 && !masked[11]);
    source_b = false;
    struct irq_chain_stats s;
    for (unsigned i = 0; i < IRQ_CHAIN_STUCK_PASSES; i++) {
        fake_irq(11);
        drain_work();
        CHECK(irq_chain_snapshot(11, &s));
        CHECK(s.consecutive_unclaimed == i + 1);
        CHECK(s.quarantined == (i + 1 == IRQ_CHAIN_STUCK_PASSES));
    }
    CHECK(masked[11] && s.quarantines == 1 && s.participants == 1);
    CHECK(s.handled == 4 && s.unclaimed == 1000 && s.passes == 1004);
    CHECK(registry_get((unsigned)b)->state == RS_QUARANTINED);
    CHECK(registry_release(b, registry_get((unsigned)b)->generation) == -EINVAL);
    CHECK(registry_claim(RES_IRQ, 11, 12, "new-sharer", true) == -EINVAL);
    CHECK(irq_chain_add(11, irq_b, "irq-b") == -EINVAL);
    CHECK(irq_chain_remove(11, irq_b, "irq-b") == 0 && masked[11]);

    /* Remaining handlers run in the worker after the aggregate IRQ budget. */
    a = irq_claim(10, "irq-a");
    b = irq_claim(10, "irq-b");
    CHECK(irq_chain_add(10, irq_a, "irq-a") == 0);
    CHECK(irq_chain_add(10, irq_b, "irq-b") == 0);
    source_a = source_b = true;
    unsigned nb = calls_b;
    g_tsc_per_ms = 1;
    fake_irq(10);
    CHECK(calls_b == nb && masked[10]);
    g_tsc_per_ms = 1ull << 40;
    drain_work();
    CHECK(calls_b == nb + 1 && !masked[10]);
    CHECK(irq_chain_snapshot(10, &s) && s.budget_violations == 1);
    g_tsc_per_ms = 1;
    fake_irq(10);
    g_tsc_per_ms = 1ull << 40;
    CHECK(irq_chain_remove(10, irq_b, "irq-b") == 0); /* deferred callback not yet invoked */
    release_active(b);
    drain_work();
    CHECK(calls_b == nb + 1 && !masked[10]);
    CHECK(irq_chain_remove(10, irq_a, "irq-a") == 0);
    CHECK(irq_chain_remove(10, irq_b, "irq-b") == -EINVAL && masked[10]);
    release_active(a);

    /* A failed proof between IRQ service and deferred rearm must not let
     * the worker reopen the quarantined line or retain an active peer. */
    a = irq_claim(5, "irq-a");
    b = irq_claim(5, "irq-b");
    CHECK(irq_chain_add(5, irq_a, "irq-a") == 0);
    CHECK(irq_chain_add(5, irq_b, "irq-b") == 0);
    fake_irq(5);
    CHECK(registry_quiesce(a, registry_get((unsigned)a)->generation, idle_bad) == -EFAULT);
    drain_work();
    CHECK(irq_chain_snapshot(5, &s) && s.quarantined && masked[5]);
    CHECK(registry_get((unsigned)a)->state == RS_QUARANTINED);
    CHECK(registry_get((unsigned)b)->state == RS_QUARANTINED);

    /* No continuation slot: line and claims must remain quarantined. */
    a = irq_claim(9, "irq-a");
    CHECK(irq_chain_add(9, irq_a, "irq-a") == 0);
    for (unsigned i = 0; i < KWORK_CAPACITY; i++)
        CHECK(kwork_queue(tick_work, 0));
    fake_irq(9);
    CHECK(irq_chain_snapshot(9, &s) && s.quarantined && masked[9]);
    drain_work();
    CHECK(masked[9] && registry_get((unsigned)a)->state == RS_QUARANTINED);
    CHECK(masks >= eois && unmasks >= 1003);
    printf("kernel services IRQ: PASS (two owners, removal, 1000 passes, EOI, budget/overflow)\n");
}

static void test_stackprot(void)
{
    uintptr_t saved = __stack_chk_guard;
    CHECK(saved && seeded);
    g_ticks += 99;
    stackprot_init();
    CHECK(saved == __stack_chk_guard);
    strcpy(g_current->name, "guard-test");
    expect_panic = true;
    if (!setjmp(panic_env)) { __stack_chk_fail(); }
    expect_panic = false;
    CHECK(strstr(last_log, "task=guard-test"));
    printf("stack protector guard: PASS (nonzero, initialized once, task-name panic)\n");
}
static void test_registry_probe(void)
{
    struct registry_stats before, after;
    registry_snapshot(&before);
    g_cpu_tsc = true;
    g_tsc_per_ms = 1ull << 40;
    unsigned unmasked_before = unmasks;
    CHECK(!probe_registry());
    registry_snapshot(&after);
    CHECK(after.live == before.live + 1 && after.claims - before.claims == 102 &&
          after.releases - before.releases == 101 && after.quarantines - before.quarantines == 1);
    CHECK(unmasks == unmasked_before && probe_records >= 8 && !fixture_pic.writes);
    printf("registry probe: PASS (100 cycles, zero rejected writes, quarantine, production shadow IRQ/PIC)\n");
}

static void test_generation_exhaustion(void)
{
    CHECK(!gen_matches(0, 0));
    CHECK(!registry_valid(-1, 1) && !registry_valid(1000, 1));
    /* Last test: force the allocator's terminal state without billions of calls. */
    last_gen = UINT32_MAX - 1;
    CHECK(gen_alloc() == UINT32_MAX && gen_alloc() == 0 && gen_alloc() == 0);
    struct registry_stats before, after;
    registry_snapshot(&before);
    CHECK(registry_claim(RES_PORT, 0x600, 0x610, "exhausted", false) == -ENOSPC);
    registry_snapshot(&after);
    CHECK(before.live == after.live && before.claims == after.claims);
}

int main(void)
{
    for (unsigned i = 0; i < ARRAY_SIZE(tasks); i++)
        tasks[i] = (struct task){ .id = i + 1, .generation = 1, .state = T_RUNNING };
    current(0);
    test_boot_reservations();
    test_registry();
    test_stackprot();
    test_deadlines();
    test_sync();
    test_work();
    test_irq_chain();
    test_registry_probe();
    test_generation_exhaustion();
    printf("kernel services: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
