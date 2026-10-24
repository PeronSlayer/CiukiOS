/* FreeCell engine and window host tests (G27). SPDX-License-Identifier: MIT */
#include "freecell.h"
#include "acc_test.h"
static uint32_t pixels[FC_W * FC_H];
static struct du_ui ui; static struct fc_app app;
static struct freecell g;
static void names(const struct freecell *f, int row, char out[64])
{
    out[0] = 0;
    for (int c = 0; c < FC_COLS; c++) {
        card_t card = f->ncol[c] > row ? f->col[c][row] : CARD_NONE; char n[8];
        card_name(card, n); strcat(out, n); if (c < FC_COLS - 1) strcat(out, " ");
    }
}
static void expect_row(const struct freecell *f, int row, const char *want)
{
    char got[64]; names(f, row, got);
    if (strcmp(got, want)) { fprintf(stderr, "game %u row %d: got '%s', want '%s'\n", f->game, row, got, want); abort(); }
}
static void blank(struct freecell *f)
{
    unsigned game = f->game; memset(f, 0, sizeof(*f)); f->game = game;
    for (int i = 0; i < FC_CELLS; i++) f->cell[i] = CARD_NONE;
    for (int i = 0; i < FC_FOUND; i++) f->found_suit[i] = -1;
}
static void col(struct freecell *f, int c, const card_t *cards, int n) { memcpy(f->col[c], cards, (size_t)n); f->ncol[c] = n; }
static void click(int x, int y, int button)
{
    at_move(&ui, x, y);
    struct ciuki_input_event e = {.type = CIUKI_INPUT_BUTTON, .code = button, .value = 1}; du_input(&ui, &e);
    e.value = 0; du_input(&ui, &e);
}
static void click_loc(int loc, int index) { struct du_rect r = fc_loc_rect(&app, loc, index); click(r.x + 5, r.y + 5, 1); }
int main(int argc, char **argv)
{
    const char *folder = argc > 1 ? argv[1] : NULL;
    /* The Microsoft deals: game 1 and the famous unwinnable game 11982. */
    memset(&g, 0, sizeof(g)); fc_deal(&g, 1);
    expect_row(&g, 0, "JD 2D 9H JC 5D 7H 7C 5H"); expect_row(&g, 1, "KD KC 9S 5S AD QC KH 3H");
    expect_row(&g, 2, "2S KS 9D QD JS AS AH 3C"); expect_row(&g, 3, "4C 5C 10S QH 4H AC 4D 7S");
    expect_row(&g, 4, "3S 10D 4S 10H 8H 2C JH 7D"); expect_row(&g, 5, "6D 8S 8D QS 6C 3D 8C 10C");
    expect_row(&g, 6, "6S 9C 2H 6H -- -- -- --");
    assert(g.ncol[0] == 7 && g.ncol[3] == 7 && g.ncol[4] == 6 && g.ncol[7] == 6 && fc_free_cells(&g) == 4 && fc_empty_columns(&g) == 0);
    fc_deal(&g, 11982);
    expect_row(&g, 0, "AH AS 4H AC 2D 6S 10S JS"); expect_row(&g, 1, "3D 3H QS QC 8S 7H AD KS");
    fc_deal(&g, 617); expect_row(&g, 0, "7D AD 5C 3S 5S 8C 2D AH");
    fc_deal(&g, 0); assert(g.game == 1); fc_deal(&g, 99999); assert(g.game == FC_GAME_MAX);
    /* Every deal is a complete deck. */
    for (unsigned n = 1; n <= 200; n++) {
        fc_deal(&g, n * 160 % FC_GAME_MAX + 1);
        int seen[52] = {0}; for (int c = 0; c < FC_COLS; c++) for (int i = 0; i < g.ncol[c]; i++) seen[g.col[c][i]]++;
        for (int i = 0; i < 52; i++) assert(seen[i] == 1);
    }

    /* Moves: runs, the run limit, free cells, foundations, undo. */
    memset(&g, 0, sizeof(g)); g.game = 5; blank(&g);
    col(&g, 0, (card_t[]){CARD(9, SUIT_CLUBS), CARD(8, SUIT_HEARTS), CARD(7, SUIT_SPADES)}, 3);
    col(&g, 1, (card_t[]){CARD(10, SUIT_DIAMONDS)}, 1);
    col(&g, 2, (card_t[]){CARD(1, SUIT_HEARTS)}, 1);
    col(&g, 3, (card_t[]){CARD(2, SUIT_HEARTS), CARD(5, SUIT_CLUBS)}, 2);
    assert(fc_run_ok(&g, 0, 3) && fc_run_ok(&g, 0, 1) && !fc_run_ok(&g, 3, 2) && !fc_run_ok(&g, 0, 4));
    assert(fc_free_cells(&g) == 4 && fc_empty_columns(&g) == 4);
    assert(fc_max_run(&g, 1) == (4 + 1) * 16);                                                        /* (cells + 1) * 2^empty columns */
    assert(fc_max_run(&g, 4) == (4 + 1) * 8);                                                         /* an empty destination does not count */
    assert(fc_can_move(&g, 0, 3, 1) && !fc_can_move(&g, 0, 2, 1) && !fc_can_move(&g, 0, 1, 2));     /* 9C run on 10D; 8H on 10D no; 7S on AH no */
    assert(!fc_can_move(&g, 0, 3, 0) && !fc_can_move(&g, 0, 0, 1) && !fc_can_move(&g, 0, 3, FC_CELL0) && !fc_can_move(&g, 0, 3, FC_FOUND0));
    assert(!fc_move(&g, 0, 3, 1) && g.ncol[0] == 0 && g.ncol[1] == 4 && g.moves == 1 && g.nhistory == 1);
    assert(g.col[1][3] == CARD(7, SUIT_SPADES));
    assert(!fc_undo(&g) && g.ncol[0] == 3 && g.ncol[1] == 1 && g.moves == 0 && g.nhistory == 0 && fc_undo(&g) == -1);
    assert(!fc_move(&g, 3, 1, FC_CELL0 + 1) && g.cell[1] == CARD(5, SUIT_CLUBS) && g.ncol[3] == 1 && fc_free_cells(&g) == 3);
    assert(!fc_can_move(&g, 1, 1, FC_CELL0 + 1) && !fc_can_move(&g, FC_CELL0, 1, FC_CELL0 + 2));   /* an empty cell holds nothing to move */
    assert(!fc_can_move(&g, FC_CELL0 + 1, 1, FC_FOUND0) && !fc_can_move(&g, 3, 1, FC_FOUND0 + 1));  /* a 5 and a 2 do not start a foundation */
    assert(!fc_move(&g, 2, 1, FC_FOUND0 + 2) && g.found_count[2] == 1 && g.found_suit[2] == SUIT_HEARTS && g.ncol[2] == 0);
    assert(!fc_can_move(&g, 3, 1, FC_FOUND0) && fc_can_move(&g, 3, 1, FC_FOUND0 + 2));              /* 2H follows AH only on its own foundation */
    assert(!fc_move(&g, 3, 1, FC_FOUND0 + 2) && g.found_count[2] == 2);
    assert(fc_card_at(&g, FC_FOUND0 + 2, 0) == CARD(2, SUIT_HEARTS) && fc_card_at(&g, FC_FOUND0, 0) == CARD_NONE && fc_card_at(&g, FC_CELL0 + 1, 0) == CARD(5, SUIT_CLUBS));
    assert(!fc_can_move(&g, FC_FOUND0 + 2, 1, 1));                                                  /* nothing leaves a foundation */
    assert(!fc_undo(&g) && g.found_count[2] == 1 && !fc_undo(&g) && g.found_count[2] == 0 && g.found_suit[2] == -1);
    assert(g.ncol[2] == 1 && g.ncol[3] == 1 && g.cell[1] == CARD(5, SUIT_CLUBS));
    /* The run limit with full cells and no empty column. */
    blank(&g);
    for (int c = 0; c < FC_COLS; c++) col(&g, c, (card_t[]){CARD(13, SUIT_CLUBS)}, 1);
    col(&g, 0, (card_t[]){CARD(12, SUIT_HEARTS), CARD(11, SUIT_SPADES), CARD(10, SUIT_HEARTS)}, 3);
    col(&g, 1, (card_t[]){CARD(13, SUIT_SPADES)}, 1);
    g.cell[0] = CARD(2, SUIT_CLUBS); g.cell[1] = CARD(2, SUIT_SPADES);                                   /* two cells busy: runs of three */
    col(&g, 2, (card_t[]){CARD(13, SUIT_CLUBS)}, 1);
    assert(fc_max_run(&g, 1) == 3 && fc_can_move(&g, 0, 3, 1) && fc_can_move(&g, 0, 3, 2));
    g.cell[2] = CARD(3, SUIT_CLUBS);
    assert(fc_max_run(&g, 1) == 2 && !fc_can_move(&g, 0, 3, 1) && fc_can_move(&g, 0, 2, 1) == 0);      /* QH,JS,10H onto KS needs 3; JS,10H onto KS: wrong rank */
    g.ncol[3] = 0; g.ncol[4] = 0;                                                                         /* two empty columns: (1 + 1) * 4 */
    assert(fc_max_run(&g, 1) == 8 && fc_max_run(&g, 3) == 4);
    /* A column holds at most 20 cards. */
    blank(&g);
    for (int i = 0; i < FC_MAX_COL; i++) g.col[0][i] = CARD(13 - i % 13, (i / 13) % 2 ? SUIT_CLUBS : SUIT_HEARTS);
    g.ncol[0] = FC_MAX_COL;
    /* No moves, then a move appears. */
    blank(&g);
    static const card_t dead[8] = {CARD(2, SUIT_CLUBS), CARD(2, SUIT_SPADES), CARD(3, SUIT_CLUBS), CARD(3, SUIT_SPADES),
                                   CARD(4, SUIT_CLUBS), CARD(4, SUIT_SPADES), CARD(5, SUIT_CLUBS), CARD(5, SUIT_SPADES)};
    for (int c = 0; c < FC_COLS; c++) col(&g, c, &dead[c], 1);
    g.cell[0] = CARD(13, SUIT_DIAMONDS); g.cell[1] = CARD(13, SUIT_HEARTS); g.cell[2] = CARD(12, SUIT_DIAMONDS); g.cell[3] = CARD(12, SUIT_HEARTS);
    assert(!fc_has_moves(&g));
    col(&g, 0, (card_t[]){CARD(1, SUIT_CLUBS)}, 1); assert(fc_has_moves(&g));
    col(&g, 0, dead, 1); g.cell[3] = CARD_NONE; assert(fc_has_moves(&g));                                 /* a free cell: columns can move */
    /* Quick: foundation first, else a free cell. */
    blank(&g);
    col(&g, 0, (card_t[]){CARD(9, SUIT_CLUBS), CARD(1, SUIT_HEARTS)}, 2); col(&g, 1, (card_t[]){CARD(5, SUIT_CLUBS)}, 1);
    assert(!fc_quick(&g, 0) && g.found_count[0] == 1 && g.ncol[0] == 1);
    assert(!fc_quick(&g, 1) && g.cell[0] == CARD(5, SUIT_CLUBS) && g.ncol[1] == 0);
    assert(fc_quick(&g, FC_CELL0) == -1 && fc_quick(&g, FC_FOUND0) == -1);
    /* Winning. */
    blank(&g);
    for (int k = 0; k < FC_FOUND; k++) { g.found_count[k] = 12; g.found_suit[k] = k; col(&g, k, (card_t[]){CARD(13, k)}, 1); }
    for (int k = 0; k < FC_FOUND; k++) assert(!fc_move(&g, k, 1, FC_FOUND0 + k));
    assert(g.won && g.wins == 1 && fc_is_won(&g) && !fc_has_moves(&g) && fc_undo(&g) == -1 && !fc_can_move(&g, 0, 1, 1));
    int wins = g.wins; fc_deal(&g, 3); assert(g.wins == wins && !g.won);                                    /* the session count survives a new deal */
    /* The history is bounded: undo still works after more moves than it holds. */
    blank(&g); col(&g, 0, (card_t[]){CARD(13, SUIT_CLUBS)}, 1);
    for (int i = 0; i < FC_HISTORY + 20; i++) {
        int from = g.cell[0] == CARD_NONE ? 0 : FC_CELL0;
        if (from == 0) assert(!fc_move(&g, 0, 1, FC_CELL0)); else assert(!fc_move(&g, FC_CELL0, 1, 0));
    }
    assert(g.nhistory == FC_HISTORY && !fc_undo(&g) && g.nhistory == FC_HISTORY - 1);

    /* The window. */
    du_init(&ui, pixels, FC_W, FC_H);
    memset(&app, 0, sizeof(app)); fc_deal(&app.game, 1);
    fc_attach(&app, &ui);
    du_render(&ui);
    at_save(folder, "freecell-game1", &ui);
    assert(strstr(du_get(&ui, FCV_STATUS)->text, "Game #1") && (app.game_items[3].flags & DU_ITEM_DISABLED));
    struct freecell *w = &app.game;
    blank(w); w->game = 9;
    col(w, 0, (card_t[]){CARD(9, SUIT_CLUBS), CARD(8, SUIT_HEARTS)}, 2); col(w, 1, (card_t[]){CARD(10, SUIT_DIAMONDS)}, 1);
    col(w, 2, (card_t[]){CARD(1, SUIT_SPADES)}, 1); col(w, 3, (card_t[]){CARD(6, SUIT_CLUBS)}, 1);
    du_render(&ui);
    int loc, index;
    struct du_rect r = fc_loc_rect(&app, 0, 0);
    assert(fc_hit(&app, r.x + 3, r.y + 3, &loc, &index) && loc == 0 && index == 0);
    assert(fc_hit(&app, r.x + 3, r.y + 25, &loc, &index) && loc == 0 && index == 1);
    assert(!fc_hit(&app, 3, 300, &loc, &index));
    /* Click a run, click its destination. */
    click_loc(0, 0); assert(app.sel_loc == 0 && app.sel_index == 0);
    du_render(&ui);
    at_save(folder, "freecell-selected", &ui);
    click_loc(1, 0); assert(w->ncol[0] == 0 && w->ncol[1] == 3 && app.sel_loc < 0 && w->moves == 1);
    assert(!(app.game_items[3].flags & DU_ITEM_DISABLED) && strstr(du_get(&ui, FCV_STATUS)->text, "Moves: 1"));
    /* Select, then click elsewhere that is illegal: the selection clears and nothing moves. */
    click_loc(3, 0); assert(app.sel_loc == 3);
    click_loc(1, 2); assert(w->ncol[3] == 1 && w->ncol[1] == 3 && app.sel_loc == 1 && app.sel_index == 2);   /* 6C does not fit on 8H: the click selects 8H instead */
    click(3, 300, 1); assert(app.sel_loc < 0);                                                     /* a click on the felt deselects */
    /* Drag to a free cell and to a column. */
    struct du_rect from = fc_loc_rect(&app, 3, 0), cell = fc_loc_rect(&app, FC_CELL0 + 2, 0);
    at_move(&ui, from.x + 5, from.y + 5);
    struct ciuki_input_event e = {.type = CIUKI_INPUT_BUTTON, .code = 1, .value = 1}; du_input(&ui, &e);
    at_move(&ui, from.x + 5, from.y - 30); at_move(&ui, cell.x + 10, cell.y + 10);
    du_render(&ui);
    at_save(folder, "freecell-drag", &ui);
    e.value = 0; du_input(&ui, &e);
    assert(w->cell[2] == CARD(6, SUIT_CLUBS) && w->ncol[3] == 0);
    /* Double click and right click send the Ace on. */
    r = fc_loc_rect(&app, 2, 0); ui.now = 5000000000ull;
    click(r.x + 5, r.y + 5, 1); assert(w->ncol[2] == 1);
    ui.now += 100000000ull; click(r.x + 5, r.y + 5, 1); assert(w->ncol[2] == 0 && w->found_count[0] == 1);
    click_loc(FC_CELL0 + 2, 0); click(0 + fc_loc_rect(&app, FC_CELL0 + 2, 0).x + 5, fc_loc_rect(&app, FC_CELL0 + 2, 0).y + 5, 2);   /* right click on a cell card */
    /* Undo through the menu, then restart. */
    int moves = w->moves;
    fc_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = FCV_UNDO}, &app); assert(w->moves == moves - 1);
    fc_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = FCV_RESTART}, &app);
    assert(w->game == 9 && w->moves == 0 && w->ncol[0] == 7 && w->ncol[7] == 6 && !w->nhistory);
    /* Select Game: the input box result. */
    fc_event(&ui, &(struct du_event){.type = DU_EV_DIALOG, .id = FCV_SELECT, .index = 0, .text = "617"}, &app);
    assert(w->game == 617);
    fc_event(&ui, &(struct du_event){.type = DU_EV_DIALOG, .id = FCV_SELECT, .index = 0, .text = "0"}, &app); assert(w->game == 617);
    fc_event(&ui, &(struct du_event){.type = DU_EV_DIALOG, .id = FCV_SELECT, .index = 0, .text = "12x"}, &app); assert(w->game == 617);
    fc_event(&ui, &(struct du_event){.type = DU_EV_DIALOG, .id = FCV_SELECT, .index = 1, .text = "5"}, &app); assert(w->game == 617);       /* Cancel */
    fc_event(&ui, &(struct du_event){.type = DU_EV_ACTIVATE, .id = FCV_NEW}, &app); assert(w->game >= 1 && w->game <= FC_GAME_MAX && w->moves == 0);
    /* A long column fits the window. */
    blank(w);
    for (int i = 0; i < 19; i++) w->col[0][i] = CARD(13 - i % 13, (i / 13) % 2 ? SUIT_CLUBS : SUIT_HEARTS);
    w->ncol[0] = 19;
    du_render(&ui);
    assert(fc_loc_rect(&app, 0, 18).y + CARD_H <= FC_H - 20);
    at_save(folder, "freecell-long", &ui);
    puts("freecell: PASS");
    return 0;
}
