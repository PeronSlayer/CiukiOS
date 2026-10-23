/* The APM suspend-service owner (core/power_platform.c) with the production
 * APM provider/worker and power-source arbiter. Only firmware, the native
 * stop path, timers, the RTC clock and task/wait seams are replaced.
 * SPDX-License-Identifier: GPL-2.0-only */
#define CIUKI_CPU_H
#include <stdio.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/task.h>
#include <ciuki/sync.h>
#include <ciuki/power_source.h>
static uint32_t irq_save(void) { return 0x200; }
static void irq_restore(uint32_t f) { (void)f; }
#define P2V(p) ((void *)(uintptr_t)(p))
#define V2P(p) ((uint32_t)(uintptr_t)(p))
#include <ciuki/storage.h>
#include "../../src/kernel/core/apm.h"
static unsigned failures, checks, connects, disconnects, acks, offs, rejects, stops, timers, clocks, wakes;
static unsigned apm_tasks, platform_tasks, started;
static int stop_error;
static bool fail_platform_task, clock_ok = true;
static uint16_t next_event;
static uint64_t ticks;
static char log_line[256], resume_line[256];
static struct task fake_tasks[4];
struct task *g_current;
struct ciuki_boot_info g_boot;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)
int biosvm_apm_call(struct biosvm_regs *r, uint32_t ms)
{
    CHECK(r->interrupt == 0x15 && ms && ms <= 2000);
    r->flags = 0;
    switch (r->eax) {
    case 0x5300: r->eax = 0x0102; r->ebx = 0x504d; r->ecx = 0; break;
    case 0x5301: connects++; break;
    case 0x5304: disconnects++; break;
    case 0x530e: r->eax = 0x0102; break;
    case 0x530a:
        if (r->ebx & 0x8000) { r->flags = 1; r->eax = 0x0900; break; } /* no battery sockets */
        r->ebx = 0x0100; r->ecx = 40; r->edx = 600; break;
    case 0x530b: if (next_event) { r->ebx = next_event; next_event = 0; } else { r->flags = 1; r->eax = 0x8000; } break;
    case 0x5307:
        CHECK(r->ebx == 1);
        if (r->ecx == 2) acks++; else if (r->ecx == 3) offs++; else if (r->ecx == 5) rejects++; else CHECK(false);
        break;
    default: CHECK(false);
    }
    return 0;
}
struct task *task_create_kernel(const char *name, void (*fn)(void *), void *arg, enum task_prio prio)
{
    (void)fn; CHECK(!arg && prio == P_DEVICE);
    if (!strcmp(name, "apm")) { apm_tasks++; return &fake_tasks[0]; }
    CHECK(!strcmp(name, "power-quiesce")); platform_tasks++;
    return fail_platform_task ? NULL : &fake_tasks[1];
}
void task_start(struct task *t) { CHECK(t == &fake_tasks[0] || t == &fake_tasks[1]); if (t == &fake_tasks[1]) started++; }
void kwait_init(struct kwait *q) { memset(q, 0, sizeof(*q)); }
void kwait_wake_all(struct kwait *q) { (void)q; wakes++; }
bool kwait_wait_until(struct kwait *q, kwait_cond_fn cond, void *arg, uint64_t d) { (void)q; (void)d; return cond(arg); }
int storage_power_init(void) { return 0; }
int storage_sync_bounded(uint64_t end) { (void)end; return 0; }
uint64_t deadline_after_ms(uint32_t ms) { return ticks + ms; }
bool deadline_passed(uint64_t end) { return ticks >= end; }
/* The call-73 stop path (network, audio, USB) and the time base. */
int power_devices_quiesce(void) { stops++; return stop_error; }
void pic_pit_resume(void) { timers++; }
bool file_clock_resync(void) { clocks++; return clock_ok; }
void klog(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vsnprintf(log_line, sizeof(log_line), fmt, ap); va_end(ap);
    if (strstr(log_line, "[power] platform resume")) memcpy(resume_line, log_line, sizeof(resume_line));
}
#include <ciuki/apm_ports.h>
int biosvm_apm_set_window(const struct apm_port_window *w) { CHECK(w && w->count); return 0; }
void biosvm_apm_io_snapshot(struct biosvm_apm_io *out) { *out = (struct biosvm_apm_io){0}; }
void biosvm_last_failure(struct biosvm_failure *out) { *out = (struct biosvm_failure){0}; }
#define APM_PLATFORM_HOST_CLOCK
static uint64_t now_us(void *ctx) { (void)ctx; return ticks * 1000; }
#include "../../src/kernel/core/power_source.c"
#include "../../src/kernel/core/apm_platform.c"
#include "../../src/kernel/core/power_platform.c"
void task_sleep_ms(uint32_t ms) { ticks += ms; service_once(); }
/* A fresh boot's stop state under the same installed services (the arbiter
 * refuses a second installation once APM holds the reservation). */
