/* fx-18: the production APM provider, power-source arbiter and APM port
 * window against a BIOS fixture shaped like the ThinkPad T23 (ICH3-M,
 * PMBASE 1000h, APM 1.2 BIOS whose installation check reads PM1_CNT and
 * tests SCI_EN, as the e831e685 trial showed at f000:4ccf), on a machine
 * whose ACPI stays firmware-owned. Only firmware, time and task seams are
 * replaced; the port verdicts are the production apm_port_check.
 * SPDX-License-Identifier: GPL-2.0-only */
#define CIUKI_CPU_H
#include <stdio.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/task.h>
#include <ciuki/power_source.h>
static uint32_t irq_save(void) { return 0x200; }
static void irq_restore(uint32_t f) { (void)f; }
#define P2V(p) ((void *)(uintptr_t)(p))
#define V2P(p) ((uint32_t)(uintptr_t)(p))
#include <ciuki/storage.h>
#include <ciuki/apm_ports.h>
#include <ciuki/registry.h>
#include "../../src/kernel/core/apm.h"

/* The production file declares these weak (kernels without ACPI link);
 * here they are the T23 fixture, defined below. */
int acpi_pm_blocks(struct apm_pm_blocks *) __attribute__((weak));
int acpi_ec_firmware_hold(unsigned ms) __attribute__((weak));
void acpi_ec_firmware_release(void) __attribute__((weak));
bool registry_smi_snapshot(struct registry_smi *) __attribute__((weak));
#pragma clang diagnostic ignored "-Wpointer-bool-conversion"
static unsigned failures, checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)
struct task *g_current;
struct ciuki_boot_info g_boot;
static struct task fake_tasks[4];
static uint64_t ticks;
static char log_line[256], logs[8192];
static size_t log_size;

/* ---- T23 machine model ---- */
static struct biosvm_failure failure;
static bool retf_halts;
static struct apm_port_window window;
static bool window_set, vm_quarantined, ec_held, fadt_present = true, chip_present = true;
static uint16_t pm1_cnt;            /* SCI_EN clear: firmware (legacy) mode */
static unsigned calls, holds, denials, offs, suspends, rejects, connects, disconnects, apmc_writes;
static unsigned busy_holds;
static uint16_t events[16]; static unsigned event_head, event_tail;
/* AC line, aggregate CX/DX, and the two sockets (main, UltraBay). */
static uint8_t ac = 0;
static uint16_t agg_cx = 0x014b, agg_dx = 0x805a, unit_cx[2] = {0x014b, 0x10ff}, unit_dx[2] = {0x805a, 0xffff};

