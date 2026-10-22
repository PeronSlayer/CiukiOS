/* f1-94: production SMI attribution (core/smi_model.c, core/smi.c) with
 * only CPU/PCI/PM port boundaries simulated. Synthetic host evidence: it
 * proves classification, the single enable write and the records, never a
 * physical SMI source. 1 TSC cycle = 1 ns (g_tsc_per_ms = 1000000).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define CIUKI_CPU_H
#include <ciuki/kernel.h>
#include <ciuki/task.h>
#include <ciuki/timing.h>
#define P2V(p) ((void *)(uintptr_t)(p))
#define V2P(p) ((uint32_t)(uintptr_t)(p))
static uint32_t irq_save(void);
static void irq_restore(uint32_t);
#include <ciuki/usb.h>
#include <ciuki/smi.h>

struct ciuki_boot_info g_boot;
bool g_cpu_tsc = true;
uint64_t g_tsc_per_ms = 1000000;
static uint64_t cycles = 1;
static uint32_t cpu_flags = 0x200;
static uint32_t pci_address, enable, status, gpi_rout = 0x00020000;
static uint16_t legkey[3] = { 0x2000, 0x2000, 0x2000 };
static unsigned pm_reads, pm_writes, cfg_reads;
static uint32_t written;
static bool ignore_write;
static struct registry_smi snap;
static bool snap_ok;
static struct usb_host hosts[3];
static char logbuf[8192];
static size_t logat;

static uint32_t irq_save(void) { uint32_t f = cpu_flags; cpu_flags = 0; return f; }
static void irq_restore(uint32_t f) { cpu_flags = f; }
static uint64_t read_tsc_unserialized(void) { return cycles; }
static void outl(uint16_t port, uint32_t value)
{
    if (port == 0xcf8) { assert(!cpu_flags); pci_address = value; return; }
    /* The only PM write in the module: SMI_EN, IF clear, once per boot. */
    assert(port == 0x1030 && !cpu_flags);
    pm_writes++; written = value;
    if (!ignore_write) enable = value;
}
static uint32_t inl(uint16_t port)
{
    if (port == 0xcfc) {
        assert(!cpu_flags); cfg_reads++;
        unsigned dev = pci_address >> 11 & 31, fn = pci_address >> 8 & 7, off = pci_address & 0xfc;
        assert((pci_address & 0xffff0000u) == 0x80000000u);
        if (dev == 31 && !fn && off == 0xb8) return gpi_rout;
        if (dev == 29 && fn < 3 && off == 0xc0) return 0xffff0000u | legkey[fn];
        assert(0);
    }
    pm_reads++;
    if (port == 0x1030) return enable;
    if (port == 0x1034) return status;
    assert(0); return 0;
}
static uint16_t cpu_inw(uint16_t port)
{
    pm_reads++;
    switch (port) {
    case 0x102e: return 0x0080;
    case 0x1040: return 0x0000;
    case 0x1048: return 0x0000;
    default: assert(0); return 0;
    }
}
bool registry_smi_snapshot(struct registry_smi *out) { *out = snap; return snap_ok; }
static int ec_poll_state = 1000;
int thinkpad_ec_poll_state(void) __attribute__((weak)); /* as smi.c sees it */
int thinkpad_ec_poll_state(void) { return ec_poll_state; }
unsigned usb_count(void) { return 3; }
const struct usb_host *usb_get(unsigned i) { return i < 3 ? &hosts[i] : 0; }
static void record(const char *fmt, va_list ap)
{
    char line[512];
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    assert(n > 0 && n < 240);
    assert(logat + (size_t)n + 2 < sizeof(logbuf));
    memcpy(logbuf + logat, line, (size_t)n); logat += (size_t)n;
    logbuf[logat++] = '\n'; logbuf[logat] = 0;
}
void klog(const char *fmt, ...) { va_list ap; va_start(ap, fmt); record(fmt, ap); va_end(ap); }
void klog_periodic(const char *fmt, ...) { va_list ap; va_start(ap, fmt); record(fmt, ap); va_end(ap); }

