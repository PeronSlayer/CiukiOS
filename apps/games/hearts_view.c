/* SPDX-License-Identifier: MIT
 * Hearts window: your hand at the bottom, three computer players, the trick
 * in the middle, the score board and the passing button. */
#include "hearts.h"
#include "../libdeskui/internal.h"
#include <stdio.h>
#include <string.h>
#define HAND_PITCH 24
#define STATUS_H 20
#define BACK_PITCH 12

static const char *const names[4] = {"You", "West", "North", "East"};
static int hand_y(void) { return HE_H - STATUS_H - CARD_H - 14; }
static int hand_x(int n) { return (HE_W - ((n - 1) * HAND_PITCH + CARD_W)) / 2; }
struct du_rect hearts_card_rect(const struct hearts_app *a, int p, int i)
{
    const struct hearts *h = &a->game; int n = h->nhand[p];
    if (p == 0) {
        int raise = h->phase == HEARTS_PASSING && a->marked[i] ? 14 : 0;
        return (struct du_rect){hand_x(HE_HAND) + i * HAND_PITCH, hand_y() - raise, CARD_W, CARD_H};
    }
    if (p == 2) return (struct du_rect){(HE_W - ((n > 0 ? n - 1 : 0) * 14 + CARD_W)) / 2 + i * 14, 28, CARD_W, CARD_H};
    return (struct du_rect){p == 1 ? 10 : HE_W - 10 - CARD_W, 56 + i * BACK_PITCH, CARD_W, CARD_H};
}
static struct du_rect trick_rect(int p)
{
    int cx = HE_W / 2, cy = 170;
    switch (p) {
    case 0: return (struct du_rect){cx - CARD_W / 2, cy + 6, CARD_W, CARD_H};
    case 1: return (struct du_rect){cx - CARD_W - 40, cy - CARD_H / 2 + 6, CARD_W, CARD_H};
    case 2: return (struct du_rect){cx - CARD_W / 2, cy - CARD_H - 4, CARD_W, CARD_H};
    default: return (struct du_rect){cx + 40, cy - CARD_H / 2 + 6, CARD_W, CARD_H};
    }
}
static const char *direction_text(enum hearts_direction d)
{
    return d == HEARTS_LEFT ? "left" : d == HEARTS_RIGHT ? "right" : d == HEARTS_ACROSS ? "across" : "none";
}
static int marked_count(const struct hearts_app *a) { int n = 0; for (int i = 0; i < HE_HAND; i++) n += a->marked[i]; return n; }
static void status(struct hearts_app *a)
{
    char text[128]; const struct hearts *h = &a->game;
    switch (h->phase) {
    case HEARTS_PASSING: snprintf(text, sizeof(text), "Select three cards to pass %s, then click Pass.", direction_text(hearts_direction(h))); break;
    case HEARTS_PLAYING: snprintf(text, sizeof(text), h->turn == 0 ? "Your turn." : "%s is playing...", names[h->turn >= 0 ? h->turn : 0]); break;
    case HEARTS_TRICK_DONE: snprintf(text, sizeof(text), "%s takes the trick.", names[h->trick_player[0]]); break;
    case HEARTS_ROUND_END: snprintf(text, sizeof(text), "Round over."); break;
    default: snprintf(text, sizeof(text), "Game over: %s won.", names[hearts_winner(h)]); break;
    }
    du_set_text(du_get(a->ui, HV_STATUS), text);
    du_invalidate_widget(a->ui, HV_STATUS);
}
static void update_button(struct hearts_app *a)
{
    struct du_widget *b = du_get(a->ui, HV_PASS);
    int show = a->game.phase == HEARTS_PASSING;
    b->hidden = !show; b->disabled = marked_count(a) != 3;
    du_invalidate(a->ui, b->box);
}
void hearts_paint(struct du_ui *u, void *user)
{
    struct hearts_app *a = user; struct du_canvas *c = &u->canvas; const struct hearts *h = &a->game;
    du_fill(c, (struct du_rect){0, 22, c->width, c->height - 22 - STATUS_H}, TABLE_GREEN);
    for (int p = 0; p < 4; p++) {
        for (int i = 0; i < h->nhand[p]; i++) {
            struct du_rect r = hearts_card_rect(a, p, i);
            if (p == 0) card_draw(c, r.x, r.y, h->hand[0][i], h->phase == HEARTS_PASSING && a->marked[i]);
            else card_draw_back(c, r.x, r.y);
        }
    }
    for (int i = 0; i < h->trick_count; i++) {
        struct du_rect r = trick_rect(h->trick_player[i]);
        card_draw(c, r.x, r.y, h->trick[i], 0);
    }
    /* Names with the points of the round and the game scores. */
    static const int nx[4] = {20, 10, 424, 540}, ny[4] = {HE_H - STATUS_H - CARD_H - 6, 30, 32, 30};
    for (int p = 0; p < 4; p++) {
        char text[40]; snprintf(text, sizeof(text), "%s  %d", names[p], h->score[p]);
        du_text(c, nx[p], ny[p], text, 0xffffffu);
        if (h->taken[p]) { snprintf(text, sizeof(text), "(+%d)", h->taken[p]); du_text(c, nx[p], ny[p] + 14, text, 0xffff00u); }
    }
    /* The table is painted after the widgets, so the Pass button is drawn again on top of it. */
    struct du_widget *pass = du_get(u, HV_PASS);
    if (pass && !pass->hidden) {
        struct du_rect saved = c->clip;
        c->clip = du_intersection(saved, pass->box);
        if (c->clip.w > 0 && c->clip.h > 0) du_draw_widget(u, pass, u->focus >= 0 && &u->widgets[u->focus] == pass && !u->dialog.kind);
        c->clip = saved;
    }
}
static const struct du_menu_item game_template[] = {
    {"&New Game", HV_NEW, DU_KEY_F2, 0, 0}, {"&Scores...", HV_SCORES, 0, 0, 0}, {"", 0, 0, 0, DU_ITEM_SEPARATOR}, {"E&xit", HV_EXIT, 0, 0, 0} };
