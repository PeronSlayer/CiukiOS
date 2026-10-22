/* f1-94: SMI attribution around long interrupt-off sections, the time
 * relation between blackouts and our periodic device activity, and the
 * LEGACY_USB_EN handoff on the ICH3-M. Pure decisions: core/smi_model.c.
 * Sources, per-bit decisions and records: docs/validation/f1-94-irqoff-blackout.md.
 * Cost: one SMI_STS read per PIT tick and per section of 1 ms or more,
 * one TSC store per marked activity; no I/O while not armed.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/registry.h>
#include <ciuki/timing.h>
#include <ciuki/usb.h>
#include <ciuki/smi.h>

#define SMI_SUSPECTS 3u        /* usb-hid turn, USB root scan, EC port I/O */
/* f1-94 amendment 1: the EC poll setting (ms, 0 off, -1 no EC) in the record. */
extern int thinkpad_ec_poll_state(void) __attribute__((weak));
#define SMI_REGS_EVERY 15u     /* windows between unchanged register records */

struct smi_window {
    uint32_t sts, tsc, unproven, gaps, sources, rise, leads;
    uint32_t near[SMI_SUSPECTS];
    uint64_t max, min, period_min, period_max;
    uint64_t lead_min[SMI_SUSPECTS], lead_max[SMI_SUSPECTS];
};
static const struct smi_chipset *chip;
static uint16_t base;          /* PMBASE once qualified; 0 means no SMI_STS reads */
static uint32_t en_cache, causal_cache, mitigated, rearms;
static uint32_t tick_sts;
static uint64_t tick_tsc, adjacent_start, adjacent_end, last_gap;
static bool tick_valid;
/* TSC >> 8 (32-bit, single store): about 1100 s before wrapping at 1 GHz. */
static volatile uint32_t mark_last[SMI_SUSPECTS], mark_count[SMI_SUSPECTS];
static struct smi_window win = { .min = UINT64_MAX,
    .lead_min = { UINT64_MAX, UINT64_MAX, UINT64_MAX } };
static struct { uint32_t en, rise; uint16_t legkey[3]; unsigned windows; bool printed; } regs;

static uint32_t stamp8(uint64_t tsc) { uint32_t v = (uint32_t)(tsc >> 8); return v ? v : 1; }
static uint32_t us(uint64_t cycles) { return g_tsc_per_ms ? (uint32_t)(cycles * 1000 / g_tsc_per_ms) : 0; }

static uint32_t cfg_read(unsigned dev, unsigned fn, unsigned off)
{
    uint32_t f = irq_save();
    outl(0xcf8, 0x80000000u | (uint32_t)dev << 11 | (uint32_t)fn << 8 | (off & 0xfc));
    uint32_t v = inl(0xcfc);
    irq_restore(f);
    return v;
}

void smi_mark(enum smi_suspect s)
{
    if (!g_cpu_tsc) return;
    unsigned i = s == SMI_HID_BEGIN || s == SMI_HID_END ? 0 : s == SMI_USB_SCAN ? 1 : 2;
    __atomic_store_n(&mark_last[i], stamp8(read_tsc_unserialized()), __ATOMIC_RELAXED);
    if (s != SMI_HID_END) __atomic_fetch_add(&mark_count[i], 1u, __ATOMIC_RELAXED);
}

/* PIT IRQ, IF clear. A timer gap above 2 ms is a blackout whether it fell
 * into an interrupt-off section or not (a 2.1 ms SMI always loses a tick). */
void smi_tick(uint64_t now, uint64_t previous)
{
    bool blackout = previous && g_tsc_per_ms && now > previous && now - previous > 2 * g_tsc_per_ms;
    if (base) {
        uint32_t sts = inl(base + 0x34);
        if (tick_valid) {
            uint32_t rise = sts & ~tick_sts;
            win.rise |= rise;
            if (blackout && tick_tsc == previous) win.sources |= rise & causal_cache;
        }
        tick_sts = sts; tick_tsc = now; tick_valid = true;
    }
    if (!blackout) return;
    win.gaps++;
    if (last_gap && now > last_gap) {
        uint64_t spacing = now - last_gap;
        if (!win.period_min || spacing < win.period_min) win.period_min = spacing;
        if (spacing > win.period_max) win.period_max = spacing;
    }
    last_gap = now;
    /* Activity in [previous - 1 ms, now] touches the blackout's window. */
    uint32_t now8 = stamp8(now), span8 = (uint32_t)((now - previous + g_tsc_per_ms) >> 8);
    for (unsigned i = 0; i < SMI_SUSPECTS; i++) {
        uint32_t m = __atomic_load_n(&mark_last[i], __ATOMIC_RELAXED);
        if (m && (uint32_t)(now8 - m) <= span8) win.near[i]++;
    }
}

