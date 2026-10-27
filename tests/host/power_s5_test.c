/* Host test: the ACPI S5 register fallback (core/power_acpi_s5.c, fx-acpioff).
 * A scripted port space stands in for the chipset: PM1a/PM1b control and
 * event blocks, GPE blocks and the SMI command port. The machine "turns
 * off" when SLP_EN is written with the expected SLP_TYP in ACPI mode; the
 * stall seam then leaves the sequence the way real hardware does (it never
 * returns). Every other outcome returns.
 * SPDX-License-Identifier: GPL-2.0-only */
#define POWER_HOST 1
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ciuki/power.h>
#include "../../src/kernel/core/power.c"   /* the read-only parser (POWER_HOST: no runtime) */

/* ---- the parser's tables (same layout as power_test.c) --------------- */
enum { MEM = 0x20000, RSDP = 0x10000, RSDT = 0x1000, FADT = 0x2000, DSDT = 0x4000, SSDT = 0x6000 };
static uint8_t mem[MEM];
static unsigned maps, unmaps, map_bytes;
static uint32_t rsdp_phys = RSDP;
static const uint8_t *fake_map(uint32_t phys, uint32_t len)
{ maps++; map_bytes += len; return (uint64_t)phys + len <= MEM ? mem + phys : 0; }
static void fake_unmap(const uint8_t *p, uint32_t len) { (void)p; (void)len; unmaps++; }
static void put32(uint32_t at, uint32_t v) { for (int i = 0; i < 4; i++) mem[at + i] = (uint8_t)(v >> (8 * i)); }
static void fix(uint32_t at, uint32_t sum_at, uint32_t len)
{ mem[sum_at] = 0; uint8_t s = 0; for (uint32_t i = 0; i < len; i++) s = (uint8_t)(s + mem[at + i]); mem[sum_at] = (uint8_t)-s; }
static void header(uint32_t at, const char *sig, uint32_t len, uint8_t rev)
{ memcpy(mem + at, sig, 4); put32(at + 4, len); mem[at + 8] = rev; memcpy(mem + at + 10, "CIUKI ", 6); }
static void seal(uint32_t at) { fix(at, at + 9, mem[at + 4] | mem[at + 5] << 8 | mem[at + 6] << 16); }
/* QEMU i440fx shape: PM1a_CNT 0x604, PM1a_EVT 0x600, GPE0 0x60C/4, SMI_CMD B2/F1. */
static void tables(const uint8_t *aml, uint32_t aml_len, uint32_t pm1b, uint32_t smi_cmd, uint8_t enable)
{
    memset(mem, 0, sizeof(mem));
    memcpy(mem + RSDP, "RSD PTR ", 8); memcpy(mem + RSDP + 9, "CIUKI ", 6);
    put32(RSDP + 16, RSDT); fix(RSDP, RSDP + 8, 20);
    header(RSDT, "RSDT", 36 + 4, 1); put32(RSDT + 36, FADT); seal(RSDT);
    header(FADT, "FACP", 116, 1);
    put32(FADT + 40, DSDT); put32(FADT + 48, smi_cmd); mem[FADT + 52] = enable; mem[FADT + 53] = 0xF0;
    put32(FADT + 56, 0x600); put32(FADT + 60, pm1b ? pm1b - 4 : 0);
    put32(FADT + 64, 0x604); put32(FADT + 68, pm1b);
    put32(FADT + 80, 0x60C); mem[FADT + 88] = 4; mem[FADT + 89] = 2; mem[FADT + 92] = 4;
    seal(FADT);
    header(DSDT, "DSDT", 36 + aml_len, 1); memcpy(mem + DSDT + 36, aml, aml_len); seal(DSDT);
}
static const uint8_t s5_qemu[] = { 0x10, 0x0F, 0x5C, 0x00, 0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x04, 0, 0, 0, 0, 0xA3 };
static const uint8_t s5_ich[] = { 0x08, 0x5C, '_', 'S', '5', '_', 0x12, 0x08, 0x04, 0x0A, 0x07, 0x0A, 0x07, 0, 0 };
static const uint8_t no_s5[] = { 0x08, '_', 'S', '4', '_', 0x12, 0x06, 0x04, 0, 0, 0, 0 };

