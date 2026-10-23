/* G1: the TZ entry of a program about to be spawned: the system zone's
 * POSIX rule (libciuki ciuki_tz_env, syscall 97); UTC0 in host harnesses.
 * Win32 programs get no TZ: their C runtime then reads
 * GetTimeZoneInformation, as on Windows.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_DESK_TZ_ENV_H
#define CIUKI_DESK_TZ_ENV_H
#include <stdio.h>
#include <time.h>
#ifdef __i386__
#include <ciuki/timezone.h>
#endif
#define DESK_TZ_ENV_BYTES 72
static inline char *desk_tz_env(char *out, size_t capacity)
{
#ifdef __i386__
    return ciuki_tz_env(out, capacity);
#else
    snprintf(out, capacity, "TZ=UTC0");
    return out;
#endif
}
/* Local broken-down time of the system zone (libc localtime follows TZ or
 * the system zone); the host harnesses' system zone is UTC. */
static inline struct tm *desk_localtime(const time_t *t, struct tm *out)
{
#ifdef __i386__
    return localtime_r(t, out);
#else
    struct tm *g = gmtime(t);           /* ISO C, no POSIX feature macro needed */
    if (!g) return NULL;
    *out = *g;
    return out;
#endif
}
#endif
