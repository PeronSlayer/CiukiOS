/* G1: the Win32 and DOS views of the local clock agree with the system
 * zone. Production kernel32 time.c (GetLocalTime, GetSystemTime,
 * GetTimeZoneInformation, SetLocalTime, SetSystemTime,
 * FileTimeToLocalFileTime, LocalFileTimeToFileTime) against a model of the
 * kernel record (proc/timezone.c semantics, tested in localtime_test.c),
 * and the DOS VM's INT 1Ah/CMOS view (vm/dosvm_rtc.h) of the same instant.
 * Europe/Rome around both 2026 changes, New York, Rome without automatic
 * adjustment.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <w32/kernel32.h>
#include <w32/shims.h>
#include <ciuki/tzrule.h>
#include <ciuki/tzdata.h>
#include "../../../src/kernel/vm/dosvm_rtc.h"
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "views %u: %s\n", __LINE__, #c); exit(1); } } while (0)
static unsigned checks;
static DWORD last;
static struct ciuki_w32_compat shims;
const struct ciuki_w32_compat *w32_shim_current(void) { return &shims; }
static struct w32_teb teb;
_Thread_local struct w32_teb *w32_model_teb = &teb;
DWORD W32API GetLastError(void) { return last; }
void W32API SetLastError(DWORD n) { last = n; }
int k32_status(int32_t s) { return s == 0; }
void *w32_copy(void *a, const void *b, ULONG n) { return memmove(a, b, n); }
void *w32_fill(void *a, uint8_t b, ULONG n) { return memset(a, b, n); }
ULONG w32_strlen(const char *s) { return s ? (ULONG)strlen(s) : 0; }
void w32_diag(const char *e, const char *m, const char *n) { (void)e; (void)m; (void)n; }
void w32_shim_version(struct ciuki_w32_identity *a) { (void)a; }

/* Model kernel. */
static int64_t model_utc;
static char model_zone[48] = "Europe/Rome", model_rule[64] = "CET-1CEST,M3.5.0,M10.5.0/3";
static uint32_t model_flags = CIUKI_TIME_AUTO_DST | CIUKI_TIME_RTC_LOCAL;
static int authority, service = 1;
static uint32_t last_op;
static int64_t last_value;
#define EPOCH 11644473600ll
int32_t w32_ciuki_call(void *record, uint32_t n) {
  CHECK(n == sizeof(struct ciuki_w32_system_args));
  struct ciuki_w32_system_args *a = record;
  CHECK(a->action == W32_S_TIME);
  uint64_t f = (uint64_t)(model_utc + EPOCH) * 10000000u;
  a->low = (uint32_t)f; a->high = (uint32_t)(f >> 32);
  return 0;
}
int32_t k32_zone_host(struct ciuki_w32_system_args *a, struct ciuki_time_zone *z) {
  CHECK(a->action == W32_S_TIME_ZONE && a->extra == sizeof(*z) && z->version == 1 && z->size == sizeof(*z));
  if (!service) return -38;
  last_op = z->operation;
  if (z->operation != CIUKI_TIME_GET) {
    if (!authority) { a->error = 1314; return 0; }
    last_value = z->operation == CIUKI_TIME_SET_LOCAL ? z->local_seconds : z->utc_seconds;
  }
  struct tzr r; CHECK(tzr_parse(model_rule, &r));
  struct tzr full = r;
  if (!(model_flags & CIUKI_TIME_AUTO_DST)) r.has_dst = 0;
  bool dst; int32_t off = tzr_offset(&r, model_utc, &dst);
  memset(z, 0, sizeof(*z));
  z->version = 1; z->size = sizeof(*z); z->flags = model_flags | (dst ? CIUKI_TIME_DAYLIGHT : 0);
  z->utc_seconds = model_utc; z->local_seconds = model_utc + off; z->offset_minutes = off / 60;
  z->standard_minutes = full.std_offset / 60; z->daylight_minutes = (r.has_dst ? r.dst_offset : r.std_offset) / 60;
  strcpy(z->zone, model_zone); strcpy(z->rule, model_rule); strcpy(z->standard_name, full.std); strcpy(z->daylight_name, full.dst);
  return 0;
}
extern void W32API GetLocalTime(SYSTEMTIME *);
extern DWORD W32API GetTimeZoneInformation(TIME_ZONE_INFORMATION *);
extern BOOL W32API SetLocalTime(const SYSTEMTIME *);
extern BOOL W32API SetSystemTime(const SYSTEMTIME *);
extern BOOL W32API FileTimeToLocalFileTime(const FILETIME *, FILETIME *);
extern BOOL W32API LocalFileTimeToFileTime(const FILETIME *, FILETIME *);
extern BOOL W32API DosDateTimeToFileTime(uint16_t, uint16_t, FILETIME *);
extern BOOL W32API FileTimeToDosDateTime(const FILETIME *, uint16_t *, uint16_t *);
static int64_t civil(int y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned s)
{ return tzr_days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s; }
static unsigned unbcd(unsigned b) { return (b >> 4) * 10 + (b & 15); }