static int port(uint16_t p, unsigned width, bool write, uint32_t value, uint16_t fn)
{
    uint8_t kind; const char *reason;
    enum apm_port_verdict v = window_set ? apm_port_check(&window, p, width, write, value, fn, &kind, &reason) : APM_PORT_DENY;
    if (v == APM_PORT_DENY) {
        /* The real VM: a denied port faults the call and quarantines the VM. */
        denials++; vm_quarantined = true;
        snprintf(log_line, sizeof(log_line), "[v86] step=io error=-1 port=%04x width=%u write=%u reason=policy", p, width, write);
        return -V86_EPERM;
    }
    if (write && p == 0xb2) apmc_writes++;
    return 0;
}
int biosvm_apm_call(struct biosvm_regs *r, uint32_t ms)
{
    CHECK(r->interrupt == 0x15 && ms && ms <= 2000);
    CHECK(ec_held); /* fx-18: no APM SMI may interleave with an EC quirk transaction */
    if (vm_quarantined) return -V86_EIO;
    uint16_t fn = (uint16_t)r->eax;
    calls++;
    failure = (struct biosvm_failure){0};
    /* Every function: IN AL,DX (PM1_CNT); TEST AL,1 (SCI_EN). */
    int e = port(0x1004, 1, false, 0, fn);
    if (e) {
        failure = (struct biosvm_failure){.error = e, .function = fn, .interrupt = 0x15, .reason = "port_denied",
            .cs = 0xf000, .ip = 0x4ccf, .port = 0x1004, .width = 1, .elapsed_ms = 1, .deadline_ms = ms, .budget_ms = ms};
        return e;
    }
    if (retf_halts) {
        /* Pre-fx-25 monitor: the handler's RETF 2 reached the sentinel HLT. */
        vm_quarantined = true;
        failure = (struct biosvm_failure){.error = -V86_ETIMEDOUT, .function = fn, .interrupt = 0x15,
            .reason = "halted_at_sentinel", .cs = 0x1000, .ip = 1, .ss = 0x1100, .sp = 0x1000,
            .elapsed_ms = 87, .deadline_ms = 87, .budget_ms = 100};
        return -V86_ETIMEDOUT;
    }
    r->flags = 0;
    if (pm1_cnt & 1) { r->flags = 1; r->eax = 0x8600; return 0; } /* OS runs ACPI: APM not present */
    if (fn != 0x5300) { e = port(0xb2, 1, true, fn & 0xff, fn); if (e) return e; } /* software SMI */
    switch (fn) {
    case 0x5300: r->eax = 0x0102; r->ebx = 0x504d; r->ecx = 0x0003; break;
    case 0x5301: connects++; break;
    case 0x5304: disconnects++; break;
    case 0x5310: CHECK(!r->ebx); r->ecx=0; break;
    case 0x530e: r->eax = 0x0102; break;
    case 0x5308: case 0x530f: break;
    case 0x530a:
        if (r->ebx == 1) { r->ebx = (uint32_t)ac << 8; r->ecx = agg_cx; r->edx = agg_dx; break; }
        if ((r->ebx & 0xff00) != 0x8000 || (r->ebx & 0xff) < 1 || (r->ebx & 0xff) > 2) { r->flags = 1; r->eax = 0x0900; break; }
        r->ecx = unit_cx[(r->ebx & 0xff) - 1]; r->edx = unit_dx[(r->ebx & 0xff) - 1]; r->ebx = (uint32_t)ac << 8 | 0xff;
        break;
    case 0x530b:
        if (event_head < event_tail) { r->ebx = events[event_head++]; r->ecx = 0; }
        else { r->flags = 1; r->eax = 0x8000; }
        break;
    case 0x5307:
        CHECK(r->ebx == 1);
        if (r->ecx == 5) rejects++;
        else if (r->ecx == 2) suspends++;
        else if (r->ecx == 3) {
            /* Off: the BIOS writes SLP_TYP|SLP_EN to PM1_CNT itself. */
            e = port(0x1004, 2, true, 0x3c00 | 0x2000, fn); if (e) return e;
            offs++;
        } else CHECK(false);
        break;
    default: CHECK(false);
    }
    return 0;
}
int biosvm_apm_set_window(const struct apm_port_window *w) { window = *w; window_set = true; return 0; }
void biosvm_apm_io_snapshot(struct biosvm_apm_io *out) { *out = (struct biosvm_apm_io){.denied = denials}; }
/* fx-25: the BIOS VM's record of the last failure, as the f441c5bd trial
 * produced it before the RETF 2 fix (or a port denial). */
void biosvm_last_failure(struct biosvm_failure *out) { *out = failure; }
int acpi_ec_firmware_hold(unsigned ms)
{
    CHECK(ms && ms <= 500 && !ec_held);
    if (busy_holds) { busy_holds--; return -11; }
    holds++; ec_held = true; return 0;
}
void acpi_ec_firmware_release(void) { CHECK(ec_held); ec_held = false; }
/* T23 FADT (ICH3-M, PMBASE 1000h): PM1a_EVT 1000h/4, PM1a_CNT 1004h/2,
 * PM_TMR 1008h/4, GPE0 1028h/4, GPE1 102Ch/4, SMI_CMD B2h. */
int acpi_pm_blocks(struct apm_pm_blocks *out)
{
    if (!fadt_present) return -ENOENT;
    *out = (struct apm_pm_blocks){.pm1a_evt = 0x1000, .pm1a_cnt = 0x1004, .pm_tmr = 0x1008, .gpe0 = 0x1028,
        .gpe1 = 0x102c, .smi_cmd = 0xb2, .pm1_evt_len = 4, .pm1_cnt_len = 2, .pm_tmr_len = 4, .gpe0_len = 4, .gpe1_len = 4};
    return 0;
}
bool registry_smi_snapshot(struct registry_smi *out)
{
    memset(out, 0, sizeof(*out));
    if (!chip_present) return false;
    out->supported = out->decoded = true; out->lpc_id = 0x248c8086u; out->base = 0x1000; out->pm1_cnt = pm1_cnt;
    return true;
}

