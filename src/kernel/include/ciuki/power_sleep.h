/* Reversible device power transaction; kernel private. */
#ifndef CIUKI_POWER_SLEEP_H
#define CIUKI_POWER_SLEEP_H
#include <stdbool.h>
bool net_power_resume_ready(void);
int net_power_suspend(void);
int net_power_resume(void);
bool audio_power_resume_ready(void);
int audio_power_suspend(void);
int audio_power_resume(void);
bool usb_power_resume_ready(void);
int usb_power_suspend(void);
int usb_power_resume(void);
bool ata_power_resume_ready(void);
int ata_power_suspend(void);
int ata_power_quiesce(void); /* storage fence: submissions refused, bus mastering and IRQ stopped or revoked; terminal */
int power_devices_quiesce_terminal(void); /* core/power_quiesce.c: the platform stop plus the ATA fence (call 73 only) */
int ata_power_resume(void);
int power_devices_suspend(void);
int power_devices_resume(void);
#endif
