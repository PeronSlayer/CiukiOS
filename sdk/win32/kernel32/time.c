/* Gregorian conversion in the Windows FILETIME epoch; G1 local time and
 * time zone from the system time-zone service (syscall-97 record over the
 * Win32 transport), UTC when the service is unavailable.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
#include <w32/shims.h>
#include <ciuki/abi.h>
#include <ciuki/tzdata.h>
/* The one rule evaluator of the kernel, libciuki and the tests (MIT). */
#include "../../../src/kernel/lib/tzrule.c"
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
#define K32_UNIX_EPOCH 11644473600ll /* seconds from 1601-01-01 to 1970-01-01 */
static uint64_t ft64(const FILETIME *f) { return ((uint64_t)f->high << 32) | f->low; }
static FILETIME ftmk(uint64_t n) { return (FILETIME){(DWORD)n, (DWORD)(n >> 32)}; }
#ifdef W32_HOST_MODEL
/* Host harnesses (64-bit) answer the record directly: the wire carries a
 * 32-bit address. Absent: the service is unavailable. */
int32_t k32_zone_host(struct ciuki_w32_system_args *, struct ciuki_time_zone *) __attribute__((weak));
#endif
/* One operation on the system time-zone record; 0 with SetLastError. */
static int k32_zone(struct ciuki_time_zone *z, uint32_t op) {
  struct ciuki_w32_system_args a = {.action = W32_S_TIME_ZONE};
  a.header = K32_HEADER(W32_SYSTEM, a);
  z->version = CIUKI_TIME_VERSION;
  z->size = sizeof(*z);
  z->operation = op;
  a.low = K32_PTR(z);
  a.extra = sizeof(*z);
#ifdef W32_HOST_MODEL
  if (!k32_zone_host || !k32_status(k32_zone_host(&a, z)))
    return 0;
#else
  if (!k32_status(w32_ciuki_call(&a, sizeof(a))))
    return 0;
#endif
  if (a.error) {
    SetLastError(a.error);
    return 0;
  }
  return 1;
}
static int k32_rule(const struct ciuki_time_zone *z, struct tzr *r) {
  if (!tzr_parse(z->rule, r))
    return 0;
  if (!(z->flags & CIUKI_TIME_AUTO_DST))
    r->has_dst = 0;
  return 1;
}
/* Current bias in seconds east of UTC, 0 without the service. Microsoft
 * "File Times": FileTimeToLocalFileTime and FAT conversions use the
 * CURRENT time-zone and daylight-saving settings. */
int64_t k32_current_bias(void) {
  struct ciuki_time_zone z;
  DWORD last = GetLastError();
  if (!k32_zone(&z, CIUKI_TIME_GET)) {
    SetLastError(last);
    return 0;
  }
  return (int64_t)z.offset_minutes * 60;
}
void W32API GetLocalTime(SYSTEMTIME *s) {
  FILETIME f;
  GetSystemTimeAsFileTime(&f);
  struct ciuki_time_zone z;
  struct tzr r;
  DWORD last = GetLastError();
  if (k32_zone(&z, CIUKI_TIME_GET) && k32_rule(&z, &r)) {
    uint64_t n = ft64(&f);
    int64_t unix_seconds = (int64_t)(n / 10000000u) - K32_UNIX_EPOCH;
    f = ftmk((uint64_t)((int64_t)n + (int64_t)tzr_offset(&r, unix_seconds, 0) * 10000000));
  } else
    SetLastError(last);
  FileTimeToSystemTime(&f, s);
}
static void k32_wide(WCHAR *out, const char *in) {
  unsigned i = 0;
  for (; in && in[i] && i < 31; i++)
    out[i] = (uint8_t)in[i];
  out[i] = 0;
}
/* TIME_ZONE_INFORMATION transition: wDay is the week (5 = last), wDayOfWeek
 * the weekday, local wall time of the change (Microsoft,
 * TIME_ZONE_INFORMATION). POSIX Mm.w.d maps one to one; a time past 24:00
 * (Egypt's "M10.5.4/24") is written as 23:59:59.999, as Windows does. */
