/* G1: POSIX TZ rules, civil calendar, clock.cfg codec and the bundled zone
 * table. Oracle: the host C library's own TZ implementation (glibc localtime
 * and mktime read the same POSIX rule strings), plus fixed Europe/Rome
 * instants of 2026 from the IANA rule "CET-1CEST,M3.5.0,M10.5.0/3".
 * SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <ciuki/tzrule.h>
#include <ciuki/tzdata.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static int64_t civil(int y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned s)
{ return tzr_days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s; }

static void test_civil(void)
{
    for (int64_t days = -800000; days <= 800000; days += 13) {
        int64_t y; unsigned m, d;
        tzr_civil_from_days(days, &y, &m, &d);
        CHECK(tzr_days_from_civil(y, m, d) == days);
        CHECK(m >= 1 && m <= 12 && d >= 1 && d <= tzr_month_days(y, m));
        time_t t = (time_t)(days * 86400);
        struct tm tm; gmtime_r(&t, &tm);
        CHECK(tm.tm_year + 1900 == y && (unsigned)tm.tm_mon + 1 == m && (unsigned)tm.tm_mday == d);
        CHECK((int)tzr_weekday(days) == tm.tm_wday);
    }
    CHECK(tzr_days_from_civil(1970, 1, 1) == 0 && tzr_days_from_civil(2000, 3, 1) == 11017);
    CHECK(tzr_floor_div(-1, 86400) == -1 && tzr_floor_div(86399, 86400) == 0 && tzr_floor_div(-86400, 86400) == -1);
}

static void test_rome(void)
{
    struct tzr r; bool dst;
    CHECK(tzr_parse("CET-1CEST,M3.5.0,M10.5.0/3", &r));
    CHECK(!strcmp(r.std, "CET") && !strcmp(r.dst, "CEST") && r.std_offset == 3600 && r.dst_offset == 7200 && r.has_dst);
    /* Spring 2026: Sunday 29 March, 02:00 CET -> 03:00 CEST = 01:00 UTC. */
    int64_t spring = civil(2026, 3, 29, 1, 0, 0), autumn = civil(2026, 10, 25, 1, 0, 0);
    int64_t s, e; tzr_changes(&r, 2026, &s, &e);
    CHECK(s == spring && e == autumn);
    CHECK(tzr_offset(&r, spring - 1, &dst) == 3600 && !dst);
    CHECK(tzr_offset(&r, spring, &dst) == 7200 && dst);
    /* Autumn 2026: Sunday 25 October, 03:00 CEST -> 02:00 CET = 01:00 UTC. */
    CHECK(tzr_offset(&r, autumn - 1, &dst) == 7200 && dst);
    CHECK(tzr_offset(&r, autumn, &dst) == 3600 && !dst);
    CHECK(tzr_offset(&r, civil(2026, 1, 15, 12, 0, 0), 0) == 3600);
    CHECK(tzr_offset(&r, civil(2026, 7, 1, 12, 0, 0), 0) == 7200);
    /* Local -> UTC on both transition days. */
    CHECK(tzr_local_to_utc(&r, civil(2026, 3, 29, 1, 59, 59), -1, &dst) == spring - 1 && !dst);
    CHECK(tzr_local_to_utc(&r, civil(2026, 3, 29, 3, 0, 0), -1, &dst) == spring && dst);
    /* The skipped hour 02:30 reads as standard time: 01:30 UTC = 03:30 CEST. */
    CHECK(tzr_local_to_utc(&r, civil(2026, 3, 29, 2, 30, 0), -1, &dst) == civil(2026, 3, 29, 1, 30, 0) && !dst);
    /* The repeated hour 02:30: first occurrence (CEST) by default, CET when asked. */
    CHECK(tzr_local_to_utc(&r, civil(2026, 10, 25, 2, 30, 0), -1, &dst) == civil(2026, 10, 25, 0, 30, 0) && dst);
    CHECK(tzr_local_to_utc(&r, civil(2026, 10, 25, 2, 30, 0), 0, &dst) == civil(2026, 10, 25, 1, 30, 0) && !dst);
    CHECK(tzr_local_to_utc(&r, civil(2026, 10, 25, 3, 0, 0), -1, &dst) == autumn + 3600 && !dst);
    int64_t at;
    CHECK(tzr_next_change(&r, civil(2026, 1, 1, 0, 0, 0), &at) && at == spring);
    CHECK(tzr_next_change(&r, spring, &at) && at == autumn);
    CHECK(tzr_next_change(&r, autumn, &at) && at == civil(2027, 3, 28, 1, 0, 0));
    /* 2027: 28 March / 31 October. */
    tzr_changes(&r, 2027, &s, &e);
    CHECK(s == civil(2027, 3, 28, 1, 0, 0) && e == civil(2027, 10, 31, 1, 0, 0));
    char text[TZR_TEXT];
    CHECK(tzr_standard_only("CET-1CEST,M3.5.0,M10.5.0/3", text, sizeof(text)) == 5 && !strcmp(text, "CET-1"));
    CHECK(tzr_standard_only("<+0330>-3:30", text, sizeof(text)) && !strcmp(text, "<+0330>-3:30"));
    CHECK(!tzr_standard_only("CET-1CEST,M3.5.0,M10.5.0/3", text, 5));
}

