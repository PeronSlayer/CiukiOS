/* APM real-mode worker and exclusive cached power provider.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/storage.h>
#include <ciuki/power_source.h>
#include <ciuki/power_sleep.h>
#include <ciuki/apm_ports.h>
#include <ciuki/registry.h>
#include "apm.h"

/* fx-18: the ACPI layer supplies the FADT blocks and the legacy-EC hold;
 * weak so that builds and host tests without ACPI link. */
int acpi_pm_blocks(struct apm_pm_blocks *) __attribute__((weak));
int acpi_ec_firmware_hold(unsigned ms) __attribute__((weak));
void acpi_ec_firmware_release(void) __attribute__((weak));
bool registry_smi_snapshot(struct registry_smi *) __attribute__((weak));

static struct apm_backend backend;
static bool attempted;
static struct task *apm_worker;
static bool off_pending, off_done;
static bool sleep_pending, sleep_done;
static int sleep_result;
static bool cancel_pending, cancel_done;
static int cancel_result;
static uint64_t cancel_deadline;
static uint64_t sleep_deadline;
static int off_result;
static uint64_t off_deadline;

/* fx-18: every APM call holds the legacy EC engine (acpi/ec.c) so the SMI
 * it raises never interleaves with a ThinkPad quirk transaction. Half the
 * call's budget waits for the engine (all of it, up to 500 ms, for
 * disconnect and Set Power State, which must not be skipped); EAGAIN means
 * nothing ran. */
static int firmware_call(void *ctx, struct biosvm_regs *r, uint32_t ms)
{
    (void)ctx;
    uint16_t fn = (uint16_t)r->eax;
    bool must = fn == 0x5304 || fn == 0x5307;
    uint32_t wait = must ? ms : ms / 2;
    if (wait > 500) wait = 500;
    uint64_t start = deadline_after_ms(0);
    if (acpi_ec_firmware_hold && acpi_ec_firmware_hold(wait ? wait : 1)) return -EAGAIN;
    uint64_t spent = deadline_after_ms(0) - start;
    uint32_t left = spent >= ms ? 1u : ms - (uint32_t)spent;
    int e = biosvm_apm_call(r, left);
    if (acpi_ec_firmware_release) acpi_ec_firmware_release();
    return e;
}
/* fx-18: APM is excluded unless admitted: no ACPI tables, or ACPI settled
 * with clean firmware ownership (and no handoff to ACPI requested). */
