/* SPDX-License-Identifier: MIT
 * System power requests (desktop shell "Shut Down..."): kernel call 73,
 * ciuki_power(action), added by the kernel's power interface. Until a kernel
 * implements it the call fails with ENOSYS and callers report it unavailable. */
#ifndef CIUKI_SDK_POWER_H
#define CIUKI_SDK_POWER_H
#include <stdint.h>
#define CIUKI_POWER_CALL 73u
#define CIUKI_POWER_REBOOT 1u
#define CIUKI_POWER_OFF 2u
/* Returns 0 (normally it does not return), or -1 with errno set:
 * ENOSYS when the running kernel has no power interface, EPERM for a
 * caller without the authority, EINVAL for an unknown action. */
int ciuki_power(uint32_t action);
#endif
