/* Gregorian conversion in the Windows FILETIME epoch. UTC fallback.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
static int leap(unsigned y) { return !(y % 4) && (y % 100 || !(y % 400)); }
static unsigned days(unsigned y, unsigned m) {
  static const uint8_t d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return d[m - 1] + (m == 2 && leap(y));
}
static uint64_t before(unsigned y) {
  uint64_t n = y - 1;
  return n * 365 + n / 4 - n / 100 + n / 400;
}
BOOL W32API SystemTimeToFileTime(const SYSTEMTIME *s, FILETIME *out) {
  if (!s || !out || s->year < 1601 || s->year > 30827 || s->month < 1 ||
      s->month > 12 || s->day < 1 || s->day > days(s->year, s->month) ||
      s->hour > 23 || s->minute > 59 || s->second > 59 ||
      s->milliseconds > 999) {
    SetLastError(87);
    return 0;
  }
  uint64_t d = before(s->year) - before(1601) + s->day - 1;
  for (unsigned m = 1; m < s->month; m++)
    d += days(s->year, m);
  uint64_t n =
      (((d * 24 + s->hour) * 60 + s->minute) * 60 + s->second) * 10000000 +
      s->milliseconds * 10000u;
  *out = (FILETIME){(DWORD)n, (DWORD)(n >> 32)};
  return 1;
}
BOOL W32API FileTimeToSystemTime(const FILETIME *f, SYSTEMTIME *out) {
  if (!f || !out || f->high & 0x80000000u) {
    SetLastError(87);
    return 0;
  }
  uint64_t n = ((uint64_t)f->high << 32) | f->low;
  uint64_t d = n / 864000000000ull;
  uint64_t rem = n % 864000000000ull;
  unsigned y = 1601;
  uint64_t left = d;
  /* At most 400 iterations after skipping complete Gregorian cycles. */
  y += (unsigned)(left / 146097) * 400;
  left %= 146097;
  while (left >= (unsigned)(365 + leap(y))) {
    left -= 365 + leap(y);
    y++;
  }
  unsigned m = 1;
  while (left >= days(y, m)) {
    left -= days(y, m);
    m++;
  }
  SYSTEMTIME s = {.year = (uint16_t)y,
                  .month = (uint16_t)m,
                  .day = (uint16_t)(left + 1),
                  .weekday = (uint16_t)((d + 1) % 7)};
  s.hour = (uint16_t)(rem / 36000000000ull);
  rem %= 36000000000ull;
  s.minute = (uint16_t)(rem / 600000000ull);
  rem %= 600000000ull;
  s.second = (uint16_t)(rem / 10000000);
  s.milliseconds = (uint16_t)((rem % 10000000) / 10000);
  *out = s;
  return 1;
}
LONG W32API CompareFileTime(const FILETIME *a, const FILETIME *b) {
  uint64_t x = ((uint64_t)a->high << 32) | a->low,
           y = ((uint64_t)b->high << 32) | b->low;
  return x < y ? -1 : x > y ? 1 : 0;
}
static int counter(struct ciuki_w32_counter *a) {
  w32_fill(a, 0, sizeof(*a));
  a->header = K32_HEADER(W32_COUNTER, *a);
  return k32_status(w32_ciuki_call(a, sizeof(*a)));
}
DWORD W32API GetTickCount(void) {
  struct ciuki_w32_counter a;
  if (!counter(&a))
    return 0;
  uint64_t n = ((uint64_t)a.value_hi << 32) | a.value_lo;
  return (DWORD)(n * 1000 / a.frequency_lo);
}
BOOL W32API QueryPerformanceCounter(int64_t *p) {
  if (!p) {
    SetLastError(87);
    return 0;
  }
  struct ciuki_w32_counter a;
  if (!counter(&a))
    return 0;
  *p = (int64_t)(((uint64_t)a.value_hi << 32) | a.value_lo);
  return 1;
}
BOOL W32API QueryPerformanceFrequency(int64_t *p) {
  if (!p) {
    SetLastError(87);
    return 0;
  }
  struct ciuki_w32_counter a;
  if (!counter(&a))
    return 0;
  *p = a.frequency_lo;
  return 1;
}
void W32API GetSystemTimeAsFileTime(FILETIME *f) {
  if (!f)
    return;
  struct ciuki_w32_system_args a = {.action = W32_S_TIME};
  a.header = K32_HEADER(W32_SYSTEM, a);
  if (k32_status(w32_ciuki_call(&a, sizeof(a))))
    *f = (FILETIME){a.low, a.high};
}
void W32API GetSystemTime(SYSTEMTIME *s) {
  FILETIME f;
  GetSystemTimeAsFileTime(&f);
  FileTimeToSystemTime(&f, s);
}
void W32API GetLocalTime(SYSTEMTIME *s) { GetSystemTime(s); }
DWORD W32API GetTimeZoneInformation(TIME_ZONE_INFORMATION *t) {
  if (!t) {
    SetLastError(87);
    return ~0u;
  }
  w32_fill(t, 0, sizeof(*t));
  t->standard_name[0] = 'U';
  t->standard_name[1] = 'T';
  t->standard_name[2] = 'C';
  return 0;
}
BOOL W32API FileTimeToLocalFileTime(const FILETIME *s, FILETIME *d) {
  if (!s || !d) {
    SetLastError(87);
    return 0;
  }
  *d = *s;
  return 1;
}
BOOL W32API LocalFileTimeToFileTime(const FILETIME *s, FILETIME *d) {
  return FileTimeToLocalFileTime(s, d);
}
BOOL W32API DosDateTimeToFileTime(uint16_t date, uint16_t time, FILETIME *out) {
  SYSTEMTIME s = {.year = (uint16_t)(1980 + (date >> 9)),
                  .month = (date >> 5) & 15,
                  .day = date & 31,
                  .hour = time >> 11,
                  .minute = (time >> 5) & 63,
                  .second = (uint16_t)((time & 31) * 2)};
  return SystemTimeToFileTime(&s, out);
}
BOOL W32API FileTimeToDosDateTime(const FILETIME *f, uint16_t *date,
                                  uint16_t *time) {
  SYSTEMTIME s;
  if (!date || !time || !FileTimeToSystemTime(f, &s))
    return 0;
  if (s.year < 1980 || s.year > 2107) {
    SetLastError(87);
    return 0;
  }
  *date = (uint16_t)(((s.year - 1980) << 9) | (s.month << 5) | s.day);
  *time = (uint16_t)((s.hour << 11) | (s.minute << 5) | (s.second / 2));
  return 1;
}
static int identity(struct ciuki_w32_identity *a) {
  w32_fill(a, 0, sizeof(*a));
  a->header = K32_HEADER(W32_IDENTITY, *a);
  return k32_status(w32_ciuki_call(a, sizeof(*a)));
}
DWORD W32API GetVersion(void) {
  struct ciuki_w32_identity a;
  return identity(&a) ? a.get_version : 0;
}
BOOL W32API GetVersionExA(OSVERSIONINFOA *out) {
  if (!out || out->size != sizeof(*out)) {
    SetLastError(87);
    return 0;
  }
  struct ciuki_w32_identity a;
  if (!identity(&a))
    return 0;
  OSVERSIONINFOA v = {.size = sizeof(v),
                      .major = a.major,
                      .minor = a.minor,
                      .build = a.build,
                      .platform = a.platform};
  w32_copy(v.csd, a.csd, 4);
  *out = v;
  return 1;
}
BOOL W32API GetVersionExW(OSVERSIONINFOW *out) {
  if (!out || out->size != sizeof(*out)) {
    SetLastError(87);
    return 0;
  }
  struct ciuki_w32_identity a;
  if (!identity(&a))
    return 0;
  OSVERSIONINFOW v = {.size = sizeof(v),
                      .major = a.major,
                      .minor = a.minor,
                      .build = a.build,
                      .platform = a.platform};
  for (unsigned i = 0; i < 4; i++)
    v.csd[i] = a.csd[i];
  *out = v;
  return 1;
}
static DWORD cpu_signature(DWORD *features) {
  DWORD a, b, c, d;
  __asm__ volatile("cpuid"
                   : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                   : "a"(1), "c"(0));
  if (features)
    *features = d;
  return a;
}
BOOL W32API IsProcessorFeaturePresent(DWORD feature) {
  DWORD d;
  cpu_signature(&d);
  switch (feature) {
  case 2:
    return !!(d & (1u << 8));
  case 3:
    return !!(d & (1u << 23));
  case 6:
    return !!(d & (1u << 25));
  case 8:
    return !!(d & (1u << 4));
  default:
    return 0;
  }
}
void W32API GetSystemInfo(SYSTEM_INFO *s) {
  if (!s)
    return;
  SYSTEM_INFO v = {.page_size = 4096,
                   .minimum = (void *)0x10000,
                   .maximum = (void *)0x7ffeffff,
                   .active_mask = 1,
                   .processors = 1,
                   .type = 586,
                   .granularity = 65536,
                   .level = 6};
  DWORD cpu = cpu_signature(0);
  v.level = (uint16_t)((cpu >> 8) & 15);
  if (v.level == 15)
    v.level += (cpu >> 20) & 255;
  v.revision =
      (uint16_t)((((cpu >> 4) & 15) | ((cpu >> 12) & 0xf0)) << 8) | (cpu & 15);
  *s = v;
}
void W32API GlobalMemoryStatus(MEMORYSTATUS *m) {
  if (!m)
    return;
  struct ciuki_w32_system_args a = {.action = W32_S_MEMORY};
  a.header = K32_HEADER(W32_SYSTEM, a);
  if (!k32_status(w32_ciuki_call(&a, sizeof(a))))
    return;
  uint64_t total = (uint64_t)a.low * 4096, avail = (uint64_t)a.high * 4096;
  DWORD t = total > 0x7fffffff ? 0x7fffffff : (DWORD)total,
        f = avail > 0x7fffffff ? 0x7fffffff : (DWORD)avail;
  *m = (MEMORYSTATUS){
      sizeof(*m), total ? (DWORD)((total - avail) * 100 / total) : 0,
      t,          f,
      t,          f,
      0x7fffffff, a.extra > 524287 ? 0 : 0x7fffffff - a.extra * 4096};
}
BOOL W32API GetComputerNameA(char *out, DWORD *n) {
  if (!n || !out) {
    SetLastError(87);
    return 0;
  }
  if (*n < 8) {
    *n = 8;
    SetLastError(111);
    return 0;
  }
  w32_copy(out, "CIUKIOS", 8);
  *n = 7;
  return 1;
}
