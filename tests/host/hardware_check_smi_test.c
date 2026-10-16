/* Production timing, registry reads and H experiment. Only CPU/PCI/PM and
 * PIT boundaries are simulated; durations here are synthetic host evidence. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CIUKI_CPU_H
#include <ciuki/kernel.h>
#include <ciuki/task.h>
#include <ciuki/timing.h>
#include <ciuki/registry.h>
#include <ciuki/hardware_check.h>
#define P2V(p) ((void *)(uintptr_t)(p))
#define V2P(p) ((uint32_t)(uintptr_t)(p))
static uint32_t irq_save(void);
static void irq_restore(uint32_t);
#include <ciuki/usb.h>

struct ciuki_boot_info g_boot;
struct task *g_current;
volatile uint64_t g_ticks;
bool g_cpu_tsc=true;
static uint64_t cycles=1;
static uint32_t cpu_flags=0x200;
static void (*lock_hook)(void);
static uint64_t rdtsc(void) { return cycles; }
static uint64_t read_tsc_serialized(void) { return cycles; }
static uint32_t read_eflags(void) { return cpu_flags; }
static uint32_t irq_save(void)
{ uint32_t f=cpu_flags; cpu_flags=0; if (f & 0x200) crit_begin(); if (lock_hook) lock_hook(); return f; }
static void irq_restore(uint32_t f)
{ if (f & 0x200) crit_end(); cpu_flags=f; }
static void hlt(void) { cycles+=1000; g_ticks++; }
static uint32_t pci_address, lpc_id=0x248c8086, pm_raw=0x1001;
static uint8_t decode=0x10, usb_present=7;
static uint16_t gen_config, legkey[3]={0x2000,0x2000,0x2000};
static uint32_t enable, status, target=1u<<6;
static unsigned pm_reads, writes, lines, event_lines;
static bool clear_fail, restore_fail, ineffective, rearm, change_other, jump_deadline, slow_log, slow_candidate, stale_candidate;
static bool changed, mutated, restoration_attempted;
static uint64_t next_stall, trial_start, spacing=64000;
static struct usb_host hosts[3];

static void outb(uint16_t port,uint8_t value) { (void)port; (void)value; assert(0); }
static void outl(uint16_t port,uint32_t value)
{
    if (port==0xcf8) { assert(!cpu_flags); pci_address=value; return; }
    /* No PCI CFC write, no W1C acknowledgement, and no PM write elsewhere. */
    assert(port==0x1030 && (g_boot.flags & CBI_F_HW_CHECK) && (cpu_flags & 0x200));
    assert(!((enable ^ value) & ~target));
    writes++;
    if (writes==1) {
        assert(!(value & target));
        trial_start=cycles;
        if (!clear_fail) enable=value,changed=true;
    } else {
        restoration_attempted=true;
        if (!restore_fail) enable=value,changed=false;
    }
}
static uint32_t inl(uint16_t port)
{
    if (port==0xcfc) {
        assert(!cpu_flags);
        unsigned dev=pci_address>>11 & 31, fn=pci_address>>8 & 7, off=pci_address & 0xfc;
        assert((pci_address & 0xffff0000u)==0x80000000u);
        if (dev==31 && !fn) {
            if (!off) return lpc_id;
            if (off==8) return 0x06010003;
            if (off==0x40) return pm_raw;
            if (off==0x44) return decode;
            if (off==0xa0) return gen_config;
        }
        if (dev==29 && fn<3) {
            if (!(usb_present & (1u<<fn))) return UINT32_MAX;
            if (!off) return (fn==0 ? 0x24820000 : fn==1 ? 0x24840000 : 0x24870000) | 0x8086;
            if (off==8) return 0x0c030001;
            if (off==0xc0) return legkey[fn];
        }
        assert(0);
    }
    assert(decode && (pm_raw & 0xff80)==0x1000); pm_reads++;
    if (port==0x1030) return enable;
    if (port==0x1034) return status;
    assert(0); return 0;
}
static uint16_t cpu_inw(uint16_t port)
{
    assert(decode && (pm_raw & 0xff80)==0x1000); pm_reads++;
    switch (port) {
    case 0x1000: return 0x8100;
    case 0x1002: return 0x0100;
    case 0x1004: return 0;
    case 0x1028: return 0x1234;
    case 0x102a: return 0x0123;
    case 0x1064: return 0x0080;
    default: assert(0); return 0;
    }
}
#include "latency_timing.inc"
#include "../../src/kernel/core/registry.c"
#include "../../src/kernel/core/hardware_check_smi.c"

