/* G1: POSIX TZ rules, civil calendar and the clock.cfg codec. Pure code with
 * no library calls, shared by the kernel (src/kernel/lib/tzrule.c), libciuki
 * (localtime/mktime), kernel32 and the host tests. All offsets are seconds
 * EAST of UTC (the negation of the POSIX TZ offset).
 * Sources: POSIX.1-2024 XBD 8.3 "TZ" (std offset dst [offset],start[/time],end[/time];
 * Mm.w.d, Jn and n dates; time default 02:00:00; RFC 9636 3.3.1 extends the
 * rule hours to -167..167) and the IANA tz database
 * (https://www.iana.org/time-zones, release 2026e: the POSIX footer lines of
 * the zones listed in ciuki/tzdata.h).
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_TZRULE_H
#define CIUKI_TZRULE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TZR_NAME 16u            /* abbreviation incl. NUL ("CEST", "+0530") */
#define TZR_TEXT 64u            /* longest accepted TZ string incl. NUL */
#define TZR_ZONE 48u            /* IANA identifier incl. NUL */
enum { TZR_DATE_NONE, TZR_DATE_M, TZR_DATE_J, TZR_DATE_N };
struct tzr_when {
    uint8_t kind, month, week, wday; /* M: month 1..12, week 1..5 (5 = last), wday 0 = Sunday */
    uint16_t day;                    /* J: 1..365 (no leap day); N: 0..365 */
    int32_t seconds;                 /* local wall time of the change, -167h..167h */
};
struct tzr {
    char std[TZR_NAME], dst[TZR_NAME];
    int32_t std_offset, dst_offset;  /* seconds east of UTC */
    bool has_dst;
    struct tzr_when start, end;      /* start: std->dst, in standard time; end: dst->std, in daylight time */
};

/* Civil (proleptic Gregorian) helpers; days are counted from 1970-01-01. */
int64_t tzr_days_from_civil(int64_t year, unsigned month, unsigned day);
void tzr_civil_from_days(int64_t days, int64_t *year, unsigned *month, unsigned *day);
unsigned tzr_weekday(int64_t days);         /* 0 = Sunday */
bool tzr_leap(int64_t year);
unsigned tzr_month_days(int64_t year, unsigned month);
int64_t tzr_floor_div(int64_t a, int64_t b);

/* Parses a complete POSIX TZ string. False leaves *out unspecified. */
bool tzr_parse(const char *text, struct tzr *out);
/* The "UTC0" rule: the historical convention when no zone is configured. */
void tzr_utc(struct tzr *out);
/* Offset in force at a UTC instant; *dst (optional) tells which side. */
int32_t tzr_offset(const struct tzr *, int64_t utc, bool *dst);
/* Local wall time to UTC. isdst: 1 prefer daylight, 0 prefer standard,
 * -1 automatic (an ambiguous hour resolves to its first occurrence, the
 * daylight one; a skipped hour is read with the standard offset, so 02:30 on
 * the spring day becomes 03:30 daylight). *dst (optional) is the side used. */
int64_t tzr_local_to_utc(const struct tzr *, int64_t local, int isdst, bool *dst);
/* First change strictly after utc, false when the zone has no daylight time. */
bool tzr_next_change(const struct tzr *, int64_t utc, int64_t *at);
/* UTC instants of the year's two changes (start = std->dst). */
void tzr_changes(const struct tzr *, int64_t year, int64_t *start, int64_t *end);
/* The standard-time part of a rule ("CET-1" of "CET-1CEST,..."), used when
 * automatic daylight-saving adjustment is off. Returns the length, 0 on
 * a malformed rule or a short buffer. */
size_t tzr_standard_only(const char *text, char *out, size_t cap);

/* system/settings/clock.cfg, LF lines key=value. Version 1 (f9-04b):
 * rtc_local and a fixed offset_minutes. Version 2 (G1) adds zone (IANA id),
 * tz (POSIX rule), auto_dst and rtc_bias_minutes, the offset of the local
 * time the RTC currently holds (it changes when the RTC is moved for a
 * daylight-saving change). offset_minutes stays: the standard offset. */
struct tzr_config {
    uint32_t version;
    bool rtc_local, auto_dst;
    int32_t offset_minutes, rtc_bias_minutes;
    char zone[TZR_ZONE], tz[TZR_TEXT];
};
void tzr_config_default(struct tzr_config *);      /* version 2, UTC0, RTC in UTC */
bool tzr_config_parse(const char *text, size_t length, struct tzr_config *out);
size_t tzr_config_format(const struct tzr_config *, char *out, size_t cap);
/* The rule a configuration selects (tz, its standard part without auto_dst). */
bool tzr_config_rule(const struct tzr_config *, struct tzr *out);
/* "<+0130>-1:30" style fixed rule for a minute offset; returns the length. */
size_t tzr_fixed_text(int32_t minutes, char *out, size_t cap);
#endif
