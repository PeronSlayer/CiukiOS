/* SPDX-License-Identifier: MIT
 * Proleptic Gregorian calendar arithmetic for the Clock and Calendar. */
#include "clock.h"
const char *const clock_month_names[12] = { "January", "February", "March", "April", "May", "June", "July",
                                            "August", "September", "October", "November", "December" };
const char *const clock_weekday_names[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
int clock_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
int clock_days_in_month(int y, int m)
{
    static const int days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && clock_leap(y) ? 29 : days[(m - 1) % 12];
}
int clock_weekday(int y, int m, int d)
{
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };   /* Sakamoto */
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}
int clock_day_of_year(int y, int m, int d)
{
    int n = d;
    for (int i = 1; i < m; i++) n += clock_days_in_month(y, i);
    return n;
}
int clock_iso_week(int y, int m, int d)
{
    int wd = (clock_weekday(y, m, d) + 6) % 7;            /* Monday 0 */
    int doy = clock_day_of_year(y, m, d);
    int week = (doy - wd + 9) / 7;                        /* week of the Thursday */
    if (week < 1) return clock_iso_week(y - 1, 12, 31);
    int days = clock_leap(y) ? 366 : 365;
    if (doy - wd + 3 > days) return 1;                    /* Thursday falls in the next year */
    return week;
}
struct clock_date clock_add_days(struct clock_date t, int n)
{
    while (n > 0) { if (++t.day > clock_days_in_month(t.year, t.month)) { t.day = 1; if (++t.month > 12) { t.month = 1; t.year++; } } n--; }
    while (n < 0) { if (--t.day < 1) { if (--t.month < 1) { t.month = 12; t.year--; } t.day = clock_days_in_month(t.year, t.month); } n++; }
    return t;
}
struct clock_date clock_add_months(struct clock_date t, int n)
{
    int index = t.year * 12 + (t.month - 1) + n;
    t.year = index / 12; t.month = index % 12 + 1;
    int max = clock_days_in_month(t.year, t.month);
    if (t.day > max) t.day = max;
    return t;
}
void clock_from_epoch(int64_t s, struct clock_date *date, int *hour, int *minute, int *second)
{
    int64_t days = s / 86400, rem = s % 86400;
    if (rem < 0) { rem += 86400; days--; }
    *hour = (int)(rem / 3600); *minute = (int)(rem / 60 % 60); *second = (int)(rem % 60);
    /* Days to civil date (H. Hinnant's algorithm). */
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    int64_t doe = days - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    date->year = (int)(y + (m <= 2)); date->month = (int)m; date->day = (int)d;
}
struct clock_date clock_cell_date(int y, int m, int cell)
{
    struct clock_date first = { y, m, 1 };
    return clock_add_days(first, cell - clock_weekday(y, m, 1));
}
int clock_cell_of(int y, int m, struct clock_date t)
{
    struct clock_date start = clock_cell_date(y, m, 0);
    for (int cell = 0; cell < 42; cell++, start = clock_add_days(start, 1))
        if (start.year == t.year && start.month == t.month && start.day == t.day) return cell;
    return -1;
}
