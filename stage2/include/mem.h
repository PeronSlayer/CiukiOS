#ifndef STAGE2_MEM_H
#define STAGE2_MEM_H

#include "types.h"

void *mem_copy(void *dst, const void *src, u64 count);
void *mem_set(void *dst, u8 value, u64 count);
void  mem_copy32(u32 *dst, const u32 *src, u64 count_u32);
void  mem_set32(u32 *dst, u32 value, u64 count_u32);

#endif
