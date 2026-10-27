/* fx-acpioff (2026-10-26): the ACPI S5 register fallback of call 73.
 *
 * Used only when no active power_source provider is qualified for Off
 * (T23 image 5bb73dba: ACPI tables present, firmware-owned by lead
 * decision, APM not connected because the BIOS VM quarantined at boot), or
 * after the active provider's Off returned. Provider order and sources:
 * docs/design/acpi-events-power.md, "ACPI S5 register fallback".
 *
 * What it does, after the orderly or the emergency latch (never earlier):
 * 1. once per boot a read-only, bounded parse of RSDP/RSDT(XSDT)/FADT/DSDT
 *    and the SSDTs through core/power.c acpi_soft_off_parse (every table
 *    mapped read-only through the ACPI backend's firmware-region check, at
 *    most 256 KiB each); \_S5 missing there may still come from the
 *    inventory's evaluated \_S5 (acpi/events.c acpi_s5_hint);
 * 2. no write when another owner holds the PM1 control ports in the
 *    resource registry (claimed, active, quiescing or quarantined);
 * 3. with interrupts disabled (ACPICA hwxfsleep.c: acpi_enter_sleep_state
 *    "MUST BE CALLED WITH INTERRUPTS DISABLED"; Linux acpi_power_off does
 *    local_irq_disable first): if SCI_EN is clear and the FADT names SMI_CMD
 *    and ACPI_ENABLE/ACPI_DISABLE, the ACPI_ENABLE handshake with the 3 s poll of
 *    ACPICA's acpi_enable (SMI_CMD zero means always ACPI mode, hwacpi.c
 *    acpi_hw_get_mode); in ACPI mode the fixed status bits (WAK_STS and the
 *    others, aclocal.h ACPI_BITMASK_ALL_FIXED_STATUS) are cleared and the
 *    GPE enables zeroed as acpi_hw_legacy_sleep does, so no pending wake
 *    event turns the machine back on; then SLP_TYPx, then SLP_TYPx|SLP_EN,
 *    to PM1a and PM1b (hwsleep.c, two writes "to work around poorly
 *    implemented hardware"), a bounded wait, ACPICA's one retry, -EIO;
 * 4. if S5 returns, restore saved enable masks and incoming ownership before
 *    restoring IF. A takeover requires bounded ACPI_DISABLE rollback; failed
 *    rollback keeps events masked and an explicit ownership_uncertain state.
 * No AML runs: \_PTS(5) and \_GTS are not evaluated (the risk is recorded
 * in the design document; the ACPICA-owned provider runs \_PTS and is
 * always preferred when it exists). No panic path: every refusal returns.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/abi.h>
#include <ciuki/power.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifndef POWER_HOST
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/sync.h>
#include <ciuki/registry.h>
#include <ciuki/power_source.h>
#include <ciuki/bootstat.h>
/* acpi/platform.c: read-only firmware table windows (BIOS window or E820
 * ACPI/NVS, reserved pages only). acpi/events.c: the inventory's \_S5. */
const void *acpi_firmware_table_map(uint32_t phys, uint32_t len);
void acpi_firmware_table_unmap(const void *p, uint32_t len);
int acpi_s5_hint(uint8_t *typa, uint8_t *typb);
#pragma weak acpi_firmware_table_map
#pragma weak acpi_firmware_table_unmap
#pragma weak acpi_s5_hint
static const uint8_t *s5_map(uint32_t phys, uint32_t len)
{ return acpi_firmware_table_map ? acpi_firmware_table_map(phys, len) : NULL; }
static void s5_unmap(const uint8_t *p, uint32_t len)
{ if (acpi_firmware_table_unmap) acpi_firmware_table_unmap(p, len); }
static int s5_hint(uint8_t *a, uint8_t *b) { return acpi_s5_hint ? acpi_s5_hint(a, b) : -ENOENT; }
static bool s5_acpi_decided(void) { return power_source_acpi_decided(); }
static uint32_t s5_rsdp(void) { return g_boot.acpi_rsdp_phys; }
/* The power class is skipped on safe and hardware-check boots and by the
 * startup choice (bootstat.h class 5), like every other power provider. */