static bool acpi_present(void *ctx)
{
    (void)ctx;
    return !power_source_apm_admitted(g_boot.acpi_rsdp_phys != 0);
}
static bool handoff(void *ctx)
{
    (void)ctx;
    return power_source_handoff_requested(POWER_SOURCE_APM);
}
#ifndef APM_PLATFORM_HOST_CLOCK
#include <ciuki/timing.h>
static uint64_t now_us(void *ctx)
{
    (void)ctx;
    if (g_cpu_tsc && g_tsc_per_ms) {
        uint64_t c = read_tsc_unserialized();
        return c / g_tsc_per_ms * 1000u + c % g_tsc_per_ms * 1000u / g_tsc_per_ms;
    }
    return g_ticks * 1000u;
}
#endif
static void low_battery(void *ctx, int error)
{
    (void)ctx;
    klog("[apm] event=battery_low flush_error=%d action=notify", error);
}
static int request_off(void *ctx)
{
    (void)ctx;
    if (g_current == apm_worker) return -EBUSY;
    uint32_t f = irq_save();
    if (off_pending || sleep_pending || backend.state != APM_CONNECTED) { irq_restore(f); return -EBUSY; }
    off_done = false; off_pending = true;
    off_deadline = deadline_after_ms(APM_QUIESCE_MS + APM_POWER_MS + 500);
    irq_restore(f);
    while (!off_done) {
        if (deadline_passed(off_deadline)) return -V86_ETIMEDOUT;
        task_sleep_ms(1);
    }
    return off_result;
}
static int sleep_error(int e)
{
    if (e==-VFS_ETIMEOUT) return -ETIMEDOUT;
    if (e==-VFS_ECANCELLED) return -EINTR;
    if (e==-FS_EQUARANTINED || e==-FS_EUCLEAN || e==-FS_ELOOP) return -EIO;
    return e;
}
static bool can_standby(void *ctx)
{ (void)ctx; return !off_pending && apm_backend_can_standby(&backend); }
static int request_standby(void *ctx)
{
    (void)ctx;
    if (g_current == apm_worker || off_pending || sleep_pending || cancel_pending) return -EBUSY;
    /* Flush on the requesting task while APM continues polling. The flush
     * mailbox owns its request after a timeout; never abandon live DMA. */
    uint64_t end = deadline_after_ms(60000);
    int e;
    while ((e = storage_sync_bounded(end)) == -EAGAIN && !deadline_passed(end)) task_sleep_ms(1);
    if (e) return sleep_error(e==-EAGAIN && deadline_passed(end) ? -VFS_ETIMEOUT : e);
    e = power_devices_suspend();
    if (e) return sleep_error(e);
    uint32_t f = irq_save();
    if (!can_standby(NULL)) {
        irq_restore(f); (void)power_devices_resume(); return -ENOTSUP;
    }
    sleep_done = false; sleep_pending = true; sleep_deadline = deadline_after_ms(APM_POWER_MS);
    irq_restore(f);
    /* Once firmware owns the request a wall-clock timeout cannot cancel it:
     * sleep can last arbitrarily long. The contained BIOS call is bounded
     * while executing; only its worker publishes completion. */
    while (!sleep_done) task_sleep_ms(1);
    e = power_devices_resume();
    return sleep_error(e ? e : sleep_result);
}
static bool sleep_ready(void *ctx) { (void)ctx; return power_devices_resume_ready(); }
static int request_cancel_standby(void *ctx)
{
    (void)ctx;
    uint32_t f=irq_save();
    if (g_current==apm_worker || sleep_pending || off_pending || cancel_pending) {irq_restore(f);return -EBUSY;}
    if (backend.state!=APM_CONNECTED) {irq_restore(f);return 0;}
    cancel_deadline=deadline_after_ms(APM_POWER_MS);cancel_done=false;cancel_pending=true;
    irq_restore(f);
    while (!cancel_done) task_sleep_ms(1);
    return cancel_result;
}
static int reserve(void *ctx)
{
    (void)ctx;
    /* V86 containment exists. The remaining admission is the global
     * storage/DMA/time owner, not an invented ring-0 firmware broker. */
    if (!power_source_platform_ready()) return -ENOTSUP;
    const struct power_source_ops ops = {.soft_off = request_off,
        .can_standby = can_standby, .standby = request_standby, .cancel_standby = request_cancel_standby};
    int e = power_source_reserve(POWER_SOURCE_APM, &ops);
    if (e) return e;
    /* Reserve before allocating/starting shared workers: ACPI cannot race
     * this admission and initialize a second flush consumer. */
    e = storage_power_init();
    if (e) power_source_release(POWER_SOURCE_APM, false, e);
    return e;
}
static void release(void *ctx, bool uncertain)
{
    (void)ctx;
    power_source_release(POWER_SOURCE_APM, uncertain, backend.error ? backend.error : -EIO);
}
static uint64_t now(void *ctx) { (void)ctx; return deadline_after_ms(0); }
static int step(void *ctx, enum apm_stage stage, uint64_t end)
{
    (void)ctx;
    return stage == APM_FLUSH ? storage_sync_bounded(end) : power_source_quiesce_dma(end);
}
static int resume(void *ctx, uint16_t event, uint16_t info)
{
    (void)ctx;
    return power_source_resume(event, info);
}
static void publish(void *ctx, const struct apm_status *s)
{
    (void)ctx;
    struct power_source_status status = {.ac = s->ac, .percent = s->percent, .seconds = s->seconds,
                                         .battery_flags = s->battery_flags, .units = s->units};
    for (unsigned i = 0; i < APM_UNITS && i < POWER_SOURCE_BATTERIES; i++)
        status.battery[i] = (struct power_source_battery){s->unit[i].present, s->unit[i].percent,
                                                          s->unit[i].seconds, s->unit[i].flags};
    power_source_publish(POWER_SOURCE_APM, &status);
}
static void event(void *ctx, uint16_t code, uint16_t info)
{
    (void)ctx;
    power_source_event(POWER_SOURCE_APM, code, info);
}
static int orderly(void *ctx, uint64_t end)
{
    (void)ctx;
    return power_source_orderly_shutdown(end);
}
static void emergency_record(void *ctx, int error)
{
    (void)ctx;
    klog("[apm] stage=emergency-quiesce error=%d acknowledge=suspend", error);
}
/* fx-18: the chipset window the APM BIOS needs, from the FADT and the
 * qualified chipset (ICH3-M PMBASE, 128 bytes), installed before the probe. */
