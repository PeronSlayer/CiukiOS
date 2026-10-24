/* SPDX-License-Identifier: MIT
 * Klondike engine. */
#include "solitaire.h"
#include <string.h>

int sol_top(const struct sol_pile *p) { return p->count - 1; }
int sol_face_up(const struct sol_pile *p, int i) { return i >= p->first_up && i < p->count; }
static int is_found(int p) { return p >= SOL_FOUND0 && p <= SOL_FOUND3; }
static int is_tab(int p) { return p >= SOL_TAB0 && p <= SOL_TAB6; }

void sol_init(struct solitaire *g, uint32_t seed)
{
    memset(g, 0, sizeof(*g)); g->seed = seed; sol_deal(g);
}
void sol_deal(struct solitaire *g)
{
    card_t deck[52];
    int draw = g->draw_three, scoring = g->scoring, timed = g->timed;
    g->seed = g->seed * 1664525u + 1013904223u;
    card_shuffle(g->seed, deck);
    memset(&g->s, 0, sizeof(g->s)); g->have_undo = 0; g->won = 0;
    g->draw_three = draw; g->scoring = scoring; g->timed = timed;
    int at = 0;
    for (int col = 0; col < 7; col++) {
        struct sol_pile *p = &g->s.pile[SOL_TAB0 + col];
        for (int i = 0; i <= col; i++) p->cards[p->count++] = deck[at++];
        p->first_up = col;
    }
    struct sol_pile *st = &g->s.pile[SOL_STOCK];
    while (at < 52) st->cards[st->count++] = deck[at++];
    st->first_up = st->count;   /* all face down */
    /* Windows starts the standard score at zero; a timed game bonus is added at the end. */
}
static void add_score(struct solitaire *g, int d)
{
    if (g->scoring == SOL_SCORE_NONE) return;
    g->s.score += d;
    if (g->s.score < 0) g->s.score = 0;
}
static void remember(struct solitaire *g) { g->undo = g->s; g->have_undo = 1; }

int sol_is_won(const struct solitaire *g)
{
    for (int i = SOL_FOUND0; i <= SOL_FOUND3; i++) if (g->s.pile[i].count != 13) return 0;
    return 1;
}
static void finish(struct solitaire *g)
{
    if (!g->won && sol_is_won(g)) { g->won = 1; if (g->timed) add_score(g, sol_bonus(g)); }
}
int sol_bonus(const struct solitaire *g)
{
    return g->s.seconds > 30 ? 700000 / g->s.seconds : 0;
}
int sol_draw(struct solitaire *g)
{
    struct sol_pile *st = &g->s.pile[SOL_STOCK], *w = &g->s.pile[SOL_WASTE];
    if (g->won) return 0;
    if (!st->count) {
        if (!w->count) return 0;
        remember(g);
        while (w->count) st->cards[st->count++] = w->cards[--w->count];   /* the waste turns over onto the stock */
        st->first_up = st->count; w->first_up = 0;
        g->s.passes++;
        if (!g->draw_three) add_score(g, -100);
        else if (g->s.passes > 3) add_score(g, -20);
        g->s.moves++;
        return 1;
    }
    remember(g);
    int n = g->draw_three ? 3 : 1;
    for (int i = 0; i < n && st->count; i++) w->cards[w->count++] = st->cards[--st->count];
    st->first_up = st->count; w->first_up = 0;
    g->s.moves++;
    return 1;
}
static int can_found(const struct solitaire *g, int to, card_t c)
{
    const struct sol_pile *f = &g->s.pile[to];
    if (!f->count) return CARD_RANK(c) == 1;
    card_t top = f->cards[f->count - 1];
    return CARD_SUIT(top) == CARD_SUIT(c) && CARD_RANK(top) + 1 == CARD_RANK(c);
}
static int can_tab(const struct solitaire *g, int to, card_t c)
{
    const struct sol_pile *t = &g->s.pile[to];
    if (!t->count) return CARD_RANK(c) == 13;
    card_t top = t->cards[t->count - 1];
    return CARD_RED(top) != CARD_RED(c) && CARD_RANK(top) == CARD_RANK(c) + 1;
}
int sol_can_move(const struct solitaire *g, int from, int index, int to)
{
    if (from < 0 || from >= SOL_PILES || to < 0 || to >= SOL_PILES || from == to || g->won) return 0;
    if (from == SOL_STOCK || to == SOL_STOCK || to == SOL_WASTE) return 0;
    const struct sol_pile *p = &g->s.pile[from];
    if (index < 0 || index >= p->count || !sol_face_up(p, index)) return 0;
    if (from == SOL_WASTE && index != p->count - 1) return 0;
    if (is_found(from) && index != p->count - 1) return 0;
    card_t c = p->cards[index];
    if (is_found(to)) return index == p->count - 1 && can_found(g, to, c);
    return is_tab(to) && can_tab(g, to, c);
}
int sol_move(struct solitaire *g, int from, int index, int to)
{
    if (!sol_can_move(g, from, index, to)) return -1;
    remember(g);
    struct sol_pile *p = &g->s.pile[from], *d = &g->s.pile[to];
    int n = p->count - index;
    if (!d->count) d->first_up = 0;
    memcpy(d->cards + d->count, p->cards + index, (size_t)n);
    d->count += n;
    if (is_found(to) || !is_tab(to)) d->first_up = 0;
    p->count = index;
    if (is_tab(from)) {
        if (p->count && p->first_up >= p->count) { p->first_up = p->count - 1; add_score(g, 5); }   /* the new top card turns over */
        if (!p->count) p->first_up = 0;
    }
    if (from == SOL_WASTE && is_tab(to)) add_score(g, 5);
    else if ((from == SOL_WASTE || is_tab(from)) && is_found(to)) add_score(g, 10);
    else if (is_found(from) && is_tab(to)) add_score(g, -15);
    g->s.moves++;
    finish(g);
    return 0;
}
int sol_auto(struct solitaire *g, int from)
{
    const struct sol_pile *p = &g->s.pile[from];
    if (!(from == SOL_WASTE || is_tab(from)) || !p->count) return -1;
    for (int f = SOL_FOUND0; f <= SOL_FOUND3; f++)
        if (!sol_move(g, from, p->count - 1, f)) return 0;
    return -1;
}
int sol_undo(struct solitaire *g)
{
    if (!g->have_undo) return -1;
    int seconds = g->s.seconds;
    g->s = g->undo; g->s.seconds = seconds; g->have_undo = 0;
    return 0;
}
void sol_tick(struct solitaire *g) { if (!g->won) g->s.seconds++; }
