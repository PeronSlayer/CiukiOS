/* F0-20: bounded, allocation-free verbose attribution. No IRQ logging.
 * Decisions and limitations: docs/validation/f0-20-system-stalls.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/timing.h>
#include <ciuki/sync.h>
#include <ciuki/abi.h>

#define CPU_SLOTS (CIUKI_THREAD_MAX + 64u)
#define WINDOW_MS 2000u
struct cpu_bucket { uint64_t cycles; uint32_t id, generation; char name[16]; };
static struct cpu_bucket buckets[CPU_SLOTS];
static unsigned bucket_count;
static uint32_t epoch = 1;
static uint64_t unit, last_cpu, window_start, stretch_start, idle_cycles;
static uint64_t path_cycles[LONG_COUNT], max_single;
static const char *max_site;
static bool initialized, emitting;

static uint64_t now_cycles(void)
{
    if (g_cpu_tsc && g_tsc_per_ms) {
        return ktime_cycles();
    }
    return g_ticks;
}
static void clear_window(uint64_t now)
{
    bucket_count = 0;
    if (!++epoch) ++epoch;
    memset(path_cycles, 0, sizeof(path_cycles));
    idle_cycles = max_single = 0; max_site = "none";
    last_cpu = window_start = stretch_start = now;
}
static bool enabled(void)
{
    if (!(g_boot.flags & CBI_F_VERBOSE) || !g_current) return false;
    uint64_t scale = g_cpu_tsc && g_tsc_per_ms ? g_tsc_per_ms : 1;
    if (!initialized || unit != scale) {
        unit = scale; clear_window(now_cycles()); initialized = true;
    }
    return true;
}
static struct cpu_bucket *bucket(struct task *t)
{
    if (t->attribution_epoch == epoch) return &buckets[t->attribution_slot];
    unsigned slot;
    for (slot = 0; slot < bucket_count; slot++)
        if (buckets[slot].id == t->id && buckets[slot].generation == t->generation) break;
    if (slot == bucket_count && slot < CPU_SLOTS - 1) {
        struct cpu_bucket *b = &buckets[bucket_count++];
        *b = (struct cpu_bucket){.id=t->id, .generation=t->generation};
        for (unsigned i=0; i<sizeof(b->name)-1 && t->name[i]; i++) {
            char c=t->name[i];
            b->name[i] = c>' ' && c<127 && c!=',' && c!=':' ? c : '_';
        }
    } else if (slot == CPU_SLOTS - 1) {
        if (bucket_count < CPU_SLOTS) {
            buckets[bucket_count++] = (struct cpu_bucket){.id=UINT32_MAX, .name="other"};
        }
    }
    /* The last slot aggregates excess identities, including short-lived tasks.
     * Recorded names/IDs survive task reaping; no task pointer is retained. */
    if (slot >= CPU_SLOTS) slot = CPU_SLOTS - 1;
    t->attribution_epoch = epoch; t->attribution_slot = slot;
    return &buckets[slot];
}
static void account(uint64_t now)
{
    uint64_t delta = now - last_cpu;
    if (g_current->prio == P_IDLE) idle_cycles += delta;
    else bucket(g_current)->cycles += delta;
    unsigned path = g_current->attribution_path;
    if (path && path < LONG_COUNT) {
        path_cycles[path] += delta;
        uint64_t stretch = now - stretch_start;
        if (stretch > max_single) { max_single = stretch; max_site = g_current->attribution_site; }
    }
    last_cpu = now;
}
void attribution_switch(struct task *next)
{
    if (!enabled()) return;
    uint64_t now = now_cycles(); account(now);
    if (next != g_current) stretch_start = now;
}
struct long_path_scope attribution_enter(unsigned path, const char *site)
{
    struct long_path_scope previous = {0};
    if (!enabled()) return previous;
    uint32_t flags = irq_save();
    uint64_t now = now_cycles(); account(now);
    previous = (struct long_path_scope){g_current->attribution_path, g_current->attribution_site};
    g_current->attribution_path = path; g_current->attribution_site = site;
    if (!previous.path) stretch_start = now;
    irq_restore(flags); return previous;
}
void attribution_leave(struct long_path_scope previous)
{
    if (!enabled()) return;
    uint32_t flags = irq_save();
    uint64_t now = now_cycles(); account(now);
    g_current->attribution_path = previous.path; g_current->attribution_site = previous.site;
    if (!previous.path) stretch_start = now;
    irq_restore(flags);
}
void attribution_poll(bool force)
{
    if (!enabled() || emitting || !(read_eflags() & 0x200)) return;
    uint64_t now = now_cycles();
    if (!force && now - window_start < WINDOW_MS * unit) return;
    struct cpu_bucket top[5] = {0};
    uint64_t elapsed, idle, paths[LONG_COUNT], maximum;
    const char *site;
    uint32_t flags = irq_save();
    account(now); elapsed = now - window_start;
    if (!force && max_single <= 20u * unit) {
        clear_window(now); irq_restore(flags); return;
    }
    idle = idle_cycles; maximum = max_single; site = max_site;
    memcpy(paths, path_cycles, sizeof(paths));
    for (unsigned i=0; i<bucket_count; i++) {
        for (unsigned j=0; j<5; j++) if (buckets[i].cycles > top[j].cycles) {
            for (unsigned k=4; k>j; k--) top[k] = top[k-1];
            top[j] = buckets[i]; break;
        }
    }
    clear_window(now); emitting = true; irq_restore(flags);
    char names[176]; unsigned used = 0;
    for (unsigned i=0; i<5 && top[i].cycles; i++) {
        used += (unsigned)ksnprintf(names+used, sizeof(names)-used, "%s%s#%u:%u",
            i ? "," : "", top[i].name, top[i].id,
            elapsed ? (uint32_t)(top[i].cycles * 1000 / elapsed) : 0);
    }
    if (!used) memcpy(names, "none", 5);
    klog_deferred("[cpu] window_ms=%llu idle=%u top=%s", elapsed/unit,
        elapsed ? (uint32_t)(idle*1000/elapsed) : 0, names);
    klog_deferred("[kernel] long_paths console_repaint_ms=%llu bootlog_drain_ms=%llu elf_load_ms=%llu max_single_ms=%llu site=%s",
        paths[LONG_CONSOLE]/unit, paths[LONG_BOOTLOG]/unit, paths[LONG_ELF]/unit,
        maximum/unit, site ? site : "none");
    emitting = false;
}
