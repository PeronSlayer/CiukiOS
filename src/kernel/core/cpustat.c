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

static struct task *stat_task;
static uint64_t window_start, irq_cycles, exited_cycles, classes[P_COUNT];
static uint64_t stretch_max, resched_start, resched_max, paths[CPU_PATHS];
static uint32_t stretch_eip, switches, overruns, boost_base;
static char stretch_name[16] = "none", resched_name[16] = "none";
static uint32_t stretch_id;
struct overrun_event { char name[16]; uint32_t id, count; };
static struct overrun_event overrun_events[16];
static unsigned overrun_used, overrun_lost;
static bool forced, threshold, resched_pending;
static struct sched_stats sched_base;
static struct {
    uint64_t start;
    uint32_t site;
    unsigned count;
    struct { uint64_t stamp; enum cpu_irqoff_phase phase; } marks[8];
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
void cpu_irqoff_end(uint64_t end)
{
    if (!irqoff.start) return;
    uint64_t d=end-irqoff.start;
    if (g_tsc_per_ms && end>=irqoff.start && d>=g_tsc_per_ms && d>irqoff_window.cycles) {
        uint64_t measured=0,largest=0;
        enum cpu_irqoff_phase phase=CPU_IRQOFF_NONE;
        unsigned count=irqoff.count;
        if (count>ARRAY_SIZE(irqoff.marks)) count=ARRAY_SIZE(irqoff.marks);
        /* Raw capture above has no phase subtraction, accumulation/ranking.
         * Only a qualifying section evaluates the bounded marks here. */
        for (unsigned i=0;i<count;i++) {
            if (irqoff.marks[i].phase==CPU_IRQOFF_NONE) continue;
            uint64_t a=irqoff.marks[i].stamp;
            if (i+1==count && irqoff.count>count) break;
            uint64_t b=i+1<count ? irqoff.marks[i+1].stamp : end;
            if (a<irqoff.start || b<a || b>end) continue;
            uint64_t elapsed=b-a; measured+=elapsed;
            if (elapsed>largest) { largest=elapsed; phase=irqoff.marks[i].phase; }
        }
        if (measured>d) measured=d;
        irqoff_window.cycles=d; irqoff_window.site=irqoff.site;
        irqoff_window.gap=measured<d/2+d%2;
        irqoff_window.phase=phase;
        irqoff_window.phase_cycles=irqoff_window.gap ? d-measured : largest;
        const struct task *from=irqoff.from ? irqoff.from : g_current;
        const struct task *to=irqoff.from ? irqoff.to : g_current;
        name_copy(irqoff_window.from,from ? from->name : "none");
        name_copy(irqoff_window.to,to ? to->name : "none");
    }
    irqoff.start=0;
}

static void stretch_end(struct task *t, uint64_t now, uint32_t eip)
{
    if (!t || t->prio == P_IDLE || !t->stretch_start) return;
    uint64_t d = now - t->stretch_start;
    if (d > 20 * per_ms() && !t->stretch_sampled) {
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
    t->run_cycles += d;
    classes[t->account_prio] += d;
    t->account_start = now;
    sched_charge_runtime(t, d, per_ms());
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
void cpu_overrun(struct task *t)
{
    t->account_prio = P_NORMAL; /* the IRQ entry already charged the device interval */
    overruns++; threshold = true;
    unsigned i = 0;
    while (i < overrun_used && overrun_events[i].id != t->id) i++;
    if (i == ARRAY_SIZE(overrun_events)) { overrun_lost++; return; }
    if (i == overrun_used) {
        overrun_used++;
        name_copy(overrun_events[i].name, t->name);
        overrun_events[i].id = t->id;
        overrun_events[i].count = 0;
    }
    overrun_events[i].count++;
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
    struct overrun_event events[16];
    uint64_t counts[P_COUNT], path[CPU_PATHS];
    uint32_t f = irq_save();
    uint64_t now = stamp(), total = now - window_start;
    if (!total) { irq_restore(f); return; }
    struct irqoff_long long_section=irqoff_window;
    irqoff_window.cycles=0;
    bool emit = forced || threshold || (g_boot.flags & (CBI_F_VERBOSE | CBI_F_DIAG_VERBOSE | CBI_F_TEST_REQUEST));
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
        t->max_stretch = 0; t->stretch_eip = 0;
        if (t->stretch_start) t->stretch_start = now;
        t->stretch_sampled = false; t->active_stretch_eip = 0;
        irq_restore(f);
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
        static const char *const phases[]={"other","pick","fpu","address_space","ldt/debug","vm-enter","other"};
        klog_periodic("[kernel] irqoff_long site=%08x us=%llu phase=%s phase_us=%llu from=%s to=%s",
            long_section.site,long_section.cycles*1000/g_tsc_per_ms,
            long_section.gap ? "gap" : phases[long_section.phase],
            long_section.phase_cycles*1000/g_tsc_per_ms,long_section.from,long_section.to);
    }
    if (!emit) return;
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
    for (unsigned i = 0; i < event_count; i++)
        klog_periodic("[sched] device_overrun task=%s#%u count=%u", events[i].name, events[i].id, events[i].count);
    if (lost) klog_periodic("[sched] device_overrun lost=%u", lost);
}
static void cpu_worker(void *arg)
{
    (void)arg;
    for (;;) { task_sleep_ms(1000); ledger_poll(g_ticks); if (reclaim_poll) reclaim_poll(g_ticks);
        if (pager_poll) pager_poll(g_ticks);
        task_sleep_ms(1000); ledger_poll(g_ticks); if (reclaim_poll) reclaim_poll(g_ticks); cpu_emit(); }
}
void cpu_init(void)
{
    window_start = stamp();
    if (reclaim_init) reclaim_init();
    if (pager_init) pager_init(); /* workers; the store attaches with storage */
    stat_task = task_create_kernel("cpustat", cpu_worker, 0, P_DEVICE);
    if (!stat_task) panic("cpu: cannot create cpustat");
    task_start(stat_task);
}