static void test_parse(void)
{
    static const char *const bad[] = {"", "C", "CE-1", "CET", "CET-25", "CET-1CEST,M3.5.0", "CET-1CEST,M13.5.0,M10.5.0",
        "CET-1CEST,M3.6.0,M10.5.0", "CET-1CEST,M3.5.7,M10.5.0", "CET-1CEST,J0,J100", "CET-1CEST,366,10", "<+0>1", "<+01-1",
        "CET-1CEST,M3.5.0/168,M10.5.0", "CET-1CEST-1,M3.5.0,M10.5.0", ":Europe/Rome", "CET-1 ", "CET-1CEST,M3.5.0,M10.5.0x",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ-1"};
    struct tzr r;
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) CHECK(!tzr_parse(bad[i], &r));
    CHECK(tzr_parse("UTC0", &r) && !r.has_dst && !r.std_offset);
    CHECK(tzr_parse("EST5EDT", &r) && r.has_dst && r.dst_offset == -4 * 3600 && r.start.month == 3 && r.end.month == 11);
    CHECK(tzr_parse("NST3:30NDT,M3.2.0,M11.1.0", &r) && r.std_offset == -(3 * 3600 + 1800) && r.dst_offset == -(2 * 3600 + 1800));
    CHECK(tzr_parse("<-01>1<+00>,M3.5.0/0,M10.5.0/1", &r) && !strcmp(r.std, "-01") && !strcmp(r.dst, "+00") && !r.dst_offset);
    CHECK(tzr_parse("IST-2IDT,M3.4.4/26,M10.5.0", &r) && r.start.seconds == 26 * 3600);
    CHECK(tzr_parse("<-02>2<-01>,M3.5.0/-1,M10.5.0/0", &r) && r.start.seconds == -3600);
    CHECK(tzr_parse("XXX3YYY,J60/2,300/2", &r) && r.start.kind == TZR_DATE_J && r.end.kind == TZR_DATE_N);
    /* Jn never counts 29 February; n does. */
    int64_t s, e; tzr_changes(&r, 2028, &s, &e);
    CHECK(s == civil(2028, 3, 1, 2, 0, 0) + 3 * 3600 && e == civil(2028, 10, 27, 2, 0, 0) + 2 * 3600);
    char text[TZR_TEXT];
    CHECK(tzr_fixed_text(0, text, sizeof(text)) == 4 && !strcmp(text, "UTC0"));
    CHECK(tzr_fixed_text(60, text, sizeof(text)) && !strcmp(text, "<+01>-1") && tzr_parse(text, &r) && r.std_offset == 3600);
    CHECK(tzr_fixed_text(-570, text, sizeof(text)) && !strcmp(text, "<-0930>9:30") && tzr_parse(text, &r) && r.std_offset == -34200);
    CHECK(tzr_fixed_text(345, text, sizeof(text)) && !strcmp(text, "<+0545>-5:45") && tzr_parse(text, &r) && r.std_offset == 20700);
}

/* Every bundled rule against glibc, every 3 h 17 min over 2019..2031 and
 * around each change: the offset, the abbreviation and the inverse. */