/* Every view of model_utc shows the same local wall clock. */
static void agree(int64_t expected_local)
{
  SYSTEMTIME w, u;
  GetLocalTime(&w); GetSystemTime(&u);
  int64_t y; unsigned m, d;
  tzr_civil_from_days(tzr_floor_div(expected_local, 86400), &y, &m, &d);
  int64_t tod = expected_local - tzr_floor_div(expected_local, 86400) * 86400;
  CHECK(w.year == y && w.month == m && w.day == d && w.hour == tod / 3600 && w.minute == tod / 60 % 60 && w.second == tod % 60);
  CHECK(w.weekday == tzr_weekday(tzr_floor_div(expected_local, 86400)));
  int64_t utc_tod = model_utc - tzr_floor_div(model_utc, 86400) * 86400;
  CHECK(u.hour == utc_tod / 3600 && u.minute == utc_tod / 60 % 60);
  /* DOS: INT 1Ah AH=02h/04h, as CiukiDOS CLOCK$ reads them for INT 21h 2Ah/2Ch. */
  uint16_t cx, dx;
  dosvm_rtc_time(expected_local, &cx, &dx);
  CHECK(unbcd(cx >> 8) == w.hour && unbcd(cx & 255) == w.minute && unbcd(dx >> 8) == w.second);
  dosvm_rtc_date(expected_local, &cx, &dx);
  CHECK(unbcd(cx >> 8) * 100 + unbcd(cx & 255) == w.year && unbcd(dx >> 8) == w.month && unbcd(dx & 255) == w.day);
  uint8_t b;
  CHECK(dosvm_rtc_cmos(expected_local, 4, &b) && unbcd(b) == w.hour && dosvm_rtc_cmos(expected_local, 0x32, &b) && unbcd(b) == w.year / 100);
  CHECK(dosvm_rtc_cmos(expected_local, 6, &b) && b == w.weekday + 1);
  /* BIOS ticks since midnight start at the same time of day. */
  CHECK(dosvm_rtc_ticks(expected_local) == (uint32_t)((uint64_t)tod * 0x1800B0u / 86400u) && dosvm_rtc_ticks(expected_local) < 0x1800B0u);
  /* FAT round trip with the current bias: local stamp -> UTC -> local. */
  FILETIME fat, utcf, back;
  uint16_t date = (uint16_t)((w.year - 1980) << 9 | w.month << 5 | w.day), time = (uint16_t)(w.hour << 11 | w.minute << 5 | w.second / 2);
  CHECK(DosDateTimeToFileTime(date, time, &fat) && LocalFileTimeToFileTime(&fat, &utcf) && FileTimeToLocalFileTime(&utcf, &back));
  uint16_t d2, t2; CHECK(FileTimeToDosDateTime(&back, &d2, &t2) && d2 == date && t2 == time);
  uint64_t us = ((uint64_t)utcf.high << 32 | utcf.low) / 10000000u - EPOCH;
  CHECK((int64_t)us == expected_local - (tod % 2) - (expected_local - model_utc));
}