static const struct du_menu_item help_template[] = { {"&Help Topics", HV_HELP, DU_KEY_F1, 0, 0}, {"&About Hearts", HV_ABOUT, 0, 0, 0} };
static void computers_choose_pass(struct hearts_app *a)
{
    for (int p = 1; p < 4; p++) { card_t c[3]; hearts_ai_pass(&a->game, p, c); hearts_set_pass(&a->game, p, c); }
}
static void begin_round(struct hearts_app *a)
{
    memset(a->marked, 0, sizeof(a->marked)); a->shown_round_end = 0;
    if (a->game.phase == HEARTS_PASSING) computers_choose_pass(a);
    update_button(a); status(a);
    du_invalidate(a->ui, (struct du_rect){0, 22, a->ui->canvas.width, a->ui->canvas.height - 42});
}
void hearts_attach(struct hearts_app *a, struct du_ui *u)
{
    a->ui = u; u->event = hearts_event; u->user = a; u->paint = hearts_paint;
    if (!a->game.nhand[0]) hearts_new_game(&a->game, 1);
    memcpy(a->game_items, game_template, sizeof(game_template));
    memcpy(a->help_items, help_template, sizeof(help_template));
    a->menus[0] = (struct du_menu){"&Game", a->game_items, 4};
    a->menus[1] = (struct du_menu){"&Help", a->help_items, 2};
    u->menus = a->menus; u->menu_count = 2;
    du_add(u, DU_CANVAS, HV_CANVAS, (struct du_rect){0, 22, u->canvas.width, u->canvas.height - 42}, "");
    du_add(u, DU_BUTTON, HV_PASS, (struct du_rect){HE_W / 2 - 55, 238, 110, 24}, "&Pass 3 Cards");
    du_add(u, DU_STATUS, HV_STATUS, (struct du_rect){0, u->canvas.height - STATUS_H, u->canvas.width, STATUS_H}, "");
    u->focus = -1; begin_round(a); u->dirty = 1;
}
static void scores_text(const struct hearts_app *a, char *out, size_t cap)
{
    const struct hearts *h = &a->game;
    snprintf(out, cap, "%s: %d\n%s: %d\n%s: %d\n%s: %d", names[0], h->score[0], names[1], h->score[1], names[2], h->score[2], names[3], h->score[3]);
}
static void round_over(struct hearts_app *a)
{
    const struct hearts *h = &a->game; char text[256], scores[128];
    scores_text(a, scores, sizeof(scores));
    if (h->moon >= 0) snprintf(text, sizeof(text), "%s shot the moon! Everyone else scores 26.\n\n%s", names[h->moon], scores);
    else snprintf(text, sizeof(text), "Round over.\n\n%s", scores);
    if (h->phase == HEARTS_GAME_OVER) { size_t n = strlen(text); snprintf(text + n, sizeof(text) - n, "\n\n%s won the game with the lowest score.", names[hearts_winner(h)]); }
    du_message(a->ui, HV_SCORES + 100, "Hearts", text, DU_DLG_ICON_INFO, NULL, 0, 0, 0);
    a->shown_round_end = 1;
}
void hearts_tick(struct hearts_app *a)
{
    struct hearts *h = &a->game;
    if (h->phase == HEARTS_PLAYING && h->turn != 0 && h->turn >= 0) {
        card_t c = hearts_ai_play(h, h->turn);
        if (c != CARD_NONE) hearts_play(h, h->turn, c);
    } else if (h->phase == HEARTS_TRICK_DONE) hearts_resolve_trick(h);
    else if ((h->phase == HEARTS_ROUND_END || h->phase == HEARTS_GAME_OVER) && !a->shown_round_end) round_over(a);
    else return;
    status(a);
    du_invalidate(a->ui, (struct du_rect){0, 22, a->ui->canvas.width, a->ui->canvas.height - 42});
}
static void do_pass(struct hearts_app *a)
{
    card_t c[3]; int k = 0;
    for (int i = 0; i < HE_HAND && k < 3; i++) if (a->marked[i]) c[k++] = a->game.hand[0][i];
    if (k != 3 || hearts_set_pass(&a->game, 0, c) || hearts_do_pass(&a->game)) return;
    memset(a->marked, 0, sizeof(a->marked));
    update_button(a); status(a);
    du_invalidate(a->ui, (struct du_rect){0, 22, a->ui->canvas.width, a->ui->canvas.height - 42});
}
static void pointer(struct hearts_app *a, const struct du_event *e)
{
    struct hearts *h = &a->game;
    if (e->index != DU_POINTER_DOWN || e->key != 1) return;
    int hit = -1;
    for (int i = h->nhand[0] - 1; i >= 0; i--) if (du_contains(hearts_card_rect(a, 0, i), e->x, e->y)) { hit = i; break; }
    if (hit < 0) return;
    if (h->phase == HEARTS_PASSING) {
        if (!a->marked[hit] && marked_count(a) >= 3) return;
        a->marked[hit] = !a->marked[hit];
        update_button(a); status(a);
        du_invalidate(a->ui, (struct du_rect){0, hand_y() - 20, a->ui->canvas.width, CARD_H + 24});
    } else if (h->phase == HEARTS_PLAYING && h->turn == 0) {
        if (hearts_play(h, 0, h->hand[0][hit])) {
            du_set_text(du_get(a->ui, HV_STATUS), h->trick_count == 0 ? (h->trick_no == 0 ? "The two of clubs leads." : "Hearts have not been broken yet.") : "You must follow suit.");
            du_invalidate_widget(a->ui, HV_STATUS);
            return;
        }
        status(a);
        du_invalidate(a->ui, (struct du_rect){0, 22, a->ui->canvas.width, a->ui->canvas.height - 42});
    }
}
void hearts_event(struct du_ui *u, const struct du_event *e, void *user)
{
    struct hearts_app *a = user;
    switch (e->type) {
    case DU_EV_POINTER: if (e->id == HV_CANVAS) pointer(a, e); return;
    case DU_EV_TICK: if (e->id == HV_TIMER) hearts_tick(a); return;
    case DU_EV_DIALOG:
        if (e->id == HV_SCORES + 100) {
            if (a->game.phase == HEARTS_GAME_OVER) hearts_new_game(&a->game, a->game.seed ^ 0x9e3779b9u);
            else hearts_deal(&a->game);
            begin_round(a);
        }
        return;
    case DU_EV_ACTIVATE:
        if (e->id == HV_PASS) do_pass(a);
        else if (e->id == HV_NEW) { hearts_new_game(&a->game, a->game.seed ^ 0x9e3779b9u); begin_round(a); }
        else if (e->id == HV_SCORES) { char s[128]; scores_text(a, s, sizeof(s)); du_message(u, HV_SCORES, "Scores", s, DU_DLG_ICON_INFO, NULL, 0, 0, 0); }
        else if (e->id == HV_EXIT) { if (a->quit) a->quit(a->host); }
        else if (e->id == HV_HELP)
            du_message(u, 1, "Hearts Help", "Avoid hearts (1 point each) and the queen of spades (13). The lowest score wins when someone reaches 100.\nPass three cards each round (left, right, across, then none); the two of clubs leads; follow suit; hearts cannot lead until broken.\nTake all 26 points (shoot the moon) and everyone else scores 26.\nClick a card to select it for passing or to play it.", DU_DLG_ICON_INFO, NULL, 0, 0, 0);
        else if (e->id == HV_ABOUT)
            du_message(u, 2, "About Hearts", "CiukiOS Hearts - Classic computing, one woof at a time.\nOne player against three computer players.\nCode: MIT.", DU_DLG_ICON_INFO, NULL, 0, 0, 0);
        return;
    default: return;
    }
}
