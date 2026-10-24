/* G1: the bundled time-zone list of Setup, Date/Time Properties and
 * kernel32 GetTimeZoneInformation. The tz column is the POSIX footer line of
 * the IANA tz database zone named in id (release 2026e,
 * https://www.iana.org/time-zones; each footer is the last line of the
 * compiled TZif file, RFC 9636 section 3.3), which states the rule in force
 * today. Labels follow the Windows 98 "(GMT+hh:mm) cities" style; the
 * standard/daylight names are the Windows time-zone key names returned by
 * GetTimeZoneInformation. Ordered by standard offset, as the Windows list.
 * Header-only data: every consumer gets its own read-only copy.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_TZDATA_H
#define CIUKI_TZDATA_H
#include <stddef.h>
#include <stdint.h>
struct ciuki_tz_zone {
    int16_t minutes;            /* standard offset east of UTC */
    const char *id, *label, *tz, *standard, *daylight;
};
#define CIUKI_TZ_DEFAULT_ITALY "Europe/Rome"
#define CIUKI_TZ_UTC "Etc/UTC"
static const struct ciuki_tz_zone ciuki_tz_zones[] __attribute__((unused)) = {
    {-720, "Etc/GMT+12", "(GMT-12:00) International Date Line West", "<-12>12", "Dateline Standard Time", ""},
    {-660, "Pacific/Pago_Pago", "(GMT-11:00) Midway Island, Samoa", "SST11", "UTC-11", ""},
    {-600, "Pacific/Honolulu", "(GMT-10:00) Hawaii", "HST10", "Hawaiian Standard Time", ""},
    {-570, "Pacific/Marquesas", "(GMT-09:30) Marquesas Islands", "<-0930>9:30", "Marquesas Standard Time", ""},
    {-540, "America/Anchorage", "(GMT-09:00) Alaska", "AKST9AKDT,M3.2.0,M11.1.0", "Alaskan Standard Time", "Alaskan Daylight Time"},
    {-480, "America/Los_Angeles", "(GMT-08:00) Pacific Time (US & Canada)", "PST8PDT,M3.2.0,M11.1.0", "Pacific Standard Time", "Pacific Daylight Time"},
    {-420, "America/Phoenix", "(GMT-07:00) Arizona", "MST7", "US Mountain Standard Time", ""},
    {-420, "America/Denver", "(GMT-07:00) Mountain Time (US & Canada)", "MST7MDT,M3.2.0,M11.1.0", "Mountain Standard Time", "Mountain Daylight Time"},
    {-360, "America/Chicago", "(GMT-06:00) Central Time (US & Canada)", "CST6CDT,M3.2.0,M11.1.0", "Central Standard Time", "Central Daylight Time"},
    {-360, "America/Mexico_City", "(GMT-06:00) Mexico City", "CST6", "Central Standard Time (Mexico)", ""},
    {-360, "America/Regina", "(GMT-06:00) Saskatchewan", "CST6", "Canada Central Standard Time", ""},
    {-300, "America/New_York", "(GMT-05:00) Eastern Time (US & Canada)", "EST5EDT,M3.2.0,M11.1.0", "Eastern Standard Time", "Eastern Daylight Time"},
    {-300, "America/Bogota", "(GMT-05:00) Bogota, Lima, Quito", "<-05>5", "SA Pacific Standard Time", ""},
    {-240, "America/Halifax", "(GMT-04:00) Atlantic Time (Canada)", "AST4ADT,M3.2.0,M11.1.0", "Atlantic Standard Time", "Atlantic Daylight Time"},
    {-240, "America/Caracas", "(GMT-04:00) Caracas, La Paz", "<-04>4", "SA Western Standard Time", ""},
    {-210, "America/St_Johns", "(GMT-03:30) Newfoundland", "NST3:30NDT,M3.2.0,M11.1.0", "Newfoundland Standard Time", "Newfoundland Daylight Time"},
    {-180, "America/Sao_Paulo", "(GMT-03:00) Brasilia", "<-03>3", "E. South America Standard Time", ""},
    {-180, "America/Argentina/Buenos_Aires", "(GMT-03:00) Buenos Aires", "<-03>3", "Argentina Standard Time", ""},
    {-120, "Atlantic/South_Georgia", "(GMT-02:00) Mid-Atlantic", "<-02>2", "UTC-02", ""},
    {-60, "Atlantic/Azores", "(GMT-01:00) Azores", "<-01>1<+00>,M3.5.0/0,M10.5.0/1", "Azores Standard Time", "Azores Daylight Time"},
    {-60, "Atlantic/Cape_Verde", "(GMT-01:00) Cape Verde Islands", "<-01>1", "Cape Verde Standard Time", ""},
    {0, "Etc/UTC", "(GMT) Coordinated Universal Time", "UTC0", "Coordinated Universal Time", ""},
    {0, "Europe/London", "(GMT) Dublin, Edinburgh, Lisbon, London", "GMT0BST,M3.5.0/1,M10.5.0", "GMT Standard Time", "GMT Daylight Time"},
    {60, "Europe/Rome", "(GMT+01:00) Amsterdam, Berlin, Bern, Rome, Stockholm, Vienna", "CET-1CEST,M3.5.0,M10.5.0/3", "W. Europe Standard Time", "W. Europe Daylight Time"},
    {60, "Europe/Paris", "(GMT+01:00) Brussels, Copenhagen, Madrid, Paris", "CET-1CEST,M3.5.0,M10.5.0/3", "Romance Standard Time", "Romance Daylight Time"},
    {60, "Africa/Lagos", "(GMT+01:00) West Central Africa", "WAT-1", "W. Central Africa Standard Time", ""},
    {120, "Europe/Athens", "(GMT+02:00) Athens, Bucharest", "EET-2EEST,M3.5.0/3,M10.5.0/4", "GTB Standard Time", "GTB Daylight Time"},
    {120, "Europe/Helsinki", "(GMT+02:00) Helsinki, Kyiv, Riga, Sofia, Tallinn, Vilnius", "EET-2EEST,M3.5.0/3,M10.5.0/4", "FLE Standard Time", "FLE Daylight Time"},
    {120, "Africa/Cairo", "(GMT+02:00) Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24", "Egypt Standard Time", "Egypt Daylight Time"},
    {120, "Asia/Jerusalem", "(GMT+02:00) Jerusalem", "IST-2IDT,M3.4.4/26,M10.5.0", "Israel Standard Time", "Israel Daylight Time"},
    {120, "Africa/Johannesburg", "(GMT+02:00) Harare, Pretoria", "SAST-2", "South Africa Standard Time", ""},
    {180, "Europe/Moscow", "(GMT+03:00) Moscow, St. Petersburg", "MSK-3", "Russian Standard Time", ""},
    {180, "Europe/Istanbul", "(GMT+03:00) Istanbul", "<+03>-3", "Turkey Standard Time", ""},
    {180, "Africa/Nairobi", "(GMT+03:00) Nairobi", "EAT-3", "E. Africa Standard Time", ""},
    {210, "Asia/Tehran", "(GMT+03:30) Tehran", "<+0330>-3:30", "Iran Standard Time", ""},
    {240, "Asia/Dubai", "(GMT+04:00) Abu Dhabi, Muscat", "<+04>-4", "Arabian Standard Time", ""},
    {240, "Asia/Baku", "(GMT+04:00) Baku", "<+04>-4", "Azerbaijan Standard Time", ""},
    {270, "Asia/Kabul", "(GMT+04:30) Kabul", "<+0430>-4:30", "Afghanistan Standard Time", ""},
    {300, "Asia/Karachi", "(GMT+05:00) Islamabad, Karachi", "PKT-5", "Pakistan Standard Time", ""},
    {300, "Asia/Tashkent", "(GMT+05:00) Tashkent", "<+05>-5", "West Asia Standard Time", ""},
    {330, "Asia/Kolkata", "(GMT+05:30) Chennai, Kolkata, Mumbai, New Delhi", "IST-5:30", "India Standard Time", ""},
    {345, "Asia/Kathmandu", "(GMT+05:45) Kathmandu", "<+0545>-5:45", "Nepal Standard Time", ""},
    {360, "Asia/Dhaka", "(GMT+06:00) Dhaka", "<+06>-6", "Bangladesh Standard Time", ""},
    {390, "Asia/Yangon", "(GMT+06:30) Yangon (Rangoon)", "<+0630>-6:30", "Myanmar Standard Time", ""},
    {420, "Asia/Bangkok", "(GMT+07:00) Bangkok, Hanoi, Jakarta", "<+07>-7", "SE Asia Standard Time", ""},
    {480, "Asia/Shanghai", "(GMT+08:00) Beijing, Chongqing, Hong Kong, Urumqi", "CST-8", "China Standard Time", ""},
    {480, "Asia/Singapore", "(GMT+08:00) Kuala Lumpur, Singapore", "<+08>-8", "Singapore Standard Time", ""},
    {480, "Australia/Perth", "(GMT+08:00) Perth", "AWST-8", "W. Australia Standard Time", ""},
    {540, "Asia/Tokyo", "(GMT+09:00) Osaka, Sapporo, Tokyo", "JST-9", "Tokyo Standard Time", ""},
    {540, "Asia/Seoul", "(GMT+09:00) Seoul", "KST-9", "Korea Standard Time", ""},
    {570, "Australia/Adelaide", "(GMT+09:30) Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3", "Cen. Australia Standard Time", "Cen. Australia Daylight Time"},
    {570, "Australia/Darwin", "(GMT+09:30) Darwin", "ACST-9:30", "AUS Central Standard Time", ""},
    {600, "Australia/Brisbane", "(GMT+10:00) Brisbane", "AEST-10", "E. Australia Standard Time", ""},
    {600, "Australia/Sydney", "(GMT+10:00) Canberra, Melbourne, Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3", "AUS Eastern Standard Time", "AUS Eastern Daylight Time"},
    {600, "Pacific/Guam", "(GMT+10:00) Guam, Port Moresby", "ChST-10", "West Pacific Standard Time", ""},
    {660, "Pacific/Noumea", "(GMT+11:00) Solomon Is., New Caledonia", "<+11>-11", "Central Pacific Standard Time", ""},
    {720, "Pacific/Auckland", "(GMT+12:00) Auckland, Wellington", "NZST-12NZDT,M9.5.0,M4.1.0/3", "New Zealand Standard Time", "New Zealand Daylight Time"},
    {720, "Pacific/Fiji", "(GMT+12:00) Fiji", "<+12>-12", "Fiji Standard Time", ""},
    {780, "Pacific/Tongatapu", "(GMT+13:00) Nuku'alofa", "<+13>-13", "Tonga Standard Time", ""},
    {840, "Pacific/Kiritimati", "(GMT+14:00) Kiritimati Island", "<+14>-14", "Line Islands Standard Time", ""},
};
#define CIUKI_TZ_ZONE_COUNT (sizeof(ciuki_tz_zones) / sizeof(ciuki_tz_zones[0]))
static inline int ciuki_tz_same(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
/* Index of an IANA id, -1 when not bundled. */
static inline int ciuki_tz_find(const char *id)
{
    for (unsigned i = 0; id && i < CIUKI_TZ_ZONE_COUNT; i++) if (ciuki_tz_same(ciuki_tz_zones[i].id, id)) return (int)i;
    return -1;
}
/* First zone with this standard offset (the SETUP.INF "+hh:mm" form). */
static inline int ciuki_tz_find_minutes(int minutes)
{
    for (unsigned i = 0; i < CIUKI_TZ_ZONE_COUNT; i++) if (ciuki_tz_zones[i].minutes == minutes) return (int)i;
    return -1;
}
#endif