int main(void)
{
  /* Rome, winter, both change days, summer. */
  static const struct { int64_t utc, local; DWORD id; } rome[] = {
    {1768478400, 1768482000, 1},                     /* 2026-01-15 12:00 UTC -> 13:00 CET */
    {1774745999, 1774749599, 1},                     /* 2026-03-29 00:59:59 UTC -> 01:59:59 CET */
    {1774746000, 1774753200, 2},                     /* 01:00:00 UTC -> 03:00:00 CEST */
    {1782907200, 1782914400, 2},                     /* 2026-07-01 12:00 UTC -> 14:00 CEST */
    {1792889999, 1792897199, 2},                     /* 2026-10-25 00:59:59 UTC -> 02:59:59 CEST */
    {1792890000, 1792893600, 1},                     /* 01:00:00 UTC -> 02:00:00 CET */
  };
  for (unsigned i = 0; i < sizeof(rome) / sizeof(rome[0]); i++) {
    model_utc = rome[i].utc;
    CHECK(civil(2026, 1, 1, 0, 0, 0) < model_utc);
    agree(rome[i].local);
    TIME_ZONE_INFORMATION z;
    CHECK(GetTimeZoneInformation(&z) == rome[i].id);
    CHECK(z.bias == -60 && z.standard_bias == 0 && z.daylight_bias == -60);
    CHECK(z.daylight_date.month == 3 && z.daylight_date.day == 5 && z.daylight_date.weekday == 0 && z.daylight_date.hour == 2);
    CHECK(z.standard_date.month == 10 && z.standard_date.day == 5 && z.standard_date.weekday == 0 && z.standard_date.hour == 3);
    char name[32]; for (unsigned k = 0; k < 32; k++) name[k] = (char)z.standard_name[k];
    CHECK(!strcmp(name, "W. Europe Standard Time"));
    for (unsigned k = 0; k < 32; k++) name[k] = (char)z.daylight_name[k];
    CHECK(!strcmp(name, "W. Europe Daylight Time"));
    CHECK(k32_current_bias() == (rome[i].local - rome[i].utc));
  }
  /* Every bundled zone, hourly over 2026: Win32 local equals the rule. */
  for (unsigned zi = 0; zi < CIUKI_TZ_ZONE_COUNT; zi++) {
    strcpy(model_zone, ciuki_tz_zones[zi].id); strcpy(model_rule, ciuki_tz_zones[zi].tz);
    struct tzr r; CHECK(tzr_parse(model_rule, &r));
    for (model_utc = civil(2026, 1, 1, 0, 0, 0); model_utc < civil(2027, 1, 1, 0, 0, 0); model_utc += 3600 * 7 + 60)
      agree(model_utc + tzr_offset(&r, model_utc, 0));
    TIME_ZONE_INFORMATION z;
    DWORD id = GetTimeZoneInformation(&z);
    CHECK(z.bias == -ciuki_tz_zones[zi].minutes && (id != 0) == r.has_dst);
  }
  /* Rome without automatic daylight-saving adjustment. */
  strcpy(model_zone, "Europe/Rome"); strcpy(model_rule, "CET-1CEST,M3.5.0,M10.5.0/3");
  model_flags = CIUKI_TIME_RTC_LOCAL; model_utc = 1782907200;
  agree(1782907200 + 3600);
  TIME_ZONE_INFORMATION z;
  CHECK(GetTimeZoneInformation(&z) == 0 && !z.daylight_date.month && !z.standard_date.month && !z.daylight_bias);
  model_flags = CIUKI_TIME_AUTO_DST | CIUKI_TIME_RTC_LOCAL;
  /* SetLocalTime / SetSystemTime: privilege, local seconds, weekday ignored. */
  SYSTEMTIME s = {2026, 12, 3, 24, 18, 30, 0, 0};
  authority = 0; last = 0;
  CHECK(!SetLocalTime(&s) && GetLastError() == 1314);
  authority = 1;
  CHECK(SetLocalTime(&s) && last_op == CIUKI_TIME_SET_LOCAL && last_value == civil(2026, 12, 24, 18, 30, 0));
  CHECK(SetSystemTime(&s) && last_op == CIUKI_TIME_SET_UTC && last_value == civil(2026, 12, 24, 18, 30, 0));
  s.month = 13; CHECK(!SetLocalTime(&s) && GetLastError() == 87);
  CHECK(!SetLocalTime(0) && GetLastError() == 87);
  /* DOS TIME/DATE (INT 1Ah AH=03h/05h): the VM offset moves its clock only. */
  int64_t local = civil(2026, 10, 25, 2, 30, 0), set;
  CHECK(dosvm_rtc_set_time(local, 0x2359, 0x5800, &set) && set == civil(2026, 10, 25, 23, 59, 58));
  CHECK(dosvm_rtc_set_date(local, 0x2027, 0x0229, &set) == false);              /* 2027-02-29 */
  CHECK(dosvm_rtc_set_date(local, 0x2028, 0x0229, &set) && set == civil(2028, 2, 29, 2, 30, 0));
  CHECK(!dosvm_rtc_set_time(local, 0x2400, 0, &set) && !dosvm_rtc_set_time(local, 0x125A, 0, &set));
  CHECK(!dosvm_rtc_set_date(local, 0x1979, 0x1231, &set) && !dosvm_rtc_set_date(local, 0x2026, 0x1300, &set));
  uint8_t reg; CHECK(dosvm_rtc_cmos(local, 0x0B, &reg) && reg == 2 && dosvm_rtc_cmos(local, 0x0D, &reg) && reg == 0x80);
  CHECK(!dosvm_rtc_cmos(local, 0x10, &reg) && !dosvm_rtc_cmos(local, 0x0E, &reg));
  /* No service: UTC, as before G1. */
  service = 0; model_utc = 1782907200;
  SYSTEMTIME w; GetLocalTime(&w); CHECK(w.hour == 12);
  CHECK(GetTimeZoneInformation(&z) == 0 && z.bias == 0 && z.standard_name[0] == 'C');
  CHECK(k32_current_bias() == 0);
  printf("PASS local time views: Win32 GetLocalTime/GetTimeZoneInformation/SetLocalTime and DOS INT 1Ah/CMOS agree, %u zones (%u checks)\n",
         (unsigned)CIUKI_TZ_ZONE_COUNT, checks);
  return 0;
}
