/* SPDX-License-Identifier: MIT */
#ifndef _SYS_UTSNAME_H
#define _SYS_UTSNAME_H
#include <ciuki/abi.h>
#define utsname ciuki_utsname
int uname(struct utsname *);
#endif
