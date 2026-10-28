/* T0: production F1 services with fake scheduler and PIC boundaries.
 * Build: scripts/test/host_kernel_tests.sh (ASan/UBSan).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kheap_limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <ciuki/kernel.h>
#include <ciuki/task.h>
#include <ciuki/sync.h>
#include <ciuki/work.h>
#include <ciuki/registry.h>
#include <ciuki/vga_ports.h>
#include <ciuki/init.h>
#define SEL_KCODE 0x08

static unsigned failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* cpu.h contains privileged i386 inline assembly. Supply its boundary,
 * rather than a second implementation of any service being tested. */
#define CIUKI_CPU_H
static uint32_t flags = 0x200;
static void (*on_restore)(void);
static unsigned writes, masks, unmasks, eois, starts, schedules, yields, creates;
static bool masked[16];
static bool eoi_pending;
static irq_handler_t handlers[16];
static char last_log[256];
static unsigned budget_reports;
static unsigned detached_reports;
static char last_detached[256];
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
static void irq_restore(uint32_t f) { flags = f; if ((f & 0x200) && on_restore) on_restore(); }
static void outb(uint16_t port, uint8_t v) { (void)port; (void)v; writes++; }
static void outl(uint16_t port, uint32_t v) { (void)port; (void)v; writes++; }
static uint32_t inl(uint16_t port) { (void)port; return UINT32_MAX; }
static uint16_t cpu_inw(uint16_t port) { (void)port; return UINT16_MAX; }
void *(kzalloc)(size_t n) { if (n > KHEAP_MAX_REQUEST) return 0; return calloc(1, n); }
void kfree(void *p) { free(p); }

void klog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
    va_end(ap);
    if (strstr(last_log, "budget exceeded")) budget_reports++;
    if (strstr(last_log,"[irq] member_detached ")) {
        detached_reports++;strcpy(last_detached,last_log);
    }
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
    CHECK((flags & 0x200) && (g_current->state == T_BLOCKED || g_current->state == T_READY));
    uint32_t saved = irq_save(); /* model schedule's own short run-queue lock */
    schedules++;
    if (g_current->state == T_READY) { /* wake between IF restore and schedule */ }
    else if (on_schedule)
        on_schedule();
    else {
        g_ticks = g_current->wake_tick;
        g_current->state = T_READY;
    }
    CHECK(g_current->state == T_READY);
    g_current->state = T_RUNNING;
    irq_restore(saved);
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

void pic_mask(unsigned irq)
{ CHECK(irq < 16); if (!masked[irq]) { masked[irq] = true; masks++; writes++; } }
void pic_unmask(unsigned irq)
{
    CHECK(irq < 16 && !eoi_pending);
    if (irq >= 8) pic_unmask(2);
    if (masked[irq]) { masked[irq] = false; unmasks++; writes++; }
}
void irq_set_handler(unsigned irq, irq_handler_t h) { CHECK(irq < 16); handlers[irq] = h; }

#define P2V(p) ((void *)(uintptr_t)(p))
#define V2P(p) ((uint32_t)(uintptr_t)(p))
#include "../../src/kernel/fs/fs_port.h"
#include "scheduler_stubs.h"
#include "../../src/kernel/core/sync.c"
static bool irq_clock_model;
static uint64_t irq_test_cycles;
static unsigned irq_stall_read, irq_work_us;
static uint64_t read_tsc_serialized(void)
{
    if (!irq_clock_model) return ktime_cycles();
    if (irq_stall_read && !--irq_stall_read) irq_test_cycles += 2100;
    return irq_test_cycles;
}
#include "../../src/kernel/core/irq_budget.c"
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
    const uint16_t bases[] = { 0x3F8, 0x2F8, 0x3E8, 0x2E8 };
    for (unsigned b = 0; b < ARRAY_SIZE(bases); b++) {
        g_boot.uart_base = bases[b];
        registry_init();
        unsigned irq = b % 2 ? 3 : 4, found = 0;
        for (unsigned i = 0; i < registry_count(); i++) {
            const struct resource *r = registry_get(i);
            if (r->type == RES_IRQ && r->start == irq && !strcmp(r->owner, "uart")) {
                CHECK(!r->shareable && r->end == irq + 1);
                found++;
            }
        }
        CHECK(found == 1);
        CHECK(registry_claim(RES_IRQ, irq, irq + 1, "pci-intx-fixture", true) < 0);
    }
    memset(&g_boot, 0, sizeof(g_boot));
    printf("boot reservations: PASS (input/framebuffer firmware leases transfer with fresh generations)\n");
    puts("UART reservations: PASS (COM1/3 IRQ4, COM2/4 IRQ3, exclusive claims reject PCI INTx overlap)");
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

static void wake_in_unlocked_gap(void)
{
    if (g_current->state != T_BLOCKED) return;
    on_restore = 0;
    uint32_t f = irq_save();
    wake_irq();
    irq_restore(f);
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
    condition = false;
    on_restore = wake_in_unlocked_gap;
    unsigned before_gap = starts;
    CHECK(kwait_wait_until(&test_wait, ready, &condition, deadline_after_ms(5)));
    CHECK(!on_restore && starts == before_gap + 1 && !test_wait.head && !waiters && flags == 0x200);
    puts("kernel wait: IRQ wake between unlock and schedule, exactly one enqueue, IF enabled across sleep PASS");
    kmutex_init(&mutexes[0]);
    kmutex_init(&mutexes[1]);
    kmutex_lock(&mutexes[0]);
    PANICS(kmutex_lock(&mutexes[0]));
    /* Owner-death release is exercised in the production scheduler replay. */
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
    /* The void API reports a persistent cycle after one real safety wait;
     * callers using the timed API can recover instead (scheduler replay). */
    struct kwait dependency; kwait_init(&dependency);dependency.owner=&tasks[1];
    tasks[0].waiting_on=&dependency;kmutex_init(&mutexes[0]);
    mutexes[0].owner=&tasks[0];kwait_set_owner(&mutexes[0].wait,&tasks[0]);
    current(1);uint64_t cycle_started=g_ticks;
    PANICS(kmutex_lock(&mutexes[0]));
    CHECK(strstr(last_log,"cyclic wait timeout") && g_ticks-cycle_started==1000 && !waiters);
    kwait_set_owner(&mutexes[0].wait,0);mutexes[0].owner=0;tasks[0].waiting_on=0;
    CHECK(!owned_queues);
    printf("kernel services sync: PASS (fake schedule, IRQ wake, mutex/exit checks)\n");
}

/* fx-5: the T23 trial of image 9a60215f panicked `mutex: lock order` with
 * no lock named. The record now names both locks (class/address@site) and
 * the order key is (level, address). */