/* timing.c no-work stamp pair (irq_budget_stamp): adjacent TSC reads. */
void smi_adjacent_gap(uint64_t start, uint64_t end)
{
    adjacent_start = start; adjacent_end = end;
}

/* crit_end, IF clear, sections of 1 ms or more only. */
void smi_section_end(const struct smi_section *x)
{
    struct smi_sample s = { .section = *x, .en = en_cache };
    if (base) {
        s.after = inl(base + 0x34);
        s.before = tick_sts;
        s.before_valid = tick_valid && tick_tsc <= x->start;
    }
    if (!s.section.gap_bounded && adjacent_end && adjacent_start >= x->start &&
        adjacent_end <= x->end && adjacent_end > adjacent_start) {
        s.section.gap = adjacent_end - adjacent_start;
        s.section.gap_start = adjacent_start;
        s.section.gap_bounded = true;
    }
    uint32_t sources;
    enum smi_class k = smi_classify(chip, &s, g_tsc_per_ms, &sources);
    if (k == SMI_CLASS_NONE) return;
    if (k == SMI_CLASS_UNPROVEN) { win.unproven++; return; }
    if (k == SMI_CLASS_STATUS) win.sts++; else win.tsc++;
    win.sources |= sources;
    uint64_t length = k == SMI_CLASS_TSC ? s.section.gap : x->end - x->start;
    if (length > win.max) win.max = length;
    if (length < win.min) win.min = length;
    if (k != SMI_CLASS_TSC) return;
    /* Precise blackout start: the mark that opens the gap. */
    uint32_t at = stamp8(s.section.gap_start);
    win.leads++;
    for (unsigned i = 0; i < SMI_SUSPECTS; i++) {
        uint32_t m = __atomic_load_n(&mark_last[i], __ATOMIC_RELAXED);
        if (!m || (int32_t)(at - m) < 0) continue;
        uint64_t lead = (uint64_t)(at - m) << 8;
        if (lead < win.lead_min[i]) win.lead_min[i] = lead;
        if (lead > win.lead_max[i]) win.lead_max[i] = lead;
    }
}

static unsigned uhci_owned(const struct smi_chipset *c)
{
    unsigned owned = 0;
    for (unsigned i = 0; i < usb_count(); i++) {
        const struct usb_host *h = usb_get(i);
        if (!h || !h->owned || !h->ready || h->quarantined || h->pci.bus || h->pci.dev != 29 ||
            h->pci.fn >= 3 || !(c->uhci_functions & (1u << h->pci.fn)) ||
            h->pci.vendor != 0x8086 || h->pci.device != c->uhci_device[h->pci.fn]) continue;
        owned |= 1u << h->pci.fn;
    }
    return owned;
}

static void bit_names(char *out, size_t size, uint32_t bits)
{
    static const struct { uint32_t bit; const char *name; } names[] = {
        { SMI_EN_GBL, "GBL_SMI_EN" }, { SMI_EN_BIOS, "BIOS_EN" },
        { SMI_EN_LEGACY_USB, "LEGACY_USB_EN" }, { SMI_EN_SLP, "SLP_SMI_EN" },
        { SMI_EN_APMC, "APMC_EN" }, { SMI_EN_SWSMI_TMR, "SWSMI_TMR_EN" },
        { SMI_EN_MCSMI, "MCSMI_EN" }, { SMI_EN_TCO, "TCO_EN" }, { SMI_EN_PERIODIC, "PERIODIC_EN" },
    };
    size_t at = 0;
    out[0] = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(names); i++) {
        if (!(bits & names[i].bit)) continue;
        int n = ksnprintf(out + at, size - at, "%s%s", at ? "," : "", names[i].name);
        if (n < 0 || (size_t)n >= size - at) break;
        at += (size_t)n;
    }
    if (!at) ksnprintf(out, size, "none");
}

