/* SPDX-License-Identifier: MIT
 * Solitaire window: the table, drag and drop of card runs, the stock click,
 * double and right click to the foundation, the Game menu and the status bar. */
#include "solitaire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TOP_Y 30
#define TAB_Y 112
#define PITCH 72
#define GAP_DOWN 5
#define GAP_UP 16
#define DOUBLE_NS 500000000ull

static int col_x(int col) { return (SOL_W - 7 * PITCH) / 2 + col * PITCH; }
static int pile_col(int pile)
{
    if (pile == SOL_STOCK) return 0;
    if (pile == SOL_WASTE) return 1;
    if (pile >= SOL_FOUND0 && pile <= SOL_FOUND3) return 3 + pile - SOL_FOUND0;
    return pile - SOL_TAB0;
}
static int is_tab(int pile) { return pile >= SOL_TAB0 && pile <= SOL_TAB6; }
/* Vertical offset of a tableau card: face-down cards are packed tighter, and a long run is compressed to fit the window. */
static int tab_offset(const struct sol_app *a, int pile, int index)
{
    const struct sol_pile *p = &a->game.s.pile[pile];
    int avail = SOL_H - TAB_Y - CARD_H - 26;
    int down = p->first_up < p->count ? p->first_up : p->count, up = p->count - down;
    int need = down * GAP_DOWN + (up > 0 ? (up - 1) * GAP_UP : 0), gap_up = GAP_UP, gap_down = GAP_DOWN;
    if (need > avail && p->count > 1) {
        gap_down = GAP_DOWN > 3 ? 3 : GAP_DOWN;
        need = down * gap_down + (up > 0 ? (up - 1) * GAP_UP : 0);
        if (need > avail && up > 1) gap_up = (avail - down * gap_down) / (up - 1);
        if (gap_up < 4) gap_up = 4;
    }
    int off = 0;
    for (int i = 0; i < index && i < p->count; i++) off += i < p->first_up ? gap_down : gap_up;
    return off;
}
struct du_rect sol_pile_rect(const struct sol_app *a, int pile)
{
    (void)a;
    return (struct du_rect){col_x(pile_col(pile)), is_tab(pile) ? TAB_Y : TOP_Y, CARD_W, CARD_H};
}
struct du_rect sol_card_rect(const struct sol_app *a, int pile, int index)
{
    struct du_rect r = sol_pile_rect(a, pile);
    if (is_tab(pile)) r.y += tab_offset(a, pile, index);
    else if (pile == SOL_WASTE && a->game.draw_three) {
        /* the last three cards of the waste are fanned to the right */
        int n = a->game.s.pile[SOL_WASTE].count, from = n > 3 ? n - 3 : 0;
        if (index > from) r.x += (index - from) * 14;
    }
    return r;
}
int sol_hit(const struct sol_app *a, int x, int y, int *pile, int *index)
{
    for (int p = SOL_PILES - 1; p >= 0; p--) {
        const struct sol_pile *pp = &a->game.s.pile[p];
        for (int i = pp->count - 1; i >= 0; i--) {
            if (p == SOL_STOCK && i != pp->count - 1) continue;
            struct du_rect r = sol_card_rect(a, p, i);
            if (!is_tab(p) && p != SOL_WASTE && i != pp->count - 1) continue;
            /* inside the visible strip of a covered card, or the whole top card */
            int bottom = i == pp->count - 1 ? r.h : (is_tab(p) ? tab_offset(a, p, i + 1) - tab_offset(a, p, i) : 14);
            if (x >= r.x && x < r.x + (p == SOL_WASTE && a->game.draw_three && i < pp->count - 1 ? 14 : r.w) && y >= r.y && y < r.y + bottom) { *pile = p; *index = i; return 1; }
        }
        struct du_rect slot = sol_pile_rect(a, p);
        if (!pp->count && du_contains(slot, x, y)) { *pile = p; *index = -1; return 1; }
    }
    return 0;
}
static void status(struct sol_app *a)
{
    char text[96]; const struct solitaire *g = &a->game;
    if (g->scoring == SOL_SCORE_NONE) snprintf(text, sizeof(text), "%sNo scoring   Moves: %d", g->won ? "You won!   " : "", g->s.moves);
    else snprintf(text, sizeof(text), "%sScore: %d", g->won ? "You won!   " : "", g->s.score);
    if (g->timed) { size_t n = strlen(text); snprintf(text + n, sizeof(text) - n, "   Time: %d", g->s.seconds); }
    du_set_text(du_get(a->ui, 20), text);
    du_invalidate_widget(a->ui, 20);
}
void sol_paint(struct du_ui *u, void *user)
{
    struct sol_app *a = user; struct du_canvas *c = &u->canvas; const struct solitaire *g = &a->game;
    du_fill(c, (struct du_rect){0, 22, c->width, c->height - 22 - 20}, TABLE_GREEN);
    for (int p = 0; p < SOL_PILES; p++) {
        const struct sol_pile *pp = &g->s.pile[p];
        struct du_rect slot = sol_pile_rect(a, p);
        card_draw_slot(c, slot.x, slot.y, p == SOL_STOCK ? (g->s.pile[SOL_WASTE].count ? "O" : "") : (p >= SOL_FOUND0 && p <= SOL_FOUND3 ? "A" : ""));
        int first = 0;
        if (p == SOL_WASTE && g->draw_three) first = pp->count > 3 ? pp->count - 3 : 0;
        else if (p == SOL_STOCK || (p >= SOL_FOUND0 && p <= SOL_FOUND3) || p == SOL_WASTE) first = pp->count ? pp->count - 1 : 0;
        for (int i = first; i < pp->count; i++) {
            if (a->dragging && p == a->drag_pile && i >= a->drag_index) break;
            struct du_rect r = sol_card_rect(a, p, i);
            if (sol_face_up(pp, i)) card_draw(c, r.x, r.y, pp->cards[i], 0); else card_draw_back(c, r.x, r.y);
        }
    }
    if (a->dragging) {
        const struct sol_pile *pp = &g->s.pile[a->drag_pile];
        for (int i = a->drag_index; i < pp->count; i++)
            card_draw(c, a->drag_x - a->off_x, a->drag_y - a->off_y + (i - a->drag_index) * GAP_UP, pp->cards[i], 0);
    }
}
static const struct du_menu_item game_template[] = {
    {"&Deal", SL_DEAL, DU_KEY_F2, 0, 0}, {"&Undo", SL_UNDO, 0x2c, DU_CTRL, DU_ITEM_DISABLED}, {"", 0, 0, 0, DU_ITEM_SEPARATOR},
    {"Draw &One", SL_DRAW1, 0, 0, DU_ITEM_RADIO | DU_ITEM_CHECKED}, {"Draw &Three", SL_DRAW3, 0, 0, DU_ITEM_RADIO},
    {"", 0, 0, 0, DU_ITEM_SEPARATOR}, {"&Standard scoring", SL_SCORING, 0, 0, DU_ITEM_CHECKED}, {"T&imed game", SL_TIMED, 0, 0, 0},
    {"", 0, 0, 0, DU_ITEM_SEPARATOR}, {"E&xit", SL_EXIT, 0, 0, 0} };