static void lock_at_site(struct kmutex *m) { kmutex_lock(m); }
static void test_lock_order_records(void)
{
    current(0);
    strcpy(tasks[0].name, "desktop");
    /* Two separate statics in the trial layout: the outer above the inner. */
    static struct { struct kmutex inner, outer; } trial;
    kmutex_init_named(&trial.inner, "audio.control");
    kmutex_init_named(&trial.outer, "audio.service");
    CHECK(trial.inner.init_site && trial.inner.init_site != trial.outer.init_site && !trial.inner.level);
    lock_at_site(&trial.outer);
    CHECK(trial.outer.site && trial.outer.owner == g_current);
    PANICS(kmutex_lock(&trial.inner));
    char want[64], held_text[64];
    snprintf(want, sizeof(want), "want=audio.control/%08lx@", (unsigned long)(uint32_t)(uintptr_t)&trial.inner);
    snprintf(held_text, sizeof(held_text), "held=audio.service/%08lx@%08lx", (unsigned long)(uint32_t)(uintptr_t)&trial.outer,
             (unsigned long)(uint32_t)trial.outer.site);
    CHECK(!strncmp(last_log, "mutex: lock order want=", 23) && strstr(last_log, want) && strstr(last_log, held_text));
    CHECK(strstr(last_log, " task=desktop") && strlen(last_log) < 160); /* panic()'s message buffer */
    CHECK(!trial.inner.owner && held == &trial.outer); /* refused before any state change */
    kmutex_unlock(&trial.outer);
    CHECK(!held && !trial.outer.site);
    /* An unnamed mutex is identified by its kmutex_init call site. */
    kmutex_init(&trial.inner);
    lock_at_site(&trial.outer);
    PANICS(kmutex_lock(&trial.inner));
    char init_class[32];
    snprintf(init_class, sizeof(init_class), "want=init@%08lx/", (unsigned long)(uint32_t)trial.inner.init_site);
    CHECK(strstr(last_log, init_class));
    kmutex_unlock(&trial.outer);
    /* Recursion names the same lock and where it was first taken. */
    lock_at_site(&trial.outer);
    snprintf(held_text, sizeof(held_text), "held=audio.service/%08lx@%08lx", (unsigned long)(uint32_t)(uintptr_t)&trial.outer,
             (unsigned long)(uint32_t)trial.outer.site);
    PANICS(kmutex_lock(&trial.outer));
    CHECK(!strncmp(last_log, "mutex: recursive acquisition want=audio.service/", 48) && strstr(last_log, held_text));
    kmutex_unlock(&trial.outer);
    /* Level orders classes regardless of address: an outer class above an
     * ordinary mutex nests it; the reverse panics even though it ascends. */
    kmutex_init_level(&trial.outer, "audio.service", KMUTEX_LEVEL_OUTER);
    kmutex_init_named(&trial.inner, "dma.register");
    kmutex_lock(&trial.outer); kmutex_lock(&trial.inner);
    CHECK(trial.inner.owner == g_current && held == &trial.inner);
    kmutex_unlock(&trial.inner); kmutex_unlock(&trial.outer);
    kmutex_lock(&trial.inner);
    PANICS(kmutex_lock(&trial.outer));
    CHECK(strstr(last_log, "lock order want=audio.service/") && strstr(last_log, "held=dma.register/"));
    kmutex_unlock(&trial.inner);
    /* The audio pair in its fixed struct order nests with no record. */
    static struct { struct kmutex service, control; } audio_pair;
    kmutex_init_level(&audio_pair.service, "audio.service", KMUTEX_LEVEL_OUTER);
    kmutex_init_level(&audio_pair.control, "audio.control", KMUTEX_LEVEL_OUTER);
    kmutex_lock(&audio_pair.service); kmutex_lock(&audio_pair.control); kmutex_lock(&trial.inner);
    PANICS(kmutex_unlock(&audio_pair.control));
    CHECK(strstr(last_log, "mutex: unlock order want=audio.control/") && strstr(last_log, "held=dma.register/"));
    kmutex_unlock(&trial.inner); kmutex_unlock(&audio_pair.control); kmutex_unlock(&audio_pair.service);
    CHECK(!held && !waiters);
    tasks[0].name[0] = 0;
    printf("kernel services lock order: PASS (record names both locks with class/address@site and task, init-site class, recursion, (level,address) key, unlock order)\n");
}

/* fx-5 follow-up, lead decision 2026-10-23: a normal boot logs an order
 * report once per class pair, counts it and continues; a test request and
 * hosted builds keep it fatal; recursion and unlock order stay fatal. */
