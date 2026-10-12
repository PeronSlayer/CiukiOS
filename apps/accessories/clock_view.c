/* SPDX-License-Identifier: MIT
 * Clock and Calendar window: analog face, digital time, month calendar. */
#include "clock.h"
#include <stdio.h>
#include <string.h>

#define FACE_X 108
#define FACE_Y 126
#define FACE_R 86
#define GRID_X 222
#define GRID_Y 74
#define CELL_W 24
#define CELL_H 20
/* Theme colours (theme.h): the dial is the window paper, the hands the ink,
 * today the accent; the second hand stays red. */
#define FACE_COLOR DU_WINDOW
#define HAND_COLOR DU_INK
#define SECOND_COLOR 0xb03a2eu
#define TODAY_COLOR (desk_theme->accent)

struct du_rect clock_grid_cell(int cell)
{ return (struct du_rect){GRID_X + (cell % 7) * CELL_W, GRID_Y + (cell / 7) * CELL_H, CELL_W, CELL_H}; }

static void status(struct clock_app *a)
{
    char text[128]; struct clock_date s = a->selected;
    snprintf(text, sizeof(text), "%s, %d %s %d - day %d, week %d",
             clock_weekday_names[clock_weekday(s.year, s.month, s.day)], s.day, clock_month_names[s.month - 1], s.year,
             clock_day_of_year(s.year, s.month, s.day), clock_iso_week(s.year, s.month, s.day));
    du_set_text(du_get(a->ui, CLOCK_STATUS), text);
}
static void show_month_of(struct clock_app *a, struct clock_date t)
{
    a->selected = t; a->shown_year = t.year; a->shown_month = t.month;
    status(a); a->ui->dirty = 1;
}

void clock_update(struct clock_app *a)
{
    int64_t now = a->clock ? a->clock(a->host) : 0;
    if (now == a->now && a->today.year) return;
    struct clock_date before = a->today; int h, m, s;
    a->now = now;
    clock_from_epoch(now, &a->today, &h, &m, &s);
    /* Follow midnight when the calendar showed the old today. */
    if (!before.year || (a->selected.year == before.year && a->selected.month == before.month && a->selected.day == before.day &&
        (before.day != a->today.day || before.month != a->today.month)))
        show_month_of(a, a->today);
    a->ui->dirty = 1;
}

static void hand(struct du_canvas *c, int position60x12, int length, int width, uint32_t color)
{
    /* position in 1/12 steps of 6 degrees (720 per turn): interpolate the table. */
    int i = position60x12 / 12, f = position60x12 % 12;
    int sx = (acc_sin60(i) * (12 - f) + acc_sin60(i + 1) * f) / 12, cy = (acc_cos60(i) * (12 - f) + acc_cos60(i + 1) * f) / 12;
    acc_thick_line(c, FACE_X, FACE_Y, FACE_X + sx * length / 1024, FACE_Y - cy * length / 1024, width, color);
}

