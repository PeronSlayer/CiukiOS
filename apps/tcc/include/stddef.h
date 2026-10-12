/* SPDX-License-Identifier: MIT
 * CiukiOS ILP32 compiler types. Unlike upstream's convenience stddef.h,
 * this header leaves stdint and ssize_t to newlib's corresponding headers. */
#ifndef _STDDEF_H
#define _STDDEF_H
typedef unsigned int size_t;
typedef int ptrdiff_t;
typedef int wchar_t;
#define NULL ((void *)0)
#define offsetof(type, field) __builtin_offsetof(type, field)
#endif
/* newlib requests wint_t through a second include of stddef.h. */
#if defined(__need_wint_t)
#ifndef _WINT_T
#define _WINT_T
typedef __WINT_TYPE__ wint_t;
#endif
#undef __need_wint_t
#endif
