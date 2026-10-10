/* SPDX-License-Identifier: MIT */
#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H
#include <sys/_types.h>
#include <ciuki/abi.h>
typedef ciuki_off_t off_t;
typedef ciuki_ssize_t ssize_t;
typedef ciuki_pid_t pid_t;
typedef ciuki_ino_t ino_t;
typedef ciuki_mode_t mode_t;
typedef ciuki_nlink_t nlink_t;
typedef ciuki_uid_t uid_t;
typedef ciuki_gid_t gid_t;
typedef ciuki_dev_t dev_t;
typedef ciuki_blksize_t blksize_t;
typedef ciuki_blkcnt_t blkcnt_t;
typedef ciuki_clock_t clock_t;
typedef ciuki_time_t time_t;
typedef ciuki_sigset_t sigset_t;
typedef int clockid_t;
typedef int timer_t;
typedef unsigned int useconds_t;
typedef int suseconds_t;
typedef char *caddr_t;
typedef uint8_t u_int8_t;
typedef uint16_t u_int16_t;
typedef uint32_t u_int32_t;
typedef uint64_t u_int64_t;
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
#define __time_t_defined 1
#define _TIME_T_DECLARED 1
#define __clock_t_defined 1
#define _CLOCK_T_DECLARED 1
#define __sigset_t_defined 1
#define __clockid_t_defined 1
#define __timer_t_defined 1
#endif