static bool s5_allowed(void)
{
    return !(g_boot.flags & (CBI_F_SAFE_MODE | CBI_F_HW_CHECK)) &&
           !(g_boot.flags & (1u << (CBI_F_SKIP_SHIFT + BOOTSTAT_POWER)));
}
/* Another owner of a PM1 control port range refuses the write; firmware,
 * discovered and released entries do not (the chipset's PM block is a
 * discovered firmware resource on these machines). */
static const char *s5_port_owner(uint16_t start, unsigned len)
{
    unsigned n = registry_count();
    for (unsigned i = 0; i < n; i++) {
        const struct resource *r = registry_get(i);
        if (!r || r->type != RES_PORT || r->end <= start || r->start >= (uint32_t)start + len) continue;
        if (r->state == RS_CLAIMED || r->state == RS_ACTIVE || r->state == RS_QUIESCING || r->state == RS_QUARANTINED)
            return r->owner ? r->owner : "unnamed";
    }
    return NULL;
}
static uint16_t s5_inw(uint16_t p) { return cpu_inw(p); }
static uint8_t s5_inb(uint16_t p) { return inb(p); }
static void s5_outb(uint16_t p, uint8_t v) { outb(p, v); }
static void s5_outw(uint16_t p, uint16_t v) { cpu_outw(p, v); }
/* Bounded stall with IF=0: udelay admits 50 us per call there; without a
 * calibrated TSC the classic port 80h write (about 1 us) paces instead. */
static void s5_stall_ms(unsigned ms)
{
    for (unsigned i = 0; i < ms; i++)
        for (unsigned j = 0; j < 20; j++)
            if (udelay(50)) for (unsigned k = 0; k < 50; k++) io_wait();
}
static uint32_t s5_irq_save(void) { return irq_save(); }
static void s5_irq_restore(uint32_t f) { irq_restore(f); }
#define s5_log klog
#endif

/* PM1 control (ACPI 2.0 4.7.3.2.1, aclocal.h): SCI_EN 0x0001, SLP_TYP
 * 0x1C00 at bit 10, SLP_EN 0x2000 (write-only). PM1 status (4.7.3.1.1):
 * the fixed status bits ACPICA clears before sleeping, WAK_STS included. */
#define S5_SCI_EN 0x0001u
#define S5_SLP_MASK 0x3C00u
#define S5_SLP_EN 0x2000u
#define S5_ALL_FIXED_STATUS 0xC731u
#define S5_ENABLE_POLLS 3000u      /* ACPICA acpi_enable: 3 s in 1 ms steps */
#define S5_SETTLE_MS 2000u         /* first wait after SLP_EN (events.c uses the same) */
#define S5_RETRY_MS 1000u          /* after ACPICA's one SLP_EN rewrite */
#define S5_GPE_MAX 32u             /* bytes per GPE block admitted (16 registers) */

enum { S5_IDLE, S5_PARSING, S5_PARSED };
static struct {
    unsigned state;
    bool qualified, final, ownership_uncertain;
    const char *why;
    struct acpi_soft_off off;
} s5;

