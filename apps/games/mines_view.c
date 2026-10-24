/* SPDX-License-Identifier: MIT
 * Minesweeper window: LED mine counter and timer, the face button, the board
 * and the Game menu. */
#include "mines.h"
#include <stdio.h>
#include <string.h>
#define LED_W 13
#define LED_H 23
#define PANEL_Y 26
#define PANEL_H 38
#define BOARD_Y 72

struct du_rect mines_board_rect(const struct mines *m)
{
    return (struct du_rect){((m->view_w ? m->view_w : MINES_W) - m->w * MINES_CELL) / 2, BOARD_Y, m->w * MINES_CELL, m->h * MINES_CELL};
}
struct du_rect mines_face_rect(const struct mines *m)
{
    struct du_rect b = mines_board_rect(m);
    return (struct du_rect){b.x + b.w / 2 - 13, PANEL_Y + 6, 26, 26};
}
void mines_window_size(const struct mines *m, int *w, int *h)
{
    *w = m->w * MINES_CELL + 24; *h = BOARD_Y + m->h * MINES_CELL + 12;
}
static struct du_rect led_rect(const struct mines *m, int right)
{
    struct du_rect b = mines_board_rect(m);
    return (struct du_rect){right ? b.x + b.w - 3 * LED_W - 10 : b.x + 6, PANEL_Y + 7, 3 * LED_W + 2, LED_H + 2};
}
/* Seven segments: a b c d e f g. */
static const uint8_t segments[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};
static void led_digit(struct du_canvas *c, int x, int y, int value)   /* value -1: a minus sign */
{
    uint8_t s = value < 0 ? 0x40 : segments[value];
    static const struct du_rect seg[7] = {{2, 1, 9, 2}, {10, 2, 2, 9}, {10, 12, 2, 9}, {2, 20, 9, 2}, {1, 12, 2, 9}, {1, 2, 2, 9}, {2, 10, 9, 3}};
    for (int i = 0; i < 7; i++) {
        struct du_rect r = {x + seg[i].x, y + seg[i].y, seg[i].w, seg[i].h};
        du_fill(c, r, (s >> i) & 1 ? 0xff0000u : 0x300000u);
    }
}
static void led(struct du_canvas *c, struct du_rect r, int value)
{
    du_fill(c, r, 0x000000u);
    int neg = value < 0; if (neg) value = -value;
    if (value > 999) value = 999;
    int d[3] = {value / 100, value / 10 % 10, value % 10};
    for (int i = 0; i < 3; i++) led_digit(c, r.x + 1 + i * LED_W, r.y + 1, i == 0 && neg ? -1 : d[i]);
}
static void face(struct du_canvas *c, const struct mines_app *a)
{
    struct du_rect r = mines_face_rect(&a->game);
    du_fill(c, r, 0xc0c0c0u);
    du_edge(c, r, a->pressed_face ? DU_SUNKEN_THIN : DU_RAISED_THIN);
    int cx = r.x + 13 + a->pressed_face, cy = r.y + 13 + a->pressed_face;
    acc_disc(c, cx, cy, 9, 0xffff00u); acc_circle(c, cx, cy, 9, 0x000000u);
    enum mines_state st = a->game.state;
    if (st == MINES_WON) {            /* sunglasses */
        du_fill(c, (struct du_rect){cx - 7, cy - 4, 6, 3}, 0); du_fill(c, (struct du_rect){cx + 1, cy - 4, 6, 3}, 0);
        du_fill(c, (struct du_rect){cx - 7, cy - 4, 14, 1}, 0);
    } else if (st == MINES_LOST) {    /* crossed eyes */
        acc_line(c, cx - 5, cy - 4, cx - 2, cy - 1, 0); acc_line(c, cx - 2, cy - 4, cx - 5, cy - 1, 0);
        acc_line(c, cx + 2, cy - 4, cx + 5, cy - 1, 0); acc_line(c, cx + 5, cy - 4, cx + 2, cy - 1, 0);
    } else { du_fill(c, (struct du_rect){cx - 4, cy - 3, 2, 2}, 0); du_fill(c, (struct du_rect){cx + 2, cy - 3, 2, 2}, 0); }
    if (st == MINES_LOST) { acc_line(c, cx - 4, cy + 5, cx - 2, cy + 3, 0); acc_line(c, cx - 2, cy + 3, cx + 2, cy + 3, 0); acc_line(c, cx + 2, cy + 3, cx + 4, cy + 5, 0); }
    else if (a->holding && st != MINES_WON) acc_circle(c, cx, cy + 4, 2, 0x000000u);   /* "ooh" while a cell is held down */
    else { acc_line(c, cx - 4, cy + 3, cx - 2, cy + 5, 0); acc_line(c, cx - 2, cy + 5, cx + 2, cy + 5, 0); acc_line(c, cx + 2, cy + 5, cx + 4, cy + 3, 0); }
}
static const uint32_t number_color[9] = {0, 0x0000ffu, 0x008000u, 0xff0000u, 0x000080u, 0x800000u, 0x008080u, 0x000000u, 0x808080u};
static void mine_glyph(struct du_canvas *c, int cx, int cy)
{
    acc_disc(c, cx, cy, 4, 0x000000u);
    acc_line(c, cx - 6, cy, cx + 6, cy, 0); acc_line(c, cx, cy - 6, cx, cy + 6, 0);
    acc_line(c, cx - 4, cy - 4, cx + 4, cy + 4, 0); acc_line(c, cx - 4, cy + 4, cx + 4, cy - 4, 0);
    du_fill(c, (struct du_rect){cx - 2, cy - 2, 2, 2}, 0xffffffu);
}
static void flag_glyph(struct du_canvas *c, int cx, int cy)
{
    for (int i = 0; i < 4; i++) du_fill(c, (struct du_rect){cx - 2 + i / 2, cy - 5 + i, 6 - i, 1}, 0xff0000u);   /* the pennant */
    du_fill(c, (struct du_rect){cx, cy - 1, 1, 5}, 0x000000u);
    du_fill(c, (struct du_rect){cx - 3, cy + 4, 7, 2}, 0x000000u);
}
static void cell_paint(struct du_canvas *c, const struct mines *m, int x, int y, struct du_rect r, int pressed)
{
    uint8_t v = m->cell[y][x];
    int lost = m->state == MINES_LOST;
    if ((v & MINES_CELL_OPEN) || pressed) {
        du_fill(c, r, (v & MINES_CELL_BOOM) ? 0xff0000u : 0xc0c0c0u);
        du_outline(c, r, 0x808080u);
        if (v & MINES_CELL_OPEN) {
            if (v & MINES_CELL_MINE) mine_glyph(c, r.x + 8, r.y + 8);
            else {
                int n = mines_neighbours(m, x, y);
                if (n) { char s[2] = {(char)('0' + n), 0}; du_text(c, r.x + 5, r.y + 2, s, number_color[n]); }
            }
        }
        return;
    }
    du_fill(c, r, 0xc0c0c0u);
    du_edge(c, r, DU_RAISED_THIN);
    if (v & MINES_CELL_FLAG) flag_glyph(c, r.x + 8, r.y + 8);
    else if (v & MINES_CELL_MARK) du_text(c, r.x + 5, r.y + 2, "?", 0x000000u);
    if (lost && (v & MINES_CELL_WRONG)) {
        du_fill(c, r, 0xc0c0c0u); du_outline(c, r, 0x808080u);
        mine_glyph(c, r.x + 8, r.y + 8);
        acc_line(c, r.x + 2, r.y + 2, r.x + 13, r.y + 13, 0xff0000u); acc_line(c, r.x + 13, r.y + 2, r.x + 2, r.y + 13, 0xff0000u);
    }
}
void mines_paint(struct du_ui *u, void *user)
{
    struct mines_app *a = user; struct du_canvas *c = &u->canvas; const struct mines *m = &a->game;
    du_fill(c, (struct du_rect){0, PANEL_Y - 2, u->canvas.width, u->canvas.height - PANEL_Y + 2}, DU_FACE);
    struct du_rect b = mines_board_rect(m);
    struct du_rect frame = {b.x - 6, PANEL_Y - 2, b.w + 12, BOARD_Y - PANEL_Y + b.h + 8};
    du_edge(c, frame, DU_RAISED);
    du_edge(c, (struct du_rect){b.x - 3, PANEL_Y + 2, b.w + 6, PANEL_H}, DU_SUNKEN);
    du_edge(c, (struct du_rect){b.x - 3, BOARD_Y - 3, b.w + 6, b.h + 6}, DU_SUNKEN);
    led(c, led_rect(m, 0), mines_remaining(m));
    led(c, led_rect(m, 1), m->seconds);
    face(c, a);
    for (int y = 0; y < m->h; y++) for (int x = 0; x < m->w; x++) {
        struct du_rect r = {b.x + x * MINES_CELL, b.y + y * MINES_CELL, MINES_CELL, MINES_CELL};
        int pressed = a->holding && x == a->hold_x && y == a->hold_y && !(m->cell[y][x] & (MINES_CELL_FLAG | MINES_CELL_OPEN));
        cell_paint(c, m, x, y, r, pressed);
    }
}
static const struct du_menu_item game_template[] = {
    {"&New", MN_NEW, DU_KEY_F2, 0, 0}, {"", 0, 0, 0, DU_ITEM_SEPARATOR},
    {"&Beginner", MN_BEGINNER, 0, 0, DU_ITEM_RADIO}, {"&Intermediate", MN_INTERMEDIATE, 0, 0, DU_ITEM_RADIO},
    {"&Expert", MN_EXPERT, 0, 0, DU_ITEM_RADIO}, {"&Custom...", MN_CUSTOM, 0, 0, DU_ITEM_RADIO},
    {"", 0, 0, 0, DU_ITEM_SEPARATOR}, {"&Marks (?)", MN_MARKS, 0, 0, 0}, {"", 0, 0, 0, DU_ITEM_SEPARATOR},
    {"Best &Times...", MN_BEST, 0, 0, 0}, {"", 0, 0, 0, DU_ITEM_SEPARATOR}, {"E&xit", MN_EXIT, 0, 0, 0} };