static void reset_owner(void) { quiesce_stage = PLATFORM_IDLE; quiesce_result = 0; }
/* One critical suspend: flush, DMA stop, firmware acknowledgement. */
static void critical_event(void) { next_event = 8; ticks += 1000; service_once(); }
int main(void)
{
    /* fx-18: the owner no longer depends on the RSDP (APM is admitted on a
     * firmware-owned ACPI machine); an OS-owned ACPI still excludes APM. */
    g_boot.acpi_rsdp_phys = 0xe0000; power_source_acpi_settled(CIUKI_POWER_OWNED);
    CHECK(discover() == -EBUSY && backend.state == APM_EXCLUDED && !connects && strstr(log_line, "acpi_owner"));
    acpi_settled = false; acpi_ownership = CIUKI_POWER_PENDING;
    g_boot.acpi_rsdp_phys = 0;
    g_boot.flags = CBI_F_HW_CHECK; CHECK(power_platform_start() == -EPERM && !platform_tasks); g_boot.flags = 0;
    /* No worker: no owner, never a success-returning substitute. */
    fail_platform_task = true;
    CHECK(power_platform_start() == -ENOMEM && platform_tasks == 1 && !power_source_platform_ready());
    CHECK(discover() == -ENOTSUP && !connects && strstr(log_line, "platform_quiesce_owner"));
    fail_platform_task = false;

    /* Installed once, before APM: APM now connects on a pre-ACPI machine. */
    CHECK(!power_platform_start() && platform_tasks == 2 && started == 1 && power_source_platform_ready());
    CHECK(strstr(log_line, "owner=installed"));
    CHECK(!power_platform_start() && platform_tasks == 2 && started == 1);
    const struct power_source_platform other = {.quiesce_dma = platform_quiesce_dma, .resume = platform_resume};
    CHECK(power_source_set_platform(&other) == -EBUSY);
    g_current = &fake_tasks[2]; apm_worker = &fake_tasks[0];
    CHECK(!discover() && connects == 1 && backend.state == APM_CONNECTED);
    CHECK(strstr(log_line, "connected=1"));
    struct power_source_ops acpi = {.soft_off = request_off};
    CHECK(power_source_reserve(POWER_SOURCE_ACPI, &acpi) == -EAGAIN); power_source_handoff_cancel();
    service_once();
    struct ciuki_power_status record; power_source_status(&record);
    CHECK(record.provider == CIUKI_POWER_PROVIDER_APM && record.ownership == CIUKI_POWER_OWNED &&
          record.batteries[0].remaining == 40 && record.batteries[0].seconds == 600);

    /* Critical suspend: the stop runs on the owner's worker while the APM
     * worker keeps servicing; firmware is acknowledged only after it. */
    critical_event(); CHECK(backend.emergency && backend.stage == APM_STOP_DMA && !acks);
    service_once(); CHECK(quiesce_stage == PLATFORM_REQUESTED && wakes == 1 && !stops && !acks);
    ticks += 10; service_once(); CHECK(!acks && backend.emergency && wakes == 1);
    quiesce_step(); CHECK(stops == 1 && quiesce_stage == PLATFORM_DONE && strstr(log_line, "result=0"));
    service_once();
    CHECK(acks == 1 && !backend.emergency && !backend.emergency_error && backend.state == APM_CONNECTED);
    CHECK(timers == 1 && clocks == 1 && strstr(resume_line, "event=4 timers=1 clock=rtc devices=stopped_until_reboot"));
    /* Terminal for this boot: a second critical suspend reuses the result. */
    critical_event(); service_once();
    CHECK(acks == 2 && stops == 1 && !backend.emergency_error && timers == 2);

    /* Expiry: still acknowledged, recorded as a timeout, never as success;
     * the stop continues and its failure is kept (quarantined claims). */
    reset_owner(); CHECK(power_source_platform_ready() && owner_installed);
    critical_event(); service_once(); CHECK(quiesce_stage == PLATFORM_REQUESTED && acks == 2);
    ticks = backend.emergency_start + APM_QUIESCE_MS + 1; service_once();
    CHECK(acks == 3 && backend.emergency_error == -V86_ETIMEDOUT && stops == 1);
    CHECK(strstr(resume_line, "event=4 timers=1 clock=rtc devices=stopping"));
    stop_error = -EIO; quiesce_step(); CHECK(stops == 2 && quiesce_stage == PLATFORM_DONE && quiesce_result == -EIO);
    CHECK(platform_quiesce_dma(NULL, ticks + 100) == -EIO);

    /* Firmware-initiated resume and time notifications: timers only after
     * a suspend, the RTC clock for every notification. */
    unsigned t = timers, c = clocks;
    next_event = 3; ticks += 1000; service_once(); CHECK(timers == t + 1 && clocks == c + 1);
    next_event = 7; ticks += 1000; service_once(); CHECK(timers == t + 1 && clocks == c + 2);
    clock_ok = false; next_event = 11; ticks += 1000; service_once();
    CHECK(timers == t + 2 && clocks == c + 3 && strstr(resume_line, "event=11 timers=1 clock=unchanged devices=quarantined"));
    CHECK(backend.state == APM_CONNECTED);
    /* Ordinary suspend requests stay rejected. */
    next_event = 2; ticks += 1000; service_once(); CHECK(rejects == 1 && stops == 2);

    /* Soft-off is still call 73's: no Off before its stopped latch. */
    CHECK(power_source_soft_off() == -EBUSY && !offs);
    power_source_mark_stopped();
    CHECK(power_source_soft_off() == -EIO && offs == 1 && disconnects == 1 && !apm_tasks);
    printf("APM platform owner: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
