/* F0-35: H-page-only observation and one reversible SMI source experiment.
 * Register authority/selection limits: docs/validation/f0-30-irqoff-audit.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/timing.h>
#include <ciuki/registry.h>
#include <ciuki/hardware_check.h>
#include <ciuki/usb.h>

#define SMI_USB (1u<<3)
#define SMI_SW  (1u<<6)
#define SMI_PERIODIC (1u<<14)
/* Defined readable/writable bits; bit 7 is a write-only SCI command. */
#define SMI_RW 0x0000687fu
struct timer_measurement {
    struct timing_stats timing;
    struct timing_stall_stats stalls;
    uint64_t elapsed, ticks, lost;
    bool complete;
};
static struct timer_measurement baseline, trial;
static struct registry_smi pm_start, pm_end;
static bool baseline_valid, have_trial, attempted, clear_ok, restored;
static uint32_t selected_bit, restored_en;
static const char *selected_name="unresolved";

static void pm_lines(const struct hw_check_ops *ops, const char *phase, const struct registry_smi *p)
{
    char line[192];
    ksnprintf(line,sizeof(line),"SMI %s LPC=%08x PMBASE=%08x decode=%u ACPI_CNTL=%02x GEN_PMCON1=%04x",
        phase,p->lpc_id,p->pmbase_raw,p->decoded,p->acpi_cntl,p->gen_pmcon1);
    ops->line(ops->ctx,line);
    if (!p->decoded) return;
    ksnprintf(line,sizeof(line),"SMI %s SMI_EN=%08x SMI_STS=%08x PM1_STS/EN=%04x/%04x PM1_CNT=%04x",
        phase,p->smi_en,p->smi_sts,p->pm1_sts,p->pm1_en,p->pm1_cnt);
    ops->line(ops->ctx,line);
    ksnprintf(line,sizeof(line),"SMI %s GPE0_STS/EN=%04x/%04x TCO1_STS=%04x USB_LEGKEY=%04x/%04x/%04x present=%x",
        phase,p->gpe0_sts,p->gpe0_en,p->tco1_sts,p->usb_legkey[0],p->usb_legkey[1],p->usb_legkey[2],p->usb_present);
    ops->line(ops->ctx,line);
}

static void measure(uint64_t until, struct timer_measurement *m, const struct hw_check_ops *ops,
                    uint16_t monitor_port, uint32_t bit, bool *rearmed)
{
    memset(m,0,sizeof(*m));
    timing_reset();
    struct timing_stats before;
    timing_snapshot(&before);
    timing_stalls_begin();
    uint64_t start=read_tsc_serialized(), now=ops->now(ops->ctx);
    /* Raw TSC avoids stopping a fractional millisecond short when the
     * deadline clock is quantised to integer milliseconds. */
    while (read_tsc_serialized()-start<10000*g_tsc_per_ms && now<until) {
        if (bit && (inl(monitor_port) & bit)) *rearmed=true;
        task_sleep_ms(1);
        now=ops->now(ops->ctx);
    }
    if (bit && (inl(monitor_port) & bit)) *rearmed=true;
    timing_stalls_end(&m->stalls,&m->timing);
    m->elapsed=m->stalls.per_ms ? (m->stalls.end_tsc-m->stalls.start_tsc)/m->stalls.per_ms : 0;
    m->ticks=m->timing.pit_irqs-before.pit_irqs;
    m->lost=m->elapsed>m->ticks ? m->elapsed-m->ticks : 0;
    m->complete=m->elapsed>=10000 && now<=until && !m->stalls.dropped;
}

