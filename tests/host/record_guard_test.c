/* Real record output on fake serial/console sinks, no privileged execution.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/arch.h>
#include <ciuki/init.h>

static unsigned failures, lines, captures, warnings;
static char serial_lines[16][256];
static char console_lines[16][256];
static unsigned console_count;
#define CHECK(x) do { if (!(x)) { printf("FAIL record guard line %d: %s\n", __LINE__, #x); failures++; } } while (0)
struct task *g_current;
struct tss g_tss;
struct ciuki_boot_info g_boot;
static uint32_t test_irq_save(void) { return 0; }
static void test_irq_restore(uint32_t flags) { (void)flags; }
static void test_cli(void) { }
static void test_hlt(void) { abort(); }
static uint32_t test_read_cr2(void) { return 0; }
#define irq_save test_irq_save
#define irq_restore test_irq_restore
#define cli test_cli
#define hlt test_hlt
#define read_cr2 test_read_cr2

void serial_write(const char *s, size_t n)
{
    CHECK(lines < ARRAY_SIZE(serial_lines) && n < sizeof(serial_lines[0]));
    if (lines < ARRAY_SIZE(serial_lines) && n < sizeof(serial_lines[0])) {
        memcpy(serial_lines[lines], s, n); serial_lines[lines++][n] = 0;
    }
    if (strstr(s, "[records]")) warnings++;
}
void console_write(const char *s, size_t n)
{
    CHECK(console_count < ARRAY_SIZE(console_lines) && n < sizeof(console_lines[0]));
    if (console_count < ARRAY_SIZE(console_lines) && n < sizeof(console_lines[0])) {
        memcpy(console_lines[console_count], s, n); console_lines[console_count++][n] = 0;
    }
}
void console_set_color(uint32_t fg, uint32_t bg) { (void)fg; (void)bg; }
void console_reclaim(void) { }
void bootlog_capture(const char *s, size_t n) { CHECK(s && n); captures++; }
#include "../../src/kernel/lib/fmt.c"
#include "../../src/kernel/core/output.c"

int main(void)
{
    CHECK(rec_premature_records() == 0);
    rec_emit("boot", "DATA", "before=run");
    rec_emit("boot", "BEGIN", 0);
    CHECK(rec_premature_records() == 2 && lines == 1 && warnings == 1);
    CHECK(!strstr(serial_lines[0], "CIUKI_TEST"));
    rec_set_run("12ab34cd");
    rec_emit("boot", "DATA", "before=begin");
    CHECK(rec_premature_records() == 3 && lines == 1 && warnings == 1);
    rec_emit("boot", "BEGIN", 0);
    CHECK(strstr(serial_lines[1], "run=12ab34cd seq=000001 probe=boot event=BEGIN"));
    rec_emit("boot", "DATA", "premature_records=%u", rec_premature_records());
    CHECK(strstr(serial_lines[2], "seq=000002") && strstr(serial_lines[2], "premature_records=3"));
    rec_emit_panicsafe("panic", "PANIC", "vector=14");
    CHECK(strstr(serial_lines[3], "seq=000003 probe=panic event=PANIC"));
    rec_set_run("abcdef01");
    rec_emit("boot", "DATA", 0);
    CHECK(rec_premature_records() == 4 && lines == 4 && warnings == 1);
    rec_emit("boot", "BEGIN", 0);
    CHECK(strstr(serial_lines[4], "run=abcdef01") && strstr(serial_lines[4], "event=BEGIN"));
    CHECK(lines == console_count);
    CHECK(captures == 4); /* warning and ordinary records; panic uses raw sinks */
    rec_set_run("12345678");
    rec_emit("sweep", "BEGIN", "phase=0 step=10");
    rec_emit("panic", "SWEEP", "step=9 result=not_run reason=reset_before_completion");
    rec_emit("sweep", "SWEEP_END", "passed=9 failed=0 not_run=1");
    CHECK(rec_premature_records() == 4 && lines == 8 && captures == 7);
    CHECK(strstr(serial_lines[5], "probe=sweep event=BEGIN"));
    CHECK(strstr(serial_lines[6], "probe=panic event=SWEEP"));
    CHECK(strstr(serial_lines[7], "event=SWEEP_END passed=9 failed=0 not_run=1"));
    CHECK(lines == console_count);
    for (unsigned i = 0; i < lines; i++) CHECK(!strcmp(serial_lines[i], console_lines[i]));
    /* Diagnostics share framing/sinks without claiming a probe scope. */
    run_selected = run_begun = false;
    memcpy(run_id, "00000000", 9);
    unsigned before = lines, captured = captures, drops = rec_premature_records();
    rec_diag("vm-boot", "group=vm_db dr6=%08x", 0xFFFF4FF0u);
    CHECK(lines == before && captures == captured);
    g_boot.flags = CBI_F_VERBOSE;
    rec_diag("vm-boot", "group=vm_db dr6=%08x", 0xFFFF4FF0u);
    CHECK(lines == before + 1 && captures == captured + 1 && !run_selected && !run_begun);
    CHECK(strstr(serial_lines[before], "run=00000000") && strstr(serial_lines[before], "event=DATA group=vm_db dr6=ffff4ff0"));
    rec_set_run("abcdef01");
    rec_diag("vm-boot", "group=vm_db");
    CHECK(lines == before + 1 && rec_premature_records() == drops);
    g_boot.flags = 0;
    CHECK(lines == console_count && !strcmp(serial_lines[before], console_lines[before]));
    printf("record runtime guard: %s (%u failures; drops before run/BEGIN, one warning, identical sinks, panic bypass)\n",
           failures ? "FAIL" : "PASS", failures);
    printf("verbose runtime diagnostics: %s (quiet off, no run/BEGIN needed, evidence scope preserved)\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