#include "../../src/kernel/core/smi_model.c"
#include "../../src/kernel/core/smi.c"

/* Owner's T23 (b453bc92 era, f0-35 H page): SMI_EN 0000402b, SMI_STS 00000900. */
static void fixture(void)
{
    g_boot = (struct ciuki_boot_info){ .pci_bios = 0x101, .flags = 0 };
    cycles = 1; cpu_flags = 0x200; g_tsc_per_ms = 1000000; g_cpu_tsc = true;
    enable = 0x402b; status = 0x900; ignore_write = false; written = 0;
    pm_reads = pm_writes = cfg_reads = 0;
    for (unsigned i = 0; i < 3; i++) {
        legkey[i] = 0x2000;
        hosts[i] = (struct usb_host){ .ready = true, .owned = true, .pci = { .bus = 0, .dev = 29, .fn = i,
            .vendor = 0x8086, .device = i == 0 ? 0x2482 : i == 1 ? 0x2484 : 0x2487 } };
    }
    snap = (struct registry_smi){ .supported = true, .decoded = true, .lpc_id = 0x248c8086,
        .pmbase_raw = 0x1001, .smi_en = 0x402b, .smi_sts = 0x900, .base = 0x1000,
        .gen_pmcon1 = 0x0000, .pm1_sts = 0x0001, .pm1_en = 0x0100, .pm1_cnt = 0x0000,
        .gpe0_sts = 0, .gpe0_en = 0x0000, .usb_legkey = { 0x2000, 0x2000, 0x2000 },
        .acpi_cntl = 0x10, .usb_present = 7 };
    snap_ok = true;
    /* Module state: a fresh boot. */
    chip = 0; base = 0; en_cache = causal_cache = mitigated = rearms = 0;
    tick_sts = 0; tick_tsc = adjacent_start = adjacent_end = last_gap = 0; tick_valid = false;
    memset((void *)mark_last, 0, sizeof(mark_last)); memset((void *)mark_count, 0, sizeof(mark_count));
    memset(&win, 0, sizeof(win)); win.min = UINT64_MAX;
    for (unsigned i = 0; i < SMI_SUSPECTS; i++) win.lead_min[i] = UINT64_MAX;
    memset(&regs, 0, sizeof(regs));
    logat = 0; logbuf[0] = 0;
}

static void chipset_tests(void)
{
    const struct smi_chipset *c = smi_chipset(0x248c8086);
    assert(c && !strcmp(c->name, "ICH3-M") && c->qualified && c->en_rw == 0x687f);
    assert(c->uhci_functions == 7 && c->uhci_device[2] == 0x2487);
    /* ICH3-S, ICH2-M, ICH4-M, PIIX4 and VIA have no checked map here. */
    static const uint32_t other[] = { 0x24808086, 0x244c8086, 0x24cc8086, 0x71108086, 0x30741106, 0 };
    for (unsigned i = 0; i < ARRAY_SIZE(other); i++) assert(!smi_chipset(other[i]));
    /* T23 enables: GBL, EOS, LEGACY_USB, APMC, PERIODIC. MCSMI (bit 11 of
     * the observed 00000900) is not causal: MCSMI_EN is clear. */
    assert(smi_causal_status(c, 0x402b) == 0x1f728);
    assert(!(smi_causal_status(c, 0x402b) & SMI_STS_MCSMI));
    assert(smi_causal_status(c, 0x402a) == 0);           /* GBL_SMI_EN clear */
    assert(smi_causal_status(0, 0x402b) == 0);
    assert(smi_causal_status(c, 0x687f) == c->sts_defined);
    puts("PASS f1-94 SMI_EN map per chipset id: ICH3-M only, T23 causal status 0001f728, MCSMI not causal, GBL off -> none");
}