static const struct du_menu_item help_template[] = { {"&Help Topics", MN_HELP, DU_KEY_F1, 0, 0}, {"&About Minesweeper", MN_ABOUT, 0, 0, 0} };
static void sync_menu(struct mines_app *a)
{
    for (int i = 0; i < 4; i++) a->game_items[2 + i].flags = DU_ITEM_RADIO | (a->game.level == i ? DU_ITEM_CHECKED : 0);
    a->game_items[7].flags = a->game.marks_enabled ? DU_ITEM_CHECKED : 0;
}
void mines_attach(struct mines_app *a, struct du_ui *u)
{
    a->ui = u; u->event = mines_event; u->user = a; u->paint = mines_paint;
    if (!a->game.w) mines_init(&a->game, 1);
    a->game.view_w = u->canvas.width;
    int w, h;
    acc_window_size(MINES_W, MINES_H, &w, &h);
    a->game.max_h = (h - BOARD_Y - 10) / MINES_CELL; if (a->game.max_h > MINES_MAX_H) a->game.max_h = MINES_MAX_H;
    a->game.max_w = (w - 20) / MINES_CELL; if (a->game.max_w > MINES_MAX_W) a->game.max_w = MINES_MAX_W;
    if (a->best_path) mines_best_load(&a->game, a->best_path);
    memcpy(a->game_items, game_template, sizeof(game_template));
    memcpy(a->help_items, help_template, sizeof(help_template));
    a->menus[0] = (struct du_menu){"&Game", a->game_items, 12};
    a->menus[1] = (struct du_menu){"&Help", a->help_items, 2};
    u->menus = a->menus; u->menu_count = 2;
    du_add(u, DU_CANVAS, MN_CANVAS, (struct du_rect){0, PANEL_Y - 2, u->canvas.width, u->canvas.height - PANEL_Y + 2}, "");
    sync_menu(a); u->focus = -1; u->dirty = 1;
}
static void restart(struct mines_app *a);
static void level_changed(struct mines_app *a)
{
    int w, h;
    mines_window_size(&a->game, &w, &h);
    if (a->relayout && (w != a->ui->canvas.width || h != a->ui->canvas.height)) {
        mines_new(&a->game); a->holding = 0;
        a->relayout(a->host, w, h);
        return;
    }
    restart(a);
}
static void restart(struct mines_app *a)
{
    mines_new(&a->game);
    du_timer_stop(a->ui, MN_TIMER);
    a->holding = 0;
    du_invalidate(a->ui, (struct du_rect){0, 0, a->ui->canvas.width, a->ui->canvas.height});
}
static int cell_at(const struct mines_app *a, int px, int py, int *x, int *y)
{
    struct du_rect b = mines_board_rect(&a->game);
    if (!du_contains(b, px, py)) return 0;
    *x = (px - b.x) / MINES_CELL; *y = (py - b.y) / MINES_CELL;
    return 1;
}
static void after_move(struct mines_app *a, enum mines_state before)
{
    struct mines *m = &a->game;
    if (before == MINES_READY && m->state == MINES_PLAYING) du_timer_start(a->ui, MN_TIMER, 1000000000ull, 1);
    if (m->state == MINES_WON || m->state == MINES_LOST) du_timer_stop(a->ui, MN_TIMER);
    if (m->state == MINES_WON && before != MINES_WON && mines_qualifies(m)) {
        int level = mines_record(m, "Anonymous");
        if (a->best_path) mines_best_save(m, a->best_path);
        char text[128];
        snprintf(text, sizeof(text), "You have the fastest time for %s level (%d seconds).", level == 0 ? "beginner" : level == 1 ? "intermediate" : "expert", m->seconds);
        du_message(a->ui, MN_BEST, "Minesweeper", text, DU_DLG_ICON_INFO, NULL, 0, 0, 0);
    }
    du_invalidate(a->ui, (struct du_rect){0, PANEL_Y - 2, a->ui->canvas.width, a->ui->canvas.height - PANEL_Y + 2});
}
static void show_best(struct mines_app *a)
{
    char text[400]; const struct mines_best *b = &a->game.best;
    snprintf(text, sizeof(text), "Beginner:\t%d seconds\t%s\nIntermediate:\t%d seconds\t%s\nExpert:\t%d seconds\t%s",
             b->seconds[0], b->name[0], b->seconds[1], b->name[1], b->seconds[2], b->name[2]);
    du_message(a->ui, MN_BEST, "Fastest Mine Sweepers", text, DU_DLG_ICON_NONE, NULL, 0, 0, 0);
}
static void pointer(struct mines_app *a, const struct du_event *e)
{
    struct mines *m = &a->game; struct du_ui *u = a->ui;
    int x = 0, y = 0, in = cell_at(a, e->x, e->y, &x, &y), button = (int)e->key;
    enum mines_state before = m->state;
    if (e->index == DU_POINTER_CANCEL) { a->holding = 0; a->pressed_face = 0; du_invalidate(u, mines_face_rect(m)); return; }
    if (e->index == DU_POINTER_DOWN) {
        if (button == 1 && du_contains(mines_face_rect(m), e->x, e->y)) { a->pressed_face = 1; du_invalidate(u, mines_face_rect(m)); return; }
        if (m->state == MINES_WON || m->state == MINES_LOST) return;
        if (!in) return;
        if (button == 2) {
            if (u->buttons & 1u) { mines_chord(m, x, y); after_move(a, before); }
            else { mines_flag(m, x, y); du_invalidate(u, (struct du_rect){0, PANEL_Y, u->canvas.width, BOARD_Y - PANEL_Y + m->h * MINES_CELL}); }
            return;
        }
        a->holding = button == 1 || button == 3; a->hold_x = x; a->hold_y = y;
        du_invalidate(u, mines_face_rect(m)); du_invalidate(u, mines_board_rect(m));
    } else if (e->index == DU_POINTER_MOVE) {
        if (a->pressed_face) return;
        if (a->holding && (in ? (x != a->hold_x || y != a->hold_y) : 1)) {
            a->hold_x = in ? x : -1; a->hold_y = in ? y : -1;
            du_invalidate(u, mines_board_rect(m));
        }
    } else if (e->index == DU_POINTER_UP) {
        if (a->pressed_face) {
            int over = du_contains(mines_face_rect(m), e->x, e->y);
            a->pressed_face = 0;
            if (over) restart(a); else du_invalidate(u, mines_face_rect(m));
            return;
        }
        if (!a->holding) return;
        a->holding = 0;
        if (in) {
            if (button == 3 || (u->buttons & 2u)) mines_chord(m, x, y);
            else if (button == 1) mines_open(m, x, y);
        }
        after_move(a, before);
    }
}
void mines_event(struct du_ui *u, const struct du_event *e, void *user)
{
    struct mines_app *a = user;
    switch (e->type) {
    case DU_EV_POINTER: if (e->id == MN_CANVAS) pointer(a, e); return;
    case DU_EV_TICK:
        if (e->id == MN_TIMER) { int before = a->game.seconds; mines_tick(&a->game); if (a->game.seconds != before) du_invalidate(u, led_rect(&a->game, 1)); }
        return;
    case DU_EV_KEY: if (e->key == DU_KEY_F2) { restart(a); } return;
    case DU_EV_DIALOG:
        if (e->id == MN_CUSTOM_BOX && e->index == 0 && e->text) {
            int h, w, n; char extra;
            if (sscanf(e->text, "%d %d %d %c", &h, &w, &n, &extra) == 3) {
                mines_set_level(&a->game, MINES_CUSTOM, w, h, n);   /* clamped like Windows: 9-30 wide, 9-24 high (what the window shows), 10 to (w-1)(h-1) mines */
                sync_menu(a); level_changed(a);
            } else du_message(u, MN_CUSTOM_BOX + 1, "Minesweeper", "Enter three numbers: height, width and mines.", DU_DLG_ICON_WARNING, NULL, 0, 0, 0);
        }
        return;
    case DU_EV_ACTIVATE:
        if (e->id == MN_NEW) restart(a);
        else if (e->id >= MN_BEGINNER && e->id <= MN_EXPERT) {
            mines_set_level(&a->game, (enum mines_level)(e->id - MN_BEGINNER), 0, 0, 0); sync_menu(a); level_changed(a);
        } else if (e->id == MN_CUSTOM) {   /* Windows asks for the height, width and mines of a custom field */
            char initial[24]; const struct mines *m = &a->game;
            snprintf(initial, sizeof(initial), "%d %d %d", m->level == MINES_CUSTOM ? m->h : 16, m->level == MINES_CUSTOM ? m->w : 16, m->level == MINES_CUSTOM ? m->mines : 40);
            du_input_box(u, MN_CUSTOM_BOX, "Custom Field", "Height, width and mines, for example 16 30 99:", initial);
        } else if (e->id == MN_MARKS) { a->game.marks_enabled = !a->game.marks_enabled; sync_menu(a); }
        else if (e->id == MN_BEST) show_best(a);
        else if (e->id == MN_EXIT) { if (a->quit) a->quit(a->host); }
        else if (e->id == MN_HELP)
            du_message(u, 1, "Minesweeper Help", "Left click opens a square, right click flags it (twice for ? with Marks).\nBoth buttons, or the middle button, on a number opens its neighbours when the flags match.\nOpen every square without a mine to win. The first click is always safe.\nF2 starts a new game.", DU_DLG_ICON_INFO, NULL, 0, 0, 0);
        else if (e->id == MN_ABOUT)
            du_message(u, 2, "About Minesweeper", "CiukiOS Minesweeper - Classic computing, one woof at a time.\nCode: MIT.", DU_DLG_ICON_INFO, NULL, 0, 0, 0);
        return;
    default: return;
    }
}