#define S5_MAPS 96u
struct s5_window { uint32_t phys, len; const uint8_t *p; };
static struct s5_window s5_windows[S5_MAPS];
static unsigned s5_window_count;
static const uint8_t *s5_map_cb(void *ctx, uint32_t phys, uint32_t len)
{
    (void)ctx;
    for (unsigned i = 0; i < s5_window_count; i++)
        if (s5_windows[i].phys == phys && s5_windows[i].len >= len) return s5_windows[i].p;
    if (s5_window_count == S5_MAPS) return NULL;
    const uint8_t *p = s5_map(phys, len);
    if (!p) return NULL;
    s5_windows[s5_window_count++] = (struct s5_window){phys, len, p};
    return p;
}
static void s5_unmap_all(void)
{
    for (unsigned i = 0; i < s5_window_count; i++) s5_unmap(s5_windows[i].p, s5_windows[i].len);
    s5_window_count = 0;
}
static bool s5_why_is_s5(void) { return s5.why && s5.why[0] == 's' && s5.why[1] == '5' && !s5.why[2]; }
static const char *s5_source_name(unsigned source)
{
    return source == ACPI_S5_SOURCE_DSDT ? "dsdt" : source == ACPI_S5_SOURCE_SSDT ? "ssdt" :
           source == ACPI_S5_SOURCE_ACPICA ? "acpica" : "none";
}
static void s5_log_available(void)
{
    const struct acpi_soft_off *o = &s5.off;
    s5_log("[power] acpi s5 fallback available source=%s pm1a_cnt=%04x pm1b_cnt=%04x cnt_len=%u smi_cmd=%04x "
           "acpi_enable=%02x typ=%u/%u pm1_evt=%04x/%04x evt_len=%u gpe0=%04x/%u gpe1=%04x/%u sci=%u fadt_rev=%u",
           s5_source_name(o->source), o->pm1a_cnt, o->pm1b_cnt, o->pm1_cnt_len, o->smi_cmd, o->acpi_enable,
           o->slp_typa, o->slp_typb, o->pm1a_evt, o->pm1b_evt, o->pm1_evt_len, o->gpe0, o->gpe0_len,
           o->gpe1, o->gpe1_len, o->sci_int, o->fadt_revision);
}
/* The inventory's evaluated \_S5 is consulted until the inventory decided
 * ownership; afterwards a missing \_S5 is final. */
static void s5_hint_check(void)
{
    uint8_t a = 0, b = 0;
    if (!s5_hint(&a, &b) && a <= 7 && b <= 7) {
        s5.off.slp_typa = a; s5.off.slp_typb = b; s5.off.source = ACPI_S5_SOURCE_ACPICA;
        s5.qualified = s5.final = true; s5.why = "ok";
        s5_log_available();
    } else if (s5_acpi_decided()) {
        s5.final = true;
        s5_log("[power] acpi s5 fallback unavailable step=s5 (no \\_S5 package in DSDT, SSDTs or the inventory)");
    }
}
static void s5_parse(void)
{
    struct acpi_soft_off off;
    const char *why = "rsdp";
    memset(&off, 0, sizeof(off));
    int e = acpi_soft_off_parse(s5_map_cb, NULL, s5_rsdp(), &off, &why);
    s5_unmap_all();
    s5.off = off; s5.why = why;
    if (!e) { s5.qualified = s5.final = true; s5_log_available(); }
    else if (s5_why_is_s5()) s5_hint_check();
    else {
        s5.final = true;
        s5_log("[power] acpi s5 fallback unavailable step=%s", why);
    }
    s5.state = S5_PARSED;
}
bool power_s5_fallback_qualified(void)
{
    if (s5.ownership_uncertain) return false;
    if (!s5_allowed()) { s5.why = "policy"; return false; }
    uint32_t f = s5_irq_save();
    unsigned state = s5.state;
    if (state == S5_IDLE) s5.state = S5_PARSING;
    s5_irq_restore(f);
    if (state == S5_PARSING) return false;
    if (state == S5_IDLE) s5_parse();
    else if (!s5.final) s5_hint_check();
    return s5.qualified;
}
const char *power_s5_fallback_state(void)
{
    if (s5.ownership_uncertain) return "ownership_uncertain";
    if (s5.qualified) return "available";
    if (s5.state == S5_PARSING) return "parsing";
    if (s5.state == S5_PARSED && !s5.final) return "pending";
    return s5.why ? s5.why : "unparsed";
}
/* Legacy/ACPI select (ACPI 2.0 4.7.2.5; ACPICA hwacpi.c): SCI_EN already
 * set, or no SMI_CMD, is ACPI mode; otherwise ACPI_ENABLE to SMI_CMD and
 * poll SCI_EN. A platform that never answers still gets the SLP_EN write,
 * in legacy mode, because the alternative is the user's power button. */
