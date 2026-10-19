/* Signed i386 arithmetic helpers. SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/base.h>
extern uint64_t W32API k32_udiv(uint64_t, uint64_t);
int64_t W32API gdi_sdiv(int64_t n, int64_t d) {
  uint64_t a = (uint64_t)n, b = (uint64_t)d;
  if (n < 0)
    a = 0 - a;
  if (d < 0)
    b = 0 - b;
  uint64_t q = k32_udiv(a, b);
  return (int64_t)((n < 0) != (d < 0) ? 0 - q : q);
}
int64_t W32API gdi_srem(int64_t n, int64_t d) { return n - gdi_sdiv(n, d) * d; }