static bool trace_lines(uint64_t until, const char *phase, const struct timer_measurement *m,
                        const struct hw_check_ops *ops)
{
    char line[200];
    ksnprintf(line,sizeof(line),"SMI %s window_tsc=%016llx..%016llx per_ms=%llu stalls=%u dropped=%u intervals=%u period_us=%llu/%llu/%llu jitter_pp_us=%llu",
        phase,m->stalls.start_tsc,m->stalls.end_tsc,m->stalls.per_ms,m->stalls.count,m->stalls.dropped,
        m->stalls.count ? m->stalls.count-1 : 0,
        m->stalls.period_min_us,m->stalls.period_mean_us,m->stalls.period_max_us,m->stalls.jitter_us);
    ops->line(ops->ctx,line);
    unsigned i=0;
    for (;i<m->stalls.count && ops->now(ops->ctx)<until;i++) {
        struct timing_stall e;
        if (!timing_stall_get(i,&e)) break;
        ksnprintf(line,sizeof(line),"SMI %s stall=%u start_tsc=%016llx end_tsc=%016llx us=%llu section=%s site=%08x",
            phase,i,e.start_tsc,e.end_tsc,m->stalls.per_ms ? (e.end_tsc-e.start_tsc)*1000/m->stalls.per_ms : 0,
            e.section,e.site);
        ops->line(ops->ctx,line);
    }
    if (i<m->stalls.count) {
        ksnprintf(line,sizeof(line),"SMI %s trace incomplete: printed=%u retained=%u",phase,i,m->stalls.count);
        ops->line(ops->ctx,line);
    }
    return i==m->stalls.count;
}

static bool usb_owned(void)
{
    unsigned owned=0;
    for (unsigned i=0;i<usb_count();i++) {
        const struct usb_host *h=usb_get(i);
        if (!h || !h->ready || h->quarantined || h->pci.bus || h->pci.dev!=29 || h->pci.fn>=3 ||
            h->pci.vendor!=0x8086 || h->pci.device!=(h->pci.fn==0 ? 0x2482 : h->pci.fn==1 ? 0x2484 : 0x2487)) continue;
        owned|=1u<<h->pci.fn;
    }
    return owned==7;
}

/* Select an experimental hypothesis, never claim a status snapshot proves
 * causality. Firmware can clear STS in SMM before we can read it. Missing
 * observations are not fabricated into a timer period or a USB source. */
static uint32_t candidate(const struct registry_smi *p)
{
    if (!baseline_valid || baseline.stalls.count<2 || !(p->smi_en & 1) ||
        (p->smi_en & ~SMI_RW)) return 0;
    uint64_t lo=baseline.stalls.period_min_us, hi=baseline.stalls.period_max_us;
    uint32_t choices=0;
    uint32_t status=p->smi_sts | pm_start.smi_sts | pm_end.smi_sts;
    if ((p->smi_en & SMI_USB) && (status & SMI_USB) && p->usb_present==7 && usb_owned()) {
        unsigned traps=p->usb_legkey[0] | p->usb_legkey[1] | p->usb_legkey[2];
        if (traps & 0xbf) choices|=SMI_USB; /* §11.1.16, excludes native bit 13 */
    }
    /* Include measured handler length in the upper software-timer bound.
     * Require direct consecutive cadence, not guessed missing multiples. */
    if (p->smi_en & SMI_SW) {
        uint64_t min=p->gen_pmcon1 & (1u<<10) ? 1000 : 60000;
        uint64_t max=p->gen_pmcon1 & (1u<<10) ? 2000 : 68000;
        if (lo>=min && hi<=max+baseline.timing.critical_us) choices|=SMI_SW;
    }
    if (p->smi_en & SMI_PERIODIC) {
        static const uint32_t seconds[4]={60,32,16,8};
        uint64_t period=seconds[p->gen_pmcon1 & 3]*1000000ull;
        if (lo>=period-period/100 && hi<=period+period/100) choices|=SMI_PERIODIC;
    }
    return choices && !(choices & (choices-1)) ? choices : 0;
}

static const char *bit_name(uint32_t bit)
{ return bit==SMI_USB ? "LEGACY_USB_EN" : bit==SMI_SW ? "SWSMI_TMR_EN" : "PERIODIC_EN"; }

/* Every attempt, including failed clear readback, reaches restoration.
 * Merge only the saved target bit into the current enable register so SMM
 * changes to other enables/EOS are not overwritten by a stale full value. */
static bool restore_bit(uint16_t port, uint32_t bit, uint32_t saved, uint32_t *readback)
{
    for (unsigned retry=0;retry<3;retry++) {
        uint32_t current=inl(port);
        outl(port,(current & SMI_RW & ~bit) | (saved & bit));
        *readback=inl(port);
        if ((*readback & bit)==(saved & bit)) return true;
    }
    return false;
}

