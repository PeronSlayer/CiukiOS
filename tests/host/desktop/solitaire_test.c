/* Solitaire engine and window host tests (G27). SPDX-License-Identifier: MIT */
#include "solitaire.h"
#include "acc_test.h"
static uint32_t pixels[SOL_W * SOL_H];
static struct du_ui ui; static struct sol_app app;
static int total(const struct solitaire *g)
{
    int n = 0; for (int p = 0; p < SOL_PILES; p++) n += g->s.pile[p].count; return n;
}
static void check_deck(const struct solitaire *g)
{
    int seen[52] = {0};
    for (int p = 0; p < SOL_PILES; p++) for (int i = 0; i < g->s.pile[p].count; i++) seen[g->s.pile[p].cards[i]]++;
    for (int i = 0; i < 52; i++) assert(seen[i] == 1);   /* always exactly one of each card */
}
static void put(struct solitaire *g, int pile, card_t c, int up)
{
    struct sol_pile *p = &g->s.pile[pile];
    p->cards[p->count++] = c;
    if (!up) p->first_up = p->count;   /* cards below first_up are face down; a face-down card is only added on a face-down pile */
}
static void clear(struct solitaire *g) { memset(&g->s, 0, sizeof(g->s)); g->have_undo = 0; g->won = 0; }
static void drag(int x0, int y0, int x1, int y1)
{
    at_move(&ui, x0, y0);
    struct ciuki_input_event e = {.type = CIUKI_INPUT_BUTTON, .code = 1, .value = 1}; du_input(&ui, &e);
    at_move(&ui, (x0 + x1) / 2, (y0 + y1) / 2); at_move(&ui, x1, y1);
    e.value = 0; du_input(&ui, &e);
}
static void click(int x, int y, int button)
{
    at_move(&ui, x, y);
    struct ciuki_input_event e = {.type = CIUKI_INPUT_BUTTON, .code = button, .value = 1}; du_input(&ui, &e);
    e.value = 0; du_input(&ui, &e);
}
int main(int argc, char **argv)
{
    const char *folder = argc > 1 ? argv[1] : NULL;
    static struct solitaire g;
    sol_init(&g, 5);
    check_deck(&g);
    assert(g.s.pile[SOL_STOCK].count == 24 && total(&g) == 52 && g.s.score == 0);
    for (int c = 0; c < 7; c++) {
        const struct sol_pile *t = &g.s.pile[SOL_TAB0 + c];
        assert(t->count == c + 1 && t->first_up == c);                    /* the last card of each column is face up */
    }
    /* Different seeds, different deals. */
    struct solitaire h; sol_init(&h, 6); assert(memcmp(g.s.pile[SOL_TAB6].cards, h.s.pile[SOL_TAB6].cards, 7) != 0);

    /* Draw one, draw three, recycling and the scoring of recycles. */
    for (int i = 0; i < 24; i++) assert(sol_draw(&g) == 1);
    assert(g.s.pile[SOL_STOCK].count == 0 && g.s.pile[SOL_WASTE].count == 24 && g.s.score == 0);
    assert(sol_draw(&g) == 1 && g.s.pile[SOL_STOCK].count == 24 && g.s.pile[SOL_WASTE].count == 0 && g.s.passes == 1 && g.s.score == 0);   /* score floors at 0 */
    check_deck(&g);
    g.s.score = 250; for (int i = 0; i < 24; i++) sol_draw(&g); sol_draw(&g);
    assert(g.s.score == 150 && g.s.passes == 2);                               /* draw one: -100 per recycle */
    g.draw_three = 1; sol_deal(&g); assert(g.draw_three);
    sol_draw(&g); assert(g.s.pile[SOL_WASTE].count == 3 && g.s.pile[SOL_STOCK].count == 21);
    g.s.score = 100;
    for (int pass = 1; pass <= 4; pass++) { while (g.s.pile[SOL_STOCK].count) sol_draw(&g); sol_draw(&g); }
    assert(g.s.passes == 4 && g.s.score == 80);                                /* free for three passes, then -20 */
    check_deck(&g);

    /* Moves and scoring on a prepared table. */
    sol_init(&g, 1); clear(&g);
    put(&g, SOL_TAB0, CARD(13, SUIT_SPADES), 1);
    put(&g, SOL_TAB1, CARD(5, SUIT_CLUBS), 0); put(&g, SOL_TAB1, CARD(12, SUIT_HEARTS), 1); put(&g, SOL_TAB1, CARD(11, SUIT_SPADES), 1);   /* face-down 5C under QH, JS */
    put(&g, SOL_TAB2, CARD(10, SUIT_HEARTS), 1);
    put(&g, SOL_WASTE, CARD(9, SUIT_CLUBS), 1);
    put(&g, SOL_WASTE, CARD(1, SUIT_DIAMONDS), 1);
    assert(sol_can_move(&g, SOL_TAB1, 1, SOL_TAB0) && !sol_can_move(&g, SOL_TAB1, 0, SOL_TAB0));   /* face-down cards do not move */
    assert(sol_can_move(&g, SOL_TAB1, 2, SOL_TAB2) == 0);                                           /* JS on 10H: wrong direction */
    assert(!sol_can_move(&g, SOL_TAB1, 1, SOL_TAB1) && !sol_can_move(&g, SOL_TAB1, 1, SOL_STOCK) && !sol_can_move(&g, SOL_TAB1, 1, SOL_WASTE));
    assert(!sol_move(&g, SOL_TAB1, 1, SOL_TAB0));                                                   /* QH,JS onto KS: a run of two */
    assert(g.s.pile[SOL_TAB0].count == 3 && g.s.pile[SOL_TAB1].count == 1 && g.s.pile[SOL_TAB1].first_up == 0 && g.s.score == 5);   /* 5C turned over: +5 */
    assert(g.s.moves == 1 && g.have_undo);
    assert(!sol_move(&g, SOL_WASTE, 1, SOL_FOUND2) && g.s.score == 15 && g.s.pile[SOL_FOUND2].count == 1);   /* AD to a foundation: +10 */
    assert(!sol_can_move(&g, SOL_WASTE, 0, SOL_TAB1) && sol_can_move(&g, SOL_WASTE, 0, SOL_TAB2));           /* 9C: not on 5C, yes on 10H */
    assert(!sol_move(&g, SOL_WASTE, 0, SOL_TAB2) && g.s.score == 20);                                        /* waste to column: +5 */
    /* Foundation rules: same suit, ascending; moving back costs 15. */
    put(&g, SOL_TAB3, CARD(2, SUIT_DIAMONDS), 1); put(&g, SOL_TAB4, CARD(2, SUIT_SPADES), 1);
    assert(!sol_can_move(&g, SOL_TAB4, 0, SOL_FOUND2) && !sol_can_move(&g, SOL_TAB4, 0, SOL_FOUND0) && sol_can_move(&g, SOL_TAB3, 0, SOL_FOUND2));
    assert(!sol_move(&g, SOL_TAB3, 0, SOL_FOUND2) && g.s.score == 30);
    assert(!sol_can_move(&g, SOL_FOUND2, 1, SOL_TAB4));                                                      /* 2D onto 2S: not allowed */
    put(&g, SOL_TAB5, CARD(3, SUIT_CLUBS), 1);
    assert(!sol_move(&g, SOL_FOUND2, 1, SOL_TAB5) && g.s.score == 15);                                       /* 2D on 3C: -15 */
    assert(total(&g) == 10);                                                                         /* cards are only ever moved */
    /* Undo restores everything but the clock. */
    g.s.seconds = 42;
    assert(!sol_undo(&g) && g.s.score == 30 && g.s.seconds == 42 && g.s.pile[SOL_FOUND2].count == 2);
    assert(sol_undo(&g) == -1);                                                                     /* one level */
    /* An empty column takes only a king. */
    clear(&g); put(&g, SOL_TAB0, CARD(12, SUIT_CLUBS), 1); put(&g, SOL_TAB1, CARD(13, SUIT_HEARTS), 1);
    assert(!sol_can_move(&g, SOL_TAB0, 0, SOL_TAB2) && sol_can_move(&g, SOL_TAB1, 0, SOL_TAB2));
    /* Auto: double click sends to a foundation. */
    clear(&g); put(&g, SOL_TAB0, CARD(1, SUIT_CLUBS), 1); put(&g, SOL_WASTE, CARD(1, SUIT_HEARTS), 1);
    assert(!sol_auto(&g, SOL_TAB0) && !sol_auto(&g, SOL_WASTE) && g.s.pile[SOL_FOUND0].count == 1 && g.s.pile[SOL_FOUND1].count == 1);
    assert(sol_auto(&g, SOL_TAB0) == -1 && sol_auto(&g, SOL_STOCK) == -1);
    /* Winning: scoring bonus of a timed game. */
    clear(&g); g.timed = 1; g.s.seconds = 70;
    for (int s = 0; s < 4; s++) for (int r = 1; r <= 12; r++) put(&g, SOL_FOUND0 + s, CARD(r, s), 1);
    for (int s = 0; s < 4; s++) put(&g, SOL_TAB0 + s, CARD(13, s), 1);
    g.s.score = 100;
    for (int s = 0; s < 4; s++) assert(!sol_move(&g, SOL_TAB0 + s, 0, SOL_FOUND0 + s));
    assert(g.won && sol_is_won(&g) && g.s.score == 100 + 4 * 10 + 700000 / 70);
    assert(sol_draw(&g) == 0 && sol_move(&g, SOL_FOUND0, 12, SOL_TAB4) == -1);
    sol_tick(&g); assert(g.s.seconds == 70);                                                        /* the clock stops */
    clear(&g); g.scoring = SOL_SCORE_NONE; put(&g, SOL_TAB0, CARD(1, SUIT_CLUBS), 1);
    assert(!sol_auto(&g, SOL_TAB0) && g.s.score == 0);                                              /* no scoring */
    g.s.seconds = 20; assert(sol_bonus(&g) == 0);

    /* The window: drag, drop, stock, double click, right click, undo, menus. */
    du_init(&ui, pixels, SOL_W, SOL_H);
    sol_init(&app.game, 3);
    sol_attach(&app, &ui);
    du_render(&ui);
    at_save(folder, "solitaire-deal", &ui);
    struct solitaire *w = &app.game;
    struct du_rect stock = sol_pile_rect(&app, SOL_STOCK);
    click(stock.x + 5, stock.y + 5, 1);
    assert(w->s.pile[SOL_WASTE].count == 1 && w->s.pile[SOL_STOCK].count == 23);
    int pile, index;
    struct du_rect wr = sol_card_rect(&app, SOL_WASTE, 0);
    assert(sol_hit(&app, wr.x + 3, wr.y + 3, &pile, &index) && pile == SOL_WASTE && index == 0);
    /* Build a position where the waste card fits a column, then drag it there. */
    clear(w); w->have_undo = 0;
    put(w, SOL_TAB0, CARD(8, SUIT_SPADES), 1); put(w, SOL_TAB1, CARD(7, SUIT_HEARTS), 1); put(w, SOL_WASTE, CARD(1, SUIT_CLUBS), 1);
    put(w, SOL_TAB2, CARD(2, SUIT_CLUBS), 1);
    du_render(&ui);
    struct du_rect a1 = sol_card_rect(&app, SOL_TAB1, 0), t0 = sol_card_rect(&app, SOL_TAB0, 0);
    drag(a1.x + 5, a1.y + 5, t0.x + 10, t0.y + 20);                                                  /* 7H onto 8S */
    assert(w->s.pile[SOL_TAB0].count == 2 && w->s.pile[SOL_TAB1].count == 0 && w->s.pile[SOL_TAB0].cards[1] == CARD(7, SUIT_HEARTS));
    drag(a1.x + 5, a1.y + 5, t0.x + 10, t0.y + 20);                                                  /* the slot is empty now: nothing happens */
    assert(w->s.pile[SOL_TAB0].count == 2);
    struct du_rect w0 = sol_card_rect(&app, SOL_WASTE, 0), f0 = sol_pile_rect(&app, SOL_FOUND1);
    drag(w0.x + 5, w0.y + 5, f0.x + 10, f0.y + 10);                                                  /* AC to a foundation */
    assert(w->s.pile[SOL_FOUND1].count == 1 && w->s.pile[SOL_WASTE].count == 0 && w->s.score == 10);
    struct du_rect t2 = sol_card_rect(&app, SOL_TAB2, 0);
    drag(t2.x + 5, t2.y + 5, f0.x + 10, f0.y + 10);                                                  /* 2C follows */
    assert(w->s.pile[SOL_FOUND1].count == 2 && w->s.score == 20);
    drag(t0.x + 5, t0.y + 5, t0.x + 6, t0.y + 6);                                                    /* a tiny wobble is a click, not a drag */
    assert(w->s.pile[SOL_TAB0].count == 2);
    assert(sol_card_rect(&app, SOL_TAB0, 1).y > t0.y);
    /* Double click and right click. */
    clear(w); put(w, SOL_TAB0, CARD(1, SUIT_HEARTS), 1); put(w, SOL_TAB1, CARD(1, SUIT_SPADES), 1);
    du_render(&ui);
    t0 = sol_card_rect(&app, SOL_TAB0, 0); ui.now = 1000000000ull;
    click(t0.x + 5, t0.y + 5, 1); assert(w->s.pile[SOL_TAB0].count == 1);
    ui.now += 200000000ull; click(t0.x + 5, t0.y + 5, 1); assert(w->s.pile[SOL_TAB0].count == 0 && w->s.pile[SOL_FOUND0].count == 1);
    t0 = sol_card_rect(&app, SOL_TAB1, 0); click(t0.x + 5, t0.y + 5, 2);
    assert(w->s.pile[SOL_TAB1].count == 0 && w->s.pile[SOL_FOUND1].count == 1);
    sol_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = SL_UNDO}, &app);
    assert(w->s.pile[SOL_TAB1].count == 1);
    /* Menus. */
    sol_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = SL_DRAW3}, &app);
    assert(w->draw_three && (app.game_items[4].flags & DU_ITEM_CHECKED) && w->s.pile[SOL_STOCK].count == 24);
    click(stock.x + 5, stock.y + 5, 1); assert(w->s.pile[SOL_WASTE].count == 3);
    du_render(&ui);
    at_save(folder, "solitaire-draw3", &ui);
    struct du_rect top3 = sol_card_rect(&app, SOL_WASTE, 2); assert(top3.x == stock.x + 72 + 28);
    sol_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = SL_TIMED}, &app); assert(w->timed && w->draw_three);
    sol_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = SL_SCORING}, &app); assert(w->scoring == SOL_SCORE_NONE);
    assert(strstr(du_get(&ui, 20)->text, "No scoring"));
    click(stock.x + 5, stock.y + 5, 1);
    int run = 0; for (int i = 0; i < DU_TIMERS; i++) run += ui.timers[i].active && ui.timers[i].id == SL_TIMER;
    assert(run == 1);
    sol_event(&ui, &(struct du_event){.type = DU_EV_TICK, .id = SL_TIMER}, &app); assert(w->s.seconds == 1 && strstr(du_get(&ui, 20)->text, "Time: 1"));
    /* A long column is compressed to fit the window. */
    clear(w); w->draw_three = 0;
    for (int r = 13; r >= 1; r--) put(w, SOL_TAB0, CARD(r, r % 2 ? SUIT_CLUBS : SUIT_HEARTS), 1);
    for (int i = 0; i < 6; i++) { put(w, SOL_TAB6, (card_t)(40 + i), 0); }
    put(w, SOL_TAB6, CARD(13, SUIT_SPADES), 1);
    for (int r = 12; r >= 1; r--) put(w, SOL_TAB6, CARD(r, r % 2 ? SUIT_HEARTS : SUIT_CLUBS), 1);
    du_render(&ui);
    struct du_rect last = sol_card_rect(&app, SOL_TAB6, w->s.pile[SOL_TAB6].count - 1);
    assert(last.y + CARD_H <= SOL_H - 20);
    at_save(folder, "solitaire-long", &ui);
    puts("solitaire: PASS");
    return 0;
}
