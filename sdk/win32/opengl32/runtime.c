/* Small freestanding CRT boundary; no imported commercial runtime.
 * Scalar x87 is the existing user-process ABI. SPDX-License-Identifier: MIT */
#ifndef W32_HOST_MODEL
#include <stddef.h>
#include <w32/services.h>
extern void *gl_alloc(unsigned);
extern void gl_dealloc(void *);
void *malloc(size_t n) { return n <= 0x3fffffff ? gl_alloc((unsigned)n) : 0; }
void *calloc(size_t n, size_t size) {
  return size && n > 0x3fffffff / size ? 0 : malloc(n * size);
}
void free(void *p) { gl_dealloc(p); }
void *memcpy(void *d, const void *s, size_t n) {
  return w32_copy(d, s, (uint32_t)n);
}
void *memset(void *d, int c, size_t n) { return w32_fill(d, c, (uint32_t)n); }
void *memmove(void *d, const void *s, size_t n) {
  uint8_t *a = d;
  const uint8_t *b = s;
  if (a > b)
    for (size_t i = n; i; i--)
      a[i - 1] = b[i - 1];
  else
    for (size_t i = 0; i < n; i++)
      a[i] = b[i];
  return d;
}
int memcmp(const void *a, const void *b, size_t n) {
  const uint8_t *p = a, *q = b;
  for (size_t i = 0; i < n; i++)
    if (p[i] != q[i])
      return p[i] < q[i] ? -1 : 1;
  return 0;
}
size_t strlen(const char *s) { return w32_strlen(s); }
__attribute__((noreturn)) void exit(int status) {
  const struct w32_services *s = w32_get_services();
  s->terminate(s->opaque, (uint32_t)status);
  __builtin_unreachable();
}
int _fltused = 1;
double fabs(double x) { return x < 0 ? -x : x; }
double floor(double x) {
  if (x > 2147483646 || x < -2147483646)
    return x;
  int i = (int)x;
  return i > x ? i - 1 : i;
}
double sqrt(double x) {
  double r;
  __asm__("fsqrt" : "=t"(r) : "0"(x));
  return r;
}
double sin(double x) {
  if (x > 1e9 || x < -1e9)
    return 0;
  x -= floor((x + 3.141592653589793) / 6.283185307179586) * 6.283185307179586;
  double y = x * x;
  return x * (1 + y * (-1.0 / 6 +
                       y * (1.0 / 120 +
                            y * (-1.0 / 5040 + y * (1.0 / 362880 +
                                                    y * (-1.0 / 39916800 +
                                                         y / 6227020800.0))))));
}
double cos(double x) { return sin(x + 1.5707963267948966); }
double exp(double x) {
  if (x < -64)
    return 0;
  if (x > 64)
    x = 64;
  int n = (int)floor(x / 0.6931471805599453);
  double r = x - n * 0.6931471805599453, t = 1, v = 1;
  for (int i = 1; i <= 14; i++) {
    t *= r / i;
    v += t;
  }
  while (n > 0) {
    v *= 2;
    n--;
  }
  while (n < 0) {
    v *= 0.5;
    n++;
  }
  return v;
}
double pow(double x, double y) {
  if (x <= 0)
    return y == 0 ? 1 : 0;
  int n = 0;
  while (x >= 2 && n < 1024) {
    x *= 0.5;
    n++;
  }
  while (x < 1 && n > -1024) {
    x *= 2;
    n--;
  }
  double z = (x - 1) / (x + 1), z2 = z * z, t = z, sum = 0;
  for (int i = 1; i < 32; i += 2) {
    sum += t / i;
    t *= z2;
  }
  return exp((2 * sum + n * 0.6931471805599453) * y);
}
void tgl_warning(const char *s, ...) { (void)s; }
void tgl_trace(const char *s, ...) { (void)s; }
void tgl_fixme(const char *s, ...) { (void)s; }
void gl_fatal_error(char *s, ...) {
  (void)s;
  exit(8);
}
#endif