void hardware_check_timer(uint64_t until, bool experiment, struct hw_check_result *out,
                          const struct hw_check_ops *ops)
{
    out->status=HW_SKIP;
    if (!experiment) {
        baseline_valid=have_trial=attempted=clear_ok=restored=false;
        selected_bit=restored_en=0; selected_name="unresolved";
        memset(&baseline,0,sizeof(baseline)); memset(&trial,0,sizeof(trial));
        memset(&pm_start,0,sizeof(pm_start)); memset(&pm_end,0,sizeof(pm_end));
    }
    if (!(g_boot.flags & CBI_F_HW_CHECK) || !(read_eflags() & 0x200)) {
        ksnprintf(out->detail,sizeof(out->detail),"H page task with IF=1 required"); return;
    }
    if (!g_cpu_tsc || !g_tsc_per_ms) {
        ksnprintf(out->detail,sizeof(out->detail),"no calibrated independent clock"); return;
    }
    if (!experiment) {
        registry_smi_snapshot(&pm_start); pm_lines(ops,"before",&pm_start);
        uint64_t now=ops->now(ops->ctx);
        if (now>=until || until-now<11500) {
            out->status=HW_FAIL; ksnprintf(out->detail,sizeof(out->detail),"insufficient 10 s measurement budget"); return;
        }
        measure(until-1500,&baseline,ops,0,0,0);
        registry_smi_snapshot(&pm_end); pm_lines(ops,"after",&pm_end);
        bool logged=trace_lines(until-500,"before",&baseline,ops);
        baseline_valid=baseline.complete && logged;
        out->status=baseline_valid && baseline.lost<=2 && baseline.timing.critical_us<=CRIT_BUDGET_US ? HW_PASS : HW_FAIL;
        ksnprintf(out->detail,sizeof(out->detail),"ms=%llu ticks=%llu lost=%llu IF-off=%uus section=%s site=%08x",
            baseline.elapsed,baseline.ticks,baseline.lost,baseline.timing.critical_us,
            baseline.timing.critical_section,baseline.timing.critical_site);
        return;
    }
    struct registry_smi p;
    if (!registry_smi_snapshot(&p)) {
        ksnprintf(out->detail,sizeof(out->detail),"ICH3-M PM decode unavailable; no write"); return;
    }
    pm_lines(ops,"trial-before",&p);
    /* Do not transfer attribution across changed timer configuration. */
    if (!pm_start.decoded || !pm_end.decoded || p.base!=pm_start.base || p.base!=pm_end.base ||
        p.gen_pmcon1!=pm_start.gen_pmcon1 || p.gen_pmcon1!=pm_end.gen_pmcon1 ||
        ((p.smi_en ^ pm_start.smi_en) & ~2u) || ((p.smi_en ^ pm_end.smi_en) & ~2u)) {
        ksnprintf(out->detail,sizeof(out->detail),"PM configuration changed/unqualified; no write"); return;
    }
    selected_bit=candidate(&p);
    if (!selected_bit) {
        ksnprintf(out->detail,sizeof(out->detail),"source unresolved: cadence/status ambiguous; no write"); return;
    }
    selected_name=bit_name(selected_bit);
    uint64_t now=ops->now(ops->ctx);
    if (now>=until || until-now<11500) {
        out->status=HW_FAIL; ksnprintf(out->detail,sizeof(out->detail),"%s: insufficient 10 s budget; no write",selected_name); return;
    }
    char line[160];
    ksnprintf(line,sizeof(line),"SMI candidate=%s bit=%08x ICH3-M section=9.8.3.14; clear/re-measure/restore (hypothesis)",selected_name,selected_bit);
    ops->line(ops->ctx,line);
    /* No formatting, logging, allocation or alternate return while changed. */
    uint16_t port=p.base+0x30;
    uint32_t saved=inl(port);
    now=ops->now(ops->ctx);
    if (now>=until || until-now<11500) {
        out->status=HW_FAIL; ksnprintf(out->detail,sizeof(out->detail),"candidate reporting exhausted 10 s budget; no write"); return;
    }
    bool stable=(saved & ~2u)==(p.smi_en & ~2u);
    if (!stable) {
        ksnprintf(out->detail,sizeof(out->detail),"SMI_EN changed before clear; no write"); return;
    }
    attempted=true;
    outl(port,saved & ~selected_bit);
    bool cleared=!(inl(port) & selected_bit), rearmed=false;
    clear_ok=cleared;
    if (cleared) { have_trial=true; measure(until-1500,&trial,ops,port,selected_bit,&rearmed); }
    restored=restore_bit(port,selected_bit,saved,&restored_en);
    registry_smi_snapshot(&p); pm_lines(ops,"restored",&p);
    bool logged=!have_trial || trace_lines(until-500,"clear",&trial,ops);
    bool improved=have_trial && baseline.stalls.count && !trial.stalls.count && trial.lost<=2 &&
        trial.timing.critical_us<=CRIT_BUDGET_US;
    out->status=cleared && restored && !rearmed && trial.complete && logged && improved ? HW_PASS : HW_FAIL;
    ksnprintf(out->detail,sizeof(out->detail),"%s stalls=%u/%u lost=%llu/%llu max=%u/%uus clear=%u rearm=%u restore=%u",
        selected_name,baseline.stalls.count,trial.stalls.count,baseline.lost,trial.lost,
        baseline.timing.critical_us,trial.timing.critical_us,cleared,rearmed,restored);
}

