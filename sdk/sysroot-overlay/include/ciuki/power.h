/* Power control (call 73, execution-abi.md amendment 2026-10-12). Only the
 * desktop that PID 1 spawned may call it; other callers get EPERM.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_USER_POWER_H
#define CIUKI_USER_POWER_H
#include <ciuki/abi.h>
#define __CIUKI_POWER_VERSION__ 1
/* action: CIUKI_POWER_REBOOT, CIUKI_POWER_OFF or CIUKI_POWER_QUERY.
 * QUERY returns the CIUKI_POWER_CAN_* bits. REBOOT does not return on
 * success. OFF flushes storage and powers off through ACPI S5; it returns -1
 * with errno ENOTSUP (no soft-off method) or EIO (the machine stayed on)
 * after storage has stopped: show "It is now safe to turn off your
 * computer". EBUSY means files are still open and nothing was changed. */
int ciuki_power(uint32_t action);
#endif
