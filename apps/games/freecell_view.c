/* SPDX-License-Identifier: MIT
 * FreeCell window: free cells, foundations and eight columns; select then
 * move, or drag; double and right click send a card on; the Game menu. */
#include "freecell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TOP_Y 30
#define COL_Y 112
#define PITCH 72
#define GAP 16
#define DOUBLE_NS 500000000ull

static int col_x(int col) { return (FC_W - FC_COLS * PITCH) / 2 + col * PITCH; }
static int gap(const struct fc_app *a, int col)
{
    int n = a->game.ncol[col], avail = FC_H - COL_Y - CARD_H - 26;
    if (n > 1 && (n - 1) * GAP > avail) return avail / (n - 1) > 6 ? avail / (n - 1) : 6;
    return GAP;
}
struct du_rect fc_loc_rect(const struct fc_app *a, int loc, int index)
{
    if (loc < FC_COLS) return (struct du_rect){col_x(loc), COL_Y + (index > 0 ? index : 0) * gap(a, loc), CARD_W, CARD_H};
    if (loc < FC_FOUND0) return (struct du_rect){col_x(loc - FC_CELL0), TOP_Y, CARD_W, CARD_H};
    return (struct du_rect){col_x(4 + loc - FC_FOUND0), TOP_Y, CARD_W, CARD_H};
}
int fc_hit(const struct fc_app *a, int x, int y, int *loc, int *index)
{
    for (int l = 0; l < FC_FOUND0 + FC_FOUND; l++) {
        if (l < FC_COLS) {
            int n = a->game.ncol[l], g = gap(a, l);
            for (int i = n - 1; i >= 0; i--) {
                struct du_rect r = fc_loc_rect(a, l, i);
                if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + (i == n - 1 ? CARD_H : g)) { *loc = l; *index = i; return 1; }
            }
            if (!n && du_contains(fc_loc_rect(a, l, 0), x, y)) { *loc = l; *index = -1; return 1; }
        } else if (du_contains(fc_loc_rect(a, l, 0), x, y)) {
            *loc = l; *index = fc_card_at(&a->game, l, 0) == CARD_NONE ? -1 : 0; return 1;
        }
    }
    return 0;
}
static void status(struct fc_app *a)
{
    char text[128]; const struct freecell *g = &a->game;
    snprintf(text, sizeof(text), "Game #%u   Moves: %d   Wins: %d  Losses: %d", g->game, g->moves, g->wins, g->losses);
    du_set_text(du_get(a->ui, FCV_STATUS), text);
    du_invalidate_widget(a->ui, FCV_STATUS);
}
void fc_paint(struct du_ui *u, void *user)
{
    struct fc_app *a = user; struct du_canvas *c = &u->canvas; const struct freecell *g = &a->game;
    du_fill(c, (struct du_rect){0, 22, c->width, c->height - 42}, TABLE_GREEN);
    for (int l = FC_CELL0; l < FC_FOUND0 + FC_FOUND; l++) {
        struct du_rect r = fc_loc_rect(a, l, 0);
        card_draw_slot(c, r.x, r.y, l >= FC_FOUND0 ? "A" : "");
        card_t card = fc_card_at(g, l, 0);
        if (card != CARD_NONE && !(a->dragging > 0 && a->drag_loc == l)) card_draw(c, r.x, r.y, card, a->sel_loc == l);
    }
    for (int col = 0; col < FC_COLS; col++) {
        struct du_rect s = fc_loc_rect(a, col, 0);
        card_draw_slot(c, s.x, s.y, "");
        for (int i = 0; i < g->ncol[col]; i++) {
            if (a->dragging > 0 && a->drag_loc == col && i >= a->drag_index) break;
            struct du_rect r = fc_loc_rect(a, col, i);
            card_draw(c, r.x, r.y, g->col[col][i], a->sel_loc == col && i >= a->sel_index);
        }
    }
    if (a->dragging > 0) {
        int n = a->drag_loc < FC_COLS ? g->ncol[a->drag_loc] - a->drag_index : 1;
        for (int i = 0; i < n; i++)
            card_draw(c, a->drag_x - a->off_x, a->drag_y - a->off_y + i * GAP, fc_card_at(g, a->drag_loc, a->drag_index + i), 0);
    }
}
static const struct du_menu_item game_template[] = {
    {"&New Game", FCV_NEW, DU_KEY_F2, 0, 0}, {"&Select Game...", FCV_SELECT, 0x3d, 0, 0}, {"&Restart Game", FCV_RESTART, 0, 0, 0},
    {"&Undo", FCV_UNDO, DU_KEY_F10, 0, DU_ITEM_DISABLED}, {"", 0, 0, 0, DU_ITEM_SEPARATOR}, {"E&xit", FCV_EXIT, 0, 0, 0} };