unsigned usb_count(void) { return 3; }
const struct usb_host *usb_get(unsigned i) { return i<3 ? &hosts[i] : 0; }
static uint64_t now(void *ctx) { (void)ctx; return cycles/1000; }
static void line(void *ctx,const char *s)
{
    (void)ctx;
    assert(!changed || restoration_attempted); /* No reporting before cleanup. */
    assert(cpu_flags & 0x200);
    assert(strlen(s)<200);
    lines++;
    if (strstr(s," stall=")) event_lines++;
    if (slow_log && strstr(s," stall=")) cycles+=3000000;
    if (slow_candidate && strstr(s,"SMI candidate=")) cycles+=6000000;
    if (stale_candidate && strstr(s,"SMI candidate=")) enable|=1u<<4;
}
static const struct hw_check_ops ops={0,now,0,line};

void task_sleep_ms(uint32_t ms)
{
    assert(ms==1 && (cpu_flags & 0x200));
    cycles+=1000;
    if (changed && !mutated && (change_other || rearm)) {
        mutated=true;
        if (change_other) enable|=1u<<4;
        if (rearm) enable|=target;
    }
    if (changed && jump_deadline) cycles+=20000000;
    if (cycles>=next_stall) {
        next_stall+=spacing;
        if ((enable & target) || ineffective) {
            uint32_t f=irq_save(); crit_label("schedule"); cycles+=2108; irq_restore(f);
        }
    }
    g_ticks++; timing_timer_irq();
}

