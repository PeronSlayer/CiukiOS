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
void serial_write(const char *s, size_t n)
{
    if (!(cpu_flags & 0x200) || strlen(output) + n >= sizeof(output)) abort();
    size_t at = strlen(output); memcpy(output + at, s, n); output[at + n] = 0;
}
void console_write(const char *s, size_t n) { (void)s; (void)n; }
void serial_panic(void) { if (cpu_flags & 0x200) abort(); }
void console_reclaim(void) {}
void console_set_color(uint32_t fg, uint32_t bg) { (void)fg; (void)bg; }
void bootlog_capture(const char *s, size_t n) { (void)s; (void)n; }
#include "../../src/kernel/core/output.c"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); return 1; } } while (0)
int main(void)
{
    CHECK(!rec_active());
    /* Runtime callers omit evidence during both quiet and L normal boots. */
    if (rec_active()) rec_emit("vm-window", "DATA", "group=admission");
    CHECK(!output[0] && !rec_premature_records());
    g_boot.flags = CBI_F_VERBOSE;
    rec_diag("vm-window", "group=vm_heartbeat pm=1 vif=0");
    CHECK(strstr(output, "run=00000000") && strstr(output, "group=vm_heartbeat") && !rec_premature_records());
    rec_set_run("12345678");
    CHECK(!rec_active());
    output[0] = 0; rec_diag("vm-window", "group=early"); CHECK(!output[0]);
    /* A mistaken evidence producer is still rejected and counted. */
    rec_emit("vm-window", "DATA", "group=early");
    CHECK(rec_premature_records() == 1 && strstr(output, "dropped premature evidence record"));
    output[0] = 0; rec_emit("vm-window", "BEGIN", "version=1"); CHECK(rec_active());
    if (rec_active()) rec_emit("vm-window", "DATA", "group=admission");
    CHECK(strstr(output, "run=12345678") && strstr(output, "group=admission"));
    rec_set_run("87654321"); CHECK(!rec_active());
    puts("record runtime: normal launch silent, L diagnostics framed, evidence remains gated until BEGIN PASS");
    return 0;
}
