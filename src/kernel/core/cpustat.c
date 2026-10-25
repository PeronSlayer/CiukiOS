/* Bounded CPU attribution; all output is from the cpustat task, never an IRQ.
 * Research/record format: docs/design/research/kernel-latency-2026-10-14.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/cpustat.h>
#include <ciuki/sync.h>
#include <ciuki/timing.h>
#include <ciuki/bootlog.h>
#include <ciuki/work.h>
#include <ciuki/ledger.h>
#include <ciuki/reclaim.h>
#include <ciuki/pager.h>
#include <ciuki/smi.h>

/* f1-100: the ThinkPad EC traffic record of the window (drivers/thinkpad_ec.c). */
extern void thinkpad_ec_window_emit(void) __attribute__((weak));
extern void display_fullscreen_stall_poll(void) __attribute__((weak)); /* fx-22 */
/* fx-5 follow-up: lock-order reports of a normal boot (core/sync.c). */
#pragma weak ksync_order_snapshot

static struct task *stat_task;
static uint64_t window_start, irq_cycles, exited_cycles, classes[P_COUNT];
static uint64_t stretch_max, resched_start, resched_max, paths[CPU_PATHS];
static uint32_t stretch_eip, switches, overruns, boost_base;
static char stretch_name[16] = "none", resched_name[16] = "none";
static uint32_t stretch_id;
/* consumed_us: the largest own device-window consumption at a demotion. */
struct overrun_event { char name[16]; uint32_t id, count, consumed_us; };
static struct overrun_event overrun_events[16], audio_events[4];
static unsigned overrun_used, overrun_lost, audio_used, audio_lost;
/* f1-98: reservation inheritances per window (from waiter to holder). */
struct inherit_event { char from[16], to[16]; uint32_t from_id, to_id, count; uint8_t level, reason; };
static struct inherit_event inherit_events[8];
static unsigned inherit_used, inherit_lost;
/* f1-101: audio producer boosts per task and window. */
struct producer_event { char name[16]; uint32_t id, boosts, over_budget; uint64_t cycles; };
static struct producer_event producer_events[8];
static unsigned producer_used, producer_lost;
static struct sched_share_stats share_base;
static bool forced, threshold, resched_pending;
static uint64_t automatic_window; /* normal-boot diagnostic dumps: at most one/minute */
static bool diagnostic_active;
bool cpu_diagnostic_active(void) { return diagnostic_active; }
static struct sched_stats sched_base;
static struct {
    uint64_t start;
    uint32_t site;
    unsigned count;
    /* f1-94: the IRQ path, the PIT top half and schedule() in one section
     * place up to 17 marks; the tail beyond capacity stays unclassified. */
    struct { uint64_t stamp; enum cpu_irqoff_phase phase; } marks[20];
    const struct task *from, *to;
} irqoff;
static struct irqoff_long {
    uint64_t cycles, phase_cycles;
    uint32_t site;
    enum cpu_irqoff_phase phase;
    bool gap;
    char from[16], to[16];
} irqoff_window;

static uint64_t stamp(void) { return g_tsc_per_ms ? ktime_cycles() : g_ticks; }
static uint64_t per_ms(void) { return g_tsc_per_ms ? g_tsc_per_ms : 1; }
static uint32_t ms(uint64_t cycles) { return (uint32_t)(cycles / per_ms()); }
static unsigned pm(uint64_t cycles, uint64_t total)
{ return total ? (unsigned)(cycles * 1000 / total) : 0; }

/* f2-96: firmware (SMM) time is not task work. On the T23 every EC
 * transaction raises two 2.1 ms SMIs (f1-100); trial 5a10eef4 logged 14
 * audio_refill_overrun records with consumed_us=2182-2227, one SMI each
 * charged to the refill's 2 ms own budget. Two conservative proofs, and only
 * the scheduler budgets (device, audio refill, desktop share) leave the time
 * out; run_cycles and the class totals stay raw:
 *  - IF clear: a gap of 1 ms or more between two marks of straight-line
 *    phases (the f1-94 TSC rule, cpu_irqoff_bounded);
 *  - IF set: a PIT interrupt more than one period late (tick gap above 2 ms,
 *    the f1-94 blackout rule). The missed edge fell inside SMM, or the CPU
 *    would have taken it, so the time from the edge to the IRQ entry, after
 *    the last IF-clear section and the task's last accounting point, was
 *    spent outside the OS. V86 frames are excluded: a guest with IOPL 3 can
 *    clear IF itself. */
