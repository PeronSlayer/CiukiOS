/* SPDX-License-Identifier: MIT
 * FreeCell engine. */
#include "freecell.h"
#include <string.h>

static int is_col(int loc) { return loc >= 0 && loc < FC_COLS; }
static int is_cell(int loc) { return loc >= FC_CELL0 && loc < FC_CELL0 + FC_CELLS; }
static int is_found(int loc) { return loc >= FC_FOUND0 && loc < FC_FOUND0 + FC_FOUND; }

void fc_deal(struct freecell *g, unsigned game)
{
    int wins = g->wins, losses = g->losses;
    if (game < 1) game = 1;
    if (game > FC_GAME_MAX) game = FC_GAME_MAX;
    memset(g, 0, sizeof(*g)); g->wins = wins; g->losses = losses;
    g->game = game;
    card_t deck[52];
    card_deal_numbered(game, deck);
    for (int i = 0; i < 52; i++) g->col[i % FC_COLS][g->ncol[i % FC_COLS]++] = deck[i];   /* cards are dealt across the columns */
    for (int i = 0; i < FC_CELLS; i++) g->cell[i] = CARD_NONE;
    for (int i = 0; i < FC_FOUND; i++) g->found_suit[i] = -1;
}
void fc_restart(struct freecell *g) { fc_deal(g, g->game); }
int fc_free_cells(const struct freecell *g)
{
    int n = 0; for (int i = 0; i < FC_CELLS; i++) n += g->cell[i] == CARD_NONE; return n;
}
int fc_empty_columns(const struct freecell *g)
{
    int n = 0; for (int i = 0; i < FC_COLS; i++) n += g->ncol[i] == 0; return n;
}
int fc_max_run(const struct freecell *g, int to)
{
    int empty = fc_empty_columns(g);
    if (is_col(to) && g->ncol[to] == 0) empty--;          /* the destination itself does not help */
    return (fc_free_cells(g) + 1) << (empty > 0 ? empty : 0);
}
card_t fc_card_at(const struct freecell *g, int loc, int index)
{
    if (is_col(loc)) return index >= 0 && index < g->ncol[loc] ? g->col[loc][index] : CARD_NONE;
    if (is_cell(loc)) return g->cell[loc - FC_CELL0];
    if (is_found(loc)) {
        int k = loc - FC_FOUND0;
        return g->found_count[k] ? CARD(g->found_count[k], g->found_suit[k]) : CARD_NONE;
    }
    return CARD_NONE;
}
int fc_run_ok(const struct freecell *g, int col, int count)
{
    if (!is_col(col) || count < 1 || count > g->ncol[col]) return 0;
    for (int i = g->ncol[col] - count; i + 1 < g->ncol[col]; i++) {
        card_t a = g->col[col][i], b = g->col[col][i + 1];
        if (CARD_RED(a) == CARD_RED(b) || CARD_RANK(a) != CARD_RANK(b) + 1) return 0;
    }
    return 1;
}
int fc_can_move(const struct freecell *g, int from, int count, int to)
{
    if (g->won || from == to || count < 1) return 0;
    card_t c;
    if (is_col(from)) { if (!fc_run_ok(g, from, count)) return 0; c = g->col[from][g->ncol[from] - count]; }
    else if (is_cell(from)) { if (count != 1 || g->cell[from - FC_CELL0] == CARD_NONE) return 0; c = g->cell[from - FC_CELL0]; }
    else return 0;
    if (is_col(to)) {
        if (count > fc_max_run(g, to)) return 0;
        if (!g->ncol[to]) return 1;
        card_t top = g->col[to][g->ncol[to] - 1];
        return g->ncol[to] < FC_MAX_COL && CARD_RED(top) != CARD_RED(c) && CARD_RANK(top) == CARD_RANK(c) + 1;
    }
    if (is_cell(to)) return count == 1 && g->cell[to - FC_CELL0] == CARD_NONE;
    if (is_found(to)) {
        if (count != 1) return 0;
        int k = to - FC_FOUND0;
        if (g->found_count[k]) return g->found_suit[k] == (int)CARD_SUIT(c) && g->found_count[k] + 1 == CARD_RANK(c);
        return CARD_RANK(c) == 1;
    }
    return 0;
}
int fc_is_won(const struct freecell *g)
{
    for (int k = 0; k < FC_FOUND; k++) if (g->found_count[k] != 13) return 0;
    return 1;
}
static void push_history(struct freecell *g, int from, int count, int to)
{
    if (g->nhistory == FC_HISTORY) { memmove(g->history, g->history + 1, sizeof(g->history) - sizeof(g->history[0])); g->nhistory--; }
    g->history[g->nhistory++] = (struct fc_move){(uint8_t)from, (uint8_t)to, (uint8_t)count};
}
int fc_move(struct freecell *g, int from, int count, int to)
{
    if (!fc_can_move(g, from, count, to)) return -1;
    card_t run[FC_MAX_COL];
    if (is_col(from)) { memcpy(run, g->col[from] + g->ncol[from] - count, (size_t)count); g->ncol[from] -= count; }
    else { run[0] = g->cell[from - FC_CELL0]; g->cell[from - FC_CELL0] = CARD_NONE; }
    if (is_col(to)) { memcpy(g->col[to] + g->ncol[to], run, (size_t)count); g->ncol[to] += count; }
    else if (is_cell(to)) g->cell[to - FC_CELL0] = run[0];
    else { int k = to - FC_FOUND0; g->found_suit[k] = CARD_SUIT(run[0]); g->found_count[k]++; }
    push_history(g, from, count, to);
    g->moves++;
    if (fc_is_won(g) && !g->won) { g->won = 1; g->wins++; }
    return 0;
}
int fc_undo(struct freecell *g)
{
    if (!g->nhistory || g->won) return -1;
    struct fc_move m = g->history[--g->nhistory];
    card_t run[FC_MAX_COL];
    if (is_col(m.to)) { g->ncol[m.to] -= m.count; memcpy(run, g->col[m.to] + g->ncol[m.to], m.count); }
    else if (is_cell(m.to)) { run[0] = g->cell[m.to - FC_CELL0]; g->cell[m.to - FC_CELL0] = CARD_NONE; }
    else { int k = m.to - FC_FOUND0; run[0] = CARD(g->found_count[k], g->found_suit[k]); if (--g->found_count[k] == 0) g->found_suit[k] = -1; }
    if (is_col(m.from)) { memcpy(g->col[m.from] + g->ncol[m.from], run, m.count); g->ncol[m.from] += m.count; }
    else g->cell[m.from - FC_CELL0] = run[0];
    g->moves--;
    return 0;
}
int fc_quick(struct freecell *g, int from)
{
    for (int k = 0; k < FC_FOUND; k++) if (!fc_move(g, from, 1, FC_FOUND0 + k)) return 0;
    if (is_col(from))
        for (int i = 0; i < FC_CELLS; i++) if (!fc_move(g, from, 1, FC_CELL0 + i)) return 0;
    return -1;
}
int fc_has_moves(const struct freecell *g)
{
    if (g->won) return 0;
    for (int from = 0; from < FC_CELL0 + FC_CELLS; from++) {
        int max = is_col(from) ? g->ncol[from] : 1;
        for (int count = 1; count <= max; count++)
            for (int to = 0; to < FC_FOUND0 + FC_FOUND; to++) {
                if (is_cell(to) && is_cell(from)) continue;       /* shuffling between cells is not progress */
                if (is_col(to) && is_col(from) && count == g->ncol[from] && !g->ncol[to]) continue;   /* a whole column to an empty one */
                if (fc_can_move(g, from, count, to)) return 1;
            }
    }
    return 0;
}