static const struct du_menu_item help_template[] = { {"&Help Topics", SL_HELP, DU_KEY_F1, 0, 0}, {"&About Solitaire", SL_ABOUT, 0, 0, 0} };
static void sync_menu(struct sol_app *a)
{
    const struct solitaire *g = &a->game;
    a->game_items[1].flags = g->have_undo ? 0 : DU_ITEM_DISABLED;
    a->game_items[3].flags = DU_ITEM_RADIO | (g->draw_three ? 0 : DU_ITEM_CHECKED);
    a->game_items[4].flags = DU_ITEM_RADIO | (g->draw_three ? DU_ITEM_CHECKED : 0);
    a->game_items[6].flags = g->scoring == SOL_SCORE_STANDARD ? DU_ITEM_CHECKED : 0;
    a->game_items[7].flags = g->timed ? DU_ITEM_CHECKED : 0;
}
void sol_attach(struct sol_app *a, struct du_ui *u)
{
    a->ui = u; u->event = sol_event; u->user = a; u->paint = sol_paint;
    if (!a->game.s.pile[SOL_STOCK].count && !a->game.s.pile[SOL_TAB0].count) sol_init(&a->game, 1);
    memcpy(a->game_items, game_template, sizeof(game_template));
    memcpy(a->help_items, help_template, sizeof(help_template));
    a->menus[0] = (struct du_menu){"&Game", a->game_items, 10};
    a->menus[1] = (struct du_menu){"&Help", a->help_items, 2};
    u->menus = a->menus; u->menu_count = 2;
    du_add(u, DU_CANVAS, SL_CANVAS, (struct du_rect){0, 22, u->canvas.width, u->canvas.height - 42}, "");
    du_add(u, DU_STATUS, 20, (struct du_rect){0, u->canvas.height - 20, u->canvas.width, 20}, "");
    sync_menu(a); status(a); u->focus = -1; u->dirty = 1;
}
static void redraw(struct sol_app *a)
{
    sync_menu(a); status(a);
    du_invalidate(a->ui, (struct du_rect){0, 22, a->ui->canvas.width, a->ui->canvas.height - 42});
}
static void deal(struct sol_app *a)
{
    sol_deal(&a->game); a->dragging = 0;
    du_timer_stop(a->ui, SL_TIMER);
    redraw(a);
}
static void after_move(struct sol_app *a, int was_won)
{
    struct solitaire *g = &a->game;
    if (g->timed && g->s.moves == 1) du_timer_start(a->ui, SL_TIMER, 1000000000ull, 1);
    redraw(a);
    if (g->won && !was_won) {
        du_timer_stop(a->ui, SL_TIMER);
        char text[96]; snprintf(text, sizeof(text), "Congratulations, you won!\nScore: %d", g->s.score);
        du_message(a->ui, SL_DEAL, "Solitaire", text, DU_DLG_ICON_INFO, NULL, 0, 0, 0);
    }
}
static void pointer(struct sol_app *a, const struct du_event *e)
{
    struct solitaire *g = &a->game; int pile, index, hit = sol_hit(a, e->x, e->y, &pile, &index), won = g->won, button = (int)e->key;
    if (e->index == DU_POINTER_CANCEL) { a->dragging = 0; redraw(a); return; }
    if (e->index == DU_POINTER_DOWN) {
        if (g->won || !hit) return;
        if (button == 2) { if (index >= 0 && !sol_auto(g, pile)) after_move(a, won); return; }
        if (button != 1) return;
        if (pile == SOL_STOCK) { if (sol_draw(g)) after_move(a, won); return; }
        if (index < 0 || !sol_face_up(&g->s.pile[pile], index)) return;
        if (pile == SOL_WASTE && index != g->s.pile[pile].count - 1) return;
        a->drag_pile = pile; a->drag_index = index; a->dragging = 0;
        struct du_rect r = sol_card_rect(a, pile, index);
        a->off_x = e->x - r.x; a->off_y = e->y - r.y; a->drag_x = e->x; a->drag_y = e->y;
        a->dragging = -1;   /* pressed: becomes a drag once the pointer moves */
    } else if (e->index == DU_POINTER_MOVE) {
        if (a->dragging) {
            if (a->dragging < 0 && (abs(e->x - a->drag_x) > 3 || abs(e->y - a->drag_y) > 3)) a->dragging = 1;
            if (a->dragging > 0) { a->drag_x = e->x; a->drag_y = e->y; redraw(a); }
        }
    } else if (e->index == DU_POINTER_UP) {
        int was = a->dragging; a->dragging = 0;
        if (button != 1 || !was) return;
        if (was < 0) {    /* a click without movement: a double click sends the card to a foundation */
            uint64_t now = a->ui->now;
            if (a->last_pile == a->drag_pile && a->last_index == a->drag_index && now - a->last_ns <= DOUBLE_NS && a->last_ns) {
                a->last_ns = 0;
                if (!sol_auto(g, a->drag_pile)) after_move(a, won);
            } else { a->last_pile = a->drag_pile; a->last_index = a->drag_index; a->last_ns = now ? now : 1; }
            redraw(a); return;
        }
        if (hit && pile != a->drag_pile && !sol_move(g, a->drag_pile, a->drag_index, pile)) { after_move(a, won); return; }
        redraw(a);
    }
}
void sol_event(struct du_ui *u, const struct du_event *e, void *user)
{
    struct sol_app *a = user;
    switch (e->type) {
    case DU_EV_POINTER: if (e->id == SL_CANVAS) pointer(a, e); return;
    case DU_EV_TICK: if (e->id == SL_TIMER) { sol_tick(&a->game); status(a); } return;
    case DU_EV_KEY: if (e->key == DU_KEY_F2) deal(a); return;
    case DU_EV_ACTIVATE:
        if (e->id == SL_DEAL) deal(a);
        else if (e->id == SL_UNDO) { sol_undo(&a->game); redraw(a); }
        else if (e->id == SL_DRAW1 || e->id == SL_DRAW3) { a->game.draw_three = e->id == SL_DRAW3; deal(a); }
        else if (e->id == SL_SCORING) { a->game.scoring = a->game.scoring == SOL_SCORE_STANDARD ? SOL_SCORE_NONE : SOL_SCORE_STANDARD; deal(a); }
        else if (e->id == SL_TIMED) { a->game.timed = !a->game.timed; deal(a); }
        else if (e->id == SL_EXIT) { if (a->quit) a->quit(a->host); }
        else if (e->id == SL_HELP)
            du_message(u, 1, "Solitaire Help", "Drag cards, or runs of alternating colours, down the columns; build the four foundations from ace to king in suit.\nClick the stock to draw; click the empty stock to turn the waste over.\nDouble click, or right click, sends a card to a foundation.\nF2 deals again; Ctrl+Z undoes the last move.\nStandard scoring: waste to column +5, to foundation +10, turning a card +5, foundation to column -15.", DU_DLG_ICON_INFO, NULL, 0, 0, 0);
        else if (e->id == SL_ABOUT)
            du_message(u, 2, "About Solitaire", "CiukiOS Solitaire - Classic computing, one woof at a time.\nCode: MIT.", DU_DLG_ICON_INFO, NULL, 0, 0, 0);
        return;
    default: return;
    }
}