/* ---- kernel seams ---- */
struct task *task_create_kernel(const char *name, void (*fn)(void *), void *arg, enum task_prio prio)
{
    (void)fn; (void)arg; CHECK(!strcmp(name, "apm") && prio == P_DEVICE); return &fake_tasks[0];
}
void task_start(struct task *t) { CHECK(t != NULL); }
static unsigned flushes, stops;
int power_devices_suspend(void) { return -ENOTSUP; }
int power_devices_resume(void) { return 0; }
int storage_power_init(void) { return 0; }
int storage_sync_bounded(uint64_t end) { (void)end; flushes++; return 0; }
uint64_t deadline_after_ms(uint32_t ms) { return ticks + ms; }
bool deadline_passed(uint64_t end) { return ticks >= end; }
static int quiesce(void *ctx, uint64_t end) { (void)ctx; (void)end; stops++; return 0; }
static int recover(void *ctx, uint16_t event, uint16_t info) { (void)ctx; (void)event; (void)info; return 0; }
void klog(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vsnprintf(log_line, sizeof(log_line), fmt, ap); va_end(ap);
    size_t n = strlen(log_line);
    if (log_size + n + 2 < sizeof(logs)) { memcpy(logs + log_size, log_line, n); log_size += n; logs[log_size++] = '\n'; logs[log_size] = 0; }
}
#define APM_PLATFORM_HOST_CLOCK
static uint64_t now_us(void *ctx) { (void)ctx; return ticks * 1000 + calls * 3; } /* modelled 3 us per BIOS call */
#include "../../src/kernel/core/power_source.c"
bool power_devices_resume_ready(void) { return false; }
#include "../../src/kernel/core/apm_platform.c"
void task_sleep_ms(uint32_t ms) { ticks += ms; service_once(); }
static void post(uint16_t e) { events[event_tail++ % 16] = e; }
static void reset_machine(void)
{
    vm_quarantined = window_set = false; calls = denials = holds = 0; log_size = 0; logs[0] = 0;
}
int main(void)
{
    g_boot.acpi_rsdp_phys = 0xf6f10; /* the T23 has ACPI tables */
    g_current = &fake_tasks[1]; apm_worker = &fake_tasks[0];
    const struct power_source_platform platform = {.quiesce_dma = quiesce, .resume = recover};
    CHECK(!power_source_set_platform(&platform));
    /* The ACPI inventory settled firmware-owned (T23: passive cooling). */
    power_source_acpi_settled(CIUKI_POWER_FIRMWARE);
    struct ciuki_power_status acpi;
    memset(&acpi, 0xff, sizeof(acpi));
    acpi.version = CIUKI_POWER_VERSION; acpi.size = sizeof(acpi); acpi.ownership = CIUKI_POWER_FIRMWARE;
    acpi.provider = CIUKI_POWER_PROVIDER_NONE; acpi.battery_count = 0; acpi.zone_count = 1; acpi.reserved = 0;
    acpi.shutdown_requests = acpi.sequence = acpi.event_overflows = 0; acpi.error = 4; acpi.lid_open = CIUKI_POWER_UNKNOWN;
    acpi.zones[0].id = 7;
    power_source_publish_record(POWER_SOURCE_ACPI, &acpi);

    /* 1. The trial: the BIOS's PM1_CNT read outside the window faults the
     * installation check and quarantines the VM (e831e685 records). */
    fadt_present = chip_present = false;
    CHECK(discover() == -V86_EPERM && backend.state == APM_ABSENT && !connects && vm_quarantined);
    CHECK(strstr(logs, "[v86] step=io error=-1 port=1004") || denials == 1);
    CHECK(strstr(log_line, "[apm] state=1") && strstr(log_line, "reason=firmware"));
    struct ciuki_power_status record; power_source_status(&record);
    CHECK(record.provider == CIUKI_POWER_PROVIDER_NONE && !record.battery_count && record.ac_online == CIUKI_POWER_UNKNOWN);

    CHECK(strstr(logs, "[apm] call fn=5300 error=-1 reason=port_denied cs=f000 ip=4ccf") && strstr(logs, "port=1004 width=1 write=0"));
    /* 1b. The f441c5bd trial: the window admits PM1_CNT, the BIOS returns by
     * RETF 2 and the old monitor waited at the sentinel HLT; the record now
     * names the reason, the place and the budget. */
    reset_machine(); fadt_present = chip_present = true; retf_halts = true;
    CHECK(discover() == -V86_ETIMEDOUT && !connects);
    CHECK(strstr(logs, "[apm] call fn=5300 error=-110 reason=halted_at_sentinel cs=1000 ip=0001 ss=1100 sp=1000") &&
          strstr(logs, "elapsed_ms=87 deadline_ms=87 budget_ms=100"));
    retf_halts = false;

    /* 2. fx-18: the FADT + ICH3-M window admits exactly what the BIOS uses. */
    reset_machine(); fadt_present = chip_present = true;
    CHECK(!discover() && backend.state == APM_CONNECTED && connects == 1 && !denials && !vm_quarantined);
    CHECK(strstr(logs, "[apm] window ranges=") && strstr(logs, "source=fadt+chipset") && strstr(logs, "sci_en=0"));
    CHECK(strstr(logs, "reason=real_mode") && holds == calls);
    service_once(); /* first status: aggregate + unit probe 8001h, 8002h, 8003h (AH=09h) */
    CHECK(backend.units == 2 && apmc_writes == calls - 1);
    power_source_status(&record);
    CHECK(record.provider == CIUKI_POWER_PROVIDER_APM && record.ownership == CIUKI_POWER_OWNED && record.ac_online == 0);
    CHECK(record.battery_count == 2 && record.batteries[0].id == 1 && record.batteries[0].present == 1 &&
          record.batteries[0].unit == CIUKI_POWER_UNIT_PERCENT && record.batteries[0].remaining == 75 &&
          record.batteries[0].seconds == 5400 && record.batteries[0].state == 1); /* discharging */
    CHECK(record.batteries[1].id == 2 && record.batteries[1].present == 0); /* empty UltraBay */
    CHECK(record.zone_count == 1 && record.zones[0].id == 7 && record.lid_open == CIUKI_POWER_UNKNOWN);
    /* Not a T23 BIOS behaviour, but the window's bounds: SCI_EN never, SLP_EN
     * only under Set Power State, read-only chipset/timer, nothing else. */
    uint8_t k; const char *why;
    CHECK(apm_port_check(&window, 0x1004, 2, true, 0x0001, 0x530a, &k, &why) == APM_PORT_DENY && !strcmp(why, "sci_en"));
    CHECK(apm_port_check(&window, 0x1005, 1, true, 0x20, 0x530a, &k, &why) == APM_PORT_DENY && !strcmp(why, "slp_en"));
    CHECK(apm_port_check(&window, 0x1005, 1, true, 0x20, 0x5307, &k, &why) == APM_PORT_ALLOW && k == APM_PORT_PM1_CNT);
    CHECK(apm_port_check(&window, 0x1008, 4, false, 0, 0x530a, &k, &why) == APM_PORT_ALLOW && k == APM_PORT_PM_TMR);
    CHECK(apm_port_check(&window, 0x1008, 4, true, 0, 0x530a, &k, &why) == APM_PORT_DENY);
    CHECK(apm_port_check(&window, 0x1030, 4, false, 0, 0x530a, &k, &why) == APM_PORT_ALLOW && k == APM_PORT_CHIPSET);
    CHECK(apm_port_check(&window, 0x1030, 4, true, 0, 0x530a, &k, &why) == APM_PORT_DENY && !strcmp(why, "read_only"));
    CHECK(apm_port_check(&window, 0x1028, 2, true, 0x1000, 0x530a, &k, &why) == APM_PORT_ALLOW && k == APM_PORT_GPE);
    CHECK(apm_port_check(&window, 0x1080, 1, false, 0, 0x530a, &k, &why) == APM_PORT_DENY);
    CHECK(apm_port_check(&window, 0x62, 1, false, 0, 0x530a, &k, &why) == APM_PORT_DENY && k == APM_PORT_NONE);
    CHECK(apm_port_check(&window, 0xb2, 2, true, 0, 0x530a, &k, &why) == APM_PORT_DENY);
    CHECK(apm_port_check(&window, 0x80, 1, true, 0, 0x530a, &k, &why) == APM_PORT_ABSORB);

    /* 3. Polling: one 530Bh per second, Get Power Status every 10 s. */
    unsigned before = calls; uint64_t start = ticks;
    while (ticks - start < 10000) task_sleep_ms(10);
    CHECK(calls - before >= 10 && calls - before <= 14); /* 10 polls + aggregate and two units */
    /* AC plugged in: Power Status Change (APM 1.2 4.3.7) refreshes at once. */
    ac = 1; agg_cx = 0x0950; unit_cx[0] = 0x0950; unit_dx[0] = 0xffff; post(6);
    ticks += 1000; service_once();
    power_source_status(&record);
    CHECK(record.ac_online == 1 && record.batteries[0].remaining == 80 && record.batteries[0].state == 2 &&
          record.batteries[0].seconds == CIUKI_POWER_UNKNOWN);
    /* A busy EC hold skips the turn, the connection stays. */
    busy_holds = 1; ticks += 1000; service_once(); CHECK(backend.state == APM_CONNECTED && backend.cost.skipped >= 1);
    /* Battery low (4.3.6): flush and publish, no Set Power State. */
    unsigned s0 = suspends, f0 = flushes; post(5); ticks += 1000; service_once(); service_once();
    CHECK(suspends == s0 && flushes > f0 && !stops && strstr(logs, "event=battery_low"));
    /* Standby request (4.3.10): rejected. */
    post(9); ticks += 1000; service_once(); CHECK(rejects == 1 && !suspends);
    /* The 60 s cost record (f1-100 budget). */
    start = ticks; while (ticks - start < 61000) task_sleep_ms(10);
    CHECK(strstr(logs, "[apm] window calls=") && strstr(logs, "io_denied=0"));
    {
        const char *w = strstr(logs, "[apm] window calls="); unsigned wc = 0, we = 0, ws = 0;
        CHECK(w && sscanf(w, "[apm] window calls=%u events=%*u event_calls=%u status_calls=%u", &wc, &we, &ws) == 3);
        /* One 60 s window: 60 event polls (plus drained events), status
         * every 10 s or on events, aggregate + two units each time. */
        CHECK(we >= 60 && we <= 70 && ws >= 18 && ws <= 30 && wc >= we + ws && wc <= we + ws + 8);
        const char *end = strchr(w, '\n');
        printf("%.*s\n", end ? (int)(end - w) : 160, w);
    }
    /* Steady state: about 1.2 calls/s with two sockets probed (aggregate +
     * two units per 10 s): 60 event + 18 status calls per minute. */
    struct apm_cost cost; apm_backend_cost(&backend, &cost);
    CHECK(!vm_quarantined && !denials && holds == calls);

    /* 4. Soft-off: call 73's latch, then 5307h CX=3; the BIOS's own SLP_EN
     * write is admitted only under Set Power State. */
    power_source_mark_stopped();
    CHECK(power_source_soft_off() == -EIO && offs == 1 && !denials);
    struct power_source_snapshot snap; power_source_snapshot(&snap);
    CHECK(snap.provider == POWER_SOURCE_NONE && !snap.active);

    /* 5. ACPI ownership replaces a healthy APM cleanly (fresh boot state). */
    stopped = false; reset_machine();
    CHECK(!discover() && backend.state == APM_CONNECTED);
    struct power_source_ops acpi_ops = {.soft_off = request_off};
    CHECK(power_source_reserve(POWER_SOURCE_ACPI, &acpi_ops) == -EAGAIN);
    unsigned d0 = disconnects; service_once();
    CHECK(backend.state == APM_EXCLUDED && disconnects == d0 + 1 && strstr(log_line, "handoff to=acpi disconnect=clean"));
    CHECK(!power_source_reserve(POWER_SOURCE_ACPI, &acpi_ops) && !power_source_apm_admitted(true));
    power_source_snapshot(&snap); CHECK(snap.provider == POWER_SOURCE_ACPI);
    power_source_release(POWER_SOURCE_ACPI, false, 0);

    /* 6. OS-owned ACPI (SCI_EN set): APM excluded, even on a later boot. */
    acpi_settled = false; power_source_acpi_settled(CIUKI_POWER_OWNED); pm1_cnt = 1; reset_machine();
    CHECK(discover() == -EBUSY && backend.state == APM_EXCLUDED && strstr(log_line, "reason=acpi_owner"));
    unsigned c0 = connects; CHECK(connects == c0);
    printf("APM T23 fixture: %u checks, %u failures (steady cost window: %u calls, max %u us)\n",
           checks, failures, cost.calls, cost.max_us);
    return failures != 0;
}
