/* Clock and Calendar host tests: calendar arithmetic and the window.
 * SPDX-License-Identifier: MIT */
#include "clock.h"
#include "acc_test.h"
static int64_t fixed = 1791711660;   /* 2026-10-11 09:41:00 UTC */
static int64_t source(void *host) { (void)host; return fixed; }
int main(int argc, char **argv)
{
    const char *folder = argc > 1 ? argv[1] : NULL;
    assert(clock_leap(2000) && clock_leap(2024) && !clock_leap(1900) && !clock_leap(2026));
    assert(clock_days_in_month(2024, 2) == 29 && clock_days_in_month(2026, 2) == 28 && clock_days_in_month(2026, 12) == 31);
    assert(clock_weekday(2026, 10, 12) == 1);          /* Monday */
    assert(clock_weekday(2000, 1, 1) == 6);            /* Saturday */
    assert(clock_weekday(1970, 1, 1) == 4);            /* Thursday */
    assert(clock_day_of_year(2026, 12, 31) == 365 && clock_day_of_year(2024, 12, 31) == 366);
    assert(clock_iso_week(2026, 10, 12) == 42);
    assert(clock_iso_week(2021, 1, 3) == 53);          /* belongs to 2020-W53 */
    assert(clock_iso_week(2024, 12, 30) == 1);         /* 2025-W01 */
    struct clock_date d; int h, m, s;
    clock_from_epoch(0, &d, &h, &m, &s); assert(d.year == 1970 && d.month == 1 && d.day == 1 && !h && !m && !s);
    clock_from_epoch(fixed, &d, &h, &m, &s); assert(d.year == 2026 && d.month == 10 && d.day == 11 && h == 9 && m == 41 && !s);
    clock_from_epoch(951782400, &d, &h, &m, &s); assert(d.year == 2000 && d.month == 2 && d.day == 29);
    clock_from_epoch(4102444799LL, &d, &h, &m, &s); assert(d.year == 2099 && d.month == 12 && d.day == 31 && h == 23 && s == 59);
    struct clock_date t = clock_add_days((struct clock_date){2026, 12, 31}, 1); assert(t.year == 2027 && t.month == 1 && t.day == 1);
    t = clock_add_days((struct clock_date){2024, 3, 1}, -1); assert(t.month == 2 && t.day == 29);
    t = clock_add_months((struct clock_date){2026, 1, 31}, 1); assert(t.month == 2 && t.day == 28);
    t = clock_add_months((struct clock_date){2026, 1, 15}, -1); assert(t.year == 2025 && t.month == 12);
    t = clock_cell_date(2026, 10, 0); assert(t.year == 2026 && t.month == 9 && t.day == 27);   /* Sunday before 1 Oct (Thursday) */
    assert(clock_cell_of(2026, 10, (struct clock_date){2026, 10, 1}) == 4);
    assert(clock_cell_of(2026, 10, (struct clock_date){2026, 12, 1}) == -1);

    static uint32_t pixels[CLOCK_W * CLOCK_H];
    static struct du_ui ui; static struct clock_app app;
    du_init(&ui, pixels, CLOCK_W, CLOCK_H);
    clock_attach(&app, &ui, source, NULL);
    assert(app.selected.day == 11 && app.shown_month == 10);
    du_render(&ui);
    at_save(folder, "clock-initial", &ui);
    /* Click 20 October on the grid; Next month; keyboard back; Today. */
    int cell = clock_cell_of(2026, 10, (struct clock_date){2026, 10, 20});
    struct du_rect r = clock_grid_cell(cell);
    at_click(&ui, r.x + r.w / 2, r.y + r.h / 2);
    assert(app.selected.day == 20 && app.selected.month == 10);
    assert(strstr(du_get(&ui, CLOCK_STATUS)->text, "Tuesday, 20 October 2026"));
    at_click_widget(&ui, CLOCK_NEXT); assert(app.shown_month == 11 && app.selected.day == 20);
    at_tap(&ui, DU_KEY_RIGHT); assert(app.selected.day == 21);
    at_tap(&ui, DU_KEY_PGUP); assert(app.shown_month == 10);
    at_tap(&ui, DU_KEY_HOME); assert(app.selected.day == 11);
    /* The clock ticks; midnight moves the selected today. */
    fixed = 1791763199;   /* 23:59:59 */
    clock_event(&ui, &(struct du_event){.type = DU_EV_TICK, .id = 1}, &app);
    fixed += 1;
    clock_event(&ui, &(struct du_event){.type = DU_EV_TICK, .id = 1}, &app);
    assert(app.today.day == 12 && app.selected.day == 12);
    fixed = 1791711660 + 3 * 3600 + 17 * 60 + 42;     /* 12:58:42 */
    clock_update(&app);
    du_render(&ui);
    at_save(folder, "clock-noon", &ui);
    puts("clock: PASS");
    return 0;
}
