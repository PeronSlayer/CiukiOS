/* G1: the system time zone service (src/kernel/proc/timezone.c).
 * The kernel keeps UTC (file_clock_*); local time is derived from the
 * configured POSIX rule. The RTC holds local time when the configuration
 * says so (Windows convention) and its bias is persisted in clock.cfg.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SYSTIME_H
#define CIUKI_SYSTIME_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ciuki/abi.h>
struct process;
struct trap_frame;
/* Persistent store of system/settings/clock.cfg (setup_clock.c in the
 * kernel, a fake in host tests): 0 or -errno; read returns -ENOENT when the
 * file is absent. */
int timezone_store_read(char *text, size_t capacity, size_t *length);
int timezone_store_write(const char *text, size_t length);

int file_clock_rtc_offset(void);            /* seconds; loads clock.cfg once */
int32_t timezone_offset_at(int64_t utc);    /* seconds east of UTC */
int64_t timezone_local_utc(int64_t local);  /* local wall time to UTC */
/* One record operation for the caller (syscall 97 and the Win32 transport).
 * The record is updated with the state after the operation. */
int timezone_call(struct process *, struct ciuki_time_zone *);
int32_t time_syscall(struct trap_frame *);
/* Rewrites the RTC when the local offset changed (daylight saving) and the
 * RTC holds local time. Called on every GET; returns true when it moved. */
bool timezone_poll(void);
/* "TZ=<rule>" for spawned programs; the length, 0 on a short buffer. */
size_t timezone_env(char *out, size_t capacity);
void timezone_reset(void);                  /* host tests: forget the loaded state */
void timezone_set_store(int (*read)(char *, size_t, size_t *), int (*write)(const char *, size_t));
#endif
