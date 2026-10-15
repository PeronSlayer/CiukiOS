/* Production record selection/begin gate; no physical UART or IRQs.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CIUKI_CPU_H
#include <ciuki/kernel.h>
#include <ciuki/arch.h>
static uint32_t cpu_flags = 0x200;
static uint32_t irq_save(void) { uint32_t f = cpu_flags; cpu_flags = 0; return f; }
static void irq_restore(uint32_t f) { cpu_flags = f; }
static void cli(void) { cpu_flags = 0; }
static void hlt(void) { abort(); }
static uint32_t read_cr2(void) { return 0; }
struct ciuki_boot_info g_boot;
static char output[8192];
static unsigned ordinary_writes,console_writes,ring_captures;
static unsigned blackbox_captures;
static char last_ring[256], last_blackbox[256];
static void append_output(const char *s,size_t n)
{
    /* The serial queue accepts IRQ/atomic publications too; only the
     * separate UART consumer requires ordinary task context. */
    if (strlen(output) + n >= sizeof(output)) abort();
    size_t at = strlen(output); memcpy(output + at, s, n); output[at + n] = 0;
}
void serial_write(const char *s,size_t n) { ordinary_writes++;append_output(s,n); }
void console_write(const char *s, size_t n) { (void)s; (void)n;console_writes++; }
void serial_panic(void) { if (cpu_flags & 0x200) abort(); }
void console_reclaim(void) {}
void console_set_color(uint32_t fg, uint32_t bg) { (void)fg; (void)bg; }
void bootlog_capture(const char *s, size_t n) { (void)s; (void)n; }
void bootlog_capture_ring(const char *s, size_t n)
{ if (n >= sizeof(last_ring)) abort(); memcpy(last_ring,s,n); last_ring[n]=0; ring_captures++; }
void blackbox_capture(const char *s, size_t n)
{ if (n >= sizeof(last_blackbox)) abort(); memcpy(last_blackbox,s,n); last_blackbox[n]=0; blackbox_captures++; }
#include "../../src/kernel/core/output.c"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); return 1; } } while (0)
int main(void)
{
    unsigned initial_ring=ring_captures;
    rec_diag_disk_bootlog("vm-window","group=vm_perf_cpu ms=5000"); CHECK(ring_captures==initial_ring+1);
    g_boot.flags=CBI_F_VERBOSE;
    rec_diag_disk_bootlog("vm-window","group=vm_perf_cpu ms=5000"); CHECK(ring_captures==initial_ring+2);
    g_boot.flags=CBI_F_DIAG_VERBOSE;
    rec_diag_disk_bootlog("vm-window","group=vm_perf_cpu ms=5000");
    CHECK(ring_captures==initial_ring+3 && !ordinary_writes && !console_writes && !output[0]);
    CHECK(!blackbox_captures && strstr(last_ring,"group=vm_perf_cpu ms=5000"));
    rec_diag_disk("disk","group=disk_event status=ok");
    CHECK(blackbox_captures==1 && strstr(last_blackbox,"group=disk_event status=ok"));
    CHECK(ring_captures==initial_ring+4 && strstr(last_ring,"group=disk_event status=ok"));
    CHECK(!ordinary_writes && !console_writes && !output[0]);
    g_boot.flags=0;
    CHECK(!rec_active());
    /* Runtime callers omit evidence during both quiet and L normal boots. */
    if (rec_active()) rec_emit("vm-window", "DATA", "group=admission");
    CHECK(!output[0] && !rec_premature_records());
    g_boot.flags = CBI_F_VERBOSE;
    rec_diag("vm-window", "group=vm_heartbeat pm=1 vif=0");
    CHECK(strstr(output, "run=00000000") && strstr(output, "group=vm_heartbeat") && !rec_premature_records());
    rec_set_run("12345678");
    g_boot.flags=CBI_F_DIAG_VERBOSE;
    unsigned selected_ring=ring_captures;
    unsigned selected_blackbox=blackbox_captures;
    rec_diag_disk_bootlog("vm-window","group=vm_perf_cpu ms=5000");CHECK(ring_captures==selected_ring);
    rec_diag_disk("disk","group=disk_event status=early");CHECK(blackbox_captures==selected_blackbox);
    CHECK(!rec_active());
    output[0] = 0; rec_diag("vm-window", "group=early"); CHECK(!output[0]);
    /* A mistaken evidence producer is still rejected and counted. */
    rec_emit("vm-window", "DATA", "group=early");
    CHECK(rec_premature_records() == 1 && strstr(output, "dropped premature evidence record"));
    output[0] = 0; rec_emit("vm-window", "BEGIN", "version=1"); CHECK(rec_active());
    selected_ring=ring_captures; selected_blackbox=blackbox_captures;
    rec_diag_disk_bootlog("vm-window","group=vm_perf_cost cause=io exits=12");
    CHECK(ring_captures==selected_ring+1 && blackbox_captures==selected_blackbox);
    CHECK(strstr(last_ring,"run=12345678") && strstr(last_ring,"group=vm_perf_cost"));
    if (rec_active()) rec_emit("vm-window", "DATA", "group=admission");
    CHECK(strstr(output, "run=12345678") && strstr(output, "group=admission"));
    rec_set_run("87654321"); CHECK(!rec_active());
    unsigned ordinary=ordinary_writes,screen=console_writes,ring=ring_captures;
    output[0]=0;klog_periodic("[cpu] window_ms=%u idle=%u top=%s",2000u,900u,"desktop#4:100");
    CHECK(!strcmp(output,"[cpu] window_ms=2000 idle=900 top=desktop#4:100\n"));
    CHECK(ordinary_writes==ordinary+1 && console_writes==screen+1 && ring_captures==ring+1);
    puts("record runtime: normal launch silent, L diagnostics framed, evidence remains gated until BEGIN PASS");
    puts("record F0-20: periodic CPU records use runtime RAM ring and normal serial queue PASS");
    puts("record f3-31: vm_perf rows reach boot-log ring only; ordinary disk records still reach black box; BEGIN gate preserved PASS");
    return 0;
}
