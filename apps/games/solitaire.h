/* SPDX-License-Identifier: MIT
 * Solitaire (Klondike, the Windows 98 "Solitaire" equivalent): Draw One or
 * Draw Three, Standard scoring (waste to tableau +5, waste or tableau to
 * foundation +10, turning a tableau card +5, foundation to tableau -15,
 * recycling the waste -100 in Draw One and -20 per pass after the third in
 * Draw Three), optional timed bonus, one-level Undo, drag and drop, and a
 * double click to the foundation. */
#ifndef CIUKI_SOLITAIRE_H
#define CIUKI_SOLITAIRE_H
#include "games.h"
enum sol_pile_id { SOL_STOCK, SOL_WASTE, SOL_FOUND0, SOL_FOUND1, SOL_FOUND2, SOL_FOUND3,
                   SOL_TAB0, SOL_TAB1, SOL_TAB2, SOL_TAB3, SOL_TAB4, SOL_TAB5, SOL_TAB6, SOL_PILES };
enum sol_scoring { SOL_SCORE_STANDARD, SOL_SCORE_NONE };
struct sol_pile { card_t cards[52]; int count, first_up; };   /* cards at index >= first_up are face up */
struct sol_state {
    struct sol_pile pile[SOL_PILES];
    int score, moves, passes, seconds;
};
struct solitaire {
    struct sol_state s, undo; int have_undo;
    int draw_three, scoring, timed, won;
    uint32_t seed;
};
void sol_init(struct solitaire *, uint32_t seed);
void sol_deal(struct solitaire *);                       /* a new game with the current options */
int sol_draw(struct solitaire *);                        /* stock click: draw (or recycle); 0 when nothing to do */
int sol_can_move(const struct solitaire *, int from, int index, int to);   /* the cards from index (in pile from) onto pile to */
int sol_move(struct solitaire *, int from, int index, int to);              /* 0 when moved, -1 when illegal */
int sol_auto(struct solitaire *, int from);              /* double click: the top card to a foundation; 0 when moved */
int sol_undo(struct solitaire *);                        /* 0 when something was undone */
int sol_is_won(const struct solitaire *);
void sol_tick(struct solitaire *);                       /* one second of play */
int sol_bonus(const struct solitaire *);                 /* timed-game bonus at the end: 700000 / seconds when over 30 s */
int sol_top(const struct sol_pile *);                    /* index of the top card, -1 when empty */
int sol_face_up(const struct sol_pile *, int index);

#define SOL_W 600
#define SOL_H 384
struct sol_app {
    struct du_ui *ui; struct solitaire game;
    struct du_menu menus[2]; struct du_menu_item game_items[10], help_items[2];
    int drag_pile, drag_index, dragging, drag_x, drag_y, off_x, off_y;   /* the held cards; dragging -1: pressed, 1: moving */
    int last_pile, last_index; uint64_t last_ns;                          /* double click */
    void (*quit)(void *host); void *host;
};
enum { SL_CANVAS = 1, SL_TIMER = 1, SL_DEAL = 10, SL_UNDO, SL_DRAW1, SL_DRAW3, SL_SCORING, SL_TIMED, SL_EXIT, SL_HELP, SL_ABOUT };
void sol_attach(struct sol_app *, struct du_ui *);
void sol_event(struct du_ui *, const struct du_event *, void *);
void sol_paint(struct du_ui *, void *);
struct du_rect sol_pile_rect(const struct sol_app *, int pile);                         /* the slot */
struct du_rect sol_card_rect(const struct sol_app *, int pile, int index);              /* one card of a pile */
int sol_hit(const struct sol_app *, int x, int y, int *pile, int *index);               /* 1 when over a pile; index -1 on the empty slot */
#endif
