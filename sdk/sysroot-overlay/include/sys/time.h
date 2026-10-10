/* SPDX-License-Identifier: MIT */
#ifndef _SYS_TIME_H
#define _SYS_TIME_H
#include <sys/types.h>
#include <sys/_timespec.h>
#define timeval ciuki_timeval
int gettimeofday(struct timeval *, void *);
#endif
