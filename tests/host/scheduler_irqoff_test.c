/* Real schedule -> timing -> cpustat hooks, only EFLAGS/TSC/stack fake.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main latency_fixture_main
#include "kernel_latency_test.c"
#undef main
#undef crit_begin
#undef crit_end
#define crit_begin timing_crit_begin
#define crit_end timing_crit_end
#define timing_snapshot real_timing_snapshot
static uint64_t rdtsc(void) { return cycles; }
static void hlt(void) { abort(); }
bool g_cpu_tsc=true;
#include "latency_timing.inc"
#undef timing_snapshot

int main(void) {
    reset(); timing_begin_hook=timing_crit_begin; timing_end_hook=timing_crit_end;
    flags=0; rq_push(&normal); fpu_cost=1600;
    schedule();
    CHECK(!flags && !crit_start && !irqoff.start);
    CHECK(irqoff_window.cycles==1600 && irqoff_window.phase==CPU_IRQOFF_FPU && !irqoff_window.gap);
    CHECK(!strcmp(irqoff_window.from,"idle") && !strcmp(irqoff_window.to,"worker"));
    cpu_emit(); CHECK(strstr(records,"us=1600 phase=fpu phase_us=1600 from=idle to=worker\n"));
    /* An existing caller owns the outer section: no restart at schedule. */
    reset(); uint32_t f=irq_save(); crit_label("outer");
    uint64_t outer_start=crit_start; uint32_t outer_site=crit_site;
    cycles+=100; rq_push(&normal); fpu_cost=1600; schedule();
    CHECK(!flags && crit_start && irqoff.start);
    CHECK(irqoff_window.cycles==1700 && irqoff_window.site==outer_site);
    CHECK(irqoff_window.cycles==cycles-outer_start && irqoff_window.phase_cycles==1600);
    irq_restore(f); CHECK(flags==0x200 && !crit_start && !irqoff.start);
    /* Same-task return also retires only a section created at this entry. */
    reset(); flags=0; schedule(); CHECK(!flags && !crit_start && !irqoff.start);
    reset(); f=irq_save(); outer_start=crit_start; schedule();
    CHECK(crit_start==outer_start && irqoff.start==outer_start && !flags);
    irq_restore(f); CHECK(!crit_start && !irqoff.start);
    /* Begin/end without phases must leave the stale raw slots untouched. */
    reset(); irqoff.marks[7].stamp=UINT64_MAX;
    f=irq_save(); CHECK(!irqoff.count && irqoff.marks[7].stamp==UINT64_MAX);
    cycles+=999; irq_restore(f); CHECK(!irqoff_window.cycles);
    CHECK(!irqoff.count && irqoff.marks[7].stamp==UINT64_MAX);
    /* Capturing stores raw stamps only, including the sub-ms fast path. */
    f=irq_save(); uint64_t start=cycles;
    cpu_irqoff_mark(CPU_IRQOFF_PICK); cycles+=499;
    cpu_irqoff_mark(CPU_IRQOFF_FPU); cycles+=500;
    CHECK(irqoff.count==2 && irqoff.marks[0].stamp==start && irqoff.marks[1].stamp==start+499);
    CHECK(!irqoff_window.cycles); irq_restore(f); CHECK(!irqoff_window.cycles);
    f=irq_save(); cpu_irqoff_mark(CPU_IRQOFF_PICK); cycles+=500;
    cpu_irqoff_mark(CPU_IRQOFF_FPU); cycles+=500;
    irq_restore(f); CHECK(irqoff_window.cycles==1000 && irqoff_window.phase_cycles==500);
    /* A bounded mark overflow leaves its tail unclassified, never an OOB
     * access or a fabricated long duration for the final recorded phase. */
    irqoff_window.cycles=0; f=irq_save();
    for (unsigned i=0;i<20;i++) { cpu_irqoff_mark(CPU_IRQOFF_PICK); cycles+=10; }
    cycles+=2000; irq_restore(f);
    CHECK(irqoff.count==9 && irqoff_window.gap && irqoff_window.phase_cycles==2130);
    puts("PASS f1-86 real scheduler/timing/cpustat hooks: IF-clear inactive entry, existing outer owner/site retained, switch and same-task cleanup, IF preserved");
    puts("PASS f1-86 deferred phases: raw timestamps only, no clearing unused slots, 999/1000us threshold, bounded overflow tail");
    return 0;
}
