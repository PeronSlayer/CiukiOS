/* SPDX-License-Identifier: MIT
 * Minesweeper (Windows 98 "Minesweeper" equivalent): Beginner 9x9/10,
 * Intermediate 16x16/40, Expert 30x16/99 and Custom boards, a first click that
 * never loses, flags and optional ? marks, chording (both buttons or the middle
 * button on an opened number), the mine counter, the timer, the face button and
 * the Best Times table. */
#ifndef CIUKI_MINES_H
#define CIUKI_MINES_H
#include "games.h"
#define MINES_MAX_W 30
#define MINES_MAX_H 24
#define MINES_MIN_W 9
#define MINES_MIN_H 9
#define MINES_CELL 16
#define MINES_TIME_MAX 999
enum mines_state { MINES_READY, MINES_PLAYING, MINES_WON, MINES_LOST };
enum mines_level { MINES_BEGINNER, MINES_INTERMEDIATE, MINES_EXPERT, MINES_CUSTOM };
enum { MINES_CELL_MINE = 1, MINES_CELL_OPEN = 2, MINES_CELL_FLAG = 4, MINES_CELL_MARK = 8, MINES_CELL_BOOM = 16, MINES_CELL_WRONG = 32 };
struct mines_best { int seconds[3]; char name[3][32]; };
struct mines {
    int w, h, mines, level;
    enum mines_state state;
    uint8_t cell[MINES_MAX_H][MINES_MAX_W];
    int flags, opened, seconds;
    int marks_enabled;                    /* the Marks (?) menu option */
    int max_w, max_h;                     /* Custom limits: 30x24 or what the window shows */
    uint32_t seed;
    struct mines_best best;
};
void mines_init(struct mines *, uint32_t seed);
void mines_set_level(struct mines *, enum mines_level, int w, int h, int count);   /* Custom clamps like Windows */
void mines_new(struct mines *);                       /* a fresh board of the current size, state READY */
int mines_neighbours(const struct mines *, int x, int y);          /* mines around a cell */
int mines_flags_around(const struct mines *, int x, int y);
void mines_open(struct mines *, int x, int y);        /* left click; the first one is always safe */
void mines_flag(struct mines *, int x, int y);        /* right click: flag, then ? when marks are enabled, then clear */
void mines_chord(struct mines *, int x, int y);       /* both buttons on an opened number */
void mines_tick(struct mines *);                      /* one second of the timer */
int mines_remaining(const struct mines *);            /* mines minus flags, as the counter shows (may be negative) */
/* Best times: the new time of the current level when it qualifies; returns the slot (0-2) or -1. */
int mines_record(struct mines *, const char *name);
int mines_qualifies(const struct mines *);
void mines_best_reset(struct mines *);
int mines_best_load(struct mines *, const char *path);
int mines_best_save(const struct mines *, const char *path);

/* Window (mines_view.c). */
#define MINES_W 504
#define MINES_H 440
struct mines_app {
    struct du_ui *ui; struct mines game;
    struct du_menu menus[2]; struct du_menu_item game_items[13], help_items[2];
    const char *best_path;                /* where the best times persist, NULL: not saved */
    int pressed_face, hold_x, hold_y, holding;
    void (*quit)(void *host); void *host;   /* Game > Exit */
};
enum { MN_CANVAS = 1, MN_NEW = 10, MN_BEGINNER, MN_INTERMEDIATE, MN_EXPERT, MN_CUSTOM, MN_MARKS, MN_BEST, MN_EXIT, MN_ABOUT, MN_HELP, MN_CUSTOM_BOX, MN_TIMER = 1 };
void mines_attach(struct mines_app *, struct du_ui *);
void mines_event(struct du_ui *, const struct du_event *, void *);
void mines_paint(struct du_ui *, void *);
struct du_rect mines_board_rect(const struct mines *);
struct du_rect mines_face_rect(const struct mines *);
#endif