/* After usb_init: every UHCI handoff (USB_LEGKEY write) has completed. */
void smi_attribution_init(void)
{
    struct registry_smi p;
    bool decoded = registry_smi_snapshot(&p);
    const struct smi_chipset *c = p.supported ? smi_chipset(p.lpc_id) : 0;
    if (!decoded || !c) {
        klog("[kernel] smi_config chip=%s lpc=%08x decoded=%u sampler=off",
             c ? c->name : "unsupported", p.lpc_id, decoded);
        return;
    }
    /* Read-only context for the source hunt (290716-001 9.8.1.5, 9.8.3.13,
     * 9.8.3.16, 9.8.3.18): GPI routing to SMI, GPI enables, device traps. */
    uint16_t gpe1_en = cpu_inw(p.base + 0x2e), mon = cpu_inw(p.base + 0x40);
    uint16_t devtrap = cpu_inw(p.base + 0x48);
    uint32_t gpi_rout = cfg_read(31, 0, 0xb8);
    klog("[kernel] smi_config chip=%s pmbase=%04x en=%08x sts=%08x pm1_cnt=%04x pm1_en=%04x gen_pmcon1=%04x gpe0_en=%04x gpe1_en=%04x gpi_rout=%08x devtrap=%04x mon_smi=%04x legkey=%04x/%04x/%04x",
         c->name, p.base, p.smi_en, p.smi_sts, p.pm1_cnt, p.pm1_en, p.gen_pmcon1, p.gpe0_en,
         gpe1_en, gpi_rout, devtrap, mon, p.usb_legkey[0], p.usb_legkey[1], p.usb_legkey[2]);
    /* f1-100, read-only: the I/O monitors 4-7 (290716-001 9.8.1.6-9.8.1.8,
     * D31:F0 C0h-CDh; MON_SMI 9.8.3.16 holds their status/enable), GPE1_STS
     * (9.8.3.12) and the last I/O cycle latched at SMI# (BUS_ADDR_TRACK,
     * BUS_CYC_TRACK, 9.8.3.19/20). Names what monitor 4 watched. */
    uint32_t fwd = cfg_read(31, 0, 0xc0), rng45 = cfg_read(31, 0, 0xc4);
    uint32_t rng67 = cfg_read(31, 0, 0xc8), msk = cfg_read(31, 0, 0xcc);
    klog("[kernel] smi_monitors trp_fwd=%02x mon_rng=%04x/%04x/%04x/%04x mon_msk=%04x gpe1_sts=%04x last_io=%04x/%02x",
         fwd & 0xff, rng45 & 0xffff, rng45 >> 16, rng67 & 0xffff, rng67 >> 16, msk & 0xffff,
         cpu_inw(p.base + 0x2c), cpu_inw(p.base + 0x4c), inb(p.base + 0x4e));
    uint32_t f = irq_save();
    chip = c; base = p.base; en_cache = p.smi_en; causal_cache = smi_causal_status(c, p.smi_en);
    tick_valid = false;
    irq_restore(f);
    bool allowed = !(g_boot.flags & (CBI_F_SAFE_MODE | CBI_F_HW_CHECK));
    struct smi_plan plan = smi_mitigation_plan(c, p.smi_en, p.usb_present, uhci_owned(c),
                                               p.usb_legkey, allowed);
    uint32_t before = p.smi_en, after = p.smi_en;
    const char *result = smi_reason_name(plan.reason);
    if (plan.clear) {
        /* One read-modify-write of the enables, nothing else: no status
         * acknowledgement, no USB_LEGKEY/PM1/GPE write, EOS kept as read. */
        f = irq_save();
        before = inl(base + 0x30);
        uint32_t clear = plan.clear & before;
        if (clear) outl(base + 0x30, smi_en_write_value(before, clear));
        after = inl(base + 0x30);
        if (clear && !(after & clear)) mitigated = clear;
        en_cache = after; causal_cache = smi_causal_status(c, after);
        irq_restore(f);
        result = mitigated ? "cleared" : (before & plan.clear) ? "readback_failed" : "not_armed";
    }
    char kept[128];
    bit_names(kept, sizeof(kept), plan.kept & after);
    klog("[kernel] smi_mitigate result=%s clear=%08x en_before=%08x en_after=%08x kept=%s",
         result, mitigated, before, after, kept);
}

