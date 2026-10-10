/* SPDX-License-Identifier: MIT */
#ifndef _MACHINE__TYPES_H
#define _MACHINE__TYPES_H
#include <machine/_default_types.h>
/* Newlib internal counterparts; public aliases come from ciuki/abi.h. */
typedef __int64_t _off_t;
typedef __int64_t _fpos_t;
typedef __int64_t __blkcnt_t;
typedef __uint64_t __ino_t;
typedef __uint32_t __dev_t;
typedef __uint32_t __uid_t;
typedef __uint32_t __gid_t;
typedef __uint32_t __mode_t;
#define __machine_off_t_defined 1
#define __machine_fpos_t_defined 1
#define __machine_blkcnt_t_defined 1
#define __machine_ino_t_defined 1
#define __machine_dev_t_defined 1
#define __machine_uid_t_defined 1
#define __machine_gid_t_defined 1
#define __machine_mode_t_defined 1
#define __machine_clock_t_defined 1
#define _CLOCK_T_ __int64_t
#endif
