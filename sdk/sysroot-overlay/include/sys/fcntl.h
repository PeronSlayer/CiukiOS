/* SPDX-License-Identifier: MIT */
#ifndef _SYS_FCNTL_H
#define _SYS_FCNTL_H
#include <sys/types.h>
int open(const char *, int, ...);
int creat(const char *, mode_t);
int fcntl(int, int, ...);
#endif
