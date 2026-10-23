/* G1: local time for native programs. localtime, mktime, tzset and the
 * newlib tzinfo used by strftime %z/%Z follow TZ (a POSIX rule) or, when TZ
 * is unset or empty, the system time zone (POSIX leaves the default to the
 * implementation; Windows programs always see the system zone). The rule
 * is evaluated by the kernel's own tzrule.c, so libc, the kernel, FAT
 * stamps, kernel32 and the DOS VMs agree.
 * SPDX-License-Identifier: MIT */
#include <ciuki/runtime.h>
#include <ciuki/timezone.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/_tz_structs.h>
#include <time.h>
#include <ciuki/tzrule.h>                  /* evaluator: tzrule.c (own object) */

extern void __tz_lock(void), __tz_unlock(void);
static struct tzr cu_rule;
static char cu_key[TZR_TEXT + 1], cu_std[TZR_NAME], cu_dst[TZR_NAME];
static int cu_ready;

int ciuki_time_zone(struct ciuki_time_zone *z, uint32_t operation)
{
    if (!z) { errno = EINVAL; return -1; }
    z->version = CIUKI_TIME_VERSION; z->size = sizeof(*z); z->operation = operation;
    int e = ciuki_error(CU_CALL(TIME, CU_PTR(z), sizeof(*z), 0, 0, 0, 0));
    if (e) { errno = e; return -1; }
    return 0;
}

/* The system rule as TZ would carry it. */
static int system_rule(char *out, size_t cap)
{
    struct ciuki_time_zone z;
    memset(&z, 0, sizeof(z));
    int saved = errno;
    if (ciuki_time_zone(&z, CIUKI_TIME_GET)) { errno = saved; return 0; }
    z.rule[sizeof(z.rule) - 1] = 0;
    if (!(z.flags & CIUKI_TIME_AUTO_DST)) return tzr_standard_only(z.rule, out, cap) > 0;
    size_t n = strlen(z.rule);
    if (n + 1 > cap) return 0;
    memcpy(out, z.rule, n + 1);
    return 1;
}

char *ciuki_tz_env(char *out, size_t capacity)
{
    char rule[TZR_TEXT];
    if (!out || capacity < 8) return out;
    if (!system_rule(rule, sizeof(rule)) || strlen(rule) + 4 > capacity) strcpy(rule, "UTC0");
    strcpy(out, "TZ="); strcat(out, rule);
    return out;
}

/* Loads the rule; caller holds the tz lock. */
static void cu_update(void)
{
    const char *tz = getenv("TZ");
    char system[TZR_TEXT];
    if (!tz || !*tz) tz = system_rule(system, sizeof(system)) ? system : "UTC0";
    if (cu_ready && !strncmp(cu_key, tz, sizeof(cu_key) - 1)) return;
    struct tzr r;
    if (!tzr_parse(*tz == ':' ? "UTC0" : tz, &r)) tzr_utc(&r);
    cu_rule = r;
    strncpy(cu_key, tz, sizeof(cu_key) - 1); cu_key[sizeof(cu_key) - 1] = 0;
    strcpy(cu_std, r.std); strcpy(cu_dst, r.has_dst ? r.dst : r.std);
    _tzname[0] = cu_std; _tzname[1] = cu_dst;
    _timezone = -r.std_offset;
    _daylight = r.has_dst;
    __tzinfo_type *info = __gettzinfo();
    info->__tznorth = 1;
    info->__tzrule[0].offset = -r.std_offset;
    info->__tzrule[1].offset = -(r.has_dst ? r.dst_offset : r.std_offset);
    cu_ready = 1;
}

void _tzset_unlocked_r(struct _reent *r) { (void)r; cu_update(); }
void _tzset_unlocked(void) { cu_update(); }
void _tzset_r(struct _reent *r) { (void)r; __tz_lock(); cu_update(); __tz_unlock(); }
void tzset(void) { _tzset_r(__getreent()); }

struct tm *localtime_r(const time_t *t, struct tm *out)
{
    if (!t || !out) { errno = EINVAL; return 0; }
    __tz_lock(); cu_update(); struct tzr r = cu_rule; __tz_unlock();
    bool dst;
    int32_t offset = tzr_offset(&r, (int64_t)*t, &dst);
    time_t local = *t + offset;
    if (!gmtime_r(&local, out)) return 0;
    out->tm_isdst = dst;
    return out;
}
struct tm *localtime(const time_t *t) { return localtime_r(t, (struct tm *)_REENT_TM(__getreent())); }

time_t mktime(struct tm *tm)
{
    if (!tm) { errno = EINVAL; return (time_t)-1; }
    __tz_lock(); cu_update(); struct tzr r = cu_rule; __tz_unlock();
    int64_t month = tm->tm_mon, year = (int64_t)tm->tm_year + 1900 + tzr_floor_div(month, 12);
    month -= tzr_floor_div(month, 12) * 12;
    int64_t local = (tzr_days_from_civil(year, (unsigned)month + 1, 1) + tm->tm_mday - 1) * 86400 +
                    (int64_t)tm->tm_hour * 3600 + (int64_t)tm->tm_min * 60 + tm->tm_sec;
    time_t utc = (time_t)tzr_local_to_utc(&r, local, tm->tm_isdst < 0 ? -1 : tm->tm_isdst > 0, 0);
    if (!localtime_r(&utc, tm)) return (time_t)-1;
    return utc;
}
