/* i386 compiler helpers; no dependence on a host CRT.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/base.h>
static uint64_t divide(uint64_t n, uint64_t d, uint64_t *rem) {
  uint64_t q = 0, r = 0;
  for (unsigned i = 64; i; i--) {
    uint64_t high = r >> 63;
    r = (r << 1) | ((n >> (i - 1)) & 1);
    if (high || r >= d) {
      r -= d;
      q |= (uint64_t)1 << (i - 1);
    }
  }
  if (rem)
    *rem = r;
  return q;
}
uint64_t W32API k32_udiv(uint64_t n, uint64_t d) { return divide(n, d, 0); }
uint64_t W32API k32_urem(uint64_t n, uint64_t d) {
  uint64_t r;
  divide(n, d, &r);
  return r;
}
