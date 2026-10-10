/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_RUNTIME_H
#define CIUKI_RUNTIME_H
#include <ciuki/abi.h>
#include <reent.h>
#include <pthread.h>
#include <stdint.h>
uint32_t ciuki_syscall(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
#define CU_PTR(p) ((uint32_t)(uintptr_t)(p))
#define CU_CALL(n,b,c,d,s,i,p) ciuki_syscall(CIUKI_SYS_##n,(uint32_t)(b),(uint32_t)(c),(uint32_t)(d),(uint32_t)(s),(uint32_t)(i),(uint32_t)(p))
static inline int ciuki_error(uint32_t r) {
    return r >= (uint32_t)-CIUKI_SYSCALL_ERROR_MAX ? -(int32_t)r : 0;
}
static inline struct ciuki_tcb *ciuki_tcb(void) {
    struct ciuki_tcb *t; __asm__ volatile("movl %%gs:%c1,%0":"=r"(t):"i"(__builtin_offsetof(struct ciuki_tcb,self))); return t;
}
uint32_t ciuki_heap_high_water(void);
void __ciuki_pthread_main(void);
void __ciuki_sigreturn(void);
#endif
