/* SPDX-License-Identifier: MIT
 * FreeCell (the Windows 98 "FreeCell" equivalent): the numbered Microsoft
 * deals (game 1 .. 32000, so the famous deals are the same), eight columns,
 * four free cells, four foundations, moves of runs limited by the free cells
 * and empty columns, unlimited Undo, restart of the same deal, and the "no
 * more moves" test. Click source then destination, or drag. */
#ifndef CIUKI_FREECELL_H
#define CIUKI_FREECELL_H
#include "games.h"
#define FC_COLS 8
#define FC_CELLS 4
#define FC_FOUND 4
#define FC_MAX_COL 20
#define FC_HISTORY 2048
#define FC_GAME_MAX 32000
/* Locations: columns 0-7, free cells 8-11, foundations 12-15. */
#define FC_CELL0 8
#define FC_FOUND0 12
struct fc_move { uint8_t from, to, count; };
struct freecell {
    card_t col[FC_COLS][FC_MAX_COL]; int ncol[FC_COLS];
    card_t cell[FC_CELLS];
    int found_count[FC_FOUND]; int found_suit[FC_FOUND];
    unsigned game; int moves, won;
    struct fc_move history[FC_HISTORY]; int nhistory;
    int wins, losses;                    /* this session */
};
void fc_deal(struct freecell *, unsigned game);        /* game 1..32000, clamped */
void fc_restart(struct freecell *);                    /* the same deal again */
int fc_free_cells(const struct freecell *);
int fc_empty_columns(const struct freecell *);
int fc_max_run(const struct freecell *, int to);       /* longest run that may go to column `to` */
int fc_run_ok(const struct freecell *, int col, int count);   /* the last `count` cards form a descending alternating run */
int fc_can_move(const struct freecell *, int from, int count, int to);
int fc_move(struct freecell *, int from, int count, int to);  /* 0 when moved, -1 when illegal */
int fc_quick(struct freecell *, int from);             /* double click: to a foundation, else a column card to a free cell */
int fc_undo(struct freecell *);                        /* 0 when a move was undone */
int fc_has_moves(const struct freecell *);
int fc_is_won(const struct freecell *);
card_t fc_card_at(const struct freecell *, int loc, int index);   /* CARD_NONE when empty */

#define FC_W 600
#define FC_H 384
struct fc_app {
    struct du_ui *ui; struct freecell game;
    struct du_menu menus[2]; struct du_menu_item game_items[8], help_items[2];
    int sel_loc, sel_index;              /* the selected card or run, loc -1: none */
    int drag_loc, drag_index, dragging, drag_x, drag_y, off_x, off_y;   /* dragging -1: pressed, 1: moving */
    int last_loc, last_index; uint64_t last_ns;                          /* double click */
    void (*quit)(void *host); void *host;
};
enum { FCV_CANVAS = 1, FCV_STATUS = 20, FCV_NEW = 10, FCV_SELECT, FCV_RESTART, FCV_UNDO, FCV_EXIT, FCV_HELP, FCV_ABOUT, FCV_PICK = 30 };
void fc_attach(struct fc_app *, struct du_ui *);
void fc_event(struct du_ui *, const struct du_event *, void *);
void fc_paint(struct du_ui *, void *);
struct du_rect fc_loc_rect(const struct fc_app *, int loc, int index);
int fc_hit(const struct fc_app *, int x, int y, int *loc, int *index);   /* 1 over a location; index -1 on an empty one */
#endif