static void plan_tests(void)
{
    const struct smi_chipset *c = smi_chipset(0x248c8086);
    const uint16_t owned[3] = { 0x2000, 0x2000, 0x2000 };
    struct smi_plan p = smi_mitigation_plan(c, 0x402b, 7, 7, owned, true);
    assert(p.reason == SMI_PLAN_CLEAR && p.clear == SMI_EN_LEGACY_USB && p.kept == 0x4021);
    assert(smi_en_write_value(0x402b, p.clear) == 0x4023);
    /* EOS stays as read (0 while an SMI is being arbitrated), BIOS_RLS is
     * never written, reserved bits are preserved. */
    assert(smi_en_write_value(0x4029, SMI_EN_LEGACY_USB) == 0x4021);
    assert(smi_en_write_value(0x80000088u, SMI_EN_LEGACY_USB) == 0x80000000u);
    const uint16_t armed[3] = { 0x2000, 0x2010, 0x2000 }, trap[3] = { 0x0001, 0x2000, 0x2000 };
    assert(smi_mitigation_plan(c, 0x402b, 7, 7, armed, true).reason == SMI_PLAN_LEGKEY_ARMED);
    assert(smi_mitigation_plan(c, 0x402b, 7, 7, trap, true).reason == SMI_PLAN_LEGKEY_ARMED);
    assert(smi_mitigation_plan(c, 0x402b, 7, 5, owned, true).reason == SMI_PLAN_USB_NOT_OWNED);
    assert(smi_mitigation_plan(c, 0x402b, 5, 5, owned, true).reason == SMI_PLAN_USB_INCOMPLETE);
    p = smi_mitigation_plan(c, 0x402b, 7, 7, owned, false);
    assert(p.reason == SMI_PLAN_DIAGNOSTIC && !p.clear && p.kept == 0x4029);
    assert(smi_mitigation_plan(c, 0x4023, 7, 7, owned, true).reason == SMI_PLAN_NOT_ARMED);
    assert(smi_mitigation_plan(c, 0x402a, 7, 7, owned, true).reason == SMI_PLAN_GLOBAL_OFF);
    assert(smi_mitigation_plan(0, 0x402b, 7, 7, owned, true).reason == SMI_PLAN_UNQUALIFIED);
    /* Exhaustive over the defined enables: only LEGACY_USB_EN can ever be
     * cleared; GBL_SMI_EN, BIOS_EN, APMC, PERIODIC, SWSMI, TCO, MCSMI never. */
    for (uint32_t en = 0; en < 0x8000; en++) {
        if (en & ~c->en_rw) continue;
        for (unsigned allowed = 0; allowed < 2; allowed++) {
            p = smi_mitigation_plan(c, en, 7, 7, owned, allowed);
            assert(!(p.clear & ~SMI_EN_LEGACY_USB));
            assert(!p.clear || ((en & (SMI_EN_GBL | SMI_EN_LEGACY_USB)) == (SMI_EN_GBL | SMI_EN_LEGACY_USB) && allowed));
            assert(!(p.kept & p.clear) && !(p.kept & (SMI_EN_EOS | SMI_EN_BIOS_RLS)));
        }
    }
    puts("PASS f1-94 mitigation plan: LEGACY_USB_EN only after all three UHCI handoffs with LEGKEY 2000, EOS as read, no BIOS_RLS, every other enable kept");
}

static struct smi_sample sample(uint64_t d, uint64_t gap, bool bounded)
{
    return (struct smi_sample){ .section = { 1000000, 1000000 + d, gap, 1000000 + (d - gap) / 2, bounded },
        .before_valid = true, .before = 0x900, .after = 0x900, .en = 0x402b };
}

