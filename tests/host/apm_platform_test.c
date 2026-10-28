/* Production APM provider/worker with only firmware, time and device seams
 * replaced. The real power-source arbiter is included below.
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
#include "../../src/kernel/core/apm.h"
static unsigned failures, checks, probes, connects, disconnects, tasks, polls, acks, offs, stops;
static int flush_error, stop_error, firmware_error, storage_init_error;
static bool fail_task, broker_dead;
static uint16_t next_event, firmware_caps;
static unsigned device_sleeps, device_resumes, standby_calls, cancel_calls;
static int device_sleep_error;
static uint64_t ticks;
static char log_line[256];
static struct task fake_tasks[8];
struct task *g_current;
struct ciuki_boot_info g_boot;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)
int biosvm_apm_call(struct biosvm_regs *r, uint32_t ms)
{
    CHECK(r->interrupt == 0x15 && ms && ms <= 2000);
    uint32_t fn = r->eax;
    if (fn == 0x5304) disconnects++;
    if (broker_dead) return -EIO;
    if (firmware_error) { broker_dead = true; return firmware_error; }
    r->flags = 0;
    switch (fn) {
    case 0x5300: probes++; r->eax = 0x0102; r->ebx = 0x504d; r->ecx = 0; break;
    case 0x5301: connects++; break;
    case 0x5304: break;
    case 0x5310: CHECK(!r->ebx); r->ecx=firmware_caps; break;
    case 0x530e: r->eax = 0x0102; break;
    case 0x530a:
        if (r->ebx & 0x8000) { r->flags = 1; r->eax = 0x0900; break; } /* no battery sockets */
        r->ebx = 0x0100; r->ecx = 75; r->edx = 120; break;
    case 0x530b:
        polls++;
        if (next_event) { r->ebx = next_event; next_event = 0; }
        else { r->flags = 1; r->eax = 0x8000; }
        break;
    case 0x5307:
        CHECK(r->ebx == 1);
        if (r->ecx == 1) standby_calls++;
        else if (r->ecx == 2) { acks++; CHECK(stops); }
        else if (r->ecx == 3) offs++;
        else if (r->ecx==5) cancel_calls++; else CHECK(r->ecx==4);
        break;
    default: CHECK(false);
    }
    return 0;
}
struct task *task_create_kernel(const char *name, void (*fn)(void *), void *arg, enum task_prio prio)
{
    (void)fn; (void)arg;
    CHECK(!strcmp(name, "apm") && prio == P_DEVICE); tasks++;
    return fail_task ? NULL : &fake_tasks[tasks % 8];
}
void task_start(struct task *t) { CHECK(t != NULL); }
int power_devices_suspend(void) { device_sleeps++; return device_sleep_error; }
int power_devices_resume(void) { device_resumes++; return 0; }
int storage_power_init(void) { return storage_init_error; }
int storage_sync_bounded(uint64_t end) { CHECK(end >= ticks || flush_error); return flush_error; }
uint64_t deadline_after_ms(uint32_t ms) { return ticks + ms; }
bool deadline_passed(uint64_t end) { return ticks >= end; }
static int quiesce(void *ctx, uint64_t end) { (void)ctx; (void)end; stops++; return stop_error; }
static int recover(void *ctx, uint16_t event, uint16_t info) { (void)ctx; (void)event; (void)info; return 0; }
void klog(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vsnprintf(log_line, sizeof(log_line), fmt, ap); va_end(ap);
}
/* fx-18 seams: the BIOS VM window and its counters, a millisecond clock. */
#include <ciuki/apm_ports.h>
static unsigned windows;
int biosvm_apm_set_window(const struct apm_port_window *w) { CHECK(w && w->count); windows++; return 0; }
void biosvm_apm_io_snapshot(struct biosvm_apm_io *out) { *out = (struct biosvm_apm_io){0}; }
void biosvm_last_failure(struct biosvm_failure *out) { *out = (struct biosvm_failure){0}; }
#define APM_PLATFORM_HOST_CLOCK
static uint64_t now_us(void *ctx) { (void)ctx; return ticks * 1000; }
#include "../../src/kernel/core/power_source.c"
bool power_devices_resume_ready(void) { return true; }
/* sec-k08 (U4-3): the kernel's firmware-resume lock (power_source.c). */
static unsigned resume_locks;
void security_attention_resume(void) { resume_locks++; }
#include "../../src/kernel/core/apm_platform.c"
void task_sleep_ms(uint32_t ms) { ticks += ms; service_once(); }
int main(void)
{
    for (unsigned i = 0; i < 2; i++) {
        attempted = false; g_boot.flags = i ? CBI_F_HW_CHECK : CBI_F_SAFE_MODE;
        apm_start(); CHECK(!tasks && !probes);
    }
    g_boot.flags = 0; apm_start();
    CHECK(tasks == 1); CHECK(discover() == -ENOTSUP && connects == 0);
    CHECK(strstr(log_line, "platform_quiesce_owner"));
    apm_start(); CHECK(tasks == 1);
    /* fx-18: with ACPI tables APM waits (bounded) for the inventory's
     * decision; OS-owned or uncertain ACPI excludes it after the probe. */
    g_boot.acpi_rsdp_phys = 0xe0000; unsigned seen = probes;
    uint64_t before = ticks;
    CHECK(discover() == -EBUSY && backend.state == APM_EXCLUDED && !connects && probes == seen);
    CHECK(strstr(log_line, "acpi_unsettled") && ticks - before >= APM_SETTLE_MS);
    power_source_acpi_settled(CIUKI_POWER_OWNED);
    CHECK(discover() == -EBUSY && backend.state == APM_EXCLUDED && !connects);
    CHECK(strstr(log_line, "acpi_owner"));
    acpi_ownership = CIUKI_POWER_QUARANTINED;
    CHECK(discover() == -EBUSY && backend.state == APM_EXCLUDED && !connects);
    acpi_settled = false; acpi_ownership = CIUKI_POWER_PENDING;
    g_boot.acpi_rsdp_phys = 0;
    attempted = false; fail_task = true; apm_start();
    CHECK(tasks == 2 && strstr(log_line, "reason=worker")); fail_task = false;

    CHECK(power_source_set_platform(NULL) == -EINVAL);
    const struct power_source_platform platform = {.quiesce_dma=quiesce, .resume=recover};
    CHECK(!power_source_set_platform(&platform));
    CHECK(power_source_set_platform(&platform) == -EBUSY);
    g_current = &fake_tasks[0]; apm_worker = &fake_tasks[1];
    storage_init_error = -ENOMEM;
    CHECK(discover() == -ENOMEM && !connects && windows);
    struct power_source_snapshot unreserved;
    power_source_snapshot(&unreserved);
    CHECK(unreserved.provider == POWER_SOURCE_NONE && !unreserved.active);
    storage_init_error = 0;
    CHECK(!discover() && connects == 1);
    struct power_source_snapshot snap;
    power_source_snapshot(&snap);
    CHECK(snap.provider == POWER_SOURCE_APM && snap.active && snap.status.ac == POWER_SOURCE_UNKNOWN);
    struct power_source_ops ops = {.soft_off=request_off};
    /* fx-18: ACPI asks a healthy APM to yield (EAGAIN); cancelled here, the
     * handoff itself is exercised at the end. */
    CHECK(power_source_reserve(POWER_SOURCE_ACPI, &ops) == -EAGAIN && power_source_handoff_requested(POWER_SOURCE_APM));
    CHECK(!power_source_apm_admitted(false));
    power_source_handoff_cancel(); CHECK(power_source_apm_admitted(false));
    service_once();
    power_source_snapshot(&snap); CHECK(snap.status.ac == 1 && snap.status.percent == 75 && snap.status.seconds == 120);
    unsigned calls = polls; power_source_snapshot(&snap); CHECK(polls == calls);
    next_event = 6; ticks += 1000; service_once();
    power_source_snapshot(&snap); CHECK(snap.events == 1 && snap.last_event == 6);
    /* Without the call-73 stopped latch neither the arbiter nor the APM
     * backend reaches firmware Off. */
    firmware_caps=3; backend.capabilities_due=true; service_once();
    CHECK(power_source_standby_reason()==CIUKI_POWER_STANDBY_AVAILABLE);
    CHECK(!power_source_standby() && standby_calls==1 && device_sleeps==1 && device_resumes==1);
    CHECK(!sleep_pending && !off_running && !stopped);
    /* sec-k08 (U4-3): the BIOS's resume notification of a call-73 Stand by
     * (armed by the desktop) does not lock; an unsolicited one does. */
    next_event = 11; ticks += 1000; service_once(); CHECK(!resume_locks);
    next_event = 3; ticks += 1000; service_once(); CHECK(resume_locks == 1);
    next_event = 4; ticks += 1000; service_once(); CHECK(resume_locks == 2);
    next_event = 6; ticks += 1000; service_once(); CHECK(resume_locks == 2);
    backend.sleep_request=1;CHECK(!power_source_cancel_standby() && !backend.sleep_request && cancel_calls==1);
    flush_error=-VFS_ETIMEOUT;
    CHECK(power_source_standby()==-ETIMEDOUT && device_sleeps==1 && standby_calls==1);
    flush_error=0;device_sleep_error=-EBUSY;
    CHECK(power_source_standby()==-EBUSY && device_sleeps==2 && standby_calls==1);
    device_sleep_error=0;firmware_caps=0;backend.capabilities_due=true;service_once();
    CHECK(power_source_can_off());
    CHECK(power_source_soft_off() == -EBUSY && !offs);
    CHECK(apm_backend_off(&backend) == -EBUSY && !offs && backend.state == APM_CONNECTED);
    struct ciuki_power_status record;
    power_source_status(&record);
    CHECK(record.provider == CIUKI_POWER_PROVIDER_APM && record.ownership == CIUKI_POWER_OWNED);
    CHECK(record.ac_online == 1 && record.battery_count == 1 && record.lid_open == CIUKI_POWER_UNKNOWN);
    CHECK(record.batteries[0].unit == CIUKI_POWER_UNIT_PERCENT && record.batteries[0].full == 100 &&
          record.batteries[0].remaining == 75 && record.batteries[0].seconds == 120 && record.batteries[0].state == 0);
    power_source_mark_stopped();
    CHECK(power_source_soft_off() == -EIO && offs == 1 && disconnects == 1);
    power_source_snapshot(&snap); CHECK(!snap.active && snap.provider == POWER_SOURCE_NONE);
    CHECK(power_source_soft_off() == -ENOTSUP);

    CHECK(!discover()); next_event = 8; ticks += 1000; flush_error = -EAGAIN;
    service_once(); CHECK(backend.emergency && !acks);
    CHECK(resume_locks == 3);   /* sec-k08: locked at the Critical Suspend notification, before the acknowledgment */
    ticks = backend.emergency_start + APM_FLUSH_MS; flush_error = -VFS_ETIMEOUT;
    service_once(); stop_error = -EIO; service_once();
    CHECK(acks == 1 && !backend.emergency && backend.emergency_error == -VFS_ETIMEOUT);
    ticks += 1000; firmware_error = -EFAULT; service_once();
    CHECK(backend.state == APM_FAILED && disconnects == 2);
    power_source_snapshot(&snap); CHECK(!snap.active && snap.provider == POWER_SOURCE_APM && snap.error == -EFAULT);
    power_source_status(&record);
    CHECK(record.provider == CIUKI_POWER_PROVIDER_APM && record.ownership == CIUKI_POWER_QUARANTINED &&
          record.error == EFAULT && !record.battery_count && record.ac_online == CIUKI_POWER_UNKNOWN);
    CHECK(power_source_reserve(POWER_SOURCE_ACPI, &ops) == -EBUSY);
    CHECK(power_source_activate(POWER_SOURCE_APM) == -EBUSY);
    power_source_release(POWER_SOURCE_NONE, false, 0);
    power_source_snapshot(&snap); CHECK(snap.provider == POWER_SOURCE_APM);
    printf("APM production provider: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
