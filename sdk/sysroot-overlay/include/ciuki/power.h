/* Power control (call 73, execution-abi.md amendment 2026-10-12). Only the
 * desktop that PID 1 spawned may call it; other callers get EPERM.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_USER_POWER_H
#define CIUKI_USER_POWER_H
#include <ciuki/abi.h>
#define __CIUKI_POWER_VERSION__ 1
/* action: CIUKI_POWER_REBOOT, CIUKI_POWER_OFF or CIUKI_POWER_QUERY.
 * QUERY returns CIUKI_POWER_CAN_* and CIUKI_POWER_STOPPED/UNCERTAIN bits.
 * Either progress bit means the session cannot resume. REBOOT does not return on
 * success. OFF flushes storage and powers off through ACPI S5; it returns -1
 * with errno ENOTSUP (no soft-off method) or EIO (the machine stayed on)
 * after storage has stopped: show "It is now safe to turn off your
 * computer" only when QUERY also reports STOPPED. EBUSY with no progress
 * bit means open files prevented flushing; UNCERTAIN means a stop proof
 * failed after shutdown began: retain a terminal diagnostic and require reset. */
int ciuki_power(uint32_t action);
/* Read-only cached firmware status; initializes version/size for the caller.
 * Unknown values use CIUKI_POWER_UNKNOWN. No AML runs during this query. */
int ciuki_power_status(struct ciuki_power_status *status);
#endif
