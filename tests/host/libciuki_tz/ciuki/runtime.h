/* G1 host shim of the libciuki runtime for sdk/libciuki/tz.c: the syscall
 * is answered by tests/host/libciuki_tz/tz_test.c, newlib's reentrancy and
 * tz globals are local stand-ins, and the public names are renamed so the
 * host C library stays the oracle.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_TZ_SHIM_RUNTIME_H
#define CIUKI_TZ_SHIM_RUNTIME_H
#include <ciuki/abi.h>
#include <stdint.h>
#define localtime_r cu_localtime_r
#define localtime cu_localtime
#define mktime cu_mktime
#define tzset cu_tzset
#define _tzset_r cu__tzset_r
#define _tzset_unlocked cu__tzset_unlocked
#define _tzset_unlocked_r cu__tzset_unlocked_r
#define _tzname cu__tzname
#define _timezone cu__timezone
#define _daylight cu__daylight
struct _reent { struct tm *_localtime_buf; };
#define _REENT_TM(r) ((r)->_localtime_buf)
struct _reent *__getreent(void);
void __tz_lock(void);
void __tz_unlock(void);
extern char *cu__tzname[2];
extern long cu__timezone;
extern int cu__daylight;
uint32_t shim_time_call(uintptr_t record, uint32_t size);
#define CU_PTR(p) ((uintptr_t)(p))
#define CU_CALL(n, b, c, d, s, i, p) shim_time_call((uintptr_t)(b), (uint32_t)(c))
static inline int ciuki_error(uint32_t r) { return r >= 0xfffff001u ? (int)-(int32_t)r : 0; }
#endif
