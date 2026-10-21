/* Replace only user-address translation and the syscall trap in the native
 * host wrapper fixture. Target compilation still uses the actual runtime.h.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_RUNTIME_H
#define CIUKI_RUNTIME_H
#include <ciuki/abi.h>
#include <stddef.h>
#include <sys/types.h>
#define _SSIZE_T_DECLARED
#define _OFF_T_DECLARED
uint32_t cu_test_pointer(const void *);
void *cu_test_address(uint32_t);
uint32_t ciuki_syscall(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
#define CU_PTR(p) cu_test_pointer(p)
#define CU_CALL(n,b,c,d,s,i,p) ciuki_syscall(CIUKI_SYS_##n,(uint32_t)(b),(uint32_t)(c),(uint32_t)(d),(uint32_t)(s),(uint32_t)(i),(uint32_t)(p))
static inline int ciuki_error(uint32_t r) { return r>=(uint32_t)-CIUKI_SYSCALL_ERROR_MAX ? -(int32_t)r : 0; }
#endif