static SYSTEMTIME k32_when(const struct tzr_when *w) {
  SYSTEMTIME s = {0, 0, 0, 0, 0, 0, 0, 0};
  if (w->kind != TZR_DATE_M)
    return s;
  int32_t sec = w->seconds;
  s.month = w->month;
  s.weekday = w->wday;
  s.day = w->week;
  if (sec < 0)
    sec = 0;
  if (sec >= 86400) {
    s.hour = 23; s.minute = 59; s.second = 59; s.milliseconds = 999;
  } else {
    s.hour = (uint16_t)(sec / 3600);
    s.minute = (uint16_t)(sec / 60 % 60);
    s.second = (uint16_t)(sec % 60);
  }
  return s;
}
DWORD W32API GetTimeZoneInformation(TIME_ZONE_INFORMATION *t) {
  if (!t) {
    SetLastError(87);
    return ~0u;
  }
  w32_fill(t, 0, sizeof(*t));
  struct ciuki_time_zone z;
  struct tzr r;
  DWORD last = GetLastError();
  if (!k32_zone(&z, CIUKI_TIME_GET) || !tzr_parse(z.rule, &r)) {
    SetLastError(last);
    k32_wide(t->standard_name, "Coordinated Universal Time");
    return 0;
  }
  static volatile ULONG once;
  if (!__atomic_exchange_n(&once, 1, __ATOMIC_RELAXED))
    w32_diag("load", "kernel32.dll", z.zone[0] ? z.zone : z.rule);
  int index = ciuki_tz_find(z.zone);
  const struct ciuki_tz_zone *row = index >= 0 ? &ciuki_tz_zones[index] : 0;
  t->bias = -z.standard_minutes;
  k32_wide(t->standard_name, row ? row->standard : z.standard_name);
  k32_wide(t->daylight_name, row && row->daylight[0] ? row->daylight : z.daylight_name[0] ? z.daylight_name : (row ? row->standard : z.standard_name));
  if (!r.has_dst || !(z.flags & CIUKI_TIME_AUTO_DST))
    return 0; /* TIME_ZONE_ID_UNKNOWN: no daylight time is applied */
  t->daylight_bias = -(r.dst_offset - r.std_offset) / 60;
  t->daylight_date = k32_when(&r.start);
  t->standard_date = k32_when(&r.end);
  return (z.flags & CIUKI_TIME_DAYLIGHT) ? 2 : 1;
}
static int k32_set(const SYSTEMTIME *s, uint32_t op) {
  FILETIME f;
  if (!s || !SystemTimeToFileTime(s, &f)) {
    SetLastError(87);
    return 0;
  }
  struct ciuki_time_zone z;
  w32_fill(&z, 0, sizeof(z));
  int64_t seconds = (int64_t)(ft64(&f) / 10000000u) - K32_UNIX_EPOCH;
  if (op == CIUKI_TIME_SET_LOCAL)
    z.local_seconds = seconds;
  else
    z.utc_seconds = seconds;
  return k32_zone(&z, op);
}
/* SetLocalTime/SetSystemTime: the caller needs the time authority (the
 * SE_SYSTEMTIME_NAME equivalent); otherwise ERROR_PRIVILEGE_NOT_HELD.
 * wDayOfWeek is ignored. The kernel converts local time with the rule of
 * the new date, so the documented second SetLocalTime call is harmless. */
BOOL W32API SetLocalTime(const SYSTEMTIME *s) { return k32_set(s, CIUKI_TIME_SET_LOCAL); }
BOOL W32API SetSystemTime(const SYSTEMTIME *s) { return k32_set(s, CIUKI_TIME_SET_UTC); }
BOOL W32API FileTimeToLocalFileTime(const FILETIME *s, FILETIME *d) {
  if (!s || !d) {
    SetLastError(87);
    return 0;
  }
  *d = ftmk((uint64_t)((int64_t)ft64(s) + k32_current_bias() * 10000000));
  return 1;
}
BOOL W32API LocalFileTimeToFileTime(const FILETIME *s, FILETIME *d) {
  if (!s || !d) {
    SetLastError(87);
    return 0;
  }
  *d = ftmk((uint64_t)((int64_t)ft64(s) - k32_current_bias() * 10000000));
  return 1;
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
  int ok=k32_status(w32_ciuki_call(a, sizeof(*a)));
  if(ok)w32_shim_version(a);
  return ok;
}
DWORD W32API GetVersion(void) {
  struct ciuki_w32_identity a;
  return identity(&a) ? a.get_version : 0;
}
/* f5-15B: Microsoft "GetProcessVersion": the major (high word) and minor
 * (low word) subsystem version of the image of the given process, 0 or the
 * own identifier meaning the caller. Other processes' images are not
 * readable from a Win32 process here: ERROR_INVALID_PARAMETER, like an
 * unknown identifier. The image is mapped; its headers were validated by
 * the loader, so the optional-header fields are read directly. */
