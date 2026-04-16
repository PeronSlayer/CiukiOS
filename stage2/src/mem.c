#include "mem.h"

void *mem_copy(void *dst, const void *src, u64 count) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;

    /* Fast path: 8-byte aligned copies */
    if (((u64)d & 7) == 0 && ((u64)s & 7) == 0) {
        while (count >= 8) {
            *(u64 *)d = *(const u64 *)s;
            d += 8;
            s += 8;
            count -= 8;
        }
    }

    while (count--) {
        *d++ = *s++;
    }
    return dst;
}

void *mem_set(void *dst, u8 value, u64 count) {
    u8 *d = (u8 *)dst;

    /* Build 8-byte fill pattern */
    if (((u64)d & 7) == 0 && count >= 8) {
        u64 v64 = (u64)value;
        v64 |= v64 << 8;
        v64 |= v64 << 16;
        v64 |= v64 << 32;
        while (count >= 8) {
            *(u64 *)d = v64;
            d += 8;
            count -= 8;
        }
    }

    while (count--) {
        *d++ = value;
    }
    return dst;
}

void mem_copy32(u32 *dst, const u32 *src, u64 count_u32) {
    /* Copy 2 u32s at a time via u64 when aligned */
    if (((u64)dst & 7) == 0 && ((u64)src & 7) == 0) {
        while (count_u32 >= 2) {
            *(u64 *)dst = *(const u64 *)src;
            dst += 2;
            src += 2;
            count_u32 -= 2;
        }
    }
    while (count_u32--) {
        *dst++ = *src++;
    }
}

void mem_set32(u32 *dst, u32 value, u64 count_u32) {
    /* Build 8-byte fill pattern from 4-byte value */
    u64 v64 = ((u64)value << 32) | (u64)value;

    if (((u64)dst & 7) == 0) {
        while (count_u32 >= 2) {
            *(u64 *)dst = v64;
            dst += 2;
            count_u32 -= 2;
        }
    }
    while (count_u32--) {
        *dst++ = value;
    }
}
