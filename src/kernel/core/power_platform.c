/* Native DMA/time owner of the APM suspend services (f1-121 platform service,
 * lead decision 2026-10-23). Installed only on pre-ACPI machines, before APM
 * connects. ACPI never uses these services; any RSDP leaves them absent and
 * APM excluded, so the ACPI/APM exclusivity is unchanged.
 *
 * quiesce_dma runs call 73's native stop path (power_devices_quiesce:
 * network including NDIS external netifs, audio, USB) on its own worker, so
 * the APM worker keeps servicing firmware every 10 ms. It returns -EAGAIN
 * while that stop runs, the stop's result once done, and -ETIMEDOUT after
 * the deadline: the stop then continues, and its own failure path keeps
 * uncertain claims in quarantine. Nothing here retires another caller's I/O.
 * The stop path is the S5 barrier and terminal for this boot, so a later
 * request returns the first result.
 *
 * resume restores the time base before workloads continue: after a
 * suspend/resume notification the 8259A pair and PIT channel 0, and for
 * every notification the wall clock from the boot-qualified RTC. Native
 * devices stopped by quiesce_dma stay stopped (proved idle, or quarantined)
 * until the next boot: no driver restart path exists yet.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/arch.h>
#include <ciuki/task.h>
#include <ciuki/sync.h>
#include <ciuki/clock.h>
#include <ciuki/power_source.h>

extern int power_devices_quiesce(void);

enum { PLATFORM_IDLE, PLATFORM_REQUESTED, PLATFORM_DONE };
static struct task *quiesce_worker;
static struct kwait quiesce_wake;
static int quiesce_stage, quiesce_result;
static bool owner_installed;

static bool quiesce_requested(void *arg) { (void)arg; return quiesce_stage == PLATFORM_REQUESTED; }
static void quiesce_step(void)
{
    int e = power_devices_quiesce();
    klog("[power] platform stage=quiesce_dma result=%d", e);
    uint32_t f = irq_save(); quiesce_result = e; quiesce_stage = PLATFORM_DONE; irq_restore(f);
}
static void quiesce_main(void *arg)
{
    (void)arg;
    for (;;) if (kwait_wait_until(&quiesce_wake, quiesce_requested, 0, UINT64_MAX)) quiesce_step();
}
static int platform_quiesce_dma(void *ctx, uint64_t deadline_ms)
{
    (void)ctx;
    uint32_t f = irq_save();
    bool start = quiesce_stage == PLATFORM_IDLE;
    if (start) quiesce_stage = PLATFORM_REQUESTED;
    int now = quiesce_stage, result = quiesce_result;
    irq_restore(f);
    if (start) kwait_wake_all(&quiesce_wake);
    if (now == PLATFORM_DONE) return result;
    /* An expired deadline still starts the stop: the request is recorded
     * as a timeout, never as quiescence. */
    return deadline_passed(deadline_ms) ? -ETIMEDOUT : -EAGAIN;
}
/* APM 1.2 notifications: 3 normal resume, 4 critical resume, 11 standby
 * resume (firmware may have reprogrammed the PIC/PIT), 7 update time. */
static int platform_resume(void *ctx, uint16_t event, uint16_t info)
{
    (void)ctx; (void)info;
    bool timers = event == 3 || event == 4 || event == 11;
    if (timers) pic_pit_resume();
    bool clock = file_clock_resync();
    uint32_t f = irq_save(); int now = quiesce_stage, result = quiesce_result; irq_restore(f);
    klog("[power] platform resume event=%u timers=%u clock=%s devices=%s", event, timers,
         clock ? "rtc" : "unchanged",
         now == PLATFORM_IDLE ? "running" : now == PLATFORM_REQUESTED ? "stopping" :
         result ? "quarantined" : "stopped_until_reboot");
    return 0;
}
int power_platform_start(void)
{
    if (owner_installed) return 0;
    if (g_boot.acpi_rsdp_phys) { klog("[power] platform owner=none reason=acpi_present"); return -EBUSY; }
    if (g_boot.flags & (CBI_F_SAFE_MODE | CBI_F_HW_CHECK)) return -EPERM;
    kwait_init(&quiesce_wake);
    quiesce_worker = task_create_kernel("power-quiesce", quiesce_main, NULL, P_DEVICE);
    if (!quiesce_worker) { klog("[power] platform owner=none reason=worker"); return -ENOMEM; }
    static const struct power_source_platform services = {
        .quiesce_dma = platform_quiesce_dma, .resume = platform_resume};
    int e = power_source_set_platform(&services);
    if (e) { klog("[power] platform owner=none error=%d", e); return e; } /* worker never started */
    task_start(quiesce_worker);
    owner_installed = true;
    klog("[power] platform owner=installed services=quiesce_dma,resume");
    return 0;
}
