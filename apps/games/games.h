/* SPDX-License-Identifier: MIT
 * Games accessories (G27, AX-11): Minesweeper, Solitaire, FreeCell and Hearts,
 * equivalents of the Windows 98 "Games" group on libdeskui. Each game has an
 * engine (host-tested, no drawing), a window and a main; the playing-card
 * helpers are shared. Integers only (no x87), like the toolkit. */
#ifndef CIUKI_GAMES_H
#define CIUKI_GAMES_H
#include "../accessories/acc.h"
#include <stddef.h>
#include <stdint.h>

/* ---- playing cards (cards.c) ---- */
enum { SUIT_CLUBS, SUIT_DIAMONDS, SUIT_HEARTS, SUIT_SPADES };
#define CARD_W 50
#define CARD_H 68
#define CARD_NONE 0xff
/* A card is rank 1..13 (ace..king) * 4 + suit, or CARD_NONE; the same numbering as the Microsoft deal (rank-1)*4+suit. */
typedef uint8_t card_t;
#define CARD(rank, suit) ((card_t)(((rank) - 1) * 4 + (suit)))
#define CARD_RANK(c) ((c) / 4 + 1)
#define CARD_SUIT(c) ((c) % 4)
#define CARD_RED(c) (CARD_SUIT(c) == SUIT_DIAMONDS || CARD_SUIT(c) == SUIT_HEARTS)
/* Linear congruential generator of the Microsoft C runtime (the FreeCell game numbers rely on it). */
struct card_rng { uint32_t seed; };
unsigned card_rand(struct card_rng *);                 /* 15 bits */
/* Fills out[0..51] with the deal of a game number exactly as Windows FreeCell numbers it. */
void card_deal_numbered(unsigned game, card_t out[52]);
/* Fills out[0..51] with a shuffle from an arbitrary seed (Solitaire, Hearts). */
void card_shuffle(uint32_t seed, card_t out[52]);
const char *card_rank_name(int rank);                  /* "A", "2", ..., "10", "J", "Q", "K" */
void card_name(card_t, char out[8]);                   /* "10H" style, for logs and tests */
/* Drawing: a face-up card, the back, an empty slot, and a suit symbol. */
void card_draw(struct du_canvas *, int x, int y, card_t, int selected);
void card_draw_back(struct du_canvas *, int x, int y);
void card_draw_slot(struct du_canvas *, int x, int y, const char *mark);
void card_draw_suit(struct du_canvas *, int cx, int cy, int size, int suit, uint32_t color);
#define TABLE_GREEN 0x008000u
#endif
