/* SPDX-License-Identifier: MIT
 * Hearts engine and computer players. */
#include "hearts.h"
#include <string.h>

int hearts_points(card_t c) { return CARD_SUIT(c) == SUIT_HEARTS ? 1 : (c == CARD(12, SUIT_SPADES) ? 13 : 0); }
static card_t QS(void) { return CARD(12, SUIT_SPADES); }
static card_t C2(void) { return CARD(2, SUIT_CLUBS); }
/* Sort key: the hand shows clubs, diamonds, spades, hearts (alternating colours), then rank. */
static int key(card_t c)
{
    static const int suit_order[4] = {0, 1, 3, 2};   /* indexed by suit: clubs 0, diamonds 1, hearts 3, spades 2 */
    return suit_order[CARD_SUIT(c)] * 16 + CARD_RANK(c);
}
static void sort_hand(struct hearts *h, int p)
{
    for (int i = 1; i < h->nhand[p]; i++) {
        card_t c = h->hand[p][i]; int j = i - 1;
        while (j >= 0 && key(h->hand[p][j]) > key(c)) { h->hand[p][j + 1] = h->hand[p][j]; j--; }
        h->hand[p][j + 1] = c;
    }
}
int hearts_has_card(const struct hearts *h, int p, card_t c)
{
    for (int i = 0; i < h->nhand[p]; i++) if (h->hand[p][i] == c) return 1;
    return 0;
}
static void remove_card(struct hearts *h, int p, card_t c)
{
    for (int i = 0; i < h->nhand[p]; i++) if (h->hand[p][i] == c) {
        memmove(&h->hand[p][i], &h->hand[p][i + 1], (size_t)(h->nhand[p] - i - 1));
        h->nhand[p]--; return;
    }
}
void hearts_new_game(struct hearts *h, uint32_t seed)
{
    memset(h, 0, sizeof(*h)); h->seed = seed; h->round = -1; h->moon = -1; h->last_winner = -1;
    hearts_deal(h);
}
enum hearts_direction hearts_direction(const struct hearts *h) { return (enum hearts_direction)(h->round % 4); }
int hearts_target(const struct hearts *h, int from)
{
    switch (hearts_direction(h)) {
    case HEARTS_LEFT: return (from + 1) % 4;
    case HEARTS_RIGHT: return (from + 3) % 4;
    case HEARTS_ACROSS: return (from + 2) % 4;
    default: return from;
    }
}
static void start_play(struct hearts *h)
{
    h->phase = HEARTS_PLAYING; h->trick_count = 0; h->trick_no = 0; h->hearts_broken = 0;
    for (int p = 0; p < 4; p++) if (hearts_has_card(h, p, C2())) h->leader = h->turn = p;
}
void hearts_deal(struct hearts *h)
{
    card_t deck[52];
    h->seed = h->seed * 1664525u + 1013904223u;
    card_shuffle(h->seed, deck);
    h->round++;
    memset(h->hand, 0, sizeof(h->hand)); memset(h->pass, 0, sizeof(h->pass));
    memset(h->npass, 0, sizeof(h->npass)); memset(h->taken, 0, sizeof(h->taken));
    h->moon = -1;
    for (int p = 0; p < 4; p++) {
        h->nhand[p] = HE_HAND;
        memcpy(h->hand[p], deck + p * HE_HAND, HE_HAND);
        sort_hand(h, p);
    }
    h->trick_count = 0;
    if (hearts_direction(h) == HEARTS_HOLD) start_play(h); else h->phase = HEARTS_PASSING;
}
int hearts_set_pass(struct hearts *h, int p, const card_t cards[3])
{
    if (h->phase != HEARTS_PASSING) return -1;
    for (int i = 0; i < 3; i++) {
        if (!hearts_has_card(h, p, cards[i])) return -1;
        for (int j = 0; j < i; j++) if (cards[j] == cards[i]) return -1;
    }
    memcpy(h->pass[p], cards, 3); h->npass[p] = 3;
    return 0;
}
/* How much a card is worth getting rid of when passing. */
static int risk(const struct hearts *h, int p, card_t c)
{
    int spades = 0; for (int i = 0; i < h->nhand[p]; i++) spades += CARD_SUIT(h->hand[p][i]) == SUIT_SPADES;
    int r = CARD_RANK(c); if (r == 1) r = 14;
    if (c == QS()) return 200;
    if (CARD_SUIT(c) == SUIT_SPADES && r >= 13 && spades < 5) return 120 + r;       /* A, K of spades are dangerous while the queen is out */
    if (CARD_SUIT(c) == SUIT_HEARTS) return 40 + r * 3;
    return r * 2;
}
void hearts_ai_pass(const struct hearts *h, int p, card_t out[3])
{
    card_t best[3] = {CARD_NONE, CARD_NONE, CARD_NONE}; int score[3] = {-1, -1, -1};
    for (int i = 0; i < h->nhand[p]; i++) {
        card_t c = h->hand[p][i]; int s = risk(h, p, c);
        for (int k = 0; k < 3; k++) if (s > score[k]) {
            for (int m = 2; m > k; m--) { best[m] = best[m - 1]; score[m] = score[m - 1]; }
            best[k] = c; score[k] = s; break;
        }
    }
    memcpy(out, best, 3);
}
int hearts_pass_ready(const struct hearts *h)
{
    for (int p = 0; p < 4; p++) if (h->npass[p] != 3) return 0;
    return 1;
}
int hearts_do_pass(struct hearts *h)
{
    if (h->phase != HEARTS_PASSING || !hearts_pass_ready(h)) return -1;
    for (int p = 0; p < 4; p++) for (int i = 0; i < 3; i++) remove_card(h, p, h->pass[p][i]);
    for (int p = 0; p < 4; p++) {
        int to = hearts_target(h, p);
        for (int i = 0; i < 3; i++) h->hand[to][h->nhand[to]++] = h->pass[p][i];
    }
    for (int p = 0; p < 4; p++) sort_hand(h, p);
    memset(h->npass, 0, sizeof(h->npass));
    start_play(h);
    return 0;
}
static int count_suit(const struct hearts *h, int p, int suit)
{
    int n = 0; for (int i = 0; i < h->nhand[p]; i++) n += (int)CARD_SUIT(h->hand[p][i]) == suit; return n;
}
int hearts_legal(const struct hearts *h, int p, card_t c)
{
    if (h->phase != HEARTS_PLAYING || p != h->turn || h->trick_count >= 4 || !hearts_has_card(h, p, c)) return 0;
    int first = h->trick_no == 0;
    if (h->trick_count == 0) {
        if (first) return c == C2();                                   /* the two of clubs always leads */
        if (CARD_SUIT(c) == SUIT_HEARTS && !h->hearts_broken && h->nhand[p] != count_suit(h, p, SUIT_HEARTS)) return 0;
        return 1;
    }
    int led = (int)CARD_SUIT(h->trick[0]);
    if ((int)CARD_SUIT(c) == led) return 1;
    if (count_suit(h, p, led)) return 0;                               /* must follow suit */
    if (first && hearts_points(c)) {                                   /* no points on the first trick unless nothing else */
        for (int i = 0; i < h->nhand[p]; i++) if (!hearts_points(h->hand[p][i])) return 0;
    }
    return 1;
}
static int trick_winner_index(const struct hearts *h)
{
    int led = (int)CARD_SUIT(h->trick[0]), best = 0;
    for (int i = 1; i < h->trick_count; i++) {
        if ((int)CARD_SUIT(h->trick[i]) != led) continue;
        int a = CARD_RANK(h->trick[i]), b = CARD_RANK(h->trick[best]);
        if (a == 1) a = 14;
        if (b == 1) b = 14;
        if (a > b) best = i;
    }
    return best;
}
static int rank14(card_t c) { return CARD_RANK(c) == 1 ? 14 : CARD_RANK(c); }
card_t hearts_ai_play(const struct hearts *h, int p)
{
    card_t pick = CARD_NONE; int pick_score = -1000;
    int trick_points = 0; for (int i = 0; i < h->trick_count; i++) trick_points += hearts_points(h->trick[i]);
    int winning = h->trick_count ? rank14(h->trick[trick_winner_index(h)]) : 0;
    int led = h->trick_count ? (int)CARD_SUIT(h->trick[0]) : -1;
    int last = h->trick_count == 3;
    for (int i = 0; i < h->nhand[p]; i++) {
        card_t c = h->hand[p][i];
        if (!hearts_legal(h, p, c)) continue;
        int r = rank14(c), s;
        if (h->trick_count == 0) {                                    /* leading: low cards of safe suits */
            s = 100 - r * 4 - (CARD_SUIT(c) == SUIT_HEARTS ? 30 : 0);
            if (c == C2()) s += 1000;
            if (CARD_SUIT(c) == SUIT_SPADES && r < 12) s += 10;       /* low spades flush the queen out */
        } else if ((int)CARD_SUIT(c) == led) {
            int takes = r > winning;
            if (!takes) s = 200 + r;                                  /* duck as high as is safe */
            else if (last && trick_points == 0) s = c == QS() ? -500 : 100 + r;   /* nobody is hurt: take it high, keep the queen */
            else s = 100 - r - trick_points * 10;                     /* forced to take: the lowest card */
        } else {                                                      /* void: dump the dangerous cards */
            s = c == QS() ? 900 : (CARD_SUIT(c) == SUIT_HEARTS ? 500 + r : r * 2);
        }
        if (s > pick_score) { pick_score = s; pick = c; }
    }
    return pick;
}
int hearts_play(struct hearts *h, int p, card_t c)
{
    if (!hearts_legal(h, p, c)) return -1;
    remove_card(h, p, c);
    h->trick[h->trick_count] = c; h->trick_player[h->trick_count] = p; h->trick_count++;
    if (CARD_SUIT(c) == SUIT_HEARTS) h->hearts_broken = 1;
    if (h->trick_count == 4) { h->phase = HEARTS_TRICK_DONE; h->turn = -1; }
    else h->turn = (p + 1) % 4;
    return 0;
}
int hearts_resolve_trick(struct hearts *h)
{
    if (h->phase != HEARTS_TRICK_DONE) return -1;
    int w = h->trick_player[trick_winner_index(h)], pts = 0;
    for (int i = 0; i < 4; i++) pts += hearts_points(h->trick[i]);
    h->taken[w] += pts; h->last_winner = w;
    h->trick_count = 0; h->trick_no++;
    if (h->trick_no < HE_HAND) { h->phase = HEARTS_PLAYING; h->leader = h->turn = w; return w; }
    /* The round is over: the moon, then the scores. */
    h->moon = -1;
    for (int p = 0; p < 4; p++) if (h->taken[p] == 26) h->moon = p;
    for (int p = 0; p < 4; p++) h->score[p] += h->moon >= 0 ? (p == h->moon ? 0 : 26) : h->taken[p];
    h->phase = HEARTS_ROUND_END;
    for (int p = 0; p < 4; p++) if (h->score[p] >= HE_END_SCORE) h->phase = HEARTS_GAME_OVER;
    h->turn = -1;
    return w;
}
int hearts_winner(const struct hearts *h)
{
    if (h->phase != HEARTS_GAME_OVER) return -1;
    int best = 0;
    for (int p = 1; p < 4; p++) if (h->score[p] < h->score[best]) best = p;
    return best;
}
