/* Native shutdown barrier, after application closure and storage_sync.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
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