void clock_paint(struct du_ui *u, void *user)
{
    struct clock_app *a = user; struct du_canvas *c = &u->canvas;
    struct clock_date d; int h, m, s;
    clock_from_epoch(a->now, &d, &h, &m, &s);
    /* Face: a cream dial with an etched rim, minute ticks and the four numbers. */
    acc_disc(c, FACE_X, FACE_Y, FACE_R + 3, DU_SHADOW);
    acc_disc(c, FACE_X, FACE_Y, FACE_R + 1, DU_HILITE);
    acc_disc(c, FACE_X, FACE_Y, FACE_R, FACE_COLOR);
    for (int i = 0; i < 60; i++) {
        int outer = FACE_R - 4, inner = i % 5 ? FACE_R - 8 : FACE_R - 14;
        int x0 = FACE_X + acc_sin60(i) * inner / 1024, y0 = FACE_Y - acc_cos60(i) * inner / 1024;
        int x1 = FACE_X + acc_sin60(i) * outer / 1024, y1 = FACE_Y - acc_cos60(i) * outer / 1024;
        if (i % 5) acc_line(c, x0, y0, x1, y1, DU_GRAY); else acc_thick_line(c, x0, y0, x1, y1, 2, DU_INK);
    }
    static const char *const numbers[4] = { "12", "3", "6", "9" };
    for (int q = 0; q < 4; q++) {
        int r = FACE_R - 24, x = FACE_X + acc_sin60(q * 15) * r / 1024, y = FACE_Y - acc_cos60(q * 15) * r / 1024;
        du_text(c, x - du_text_width(numbers[q]) / 2, y - DU_GLYPH_H / 2, numbers[q], DU_INK);
    }
    /* Ciuki's tagline on the dial, as the 0.8 identity. */
    du_text(c, FACE_X - du_text_width("CiukiOS") / 2, FACE_Y + 26, "CiukiOS", DU_GRAY);
    hand(c, ((h % 12) * 60 + m) * 12 / 12, FACE_R * 52 / 100, 4, HAND_COLOR);     /* hour: 720 units per 12 h */
    hand(c, m * 12 + s / 5, FACE_R * 78 / 100, 3, HAND_COLOR);
    if (a->seconds_hand) hand(c, s * 12, FACE_R * 86 / 100, 1, SECOND_COLOR);
    acc_disc(c, FACE_X, FACE_Y, 4, a->seconds_hand ? SECOND_COLOR : HAND_COLOR);
    /* Digital time and the zone below the dial. */
    char text[64];
    if (a->h24) snprintf(text, sizeof(text), "%02d:%02d:%02d UTC", h, m, s);
    else snprintf(text, sizeof(text), "%d:%02d:%02d %s UTC", h % 12 ? h % 12 : 12, m, s, h < 12 ? "AM" : "PM");
    struct du_rect box = { FACE_X - 70, 222, 140, 16 };
    du_fill(c, box, DU_WINDOW); du_edge(c, box, DU_SUNKEN_THIN);
    du_text(c, FACE_X - du_text_width(text) / 2, box.y + 5, text, DU_INK);
    /* Calendar: month title, weekday header and the 6x7 grid. */
    snprintf(text, sizeof(text), "%s %d", clock_month_names[a->shown_month - 1], a->shown_year);
    du_text(c, GRID_X + 7 * CELL_W / 2 - du_text_width(text) / 2, 34, text, DU_INK);
    struct du_rect frame = { GRID_X - 2, GRID_Y - 18, 7 * CELL_W + 4, 6 * CELL_H + 20 };
    du_fill(c, frame, DU_WINDOW); du_edge(c, frame, DU_SUNKEN);
    static const char *const heads[7] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
    du_fill(c, (struct du_rect){GRID_X, GRID_Y - 16, 7 * CELL_W, 14}, DU_SELECT);
    for (int i = 0; i < 7; i++) du_text(c, GRID_X + i * CELL_W + (CELL_W - du_text_width(heads[i])) / 2, GRID_Y - 12, heads[i], DU_SELECT_INK);
    for (int cell = 0; cell < 42; cell++) {
        struct clock_date t = clock_cell_date(a->shown_year, a->shown_month, cell);
        struct du_rect r = clock_grid_cell(cell);
        int selected = t.year == a->selected.year && t.month == a->selected.month && t.day == a->selected.day;
        int today = t.year == a->today.year && t.month == a->today.month && t.day == a->today.day;
        snprintf(text, sizeof(text), "%d", t.day);
        uint32_t ink = t.month != a->shown_month ? DU_GRAY : DU_INK;
        if (selected) { du_fill(c, (struct du_rect){r.x + 1, r.y + 1, r.w - 2, r.h - 2}, DU_SELECT); ink = DU_SELECT_INK; }
        if (today) du_outline(c, (struct du_rect){r.x + 1, r.y + 1, r.w - 2, r.h - 2}, TODAY_COLOR);
        du_text(c, r.x + (r.w - du_text_width(text)) / 2, r.y + (r.h - DU_GLYPH_H) / 2, text, ink);
    }
}

static const struct du_menu_item view_template[] = {
    {"&Seconds Hand", CLOCK_M_SECONDS, 0, 0, DU_ITEM_CHECKED}, {"&24-Hour Clock", CLOCK_M_24H, 0, 0, DU_ITEM_CHECKED} };