static const struct du_menu_item help_template[] = { {"&Help Topics", FCV_HELP, DU_KEY_F1, 0, 0}, {"&About FreeCell", FCV_ABOUT, 0, 0, 0} };
void fc_attach(struct fc_app *a, struct du_ui *u)
{
    a->ui = u; u->event = fc_event; u->user = a; u->paint = fc_paint;
    if (!a->game.game) fc_deal(&a->game, 1);
    memcpy(a->game_items, game_template, sizeof(game_template));
    memcpy(a->help_items, help_template, sizeof(help_template));
    a->menus[0] = (struct du_menu){"&Game", a->game_items, 6};
    a->menus[1] = (struct du_menu){"&Help", a->help_items, 2};
    u->menus = a->menus; u->menu_count = 2;
    du_add(u, DU_CANVAS, FCV_CANVAS, (struct du_rect){0, 22, u->canvas.width, u->canvas.height - 42}, "");
    du_add(u, DU_STATUS, FCV_STATUS, (struct du_rect){0, u->canvas.height - 20, u->canvas.width, 20}, "");
    a->sel_loc = -1; status(a); u->focus = -1; u->dirty = 1;
}
static void redraw(struct fc_app *a)
{
    a->game_items[3].flags = a->game.nhistory && !a->game.won ? 0 : DU_ITEM_DISABLED;
    status(a);
    du_invalidate(a->ui, (struct du_rect){0, 22, a->ui->canvas.width, a->ui->canvas.height - 42});
}
static void start(struct fc_app *a, unsigned game)
{
    if (game) fc_deal(&a->game, game); else fc_restart(&a->game);
    a->sel_loc = -1; a->dragging = 0; redraw(a);
}
static unsigned random_game(struct fc_app *a)
{
    uint32_t x = a->game.game * 2654435761u + (uint32_t)a->game.moves * 97u + 12345u + (uint32_t)a->ui->now;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x % FC_GAME_MAX + 1;
}
static void after_move(struct fc_app *a, int was_won)
{
    redraw(a);
    if (a->game.won && !was_won) du_message(a->ui, FCV_NEW, "FreeCell", "Congratulations! You won.\nDeal another game?", DU_DLG_ICON_INFO, NULL, 0, 0, 0);
    else if (!a->game.won && !fc_has_moves(&a->game)) {
        a->game.losses++; status(a);
        du_message(a->ui, FCV_NEW, "FreeCell", "Sorry, you lose: there are no more moves.", DU_DLG_ICON_WARNING, NULL, 0, 0, 0);
    }
}
static int run_count(const struct fc_app *a, int loc, int index)
{
    return loc < FC_COLS ? a->game.ncol[loc] - index : 1;
}
static void pointer(struct fc_app *a, const struct du_event *e)
{
    struct freecell *g = &a->game; int loc, index, hit = fc_hit(a, e->x, e->y, &loc, &index), won = g->won, button = (int)e->key;
    if (e->index == DU_POINTER_CANCEL) { a->dragging = 0; redraw(a); return; }
    if (e->index == DU_POINTER_DOWN) {
        if (g->won || !hit) { if (!hit && a->sel_loc >= 0) { a->sel_loc = -1; redraw(a); } return; }
        if (button == 2) { if (index >= 0 && loc < FC_FOUND0 && !fc_quick(g, loc)) { a->sel_loc = -1; after_move(a, won); } return; }
        if (button != 1) return;
        a->drag_loc = loc; a->drag_index = index; a->dragging = 0;
        struct du_rect r = fc_loc_rect(a, loc, index);
        a->off_x = e->x - r.x; a->off_y = e->y - r.y; a->drag_x = e->x; a->drag_y = e->y;
        a->dragging = -1;
    } else if (e->index == DU_POINTER_MOVE) {
        if (a->dragging) {
            if (a->dragging < 0 && (abs(e->x - a->drag_x) > 3 || abs(e->y - a->drag_y) > 3) && a->drag_index >= 0 && a->drag_loc < FC_FOUND0 &&
                (a->drag_loc >= FC_COLS || fc_run_ok(g, a->drag_loc, run_count(a, a->drag_loc, a->drag_index)))) a->dragging = 1;
            if (a->dragging > 0) { a->drag_x = e->x; a->drag_y = e->y; redraw(a); }
        }
    } else if (e->index == DU_POINTER_UP) {
        int was = a->dragging; a->dragging = 0;
        if (button != 1 || !was) return;
        if (was > 0) {                                  /* a drop */
            if (hit && loc != a->drag_loc && !fc_move(g, a->drag_loc, run_count(a, a->drag_loc, a->drag_index), loc)) { a->sel_loc = -1; after_move(a, won); return; }
            redraw(a); return;
        }
        /* A click: a double click sends the card on; else select, or move the selection here. */
        uint64_t now = a->ui->now;
        if (a->last_loc == a->drag_loc && a->last_index == a->drag_index && a->last_ns && now - a->last_ns <= DOUBLE_NS) {
            a->last_ns = 0; a->sel_loc = -1;
            if (a->drag_index >= 0 && a->drag_loc < FC_FOUND0 && !fc_quick(g, a->drag_loc)) { after_move(a, won); return; }
        }
        a->last_loc = a->drag_loc; a->last_index = a->drag_index; a->last_ns = now ? now : 1;
        if (a->sel_loc >= 0) {
            int from = a->sel_loc, count = run_count(a, from, a->sel_index);
            a->sel_loc = -1;
            if (hit && loc != from && !fc_move(g, from, count, loc)) { a->last_ns = 0; after_move(a, won); return; }
            if (hit && loc == from) { redraw(a); return; }
        }
        if (hit && index >= 0 && loc < FC_FOUND0 && (loc >= FC_COLS || fc_run_ok(g, loc, g->ncol[loc] - index))) { a->sel_loc = loc; a->sel_index = index; }
        redraw(a);
    }
}
void fc_event(struct du_ui *u, const struct du_event *e, void *user)
{
    struct fc_app *a = user;
    switch (e->type) {
    case DU_EV_POINTER: if (e->id == FCV_CANVAS) pointer(a, e); return;
    case DU_EV_DIALOG:
        if (e->id == FCV_SELECT && e->index == 0 && e->text) {
            char *end; long n = strtol(e->text, &end, 10);
            if (end != e->text && !*end && n >= 1 && n <= FC_GAME_MAX) start(a, (unsigned)n);
            else du_message(u, FCV_PICK, "FreeCell", "Enter a game number from 1 to 32000.", DU_DLG_ICON_WARNING, NULL, 0, 0, 0);
        } else if (e->id == FCV_NEW && e->index == 0 && (a->game.won || !fc_has_moves(&a->game))) start(a, random_game(a));
        return;
    case DU_EV_ACTIVATE:
        if (e->id == FCV_NEW) start(a, random_game(a));
        else if (e->id == FCV_SELECT) { char n[16]; snprintf(n, sizeof(n), "%u", a->game.game); du_input_box(u, FCV_SELECT, "Game Number", "Game number (1 - 32000):", n); }
        else if (e->id == FCV_RESTART) start(a, 0);
        else if (e->id == FCV_UNDO) { fc_undo(&a->game); a->sel_loc = -1; redraw(a); }
        else if (e->id == FCV_EXIT) { if (a->quit) a->quit(a->host); }
        else if (e->id == FCV_HELP)
            du_message(u, 1, "FreeCell Help", "Build the four foundations from ace to king in suit.\nMove one card at a time to a free cell, or a run of alternating colours down a column; the run length is limited by the free cells and empty columns.\nClick a card, then where it goes, or drag it. Double click or right click sends a card on.\nF2 starts a new game, F3 selects a game number, F10 undoes.", DU_DLG_ICON_INFO, NULL, 0, 0, 0);
        else if (e->id == FCV_ABOUT)
            du_message(u, 2, "About FreeCell", "CiukiOS FreeCell - Classic computing, one woof at a time.\nGame numbers 1 - 32000 deal like Windows FreeCell.\nCode: MIT.", DU_DLG_ICON_INFO, NULL, 0, 0, 0);
        return;
    default: return;
    }
}
