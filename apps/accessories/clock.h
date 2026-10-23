/* SPDX-License-Identifier: MIT
 * Clock and Calendar: an analog and digital clock with a month calendar, in
 * the manner of the 0.8 Date and Time applet and the Windows 98 clock. It
 * shows the local time of the system zone (G1); the host supplies the wall
 * seconds. */
#ifndef CIUKI_ACC_CLOCK_H
#define CIUKI_ACC_CLOCK_H
#include "acc.h"
#include <time.h>
struct clock_date { int year, month, day; };      /* month 1..12 */
int clock_leap(int year);
int clock_days_in_month(int year, int month);
int clock_weekday(int year, int month, int day);   /* 0 Sunday .. 6 Saturday */
int clock_day_of_year(int year, int month, int day);
int clock_iso_week(int year, int month, int day);
struct clock_date clock_add_days(struct clock_date, int days);
struct clock_date clock_add_months(struct clock_date, int months);   /* day clamped */
void clock_from_epoch(int64_t seconds, struct clock_date *, int *hour, int *minute, int *second);
extern const char *const clock_month_names[12];
extern const char *const clock_weekday_names[7];
/* Calendar grid: cell 0..41 -> date (the 6x7 grid starts on the Sunday before the 1st). */
struct clock_date clock_cell_date(int year, int month, int cell);
int clock_cell_of(int year, int month, struct clock_date);   /* -1 when outside */

#define CLOCK_W 400
#define CLOCK_H 262
struct clock_app {
    struct du_ui *ui;
    int64_t now;                       /* local wall seconds since 1970 */
    int64_t (*clock)(void *); void *host;
    struct clock_date today, selected; int shown_year, shown_month;
    int seconds_hand, h24;
    struct du_menu menus[2]; struct du_menu_item view_items[2], help_items[1];
};
enum { CLOCK_M_SECONDS = 1, CLOCK_M_24H, CLOCK_M_ABOUT, CLOCK_PREV = 100, CLOCK_NEXT, CLOCK_TODAY, CLOCK_GRID, CLOCK_STATUS, CLOCK_FACE };
void clock_attach(struct clock_app *, struct du_ui *, int64_t (*clock)(void *), void *host);
void clock_update(struct clock_app *);    /* reads the clock; redraws when the second changed */
void clock_event(struct du_ui *, const struct du_event *, void *);
void clock_paint(struct du_ui *, void *);
struct du_rect clock_grid_cell(int cell);
#endif