static void test_table_oracle(void)
{
    CHECK(CIUKI_TZ_ZONE_COUNT >= 50);
    CHECK(ciuki_tz_find(CIUKI_TZ_DEFAULT_ITALY) >= 0 && ciuki_tz_find(CIUKI_TZ_UTC) >= 0 && ciuki_tz_find("Mars/Base") < 0);
    CHECK(!strcmp(ciuki_tz_zones[ciuki_tz_find_minutes(60)].id, "Europe/Rome"));
    unsigned samples = 0;
    for (unsigned z = 0; z < CIUKI_TZ_ZONE_COUNT; z++) {
        const struct ciuki_tz_zone *zone = &ciuki_tz_zones[z];
        struct tzr r;
        CHECK(tzr_parse(zone->tz, &r));
        CHECK(r.std_offset == zone->minutes * 60);
        CHECK(!!r.has_dst == !!zone->daylight[0]);
        CHECK(!z || ciuki_tz_zones[z - 1].minutes <= zone->minutes);
        for (unsigned k = 0; k < z; k++) CHECK(strcmp(ciuki_tz_zones[k].id, zone->id));
        setenv("TZ", zone->tz, 1); tzset();
        int64_t first = civil(2019, 1, 1, 0, 0, 0), last = civil(2031, 12, 31, 0, 0, 0);
        for (int64_t t = first; t < last; t += 3 * 3600 + 17 * 60) {
            int64_t probe[5] = {t, 0, 0, 0, 0};
            int64_t at;
            unsigned n = 1;
            if (tzr_next_change(&r, t, &at) && at - t < 3 * 3600 + 17 * 60) { probe[n++] = at - 1; probe[n++] = at; probe[n++] = at + 1; }
            for (unsigned i = 0; i < n; i++) {
                time_t u = (time_t)probe[i];
                struct tm tm; localtime_r(&u, &tm);
                bool dst;
                CHECK(tzr_offset(&r, probe[i], &dst) == tm.tm_gmtoff);
                CHECK(dst == (tm.tm_isdst > 0));
                CHECK(!strcmp(dst ? r.dst : r.std, tm.tm_zone));
                int64_t local = probe[i] + tm.tm_gmtoff;
                CHECK(tzr_local_to_utc(&r, local, tm.tm_isdst > 0, 0) == probe[i]);
                samples++;
            }
        }
    }
    unsetenv("TZ"); tzset();
    printf("tzrule oracle: %u zones, %u samples agree with the host C library\n", (unsigned)CIUKI_TZ_ZONE_COUNT, samples);
}

static void test_config(void)
{
    struct tzr_config c;
    const char v1[] = "version=1\nrtc_local=1\noffset_minutes=60\n";
    CHECK(tzr_config_parse(v1, sizeof(v1) - 1, &c) && c.version == 2 && c.rtc_local && c.rtc_bias_minutes == 60 &&
          !strcmp(c.tz, "<+01>-1") && !c.zone[0]);
    const char v1utc[] = "version=1\nrtc_local=0\noffset_minutes=-300\n";
    CHECK(tzr_config_parse(v1utc, sizeof(v1utc) - 1, &c) && !c.rtc_local && !c.rtc_bias_minutes && !strcmp(c.tz, "<-05>5"));
    const char v2[] = "version=2\nrtc_local=1\noffset_minutes=60\nzone=Europe/Rome\ntz=CET-1CEST,M3.5.0,M10.5.0/3\nauto_dst=1\nrtc_bias_minutes=120\n";
    CHECK(tzr_config_parse(v2, sizeof(v2) - 1, &c) && c.rtc_bias_minutes == 120 && !strcmp(c.zone, "Europe/Rome") && c.auto_dst);
    char text[512];
    size_t n = tzr_config_format(&c, text, sizeof(text));
    CHECK(n == sizeof(v2) - 1 && !memcmp(text, v2, n));
    CHECK(!tzr_config_format(&c, text, 20));
    struct tzr r;
    c.auto_dst = false;
    CHECK(tzr_config_rule(&c, &r) && !r.has_dst && r.std_offset == 3600);
    const char nobias[] = "version=2\nrtc_local=1\nzone=Europe/Rome\ntz=CET-1CEST,M3.5.0,M10.5.0/3\nauto_dst=1\n";
    CHECK(tzr_config_parse(nobias, sizeof(nobias) - 1, &c) && c.rtc_bias_minutes == 60 && c.offset_minutes == 60);
    const char future[] = "version=2\nrtc_local=0\ntz=UTC0\nlater=yes\n";
    CHECK(tzr_config_parse(future, sizeof(future) - 1, &c) && !c.rtc_local);
    static const char *const bad[] = {"version=2\nrtc_local=1\n", "version=2\nrtc_local=1\ntz=CET\n",
        "version=3\nrtc_local=1\ntz=UTC0\n", "version=2\nrtc_local=1\ntz=UTC0", "version=2\nrtc_local=2\ntz=UTC0\n",
        "version=1\nrtc_local=1\n", "version=1\nrtc_local=1\noffset_minutes=841\n", "rtc_local=1\ntz=UTC0\n",
        "version=2\nrtc_local=1\nzone=Europe/Rome;\ntz=UTC0\n", "version=2\nrtc_local=1\ntz=UTC0\nrtc_bias_minutes=x\n"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) CHECK(!tzr_config_parse(bad[i], strlen(bad[i]), &c));
    tzr_config_default(&c);
    CHECK(c.version == 2 && !c.rtc_local && c.auto_dst && !strcmp(c.tz, "UTC0"));
}

int main(void)
{
    test_civil(); test_rome(); test_parse(); test_table_oracle(); test_config();
    printf("PASS tzrule: civil calendar, Rome 2026/2027 changes, parser, zone table, clock.cfg (%u checks)\n", checks);
    return 0;
}
