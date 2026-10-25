/* 101-a: real timing/cpustat/scheduler, T23-sized bounded and unknown gaps.
 * SPDX-License-Identifier: GPL-2.0-only */
#define IRQOFF_MAIN previous_irqoff_main
#include "scheduler_irqoff_test.c"

int main(void)
{
#ifndef GENERAL_SPEED_STATS_ONLY
    previous_irqoff_main();
    reset(); timing_reset();
    timing_begin_hook=timing_crit_begin; timing_end_hook=timing_crit_end;
    uint32_t f=irq_save(); crit_label("schedule");
    cycles+=4; cpu_irqoff_mark(CPU_IRQOFF_LDT_DEBUG); cycles+=2125;
    irq_restore(f);
    struct timing_stats s; real_timing_snapshot(&s);
    printf("101-a bounded replay: raw=%u work=%u firmware=%u violations=%u unclassified=%u\n",
           s.critical_us,s.critical_work_us,s.firmware_us,s.budget_violations,s.unclassified_irqoff);
    CHECK(s.critical_us==2129 && s.critical_work_us<=10 && s.firmware_us==2125);
    CHECK(!s.budget_violations && !s.unclassified_irqoff);
    /* Two firmware visits in one section; all marked gaps, no raw loss. */
    timing_reset(); f=irq_save(); cpu_irqoff_mark(CPU_IRQOFF_PICK); cycles+=2117;
    cpu_irqoff_mark(CPU_IRQOFF_LDT_DEBUG); cycles+=2138;
    cpu_irqoff_mark(CPU_IRQOFF_NONE); cycles+=4; irq_restore(f);
    real_timing_snapshot(&s);
    CHECK(s.critical_us==4259 && s.critical_work_us==4 && s.firmware_us==4255 && !s.budget_violations);
    /* No marks, arbitrary device handler, and genuinely slow remaining work
     * are never excused by the scheduler's nearest label. */
    timing_reset(); f=irq_save(); crit_label("schedule"); cycles+=2129; irq_restore(f);
    real_timing_snapshot(&s); CHECK(s.critical_work_us==2129 && s.budget_violations==1 && s.unclassified_irqoff==1);
    timing_reset(); f=irq_save(); cpu_irqoff_mark(CPU_IRQOFF_IRQ_HANDLER); cycles+=2129; irq_restore(f);
    real_timing_snapshot(&s); CHECK(s.critical_work_us==2129 && s.budget_violations==1);
    timing_reset(); f=irq_save(); cpu_irqoff_mark(CPU_IRQOFF_LDT_DEBUG); cycles+=2117;
    cpu_irqoff_mark(CPU_IRQOFF_IRQ_HANDLER); cycles+=150; irq_restore(f);
    real_timing_snapshot(&s); CHECK(s.critical_work_us==150 && s.budget_violations==1);
    /* Adjacent no-work probes may overlap a bounded mark: count once. */
    timing_reset(); f=irq_save(); cpu_irqoff_mark(CPU_IRQOFF_LDT_DEBUG);
    uint64_t a=cycles; cycles+=2117; timing_stall_observe(a,cycles,"smi_attributed"); irq_restore(f);
    real_timing_snapshot(&s); CHECK(s.firmware_us==2117 && !s.critical_work_us && !s.budget_violations);
#endif

    reset();
    /* Replay the trial's recurring eight-ms desktop stretches: normal boots
     * must not turn every two-second window into a full diagnostic dump. */
    unsigned windows=30, emitted=0, bytes=0;
    for (unsigned i=0;i<windows;i++) {
        threshold=true; cycles+=2000000; record_bytes=0; records[0]=0; cpu_emit();
        emitted+=strstr(records,"[cpu] window_ms=")!=NULL;
        bytes+=record_bytes;
        CHECK(!cpu_diagnostic_active());
    }
    printf("101-a normal diagnostic replay: windows=%u emitted=%u bytes=%u\n",windows,emitted,bytes);
    CHECK(emitted<=1);
    g_boot.flags=CBI_F_VERBOSE; emitted=0;
    for (unsigned i=0;i<windows;i++) {
        cycles+=2000000; record_bytes=0; records[0]=0; cpu_emit();
        emitted+=strstr(records,"[cpu] window_ms=")!=NULL;
        CHECK(!cpu_diagnostic_active());
    }
    CHECK(emitted==windows);
    puts("PASS 101-a: bounded firmware attribution, real/unclassified work retained, normal/logged statistics");
    return 0;
}
