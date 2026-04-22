#include "mem.h"

void *mem_copy(void *dst, const void *src, u64 count) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;

    /* Fast path: use `rep movsq` when both pointers are 8-byte aligned.
     * On modern x86-64 CPUs with ERMSB/FSRM this is microcoded to a
     * cache-line-granular copy — the fastest non-SSE memcpy available
     * and the least likely to produce visible tearing in framebuffer
     * writes (each movsq is atomic w.r.t. a single cache line). */
    if (((u64)d & 7) == 0 && ((u64)s & 7) == 0 && count >= 64) {
        u64 qwords = count >> 3;
        u64 tail   = count & 7U;
        __asm__ volatile (
            "rep movsq\n\t"
            : "+D"(d), "+S"(s), "+c"(qwords)
            :
            : "memory"
        );
        count = tail;
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

    /* Fast path: use `rep stosq` when aligned and enough qwords.
     * This is by far the fastest fill on x86-64 (ERMSB/FSRM). */
    if (((u64)dst & 7) == 0 && count_u32 >= 16) {
        u64 qwords = count_u32 >> 1;    /* pairs of u32 per qword */
        u64 *dq = (u64 *)dst;
        __asm__ volatile (
            "rep stosq\n\t"
            : "+D"(dq), "+c"(qwords)
            : "a"(v64)
            : "memory"
        );
        dst = (u32 *)dq;
        count_u32 &= 1U;                /* leftover single u32 */
    }

    while (count_u32--) {
        *dst++ = value;
    }
}

void *mem_copy_nt(void *dst, const void *src, u64 count) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;

    /* Fast path: 8-byte aligned streaming copy using movnti. */
    if (((u64)d & 7) == 0 && ((u64)s & 7) == 0 && count >= 64) {
        u64 qwords = count >> 3;
        u64 tail   = count & 7U;
        u64 tmp;
        while (qwords--) {
            tmp = *(const u64 *)s;
            __asm__ volatile (
                "movnti %1, (%0)\n\t"
                :
                : "r"(d), "r"(tmp)
                : "memory"
            );
            d += 8;
            s += 8;
        }
        /* Drain write-combining buffers before any subsequent stores. */
        __asm__ volatile ("sfence" ::: "memory");
        count = tail;
    }

    while (count--) {
        *d++ = *s++;
    }
    return dst;
}
