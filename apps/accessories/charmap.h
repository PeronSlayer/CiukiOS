/* SPDX-License-Identifier: MIT
 * Character Map: the characters of the system font (printable ASCII, the
 * compositor's glyph table) in a grid, an enlarged preview, and a "Characters
 * to copy" field with Select and Copy, as the Windows 98 accessory. */
#ifndef CIUKI_ACC_CHARMAP_H
#define CIUKI_ACC_CHARMAP_H
#include "acc.h"
#define CHARMAP_W 420
#define CHARMAP_H 268
#define CHARMAP_FIRST 32
#define CHARMAP_COUNT 95
#define CHARMAP_COLS 19
struct charmap_app { struct du_ui *ui; int selected; struct du_menu menus[1]; struct du_menu_item help_items[1]; };
enum { CM_M_ABOUT = 1, CM_GRID = 100, CM_FIELD, CM_SELECT, CM_COPY, CM_STATUS, CM_LABEL };
void charmap_attach(struct charmap_app *, struct du_ui *);
void charmap_event(struct du_ui *, const struct du_event *, void *);
void charmap_paint(struct du_ui *, void *);
struct du_rect charmap_cell(int index);
void charmap_select(struct charmap_app *, int index);
#endif