static void classify_tests(void)
{
    const struct smi_chipset *c = smi_chipset(0x248c8086);
    uint32_t src;
    /* b453bc92 irqoff_long lines (us, phase, phase_us): bounded phases are
     * TSC blackouts, the unmarked "gap" phase stays unproven. */
    static const struct { unsigned us, phase_us; bool bounded; enum smi_class want; } trial[] = {
        { 2116, 2114, true, SMI_CLASS_TSC },  /* ldt/debug */
        { 2120, 2117, true, SMI_CLASS_TSC },  /* pick */
        { 2110, 2107, true, SMI_CLASS_TSC },  /* vm-enter */
        { 2120, 2117, true, SMI_CLASS_TSC },  /* address_space */
        { 2121, 2121, false, SMI_CLASS_UNPROVEN }, /* gap, wait_until */
        { 2111, 2111, false, SMI_CLASS_UNPROVEN }, /* gap, usb-hid */
    };
    for (unsigned i = 0; i < ARRAY_SIZE(trial); i++) {
        struct smi_sample s = sample(trial[i].us * 1000ull, trial[i].phase_us * 1000ull, trial[i].bounded);
        assert(smi_classify(c, &s, g_tsc_per_ms, &src) == trial[i].want && !src);
    }
    struct smi_sample s = sample(2116000, 2114000, true);
    s.after = 0x908;                        /* LEGACY_USB_STS rose */
    assert(smi_classify(c, &s, g_tsc_per_ms, &src) == SMI_CLASS_STATUS && src == SMI_STS_LEGACY_USB);
    s.after = 0x8900; assert(smi_classify(c, &s, g_tsc_per_ms, &src) == SMI_CLASS_STATUS && src == SMI_STS_SERIRQ);
    s.before = 0x100; s.after = 0x900;      /* MCSMI rose, MCSMI_EN clear */
    assert(smi_classify(c, &s, g_tsc_per_ms, &src) == SMI_CLASS_TSC && !src);
    s.before = 0x900; s.after = 0x4900; s.before_valid = false; /* stale "before" sample */
    assert(smi_classify(c, &s, g_tsc_per_ms, &src) == SMI_CLASS_TSC && !src);
    s = sample(2116000, 999000, true);      /* below 1 ms */
    assert(smi_classify(c, &s, g_tsc_per_ms, &src) == SMI_CLASS_UNPROVEN);
    s = sample(3000000, 1400000, true);     /* executed work exceeds the gap */
    assert(smi_classify(c, &s, g_tsc_per_ms, &src) == SMI_CLASS_UNPROVEN);
    s = sample(999999, 999000, true);       /* not a long section */
    assert(smi_classify(c, &s, g_tsc_per_ms, &src) == SMI_CLASS_NONE);
    s = sample(2116000, 2114000, true); s.section.gap_start = 0; /* gap outside the section */
    assert(smi_classify(c, &s, g_tsc_per_ms, &src) == SMI_CLASS_UNPROVEN);
    s = sample(2116000, 2114000, true);
    assert(smi_classify(0, &s, g_tsc_per_ms, &src) == SMI_CLASS_TSC); /* no chipset: TSC rule only */
    assert(smi_classify(c, &s, 0, &src) == SMI_CLASS_NONE);
    puts("PASS f1-94 classification: trial ldt/debug, pick, vm-enter, address_space -> tsc; unmarked gap -> unproven; causal status rise first; MCSMI/stale ignored");
}

static void tick(void) { cycles += 1000000; smi_tick(cycles, cycles - 1000000); }