static void test_lock_order_policy(void)
{
    current(0);
    strcpy(tasks[0].name, "desktop");
    CHECK(ksync_order_fatal); /* hosted default */
    ksync_order_fatal = false; g_boot.flags = 0;
    static struct { struct kmutex inner, outer; } pair;
    kmutex_init_named(&pair.inner, "audio.control");
    kmutex_init_named(&pair.outer, "audio.service");
    struct ksync_order_stats base, now;
    ksync_order_snapshot(&base);
    kmutex_lock(&pair.outer); kmutex_lock(&pair.inner); /* reported, then acquired */
    CHECK(pair.inner.owner == g_current && held == &pair.inner);
    CHECK(!strncmp(last_log, "[kernel] lock_order want=audio.control/", 39) && strstr(last_log, "held=audio.service/") &&
          strstr(last_log, " task=desktop") && strstr(last_log, "action=continue"));
    kmutex_unlock(&pair.inner); kmutex_unlock(&pair.outer);
    last_log[0] = 0;
    kmutex_lock(&pair.outer); kmutex_lock(&pair.inner); /* same pair: counted, no second record */
    CHECK(!last_log[0]);
    kmutex_unlock(&pair.inner); kmutex_unlock(&pair.outer);
    ksync_order_snapshot(&now);
    CHECK(now.violations == base.violations + 2 && now.pairs == base.pairs + 1 && now.dropped == base.dropped);
    /* Recursion and unlock order remain fatal in a normal boot. */
    kmutex_lock(&pair.outer);
    PANICS(kmutex_lock(&pair.outer));
    CHECK(strstr(last_log, "mutex: recursive acquisition"));
    kmutex_unlock(&pair.outer);
    kmutex_lock(&pair.inner); kmutex_lock(&pair.outer);
    PANICS(kmutex_unlock(&pair.inner));
    CHECK(strstr(last_log, "mutex: unlock order"));
    kmutex_unlock(&pair.outer); kmutex_unlock(&pair.inner);
    /* A boot carrying a test request keeps the order report fatal. */
    g_boot.flags = CBI_F_TEST_REQUEST;
    kmutex_lock(&pair.outer);
    PANICS(kmutex_lock(&pair.inner));
    CHECK(!strncmp(last_log, "mutex: lock order want=audio.control/", 37) && !pair.inner.owner);
    kmutex_unlock(&pair.outer);
    g_boot.flags = 0;
    /* The pair table is bounded: later new pairs are counted as dropped. */
    static struct kmutex classes[KSYNC_ORDER_PAIRS + 4];
    static char names[KSYNC_ORDER_PAIRS + 4][8];
    for (unsigned i = 0; i < KSYNC_ORDER_PAIRS + 4; i++) {
        snprintf(names[i], sizeof(names[i]), "c%02u", i);
        kmutex_init_level(&classes[i], names[i], KMUTEX_LEVEL_OUTER);
        kmutex_lock(&pair.outer); kmutex_lock(&classes[i]); /* outer level under level 0 */
        kmutex_unlock(&classes[i]); kmutex_unlock(&pair.outer);
    }
    ksync_order_snapshot(&now);
    CHECK(now.pairs == KSYNC_ORDER_PAIRS && now.dropped == base.dropped + (base.pairs + 1 + KSYNC_ORDER_PAIRS + 4 - KSYNC_ORDER_PAIRS));
    CHECK(now.violations == base.violations + 2 + KSYNC_ORDER_PAIRS + 4 && !held && !waiters);
    ksync_order_fatal = true;
    tasks[0].name[0] = 0;
    printf("kernel services lock order policy: PASS (normal boot logs once per class pair and continues, counters and bounded table, test request and hosted builds fatal, recursion and unlock order fatal)\n");
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
        CHECK(!kwork_queue(count_work, (void *)(uintptr_t)UINT32_MAX) && !flags);
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


extern void irq_model_phase(unsigned) __attribute__((weak));
static struct ktimer timer_entries[1000];
static uint32_t timer_generations[1000];
static unsigned timer_seen, timer_last;
static bool timer_rearm, timer_cancel_inflight;
static void timer_callback(void *arg, uint32_t generation, unsigned kind)
{
    unsigned i=(unsigned)(uintptr_t)arg;
    CHECK(flags & 0x200); CHECK(generation==timer_generations[i]);
    CHECK(kind==7);timer_seen++;timer_last=i;
    if(timer_cancel_inflight) { CHECK(!ktimer_cancel(&timer_entries[i])); timer_cancel_inflight=false; }
    if(timer_rearm) { timer_rearm=false;ktimer_arm(&timer_entries[i],g_ticks+1,7,arg,&timer_generations[i],timer_callback); }
}
static unsigned timer_tree_check(struct ktimer *t, struct ktimer *parent)
{
    if(!t)return 0;
    CHECK(t->parent==parent && t->state==KTIMER_ARMED);
    unsigned a=timer_tree_check(t->left,t),b=timer_tree_check(t->right,t);
    CHECK(a<=b+1 && b<=a+1 && t->height==1+(a>b?a:b));
    return t->height;
}
static void timer_drain(void)
{
    for(unsigned n=0;n<2000;n++) {
        ktimer_tick(g_ticks);drain_work();
        if(!timer_stats.armed && !timer_stats.due)break;
    }
}
static void test_timer_queue(void)
{
    current(0);g_ticks=0;timer_seen=0;
    for(unsigned n=0;n<2;n++) {
        unsigned count=n?1000:10;
        if(irq_model_phase)irq_model_phase(count);
        for(unsigned i=0;i<count;i++) {
            timer_generations[i]=gen_alloc();
            ktimer_arm(&timer_entries[i],100+(i*613)%count,7,(void *)(uintptr_t)i,&timer_generations[i],timer_callback);
            timer_tree_check(timer_root,0);
        }
        struct ktimer_stats a,b;ktimer_snapshot(&a);
        for(unsigned i=0;i<100;i++)ktimer_tick(0);
        ktimer_snapshot(&b);
        CHECK(b.comparisons==a.comparisons && b.armed==count && !b.due);
        /* Shuffled cancellation exercises both-child AVL deletion. */
        for(unsigned i=0;i<count;i+=3) {CHECK(ktimer_cancel(&timer_entries[i]));timer_tree_check(timer_root,0);}
        g_ticks=3000;timer_drain();timer_tree_check(timer_root,0);
        CHECK(!timer_root && !timer_first && !timer_stats.armed && !timer_stats.due);
        printf("f0-46 timers count=%u idle_ticks=100 insertion_comparisons=%llu irq_batch<=%u task_walk=0 PASS\n",count,(unsigned long long)a.comparisons,KTIMER_IRQ_BATCH);
        g_ticks=0;
        if(irq_model_phase)irq_model_phase(0);
    }
    unsigned before=timer_seen;
    timer_generations[0]=gen_alloc();g_ticks=UINT64_MAX-2;
    ktimer_arm(&timer_entries[0],0,7,0,&timer_generations[0],timer_callback);
    ktimer_tick(UINT64_MAX);drain_work();CHECK(timer_seen==before);
    g_ticks=0;timer_drain();CHECK(timer_seen==before+1 && !timer_last);
    /* Rearm a due entry: the queued old callback cannot consume new state. */
    ktimer_arm(&timer_entries[0],0,7,0,&timer_generations[0],timer_callback);
    ktimer_tick(0);CHECK(timer_entries[0].state==KTIMER_DUE);
    ktimer_arm(&timer_entries[0],5,7,0,&timer_generations[0],timer_callback);
    drain_work();CHECK(timer_seen==before+1);
    g_ticks=5;timer_rearm=true;ktimer_tick(g_ticks);drain_work();
    CHECK(timer_entries[0].state==KTIMER_ARMED && timer_seen==before+2);
    g_ticks=6;timer_drain();CHECK(timer_seen==before+3);
    /* Expiry/cancel interleaving, destruction/reuse, stale generation. */
    ktimer_arm(&timer_entries[0],6,7,0,&timer_generations[0],timer_callback);
    ktimer_tick(6);CHECK(ktimer_cancel(&timer_entries[0]));
    timer_generations[0]=gen_alloc();drain_work();CHECK(timer_seen==before+3);
    ktimer_arm(&timer_entries[0],7,7,0,&timer_generations[0],timer_callback);
    ktimer_tick(7);timer_generations[0]=gen_alloc();drain_work();
    CHECK(timer_seen==before+3 && timer_stats.stale);
    ktimer_arm(&timer_entries[0],7,7,0,&timer_generations[0],timer_callback);
    timer_cancel_inflight=true;g_ticks=7;ktimer_tick(7);drain_work();
    CHECK(!timer_cancel_inflight && timer_entries[0].state==KTIMER_CANCELLED && !timer_entries[0].executing);
    before++; /* cancellation in flight retains owner until callback completion */
    struct ktimer *retired=calloc(1,sizeof(*retired));CHECK(retired);
    ktimer_arm(retired,7,7,0,&timer_generations[0],timer_callback);
    ktimer_tick(7);CHECK(ktimer_cancel(retired));free(retired);drain_work();
    CHECK(timer_seen==before+3); /* ASan checks that no deferred pointer survives */
    /* A lost-tick/firmware pause is one absolute-time advance, no tick loop. */
    for(unsigned i=0;i<1000;i++) {
        timer_generations[i]=gen_alloc();
        ktimer_arm(&timer_entries[i],10+i,7,(void *)(uintptr_t)i,&timer_generations[i],timer_callback);
    }
    g_ticks=100000;ktimer_tick(g_ticks);CHECK(timer_stats.due==KTIMER_IRQ_BATCH);
    timer_drain();CHECK(timer_seen==before+1003 && timer_stats.max_irq_batch==KTIMER_IRQ_BATCH);
    CHECK(!timer_stats.armed && !timer_stats.due);
    puts("f0-46 timers: exact zero/wrap, due rearm, callback rearm, cancel/expiry, generation reuse, lost ticks/firmware pause, 1000 simultaneous deadlines PASS");
}
static void test_irq_notification(void)
{
    static struct kwait q;
    kwait_init(&q);
    bool done=true;
    struct kwaiter a={.task=&tasks[0],.queue=&q,.cond=ready,.arg=&done};
    struct kwaiter b={.task=&tasks[1],.queue=&q,.cond=ready,.arg=&done};
    a.next=a.all_next=&b;q.head=waiters=&a;
    tasks[0].sync_waiter=&a;tasks[1].sync_waiter=&b;
    tasks[0].state=tasks[1].state=T_BLOCKED;
    unsigned before=starts;flags=0;
    for(unsigned i=0;i<1000;i++)kwait_wake_irq(&q,true);
    CHECK(starts==before && q.notifications==1000 && q.notify_active);
    /* Cancel the first stack waiter before the deferred cursor reaches it. */
    ksync_task_exit(&tasks[0]);tasks[0].state=T_ZOMBIE;
    flags=0x200;drain_work();
    CHECK(starts==before+1 && tasks[1].state==T_READY && !q.notify_active);
    wait_unlink(&b);CHECK(!waiters && !q.head && !q.notify_cursor);
    current(0);
    puts("f0-46 IRQ notifications: 1000 captures do no predicate/donation/ready walk, one cancelled owner, one live completion, counted bounded continuations PASS");
}
/* f1-90: the audio refill's single wait is the interrupt's own deferred
 * work and is woken in the top half; any other waiter, an I/O wake or a
 * second waiter keeps the deferred, bounded notification path. */
static void test_audio_irq_wake(void)
{
    static struct kwait q;
    bool done=true;
    for (unsigned mode=0;mode<4;mode++) {
        kwait_init(&q);
        struct kwaiter a={.task=&tasks[0],.queue=&q,.cond=ready,.arg=&done};
        struct kwaiter b={.task=&tasks[1],.queue=&q,.cond=ready,.arg=&done};
        q.head=waiters=&a; if (mode==2) a.next=a.all_next=&b;
        tasks[0].sync_waiter=&a;tasks[1].sync_waiter=&b;
        tasks[0].audio_class=mode!=1;tasks[0].state=tasks[1].state=T_BLOCKED;
        struct kwork_stats before_work;kwork_snapshot(&before_work);
        unsigned before=starts;flags=0;
        kwait_wake_irq(&q,mode==3);
        struct kwork_stats after_work;kwork_snapshot(&after_work);
        if (!mode) CHECK(starts==before+1 && tasks[0].state==T_READY && !q.notify_active && after_work.queued==before_work.queued);
        else CHECK(starts==before && q.notify_active && after_work.queued==before_work.queued+1);
        flags=0x200;drain_work();
        CHECK(tasks[0].state==T_READY && !q.notify_active);
        if (mode==2) { CHECK(tasks[1].state==T_READY); wait_unlink(&b); }
        wait_unlink(&a);CHECK(!waiters && !q.head);
        tasks[0].audio_class=false;tasks[0].state=tasks[1].state=T_RUNNING;
    }
    current(0);
    puts("f1-90 IRQ wake: audio-class single waiter woken in the top half without deferred work; other, shared or I/O waits stay deferred PASS");
}
static unsigned coalesced_seen, coalesced_count;
static void coalesced_work(void *arg)
{
    (void)arg;coalesced_seen++;coalesced_count=kwork_notifications();
    if(coalesced_seen==1)for(unsigned i=0;i<1000;i++)CHECK(kwork_queue(coalesced_work,0));
}
static void test_coalesced_work(void)
{
    current(0);
    for(unsigned i=0;i<1000;i++)CHECK(kwork_queue(coalesced_work,0));
    struct kwork_stats s;kwork_snapshot(&s);CHECK(s.pending==1);
    drain_work();CHECK(coalesced_seen==2 && coalesced_count==1000);
    kwork_snapshot(&s);CHECK(!s.pending && s.coalesced>=1998);
    puts("f0-46 deferred: 2000 notifications, two counted turns, callback self-notifications requeue, no inline chain PASS");
}
static fs_lock contended_fs;
static unsigned fs_schedules;
static void release_fs_owner(void)
{
    CHECK(!flags && g_current->state == T_BLOCKED && g_current->prio == P_DEVICE);
    struct task *waiter = g_current;
    g_current = &tasks[0]; CHECK(g_current->prio == P_NORMAL);
    fs_schedules++;
    fs_lock_drop(&contended_fs);
    g_current = waiter;
}
static void test_fs_wait_lock(void)
{
    fs_lock_init(&contended_fs);
    for (unsigned i = 0; i < 32; i++) {
        current(0); tasks[0].prio = P_NORMAL;
        fs_lock_take(&contended_fs); CHECK(contended_fs.held && flags == 0x200);
        current(1); tasks[1].prio = P_DEVICE; on_schedule = release_fs_owner;
        fs_lock_take(&contended_fs);
        CHECK(contended_fs.held && flags == 0x200 && !contended_fs.wait.head && !waiters);
        fs_lock_drop(&contended_fs); CHECK(!contended_fs.held && flags == 0x200);
    }
    CHECK(fs_schedules == 32);
    on_schedule = 0; current(0);
    /* Wake before enqueue: the recheck acquires without sleeping. */
    fs_lock_take(&contended_fs); fs_lock_drop(&contended_fs);
    unsigned before = schedules;
    fs_lock_take(&contended_fs); CHECK(schedules == before); fs_lock_drop(&contended_fs);
    fs_lock_destroy(&contended_fs);
    puts("kernel fs_lock: P_DEVICE blocks behind P_NORMAL, wake/recheck, IF and waiter baselines PASS");
}
static void overrun_work(void *arg) { (void)arg; g_ticks += 2; }
static void test_work_report_bound(void)
{
    unsigned before = budget_reports;
    bool tsc = g_cpu_tsc; g_cpu_tsc = false; /* deterministic two-ms callbacks */
    for (unsigned i = 0; i < 2048; i++) {
        current(0); CHECK(kwork_queue(overrun_work, 0)); drain_work();
    }
    struct kwork_stats s; kwork_snapshot(&s);
    CHECK(s.budget_violations >= 2048 && budget_reports - before <= 10 && !s.pending);
    g_cpu_tsc = tsc;
    puts("kernel work reports: 2048 sustained overruns produce at most ten reports PASS");
}

static unsigned calls_a, calls_b;
static bool source_a, source_b;
static bool irq_a(unsigned irq, const char *owner)
{
    CHECK((!(flags & 0x200) || masked[irq]) && strcmp(owner, "irq-a") == 0);
    calls_a++;
    if (irq_clock_model) irq_test_cycles += irq_work_us;
    return source_a;
}
static bool irq_b(unsigned irq, const char *owner)
{
    CHECK((!(flags & 0x200) || masked[irq]) && strcmp(owner, "irq-b") == 0);
    calls_b++;
    return source_b;
}

static void fake_irq(unsigned irq)
{
    CHECK(handlers[irq] && !masked[irq]);
    struct trap_frame tf = { .vector = 0x20 + irq };
    flags = 0;
    eoi_pending = true;
    handlers[irq](&tf);
    CHECK(masked[irq] == (chains[irq].stats.deferred || chains[irq].stats.quarantined));
    eois++;
    eoi_pending = false;
    flags = 0x200;
}

static int irq_claim(unsigned irq, const char *owner)
{
    int h = registry_claim(RES_IRQ, irq, irq + 1, owner, true);
    CHECK(h >= 0);
    CHECK(registry_activate(h, registry_get((unsigned)h)->generation) == 0);
    return h;
}
static int async_claim;
static gen_t async_generation;
static uint32_t async_token=1;
static bool async_source(unsigned irq,const char *owner)
{
    CHECK(irq==7 && !strcmp(owner,"async-net") && eoi_pending);
    /* Qualified native function mask occurs here, before pending/EOI;
     * this model never masks the shared PIC line. */
    CHECK(!irq_chain_pending(irq,async_claim,async_generation+1,async_token));
    CHECK(irq_chain_pending(irq,async_claim,async_generation,async_token));
    return false; /* pending is separate from the synchronous Boolean */
}
static void test_async_irq(void)
{
    current(0);
    async_claim=irq_claim(7,"async-net");
    async_generation=registry_get((unsigned)async_claim)->generation;
    int peer=irq_claim(7,"irq-b");source_b=false;
    CHECK(!irq_chain_add(7,async_source,"async-net") && !irq_chain_add(7,irq_b,"irq-b"));
    struct irq_chain_stats before,after;CHECK(irq_chain_snapshot(7,&before));
    CHECK(!irq_chain_pending(7,async_claim,async_generation,async_token)); /* outside callback */
    for(unsigned i=0;i<IRQ_CHAIN_STUCK_PASSES+2;i++) fake_irq(7);
    CHECK(irq_chain_snapshot(7,&after) && !after.quarantined && !masked[7]);
    CHECK(after.unclaimed==before.unclaimed && after.pending_passes==before.pending_passes+IRQ_CHAIN_STUCK_PASSES+2);
    CHECK(irq_chain_resolve_pending(7,async_claim,async_generation,1,false));
    CHECK(!irq_chain_resolve_pending(7,async_claim,async_generation,1,true));
    async_token=2;fake_irq(7);
    CHECK(!irq_chain_resolve_pending(7,async_claim,async_generation,1,true));
    CHECK(!registry_quarantine(async_claim,async_generation));
    CHECK(!irq_chain_resolve_pending(7,async_claim,async_generation,2,true));
    source_b=true;unsigned calls=calls_b;fake_irq(7);
    CHECK(calls_b==calls+1 && !masked[7]);drain_work();
    CHECK(!irq_chain_remove(7,irq_b,"irq-b"));release_active(peer);
    puts("ND14 shared IRQ: pending separated from not-mine, stale/duplicate generations/tokens refused, peer survives PASS");
}
static unsigned shared_status[3], shared_acks[3], shared_calls[3];
static bool assert_during_handler;
static bool three_devices(unsigned irq, const char *owner)
{
    unsigned i = (unsigned)(owner[0] - '0');
    CHECK(i < 3 && (!(flags & 0x200) || masked[irq]));
    shared_calls[i]++;
    unsigned status = shared_status[i];
    if (status) { shared_acks[i] |= status; shared_status[i] &= ~status; }
    if (i == 2 && assert_during_handler) { shared_status[0] |= 8; assert_during_handler = false; }
    return status != 0;
}
static void test_three_shared_devices(void)
{
    static const char *const owners[] = {"0-audio", "1-usb", "2-net"};
    int handles[3]; unsigned before = eois;
    for (unsigned i = 0; i < 3; i++) {
        handles[i] = irq_claim(7, owners[i]);
        CHECK(!irq_chain_add(7, three_devices, owners[i]));
    }
    for (unsigned i = 0; i < 3; i++) {
        shared_status[i] = 1u << i;
        fake_irq(7); drain_work();
        CHECK(shared_acks[i] == (1u << i) && eois == before + i + 1);
    }
    shared_status[2] = 4; assert_during_handler = true;
    fake_irq(7); drain_work();
    CHECK(shared_status[0] == 8 && !masked[7]); /* asserted after its handler */
    fake_irq(7); drain_work();
    CHECK(!shared_status[0] && shared_acks[0] == 9 && eois == before + 5);
    for (unsigned i = 0; i < 3; i++) CHECK(shared_calls[i] == 5);
    /* Failed device has stopped its source; detach after a normal pass. */
    fake_irq(7);
    CHECK(!irq_chain_remove(7, three_devices, owners[1]));
    release_active(handles[1]);
    drain_work(); CHECK(!masked[7]);
    shared_status[0] = shared_status[2] = 16;
    fake_irq(7); drain_work();
    CHECK(shared_calls[1] == 6 && shared_calls[0] == 7 && shared_calls[2] == 7);
    struct irq_chain_stats s;
    CHECK(irq_chain_snapshot(7, &s) && s.participants == 2 && s.handled == 6 && s.unclaimed == 1);
    CHECK(registry_get((unsigned)handles[1])->state == RS_RELEASED);
    irq_chain_spurious(7); irq_chain_spurious(15);
    CHECK(irq_chain_snapshot(7, &s) && s.spurious == 1 && eois == before + 7);
    CHECK(irq_chain_snapshot(15, &s) && s.spurious == 1);
    unsigned mask_before = masks, unmask_before = unmasks;
    for (unsigned i = 0; i < 3; i += 2) {
        CHECK(!irq_chain_remove(7, three_devices, owners[i])); release_active(handles[i]);
    }
    CHECK(!masked[7] && !handlers[7] && masks == mask_before && unmasks == unmask_before);
    puts("shared INTx: three devices, interleaved/late assertion, one EOI per pass, failed device release preserves peers PASS");
}
static int fatal_irq_lease;
static unsigned fatal_irq_passes;
static bool fatal_irq_source(unsigned irq, const char *owner)
{
    CHECK(irq == 6 && !strcmp(owner, "fatal-usb") && !flags && eoi_pending);
    fatal_irq_passes++;
    CHECK(!registry_quarantine(fatal_irq_lease, registry_get((unsigned)fatal_irq_lease)->generation));
    CHECK(!masked[irq]); /* only this member is revoked, before EOI */
    return true;       /* a persistent source claims every pass */
}
static void test_claimed_fatal_quarantine(void)
{
    fatal_irq_lease = irq_claim(6, "fatal-usb");
    int peer = irq_claim(6, "irq-b");
    CHECK(!irq_chain_add(6, fatal_irq_source, "fatal-usb"));
    CHECK(!irq_chain_add(6, irq_b, "irq-b"));
    unsigned mask_before = masks, unmask_before = unmasks, eoi_before = eois;
    fake_irq(6);
    struct irq_chain_stats s;
    CHECK(irq_chain_snapshot(6, &s) && !s.quarantined && s.participants == 1 && s.passes == 1 && !s.unclaimed);
    CHECK(fatal_irq_passes == 1 && eois == eoi_before + 1 && !masked[6]);
    CHECK(registry_get((unsigned)peer)->state == RS_ACTIVE);
    drain_work();
    unsigned calls = calls_b;
    fake_irq(6); drain_work();
    CHECK(fatal_irq_passes == 1 && calls_b == calls + 1);
    CHECK(!masked[6] && masks == mask_before && unmasks == unmask_before);
    CHECK(!chains[6].members[0].handler);
    puts("shared IRQ: self-quarantine clears one member before EOI; peers continue without mask writes PASS");
}

static unsigned newcomer_calls;
static bool irq_newcomer(unsigned irq,const char *owner)
{
    CHECK((!(flags&0x200) || masked[irq]) && !strcmp(owner,"irq-c"));
    newcomer_calls++;return true;
}
static void test_chain_smi_budget(void)
{
    unsigned detached_before=detached_reports;
    current(0); irq_clock_model = true; g_cpu_tsc = true; g_tsc_per_ms = 1000;
    source_a = source_b = true;
    int a = irq_claim(4, "irq-a"), b = irq_claim(4, "irq-b");
    CHECK(!irq_chain_add(4, irq_a, "irq-a") && !irq_chain_add(4, irq_b, "irq-b"));
    unsigned nb = calls_b;
    irq_stall_read = 6; /* second read of the first member's end probe */
    fake_irq(4);
    struct irq_chain_stats s;
    CHECK(irq_chain_snapshot(4, &s) && s.deferred && s.smi_attributed == 1 && !s.quarantined);
    CHECK(calls_b == nb && !chains[4].members[0].budget.count);
    drain_work(); CHECK(calls_b == nb + 1 && !masked[4]);
    irq_work_us = 2100;
    for (unsigned i = 0; i < 3; i++) {
        /* Detachment logging must not steal the last continuation slot. */
        if (i == 2) for (unsigned j = 1; j < KWORK_CAPACITY; j++) CHECK(kwork_queue(tick_work, 0));
        fake_irq(4); drain_work();
        CHECK(irq_chain_snapshot(4, &s) && !s.quarantined && !masked[4]);
        CHECK(s.smi_attributed == 1 && s.participants == (i == 2 ? 1u : 2u));
        CHECK(registry_get((unsigned)b)->state == RS_ACTIVE);
    }
    CHECK(registry_get((unsigned)a)->state == RS_QUARANTINED);
    CHECK(calls_b == nb + 4);
    CHECK(detached_reports==detached_before+1 && strstr(last_detached,"irq=4 owner=irq-a"));
    /* f0-40: the failed lease must not reject an unrelated future member
     * while the surviving peer still owns this healthy shared line. */
    CHECK(registry_claim(RES_IRQ,4,5,"irq-a",true)==-EINVAL); /* no self-restart */
    CHECK(registry_claim(RES_IRQ,4,5,"exclusive",false)==-EINVAL);
    int newcomer=registry_claim(RES_IRQ,4,5,"irq-c",true);
    CHECK(newcomer>=0);if(newcomer<0)exit(1);
    CHECK(strstr(last_log,"stage=irq-member-admit irq=4 owner=irq-c retained=irq-a"));
    gen_t admitted=registry_get((unsigned)newcomer)->generation;
    CHECK(!registry_activate(newcomer,admitted) && !irq_chain_add(4,irq_newcomer,"irq-c"));
    irq_work_us=0;nb=calls_b;fake_irq(4);drain_work();
    CHECK(newcomer_calls==1 && calls_b==nb+1 && !masked[4] && !chains[4].stats.quarantined);
    CHECK(registry_get((unsigned)a)->state==RS_QUARANTINED);
    CHECK(!irq_chain_remove(4,irq_newcomer,"irq-c"));release_active(newcomer);
    /* Replay a lease already released elsewhere: never invoke that slot. */
    res[b].state = RS_RELEASED;
    nb = calls_b; fake_irq(4); drain_work();
    CHECK(calls_b == nb && !chains[4].stats.participants && !chains[4].stats.quarantined);
    res[b].state = RS_ACTIVE; /* restore fixture accounting for real release */
    release_active(b);
    CHECK(registry_claim(RES_IRQ,4,5,"empty-line",true)==-EINVAL);
    /* A stale generation is skipped and cleared without poisoning peers. */
    a = irq_claim(3, "irq-a"); b = irq_claim(3, "irq-b");
    CHECK(!irq_chain_add(3, irq_a, "irq-a") && !irq_chain_add(3, irq_b, "irq-b"));
    chains[3].members[0].generation++;
    nb = calls_b; fake_irq(3); drain_work();
    CHECK(!chains[3].members[0].handler && calls_b == nb + 1 && !masked[3]);
    CHECK(registry_get((unsigned)b)->state == RS_ACTIVE && !chains[3].stats.quarantined);
    /* A mismatched IRQ identity is corruption of the line itself. */
    res[b].end++;
    fake_irq(3); drain_work();
    CHECK(chains[3].stats.quarantined && masked[3]);
    irq_clock_model = false; irq_work_us = 0; g_tsc_per_ms = 1ull << 40;
    puts("shared IRQ F0-39: measured SMI defers without fault; third genuine overrun detaches only offender; stale skip and corrupt-line quarantine PASS");
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
    w = writes;
    struct kwork_stats work_before, work_after;
    kwork_snapshot(&work_before);
    fake_irq(11);
    kwork_snapshot(&work_after);
    CHECK(calls_a == 1 && calls_b == 1 && eois == 1 && writes == w && !masked[11]);
    CHECK(!chains[11].stats.deferred && work_after.queued == work_before.queued);
    drain_work();
    CHECK(!masked[11] && writes == w);
    source_a = false;
    fake_irq(11);
    CHECK(calls_a == 2 && calls_b == 2);
    gen_t ag = registry_get((unsigned)a)->generation;
    CHECK(registry_quiesce(a, ag, idle_ok) == 0);
    CHECK(registry_release(a, ag) == -EINVAL); /* handler must be detached */
    drain_work();
    fake_irq(11);                         /* quiescing owner can still drain */
    CHECK(calls_a == 3 && calls_b == 3);
    CHECK(irq_chain_remove(11, irq_a, "irq-a") == 0);
    CHECK(registry_release(a, ag) == 0);
    drain_work();
    fake_irq(11);
    drain_work();
    CHECK(calls_a == 3 && calls_b == 4 && !masked[11]);
    source_b = false;
    unsigned mask_before = masks, unmask_before = unmasks;
    struct irq_chain_stats s;
    for (unsigned i = 0; i < IRQ_CHAIN_STUCK_PASSES; i++) {
        fake_irq(11);
        drain_work();
        CHECK(irq_chain_snapshot(11, &s));
        CHECK(s.consecutive_unclaimed == i + 1);
        CHECK(s.quarantined == (i + 1 == IRQ_CHAIN_STUCK_PASSES));
        CHECK(masks == mask_before + (s.quarantined ? 1u : 0u) && unmasks == unmask_before);
    }
    CHECK(masked[11] && s.quarantines == 1 && s.participants == 0);
    CHECK(s.handled == 4 && s.unclaimed == 1000 && s.passes == 1004);
    CHECK(registry_get((unsigned)b)->state == RS_QUARANTINED);
    CHECK(registry_release(b, registry_get((unsigned)b)->generation) == -EINVAL);
    CHECK(registry_claim(RES_IRQ, 11, 12, "new-sharer", true) == -EINVAL);
    CHECK(irq_chain_add(11, irq_b, "irq-b") == -EINVAL);
    CHECK(irq_chain_remove(11, irq_b, "irq-b") == -EINVAL && masked[11]);

    /* Remaining handlers run in the worker after the aggregate IRQ budget. */
    a = irq_claim(10, "irq-a");
    b = irq_claim(10, "irq-b");
    CHECK(irq_chain_add(10, irq_a, "irq-a") == 0);
    CHECK(irq_chain_add(10, irq_b, "irq-b") == 0);
    source_a = source_b = true;
    unsigned nb = calls_b;
    mask_before = masks; unmask_before = unmasks;
    g_tsc_per_ms = 1;
    fake_irq(10);
    CHECK(calls_b == nb && masked[10] && masks == mask_before + 1 && unmasks == unmask_before);
    /* Even an already latched dispatch while deferred must not mask twice. */
    struct trap_frame pending = { .vector = 0x20 + 10 };
    flags = 0; handlers[10](&pending); flags = 0x200;
    CHECK(masks == mask_before + 1);
    g_tsc_per_ms = 1ull << 40;
    drain_work();
    CHECK(calls_b == nb + 1 && !masked[10] && masks == mask_before + 1 && unmasks == unmask_before + 1);
    CHECK(irq_chain_snapshot(10, &s) && s.budget_violations == 1);
    g_tsc_per_ms = 1;
    fake_irq(10);
    g_tsc_per_ms = 1ull << 40;
    CHECK(irq_chain_remove(10, irq_b, "irq-b") == 0); /* deferred callback not yet invoked */
    release_active(b);
    drain_work();
    CHECK(calls_b == nb + 1 && !masked[10]);
    mask_before = masks; unmask_before = unmasks;
    CHECK(irq_chain_remove(10, irq_a, "irq-a") == 0);
    CHECK(irq_chain_remove(10, irq_b, "irq-b") == -EINVAL && !masked[10] && !handlers[10]);
    CHECK(masks == mask_before && unmasks == unmask_before);
    release_active(a);

    /* Clean reattach is a cached no-op. Removing the final member while
     * deferred must retain that existing mask until a new activation. */
    a = irq_claim(10, "irq-a");
    CHECK(!irq_chain_add(10, irq_a, "irq-a") && handlers[10]);
    CHECK(masks == mask_before && unmasks == unmask_before);
    g_tsc_per_ms = 1;
    fake_irq(10);
    g_tsc_per_ms = 1ull << 40;
    CHECK(chains[10].stats.deferred && masks == mask_before + 1);
    CHECK(!irq_chain_remove(10, irq_a, "irq-a") && !handlers[10]);
    release_active(a);
    drain_work();
    CHECK(!chains[10].stats.deferred && masked[10] && masks == mask_before + 1 && unmasks == unmask_before);
    a = irq_claim(10, "irq-a");
    CHECK(!irq_chain_add(10, irq_a, "irq-a") && !masked[10]);
    CHECK(masks == mask_before + 1 && unmasks == unmask_before + 1);
    CHECK(!irq_chain_remove(10, irq_a, "irq-a") && !handlers[10] && !masked[10]);
    release_active(a);

    /* A failed member proof during deferral preserves the peer/rearm. */
    a = irq_claim(5, "irq-a");
    b = irq_claim(5, "irq-b");
    CHECK(irq_chain_add(5, irq_a, "irq-a") == 0);
    CHECK(irq_chain_add(5, irq_b, "irq-b") == 0);
    mask_before = masks; unmask_before = unmasks;
    g_tsc_per_ms = 1;
    fake_irq(5);
    g_tsc_per_ms = 1ull << 40;
    CHECK(chains[5].stats.deferred && masks == mask_before + 1);
    CHECK(registry_quiesce(a, registry_get((unsigned)a)->generation, idle_bad) == -EFAULT);
    drain_work();
    CHECK(irq_chain_snapshot(5, &s) && !s.quarantined && !masked[5] && s.participants == 1);
    CHECK(registry_get((unsigned)a)->state == RS_QUARANTINED);
    CHECK(registry_get((unsigned)b)->state == RS_ACTIVE);
    CHECK(masks == mask_before + 1 && unmasks == unmask_before + 1);

    /* No continuation slot: line and claims must remain quarantined. */
    a = irq_claim(9, "irq-a");
    CHECK(irq_chain_add(9, irq_a, "irq-a") == 0);
    for (unsigned i = 0; i < KWORK_CAPACITY; i++)
        CHECK(kwork_queue(tick_work, (void *)(uintptr_t)i));
    mask_before = masks; unmask_before = unmasks;
    /* A complete claiming pass needs no continuation slot, even when full. */
    fake_irq(9);
    CHECK(irq_chain_snapshot(9, &s) && !s.quarantined && !s.deferred && !masked[9]);
    CHECK(masks == mask_before && unmasks == unmask_before);
    g_tsc_per_ms = 1;
    fake_irq(9);
    g_tsc_per_ms = 1ull << 40;
    CHECK(irq_chain_snapshot(9, &s) && s.quarantined && masked[9]);
    drain_work();
    CHECK(masked[9] && registry_get((unsigned)a)->state == RS_QUARANTINED);
    CHECK(masks == mask_before + 1 && unmasks == unmask_before);
    printf("kernel services IRQ: PASS (claiming pass/clean final detach=0 mask writes; deferral=1 mask/1 unmask; 1000 unclaimed passes=1 quarantine mask; EOI, removal, budget/overflow)\n");
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

static bool takeover_idle(int h, gen_t g)
{
    CHECK(flags & 0x200);
    CHECK(registry_get(h)->state==RS_QUARANTINED);
    CHECK(registry_release(h,g)==-EINVAL);
    return true;
}
static bool takeover_busy(int h, gen_t g) { (void)h; (void)g; return false; }
static bool takeover_cancelled(int h, gen_t g)
{ CHECK(!registry_quarantine(h,g)); return true; }
static void release_active_or_claimed(int h)
{
    gen_t g = registry_get((unsigned)h)->generation;
    CHECK(registry_activate(h, g) == 0);
    release_active(h);
}
/* fx-1 on the production registry: LPT1 at 3BCh (the T23's BDA value) holds
 * 3BCh-3BEh; the display claims the Windows pair 3B0h-3BBh + 3C0h-3DFh. */
static void test_vga_ports_with_lpt(void)
{
    static const uint16_t lpt_bases[] = {0x3BC, 0x378, 0x278};
    for (unsigned i = 0; i < 3; i++) {
        registry_init();
        int lpt = registry_claim(RES_PORT, lpt_bases[i], lpt_bases[i] + 3u, "lpt1", false);
        CHECK(lpt >= 0);
        gen_t lg = registry_get((unsigned)lpt)->generation;
        CHECK(registry_activate(lpt, lg) == 0);
        /* The pre-fx-1 single span collides only where LPT1 really lives. */
        int span = registry_claim(RES_PORT, 0x3B0, 0x3E0, "savage-vga", false);
        CHECK((span < 0) == (lpt_bases[i] == 0x3BC));
        if (span >= 0) release_active_or_claimed(span);
        int color = registry_claim(RES_PORT, CIUKI_VGA_COLOR_FIRST, CIUKI_VGA_COLOR_END, "savage-vga", false);
        int mono = registry_claim(RES_PORT, CIUKI_VGA_MONO_FIRST, CIUKI_VGA_MONO_END, "i810", false);
        CHECK(color >= 0 && mono >= 0);
        /* Genuine overlaps with either side are still refused and change nothing. */
        struct registry_stats before, after; registry_snapshot(&before);
        CHECK(registry_claim(RES_PORT, 0x3C8, 0x3CC, "intruder", false) == -EINVAL);
        CHECK(registry_claim(RES_PORT, 0x3B4, 0x3B6, "intruder", false) == -EINVAL);
        CHECK(registry_claim(RES_PORT, lpt_bases[i] + 2u, lpt_bases[i] + 4u, "intruder", false) == -EINVAL);
        registry_snapshot(&after);
        CHECK(after.live == before.live && after.claims == before.claims);
        if (color >= 0) { gen_t g = registry_get((unsigned)color)->generation; CHECK(registry_activate(color, g) == 0); release_active(color); }
        if (mono >= 0) { gen_t g = registry_get((unsigned)mono)->generation; CHECK(registry_activate(mono, g) == 0); release_active(mono); }
        release_active(lpt);
    }
    puts("VGA ports with LPT: PASS (3BCh/378h/278h x Windows VGA ranges; old single span refused only at 3BCh; true overlaps refused)");
}
static void test_takeover(void)
{
    for (unsigned kind=0;kind<2;kind++) {
        enum res_type type=kind ? RES_MMIO : RES_PORT;
        uint32_t start=kind ? 0xF0000000 : 0x3B0, end=kind ? 0xF0080000 : 0x3E0;
        int h=registry_claim(type,start,end,"savage-vga",false); CHECK(h>=0);
        gen_t g=registry_get(h)->generation;
        CHECK(!registry_activate(h,g) && !registry_quarantine(h,g));
        CHECK(registry_claim(type,start,end,"supersavage-mode",false)==-EINVAL);
        CHECK(registry_offer_takeover(h,g+1,"supersavage-mode",takeover_idle)==-EINVAL);
        flags=0; CHECK(registry_offer_takeover(h,g,"supersavage-mode",takeover_idle)==-EINVAL); flags=0x200;
        CHECK(registry_offer_takeover(h,g,"supersavage-mode",takeover_busy)==-EFAULT);
        CHECK(registry_claim(type,start,end,"supersavage-mode",false)==-EINVAL);
        CHECK(registry_offer_takeover(h,g,"supersavage-mode",takeover_cancelled)==-EINVAL);
        CHECK(registry_claim(type,start,end,"supersavage-mode",false)==-EINVAL);
        CHECK(!registry_offer_takeover(h,g,"supersavage-mode",takeover_idle));
        struct registry_stats before,after; registry_snapshot(&before);
        CHECK(registry_claim(type,start,end,"intruder",false)==-EINVAL);
        CHECK(registry_claim(type,start+1,end,"supersavage-mode",false)==-EINVAL);
        CHECK(registry_claim(type,start,end+1,"supersavage-mode",false)==-EINVAL);
        CHECK(registry_claim(type,start,end,"supersavage-mode",false)==h);
        gen_t next=registry_get(h)->generation;
        CHECK(next!=g && registry_valid(h,next) && !registry_valid(h,g));
        CHECK(registry_quarantine(h,g)==-EINVAL && registry_release(h,g)==-EINVAL);
        registry_snapshot(&after);
        CHECK(after.live==before.live && after.claims==before.claims+1 && after.releases==before.releases+1);
        CHECK(!registry_activate(h,next)); release_active(h);
    }
    puts("PASS f3-43 registry: idle-authorized exact port/MMIO takeover only, unsafe/unoffered/foreign/stale rejected, fresh generation and live counters");
}

int main(void)
{
    memset(masked, true, sizeof(masked));
    for (unsigned i = 0; i < ARRAY_SIZE(tasks); i++)
        tasks[i] = (struct task){ .id = i + 1, .generation = 1, .state = T_RUNNING };
    current(0);
    test_boot_reservations();
    test_registry();
    test_stackprot();
    test_deadlines();
    test_sync();
    test_lock_order_records();
    test_lock_order_policy();
    test_work();
    test_timer_queue();
    test_coalesced_work();
    test_irq_notification();
    test_audio_irq_wake();
    test_irq_chain();
    test_three_shared_devices();
    test_async_irq();
    test_claimed_fatal_quarantine();
    test_chain_smi_budget();
    test_registry_probe();
    test_fs_wait_lock();
    test_work_report_bound();
    test_takeover();
    test_vga_ports_with_lpt();
    test_generation_exhaustion();
    printf("kernel services: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