DWORD W32API GetProcessVersion(DWORD pid) {
  if (pid && pid != (DWORD)(uintptr_t)NtCurrentTeb()->ClientId.UniqueProcess) {
    SetLastError(87);
    return 0;
  }
  const uint8_t *base = NtCurrentTeb()->ProcessEnvironmentBlock->ImageBaseAddress;
  if (!base) {
    SetLastError(87);
    return 0;
  }
  uint32_t nt;
  w32_copy(&nt, base + 60, 4);
  uint16_t major, minor;
  w32_copy(&major, base + nt + 24 + 48, 2);
  w32_copy(&minor, base + nt + 24 + 50, 2);
  return ((DWORD)major << 16) | minor;
}
BOOL W32API GetVersionExA(OSVERSIONINFOA *out) {
  if (!out || (out->size != sizeof(*out)&&out->size!=sizeof(OSVERSIONINFOEXA))) {
    SetLastError(87);
    return 0;
  }
  struct ciuki_w32_identity a;
  if (!identity(&a))
    return 0;
  DWORD requested=out->size;
  OSVERSIONINFOA v = {.size = requested,
                      .major = a.major,
                      .minor = a.minor,
                      .build = a.build,
                      .platform = a.platform};
  w32_copy(v.csd, a.csd, 4);
  *out = v;
  if(requested==sizeof(OSVERSIONINFOEXA)){OSVERSIONINFOEXA *ex=(void *)out;ex->service_major=0;ex->service_minor=0;ex->suite=0;ex->product=1;ex->reserved=0;}
  return 1;
}
uint64_t W32API VerSetConditionMask(uint64_t mask,DWORD type,uint8_t condition){
  if(condition<1||condition>7)return mask;
  for(unsigned i=0;i<8;i++)if(type&(1u<<i)){mask&=~(7ull<<(3*i));mask|=(uint64_t)condition<<(3*i);}
  return mask;
}
static BOOL verify_version(DWORD major,DWORD minor,DWORD build,DWORD platform,DWORD spmajor,DWORD spminor,DWORD suite,DWORD product,DWORD type,uint64_t mask){
  if(!type||(type&~255u)){SetLastError(87);return 0;}
  struct ciuki_w32_identity a;if(!identity(&a))return 0;
  const DWORD have[8]={a.minor,a.major,a.build,a.platform,0,0,0,1};
  const DWORD want[8]={minor,major,build,platform,spminor,spmajor,suite,product};
  /* Version fields compare lexicographically, as Microsoft's hierarchy:
   * major, minor, service-pack major, service-pack minor. */
  const unsigned order[8]={1,0,5,4,2,3,6,7};int decided=0;unsigned hierarchy=0;
  for(unsigned j=0;j<8;j++){
    unsigned i=order[j];if(!(type&(1u<<i)))continue;
    unsigned op=(unsigned)(mask>>(i*3))&7;if(!op||(i!=6&&op>5)||(i==6&&op<6)){SetLastError(87);return 0;}
    if(j<4&&decided)continue;
    DWORD x=have[i],y=want[i];int ok=op==1?x==y:op==2?x>y:op==3?x>=y:op==4?x<y:op==5?x<=y:op==6?(x&y)==y:(x&y)!=0;
    if(j<4){
      if(!hierarchy&&op!=1)hierarchy=op;
      if(hierarchy){if(x==y)continue;op=hierarchy;ok=op==2||op==3?x>y:x<y;}
    }
    if(!ok){SetLastError(1150);return 0;}if(j<4&&x!=y)decided=1;
  }
  if(!decided&&(hierarchy==2||hierarchy==4)){SetLastError(1150);return 0;}
  return 1;
}
BOOL W32API VerifyVersionInfoA(OSVERSIONINFOEXA *v,DWORD type,uint64_t mask){
  if(!v||v->base.size!=sizeof(*v)){SetLastError(87);return 0;}
  return verify_version(v->base.major,v->base.minor,v->base.build,v->base.platform,v->service_major,v->service_minor,v->suite,v->product,type,mask);
}
BOOL W32API VerifyVersionInfoW(OSVERSIONINFOEXW *v,DWORD type,uint64_t mask){
  if(!v||v->base.size!=sizeof(*v)){SetLastError(87);return 0;}
  return verify_version(v->base.major,v->base.minor,v->base.build,v->base.platform,v->service_major,v->service_minor,v->suite,v->product,type,mask);
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
BOOL W32API GlobalMemoryStatusEx(MEMORYSTATUSEX *m) {
  if (!m || m->length!=sizeof(*m)) { SetLastError(87); return 0; }
  struct ciuki_w32_system_args a={.header={1,sizeof(a),W32_SYSTEM,0},.action=W32_S_MEMORY};
  if (!k32_status(w32_ciuki_call(&a,sizeof(a)))) return 0;
  if (a.error) { SetLastError(a.error); return 0; }
  uint64_t total=(uint64_t)a.low*4096,avail=(uint64_t)a.high*4096;
  uint64_t cap=(uint64_t)w32_shim_current()->memory_mib*1048576;
  if (cap && total>cap) total=cap;
  if (avail>total) avail=total;
  /* Match GetSystemInfo: inclusive maximum 0x7ffeffff, minimum 0x10000. */
  uint64_t virt=0x7ffe0000u,reserved=(uint64_t)a.extra*4096;
  /* This image has physical-backed commitment, no enabled paging file. The
   * pagefile fields describe commitment capacity, never disk free space. */
  *m=(MEMORYSTATUSEX){sizeof(*m),total ? (DWORD)((total-avail)*100/total) : 0,
      total,avail,total,avail,virt,reserved>virt ? 0 : virt-reserved,0};
  return 1;
}
static DWORD mem32(uint64_t n) { return n>0x7fffffffu ? 0x7fffffffu : (DWORD)n; }
void W32API GlobalMemoryStatus(MEMORYSTATUS *m) {
  if (!m) return;
  MEMORYSTATUSEX x={.length=sizeof(x)};
  if (!GlobalMemoryStatusEx(&x)) return;
  *m=(MEMORYSTATUS){sizeof(*m),x.load,mem32(x.total_phys),mem32(x.avail_phys),
      mem32(x.total_pagefile),mem32(x.avail_pagefile),mem32(x.total_virtual),mem32(x.avail_virtual)};
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
