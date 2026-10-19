/* SPDX-License-Identifier: MIT */
#include <ciuki/abi.h>
#include <stdint.h>
uint32_t ciuki_syscall(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
uint32_t desk_w32_record(void *record,uint32_t bytes)
{return ciuki_syscall(CIUKI_SYS_W32,(uint32_t)(uintptr_t)record,bytes,0,0,0,0);}
