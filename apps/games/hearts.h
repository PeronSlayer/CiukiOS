/* SPDX-License-Identifier: MIT
 * Hearts (the Windows 98 "Hearts" equivalent, played against three computer
 * players): pass three cards left, right, across or not at all, the two of
 * clubs leads, follow suit, no points on the first trick, hearts must be
 * broken before they lead, one point per heart and thirteen for the queen of
 * spades, shooting the moon (the others get 26), the game ends when a player
 * reaches 100 and the lowest score wins. Network play (Windows Hearts used
 * DDE over a LAN) is not part of this program. */
#ifndef CIUKI_HEARTS_H
#define CIUKI_HEARTS_H
#include "games.h"
#define HE_PLAYERS 4
#define HE_HAND 13
#define HE_END_SCORE 100
enum hearts_phase { HEARTS_PASSING, HEARTS_PLAYING, HEARTS_TRICK_DONE, HEARTS_ROUND_END, HEARTS_GAME_OVER };
enum hearts_direction { HEARTS_LEFT, HEARTS_RIGHT, HEARTS_ACROSS, HEARTS_HOLD };
struct hearts {
    card_t hand[HE_PLAYERS][HE_HAND]; int nhand[HE_PLAYERS];     /* kept sorted: suits, then rank */
    card_t pass[HE_PLAYERS][3]; int npass[HE_PLAYERS];
    enum hearts_phase phase; int round;
    card_t trick[HE_PLAYERS]; int trick_player[HE_PLAYERS]; int trick_count, leader, turn, trick_no;
    int hearts_broken;
    int taken[HE_PLAYERS];                                       /* points taken this round */
    int score[HE_PLAYERS];
    int moon;                                                    /* player who shot the moon this round, -1 none */
    int last_winner;
    uint32_t seed;
};
#define HEARTS_HUMAN 0                                           /* player 0 sits at the bottom */
void hearts_new_game(struct hearts *, uint32_t seed);
void hearts_deal(struct hearts *);                               /* the next round of the game */
enum hearts_direction hearts_direction(const struct hearts *);   /* of the current round */
int hearts_target(const struct hearts *, int from);              /* who receives from's passed cards */
int hearts_set_pass(struct hearts *, int player, const card_t cards[3]);   /* 0, or -1 when not in the hand */
void hearts_ai_pass(const struct hearts *, int player, card_t out[3]);
int hearts_pass_ready(const struct hearts *);                    /* all four players have chosen */
int hearts_do_pass(struct hearts *);                             /* exchanges the cards and starts play; 0 on success */
int hearts_legal(const struct hearts *, int player, card_t);
card_t hearts_ai_play(const struct hearts *, int player);
int hearts_play(struct hearts *, int player, card_t);            /* 0, or -1 when it is not legal or not the turn */
int hearts_resolve_trick(struct hearts *);                       /* after four cards: winner takes it; ends the round after the 13th */
int hearts_points(card_t);                                       /* 1 for a heart, 13 for the queen of spades */
int hearts_winner(const struct hearts *);                        /* lowest score once the game is over, -1 before */
int hearts_has_card(const struct hearts *, int player, card_t);

#define HE_W 600
#define HE_H 384
struct hearts_app {
    struct du_ui *ui; struct hearts game;
    struct du_menu menus[2]; struct du_menu_item game_items[4], help_items[2];
    int marked[HE_HAND];                                         /* cards of the human selected to pass */
    void (*quit)(void *host); void *host;
    int shown_round_end;
};
enum { HV_CANVAS = 1, HV_PASS = 2, HV_STATUS = 20, HV_TIMER = 1, HV_NEW = 10, HV_SCORES, HV_EXIT, HV_HELP, HV_ABOUT };
void hearts_attach(struct hearts_app *, struct du_ui *);
void hearts_event(struct du_ui *, const struct du_event *, void *);
void hearts_paint(struct du_ui *, void *);
struct du_rect hearts_card_rect(const struct hearts_app *, int player, int index);
void hearts_tick(struct hearts_app *);                           /* one computer move or trick turn-over; the timer calls it */
#endif
