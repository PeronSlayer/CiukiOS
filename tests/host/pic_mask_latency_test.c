/* Production PIC masks and timing with a clocked ISA/PIT boundary.
 * The injected costs are a host model, never measured T23 latencies. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CIUKI_CPU_H
#include <ciuki/kernel.h>
#include <ciuki/task.h>
#include <ciuki/timing.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"pic latency:%d: %s\n",__LINE__,#c); exit(1); } } while (0)
bool g_cpu_tsc = true;
struct task *g_current;
volatile uint64_t g_ticks;
static uint64_t cycles = 1, next_pit;
static uint32_t flags = 0x200;
static unsigned writes, section_writes, max_writes, missed;
static bool pending;
static uint64_t rdtsc(void) { return cycles; }
uint64_t ktime_cycles(void) { return cycles; }
static uint64_t read_tsc_serialized(void) { return cycles; }
static void advance(unsigned us)
{
    uint64_t end = cycles + us * 1000ull;
    while (next_pit && next_pit <= end) {
        cycles = next_pit; next_pit += 1000000;
        if (flags & 0x200) { g_ticks++; timing_timer_irq(); }
        else { if (pending) missed++; pending = true; }
    }
    cycles = end;
}
static uint32_t read_eflags(void) { return flags; }
static uint32_t irq_save(void)
{
    uint32_t f = flags; flags = 0;
    if (f & 0x200) { section_writes = 0; crit_begin(); }
    advance(1); return f;
}
static void irq_restore(uint32_t f)
{
    advance(1);
    if (f & 0x200) {
        crit_end(); flags = f;
        if (section_writes > max_writes) max_writes = section_writes;
        if (pending) { pending = false; g_ticks++; timing_timer_irq(); }
    }
}
static void hlt(void) { advance(1000); }
static void outb(uint16_t port, uint8_t value)
{
    (void)value; CHECK(!flags && (port == 0x21 || port == 0xa1));
    writes++; section_writes++; advance(5);
}
#include "latency_timing.inc"
#include "latency_pic.inc"
int main(void)
{
    g_tsc_per_ms = 1000000;
    pic_unmask(11); CHECK(writes == 2 && max_writes == 1);
    pic_unmask(11); CHECK(writes == 2); /* no repeated slave/master OUT */
    pic_mask(11); CHECK(writes == 3);
    pic_mask(11); pic_mask(16); pic_unmask(31); CHECK(writes == 3);
    struct timing_stats s; timing_snapshot(&s);
    CHECK(s.pic_mask_writes == 3 && s.pic_mask_out_max_us == 5 && s.critical_us == 7);
    puts("PIC: one OUT/transaction, cascade, no-op/invalid masks, TSC OUT=5us critical=7us PASS");

    timing_reset(); next_pit = cycles + 1000000;
    uint64_t end = cycles + 10000000000ull;
    while (cycles < end) {
        if (end - cycles < 128000) { advance((unsigned)((end - cycles) / 1000)); break; }
        pic_mask(11); pic_unmask(11);
        /* Replay the longest admitted non-I/O section across every PIT
         * phase, and thread work spanning multiple ticks with IF enabled. */
        uint32_t f = irq_save(); crit_label("audit_worst_90us"); advance(88); irq_restore(f);
        unsigned remaining = (unsigned)((end - cycles) / 1000);
        advance(remaining > 2713 ? 2713 : remaining);
        if (end - cycles < 1000) cycles = end;
    }
    timing_snapshot(&s);
    CHECK(g_ticks == 10000 && s.pit_irqs == 10000 && !missed && !pending);
    CHECK(!s.gaps_software && !s.gaps_unclassified && s.critical_us == 90 && !s.budget_violations);
    printf("IRQ audit model: 10 s ticks=%llu lost=%u gaps=%u/%u max=%uus section=%s PASS\n",
        (unsigned long long)s.pit_irqs, missed, s.gaps_software, s.gaps_unclassified,
        s.critical_us, s.critical_section);
    /* Negative control: the original 4.23 ms stall loses edges and remains
     * visible. Do not manufacture catch-up ticks or hide external gaps. */
    uint32_t f = irq_save(); crit_label("old_stall"); advance(4230); irq_restore(f);
    timing_snapshot(&s);
    CHECK(missed >= 3 && s.gaps_software && s.critical_us >= 4230);
    puts("IRQ audit model: 4.23ms negative control loses PIT edges and reports software gap PASS");
    return 0;
}