/* ---- the chipset ------------------------------------------------------- */
static uint16_t port16[0x10000];
static uint8_t port8[0x10000];
static char trace[196608];              /* bounded enable + rollback polls of A and B */
static unsigned trace_len, enable_polls_needed, stalls_ms;
static unsigned disable_polls_needed, disable_requested_ms;
static bool powered_off, enable_ignored, disable_ignored, sleep_ignored, sci_in_b;
static bool enable_requested, disable_requested, change_enable_masks;
static jmp_buf off_jump;
static void record(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(trace + trace_len, sizeof(trace) - trace_len, fmt, ap);
    va_end(ap); assert(n >= 0 && (unsigned)n < sizeof(trace) - trace_len); trace_len += (unsigned)n;
}
static uint16_t fake_inw(uint16_t p) { record("r%04x", p); return port16[p]; }
static uint8_t fake_inb(uint16_t p) { record("i%04x", p); return port8[p]; }
static void fake_outb(uint16_t p, uint8_t v)
{
    record("b%04x=%02x", p, v);
    port8[p] = v;
    if (p == 0xB2 && v == 0xF1 && !enable_ignored) {
        enable_requested = true;
        if (!enable_polls_needed) port16[sci_in_b ? 0x644 : 0x604] |= 1;
        if (change_enable_masks) {
            port16[0x602] = port16[0x642] = 0;
            port8[0x60E] = port8[0x60F] = port8[0x622] = port8[0x623] = 0;
        }
    }
    if (p == 0xB2 && v == 0xF0) {
        disable_requested = true; disable_requested_ms = stalls_ms;
        if (!disable_ignored && !disable_polls_needed) { port16[0x604] &= ~1u; port16[0x644] &= ~1u; }
    }
}
static void fake_outw(uint16_t p, uint16_t v)
{
    record("w%04x=%04x", p, v);
    if (p == 0x600 || p == 0x640) { port16[p] &= ~v; return; } /* PM1 status: write 1 clears */
    if (p == 0x604 || p == 0x644) {
        /* SCI_EN is read-only; SLP_EN reads as zero. */
        port16[p] = (v & ~0x2001u) | (port16[p] & 1u);
        if (p == 0x604 && (v & 0x2000) && ((port16[0x604] | port16[0x644]) & 1) && !sleep_ignored) powered_off = true;
    } else port16[p] = v;
}
static void fake_stall(unsigned ms)
{
    if (powered_off) longjmp(off_jump, 1); /* real hardware never returns from here */
    stalls_ms += ms;
    if (enable_requested && !disable_requested && enable_polls_needed && stalls_ms >= enable_polls_needed)
        port16[sci_in_b ? 0x644 : 0x604] |= 1;
    if (disable_requested && !disable_ignored && stalls_ms - disable_requested_ms >= disable_polls_needed) {
        port16[0x604] &= ~1u; port16[0x644] &= ~1u;
    }
}
/* The IRQ trace is kept apart from the I/O trace: qualified()'s state latch
 * takes the lock too. io_at_restore tells whether I/O followed a restore. */
static char irq_trace[32];
static unsigned io_at_restore;
static uint32_t fake_irq_save(void) { strcat(irq_trace, "C"); return 0x200; }
static void fake_irq_restore(uint32_t f) { (void)f; strcat(irq_trace, "S"); io_at_restore = trace_len; }
static char last_log[512], all_logs[8192];
static void fake_log(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vsnprintf(last_log, sizeof(last_log), fmt, ap); va_end(ap);
    strncat(all_logs, last_log, sizeof(all_logs) - strlen(all_logs) - 2); strcat(all_logs, "\n");
}
/* ---- policy, hint, registry seams ------------------------------------- */
static bool allowed = true, acpi_decided = true;
static int hint_result = -ENOENT;
static uint8_t hint_a, hint_b;
static int fake_hint(uint8_t *a, uint8_t *b) { if (hint_result) return hint_result; *a = hint_a; *b = hint_b; return 0; }
static const char *port_owner;
static uint16_t owner_start, owner_end;
static const char *fake_owner(uint16_t start, unsigned len)
{ return port_owner && owner_end > start && owner_start < start + len ? port_owner : NULL; }

