/* f7: the i386 64-bit division helpers (sdk/win32/kernel32/compiler.c,
 * __aulldiv/__aullrem; gdi_sdiv/gdi_srem build __alldiv/__allrem on them)
 * against the host compiler's division: random operands of every width
 * class, quotient boundaries and divisors of 2^32 and more.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <stdint.h>
#include <stdio.h>
#ifdef __i386__
#define API __attribute__((stdcall))
#else
#define API
#endif
uint64_t API k32_udiv(uint64_t, uint64_t);
uint64_t API k32_urem(uint64_t, uint64_t);
int64_t API gdi_sdiv(int64_t, int64_t);
int64_t API gdi_srem(int64_t, int64_t);
static uint64_t state = 88172645463325252ull;
static uint64_t next(void) {
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return state;
}
int main(void) {
  for (long i = 0; i < 4000000; i++) {
    uint64_t n = next(), d = next();
    switch (i % 6) {
    case 0: d >>= next() % 64; break;
    case 1: d &= 0xffffffffu; break;
    case 2: d >>= 32 + next() % 32; n >>= next() % 64; break;
    case 3: n >>= next() % 64; break;
    case 4: d = (d >> 31) | 1; break;
    default: n = d * (next() % 7) + next() % (d ? d : 1); break;
    }
    if (!d)
      continue;
    if (k32_udiv(n, d) != n / d || k32_urem(n, d) != n % d) {
      printf("FAIL unsigned %llu / %llu\n", (unsigned long long)n, (unsigned long long)d);
      return 1;
    }
    int64_t a = (int64_t)n >> (i % 5), b = (int64_t)d >> (i % 7);
    if (b && !(a == INT64_MIN && b == -1) &&
        (gdi_sdiv(a, b) != a / b || gdi_srem(a, b) != a % b)) {
      printf("FAIL signed %lld / %lld\n", (long long)a, (long long)b);
      return 1;
    }
  }
  static const uint64_t edge[][2] = {{~0ull, 1}, {~0ull, ~0ull}, {~0ull, 1ull << 32},
      {1ull << 63, (1ull << 32) + 1}, {12345, 0x100000000ull}, {0, 7}, {~0ull, 0xffffffffull},
      {0xfffffffeffffffffull, 0xffffffffull}, {1ull << 32, 1ull << 32}, {(1ull << 32) - 1, 1ull << 32}};
  for (unsigned i = 0; i < sizeof(edge) / sizeof(edge[0]); i++)
    if (k32_udiv(edge[i][0], edge[i][1]) != edge[i][0] / edge[i][1] ||
        k32_urem(edge[i][0], edge[i][1]) != edge[i][0] % edge[i][1]) {
      printf("FAIL edge %u\n", i);
      return 1;
    }
  /* A zero divisor keeps the former helper's answer rather than faulting. */
  if (k32_udiv(5, 0) != ~0ull || k32_urem(5, 0) != 5)
    return 1;
  puts("PASS i386 64-bit division helpers: 4M random and edge operands, signed and unsigned");
  return 0;
}
