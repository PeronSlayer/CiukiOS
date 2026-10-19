/* Shift-subtract 64-bit division behind the ntdll compiler helpers; no
 * 64-bit '/' or '%' here, which would recurse into the helpers themselves.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. */
#include <stdint.h>
uint64_t w32_divmod64(uint64_t n, uint64_t d, uint32_t mode);
/* mode 0 signed quotient, 1 signed remainder, 2 unsigned quotient,
 * 3 unsigned remainder. Signed results truncate toward zero; the remainder
 * takes the dividend's sign; INT64_MIN / -1 wraps like the MSVC helper. */
uint64_t w32_divmod64(uint64_t n, uint64_t d, uint32_t mode) {
    int negative_q = 0, negative_r = 0;
    if (mode < 2) {
        if (n >> 63) {
            n = 0 - n;
            negative_q = negative_r = 1;
        }
        if (d >> 63) {
            d = 0 - d;
            negative_q = !negative_q;
        }
    }
    uint64_t q = 0, r = 0;
    if (!d)
        return 0;
    for (unsigned i = 64; i; i--) {
        uint64_t high = r >> 63;
        r = (r << 1) | ((n >> (i - 1)) & 1);
        if (high || r >= d) {
            r -= d;
            q |= (uint64_t)1 << (i - 1);
        }
    }
    if (mode == 0)
        return negative_q ? 0 - q : q;
    if (mode == 1)
        return negative_r ? 0 - r : r;
    return mode == 2 ? q : r;
}
