/* G1: sdk/libciuki/tz.c (native localtime, mktime, tzset, strftime tzinfo,
 * the system-zone default and ciuki_tz_env) against the host C library for
 * every bundled zone, with the time syscall answered by a model kernel.
 * SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../../sdk/libciuki/tz.c"
#include "../../../src/kernel/lib/tzrule.c"
#include <ciuki/tzdata.h>
/* From here on the plain names are the host C library (the oracle). */
#undef localtime_r
#undef localtime
#undef mktime
#undef tzset

static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

char *cu__tzname[2];
long cu__timezone;
int cu__daylight;
static struct tm reent_tm;
static struct _reent reent = {&reent_tm};
struct _reent *__getreent(void) { return &reent; }
void __tz_lock(void) {}
void __tz_unlock(void) {}
static __tzinfo_type info;
__tzinfo_type *__gettzinfo(void) { return &info; }

/* Model kernel: the system zone the GET returns. */
static char system_rule_text[64] = "CET-1CEST,M3.5.0,M10.5.0/3";
static uint32_t system_flags = CIUKI_TIME_AUTO_DST;
static int service_up = 1;
static unsigned calls;
uint32_t shim_time_call(uintptr_t record, uint32_t size)
{
    calls++;
    if (!service_up) return (uint32_t)-38;
    struct ciuki_time_zone *z = (struct ciuki_time_zone *)record;
    CHECK(size == sizeof(*z) && z->version == CIUKI_TIME_VERSION && z->size == sizeof(*z) && z->operation == CIUKI_TIME_GET);
    strcpy(z->rule, system_rule_text); z->flags = system_flags;
    return 0;
}

static void set_tz(const char *tz) { if (tz) setenv("TZ", tz, 1); else unsetenv("TZ"); tzset(); cu_tzset(); }

static void compare(const char *tz)
{
    set_tz(tz);
    for (int64_t t = 1546300800; t < 1956528000; t += 7 * 3600 + 11 * 60) {
        time_t u = (time_t)t;
        struct tm a, b;
        CHECK(localtime_r(&u, &a) && cu_localtime_r(&u, &b));
        CHECK(a.tm_year == b.tm_year && a.tm_mon == b.tm_mon && a.tm_mday == b.tm_mday && a.tm_hour == b.tm_hour &&
              a.tm_min == b.tm_min && a.tm_sec == b.tm_sec && a.tm_wday == b.tm_wday && a.tm_yday == b.tm_yday &&
              (a.tm_isdst > 0) == (b.tm_isdst > 0));
        struct tm c = b; c.tm_isdst = -1;
        time_t back = cu_mktime(&c);
        struct tm d = a; d.tm_isdst = -1;
        CHECK(back == mktime(&d) || (b.tm_isdst > 0) != (c.tm_isdst > 0));
        /* Unnormalised fields: 40 days and 30 hours past the instant. */
        c = b; c.tm_mday += 40; c.tm_hour += 30; c.tm_isdst = b.tm_isdst;
        d = a; d.tm_mday += 40; d.tm_hour += 30; d.tm_isdst = a.tm_isdst;
        CHECK(cu_mktime(&c) == mktime(&d) && c.tm_mday == d.tm_mday && c.tm_hour == d.tm_hour);
    }
    CHECK(cu__timezone == timezone && (cu__daylight != 0) == (daylight != 0));
    CHECK(!strcmp(cu__tzname[0], tzname[0]));
}

int main(void)
{
    for (unsigned z = 0; z < CIUKI_TZ_ZONE_COUNT; z++) compare(ciuki_tz_zones[z].tz);
    compare("UTC0");
    /* Rome on both 2026 change days, by hand. */
    set_tz("CET-1CEST,M3.5.0,M10.5.0/3");
    struct tm m = {.tm_year = 126, .tm_mon = 2, .tm_mday = 29, .tm_hour = 2, .tm_min = 30, .tm_isdst = -1};
    CHECK(cu_mktime(&m) == 1774747800 && m.tm_hour == 3 && m.tm_isdst == 1);   /* skipped hour */
    m = (struct tm){.tm_year = 126, .tm_mon = 9, .tm_mday = 25, .tm_hour = 2, .tm_min = 30, .tm_isdst = 0};
    CHECK(cu_mktime(&m) == 1792891800 && m.tm_hour == 2 && m.tm_isdst == 0);    /* repeated hour, CET */
    m = (struct tm){.tm_year = 126, .tm_mon = 9, .tm_mday = 25, .tm_hour = 2, .tm_min = 30, .tm_isdst = 1};
    CHECK(cu_mktime(&m) == 1792888200 && m.tm_isdst == 1);                       /* repeated hour, CEST */
    CHECK(info.__tzrule[0].offset == -3600 && info.__tzrule[1].offset == -7200);
    /* TZ unset: the system zone of the service. */
    unsetenv("TZ"); cu_ready = 0; calls = 0;
    time_t summer = 1782900000; struct tm s;           /* 2026-07-01 10:00 UTC */
    CHECK(cu_localtime_r(&summer, &s) && s.tm_hour == 12 && s.tm_isdst == 1 && calls == 1);
    char env[80];
    CHECK(!strcmp(ciuki_tz_env(env, sizeof(env)), "TZ=CET-1CEST,M3.5.0,M10.5.0/3"));
    /* Automatic adjustment off: standard time all year. */
    system_flags = 0; cu_ready = 0;
    CHECK(cu_localtime_r(&summer, &s) && s.tm_hour == 11 && s.tm_isdst == 0);
    CHECK(!strcmp(ciuki_tz_env(env, sizeof(env)), "TZ=CET-1"));
    /* A zone change reaches the next call without tzset. */
    system_flags = CIUKI_TIME_AUTO_DST; strcpy(system_rule_text, "EST5EDT,M3.2.0,M11.1.0");
    CHECK(cu_localtime_r(&summer, &s) && s.tm_hour == 6 && s.tm_isdst == 1);
    /* No service: UTC. */
    service_up = 0; cu_ready = 0;
    CHECK(cu_localtime_r(&summer, &s) && s.tm_hour == 10 && !s.tm_isdst);
    CHECK(!strcmp(ciuki_tz_env(env, sizeof(env)), "TZ=UTC0") && !strcmp(ciuki_tz_env(env, 8), "TZ=UTC0"));
    /* An explicit TZ wins over the system zone. */
    service_up = 1; setenv("TZ", "JST-9", 1);
    CHECK(cu_localtime_r(&summer, &s) && s.tm_hour == 19);
    printf("PASS libciuki tz: %u zones agree with the host C library (localtime, mktime, tzname, timezone), Rome change days, system default (%u checks)\n",
           (unsigned)CIUKI_TZ_ZONE_COUNT, checks);
    return 0;
}
