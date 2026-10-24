/* SPDX-License-Identifier: MIT
 * Serial Terminal screen (G27, AX-8): the 80x24 character grid with the TTY
 * and ANSI/VT100 subset a Windows 98 HyperTerminal "ANSI" or "TTY" session
 * uses: CR, LF, BS, TAB, BEL, FF, ESC [ n A B C D, H and f, J, K, s, u, m
 * (bold, reverse), with scroll-back. Bytes outside ASCII are shown as '?'
 * except Latin-1 letters, which are kept. */
#ifndef CIUKI_SERIAL_SCREEN_H
#define CIUKI_SERIAL_SCREEN_H
#include <stddef.h>
#include <stdint.h>
#define SCREEN_COLS 80
#define SCREEN_ROWS 24
#define SCREEN_SCROLLBACK 200
#define SCREEN_BOLD 1
#define SCREEN_REVERSE 2
struct screen_cell { uint8_t ch, attr; };
struct screen {
    struct screen_cell cells[SCREEN_ROWS][SCREEN_COLS];
    struct screen_cell history[SCREEN_SCROLLBACK][SCREEN_COLS]; unsigned history_count, history_first;
    int row, col, saved_row, saved_col, attr;
    int state, params[4], nparams, wrap_pending;
    unsigned bells;
    int ansi;                       /* 1: interpret ESC sequences; 0: plain TTY */
};
void screen_init(struct screen *, int ansi);
void screen_clear(struct screen *);
void screen_feed(struct screen *, const uint8_t *bytes, size_t length);
/* Row as text (no trailing blanks) into out (SCREEN_COLS + 1). */
void screen_row_text(const struct screen *, int row, char *out);
/* A scrolled-off line, 0 = newest; returns 0 when there is none. */
int screen_history_text(const struct screen *, unsigned back, char *out);
#endif