static uint16_t sci_state = 0xffff;
static struct apm_port_window window;
static void install_window(void)
{
    struct apm_pm_blocks fadt;
    bool have_fadt = acpi_pm_blocks && !acpi_pm_blocks(&fadt);
    struct registry_smi chip;
    bool have_chip = registry_smi_snapshot && registry_smi_snapshot(&chip) && chip.decoded && chip.base;
    apm_port_window_build(&window, have_fadt ? &fadt : NULL, have_chip ? chip.base : 0, have_chip ? 128 : 0);
    if (have_chip) sci_state = chip.pm1_cnt & 1;
    (void)biosvm_apm_set_window(&window);
    klog("[apm] window ranges=%u source=%s%s pm1_cnt=%04x chipset=%04x sci_en=%s",
         window.count, window.source & APM_PORT_SOURCE_FADT ? "fadt" : "none",
         window.source & APM_PORT_SOURCE_CHIPSET ? "+chipset" : "",
         have_fadt ? fadt.pm1a_cnt : 0, have_chip ? chip.base : 0,
         sci_state == 0xffff ? "unknown" : sci_state ? "1" : "0");
}
/* fx-18: with ACPI tables, the first firmware call waits for the ACPI
 * inventory's ownership decision (bounded). */
#define APM_SETTLE_MS 60000u
static bool wait_acpi(void)
{
    if (!g_boot.acpi_rsdp_phys) return true;
    uint64_t end = deadline_after_ms(APM_SETTLE_MS);
    while (!power_source_acpi_decided()) {
        if (deadline_passed(end)) return false;
        task_sleep_ms(50);
    }
    return true;
}
/* fx-25: a transport failure names the function and the BIOS VM's exact
 * reason (port, CS:IP, SS:SP, time against the budget). */
static void call_failure(int e)
{
    if (!e || !backend.transport_error || e == -EAGAIN) return;
    struct biosvm_failure f; biosvm_last_failure(&f);
    if (!f.error) {
        klog("[apm] call fn=%04x error=%d reason=broker", backend.last_function, e);
        return;
    }
    klog("[apm] call fn=%04x error=%d reason=%s cs=%04x ip=%04x ss=%04x sp=%04x port=%04x width=%u write=%u "
         "elapsed_ms=%u deadline_ms=%u budget_ms=%u",
         backend.last_function, e, f.reason ? f.reason : "error", f.cs, f.ip, f.ss, f.sp, f.port, f.width,
         (unsigned)f.write, f.elapsed_ms, f.deadline_ms, f.budget_ms);
}
static const char *reason(int e)
{
    if (backend.state == APM_CONNECTED) return "real_mode";
    if (backend.state == APM_EXCLUDED)
        return g_boot.acpi_rsdp_phys && !power_source_acpi_decided() ? "acpi_unsettled" :
               g_boot.acpi_rsdp_phys ? "acpi_owner" : "acpi_present";
    if (backend.state == APM_DISCOVERED) return e == -EAGAIN ? "ec_busy" : "platform_quiesce_owner";
    if (backend.state == APM_ABSENT && backend.bios_error == 0x86) return "no_apm_bios";
    return e ? "firmware" : "real_mode";
}
static int discover(void)
{
    const struct apm_ops ops = {.real_call = firmware_call, .acpi_present = acpi_present,
        .reserve = reserve, .release = release, .now_ms = now, .step = step,
        .resume = resume, .publish = publish, .event = event,
        .orderly_shutdown = orderly, .emergency_record = emergency_record,
        .now_us = now_us, .low_battery = low_battery, .handoff = handoff, .sleep_ready = sleep_ready};
    apm_backend_init(&backend, &ops);
    int e;
    if (!wait_acpi()) {
        backend.state = APM_EXCLUDED; e = -EBUSY;
        klog("[apm] state=%u version=0000 flags=0000 error=%d connected=0 reason=acpi_unsettled", backend.state, e);
        return e;
    }
    if (acpi_present(NULL)) {
        /* OS-owned or uncertain ACPI: no APM BIOS call at all. */
        backend.state = APM_EXCLUDED; e = -EBUSY;
        klog("[apm] state=%u version=0000 flags=0000 error=%d connected=0 reason=%s", backend.state, e, reason(e));
        return e;
    }
    install_window();
    /* EAGAIN: the EC hold was busy and nothing ran; retry a few times. */
    for (unsigned i = 0; (e = apm_backend_probe(&backend)) == -EAGAIN && i < 5; i++) task_sleep_ms(100);
    if (!e) for (unsigned i = 0; (e = apm_backend_connect(&backend)) == -EAGAIN && i < 5; i++) task_sleep_ms(100);
    if (!e) {
        e = power_source_activate(POWER_SOURCE_APM);
        /* Only our reservation can be activated. On failure disconnect
         * through V86 and retain the reservation if that cannot complete. */
        if (e) {
            struct biosvm_regs r = {.interrupt = 0x15, .eax = 0x5304};
            int disconnect = biosvm_apm_call(&r, APM_STATUS_MS);
            power_source_release(POWER_SOURCE_APM, disconnect || (r.flags & V86_CF), e);
            backend.state = APM_FAILED;
            backend.connected = backend.reserved = false;
        }
    }
    if (e) call_failure(e);
    klog("[apm] state=%u version=%04x flags=%04x error=%d connected=%u reason=%s bios_error=%02x",
         backend.state, backend.version, backend.flags, e, backend.state == APM_CONNECTED, reason(e),
         backend.bios_error);
    return e;
}
/* fx-18: one cost record per 60 s (f1-100 SMI budget): every firmware
 * call's wall time includes the SMIs it raised. */
