/* i386 compiler helpers; no dependence on a host CRT.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/base.h>
/* f7: __aulldiv/__aullrem (and, through gdi_sdiv, __alldiv/__allrem) are on
 * the per-pixel paths of the software OpenGL renderer and of GDI blits; the
 * former shift-and-subtract loop (64 iterations) took 81% of the CPU time of
 * Quake III Arena's main menu on the H1 host run. Hardware DIV, as the
 * i386 runtime helpers do (Hacker's Delight, 2nd ed., 9-5 "Unsigned
 * Doubleword Division from Long Division"): one DIVL when the high half of
 * the dividend is below the 32-bit divisor, two otherwise; a divisor of 2^32
 * or more takes one DIVL on the normalized operands and one correction.
 * A zero divisor keeps the former result (all ones, remainder n) instead of
 * a divide fault. */
static inline uint32_t divl(uint32_t high, uint32_t low, uint32_t d, uint32_t *rem) {
  uint32_t q, r; /* requires high < d */
  __asm__("divl %4" : "=a"(q), "=d"(r) : "a"(low), "d"(high), "rm"(d) : "cc");
  *rem = r;
  return q;
}
static uint64_t divide(uint64_t n, uint64_t d, uint64_t *rem) {
  uint32_t dh = (uint32_t)(d >> 32), dl = (uint32_t)d, nh = (uint32_t)(n >> 32), nl = (uint32_t)n, r;
  uint64_t q;
  if (!d) {
    q = ~(uint64_t)0;
    r = 0;
    if (rem)
      *rem = n;
    return q;
  }
  if (!dh) {
    if (nh < dl)
      q = divl(nh, nl, dl, &r);
    else {
      uint32_t qh = divl(0, nh, dl, &r);
      q = (uint64_t)qh << 32 | divl(r, nl, dl, &r);
    }
    if (rem)
      *rem = r;
    return q;
  }
  unsigned s = (unsigned)__builtin_clz(dh);
  uint32_t v1 = (uint32_t)((d << s) >> 32);
  uint64_t u1 = n >> 1;
  uint32_t q1 = divl((uint32_t)(u1 >> 32), (uint32_t)u1, v1, &r);
  q = ((uint64_t)q1 << s) >> 31;
  if (q)
    q--;
  uint64_t left = n - q * d;
  if (left >= d) {
    q++;
    left -= d;
  }
  if (rem)
    *rem = left;
  return q;
}
uint64_t W32API k32_udiv(uint64_t n, uint64_t d) { return divide(n, d, 0); }
uint64_t W32API k32_urem(uint64_t n, uint64_t d) {
  uint64_t r;
  divide(n, d, &r);
  return r;
}
