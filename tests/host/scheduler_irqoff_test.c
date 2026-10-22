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
/* f1-94: every section of 1 ms or more reaches the SMI attribution. */
static struct smi_section offered; static unsigned offers;
void smi_section_end(const struct smi_section *x) { offered=*x; offers++; }
static uint64_t tick_now, tick_previous, adjacent[2];
void smi_tick(uint64_t now, uint64_t previous) { tick_now=now; tick_previous=previous; }
void smi_adjacent_gap(uint64_t a, uint64_t b) { adjacent[0]=a; adjacent[1]=b; }

int main(void) {
    reset(); timing_begin_hook=timing_crit_begin; timing_end_hook=timing_crit_end;
    flags=0; rq_push(&normal); fpu_cost=1600;
    schedule();
    CHECK(!flags && !crit_start && !irqoff.start);
    CHECK(irqoff_window.cycles==1600 && irqoff_window.phase==CPU_IRQOFF_FPU && !irqoff_window.gap);
    CHECK(!strcmp(irqoff_window.from,"idle") && !strcmp(irqoff_window.to,"worker"));
    cpu_emit(); CHECK(strstr(records,"us=1600 phase=fpu phase_us=1600 from=idle to=worker\n"));
    CHECK(offers==1 && offered.gap==1600 && offered.gap_bounded && offered.end-offered.gap_start==1600);
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
    for (unsigned i=0;i<30;i++) { cpu_irqoff_mark(CPU_IRQOFF_PICK); cycles+=10; }
    cycles+=2000; irq_restore(f);
    CHECK(irqoff.count==21 && irqoff_window.gap && irqoff_window.phase_cycles==2110);
    /* The unmeasured tail is never offered as a gap: only measured 10s. */
    CHECK(offered.gap==10 && offered.end-offered.start==2300);
    /* f1-94 phases: a millisecond in straight-line scheduler or tick code
     * is offered as bounded; the resumed caller and a device handler are not.
     * Every long section is offered, not only the window maximum. */
    static const struct { enum cpu_irqoff_phase phase; bool bounded; const char *name; } phases[]={
        {CPU_IRQOFF_QUEUE,true,"queue"},{CPU_IRQOFF_PROC,true,"proc"},{CPU_IRQOFF_RESUME,false,"resume"},
        {CPU_IRQOFF_IRQ_ENTRY,true,"irq-entry"},{CPU_IRQOFF_IRQ_HANDLER,false,"irq-handler"},
        {CPU_IRQOFF_IRQ_EOI,true,"irq-eoi"},{CPU_IRQOFF_IRQ_EXIT,true,"irq-exit"},
        {CPU_IRQOFF_TICK_TIMING,true,"tick-timing"},{CPU_IRQOFF_TICK_SCHED,true,"tick-sched"},
        {CPU_IRQOFF_TICK_TIMERS,true,"tick-timers"},{CPU_IRQOFF_TICK_SERIAL,true,"tick-serial"}};
    for (unsigned i=0;i<ARRAY_SIZE(phases);i++) {
        reset(); offers=0; f=irq_save(); uint64_t mark=cycles+20;
        cycles+=20; cpu_irqoff_mark(CPU_IRQOFF_IRQ_ENTRY); cycles+=5;
        cpu_irqoff_mark(phases[i].phase); cycles+=2110;
        cpu_irqoff_mark(CPU_IRQOFF_IRQ_EXIT); cycles+=3+i; irq_restore(f);
        CHECK(offers==1 && offered.gap==2110 && offered.gap_bounded==phases[i].bounded);
        CHECK(offered.gap_start==mark+5 && irqoff_window.phase==phases[i].phase);
        cpu_emit(); char want[96]; snprintf(want,sizeof(want),"phase=%s phase_us=2110 ",phases[i].name);
        CHECK(strstr(records,want));
    }
    /* timing.c feeds every PIT tick (with the previous tick's stamp) and
     * each no-work adjacent stamp pair to the SMI attribution. */
    timing_reset(); cycles+=1000; timing_timer_irq(); CHECK(tick_now==cycles && !tick_previous);
    uint64_t previous=cycles; cycles+=3100; timing_timer_irq();
    CHECK(tick_now==cycles && tick_previous==previous);
    timing_stall_observe(cycles-2100,cycles,"smi_attributed"); CHECK(adjacent[1]==cycles && adjacent[0]==cycles-2100);
    timing_stall_observe(cycles-50,cycles,"other"); CHECK(adjacent[0]==cycles-2100);
    reset(); offers=0; f=irq_save(); cycles+=999; irq_restore(f); CHECK(!offers);
    f=irq_save(); cycles+=1500; irq_restore(f); CHECK(offers==1);
    f=irq_save(); cycles+=1200; irq_restore(f); CHECK(offers==2 && irqoff_window.cycles==1500);
    puts("PASS f1-86 real scheduler/timing/cpustat hooks: IF-clear inactive entry, existing outer owner/site retained, switch and same-task cleanup, IF preserved");
    puts("PASS f1-86 deferred phases: raw timestamps only, no clearing unused slots, 999/1000us threshold, bounded overflow tail");
    puts("PASS f1-94 phases: queue/proc/resume/irq-entry/irq-handler/irq-eoi/irq-exit/tick-serial/timing/sched/timers named, bounded set offered to the SMI attribution for every section >= 1 ms");
    return 0;
}