static void init_and_records(void)
{
    fixture();
    smi_attribution_init();
    assert(pm_writes == 1 && written == 0x4023 && enable == 0x4023 && mitigated == SMI_EN_LEGACY_USB);
    assert(strstr(logbuf, "[kernel] smi_config chip=ICH3-M pmbase=1000 en=0000402b sts=00000900 pm1_cnt=0000 pm1_en=0100 gen_pmcon1=0000 gpe0_en=0000 gpe1_en=0080 gpi_rout=00020000 devtrap=0000 mon_smi=0000 legkey=2000/2000/2000\n"));
    assert(strstr(logbuf, "[kernel] smi_mitigate result=cleared clear=00000008 en_before=0000402b en_after=00004023 kept=GBL_SMI_EN,APMC_EN,PERIODIC_EN\n"));
    assert(base == 0x1000 && en_cache == 0x4023 && causal_cache == 0x1f720);

    /* Ticks: one SMI_STS read each; a 3.1 ms tick gap is a blackout. */
    logat = 0; pm_reads = 0;
    tick(); tick(); assert(pm_reads == 2 && tick_valid && !win.gaps);
    smi_mark(SMI_HID_BEGIN); cycles += 50000; smi_mark(SMI_HID_END);
    smi_mark(SMI_EC_IO);                               /* EC access just before */
    cycles += 3100000 - 50000; smi_tick(cycles, cycles - 3100000);
    assert(win.gaps == 1 && win.near[0] == 1 && win.near[1] == 0 && win.near[2] == 1);
    for (unsigned i = 0; i < 249; i++) tick();         /* 250 ms later: another */
    cycles += 2100000; status = 0x8900;                /* SERIRQ_SMI_STS visible */
    smi_tick(cycles, cycles - 3100000 + 1000000);
    assert(win.gaps == 2 && win.near[2] == 1 && (win.sources & SMI_STS_SERIRQ));
    assert(win.period_min >= 250000000ull && win.period_max == win.period_min);
    status = 0x900; tick();
    assert(win.rise & SMI_STS_SERIRQ);

    /* Long sections: SMI_STS read once after the end. */
    pm_reads = 0;
    tick();
    uint64_t start = cycles + 10000;
    smi_mark(SMI_USB_SCAN);
    cycles = start + 2116000;
    struct smi_section x = { start, cycles, 2114000, start + 1000, true };
    smi_section_end(&x);
    assert(pm_reads == 2 && win.tsc == 1 && win.leads == 1);
    assert(win.lead_min[1] >= 10000 && win.lead_min[1] <= 11300 && win.lead_max[0] > 0);
    tick(); start = cycles + 1000; status = 0x920;      /* APM_STS rise inside */
    cycles = start + 2120000;
    x = (struct smi_section){ start, cycles, 0, 0, false };
    smi_section_end(&x);
    assert(win.sts == 1 && (win.sources & SMI_STS_APM));
    status = 0x900; tick(); start = cycles + 1000; cycles = start + 2111000;
    x = (struct smi_section){ start, cycles, 2111000, start, false };
    smi_section_end(&x);
    assert(win.unproven == 1);
    /* A no-work adjacent stamp pair (irq_budget_stamp) inside a section. */
    tick(); start = cycles + 1000; smi_adjacent_gap(start + 5000, start + 2105000);
    cycles = start + 2130000;
    x = (struct smi_section){ start, cycles, 0, 0, false };
    smi_section_end(&x);
    assert(win.tsc == 2);

    smi_mark(SMI_HID_BEGIN); smi_mark(SMI_HID_BEGIN); smi_mark(SMI_EC_IO);
    smi_window_emit(2000ull * 1000000, false);
    assert(strstr(logbuf, "[kernel] smi count=3 sources=00008020 max_us=2120 min_us=2100 sts=1 tsc=2 unproven=1 gaps=2 rate=1.00/s period_ms=251/251\n"));
    assert(strstr(logbuf, "[kernel] smi_coincide gaps=2 hid_near=1 scan_near=0 ec_near=1 hid_turns=3 scans=1 ec_io=2 ec_poll=1000 leads=2 hid_lead_us="));
    assert(strstr(logbuf, "[kernel] smi_regs en=00004023 sts=00000900 rise=00008000 mitigated=00000008 rearm=0 legkey=2000/2000/2000\n"));
    assert(!win.gaps && !win.tsc && win.min == UINT64_MAX && !mark_count[0]);

    /* Quiet window: no smi lines, no repeated unchanged register record. */
    logat = 0; logbuf[0] = 0;
    smi_window_emit(2000ull * 1000000, false);
    assert(!logat);
    /* The firmware re-arms the cleared enable: say so, do not fight it. */
    enable |= SMI_EN_LEGACY_USB; pm_writes = 0;
    smi_window_emit(2000ull * 1000000, false);
    assert(strstr(logbuf, "rearm=1") && strstr(logbuf, "en=0000402b") && !pm_writes);
    assert(causal_cache & SMI_STS_LEGACY_USB);
    puts("PASS f1-94 records: smi_config, smi_mitigate cleared 00000008 -> 00004023, smi/smi_coincide/smi_regs, one STS read per tick and per long section, re-arm reported without a write");
}