void hardware_check_smi_summary(const struct hw_check_ops *ops)
{
    /* Each line fits 80 columns; model's compact ten results leave room on
     * a 25-row console. Full raw before/after/trial snapshots remain logged. */
    char line[80];
    if (!pm_end.decoded) return;
    ksnprintf(line,sizeof(line),"PMBASE=%04x ACPI_CNTL=%02x GEN_PMCON1=%04x",pm_end.base,pm_end.acpi_cntl,pm_end.gen_pmcon1);
    ops->line(ops->ctx,line);
    ksnprintf(line,sizeof(line),"SMI_EN=%08x SMI_STS=%08x TCO1_STS=%04x",pm_end.smi_en,pm_end.smi_sts,pm_end.tco1_sts);
    ops->line(ops->ctx,line);
    ksnprintf(line,sizeof(line),"PM1_STS/EN=%04x/%04x GPE0_STS/EN=%04x/%04x",pm_end.pm1_sts,pm_end.pm1_en,pm_end.gpe0_sts,pm_end.gpe0_en);
    ops->line(ops->ctx,line);
    ksnprintf(line,sizeof(line),"USB_LEGKEY 00:1d.0/1/2=%04x/%04x/%04x present=%x",pm_end.usb_legkey[0],pm_end.usb_legkey[1],pm_end.usb_legkey[2],pm_end.usb_present);
    ops->line(ops->ctx,line);
    ksnprintf(line,sizeof(line),"Before: stalls=%u lost=%llu max=%uus",baseline.stalls.count,baseline.lost,baseline.timing.critical_us);
    ops->line(ops->ctx,line);
    if (baseline.stalls.count>1)
        ksnprintf(line,sizeof(line),"Before period min/mean/max=%llu/%llu/%lluus",
            baseline.stalls.period_min_us,baseline.stalls.period_mean_us,baseline.stalls.period_max_us);
    else ksnprintf(line,sizeof(line),"Before period/jitter unavailable (<2 observed stalls)");
    ops->line(ops->ctx,line);
    ksnprintf(line,sizeof(line),"Before jitter p-p=%lluus; source candidate=%s",baseline.stalls.jitter_us,selected_name);
    ops->line(ops->ctx,line);
    if (attempted) {
        ksnprintf(line,sizeof(line),"SMI %s clear=%u restore=%u EN=%08x",selected_name,clear_ok,restored,restored_en);
        ops->line(ops->ctx,line);
    }
    if (!have_trial) return;
    ksnprintf(line,sizeof(line),"Clear: stalls=%u lost=%llu max=%uus",trial.stalls.count,trial.lost,trial.timing.critical_us);
    ops->line(ops->ctx,line);
    if (trial.stalls.count>1)
        ksnprintf(line,sizeof(line),"Clear period min/mean/max=%llu/%llu/%lluus jitter=%lluus",
            trial.stalls.period_min_us,trial.stalls.period_mean_us,trial.stalls.period_max_us,trial.stalls.jitter_us);
    else ksnprintf(line,sizeof(line),"Clear period/jitter unavailable (<2 observed stalls)");
    ops->line(ops->ctx,line);
}
