#ifndef STAGE2_MEM_H
#define STAGE2_MEM_H

#include "types.h"

void *mem_copy(void *dst, const void *src, u64 count);
void *mem_set(void *dst, u8 value, u64 count);
void  mem_copy32(u32 *dst, const u32 *src, u64 count_u32);
void  mem_set32(u32 *dst, u32 value, u64 count_u32);

/* Non-temporal copy: writes to `dst` bypass the CPU cache (movnti).
 * Intended for writes to the GOP framebuffer: avoids evicting useful
 * data from L1/L2 and keeps the scan-out visually consistent because
 * the write is drained to memory as a single streaming burst. A final
 * sfence is issued to ensure ordering vs. subsequent regular stores. */
void *mem_copy_nt(void *dst, const void *src, u64 count);

#endif
