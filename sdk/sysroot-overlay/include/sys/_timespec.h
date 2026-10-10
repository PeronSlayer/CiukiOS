/* SPDX-License-Identifier: MIT */
#ifndef _SYS__TIMESPEC_H_
#define _SYS__TIMESPEC_H_
#include <sys/types.h>
#define timespec ciuki_timespec
int clock_gettime(clockid_t,struct timespec *);
int clock_getres(clockid_t,struct timespec *);
int nanosleep(const struct timespec *,struct timespec *);
#endif