#define s5_map(phys, len) fake_map(phys, len)
#define s5_unmap(p, len) fake_unmap(p, len)
#define s5_hint(a, b) fake_hint(a, b)
#define s5_acpi_decided() acpi_decided
#define s5_rsdp() rsdp_phys
#define s5_allowed() allowed
#define s5_port_owner(start, len) fake_owner(start, len)
#define s5_inw(p) fake_inw(p)
#define s5_inb(p) fake_inb(p)
#define s5_outb(p, v) fake_outb(p, v)
#define s5_outw(p, v) fake_outw(p, v)
#define s5_stall_ms(ms) fake_stall(ms)
#define s5_irq_save() fake_irq_save()
#define s5_irq_restore(f) fake_irq_restore(f)
#define s5_log fake_log
#include "../../src/kernel/core/power_acpi_s5.c"

static unsigned failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)
static void reset(void)
{
    memset(&s5, 0, sizeof(s5)); s5_window_count = 0;
    memset(port16, 0, sizeof(port16)); memset(port8, 0, sizeof(port8));
    trace[0] = 0; trace_len = 0; all_logs[0] = 0; last_log[0] = 0; irq_trace[0] = 0; io_at_restore = 0;
    maps = unmaps = map_bytes = 0; enable_polls_needed = stalls_ms = 0;
    powered_off = enable_ignored = false; allowed = acpi_decided = true;
    disable_ignored = sleep_ignored = sci_in_b = enable_requested = disable_requested = change_enable_masks = false;
    disable_polls_needed = disable_requested_ms = 0;
    hint_result = -ENOENT; port_owner = NULL; rsdp_phys = RSDP;
}
/* Runs the Off and reports whether the fake machine turned off. */
static int run_off(bool forced, bool *off)
{
    *off = false;
    if (setjmp(off_jump)) { *off = true; return 0; }
    return power_s5_fallback_off(forced);
}
static void test_parse_and_state(void)
{
    /* Qualified once: the tables are parsed on the first call only, every
     * mapped window is released, the record is logged. */
    reset(); tables(s5_qemu, sizeof(s5_qemu), 0, 0xB2, 0xF1);
    CHECK(!strcmp(power_s5_fallback_state(), "unparsed"));
    CHECK(power_s5_fallback_qualified() && !strcmp(power_s5_fallback_state(), "available"));
    CHECK(maps == unmaps && maps > 0 && s5.off.source == ACPI_S5_SOURCE_DSDT);
    CHECK(strstr(all_logs, "acpi s5 fallback available source=dsdt pm1a_cnt=0604 pm1b_cnt=0000 cnt_len=2 smi_cmd=00b2 acpi_enable=f1 typ=0/0 pm1_evt=0600/0000 evt_len=4 gpe0=060c/4"));
    unsigned first = maps;
    CHECK(power_s5_fallback_qualified() && maps == first);
    /* Policy: safe or hardware-check boots never qualify and never parse. */
    reset(); tables(s5_qemu, sizeof(s5_qemu), 0, 0xB2, 0xF1); allowed = false;
    CHECK(!power_s5_fallback_qualified() && !maps && !strcmp(power_s5_fallback_state(), "policy"));
    CHECK(power_s5_fallback_off(false) == -ENOTSUP && !trace_len);
    /* No RSDP: unavailable, final. */
    reset(); rsdp_phys = 0;
    CHECK(!power_s5_fallback_qualified() && !strcmp(power_s5_fallback_state(), "rsdp") && s5.final);
    CHECK(strstr(all_logs, "unavailable step=rsdp"));
    /* No \_S5 anywhere: pending until the inventory decided, then final;
     * the inventory's evaluated \_S5 qualifies it meanwhile. */
    reset(); tables(no_s5, sizeof(no_s5), 0, 0xB2, 0xF1); acpi_decided = false;
    CHECK(!power_s5_fallback_qualified() && !strcmp(power_s5_fallback_state(), "pending") && !s5.final);
    CHECK(s5.off.pm1a_cnt == 0x604 && s5.off.source == ACPI_S5_SOURCE_NONE);
    first = maps;
    CHECK(!power_s5_fallback_qualified() && maps == first); /* no re-parse while pending */
    hint_result = 0; hint_a = 7; hint_b = 0;
    CHECK(power_s5_fallback_qualified() && s5.off.source == ACPI_S5_SOURCE_ACPICA && s5.off.slp_typa == 7 && !s5.off.slp_typb);
    CHECK(strstr(all_logs, "available source=acpica") && maps == first);
    reset(); tables(no_s5, sizeof(no_s5), 0, 0xB2, 0xF1); acpi_decided = false;
    CHECK(!power_s5_fallback_qualified());
    acpi_decided = true;
    CHECK(!power_s5_fallback_qualified() && s5.final && !strcmp(power_s5_fallback_state(), "s5"));
    CHECK(strstr(all_logs, "unavailable step=s5"));
    hint_result = 0; CHECK(!power_s5_fallback_qualified()); /* final: a late hint changes nothing */
    /* An out-of-range hint never qualifies. */
    reset(); tables(no_s5, sizeof(no_s5), 0, 0xB2, 0xF1); hint_result = 0; hint_a = 8; hint_b = 0;
    CHECK(!power_s5_fallback_qualified() && s5.final);
    /* A concurrent caller during the parse sees "parsing" and no result. */
    reset(); tables(s5_qemu, sizeof(s5_qemu), 0, 0xB2, 0xF1); s5.state = S5_PARSING;
    CHECK(!power_s5_fallback_qualified() && !strcmp(power_s5_fallback_state(), "parsing") && !maps);
    puts("PASS s5 fallback: one bounded read-only parse, windows released, policy, pending/hint/final states");
}
static void test_off_sequence(void)
{
    bool off;
    /* ACPI mode already (SCI_EN set): no SMI write; PM1 status and GPE
     * enables cleared; SLP_TYP then SLP_TYP|SLP_EN; cli before, machine off. */
    reset(); tables(s5_ich, sizeof(s5_ich), 0, 0xB2, 0xF1);
    port16[0x604] = 0x0001 | 0x0002; /* SCI_EN, BM_RLD kept */
    CHECK(run_off(false, &off) == 0 && off);
    CHECK(strstr(trace, "w0600=c731b060e=00b060f=00b060c=ffb060d=ffr0604w0604=1c03w0604=3c03"));
    CHECK(!strcmp(irq_trace, "CSC")); /* the latch's pair, then IF=0 for the sequence; never restored: the machine went off */
    CHECK(strstr(all_logs, "mode=acpi pm1a=1c03 pm1b=1c03 events=cleared"));
    /* The log before the sequence names the source and the ports. */
    CHECK(strstr(all_logs, "acpi s5 fallback: source=dsdt typ=7/7 pm1a_cnt=0604 pm1b_cnt=0000 forced=0"));
    /* PM1b present: written after PM1a each time, with A OR B's non-sleep bits. */
    reset(); tables(s5_qemu, sizeof(s5_qemu), 0x644, 0xB2, 0xF1);
    port16[0x604] = 0x0001;
    CHECK(run_off(true, &off) == 0 && off);
    CHECK(strstr(trace, "w0600=c731w0640=c731") && strstr(trace, "r0604r0644w0604=0001w0644=0001w0604=2001w0644=2001"));
    CHECK(strstr(all_logs, "forced=1"));
    /* Legacy mode with SMI_CMD/ACPI_ENABLE: one ACPI_ENABLE write, SCI_EN
     * polled (here set after 5 ms), then the same sequence. */
    reset(); tables(s5_ich, sizeof(s5_ich), 0, 0xB2, 0xF1); enable_polls_needed = 5;
    CHECK(run_off(false, &off) == 0 && off);
    CHECK(strstr(trace, "r0604b00b2=f1r0604r0604r0604r0604r0604r0604w0600=c731") && stalls_ms == 5);
    CHECK(strstr(all_logs, "mode=enabled"));
    /* SMI_CMD zero: always ACPI mode (ACPICA acpi_hw_get_mode), no write to port 0. */
    reset(); tables(s5_qemu, sizeof(s5_qemu), 0, 0, 0xF1);
    CHECK(run_off(false, &off) == -EIO && !off); /* SCI_EN stays clear in the model: it never "powers off" */
    CHECK(!strstr(trace, "b0000=") && strstr(trace, "w0600=c731") && strstr(all_logs, "mode=acpi_no_smi_cmd"));
    CHECK(strstr(trace, "w0604=2000w0604=2000") && stalls_ms == S5_SETTLE_MS + S5_RETRY_MS);
    CHECK(!strcmp(irq_trace, "CSCS") && io_at_restore == trace_len); /* IF restored after the last write */
    CHECK(strstr(last_log, "still running after 3000 ms mode=acpi_no_smi_cmd"));
    /* ACPI_ENABLE value zero: no handshake possible; legacy write, no event clearing. */
    reset(); tables(s5_ich, sizeof(s5_ich), 0, 0xB2, 0);
    CHECK(run_off(false, &off) == -EIO && !off);
    CHECK(!strstr(trace, "b00b2=") && !strstr(trace, "w0600=") && !strstr(trace, "b060") && strstr(all_logs, "mode=legacy_no_enable_value") && strstr(all_logs, "events=firmware"));
    CHECK(strstr(trace, "w0604=1c00w0604=3c00"));
    /* The platform never answers ACPI_ENABLE: bounded at 3000 polls, then the
     * legacy write anyway, then -EIO after the bounded waits with IF restored. */
    reset(); tables(s5_ich, sizeof(s5_ich), 0, 0xB2, 0xF1); enable_ignored = true;
    CHECK(run_off(false, &off) == -EIO && !off);
    CHECK(stalls_ms == S5_ENABLE_POLLS + S5_SETTLE_MS + S5_RETRY_MS && strstr(all_logs, "mode=legacy_enable_timeout"));
    CHECK(!strcmp(irq_trace, "CSCS") && io_at_restore == trace_len);
    unsigned enables = 0; for (const char *p = trace; (p = strstr(p, "b00b2=f1")); p++) enables++;
    CHECK(enables == 1);
    /* Unsupported present GPE blocks refuse qualification before any I/O. */
    reset(); tables(s5_qemu, sizeof(s5_qemu), 0, 0xB2, 0xF1); mem[FADT + 92] = 64; seal(FADT); port16[0x604] = 1;
    CHECK(run_off(false, &off) == -ENOTSUP && !off && !trace_len);
    reset(); tables(s5_qemu, sizeof(s5_qemu), 0, 0xB2, 0xF1); mem[FADT + 92] = 3; seal(FADT); port16[0x604] = 1;
    CHECK(run_off(false, &off) == -ENOTSUP && !off && !trace_len);
    /* Another owner of the PM1 control ports: refused before any I/O. */
    reset(); tables(s5_qemu, sizeof(s5_qemu), 0x644, 0xB2, 0xF1); port_owner = "acpi-power"; owner_start = 0x604; owner_end = 0x606;
    CHECK(run_off(false, &off) == -ENOTSUP && !off && !trace_len && !strcmp(irq_trace, "CS") && strstr(last_log, "held by acpi-power"));
    reset(); tables(s5_qemu, sizeof(s5_qemu), 0x644, 0xB2, 0xF1); port_owner = "ne2000"; owner_start = 0x640; owner_end = 0x660;
    CHECK(run_off(false, &off) == -ENOTSUP && !trace_len && strstr(last_log, "held by ne2000"));
    reset(); tables(s5_qemu, sizeof(s5_qemu), 0x644, 0xB2, 0xF1); port_owner = "ne2000"; owner_start = 0x300; owner_end = 0x320; port16[0x604] = 1;
    CHECK(run_off(false, &off) == 0 && off);
    /* Not qualified: no I/O at all. */
    reset(); rsdp_phys = 0;
    CHECK(power_s5_fallback_off(false) == -ENOTSUP && !trace_len);
    puts("PASS s5 fallback: SCI_EN handshake (set, SMI_CMD zero, no enable value, timeout), status/GPE clearing in ACPI mode only, PM1a then PM1b, two writes, bounded waits, IF restored, registry owner refusal");
}
static void seed_masks(void)
{
    put32(FADT + 84, 0x620); mem[FADT + 93] = 4; seal(FADT);
    port16[0x602] = 0x0121; port16[0x642] = 0x0400;
    port8[0x60E] = 0x12; port8[0x60F] = 0xA5;
    port8[0x622] = 0x78; port8[0x623] = 0xFF;
}
static void check_masks(void)
{
    CHECK(port16[0x602] == 0x0121 && port16[0x642] == 0x0400);
    CHECK(port8[0x60E] == 0x12 && port8[0x60F] == 0xA5 && port8[0x622] == 0x78 && port8[0x623] == 0xFF);
}
static void test_review_round_1(void)
{
    bool off;
    /* P1: capture firmware masks before a takeover that changes them, then
     * hand ownership back with bounded polling before IF is restored. */
    reset(); tables(s5_ich, sizeof(s5_ich), 0x644, 0xB2, 0xF1); seed_masks();
    sleep_ignored = change_enable_masks = true; disable_polls_needed = 7;
    CHECK(run_off(false, &off) == -EIO && !off);
    CHECK(stalls_ms == S5_SETTLE_MS + S5_RETRY_MS + 7 && !(port16[0x604] & 1));
    const char *masked = strstr(trace, "w0602=0000"), *disabled = strstr(trace, "b00b2=f0"), *restored = strstr(trace, "w0602=0121");
    CHECK(masked && disabled && restored && masked < disabled && disabled < restored);
    CHECK(strstr(all_logs, "rollback=firmware incoming=legacy") && !s5.ownership_uncertain);
    CHECK(io_at_restore == trace_len && !strcmp(irq_trace, "CSCS")); check_masks();

    /* Incoming ACPI is preserved, with each block's own enable masks. */
    reset(); tables(s5_ich, sizeof(s5_ich), 0x644, 0xB2, 0xF1); seed_masks();
    port16[0x604] = 1; sleep_ignored = true;
    CHECK(run_off(true, &off) == -EIO && !off && (port16[0x604] & 1) && !strstr(trace, "b00b2="));
    check_masks();

    /* ACPI_ENABLE cannot be sent without a rollback command. */
    reset(); tables(s5_qemu, sizeof(s5_qemu), 0, 0xB2, 0xF1); mem[FADT + 53] = 0; seal(FADT);
    CHECK(run_off(false, &off) == -EIO && !off && !strstr(trace, "b00b2="));
    CHECK(strstr(all_logs, "mode=legacy_no_disable_value") && !s5.ownership_uncertain);

    /* An enable timeout can still complete during S5: always cancel it and
     * verify hand-back, including SCI_EN implemented only in B. */
    reset(); tables(s5_ich, sizeof(s5_ich), 0x644, 0xB2, 0xF1); seed_masks();
    sleep_ignored = sci_in_b = true; enable_polls_needed = S5_ENABLE_POLLS + 1; disable_polls_needed = 4;
    CHECK(run_off(false, &off) == -EIO && !off && disable_requested);
    CHECK(stalls_ms == S5_ENABLE_POLLS + S5_SETTLE_MS + S5_RETRY_MS + 4);
    CHECK(strstr(all_logs, "mode=legacy_enable_timeout") && !s5.ownership_uncertain && !(port16[0x644] & 1)); check_masks();
    reset(); tables(s5_qemu, sizeof(s5_qemu), 0, 0xB2, 0xF1); enable_ignored = true;
    CHECK(run_off(false, &off) == -EIO && disable_requested && !s5.ownership_uncertain);

    /* Failed rollback stays explicit, events stay masked, subsequent calls
     * refuse without I/O rather than treating ownership as firmware-safe. */
    reset(); tables(s5_ich, sizeof(s5_ich), 0x644, 0xB2, 0xF1); seed_masks();
    sleep_ignored = disable_ignored = sci_in_b = true;
    CHECK(run_off(false, &off) == -EIO && !off);
    CHECK(stalls_ms == S5_SETTLE_MS + S5_RETRY_MS + S5_ENABLE_POLLS);
    CHECK(!strcmp(power_s5_fallback_state(), "ownership_uncertain") && !power_s5_fallback_qualified());
    CHECK(!port16[0x602] && !port16[0x642] && !port8[0x60E] && !port8[0x60F] && !port8[0x622] && !port8[0x623]);
    CHECK(strstr(all_logs, "rollback=ownership_uncertain") && io_at_restore == trace_len);
    unsigned before = trace_len;
    CHECK(power_s5_fallback_off(false) == -EIO && power_s5_fallback_off(true) == -EIO && trace_len == before);

    /* P2: B-only SCI_EN, non-sleep bits in both blocks and differing SLP_TYP.
     * No enable command; no incoming SLP_TYP leaks into either output. */
    static const uint8_t different[] = {0x08,'_','S','5','_',0x12,4,2,1,0};
    reset(); tables(different, sizeof(different), 0x644, 0xB2, 0xF1);
    port16[0x604] = 0x1002; port16[0x644] = 0x0901;
    CHECK(run_off(false, &off) == 0 && off && !strstr(trace, "b00b2="));
    CHECK(strstr(trace, "r0604r0644w0604=0503w0644=0103w0604=2503w0644=2103"));
    reset(); tables(different, sizeof(different), 0x644, 0xB2, 0xF1); sci_in_b = true; enable_polls_needed = 3;
    CHECK(run_off(false, &off) == 0 && off && stalls_ms == 3 && strstr(all_logs, "mode=enabled"));

    /* P2 register qualification: mixed IO/Memory PM1 blocks and wrapped GPE
     * extents are refused before any hardware access, including hint use. */
    reset(); tables(no_s5, sizeof(no_s5), 0, 0xB2, 0xF1); header(FADT, "FACP", 244, 3);
    mem[FADT + 185] = 16; put32(FADT + 188, 0x644); seal(FADT); hint_result = 0;
    CHECK(run_off(false, &off) == -ENOTSUP && !trace_len && !strcmp(power_s5_fallback_state(), "pm1b_cnt"));
    reset(); tables(s5_qemu, sizeof(s5_qemu), 0, 0xB2, 0xF1); put32(FADT + 80, 0xFFFE); seal(FADT);
    CHECK(run_off(false, &off) == -ENOTSUP && !trace_len && !strcmp(power_s5_fallback_state(), "gpe0"));
    /* Largest admitted GPE block: all 16 saved enable bytes survive failure. */
    reset(); tables(s5_qemu, sizeof(s5_qemu), 0, 0xB2, 0xF1); mem[FADT + 92] = 32; seal(FADT);
    for (unsigned i = 0; i < 16; i++) port8[0x61C + i] = (uint8_t)(i + 1);
    sleep_ignored = true;
    CHECK(run_off(false, &off) == -EIO && !s5.ownership_uncertain);
    for (unsigned i = 0; i < 16; i++) CHECK(port8[0x61C + i] == i + 1);
    puts("PASS Review round 1 P1/P2: mode/mask rollback, rollback requirement, enable timeout, uncertain ownership, B-only SCI_EN and combined non-sleep bits with separate SLP_TYP");
}
int main(void)
{
    test_parse_and_state();
    test_off_sequence();
    test_review_round_1();
    printf("power s5 fallback: %u failures\n", failures);
    return failures ? 1 : 0;
}
