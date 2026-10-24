/* Native shutdown barrier, after application closure and storage_sync.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/power_source.h>
extern int audio_power_quiesce(void);
extern int usb_power_quiesce(void);
extern int net_power_quiesce(void);
int power_devices_quiesce(void)
{
    int e = net_power_quiesce();
    if (!e) e = audio_power_quiesce();
    if (!e) e = usb_power_quiesce();
    return e;
}
/* G11 standby qualification (docs/design/power-policy.md): every native
 * driver holding DMA or interrupt state needs a suspend and a resume path.
 * net_power_quiesce, audio_power_quiesce and usb_power_quiesce are the
 * terminal S5 barrier: they release or quarantine their claims and nothing
 * restarts them, so no device is resume-ready and standby stays refused. */
bool power_devices_resume_ready(void) { return false; }