static void refusal_tests(void)
{
    fixture(); g_boot.flags = CBI_F_HW_CHECK;
    smi_attribution_init();
    assert(!pm_writes && enable == 0x402b && !mitigated && base == 0x1000);
    assert(strstr(logbuf, "result=diagnostic_boot clear=00000000 en_before=0000402b en_after=0000402b kept=GBL_SMI_EN,LEGACY_USB_EN,APMC_EN,PERIODIC_EN"));
    fixture(); g_boot.flags = CBI_F_SAFE_MODE; smi_attribution_init(); assert(!pm_writes);
    fixture(); snap.usb_legkey[2] = 0x2010; smi_attribution_init();
    assert(!pm_writes && strstr(logbuf, "result=legkey_armed"));
    fixture(); hosts[1].owned = false; smi_attribution_init();
    assert(!pm_writes && strstr(logbuf, "result=usb_not_owned"));
    fixture(); hosts[0].quarantined = true; smi_attribution_init();
    assert(!pm_writes && strstr(logbuf, "result=usb_not_owned"));
    fixture(); ignore_write = true; smi_attribution_init();
    assert(pm_writes == 1 && !mitigated && strstr(logbuf, "result=readback_failed"));
    fixture(); enable = 0x4023; smi_attribution_init();   /* changed since the snapshot */
    assert(!pm_writes && !mitigated && strstr(logbuf, "result=not_armed"));
    /* Unsupported bridge: no PM/PCI access at all, then no tick I/O. */
    fixture(); snap = (struct registry_smi){ .supported = false, .lpc_id = 0x71108086 }; snap_ok = false;
    smi_attribution_init();
    assert(!pm_reads && !cfg_reads && !pm_writes && !base);
    assert(strstr(logbuf, "[kernel] smi_config chip=unsupported lpc=71108086 decoded=0 sampler=off\n"));
    tick(); tick(); cycles += 3000000; smi_tick(cycles, cycles - 3000000);
    assert(!pm_reads && win.gaps == 1);
    logat = 0; smi_window_emit(2000ull * 1000000, false); assert(!logat);  /* quiet unless verbose */
    cycles += 3000000; smi_tick(cycles, cycles - 3000000);
    smi_window_emit(2000ull * 1000000, true);
    assert(strstr(logbuf, "[kernel] smi count=0 sources=00000000") && !strstr(logbuf, "smi_regs") && !pm_reads);
    /* ecpoll=0 and a machine without the EC are named in the record. */
    for (unsigned i = 0; i < 2; i++) {
        ec_poll_state = i ? -1 : 0; logat = 0;
        cycles += 3000000; smi_tick(cycles, cycles - 3000000);
        smi_window_emit(2000ull * 1000000, true);
        assert(strstr(logbuf, i ? " ec_io=0 ec_poll=none leads=0" : " ec_io=0 ec_poll=off leads=0"));
    }
    ec_poll_state = 1000;
    puts("PASS f1-94 refusals: H page/safe mode, armed LEGKEY, unowned/quarantined UHCI, failed readback, unsupported bridge without port I/O; ec_poll=1000/off/none in smi_coincide");
}

int main(void)
{
    chipset_tests();
    plan_tests();
    classify_tests();
    init_and_records();
    refusal_tests();
    return 0;
}
