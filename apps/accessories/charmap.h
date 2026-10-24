/* SPDX-License-Identifier: MIT
 * Character Map (G27, AX-5/FT-7): any installed font (the built-in CiukiOS UI
 * face and every TrueType face of /System/fonts), the Unicode subsets a
 * Windows 98 user reaches through "Group by Unicode subrange" (Basic Latin,
 * Latin-1 Supplement with the Italian accented letters, Latin Extended,
 * Greek, Cyrillic, punctuation, currency, arrows, mathematics, box drawing,
 * symbols), a scrolling grid, "Go to Unicode", an enlarged preview, the
 * Alt+0nnn keystroke of the Windows set, and "Characters to copy" with Select
 * and Copy (UTF-8 on the clipboard). */
#ifndef CIUKI_ACC_CHARMAP_H
#define CIUKI_ACC_CHARMAP_H
#include "acc.h"
#include <ciuki/font.h>
#define CHARMAP_W 560
#define CHARMAP_H 316
#define CHARMAP_COLS 18
#define CHARMAP_ROWS 8
#define CHARMAP_CELLS (CHARMAP_COLS * CHARMAP_ROWS)
#define CHARMAP_FONTS_MAX 32
#define CHARMAP_BUILTIN 0        /* font index 0: the built-in CiukiOS UI face (bitmap, Basic Latin + Latin-1) */
struct charmap_subset { const char *name; uint32_t first, last; };
extern const struct charmap_subset charmap_subsets[];
extern const unsigned charmap_subset_count;
struct charmap_font { char label[96]; char file[64]; };   /* file empty: the built-in face */
struct charmap_app {
    struct du_ui *ui;
    struct du_menu menus[1]; struct du_menu_item help_items[1];
    const char *fonts_path;                               /* NULL: /System/fonts */
    struct charmap_font fonts[CHARMAP_FONTS_MAX]; unsigned font_count, font_selected;
    struct du_item font_items[CHARMAP_FONTS_MAX], subset_items[32];
    struct ciuki_font_face *face;                         /* selected TrueType face, NULL for the built-in one */
    unsigned subset;                                      /* index into charmap_subsets */
    uint32_t top;                                         /* code point of the first cell of the grid */
    uint32_t selected;
};
enum { CM_M_ABOUT = 1, CM_GRID = 100, CM_FIELD, CM_SELECT, CM_COPY, CM_STATUS, CM_LABEL, CM_FONTS = 110, CM_SUBSETS, CM_SCROLL, CM_GOTO, CM_KEYSTROKE };
void charmap_attach(struct charmap_app *, struct du_ui *);
void charmap_detach(struct charmap_app *);
void charmap_event(struct du_ui *, const struct du_event *, void *);
void charmap_paint(struct du_ui *, void *);
struct du_rect charmap_cell(int index);
/* Model (charmap_model.c). */
size_t charmap_utf8(uint32_t cp, char *out);              /* out holds 5 bytes */
int charmap_covered(const struct charmap_app *, uint32_t cp);   /* the font has a glyph for cp */
int charmap_keystroke(uint32_t cp, char *out, size_t cap);      /* "Alt+0233" for the Windows character set; 0 when none */
void charmap_select(struct charmap_app *, uint32_t cp);   /* clamps to the current subset and scrolls into view */
void charmap_set_subset(struct charmap_app *, unsigned subset);
int charmap_goto(struct charmap_app *, uint32_t cp);      /* switches subset when needed; -1 when no listed subset holds cp */
void charmap_scroll(struct charmap_app *, int rows);      /* moves the grid by rows within the subset */
unsigned charmap_total_rows(const struct charmap_app *);
int charmap_set_font(struct charmap_app *, unsigned index);   /* 0 on success; the built-in face when a file fails */
#endif
