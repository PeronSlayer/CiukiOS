/* SPDX-License-Identifier: MIT
 * libdeskui multi-line text area: editing model, word wrap layout, rendering
 * and input handling for a Notepad-class editor (f2-32).
 *
 * The buffer holds UTF-8 with "\n" line breaks. Each character occupies one
 * 6x13 cell (the compositor's 5x7 glyphs; a tab advances to the next multiple
 * of 8 cells, counted from the start of the visual line); characters the font
 * lacks (non-ASCII, control characters) are drawn as a hollow placeholder box.
 * All arithmetic is integer-only and every offset the API hands out lies on a
 * character boundary. Keys and text reach the widget through libdeskui's
 * dispatch (widgets.c); scrollbars reuse the toolkit's du_sb.
 */
#ifndef CIUKI_DESKUI_TEXTAREA_H
#define CIUKI_DESKUI_TEXTAREA_H
#include "deskui.h"

#define DU_TA_LIMIT 61440u  /* default capacity in bytes (the 0.8 CiukNote limit) */
#define DU_TA_TAB 8u
#define DU_TA_CELL_W DU_CHAR_W   /* the compositor's 5x7 glyphs at their 6 pixel advance */
#define DU_TA_CELL_H 13          /* line height */
#define DU_TA_BAR 16             /* scrollbar thickness */
#define DU_TA_PAD 2         /* text inset inside the sunken border */
#define DU_TA_DOUBLE_NS 500000000ull

enum du_ta_edit { DU_TA_EDIT_OTHER, DU_TA_EDIT_TYPE, DU_TA_EDIT_BACK, DU_TA_EDIT_FORWARD };
enum du_ta_motion { DU_TA_LEFT, DU_TA_RIGHT, DU_TA_UP, DU_TA_DOWN, DU_TA_HOME, DU_TA_END,
                    DU_TA_DOC_START, DU_TA_DOC_END, DU_TA_PAGE_UP, DU_TA_PAGE_DOWN,
                    DU_TA_WORD_LEFT, DU_TA_WORD_RIGHT };
enum du_ta_drag { DU_TA_DRAG_NONE, DU_TA_DRAG_SELECT, DU_TA_DRAG_VTHUMB, DU_TA_DRAG_HTHUMB };

struct du_textarea {
    char *text;                 /* cap+1 bytes, always NUL terminated */
    size_t len, cap;
    size_t cursor, anchor;      /* selection is [min,max) */
    unsigned lf;                /* number of "\n" in text */
    int wrap, bold, readonly, modified, limit_hit;
    unsigned top, left;         /* first visible visual line / cell column */
    unsigned want_col; int want_valid;
    int affinity;               /* caret drawn at the end of the previous soft-wrapped line */
    /* layout */
    uint32_t *vl; unsigned vl_count, vl_cap; int vl_dirty; unsigned vl_cols, maxcol;
    unsigned cols, rows;        /* last known viewport in cells */
    /* one undo level; undo again redoes */
    char *undo; size_t undo_len, undo_cursor, undo_anchor;
    int undo_valid, undo_kind; size_t edit_cursor;
    /* pointer */
    int drag, grab; uint64_t click_ns; int click_x, click_y, clicks;
};

int du_ta_init(struct du_textarea *, size_t cap);
void du_ta_free(struct du_textarea *);
int du_ta_set_text(struct du_textarea *, const char *, size_t); /* load: clears undo and modified */
void du_ta_set_wrap(struct du_textarea *, int);
void du_ta_set_view(struct du_textarea *, unsigned cols, unsigned rows);
void du_ta_fit(struct du_textarea *, struct du_rect box); /* derive the viewport from the widget box */

/* Editing. Insert replaces the selection; it returns -1, changing nothing and
 * setting limit_hit, when the result would exceed the capacity. */
int du_ta_insert(struct du_textarea *, const char *, size_t, enum du_ta_edit);
int du_ta_backspace(struct du_textarea *);
int du_ta_delete(struct du_textarea *);
int du_ta_delete_selection(struct du_textarea *);
int du_ta_undo(struct du_textarea *);
int du_ta_can_undo(const struct du_textarea *);

/* Selection and movement. */
int du_ta_has_selection(const struct du_textarea *);
void du_ta_selection(const struct du_textarea *, size_t *, size_t *);
void du_ta_select_all(struct du_textarea *);
void du_ta_select(struct du_textarea *, size_t anchor, size_t cursor);
void du_ta_move(struct du_textarea *, enum du_ta_motion, int extend);
void du_ta_select_word(struct du_textarea *, size_t at);
void du_ta_line_col(const struct du_textarea *, size_t at, unsigned *line, unsigned *col);
int du_ta_goto_line(struct du_textarea *, unsigned line);
unsigned du_ta_lines(const struct du_textarea *);
void du_ta_reveal(struct du_textarea *);

/* Search. Forward matches start at or after `from`; backward matches end at or
 * before it. Case folding is ASCII only. */
long du_ta_find(const struct du_textarea *, const char *, size_t, size_t from, int match_case, int backward);
int du_ta_find_select(struct du_textarea *, const char *, size_t, int match_case, int backward);
int du_ta_selection_equals(const struct du_textarea *, const char *, size_t, int match_case);
/* Replace All: returns the number of replacements, or -1 when the result would
 * exceed the capacity (nothing changes). One undo step. */
long du_ta_replace_all(struct du_textarea *, const char *, size_t, const char *, size_t, int match_case);

/* Layout (computed lazily from the view width when wrap is on). */
unsigned du_ta_visual_lines(struct du_textarea *);
unsigned du_ta_visual_line_of(struct du_textarea *, size_t at, unsigned *col);
size_t du_ta_hit(struct du_textarea *, struct du_rect box, int x, int y);

/* Widget behaviour; wired into libdeskui for widgets of kind DU_TEXTAREA. */
void du_ta_draw(struct du_canvas *, struct du_rect box, struct du_textarea *, int focused);
int du_ta_key(struct du_textarea *, unsigned code, unsigned modifiers); /* editing and movement keys */
int du_ta_text(struct du_textarea *, unsigned ch);                     /* a character from du_key_char */
int du_ta_mouse_down(struct du_textarea *, struct du_rect box, int x, int y, unsigned modifiers, uint64_t ns);
int du_ta_mouse_move(struct du_textarea *, struct du_rect box, int x, int y);
void du_ta_mouse_up(struct du_textarea *);
#endif