static const struct du_menu_item help_template[] = { {"&About Clock", CLOCK_M_ABOUT, 0, 0, 0} };

void clock_attach(struct clock_app *a, struct du_ui *u, int64_t (*source)(void *), void *host)
{
    a->ui = u; a->clock = source; a->host = host; a->seconds_hand = 1; a->h24 = 1;
    u->event = clock_event; u->user = a; u->paint = clock_paint;
    memcpy(a->view_items, view_template, sizeof(view_template));
    memcpy(a->help_items, help_template, sizeof(help_template));
    a->menus[0] = (struct du_menu){"&View", a->view_items, 2};
    a->menus[1] = (struct du_menu){"&Help", a->help_items, 1};
    u->menus = a->menus; u->menu_count = 2;
    du_add(u, DU_BUTTON, CLOCK_PREV, (struct du_rect){GRID_X - 2, 28, 22, 20}, "<");
    du_add(u, DU_BUTTON, CLOCK_NEXT, (struct du_rect){GRID_X + 7 * CELL_W - 20, 28, 22, 20}, ">");
    /* One hit target under the painted grid: the pointer position picks the day. */
    du_add(u, DU_TOOLBUTTON, CLOCK_GRID, (struct du_rect){GRID_X, GRID_Y, 7 * CELL_W, 6 * CELL_H}, "");
    du_add(u, DU_BUTTON, CLOCK_TODAY, (struct du_rect){GRID_X + 7 * CELL_W / 2 - 40, 204, 80, 22}, "&Today");
    du_add(u, DU_STATUS, CLOCK_STATUS, (struct du_rect){0, CLOCK_H - 20, CLOCK_W, 20}, "");
    clock_update(a);
    u->focus = -1;
}

static void select_date(struct clock_app *a, struct clock_date t) { show_month_of(a, t); }

void clock_event(struct du_ui *u, const struct du_event *e, void *user)
{
    struct clock_app *a = user;
    switch (e->type) {
    case DU_EV_TICK: clock_update(a); return;
    case DU_EV_ACTIVATE:
        if (e->id == CLOCK_PREV || e->id == CLOCK_NEXT) {
            struct clock_date t = { a->shown_year, a->shown_month, a->selected.day };
            select_date(a, clock_add_months(t, e->id == CLOCK_PREV ? -1 : 1));
        } else if (e->id == CLOCK_TODAY) select_date(a, a->today);
        else if (e->id == CLOCK_GRID) {
            int col = (u->mouse_x - GRID_X) / CELL_W, row = (u->mouse_y - GRID_Y) / CELL_H;
            if (col >= 0 && col < 7 && row >= 0 && row < 6) select_date(a, clock_cell_date(a->shown_year, a->shown_month, row * 7 + col));
        } else if (e->id == CLOCK_M_SECONDS) { a->seconds_hand = !a->seconds_hand; a->view_items[0].flags = a->seconds_hand ? DU_ITEM_CHECKED : 0; }
        else if (e->id == CLOCK_M_24H) { a->h24 = !a->h24; a->view_items[1].flags = a->h24 ? DU_ITEM_CHECKED : 0; }
        else if (e->id == CLOCK_M_ABOUT)
            du_message(u, 1, "About Clock", "CiukiOS Clock and Calendar - Classic computing, one woof at a time.\n"
                       "The system clock keeps UTC (battery clock).\nCode: MIT.", DU_DLG_ICON_INFO, NULL, 0, 0, 0);
        u->focus = -1;
        break;
    case DU_EV_KEY: {
        int days = e->key == DU_KEY_LEFT ? -1 : e->key == DU_KEY_RIGHT ? 1 : e->key == DU_KEY_UP ? -7 : e->key == DU_KEY_DOWN ? 7 : 0;
        if (days) select_date(a, clock_add_days(a->selected, days));
        else if (e->key == DU_KEY_PGUP || e->key == DU_KEY_PGDN) select_date(a, clock_add_months(a->selected, e->key == DU_KEY_PGUP ? -1 : 1));
        else if (e->key == DU_KEY_HOME) select_date(a, a->today);
        else return;
        break;
    }
    default: return;
    }
    u->dirty = 1;
}