static uint64_t window_end;
static void cost_record(void)
{
    uint64_t t = deadline_after_ms(0);
    if (!window_end) { window_end = t + APM_WINDOW_MS; return; }
    if (t < window_end) return;
    window_end = t + APM_WINDOW_MS;
    struct apm_cost c; apm_backend_cost(&backend, &c);
    struct biosvm_apm_io io = {0}; biosvm_apm_io_snapshot(&io);
    struct power_source_snapshot s; power_source_snapshot(&s);
    klog("[apm] window calls=%u events=%u event_calls=%u status_calls=%u skipped=%u max_us=%u total_us=%llu "
         "io_allowed=%llu io_denied=%llu ac=%d percent=%d seconds=%d units=%u",
         c.calls, c.events, c.event_calls, c.status_calls, c.skipped, c.max_us, (unsigned long long)c.total_us,
         (unsigned long long)io.allowed, (unsigned long long)io.denied,
         (int)s.status.ac, (int)s.status.percent, (int)s.status.seconds, s.status.units);
}
static void service_once(void)
{
    bool was_connected = backend.state == APM_CONNECTED;
    int e = apm_backend_service(&backend);
    if (was_connected && backend.state == APM_EXCLUDED)
        klog("[apm] handoff to=acpi disconnect=%s error=%d", e ? "uncertain" : "clean", e);
    else if (e) {
        call_failure(e);
        klog("[apm] service error=%d emergency_error=%d state=%u", e, backend.emergency_error, backend.state);
    }
    if (backend.state == APM_CONNECTED) cost_record();
    if (cancel_pending) {
        int r=deadline_passed(cancel_deadline) ? -ETIMEDOUT : apm_backend_cancel_standby(&backend);
        if (r!=-EAGAIN) {uint32_t f=irq_save();cancel_result=r;cancel_pending=false;cancel_done=true;irq_restore(f);}
    }
    if (sleep_pending) {
        int r = deadline_passed(sleep_deadline) ? -ETIMEDOUT : apm_backend_standby(&backend);
        if (r != -EAGAIN) {
            uint32_t f = irq_save(); sleep_result = r; sleep_pending = false; sleep_done = true; irq_restore(f);
        }
    }
    if (off_pending) {
        int r = deadline_passed(off_deadline) ? -V86_ETIMEDOUT :
                backend.emergency ? -EBUSY : apm_backend_off(&backend);
        if (r != -EAGAIN) {
            uint32_t f = irq_save(); off_result = r; off_pending = false; off_done = true; irq_restore(f);
        }
    }
}
static void worker(void *unused)
{
    (void)unused;
    if (discover()) return;
    while (backend.state == APM_CONNECTED) {
        service_once();
        task_sleep_ms(10);
    }
    /* Complete a queued caller even if firmware failed before servicing it. */
    if (cancel_pending) {cancel_result=-ENODEV;cancel_pending=false;cancel_done=true;}
    if (sleep_pending) { sleep_result = -ENODEV; sleep_pending = false; sleep_done = true; }
    if (off_pending) { off_result = -ENODEV; off_pending = false; off_done = true; }
}
void apm_start(void)
{
    if (attempted || (g_boot.flags & (CBI_F_SAFE_MODE | CBI_F_HW_CHECK))) return;
    attempted = true;
    apm_worker = task_create_kernel("apm", worker, NULL, P_DEVICE);
    if (!apm_worker) klog("[apm] connected=0 error=%d reason=worker", -ENOMEM);
    else task_start(apm_worker);
}
