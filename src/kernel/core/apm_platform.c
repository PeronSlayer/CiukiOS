/* APM real-mode worker and exclusive cached power provider.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/storage.h>
#include <ciuki/power_source.h>
#include "apm.h"

static struct apm_backend backend;
static bool attempted;
static struct task *apm_worker;
static bool off_pending, off_done;
static int off_result;
static uint64_t off_deadline;

static int firmware_call(void *ctx, struct biosvm_regs *r, uint32_t ms)
{
    (void)ctx;
    return biosvm_apm_call(r, ms);
}
static bool acpi_present(void *ctx)
{
    (void)ctx;
    return g_boot.acpi_rsdp_phys != 0;
}
static int request_off(void *ctx)
{
    (void)ctx;
    if (g_current == apm_worker) return -EBUSY;
    uint32_t f = irq_save();
    if (off_pending || backend.state != APM_CONNECTED) { irq_restore(f); return -EBUSY; }
    off_done = false; off_pending = true;
    off_deadline = deadline_after_ms(APM_QUIESCE_MS + APM_POWER_MS + 500);
    irq_restore(f);
    while (!off_done) {
        if (deadline_passed(off_deadline)) return -V86_ETIMEDOUT;
        task_sleep_ms(1);
    }
    return off_result;
}
static int reserve(void *ctx)
{
    (void)ctx;
    /* V86 containment exists. The remaining admission is the global
     * storage/DMA/time owner, not an invented ring-0 firmware broker. */
    if (!power_source_platform_ready()) return -ENOTSUP;
    const struct power_source_ops ops = {.soft_off = request_off};
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
    struct power_source_status status = {s->ac, s->percent, s->seconds, s->battery_flags};
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
static int discover(void)
{
    const struct apm_ops ops = {.real_call = firmware_call, .acpi_present = acpi_present,
        .reserve = reserve, .release = release, .now_ms = now, .step = step,
        .resume = resume, .publish = publish, .event = event,
        .orderly_shutdown = orderly, .emergency_record = emergency_record};
    apm_backend_init(&backend, &ops);
    int e = apm_backend_probe(&backend);
    if (!e) e = apm_backend_connect(&backend);
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
    klog("[apm] state=%u version=%04x flags=%04x error=%d connected=%u reason=%s",
         backend.state, backend.version, backend.flags, e, backend.state == APM_CONNECTED,
         backend.state == APM_EXCLUDED ? "acpi_present" :
         backend.state == APM_DISCOVERED ? "platform_quiesce_owner" : e ? "firmware" : "real_mode");
    return e;
}
static void service_once(void)
{
    int e = apm_backend_service(&backend);
    if (e) klog("[apm] service error=%d emergency_error=%d state=%u", e, backend.emergency_error, backend.state);
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
