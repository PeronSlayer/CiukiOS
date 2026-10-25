/* 101-a: real formatted output -> RAM logger -> aligned VFS writes.
 * SPDX-License-Identifier: GPL-2.0-only */
#define CIUKI_CPU_H
#include <stdlib.h>
#include <ciuki/kernel.h>
#include <ciuki/cpustat.h>
static uint32_t irq_save(void) { return 0x200; }
static void irq_restore(uint32_t f) { (void)f; }
static void cli(void) { }
static void hlt(void) { abort(); }
static uint32_t read_cr2(void) { return 0; }
struct ciuki_boot_info g_boot;
static bool window_active;
bool cpu_diagnostic_active(void) { return window_active; }
void serial_write(const char *s,size_t n) { (void)s; (void)n; }
void console_write(const char *s,size_t n) { (void)s; (void)n; }
void serial_panic(void) { }
void console_reclaim(void) { }
void console_set_color(uint32_t fg,uint32_t bg) { (void)fg; (void)bg; }
#define main previous_bootlog_main
#include "bootlog_test.c"
#undef main
#include "../../src/kernel/core/output.c"

static void statistics(void)
{
    klog_periodic("[cpu] window_ms=2000 interactive_pm=327 switches=11594");
    klog_periodic("[sched] device_overrun task=storage-writer#15 count=22 consumed_us=3787");
    klog_periodic("[kernel] irqoff_long site=c019bd31 us=2129 phase=ldt/debug");
    klog_periodic("[kernel] smi count=1 max_us=2117");
    klog_periodic("[kernel] smi_coincide ec_near=2");
    klog_periodic("[ec] thinkpad keys nvram_polls=20");
    klog_periodic("[storage] queue name=ata0 depth=0/0/0");
    klog_periodic("[kernel] bootlog writes=1 bytes=32768");
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    uint64_t bytes[2];
    for (unsigned logged=0;logged<2;logged++) {
        reset_model(); g_boot.flags=logged ? CBI_F_VERBOSE : 0;
        CHECK(!bootlog_activate(&fs,true,1));
        for (unsigned w=0;w<30;w++) {
            model_now+=2000;
            window_active=!logged && !w; /* admitted first normal window */
            statistics(); window_active=false;
            if (bootlog_due(model_now)) CHECK(!bootlog_drain());
        }
        CHECK(!bootlog_shutdown());
        struct bootlog_stats s=snapshot(); bytes[logged]=s.bytes_written;
        printf("101-a statistics disk replay: logged=%u windows=30 writes=%llu bytes=%llu protected_dropped=%u\n",
            logged,(unsigned long long)s.writes,(unsigned long long)s.bytes_written,s.dropped_protected);
        CHECK(s.writes==(logged ? 30u : 1u) && !s.dropped_protected);
        CHECK(disk_count("interactive_pm=327")== (logged ? 30u : 1u));
    }
    CHECK(bytes[0]==32768 && bytes[1]==983040);
    /* Normal milestones, faults and unknown periodic producers still reach
     * the protected sink. Only recognized performance records are opt-in. */
    reset_model(); g_boot.flags=0; window_active=false;
    klog("[storage] failure test=protected");
    klog_periodic("[kernel] lock_order violations=1");
    klog_periodic("[new-device] error=preserved");
    CHECK(!bootlog_activate(&fs,true,1)); drain(); CHECK(!bootlog_shutdown());
    CHECK(disk_count("failure test=protected")==1 && disk_count("lock_order violations=1")==1 &&
          disk_count("[new-device] error=preserved")==1);
    puts("PASS 101-a output/bootlog: 30x physical statistic-write reduction, all logged trial rows, normal faults and unknown producers retained");
    return 0;
}
