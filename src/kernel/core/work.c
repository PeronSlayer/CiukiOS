/* Bounded IRQ-to-thread work ring. No allocation in the producer path.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/sync.h>
#include <ciuki/work.h>
#include <ciuki/timing.h>
#include <ciuki/cpustat.h>

static struct { kwork_fn fn; void *arg; unsigned notifications; } ring[KWORK_CAPACITY];
static unsigned head, tail;
static struct kwork_stats stats;
static struct task *worker;
static struct kwait available;
static uint64_t slice_start, slice_tick, reported_lost;
static bool running;
static unsigned notifications;
static uint64_t service_us;

unsigned kwork_notifications(void) { return notifications; }
bool kwork_current(void) { return worker && g_current == worker; }
static uint64_t slice_us(void)
{
    return g_cpu_tsc && g_tsc_per_ms ? ((cpu_path_clock ? cpu_path_clock() : ktime_cycles()) - slice_start) * 1000 / g_tsc_per_ms :
        (deadline_after_ms(0) - slice_tick) * 1000;
}
static void slice_charge(void)
{
    uint64_t us = slice_us();
    service_us += us;
    if (us > stats.max_run_us) stats.max_run_us = us;
    if (us > 1000) stats.budget_violations++;
}

static void slice_begin(void)
{
    slice_start = cpu_path_clock ? cpu_path_clock() : ktime_cycles();
    slice_tick = deadline_after_ms(0);
}

void kwork_yield(void)
{
    if (g_current != worker || !(read_eflags() & 0x200))
        panic("work: yield outside worker");
    uint32_t f = irq_save();
    if (running) slice_charge();
    stats.yields++;
    irq_restore(f);
    task_yield();
    slice_begin();
}

void kwork_snapshot(struct kwork_stats *out)
{
    uint32_t f = irq_save();
    *out = stats;
    irq_restore(f);
}

bool kwork_queue(kwork_fn fn, void *arg)
{
    uint32_t f = irq_save();
    if (fn && worker) for (unsigned n = 0, i = head; n < stats.pending; n++, i = (i + 1) % KWORK_CAPACITY) {
        if (ring[i].fn == fn && ring[i].arg == arg) {
            if (ring[i].notifications != UINT32_MAX) ring[i].notifications++;
            stats.coalesced++; irq_restore(f); return true;
        }
    }
    if (!fn || !worker || stats.pending == KWORK_CAPACITY) {
        stats.lost++;
        irq_restore(f);
        return false;
    }
    ring[tail].fn = fn;
    ring[tail].arg = arg;
    ring[tail].notifications = 1;
    tail = (tail + 1) % KWORK_CAPACITY;
    stats.pending++;
    stats.queued++;
    kwait_wake_all(&available);
    irq_restore(f);
    return true;
}

static void report_loss(void)
{
    struct kwork_stats s;
    kwork_snapshot(&s);
    if (s.lost != reported_lost) {
        reported_lost = s.lost;
        klog("[work] lost=%llu pending=%u", reported_lost, s.pending);
    }
}

bool kwork_run_one(void)
{
    if (g_current != worker || !(read_eflags() & 0x200) || running)
        panic("work: invalid consumer");
    slice_begin();
    uint32_t f = irq_save();
    if (!stats.pending) {
        irq_restore(f);
        report_loss();
        slice_begin();
        return false;
    }
    kwork_fn fn = ring[head].fn;
    void *arg = ring[head].arg;
    notifications = ring[head].notifications;
    service_us = 0;
    head = (head + 1) % KWORK_CAPACITY;
    stats.pending--;
    irq_restore(f);
    running = true;
    g_current->device_worker_site=(uint32_t)(uintptr_t)fn;
    fn(arg);
    if (!(read_eflags() & 0x200))
        panic("work: callback left interrupts disabled");
    uint64_t us = slice_us();
    bool overrun = us > 1000;
    f = irq_save();
    slice_charge();
    g_current->device_worker_site=0;
    running = false;
    stats.completed++;
    if (service_us > stats.max_service_us) stats.max_service_us = service_us;
    if (service_us > 2000) stats.service_violations++;
    unsigned pending = stats.pending, violations = stats.budget_violations;
    irq_restore(f);
    /* The report itself feeds the boot log, whose drain runs on this worker:
     * an unbounded report per overrun loops into more overruns (lead fix,
     * 2026-10-11). Report the first eight, then one in 1024; the counter
     * in kwork_stats keeps the total. */
    static unsigned reported;
    if (overrun && (reported < 8 || (violations & 1023u) == 0)) {
        reported++;
        klog("[work] budget exceeded site=%p us=%llu pending=%u violations=%u", (void *)fn, us, pending, violations);
    }
    report_loss();
    /* Returning/yielding inside a callback remains its author's duty. */
    kwork_yield();
    return true;
}

static bool has_work(void *arg)
{
    (void)arg;
    return stats.pending != 0;
}

static void worker_main(void *arg)
{
    (void)arg;
    slice_begin();
    for (;;) {
        while (kwork_run_one())
            ;
        kwait_wait_until(&available, has_work, 0, deadline_after_ms(1000));
    }
}

int kwork_init(void)
{
    if (!(read_eflags() & 0x200) || !g_current)
        return -EINVAL;
    if (worker)
        return 0;
    kwait_init(&available);
    worker = task_create_kernel("device-work", worker_main, 0, P_DEVICE);
    if (!worker)
        return -ENOMEM;
    task_start(worker);
    return 0;
}