void smi_window_emit(uint64_t window_cycles, bool verbose)
{
    struct smi_window w;
    uint32_t counts[SMI_SUSPECTS];
    uint32_t f = irq_save();
    w = win;
    memset(&win, 0, sizeof(win));
    win.min = UINT64_MAX;
    for (unsigned i = 0; i < SMI_SUSPECTS; i++) {
        win.lead_min[i] = UINT64_MAX;
        counts[i] = __atomic_exchange_n(&mark_count[i], 0u, __ATOMIC_RELAXED);
    }
    irq_restore(f);
    uint32_t en = 0, sts = 0;
    uint16_t legkey[3] = { 0, 0, 0 };
    if (base) {
        en = inl(base + 0x30); sts = inl(base + 0x34);
        for (unsigned i = 0; i < 3; i++)
            if (chip->uhci_functions & (1u << i)) legkey[i] = (uint16_t)cfg_read(29, i, 0xc0);
        f = irq_save();
        en_cache = en; causal_cache = smi_causal_status(chip, en);
        irq_restore(f);
        /* The firmware may re-arm what we cleared: report it, never fight it. */
        if (en & mitigated) rearms++;
    }
    bool events = w.gaps || w.sts || w.tsc || w.unproven;
    if (events && (base || verbose) && g_tsc_per_ms) {
        uint32_t window_ms = (uint32_t)(window_cycles / g_tsc_per_ms);
        uint32_t rate = window_ms ? (uint32_t)((uint64_t)w.gaps * 100000u / window_ms) : 0;
        klog_periodic("[kernel] smi count=%u sources=%08x max_us=%u min_us=%u sts=%u tsc=%u unproven=%u gaps=%u rate=%u.%02u/s period_ms=%u/%u",
            w.sts + w.tsc, w.sources, us(w.max), w.min == UINT64_MAX ? 0 : us(w.min), w.sts, w.tsc,
            w.unproven, w.gaps, rate / 100, rate % 100,
            us(w.period_min) / 1000, us(w.period_max) / 1000);
        uint32_t lo[SMI_SUSPECTS], hi[SMI_SUSPECTS];
        for (unsigned i = 0; i < SMI_SUSPECTS; i++) {
            lo[i] = w.lead_min[i] == UINT64_MAX ? 0 : us(w.lead_min[i]);
            hi[i] = us(w.lead_max[i]);
        }
        int poll = thinkpad_ec_poll_state ? thinkpad_ec_poll_state() : -1;
        char ec_poll[12];
        if (poll > 0) ksnprintf(ec_poll, sizeof(ec_poll), "%u", (unsigned)poll);
        else ksnprintf(ec_poll, sizeof(ec_poll), "%s", poll ? "none" : "off");
        /* f1-100: last I/O cycle latched when SMI# last went active
         * (BUS_ADDR_TRACK/BUS_CYC_TRACK, read-only; 0 without the sampler). */
        uint16_t last_io = base ? cpu_inw(base + 0x4c) : 0;
        uint8_t last_cyc = base ? inb(base + 0x4e) : 0;
        klog_periodic("[kernel] smi_coincide gaps=%u hid_near=%u scan_near=%u ec_near=%u hid_turns=%u scans=%u ec_io=%u ec_poll=%s leads=%u hid_lead_us=%u/%u scan_lead_us=%u/%u ec_lead_us=%u/%u last_io=%04x/%02x",
            w.gaps, w.near[0], w.near[1], w.near[2], counts[0], counts[1], counts[2], ec_poll, w.leads,
            lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], last_io, last_cyc);
    }
    if (!base) return;
    regs.rise |= w.rise;
    bool changed = !regs.printed || en != regs.en || memcmp(legkey, regs.legkey, sizeof(legkey));
    if (!changed && !(en & mitigated) && ++regs.windows < SMI_REGS_EVERY) return;
    klog_periodic("[kernel] smi_regs en=%08x sts=%08x rise=%08x mitigated=%08x rearm=%u legkey=%04x/%04x/%04x",
        en, sts, regs.rise, mitigated, rearms, legkey[0], legkey[1], legkey[2]);
    regs.en = en; memcpy(regs.legkey, legkey, sizeof(legkey));
    regs.rise = 0; regs.windows = 0; regs.printed = true;
}