static void fixture(void)
{
    cycles=1; g_ticks=0; cpu_flags=0x200; g_cpu_tsc=true; g_tsc_per_ms=1000;
    g_boot=(struct ciuki_boot_info){.pci_bios=0x101,.flags=CBI_F_HW_CHECK};
    lpc_id=0x248c8086; pm_raw=0x1001; decode=0x10; usb_present=7;
    target=SMI_SW; enable=target|1|2|4|(1u<<5)|(1u<<13); status=0; gen_config=0;
    spacing=64000; next_stall=spacing;
    pm_reads=writes=lines=event_lines=0;
    clear_fail=restore_fail=ineffective=rearm=change_other=jump_deadline=slow_log=slow_candidate=stale_candidate=false;
    changed=mutated=restoration_attempted=false; trial_start=0;
    lock_hook=0;
    for (unsigned i=0;i<3;i++) {
        legkey[i]=0x2000;
        hosts[i]=(struct usb_host){.ready=true,.pci={.bus=0,.dev=29,.fn=i,.vendor=0x8086,
            .device=i==0 ? 0x2482 : i==1 ? 0x2484 : 0x2487}};
    }
    timing_reset();
}
static void run_baseline(void)
{
    struct hw_check_result r;
    hardware_check_timer(now(0)+15000,false,&r,&ops);
    assert(r.status==HW_FAIL && baseline_valid && baseline.stalls.count>2 && !writes);
    assert(baseline.elapsed>=10000 && baseline.elapsed<10004);
    assert(baseline.stalls.period_min_us>=60000 && baseline.stalls.period_max_us<=71000);
    assert(event_lines==baseline.stalls.count);
}
static void post_freeze_stall(void)
{
    if (!stall_active) { lock_hook=0; cycles+=5000; }
}
static void capture_tests(void)
{
    fixture(); cycles=0xfffffff0ull;
    timing_stalls_begin();
    struct timing_stall e;
    assert(!timing_stall_get(0,&e));
    uint32_t f=irq_save(); cycles+=1000; irq_restore(f); /* strict >1 ms */
    uint64_t start=cycles;
    f=irq_save(); crit_label("first"); cycles+=1001; irq_restore(f);
    uint64_t first_end=cycles;
    cycles+=62999; f=irq_save(); cycles+=1001; irq_restore(f); /* 64000 us spacing */
    cycles+=63099; f=irq_save(); cycles+=1001; irq_restore(f); /* 64100 us spacing */
    struct timing_stall_stats s; timing_stalls_end(&s,0);
    assert(s.count==3 && !s.dropped && s.period_min_us==64000 && s.period_max_us==64100);
    assert(s.period_mean_us==64050 && s.jitter_us==100);
    assert(timing_stall_get(0,&e) && e.start_tsc==start && e.end_tsc==first_end && !strcmp(e.section,"first"));
    assert(e.site && e.end_tsc>UINT32_MAX && !timing_stall_get(3,&e));
    f=irq_save(); cycles+=2000; irq_restore(f); timing_stalls_end(&s,0); assert(s.count==3);
    timing_stalls_begin();
    for (unsigned i=0;i<TIMING_STALL_CAPACITY+1;i++) {
        f=irq_save(); cycles+=1001; irq_restore(f);
    }
    timing_stalls_end(&s,0); assert(s.count==TIMING_STALL_CAPACITY && s.dropped==1);
    assert(timing_stall_get(TIMING_STALL_CAPACITY-1,&e));
    timing_stalls_begin(); timing_stalls_end(&s,0); assert(!s.count && !s.dropped && !s.period_mean_us);
    g_cpu_tsc=false; timing_stalls_begin(); timing_stalls_end(&s,0); assert(!s.count);
    fixture(); timing_stalls_begin();
    f=irq_save(); cycles+=1001; irq_restore(f);
    struct timing_stats frozen, live;
    lock_hook=post_freeze_stall; timing_stalls_end(&s,&frozen); timing_snapshot(&live);
    assert(s.count==1 && frozen.critical_us==1001 && live.critical_us==5000);
    puts("PASS SMI window: post-freeze analysis stall excluded from frozen timing counters");
    puts("PASS SMI trace: strict threshold, raw 64-bit timestamps, labels, min/mean/max, p-p jitter, freeze, capacity/drop/reset, no TSC");
}
static void snapshot_tests(void)
{
    fixture(); struct registry_smi p;
    assert(registry_smi_snapshot(&p) && pm_reads==8 && !writes);
    assert(p.base==0x1000 && p.gpe0_en==0x0123 && p.tco1_sts==0x80 && p.usb_present==7);
    for (unsigned i=0;i<3;i++) assert(p.usb_legkey[i]==0x2000);
    lpc_id=0x71108086; pm_reads=0; assert(!registry_smi_snapshot(&p) && !pm_reads && !p.supported);
    lpc_id=0x248c8086;
    static const uint32_t invalid[]={0,1,0x1000,0x1003,0x10001};
    for (unsigned i=0;i<ARRAY_SIZE(invalid);i++) {
        pm_raw=invalid[i]; assert(!registry_smi_snapshot(&p) && p.supported && !p.decoded && !pm_reads);
    }
    pm_raw=0x1001; decode=0; assert(!registry_smi_snapshot(&p) && !pm_reads);
    decode=0x10; usb_present=5; assert(registry_smi_snapshot(&p) && p.usb_present==5 && !p.usb_legkey[1]);
    g_boot.pci_bios=0; pm_reads=0; assert(!registry_smi_snapshot(&p) && !pm_reads && !writes);
    puts("PASS ICH3-M snapshot: chipset/base/decode qualification, exact widths/offsets, three UHCI functions, absent function, no data/config write");
}
static void experiment_tests(void)
{
    struct hw_check_result r;
    fixture(); run_baseline(); uint32_t original=enable;
    hardware_check_timer(now(0)+15000,true,&r,&ops);
    assert(r.status==HW_PASS && writes==2 && enable==original && restored && trial.complete && !trial.stalls.count);
    assert(cycles-trial_start>=10000000 && !changed && selected_bit==SMI_SW);
    hardware_check_smi_summary(&ops);
    puts("PASS SMI H experiment: measured 64 ms candidate, 10 s off, stalls removed, restore before reporting, unchanged other enables");

    /* The same single-bit cleanup covers each supported source, without a
     * global SMI disable or writes to USB_LEGKEY/PM/GPE status registers. */
    for (unsigned i=0;i<2;i++) {
        fixture(); target=i ? SMI_PERIODIC : SMI_USB;
        enable=(enable & ~SMI_SW) | target; original=enable;
        if (i) { spacing=8000000; next_stall=1000000; gen_config=3; }
        else { spacing=next_stall=128000; status=SMI_USB; legkey[0]|=0x10; }
        hardware_check_timer(now(0)+15000,false,&r,&ops);
        assert(r.status==HW_FAIL && baseline_valid && !writes && baseline.stalls.count>=2);
        hardware_check_timer(now(0)+15000,true,&r,&ops);
        assert(r.status==HW_PASS && selected_bit==target && writes==2 && restored && enable==original);
    }
    puts("PASS SMI H sources: USB status/traps with native owners and measured 8 s periodic candidate, one enable cleared/restored each");

    fixture(); run_baseline(); original=enable; clear_fail=true;
    hardware_check_timer(now(0)+15000,true,&r,&ops);
    assert(r.status==HW_FAIL && writes==2 && restored && enable==original && !have_trial);
    fixture(); run_baseline(); original=enable; ineffective=true;
    hardware_check_timer(now(0)+15000,true,&r,&ops);
    assert(r.status==HW_FAIL && writes==2 && restored && enable==original && trial.stalls.count);
    fixture(); run_baseline(); original=enable; jump_deadline=true;
    hardware_check_timer(now(0)+15000,true,&r,&ops);
    assert(r.status==HW_FAIL && writes==2 && restored && enable==original && !trial.complete);
    fixture(); run_baseline(); restore_fail=true;
    hardware_check_timer(now(0)+15000,true,&r,&ops);
    assert(r.status==HW_FAIL && writes==4 && !restored && !(enable & target));
    changed=false; /* fixture releases simulated stuck hardware for logging only */
    fixture(); run_baseline(); original=enable; rearm=change_other=true;
    hardware_check_timer(now(0)+15000,true,&r,&ops);
    assert(r.status==HW_FAIL && restored && writes==2 && enable==(original | (1u<<4)));
    assert(strstr(r.detail,"rearm=1 restore=1"));
    fixture(); run_baseline(); slow_log=true; ineffective=true;
    hardware_check_timer(now(0)+15000,true,&r,&ops);
    assert(r.status==HW_FAIL && restored && writes==2);
    puts("PASS SMI cleanup: failed clear, ineffective candidate, timeout, bounded failed restore, firmware rearm, unrelated changes, reporting overrun");
}
static void refusal_tests(void)
{
    struct hw_check_result r;
    fixture(); run_baseline();
    g_boot.flags=0; hardware_check_timer(now(0)+15000,true,&r,&ops); assert(r.status==HW_SKIP && !writes);
    g_boot.flags=CBI_F_HW_CHECK; cpu_flags=0;
    hardware_check_timer(now(0)+15000,true,&r,&ops); assert(r.status==HW_SKIP && !writes); cpu_flags=0x200;
    g_tsc_per_ms=0; hardware_check_timer(now(0)+15000,true,&r,&ops); assert(r.status==HW_SKIP && !writes);
    fixture(); run_baseline(); hardware_check_timer(now(0)+11000,true,&r,&ops); assert(r.status==HW_FAIL && !writes);
    fixture(); run_baseline(); slow_candidate=true;
    hardware_check_timer(now(0)+15000,true,&r,&ops); assert(r.status==HW_FAIL && !writes && !attempted);
    fixture(); run_baseline(); stale_candidate=true;
    hardware_check_timer(now(0)+15000,true,&r,&ops); assert(r.status==HW_SKIP && !writes && !attempted);
    fixture(); run_baseline(); enable|=SMI_USB; hardware_check_timer(now(0)+15000,true,&r,&ops); assert(r.status==HW_SKIP && !writes);
    fixture(); run_baseline(); baseline_valid=false; assert(!candidate(&pm_end));
    fixture(); run_baseline(); baseline.stalls.count=1; assert(!candidate(&pm_end));
    fixture(); run_baseline(); baseline.stalls.period_max_us=128000; assert(!candidate(&pm_end));
    fixture(); run_baseline();
    struct registry_smi p=pm_end;
    p.smi_en=1|2|SMI_USB; p.smi_sts=SMI_USB;
    assert(!candidate(&p)); /* Existing 0x2000 gates are native, not USB SMI enables. */
    p.usb_legkey[0]|=0x10; assert(candidate(&p)==SMI_USB);
    hosts[1].quarantined=true; assert(!candidate(&p)); hosts[1].quarantined=false;
    p.smi_en|=SMI_SW; assert(!candidate(&p)); /* Two plausible candidates. */
    p=pm_end; p.smi_en=1|2|SMI_PERIODIC; p.gen_pmcon1=3;
    assert(!candidate(&p)); /* 64 ms is not the chipset periodic timer. */
    baseline.stalls.period_min_us=baseline.stalls.period_max_us=8000000;
    assert(candidate(&p)==SMI_PERIODIC);
    p.smi_en=1|2|SMI_SW; p.gen_pmcon1=1u<<10;
    baseline.stalls.period_min_us=baseline.stalls.period_max_us=1500;
    assert(candidate(&p)==SMI_SW);
    p.smi_en=1|2|(1u<<13); assert(!candidate(&p)); /* Never gate TCO/global/EOS. */
    puts("PASS SMI refusal: non-H/IF/no clock/no budget, changed config, missing/ambiguous cadence, USB ownership/native gate, 8 s periodic vs 64 ms software, TCO observation only");
}
int main(void)
{
    capture_tests(); snapshot_tests(); experiment_tests(); refusal_tests();
    return 0;
}
