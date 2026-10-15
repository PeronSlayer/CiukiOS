/* Production critical-section accounting with a deterministic TSC. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CIUKI_CPU_H
#include <ciuki/kernel.h>
#include <ciuki/timing.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"timing:%d: %s\n",__LINE__,#c);exit(1); } } while (0)
bool g_cpu_tsc=true;
volatile uint64_t g_ticks;
static uint64_t cycles=1;
static uint32_t flags=0x200;
static uint64_t rdtsc(void) { return cycles; }
static uint32_t irq_save(void) { uint32_t f=flags;flags=0;if (f&0x200) crit_begin();return f; }
static void irq_restore(uint32_t f) { if (f&0x200) crit_end();flags=f; }
static void hlt(void) { cycles+=1000;g_ticks++; }
#include "latency_timing.inc"
int main(void)
{
    g_tsc_per_ms=1000;
    uint32_t f=irq_save();crit_label("cpu_task");cycles+=50;irq_restore(f);
    struct timing_stats s;timing_snapshot(&s);
    CHECK(s.critical_us==50 && !strcmp(s.critical_section,"cpu_task") && s.critical_site);
    f=irq_save();crit_label("irq11");cycles+=4230;irq_restore(f);
    timing_snapshot(&s);
    CHECK(s.critical_us==4230 && s.budget_violations==1 && !strcmp(s.critical_section,"irq11"));
    cycles+=1000;timing_timer_irq();cycles+=3000;timing_timer_irq();
    timing_snapshot(&s);CHECK(s.gaps_unclassified==1);
    f=irq_save();crit_label("serial_queue");cycles+=2500;irq_restore(f);
    timing_timer_irq();timing_snapshot(&s);CHECK(s.gaps_software==1);
    timing_reset();cycles+=1000;timing_timer_irq();cycles+=1000;timing_timer_irq();
    timing_snapshot(&s);CHECK(!s.critical_us && !s.budget_violations && !s.gaps_software && !s.gaps_unclassified);
    /* Repeated bounded sections permit intervening timer service. */
    for (unsigned i=0;i<100;i++) {
        f=irq_save();crit_label("cpu_task");cycles+=100;irq_restore(f);
        cycles+=900;timing_timer_irq();
    }
    timing_snapshot(&s);CHECK(s.critical_us==100 && !s.budget_violations && !s.gaps_software && !s.gaps_unclassified);
    g_cpu_tsc=false;g_tsc_per_ms=0;timing_snapshot(&s);CHECK(!s.critical_us);
    puts("kernel timing: longest section/address, 4230us reproduction, software/external gaps, reset and <=500us sections with zero gaps PASS");
    return 0;
}
