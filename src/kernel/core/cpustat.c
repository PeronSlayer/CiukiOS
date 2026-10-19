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
}
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
    next->account_prio = next->prio;
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
        t->window_base = t->run_cycles; t->kernel_base = t->kernel_ticks; t->user_base = t->user_ticks;
        t->max_stretch = 0; t->stretch_eip = 0;
        if (t->stretch_start) t->stretch_start = now;
        t->stretch_sampled = false; t->active_stretch_eip = 0;
        irq_restore(f);
        if (!is_idle) for (unsigned i = 0; i < ARRAY_SIZE(top); i++) {
            if (d <= top[i].cycles) continue;
            for (unsigned j = ARRAY_SIZE(top) - 1; j > i; j--) top[j] = top[j - 1];
            top[i] = sample;
            break;
        }
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
    klog_periodic("[kernel] stretch_max_ms=%u task=%s#%u eip=%08x resched_lat_max_ms=%u resched_task=%s irqoff_max_us=%u timer_gaps=%u/%u",
        stretch, name, sid, site, latency, late, timing.critical_us, timing.gaps_software, timing.gaps_unclassified);
    klog_periodic("[kernel] irqoff_section=%s site=%08x irqoff_max_us=%u violations=%u",
        timing.critical_section ? timing.critical_section : "none", timing.critical_site,
        timing.critical_us, timing.budget_violations);
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
    for (;;) { task_sleep_ms(1000); ledger_poll(g_ticks);
        task_sleep_ms(1000); ledger_poll(g_ticks); cpu_emit(); }
}
void cpu_init(void)
{
    window_start = stamp();
    stat_task = task_create_kernel("cpustat", cpu_worker, 0, P_DEVICE);
    if (!stat_task) panic("cpu: cannot create cpustat");
    task_start(stat_task);
}