static uint64_t irqoff_credited, irqoff_last_end, pit_last, pit_phase;
static uint64_t firmware_window, firmware_max, firmware_total;
static uint32_t firmware_count;
static uint64_t irqoff_firmware(uint64_t from, uint64_t now)
{
    if (!g_tsc_per_ms || !irqoff.start || now < irqoff.start || now - irqoff.start < g_tsc_per_ms) return 0;
    unsigned count = irqoff.count;
    bool tail = count <= ARRAY_SIZE(irqoff.marks); /* beyond capacity: unmeasured */
    if (!tail) count = ARRAY_SIZE(irqoff.marks);
    uint64_t total = 0;
    for (unsigned i = 0; i < count; i++) {
        uint64_t a = irqoff.marks[i].stamp;
        uint64_t b = i + 1 < count ? irqoff.marks[i + 1].stamp : tail ? now : a;
        if (a < from) a = from;
        if (b > now) b = now;
        if (b > a && b - a >= g_tsc_per_ms && cpu_irqoff_bounded(irqoff.marks[i].phase)) total += b - a;
    }
    return total;
}
static void firmware_note(uint64_t cycles)
{
    if (!cycles) return;
    firmware_window += cycles; firmware_total += cycles; firmware_count++;
    if (cycles > firmware_max) firmware_max = cycles;
}
/* The firmware time inside [from, now], the interval being charged. */
static uint64_t firmware_take(struct task *t, uint64_t from, uint64_t now)
{
    uint64_t credit = t->firmware_credit;
    t->firmware_credit = 0;
    if (irqoff.start) {
        uint64_t gap = irqoff_firmware(irqoff_credited > from ? irqoff_credited : from, now);
        irqoff_credited = now;
        firmware_note(gap); credit += gap;
    }
    return credit < now - from ? credit : now - from;
}
uint64_t cpu_firmware_cycles(void)
{
    uint32_t f = irq_save(); /* written by the PIT IRQ; no torn 64-bit read */
    uint64_t total = firmware_total;
    irq_restore(f);
    return total;
}
static void pit_late(const struct trap_frame *tf, uint64_t now)
{
    uint64_t last = pit_last;
    pit_last = now;
    if (!g_tsc_per_ms || !last || now <= last) return;
    /* An entry at most 1.5 periods after the previous one is on time: the
     * PIT (mode 2) keeps its phase, so later edges follow from this stamp. */
    if ((now - last) * 2 <= 3 * g_tsc_per_ms) { pit_phase = now; return; }
    if (!g_current || now - last <= 2 * g_tsc_per_ms || (tf->eflags & (1u << 17))) return;
    /* The missed edge: the first one after the previous entry (which may
     * itself have been late, as for the second SMI of an EC transaction). */
    uint64_t from = last + g_tsc_per_ms;
    if (pit_phase && pit_phase <= last) {
        uint64_t edge = pit_phase + g_tsc_per_ms;
        for (unsigned n = 0; edge <= last && n < 64; n++) edge += g_tsc_per_ms;
        if (edge > last && edge < from) from = edge;
    }
    if (irqoff_last_end > from) from = irqoff_last_end;
    if (g_current->account_start > from) from = g_current->account_start;
    /* This IRQ's own IF-clear section began at its entry, after the edge;
     * a gap inside the entry path is the bounded-phase proof's. */
    uint64_t to = irqoff.start && irqoff.start > from && irqoff.start < now ? irqoff.start : now;
    if (to > from) { g_current->firmware_credit += to - from; firmware_note(to - from); }
}
static void name_copy(char *dst, const char *src)
{
    unsigned i = 0;
    while (i < 15 && src[i]) {
        unsigned char c = (unsigned char)src[i];
        dst[i++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') ? (char)c : '_';
    }
    while (i < 16) dst[i++] = 0;
}
void cpu_irqoff_begin(uint64_t start,uint32_t site)
{
    /* Most sections never schedule: do not clear the phase array or copy
     * task identities on their entry/exit. count bounds all stale slots. */
    irqoff.start=start; irqoff.site=site; irqoff.count=0; irqoff.from=0;
    irqoff_credited=start;
}
void cpu_irqoff_mark(enum cpu_irqoff_phase phase)
{
    if (!irqoff.start) return;
    unsigned n=irqoff.count;
    if (n<ARRAY_SIZE(irqoff.marks)) {
        irqoff.marks[n].stamp=read_tsc_unserialized();
        irqoff.marks[n].phase=phase; irqoff.count=n+1;
    } else irqoff.count=ARRAY_SIZE(irqoff.marks)+1; /* unmeasured tail */
}
void cpu_irqoff_tasks(const struct task *from,const struct task *to)
{
    if (irqoff.start) { irqoff.from=from; irqoff.to=to; }
}
uint64_t cpu_irqoff_end(uint64_t end)
{
    if (!irqoff.start) return 0;
    uint64_t firmware=irqoff_firmware(irqoff.start,end);
    /* f2-96: bounded gaps after the section's last charge lie in the open
     * interval of the task now running (after a switch: the next task). */
    if (g_current) {
        uint64_t from=irqoff_credited>g_current->account_start ? irqoff_credited : g_current->account_start;
        uint64_t gap=irqoff_firmware(from,end);
        if (gap) { g_current->firmware_credit+=gap; firmware_note(gap); }
    }
    irqoff_last_end=end;
    uint64_t d=end-irqoff.start;
    if (g_tsc_per_ms && end>=irqoff.start && d>=g_tsc_per_ms) {
        uint64_t measured=0,largest=0,largest_start=0;
        enum cpu_irqoff_phase phase=CPU_IRQOFF_NONE;
        unsigned count=irqoff.count;
        if (count>ARRAY_SIZE(irqoff.marks)) count=ARRAY_SIZE(irqoff.marks);
        /* Raw capture above has no phase subtraction, accumulation/ranking.
         * Only a section of 1 ms or more evaluates the bounded marks here. */
        for (unsigned i=0;i<count;i++) {
            if (irqoff.marks[i].phase==CPU_IRQOFF_NONE) continue;
            uint64_t a=irqoff.marks[i].stamp;
            if (i+1==count && irqoff.count>count) break;
            uint64_t b=i+1<count ? irqoff.marks[i+1].stamp : end;
            if (a<irqoff.start || b<a || b>end) continue;
            uint64_t elapsed=b-a; measured+=elapsed;
            if (elapsed>largest) { largest=elapsed; largest_start=a; phase=irqoff.marks[i].phase; }
        }
        if (measured>d) measured=d;
        /* f1-94: every long section is offered to the SMI attribution, not
         * only the window maximum; it samples SMI_STS once, after `end`. */
        if (smi_section_end) {
            struct smi_section x={irqoff.start,end,largest,largest_start,
                largest && cpu_irqoff_bounded(phase)};
            smi_section_end(&x);
        }
        if (d>irqoff_window.cycles) {
            irqoff_window.cycles=d; irqoff_window.site=irqoff.site;
            irqoff_window.gap=measured<d/2+d%2;
            irqoff_window.phase=phase;
            irqoff_window.phase_cycles=irqoff_window.gap ? d-measured : largest;
            const struct task *from=irqoff.from ? irqoff.from : g_current;
            const struct task *to=irqoff.from ? irqoff.to : g_current;
            name_copy(irqoff_window.from,from ? from->name : "none");
            name_copy(irqoff_window.to,to ? to->name : "none");
        }
    }
    irqoff.start=0;
    return firmware;
}

static void stretch_end(struct task *t, uint64_t now, uint32_t eip)
{
    if (!t || t->prio == P_IDLE || !t->stretch_start) return;
    uint64_t d = now - t->stretch_start;
    /* fx-16: name the site of shorter stretches too. T23 trial e831e685
     * reported stretch_max_ms=7-8 for desktop#40 and dosvm#59 with
     * eip=00000000 (unsampled below 20 ms) while the VM clock service was
     * held off for the same 8-9 ms (cadence_clock gap_max_ms). The first
     * kernel PIT tick beyond 4 ms records its EIP; the 20 ms threshold of
     * the diagnostic dump is unchanged. */
    if (d > 4 * per_ms() && !t->stretch_sampled) {
        t->active_stretch_eip = eip; t->stretch_sampled = true;
    }
    if (d > t->max_stretch) {
        t->stretch_eip = t->stretch_sampled ? t->active_stretch_eip : eip;
        t->max_stretch = d;
    }
    if (d > stretch_max) {
        stretch_max = d; stretch_eip = t->stretch_eip;
        name_copy(stretch_name, t->name); stretch_id = t->id;
    }
    if (d > 20 * per_ms()) threshold = true;
}
void cpu_stretch_snapshot(char name[16], uint32_t *duration_ms)
{
    uint32_t flags = irq_save();
    memcpy(name, stretch_name, 16);
    *duration_ms = ms(stretch_max);
    irq_restore(flags);
}

static void charge(struct task *t, uint64_t now)
{
    uint64_t d = now - t->account_start;
    uint64_t firmware = firmware_take(t, t->account_start, now);
    t->run_cycles += d;
    classes[t->account_prio] += d;
    t->account_start = now;
    sched_charge_runtime(t, d - firmware, per_ms()); /* f2-96: budgets exclude SMM */
}
void cpu_account(void) { if (g_current) charge(g_current, stamp()); }
void cpu_switch(struct task *prev, struct task *next)
{
    uint64_t now = stamp();
    if (prev) {
        charge(prev, now);
        stretch_end(prev, now, prev->active_stretch_eip);
        prev->stretch_start = 0;
    }
    if (resched_pending) {
        uint64_t d = now - resched_start;
        if (d > resched_max) { resched_max = d; name_copy(resched_name, prev->name); }
        if (d > 20 * per_ms()) threshold = true;
        resched_pending = false;
    }
    if (!prev) window_start = now;
    if (prev != next) switches++;
    next->account_start = now;
    next->account_prio = next->device_demoted ? P_NORMAL : next->prio;
    next->stretch_start = now; next->stretch_sampled = false; next->active_stretch_eip = 0;
}
void cpu_reap(struct task *t)
{
    uint64_t delta = t->run_cycles - t->window_base;
    exited_cycles += delta;
    /* Class totals are charged at dispatch boundaries, including exit. */
}
void cpu_point(void)
{
    uint32_t f = irq_save();
    uint64_t now = stamp();
    stretch_end(g_current, now, (uint32_t)(uintptr_t)__builtin_return_address(0));
    g_current->stretch_start = now; g_current->stretch_sampled = false; g_current->active_stretch_eip = 0;
    irq_restore(f);
}
static void overrun_note(struct overrun_event *events, unsigned capacity, unsigned *used,
                         unsigned *lost, struct task *t, uint64_t consumed)
{
    t->account_prio = P_NORMAL; /* the IRQ entry already charged the device interval */
    overruns++; threshold = true;
    unsigned i = 0;
    while (i < *used && events[i].id != t->id) i++;
    if (i == capacity) { (*lost)++; return; }
    if (i == *used) {
        (*used)++;
        name_copy(events[i].name, t->name);
        events[i].id = t->id;
        events[i].count = events[i].consumed_us = 0;
    }
    events[i].count++;
    uint64_t us = consumed * 1000 / per_ms();
    if (us > events[i].consumed_us) events[i].consumed_us = us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}
void cpu_overrun(struct task *t, uint64_t consumed)
{
    overrun_note(overrun_events, ARRAY_SIZE(overrun_events), &overrun_used, &overrun_lost, t, consumed);
}
void cpu_audio_overrun(struct task *t, uint64_t consumed)
{
    overrun_note(audio_events, ARRAY_SIZE(audio_events), &audio_used, &audio_lost, t, consumed);
}
void cpu_inherit(const struct task *from, const struct task *to, unsigned level, unsigned reason)
{
    uint32_t f = irq_save();
    unsigned i = 0;
    while (i < inherit_used && (inherit_events[i].from_id != from->id ||
           inherit_events[i].to_id != to->id || inherit_events[i].reason != reason)) i++;
    if (i == ARRAY_SIZE(inherit_events)) { inherit_lost++; irq_restore(f); return; }
    struct inherit_event *e = &inherit_events[i];
    if (i == inherit_used) {
        inherit_used++;
        name_copy(e->from, from->name); name_copy(e->to, to->name);
        e->from_id = from->id; e->to_id = to->id; e->reason = (uint8_t)reason;
        e->count = 0; e->level = 0;
    }
    e->count++;
    if (level > e->level) e->level = (uint8_t)level;
    irq_restore(f);
}
void cpu_audio_producer(const struct task *t, unsigned boosts, unsigned over_budget, uint64_t cycles)
{
    uint32_t f = irq_save();
    unsigned i = 0;
    while (i < producer_used && producer_events[i].id != t->id) i++;
    if (i == ARRAY_SIZE(producer_events)) { producer_lost++; irq_restore(f); return; }
    struct producer_event *e = &producer_events[i];
    if (i == producer_used) {
        producer_used++;
        name_copy(e->name, t->name); e->id = t->id;
        e->boosts = e->over_budget = 0; e->cycles = 0;
    }
    e->boosts += boosts; e->over_budget += over_budget; e->cycles += cycles;
    irq_restore(f);
}
void cpu_trap_enter(const struct trap_frame *tf)
{
    if (g_current && ((tf->cs & 3) == 3 || (tf->eflags & (1u << 17)))) {
        g_current->stretch_start = stamp();
        g_current->stretch_sampled = false; g_current->active_stretch_eip = 0;
    }
}
void cpu_user_return(void)
{
    if (!g_current) return;
    stretch_end(g_current, stamp(), g_current->active_stretch_eip);
    g_current->stretch_start = 0;
}
uint64_t cpu_irq_enter(const struct trap_frame *tf)
{
    uint64_t now = stamp();
    if (tf->vector == 0x20) pit_late(tf, now);
    if (g_current) charge(g_current, now);
    if (g_current && tf->vector == 0x20) {
        if ((tf->cs & 3) == 3 || (tf->eflags & (1u << 17))) g_current->user_ticks++;
        else {
            g_current->kernel_ticks++;
            stretch_end(g_current, now, tf->eip);
        }
    }
    return now;
}
void cpu_irq_leave(uint64_t start)
{
    if (!g_current) return;
    uint64_t now = stamp();
    irq_cycles += now - start;
    g_current->account_start = now; /* IRQ time never enters the task counters */
    if (g_need_resched && !resched_pending) { resched_pending = true; resched_start = now; }
}
uint64_t cpu_path_clock(void)
{
    uint32_t f = irq_save();
    uint64_t now = stamp(), value = now;
    if (g_current) value = g_current->run_cycles + now - g_current->account_start;
    irq_restore(f);
    return value;
}
void cpu_path_add(enum cpu_path path, uint64_t cycles)
{
    uint32_t f = irq_save();
    if (path < CPU_PATHS) paths[path] += cycles;
    irq_restore(f);
}
void cpu_stall_record(const char *s, size_t n)
{
    /* Only a complete f2-61 diagnostic prefix can force a record. Input is
     * bounded, not NUL terminated. Never emit recursively inside a write. */
    static const char prefix[] = "[desktop] stall";
    for (size_t i = 0; i + sizeof(prefix) - 1 <= n; i++)
        if (!memcmp(s + i, prefix, sizeof(prefix) - 1)) {
            uint32_t f = irq_save();
            forced = true;
            if (stat_task && stat_task->state == T_BLOCKED) {
                stat_task->wake_tick = 0;
                task_start(stat_task);
                g_need_resched = true;
            }
            irq_restore(f);
            break;
        }
}
struct top { char name[16]; uint32_t id; uint64_t cycles, kernel_ticks, user_ticks, stretch; };
static void cpu_emit(void)
{
    struct top top[5] = {0};
    struct overrun_event events[16], audio_overruns[4];
    struct inherit_event inherits[8];
    struct producer_event producers[8];
    struct sched_share_stats share_now, share_delta;
    struct { char name[16]; uint32_t id; uint64_t cycles; } device_top = { "none", 0, 0 };
    uint64_t counts[P_COUNT], path[CPU_PATHS];
    uint32_t f = irq_save();
    uint64_t now = stamp(), total = now - window_start;
    if (!total) { irq_restore(f); return; }
    struct irqoff_long long_section=irqoff_window;
    irqoff_window.cycles=0;
    bool logged=!!(g_boot.flags & (CBI_F_VERBOSE | CBI_F_DIAG_VERBOSE | CBI_F_TEST_REQUEST));
    bool automatic=threshold && (!automatic_window || now-automatic_window>=60000*per_ms());
    bool emit = forced || automatic || logged;
    if (automatic && !logged) automatic_window=now;
    if (g_current) {
        charge(g_current, now);
    }
    memcpy(counts, classes, sizeof(counts));
    struct task *task_list = sched_tasks();
    if (crit_label) crit_label("cpu_window");
    uint64_t irq_time = irq_cycles, exited_time = exited_cycles;
    uint64_t stretch_time = stretch_max, latency_time = resched_max;
    uint32_t sw = switches, ov = overruns, boosts = g_starvation_boosts - boost_base;
    struct sched_stats sched_now;
    sched_snapshot(&sched_now);
    struct sched_stats sched_delta = {
        .input_granted = sched_now.input_granted - sched_base.input_granted,
        .input_expired = sched_now.input_expired - sched_base.input_expired,
        .io_granted = sched_now.io_granted - sched_base.io_granted,
        .io_expired = sched_now.io_expired - sched_base.io_expired,
        .starvation_granted = sched_now.starvation_granted - sched_base.starvation_granted,
        .starvation_expired = sched_now.starvation_expired - sched_base.starvation_expired,
        .budget_exhausted = sched_now.budget_exhausted - sched_base.budget_exhausted,
        .audio_urgent = sched_now.audio_urgent - sched_base.audio_urgent,
        .wakeups = sched_now.wakeups - sched_base.wakeups
    };
    sched_base = sched_now;
    uint32_t site = stretch_eip, sid = stretch_id;
    char name[16], late[16];
    name_copy(name, stretch_name); name_copy(late, resched_name);
    unsigned event_count = overrun_used, lost = overrun_lost;
    memcpy(events, overrun_events, event_count * sizeof(*events));
    overrun_used = overrun_lost = 0;
    unsigned audio_count = audio_used, audio_dropped = audio_lost;
    memcpy(audio_overruns, audio_events, audio_count * sizeof(*audio_overruns));
    audio_used = audio_lost = 0;
    unsigned inherit_count = inherit_used, inherit_dropped = inherit_lost;
    memcpy(inherits, inherit_events, inherit_count * sizeof(*inherits));
    inherit_used = inherit_lost = 0;
    uint64_t firmware_us = firmware_window * 1000 / per_ms(), firmware_max_us = firmware_max * 1000 / per_ms();
    uint32_t firmware_n = firmware_count;
    firmware_window = firmware_max = 0; firmware_count = 0;
    unsigned producer_count = producer_used, producer_dropped = producer_lost;
    memcpy(producers, producer_events, producer_count * sizeof(*producers));
    producer_used = producer_lost = 0;
    sched_share_snapshot(&share_now);
    share_delta = (struct sched_share_stats){ share_now.turns - share_base.turns,
        share_now.lent_turns - share_base.lent_turns, share_now.turn_cycles - share_base.turn_cycles,
        share_now.group_cycles - share_base.group_cycles };
    share_base = share_now;
    memcpy(path, paths, sizeof(path));
    window_start = now; irq_cycles = exited_cycles = 0;
    memset(classes, 0, sizeof(classes)); memset(paths, 0, sizeof(paths));
    stretch_max = resched_max = 0; switches = overruns = 0;
    stretch_eip = stretch_id = 0;
    name_copy(stretch_name, "none");
    name_copy(resched_name, "none");
    boost_base = g_starvation_boosts; forced = threshold = false;
    irq_restore(f);
    /* No scheduling point in this traversal: kernel task-list lifetime is
     * stable until we yield. IRQs may update counters, so snapshot/reset one
     * task at a time and sort only after restoring IF. */
    diagnostic_active=emit;
    for (struct task *t = task_list; t; t = t->all_next) {
        f = irq_save();
        if (crit_label) crit_label("cpu_task");
        uint64_t d = t->run_cycles - t->window_base;
        struct top sample = { .id = t->id, .cycles = d,
            .kernel_ticks = t->kernel_ticks - t->kernel_base,
            .user_ticks = t->user_ticks - t->user_base, .stretch = t->max_stretch };
        name_copy(sample.name, t->name); sample.name[8] = 0;
        bool is_idle = t->prio == P_IDLE;
        struct { unsigned class, base, effective, input, io, inherited, starvation, io_ticks;
                 uint64_t expiry; } priority = {
            t->prio, t->base_priority, t->effective_priority, t->input_priority,
            t->io_priority, t->inherited_priority, t->boosted, t->io_ticks, t->input_expiry
        };
        t->window_base = t->run_cycles; t->kernel_base = t->kernel_ticks; t->user_base = t->user_ticks;
        uint64_t device = t->device_cycles - t->device_base;
        t->device_base = t->device_cycles;
        t->max_stretch = 0; t->stretch_eip = 0;
        if (t->stretch_start) t->stretch_start = now;
        t->stretch_sampled = false; t->active_stretch_eip = 0;
        irq_restore(f);
        if (device > device_top.cycles) {
            device_top.cycles = device; device_top.id = sample.id;
            name_copy(device_top.name, t->name);
        }
        if (emit && !is_idle)
            klog_periodic("[cpu] task=%s#%u class=%u base=%u effective=%u input=%u io=%u inherited=%u starvation=%u input_until=%llu io_ticks=%u",
                sample.name, sample.id, priority.class, priority.base, priority.effective,
                priority.input, priority.io, priority.inherited, priority.starvation,
                priority.expiry, priority.io_ticks);
        if (!is_idle) for (unsigned i = 0; i < ARRAY_SIZE(top); i++) {
            if (d <= top[i].cycles) continue;
            for (unsigned j = ARRAY_SIZE(top) - 1; j > i; j--) top[j] = top[j - 1];
            top[i] = sample;
            break;
        }
    }
    if (long_section.cycles && g_tsc_per_ms) {
        static const char *const phases[CPU_IRQOFF_PHASES]={"other","pick","fpu","address_space",
            "ldt/debug","vm-enter","other","queue","proc","resume","irq-entry","irq-handler",
            "irq-eoi","irq-exit","tick-timing","tick-sched","tick-timers","tick-serial"};
        klog_periodic("[kernel] irqoff_long site=%08x us=%llu phase=%s phase_us=%llu from=%s to=%s",
            long_section.site,long_section.cycles*1000/g_tsc_per_ms,
            long_section.gap ? "gap" : phases[long_section.phase],
            long_section.phase_cycles*1000/g_tsc_per_ms,long_section.from,long_section.to);
    }
    if (smi_window_emit) smi_window_emit(total, emit);
    if (thinkpad_ec_window_emit) thinkpad_ec_window_emit();
    /* f2-96: firmware time left out of the scheduler budgets this window
     * (every window that had some, like the EC traffic record). */
    if (firmware_n)
        klog_periodic("[sched] firmware_credit count=%u us=%llu max_us=%llu", firmware_n,
            (unsigned long long)firmware_us, (unsigned long long)firmware_max_us);
    /* fx-5 follow-up: continued lock-order reports (sync.h), in every window
     * that added some and in every published window afterwards. */
    if (ksync_order_snapshot) {
        static uint32_t order_published;
        struct ksync_order_stats order; ksync_order_snapshot(&order);
        if (order.violations && (emit || order.violations != order_published)) {
            order_published = order.violations;
            klog_periodic("[kernel] lock_order violations=%u pairs=%u dropped=%u",
                order.violations, order.pairs, order.dropped);
        }
    }
    if (!emit) return; /* diagnostic_active is already false */
    struct timing_stats timing; timing_snapshot(&timing);
    struct bootlog_stats log; bootlog_snapshot(&log);
    struct kwork_stats work; kwork_snapshot(&work);
    /* Wide divisions and formatting stay outside the IRQ-off snapshot. */
    unsigned idle = pm(counts[P_IDLE], total), irq = pm(irq_time, total);
    unsigned device = pm(counts[P_DEVICE], total), interactive = pm(counts[P_INTERACTIVE], total);
    unsigned exited = pm(exited_time, total);
    uint32_t window = ms(total), stretch = ms(stretch_time), latency = ms(latency_time);
    unsigned normal = 1000 - idle - irq - device - interactive;
    klog_periodic("[cpu] window_ms=%u idle_pm=%u irq_pm=%u device_pm=%u interactive_pm=%u normal_pm=%u switches=%u boosts=%u overruns=%u exited_pm=%u",
        window, idle, irq, device, interactive, normal, sw, boosts, ov, exited);
    klog_periodic("[cpu] boosts_input=%u/%u boosts_io=%u/%u boosts_starvation=%u/%u device_budget=%u audio_urgent=%u wakeups=%u",
        sched_delta.input_granted, sched_delta.input_expired, sched_delta.io_granted, sched_delta.io_expired,
        sched_delta.starvation_granted, sched_delta.starvation_expired, sched_delta.budget_exhausted,
        sched_delta.audio_urgent, sched_delta.wakeups);
    char line[230]; unsigned at = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(top) && top[i].cycles; i++)
    {
        unsigned share = pm(top[i].cycles, total);
        uint64_t k = top[i].kernel_ticks, samples = k + top[i].user_ticks;
        unsigned kernel = samples ? (unsigned)(share * k / samples) : 0;
        unsigned n = (unsigned)ksnprintf(line + at, sizeof(line) - at, "%s%s#%u:%uk%us%u",
            i ? "," : "", top[i].name, top[i].id, share, kernel, ms(top[i].stretch));
        if (n >= sizeof(line) - at) { at = sizeof(line) - 1; break; }
        at += n;
    }
    if (!at) line[0] = 0;
    klog_periodic("[cpu] top=%s", line);
    /* f1-90: the largest consumer of the shared device budget this window. */
    klog_periodic("[cpu] device_top task=%s#%u us=%llu", device_top.name, device_top.id,
        (unsigned long long)(device_top.cycles * 1000 / per_ms()));
    klog_periodic("[kernel] stretch_max_ms=%u task=%s#%u eip=%08x resched_lat_max_ms=%u resched_task=%s irqoff_max_us=%u timer_gaps=%u/%u work_us=%u",
        stretch, name, sid, site, latency, late, timing.critical_us, timing.gaps_software, timing.gaps_unclassified, timing.critical_work_us);
    klog_periodic("[kernel] irqoff_section=%s site=%08x irqoff_max_us=%u violations=%u work_us=%u firmware_us=%u unclassified=%u owner=%s",
        timing.critical_section ? timing.critical_section : "none", timing.critical_site,
        timing.critical_us, timing.budget_violations, timing.raw_work_us,
        timing.raw_firmware_us, timing.unclassified_irqoff, timing.interrupt_owner[0] ? timing.interrupt_owner : "none");
    klog_periodic("[cpu] irqoff raw_max_us=%u work_max_us=%u firmware_max_us=%u smi_attributed=%u unclassified=%u task=%s#%u worker_site=%08x",
        timing.critical_us, timing.critical_work_us, timing.firmware_us,
        timing.firmware_gaps, timing.unclassified_irqoff,
        timing.unclassified_task[0] ? timing.unclassified_task : "none",
        timing.unclassified_task_id,timing.unclassified_worker_site);
    klog_periodic("[cpu] irqwork max_us=%u owner=%s site=%08x section=%s",
        timing.critical_work_us, timing.work_owner[0] ? timing.work_owner : "none", timing.work_site,
        timing.work_section ? timing.work_section : "none");
    klog_periodic("[cpu] deferred max_turn_us=%llu max_service_us=%llu coalesced=%llu turn_violations=%llu service_violations=%llu",
        work.max_run_us, work.max_service_us, work.coalesced, work.budget_violations, work.service_violations);
    klog_periodic("[kernel] pic_mask writes=%u out_max_us=%u",
        timing.pic_mask_writes, timing.pic_mask_out_max_us);
    klog_periodic("[kernel] long_paths serial_spin_ms=0 serial_drop=%u console_repaint_ms=%u bootlog_drain_ms=%u bootlog_queued=%u elf_load_ms=%u zero_pages_ms=%u work_max_us=%llu",
        serial_dropped(), ms(path[CPU_CONSOLE]), ms(path[CPU_BOOTLOG]), log.queued, ms(path[CPU_ELF]), ms(path[CPU_ZERO]), work.max_run_us);
    klog_periodic("[kernel] bootlog writes=%llu bytes=%llu metadata=%llu rotations=%u dropped=%u",
        log.writes, log.bytes_written, log.metadata_writes, log.rotations, log.dropped);
    klog_periodic("[kernel] bootlog dropped_protected=%u dropped_verbose=%u records_protected=%u records_verbose=%u",
        log.dropped_protected, log.dropped_verbose, log.records_dropped_protected, log.records_dropped_verbose);
    klog_periodic("[kernel] bootlog dropped_statistic=%u records_statistic=%u budget_dropped_lines=%u budget_windows=%u",
        log.dropped_statistic, log.records_dropped_statistic, log.budget_dropped_lines, log.budget_windows);
    for (unsigned i = 0; i < event_count; i++)
        klog_periodic("[sched] device_overrun task=%s#%u count=%u consumed_us=%u",
            events[i].name, events[i].id, events[i].count, events[i].consumed_us);
    if (lost) klog_periodic("[sched] device_overrun lost=%u", lost);
    for (unsigned i = 0; i < audio_count; i++)
        klog_periodic("[sched] audio_refill_overrun task=%s#%u count=%u consumed_us=%u",
            audio_overruns[i].name, audio_overruns[i].id, audio_overruns[i].count, audio_overruns[i].consumed_us);
    if (audio_dropped) klog_periodic("[sched] audio_refill_overrun lost=%u", audio_dropped);
    for (unsigned i = 0; i < inherit_count; i++)
        klog_periodic("[sched] inherit from=%s#%u to=%s#%u level=%u reason=%s count=%u",
            inherits[i].from, inherits[i].from_id, inherits[i].to, inherits[i].to_id, inherits[i].level,
            inherits[i].reason == KSYNC_DEPEND_CACHE ? "cache" : "drive", inherits[i].count);
    if (inherit_dropped) klog_periodic("[sched] inherit lost=%u", inherit_dropped);
    /* f1-101: producers raised for a stream behind its lead, and withheld
     * boosts once the shared 20 %/100 ms producer budget was spent. */
    for (unsigned i = 0; i < producer_count; i++)
        klog_periodic("[sched] audio_producer task=%s#%u boosts=%u over_budget=%u boost_us=%llu",
            producers[i].name, producers[i].id, producers[i].boosts, producers[i].over_budget,
            (unsigned long long)(producers[i].cycles * 1000 / per_ms()));
    if (producer_dropped) klog_periodic("[sched] audio_producer lost=%u", producer_dropped);
    /* f1-98: the desktop group's CPU and the turns its minimum share gave it. */
    if (share_delta.group_cycles || share_delta.turns)
        klog_periodic("[sched] desktop_share group_pm=%u turns=%u lent_turns=%u turn_ms=%u",
            pm(share_delta.group_cycles, total), share_delta.turns, share_delta.lent_turns,
            ms(share_delta.turn_cycles));
    diagnostic_active=false;
}
static void cpu_worker(void *arg)
{
    (void)arg;
    for (;;) { task_sleep_ms(1000); ledger_poll(g_ticks); if (reclaim_poll) reclaim_poll(g_ticks);
        if (pager_poll) pager_poll(g_ticks);
        if (display_fullscreen_stall_poll) display_fullscreen_stall_poll();
        task_sleep_ms(1000); ledger_poll(g_ticks); if (reclaim_poll) reclaim_poll(g_ticks); cpu_emit();
        if (display_fullscreen_stall_poll) display_fullscreen_stall_poll(); }
}
void cpu_init(void)
{
    window_start = stamp();
    automatic_window=0;
    diagnostic_active=false;
    if (reclaim_init) reclaim_init();
    if (pager_init) pager_init(); /* workers; the store attaches with storage */
    stat_task = task_create_kernel("cpustat", cpu_worker, 0, P_DEVICE);
    if (!stat_task) panic("cpu: cannot create cpustat");
    task_start(stat_task);
}
