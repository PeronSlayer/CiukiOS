/* Hearts engine and window host tests (G27). SPDX-License-Identifier: MIT */
#include "hearts.h"
#include "acc_test.h"
static uint32_t pixels[HE_W * HE_H];
static struct du_ui ui; static struct hearts_app app;
static void click(int x, int y)
{
    at_move(&ui, x, y);
    struct ciuki_input_event e = {.type = CIUKI_INPUT_BUTTON, .code = 1, .value = 1}; du_input(&ui, &e);
    e.value = 0; du_input(&ui, &e);
}
static void check_deal(const struct hearts *h)
{
    int seen[52] = {0};
    for (int p = 0; p < 4; p++) {
        assert(h->nhand[p] == 13);
        for (int i = 0; i < 13; i++) seen[h->hand[p][i]]++;
    }
    for (int i = 0; i < 52; i++) assert(seen[i] == 1);
}
static void set_hand(struct hearts *h, int p, const card_t *c, int n) { memcpy(h->hand[p], c, (size_t)n); h->nhand[p] = n; }
static int sorted_ok(const struct hearts *h, int p)
{
    static const int order[4] = {0, 1, 3, 2};
    for (int i = 1; i < h->nhand[p]; i++) {
        int a = order[CARD_SUIT(h->hand[p][i - 1])] * 16 + CARD_RANK(h->hand[p][i - 1]), b = order[CARD_SUIT(h->hand[p][i])] * 16 + CARD_RANK(h->hand[p][i]);
        if (a >= b) return 0;
    }
    return 1;
}
/* One whole round: every move by the computer players (the human too), checking the rules on the way. */
static int play_round(struct hearts *h, int *points_total)
{
    int guard = 0;
    while ((h->phase == HEARTS_PASSING) && guard++ < 10) {
        for (int p = 0; p < 4; p++) { card_t c[3]; hearts_ai_pass(h, p, c); assert(!hearts_set_pass(h, p, c)); }
        assert(hearts_pass_ready(h) && !hearts_do_pass(h));
    }
    assert(h->phase == HEARTS_PLAYING);
    check_deal(h);
    int tricks = 0;
    while (h->phase != HEARTS_ROUND_END && h->phase != HEARTS_GAME_OVER) {
        if (h->phase == HEARTS_TRICK_DONE) { assert(h->trick_count == 4); hearts_resolve_trick(h); tricks++; continue; }
        int p = h->turn;
        if (h->trick_no == 0 && h->trick_count == 0) assert(hearts_has_card(h, p, CARD(2, SUIT_CLUBS)));
        card_t c = hearts_ai_play(h, p);
        assert(c != CARD_NONE && hearts_legal(h, p, c));
        if (h->trick_no == 0 && h->trick_count == 0) assert(c == CARD(2, SUIT_CLUBS));
        assert(!hearts_play(h, p, c));
        assert(guard++ < 1000);
    }
    assert(tricks == 12 || tricks == 13);
    *points_total = 0; for (int p = 0; p < 4; p++) *points_total += h->taken[p];
    return h->moon;
}
int main(int argc, char **argv)
{
    const char *folder = argc > 1 ? argv[1] : NULL;
    static struct hearts h;
    hearts_new_game(&h, 7); check_deal(&h);
    for (int p = 0; p < 4; p++) assert(sorted_ok(&h, p));
    assert(h.phase == HEARTS_PASSING && hearts_direction(&h) == HEARTS_LEFT && h.round == 0);
    assert(hearts_target(&h, 0) == 1 && hearts_target(&h, 3) == 0);
    /* Passing: validation and the exchange. */
    card_t mine[3] = {h.hand[0][0], h.hand[0][1], h.hand[0][2]};
    card_t foreign[3] = {h.hand[1][0], h.hand[0][1], h.hand[0][2]};
    card_t twice[3] = {h.hand[0][0], h.hand[0][0], h.hand[0][2]};
    assert(hearts_set_pass(&h, 0, foreign) == -1 && hearts_set_pass(&h, 0, twice) == -1 && h.npass[0] == 0);
    assert(!hearts_set_pass(&h, 0, mine) && !hearts_pass_ready(&h) && hearts_do_pass(&h) == -1);
    card_t before[HE_PLAYERS][3];
    for (int p = 1; p < 4; p++) { hearts_ai_pass(&h, p, before[p]); assert(!hearts_set_pass(&h, p, before[p])); }
    memcpy(before[0], mine, 3);
    struct hearts snap = h;
    assert(!hearts_do_pass(&h) && h.phase == HEARTS_PLAYING);
    check_deal(&h);
    for (int p = 0; p < 4; p++) {
        int to = hearts_target(&snap, p);
        for (int i = 0; i < 3; i++) { assert(hearts_has_card(&h, to, before[p][i])); assert(!hearts_has_card(&h, p, before[p][i]) || to == p); }
        assert(sorted_ok(&h, p));
    }
    assert(hearts_has_card(&h, h.leader, CARD(2, SUIT_CLUBS)) && h.turn == h.leader);
    /* Directions over four rounds, the last one without passing. */
    hearts_deal(&h); assert(hearts_direction(&h) == HEARTS_RIGHT && hearts_target(&h, 0) == 3 && h.phase == HEARTS_PASSING);
    hearts_deal(&h); assert(hearts_direction(&h) == HEARTS_ACROSS && hearts_target(&h, 1) == 3);
    hearts_deal(&h); assert(hearts_direction(&h) == HEARTS_HOLD && h.phase == HEARTS_PLAYING && hearts_has_card(&h, h.turn, CARD(2, SUIT_CLUBS)));
    hearts_deal(&h); assert(hearts_direction(&h) == HEARTS_LEFT);

    /* Rules on prepared hands. */
    memset(&h, 0, sizeof(h)); h.phase = HEARTS_PLAYING; h.moon = -1;
    set_hand(&h, 0, (card_t[]){CARD(2, SUIT_CLUBS), CARD(9, SUIT_CLUBS), CARD(5, SUIT_HEARTS), CARD(12, SUIT_SPADES)}, 4);
    set_hand(&h, 1, (card_t[]){CARD(3, SUIT_CLUBS), CARD(7, SUIT_DIAMONDS), CARD(11, SUIT_HEARTS)}, 3);
    set_hand(&h, 2, (card_t[]){CARD(4, SUIT_DIAMONDS), CARD(12, SUIT_SPADES) - 4 + 4, CARD(13, SUIT_HEARTS)}, 3);
    h.hand[2][1] = CARD(10, SUIT_SPADES);
    set_hand(&h, 3, (card_t[]){CARD(1, SUIT_HEARTS), CARD(2, SUIT_HEARTS), CARD(3, SUIT_HEARTS)}, 3);
    h.turn = h.leader = 0;
    assert(!hearts_legal(&h, 0, CARD(9, SUIT_CLUBS)) && hearts_legal(&h, 0, CARD(2, SUIT_CLUBS)));      /* the two of clubs leads */
    assert(!hearts_legal(&h, 1, CARD(3, SUIT_CLUBS)) && hearts_play(&h, 1, CARD(3, SUIT_CLUBS)) == -1); /* not your turn */
    assert(!hearts_play(&h, 0, CARD(2, SUIT_CLUBS)) && h.turn == 1 && h.trick_count == 1);
    assert(hearts_legal(&h, 1, CARD(3, SUIT_CLUBS)) && !hearts_legal(&h, 1, CARD(7, SUIT_DIAMONDS)) && !hearts_legal(&h, 1, CARD(11, SUIT_HEARTS)));   /* follow suit */
    assert(!hearts_play(&h, 1, CARD(3, SUIT_CLUBS)));
    /* Player 2 is void in clubs on the first trick: no points while a safe card exists. */
    assert(hearts_legal(&h, 2, CARD(4, SUIT_DIAMONDS)) && hearts_legal(&h, 2, CARD(10, SUIT_SPADES)) && !hearts_legal(&h, 2, CARD(13, SUIT_HEARTS)));
    assert(!hearts_play(&h, 2, CARD(10, SUIT_SPADES)));
    /* Player 3 holds only hearts: the first trick may take points. */
    assert(hearts_legal(&h, 3, CARD(1, SUIT_HEARTS)) && !hearts_play(&h, 3, CARD(1, SUIT_HEARTS)));
    assert(h.phase == HEARTS_TRICK_DONE && h.turn == -1 && h.hearts_broken);
    assert(hearts_play(&h, 0, CARD(9, SUIT_CLUBS)) == -1);                                              /* the trick must be taken first */
    assert(hearts_resolve_trick(&h) == 1 && h.taken[1] == 1 && h.turn == 1 && h.leader == 1 && h.trick_count == 0 && h.trick_no == 1);   /* 3C beats 2C; the ace of hearts is 1 point */
    /* Leading hearts: broken now. Unbroken: not unless only hearts. */
    h.hearts_broken = 0;
    assert(!hearts_legal(&h, 1, CARD(11, SUIT_HEARTS)) && hearts_legal(&h, 1, CARD(7, SUIT_DIAMONDS)));
    h.nhand[1] = 1; h.hand[1][0] = CARD(11, SUIT_HEARTS);
    assert(hearts_legal(&h, 1, CARD(11, SUIT_HEARTS)));                                                 /* only hearts left: allowed */
    h.hearts_broken = 1; h.nhand[1] = 2; h.hand[1][1] = CARD(7, SUIT_DIAMONDS);
    assert(hearts_legal(&h, 1, CARD(11, SUIT_HEARTS)));
    /* The highest card of the led suit wins; the ace is high; the queen of spades counts 13. */
    memset(&h, 0, sizeof(h)); h.phase = HEARTS_PLAYING; h.trick_no = 3; h.moon = -1;
    set_hand(&h, 0, (card_t[]){CARD(1, SUIT_SPADES), CARD(2, SUIT_SPADES)}, 2); set_hand(&h, 1, (card_t[]){CARD(13, SUIT_SPADES), CARD(3, SUIT_SPADES)}, 2);
    set_hand(&h, 2, (card_t[]){CARD(12, SUIT_SPADES), CARD(4, SUIT_SPADES)}, 2); set_hand(&h, 3, (card_t[]){CARD(5, SUIT_HEARTS), CARD(6, SUIT_HEARTS)}, 2);
    h.turn = h.leader = 1;
    assert(!hearts_play(&h, 1, CARD(13, SUIT_SPADES)) && !hearts_play(&h, 2, CARD(12, SUIT_SPADES)) && !hearts_play(&h, 3, CARD(5, SUIT_HEARTS)));
    assert(hearts_play(&h, 0, CARD(2, SUIT_SPADES)) == 0 && hearts_resolve_trick(&h) == 1 && h.taken[1] == 13 + 1);   /* KS wins; QS 13 + the heart */
    assert(hearts_points(CARD(12, SUIT_SPADES)) == 13 && hearts_points(CARD(7, SUIT_HEARTS)) == 1 && hearts_points(CARD(12, SUIT_HEARTS)) == 1 && hearts_points(CARD(13, SUIT_SPADES)) == 0);
    memset(&h, 0, sizeof(h)); h.phase = HEARTS_PLAYING; h.trick_no = 3; h.moon = -1;           /* the ace wins over the king */
    set_hand(&h, 0, (card_t[]){CARD(1, SUIT_DIAMONDS)}, 1); set_hand(&h, 1, (card_t[]){CARD(13, SUIT_DIAMONDS)}, 1);
    set_hand(&h, 2, (card_t[]){CARD(9, SUIT_CLUBS)}, 1); set_hand(&h, 3, (card_t[]){CARD(10, SUIT_DIAMONDS)}, 1);
    h.turn = h.leader = 1;
    hearts_play(&h, 1, CARD(13, SUIT_DIAMONDS)); hearts_play(&h, 2, CARD(9, SUIT_CLUBS)); hearts_play(&h, 3, CARD(10, SUIT_DIAMONDS)); hearts_play(&h, 0, CARD(1, SUIT_DIAMONDS));
    assert(hearts_resolve_trick(&h) == 0 && h.last_winner == 0);                               /* a trick led in diamonds is won by the ace; the club cannot win */

    /* Shooting the moon, the end of the game and the winner. */
    memset(&h, 0, sizeof(h)); h.phase = HEARTS_TRICK_DONE; h.trick_no = 12; h.moon = -1; h.trick_count = 4;
    h.trick[0] = CARD(2, SUIT_DIAMONDS); h.trick[1] = CARD(3, SUIT_DIAMONDS); h.trick[2] = CARD(9, SUIT_DIAMONDS); h.trick[3] = CARD(5, SUIT_HEARTS);
    for (int i = 0; i < 4; i++) h.trick_player[i] = (i + 1) % 4;      /* led by 1: 1,2,3,0 */
    h.taken[3] = 25; h.score[0] = 50; h.score[1] = 60; h.score[2] = 70; h.score[3] = 10;
    assert(h.trick_player[2] == 3);
    assert(hearts_resolve_trick(&h) == 3 && h.moon == 3 && h.score[3] == 10 && h.score[0] == 76 && h.score[1] == 86 && h.score[2] == 96 && h.phase == HEARTS_ROUND_END);
    assert(hearts_winner(&h) == -1);
    memset(&h, 0, sizeof(h)); h.phase = HEARTS_TRICK_DONE; h.trick_no = 12; h.moon = -1; h.trick_count = 4;
    h.trick[0] = CARD(2, SUIT_DIAMONDS); h.trick[1] = CARD(3, SUIT_DIAMONDS); h.trick[2] = CARD(9, SUIT_DIAMONDS); h.trick[3] = CARD(4, SUIT_DIAMONDS);
    for (int i = 0; i < 4; i++) h.trick_player[i] = i;
    h.taken[0] = 5; h.taken[1] = 8; h.taken[2] = 9; h.taken[3] = 4; h.score[0] = 95; h.score[1] = 40; h.score[2] = 20; h.score[3] = 60;
    assert(hearts_resolve_trick(&h) == 2 && h.score[0] == 100 && h.score[1] == 48 && h.score[2] == 29 && h.phase == HEARTS_GAME_OVER && hearts_winner(&h) == 2);

    /* Many whole games by the computer: legal moves only, 26 points per round, the game ends. */
    int moons = 0, rounds = 0;
    for (uint32_t seed = 1; seed <= 60; seed++) {
        hearts_new_game(&h, seed * 7919u);
        for (int guard = 0; guard < 60 && h.phase != HEARTS_GAME_OVER; guard++) {
            int points, moon = play_round(&h, &points);
            assert(points == 26 && h.taken[0] + h.taken[1] + h.taken[2] + h.taken[3] == 26);
            moons += moon >= 0; rounds++;
            if (h.phase == HEARTS_GAME_OVER) break;
            assert(h.phase == HEARTS_ROUND_END);
            hearts_deal(&h);
        }
        assert(h.phase == HEARTS_GAME_OVER && hearts_winner(&h) >= 0);
        int top = 0; for (int p = 0; p < 4; p++) if (h.score[p] > top) top = h.score[p];
        assert(top >= HE_END_SCORE);
        int w = hearts_winner(&h); for (int p = 0; p < 4; p++) assert(h.score[w] <= h.score[p]);
    }
    assert(rounds >= 60 * 3);
    fprintf(stderr, "hearts: %d rounds, %d moons\n", rounds, moons);

    /* The window: pass three cards by clicking, then play. */
    du_init(&ui, pixels, HE_W, HE_H);
    memset(&app, 0, sizeof(app)); hearts_new_game(&app.game, 3);
    hearts_attach(&app, &ui);
    struct hearts *g = &app.game;
    du_render(&ui);
    at_save(folder, "hearts-pass", &ui);
    struct du_widget *pass = du_get(&ui, HV_PASS);
    assert(!pass->hidden && pass->disabled && strstr(du_get(&ui, HV_STATUS)->text, "pass left") && g->npass[1] == 3 && g->npass[3] == 3);
    for (int i = 0; i < 4; i++) { struct du_rect r = hearts_card_rect(&app, 0, i); click(r.x + 3, r.y + 30); }
    assert(app.marked[0] + app.marked[1] + app.marked[2] + app.marked[3] == 3 && pass->disabled == 0);   /* a fourth card is refused */
    { struct du_rect r = hearts_card_rect(&app, 0, 0); assert(r.y < HE_H - 20 - CARD_H - 14);               /* a marked card is raised */
      click(r.x + 3, r.y + 30); assert(!app.marked[0] && pass->disabled);                                  /* clicking again unmarks it */
      r = hearts_card_rect(&app, 0, 0); click(r.x + 3, r.y + 30); assert(app.marked[0] && !pass->disabled); }
    du_render(&ui);
    at_save(folder, "hearts-marked", &ui);
    card_t chosen[3]; int k = 0; for (int i = 0; i < 13 && k < 3; i++) if (app.marked[i]) chosen[k++] = g->hand[0][i];
    assert(k == 3);
    hearts_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = HV_PASS}, &app);
    assert(g->phase == HEARTS_PLAYING && pass->hidden);
    for (int i = 0; i < 3; i++) assert(hearts_has_card(g, 1, chosen[i]));                                   /* passed left: West holds them now */
    check_deal(g);
    /* Play: the computers move on ticks; the human clicks cards. Illegal clicks are refused. */
    int human_moves = 0, refused = 0;
    for (int step = 0; step < 400 && g->phase != HEARTS_ROUND_END && g->phase != HEARTS_GAME_OVER; step++) {
        if (g->phase == HEARTS_PLAYING && g->turn == 0) {
            int legal = -1, illegal = -1;
            for (int i = 0; i < g->nhand[0]; i++) { if (hearts_legal(g, 0, g->hand[0][i])) { if (legal < 0) legal = i; } else if (illegal < 0) illegal = i; }
            assert(legal >= 0);
            if (illegal >= 0 && refused < 3) {
                int cards = g->nhand[0], tc = g->trick_count; struct du_rect r = hearts_card_rect(&app, 0, illegal);
                click(r.x + 3, r.y + 5);       /* may hit the neighbouring card on top: only the count of cards tells */
                if (g->nhand[0] == cards) { refused++; assert(g->trick_count == tc); }
                else human_moves++;
                continue;
            }
            struct du_rect r = hearts_card_rect(&app, 0, legal); int cards = g->nhand[0];
            click(r.x + 3, r.y + 5); assert(g->nhand[0] == cards - 1 || g->nhand[0] == cards); human_moves += g->nhand[0] == cards - 1;
        } else hearts_tick(&app);
        if (step == 30) { du_render(&ui); at_save(folder, "hearts-playing", &ui); }
    }
    assert(human_moves >= 10 && g->phase != HEARTS_PLAYING);
    assert(g->taken[0] + g->taken[1] + g->taken[2] + g->taken[3] == 26);
    du_render(&ui);
    at_save(folder, "hearts-round-end", &ui);
    hearts_tick(&app); assert(app.shown_round_end);
    hearts_event(&ui, &(struct du_event){.type = DU_EV_DIALOG, .id = HV_SCORES + 100, .index = 0}, &app);
    assert((g->phase == HEARTS_PASSING || g->phase == HEARTS_PLAYING) && g->round == 1 && !app.shown_round_end);
    hearts_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = HV_NEW}, &app);
    assert(g->round == 0 && g->score[0] == 0 && g->score[1] == 0);
    puts("hearts: PASS");
    return 0;
}