static uint16_t s5_read_control(const struct acpi_soft_off *o)
{
    uint16_t value = s5_inw(o->pm1a_cnt);
    if (o->pm1b_cnt) value |= s5_inw(o->pm1b_cnt);
    return (uint16_t)(value & ~S5_SLP_MASK);
}
static const char *s5_enter_acpi_mode(const struct acpi_soft_off *o, bool *acpi_mode,
                                     bool *enable_attempted)
{
    *acpi_mode = true;
    if (s5_read_control(o) & S5_SCI_EN) return "acpi";
    if (!o->smi_cmd) return "acpi_no_smi_cmd";
    if (!o->acpi_enable) { *acpi_mode = false; return "legacy_no_enable_value"; }
    if (!o->acpi_disable) { *acpi_mode = false; return "legacy_no_disable_value"; }
    *enable_attempted = true;
    s5.ownership_uncertain = true; /* Remains set until hand-back is proved. */
    s5_outb(o->smi_cmd, o->acpi_enable);
    for (unsigned i = 0; i < S5_ENABLE_POLLS; i++) {
        if (s5_read_control(o) & S5_SCI_EN) return "enabled";
        s5_stall_ms(1);
    }
    *acpi_mode = false;
    return "legacy_enable_timeout";
}
/* Save before ACPI_ENABLE: firmware can change these masks during takeover.
 * The parser admits only 4-byte PM1 events and even GPE blocks <= 32 bytes. */
struct s5_saved {
    uint16_t control, pm1_enable[2];
    uint8_t gpe_enable[2][S5_GPE_MAX / 2];
};
static void s5_save_enables(const struct acpi_soft_off *o, struct s5_saved *saved)
{
    const uint16_t events[2] = {o->pm1a_evt, o->pm1b_evt}, blocks[2] = {o->gpe0, o->gpe1};
    const unsigned lengths[2] = {o->gpe0_len, o->gpe1_len};
    for (unsigned b = 0; b < 2; b++) {
        if (events[b]) saved->pm1_enable[b] = s5_inw((uint16_t)(events[b] + 2));
        for (unsigned i = 0; blocks[b] && i < lengths[b] / 2; i++)
            saved->gpe_enable[b][i] = s5_inb((uint16_t)(blocks[b] + lengths[b] / 2 + i));
    }
}
static void s5_set_enables(const struct acpi_soft_off *o, const struct s5_saved *saved)
{
    const uint16_t events[2] = {o->pm1a_evt, o->pm1b_evt}, blocks[2] = {o->gpe0, o->gpe1};
    const unsigned lengths[2] = {o->gpe0_len, o->gpe1_len};
    for (unsigned b = 0; b < 2; b++) {
        if (events[b]) s5_outw((uint16_t)(events[b] + 2), saved ? saved->pm1_enable[b] : 0);
        for (unsigned i = 0; blocks[b] && i < lengths[b] / 2; i++)
            s5_outb((uint16_t)(blocks[b] + lengths[b] / 2 + i), saved ? saved->gpe_enable[b][i] : 0);
    }
}
/* ACPI mode only: these registers belong to OSPM there (in legacy mode the
 * firmware's SMI handler owns them). PM1x status is the first half of the
 * event block; a GPE block is status bytes then enable bytes. */
