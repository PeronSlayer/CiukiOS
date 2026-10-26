/* Native shutdown barrier, after application closure and storage_sync.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/power_source.h>
#include <ciuki/power_sleep.h>
extern int audio_power_quiesce(void);
extern int usb_power_quiesce(void);
extern int net_power_quiesce(void);
/* The platform's quiesce step (APM critical suspend, the forced fence's
 * first part): network, audio and USB. Never storage: a critical suspend
 * resumes, and ATA has its own reversible transition (ata_power_suspend /
 * ata_power_resume below). Review 4, finding 5. */
int power_devices_quiesce(void)
{
    int e = net_power_quiesce();
    if (!e) e = audio_power_quiesce();
    if (!e) e = usb_power_quiesce();
    return e;
}
/* The terminal barrier of call 73 (orderly shutdown after storage_sync):
 * the same, then the ATA storage fence (review 3, finding 3): the channels
 * refuse new submissions and stop or revoke their bus mastering and
 * interrupts, so no disk DMA outlives it. Terminal for this boot. */
int power_devices_quiesce_terminal(void)
{
    int e = power_devices_quiesce();
    if (!e) e = ata_power_quiesce();
    return e;
}
/* Unlike S5, these owners keep all allocations and claims. A failed owner
 * rolls back its own partial work; this coordinator reverses earlier ones. */
static unsigned sleep_stage;
static bool sleep_failed;
bool power_devices_resume_ready(void)
{
    return !sleep_failed && net_power_resume_ready() && audio_power_resume_ready() &&
        usb_power_resume_ready() && ata_power_resume_ready();
}
int power_devices_resume(void)
{
    int first = 0, e;
    if (sleep_stage >= 4 && (e = ata_power_resume())) first = e;
    if (sleep_stage >= 3 && (e = usb_power_resume()) && !first) first = e;
    if (sleep_stage >= 2 && (e = audio_power_resume()) && !first) first = e;
    if (sleep_stage >= 1 && (e = net_power_resume()) && !first) first = e;
    sleep_stage = 0;
    if (first) sleep_failed = true;
    return first;
}
int power_devices_suspend(void)
{
    if (sleep_stage) return -EBUSY;
    if (!power_devices_resume_ready()) return -ENOTSUP;
    int e = net_power_suspend();
    if (!e) { sleep_stage = 1; e = audio_power_suspend(); }
    if (!e) { sleep_stage = 2; e = usb_power_suspend(); }
    if (!e) { sleep_stage = 3; e = ata_power_suspend(); }
    if (!e) { sleep_stage = 4; return 0; }
    int r = power_devices_resume();
    return r ? r : e;
}
