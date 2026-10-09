/* 64-bit division helpers required by clang on i386
 * (docs/design/execution-abi.md, "Runtime helpers"). Shift-subtract,
 * using only 32-bit operations so no helper calls itself.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdint.h>

static uint64_t udivmod64(uint64_t n, uint64_t d, uint64_t *rem)
{
    uint64_t q = 0, r = 0;
    if (d == 0) {
        /* Division by zero is a kernel bug: trap like the hardware does. */
        __asm__ volatile("ud2");
    }
    if ((uint32_t)(d >> 32) == 0 && (uint32_t)(n >> 32) == 0) {
        uint32_t nn = (uint32_t)n, dd = (uint32_t)d;
        if (rem)
            *rem = nn % dd;
        return nn / dd;
    }
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= (uint64_t)1 << i;
        }
    }
    if (rem)
        *rem = r;
    return q;
}

uint64_t __udivdi3(uint64_t n, uint64_t d) { return udivmod64(n, d, 0); }

uint64_t __umoddi3(uint64_t n, uint64_t d)
{
    uint64_t r;
    udivmod64(n, d, &r);
    return r;
}

/* Magnitudes and sign application use unsigned arithmetic, which is
 * defined for INT64_MIN. INT64_MIN / -1 cannot be represented: this helper
 * wraps to INT64_MIN, whereas the hardware IDIV would raise #DE. */
int64_t __divdi3(int64_t n, int64_t d)
{
    int neg = 0;
    uint64_t un = (uint64_t)n, ud = (uint64_t)d;
    if (n < 0) { un = (uint64_t)0 - un; neg ^= 1; }
    if (d < 0) { ud = (uint64_t)0 - ud; neg ^= 1; }
    uint64_t q = udivmod64(un, ud, 0);
    return (int64_t)(neg ? (uint64_t)0 - q : q);
}

int64_t __moddi3(int64_t n, int64_t d)
{
    int neg = n < 0;
    uint64_t un = (uint64_t)n, ud = (uint64_t)d, r;
    if (n < 0) un = (uint64_t)0 - un;
    if (d < 0) ud = (uint64_t)0 - ud;
    udivmod64(un, ud, &r);
    return (int64_t)(neg ? (uint64_t)0 - r : r);
}