static void s5_clear_events(const struct acpi_soft_off *o)
{
    if (o->pm1_evt_len >= 4) {
        if (o->pm1a_evt) s5_outw(o->pm1a_evt, S5_ALL_FIXED_STATUS);
        if (o->pm1b_evt) s5_outw(o->pm1b_evt, S5_ALL_FIXED_STATUS);
    }
    const uint16_t blocks[2] = {o->gpe0, o->gpe1};
    const unsigned lengths[2] = {o->gpe0_len, o->gpe1_len};
    for (unsigned b = 0; b < 2; b++) {
        unsigned half = lengths[b] / 2;
        if (!blocks[b] || !lengths[b] || (lengths[b] & 1) || lengths[b] > S5_GPE_MAX) continue;
        for (unsigned i = 0; i < half; i++) s5_outb((uint16_t)(blocks[b] + half + i), 0);
        for (unsigned i = 0; i < half; i++) s5_outb((uint16_t)(blocks[b] + i), 0xFF);
    }
}
static void s5_write(const struct acpi_soft_off *o, uint16_t a, uint16_t b)
{
    s5_outw(o->pm1a_cnt, a);
    if (o->pm1b_cnt) s5_outw(o->pm1b_cnt, b);
}
int power_s5_fallback_off(bool forced)
{
    if (s5.ownership_uncertain) return -EIO;
    if (!power_s5_fallback_qualified()) return -ENOTSUP;
    const struct acpi_soft_off *o = &s5.off;
    const char *owner = s5_port_owner(o->pm1a_cnt, o->pm1_cnt_len);
    if (!owner && o->pm1b_cnt) owner = s5_port_owner(o->pm1b_cnt, o->pm1_cnt_len);
    if (owner) {
        s5_log("[power] acpi s5 fallback refused: PM1 control ports held by %s; turn the computer off with its power button", owner);
        return -ENOTSUP;
    }
    s5_log("[power] acpi s5 fallback: source=%s typ=%u/%u pm1a_cnt=%04x pm1b_cnt=%04x forced=%u", s5_source_name(o->source),
           o->slp_typa, o->slp_typb, o->pm1a_cnt, o->pm1b_cnt, (unsigned)forced);
    uint32_t f = s5_irq_save();
    struct s5_saved saved = {.control = s5_read_control(o)};
    s5_save_enables(o, &saved);
    bool acpi_mode, enable_attempted = false;
    const char *mode = s5_enter_acpi_mode(o, &acpi_mode, &enable_attempted);
    if (acpi_mode) s5_clear_events(o);
    /* ACPI 1.0b 4.7: A OR B except the block-specific SLP_TYP fields. */
    uint16_t a = s5_read_control(o), b = a;
    a |= (uint16_t)(o->slp_typa << 10); b |= (uint16_t)(o->slp_typb << 10);
    s5_log("[power] acpi s5 fallback: mode=%s pm1a=%04x pm1b=%04x events=%s", mode, a, b, acpi_mode ? "cleared" : "firmware");
    s5_write(o, a, b);
    a |= S5_SLP_EN; b |= S5_SLP_EN;
    s5_write(o, a, b);
    s5_stall_ms(S5_SETTLE_MS);
    s5_write(o, a, b);
    s5_stall_ms(S5_RETRY_MS);
    /* S5 failed. Hand firmware its policy back before restoring IF, even
     * after a timed-out enable request (the transition may be delayed).
     * Incoming ACPI mode is never disabled by this fallback. */
    if (enable_attempted) {
        s5_set_enables(o, NULL);
        s5_write(o, saved.control, saved.control);
        s5_outb(o->smi_cmd, o->acpi_disable);
        for (unsigned i = 0; i < S5_ENABLE_POLLS; i++) {
            if (!(s5_read_control(o) & S5_SCI_EN)) { s5.ownership_uncertain = false; break; }
            s5_stall_ms(1);
        }
        if (!s5.ownership_uncertain) s5_set_enables(o, &saved);
        s5_log("[power] acpi s5 fallback: rollback=%s incoming=legacy", s5.ownership_uncertain ? "ownership_uncertain" : "firmware");
    } else if (acpi_mode) {
        s5_write(o, saved.control, saved.control);
        s5_set_enables(o, &saved);
    }
    s5_irq_restore(f);
    s5_log("[power] acpi s5 fallback: machine still running after %u ms mode=%s", S5_SETTLE_MS + S5_RETRY_MS, mode);
    return -EIO;
}
