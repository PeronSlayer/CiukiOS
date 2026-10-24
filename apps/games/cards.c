/* SPDX-License-Identifier: MIT
 * Playing cards: the Microsoft deal numbering, shuffles and vector drawing. */
#include "games.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

unsigned card_rand(struct card_rng *r)
{
    r->seed = r->seed * 214013u + 2531011u;
    return (r->seed >> 16) & 0x7fffu;
}
void card_deal_numbered(unsigned game, card_t out[52])
{
    card_t pool[52]; unsigned left = 52;
    struct card_rng rng = {game};
    for (unsigned i = 0; i < 52; i++) pool[i] = (card_t)i;
    for (unsigned i = 0; i < 52; i++) {
        unsigned j = card_rand(&rng) % left;
        out[i] = pool[j]; pool[j] = pool[--left];
    }
}
void card_shuffle(uint32_t seed, card_t out[52])
{
    for (unsigned i = 0; i < 52; i++) out[i] = (card_t)i;
    /* A 32-bit xorshift: the full state, unlike the 15-bit output of the Microsoft generator. */
    uint32_t x = seed ? seed : 0x9e3779b9u;
    for (unsigned i = 51; i > 0; i--) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        unsigned j = x % (i + 1);
        card_t t = out[i]; out[i] = out[j]; out[j] = t;
    }
}
const char *card_rank_name(int rank)
{
    static const char *const names[] = {"?", "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K"};
    return rank >= 1 && rank <= 13 ? names[rank] : names[0];
}
void card_name(card_t c, char out[8])
{
    if (c == CARD_NONE) { strcpy(out, "--"); return; }
    snprintf(out, 8, "%s%c", card_rank_name(CARD_RANK(c)), "CDHS"[CARD_SUIT(c)]);
}

static void row(struct du_canvas *c, int x, int y, int w, uint32_t color)
{
    if (w > 0) du_fill(c, (struct du_rect){x, y, w, 1}, color);
}
void card_draw_suit(struct du_canvas *c, int cx, int cy, int s, int suit, uint32_t color)
{
    int r = s / 2 > 1 ? s / 2 : 1;
    switch (suit) {
    case SUIT_DIAMONDS:
        for (int dy = -s; dy <= s; dy++) row(c, cx - (s - abs(dy)) * 2 / 3, cy + dy, (s - abs(dy)) * 4 / 3 + 1, color);
        break;
    case SUIT_HEARTS:
        acc_disc(c, cx - r + r / 3, cy - r / 2, r, color); acc_disc(c, cx + r - r / 3, cy - r / 2, r, color);
        for (int dy = 0; dy <= s; dy++) row(c, cx - (2 * r - r / 3) * (s - dy) / s, cy + dy - r / 2 + 1, (2 * r - r / 3) * 2 * (s - dy) / s + 1, color);
        break;
    case SUIT_CLUBS:
        acc_disc(c, cx, cy - r + 1, r - 0, color); acc_disc(c, cx - r, cy + r / 3, r, color); acc_disc(c, cx + r, cy + r / 3, r, color);
        du_fill(c, (struct du_rect){cx - 1, cy, 3, s}, color);
        break;
    default:   /* spades: a heart upside down on a stem */
        for (int dy = 0; dy <= s; dy++) row(c, cx - (2 * r - r / 3) * dy / s, cy - s + dy + 1, (2 * r - r / 3) * 2 * dy / s + 1, color);
        acc_disc(c, cx - r + r / 3, cy + 1, r, color); acc_disc(c, cx + r - r / 3, cy + 1, r, color);
        du_fill(c, (struct du_rect){cx - 1, cy, 3, s}, color);
        break;
    }
}
static void body(struct du_canvas *c, int x, int y, uint32_t fill)
{
    struct du_rect r = {x, y, CARD_W, CARD_H};
    du_fill(c, r, 0x000000u);
    du_fill(c, (struct du_rect){x + 1, y + 1, CARD_W - 2, CARD_H - 2}, fill);
    for (int i = 0; i < 2; i++) {   /* rounded corners */
        du_fill(c, (struct du_rect){x + (i ? CARD_W - 1 : 0), y, 1, 1}, TABLE_GREEN);
        du_fill(c, (struct du_rect){x + (i ? CARD_W - 1 : 0), y + CARD_H - 1, 1, 1}, TABLE_GREEN);
    }
}
void card_draw(struct du_canvas *c, int x, int y, card_t card, int selected)
{
    uint32_t ink = CARD_RED(card) ? 0xc00000u : 0x000000u;
    body(c, x, y, selected ? 0x8080ffu : 0xffffffu);
    const char *rank = card_rank_name(CARD_RANK(card));
    du_text(c, x + 4, y + 3, rank, ink);
    card_draw_suit(c, x + 8, y + 22, 4, CARD_SUIT(card), ink);
    card_draw_suit(c, x + CARD_W / 2, y + CARD_H / 2 + 6, 10, CARD_SUIT(card), ink);
    int w = du_text_width(rank);
    du_text(c, x + CARD_W - 4 - w, y + CARD_H - 14, rank, ink);
}
void card_draw_back(struct du_canvas *c, int x, int y)
{
    body(c, x, y, 0xffffffu);
    du_fill(c, (struct du_rect){x + 3, y + 3, CARD_W - 6, CARD_H - 6}, 0x0000a0u);
    du_checker(c, (struct du_rect){x + 5, y + 5, CARD_W - 10, CARD_H - 10}, 0x4040ffu, 0x0000a0u);
}
void card_draw_slot(struct du_canvas *c, int x, int y, const char *mark)
{
    du_outline(c, (struct du_rect){x, y, CARD_W, CARD_H}, 0x006000u);
    du_fill(c, (struct du_rect){x + 1, y + 1, CARD_W - 2, CARD_H - 2}, 0x007000u);
    if (mark && *mark) du_text(c, x + (CARD_W - du_text_width(mark)) / 2, y + CARD_H / 2 - 6, mark, 0x00a000u);
}
