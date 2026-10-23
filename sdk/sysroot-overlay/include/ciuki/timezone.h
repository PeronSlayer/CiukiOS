/* G1: the system time zone and clock (call 97, CIUKI_SYS_TIME; record and
 * flags in ciuki/abi.h, docs/design/local-time.md).
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_USER_TIMEZONE_H
#define CIUKI_USER_TIMEZONE_H
#include <ciuki/abi.h>
#include <stddef.h>
/* One operation (CIUKI_TIME_GET, _SET_ZONE, _SET_LOCAL, _SET_UTC,
 * _ACKNOWLEDGE); version, size and operation are filled in. The record
 * holds the state after the call. 0, or -1 with errno (EPERM without the
 * time authority, EINVAL for a malformed rule or time, EBUSY, EIO). */
int ciuki_time_zone(struct ciuki_time_zone *zone, uint32_t operation);
/* "TZ=<rule>" of the system zone for a program about to be spawned (the
 * standard-time part when automatic daylight saving is off); "TZ=UTC0"
 * when the service is unavailable. Returns out. */
char *ciuki_tz_env(char *out, size_t capacity);
#endif
