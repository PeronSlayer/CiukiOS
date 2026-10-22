/* SPDX-License-Identifier: MIT
 * Character Map window. */
#include "charmap.h"
#include <stdio.h>
#include <string.h>
#define GRID_X 8
#define GRID_Y 46
#define CELL 21
static int rows(void) { return (CHARMAP_COUNT + CHARMAP_COLS - 1) / CHARMAP_COLS; }
struct du_rect charmap_cell(int i) { return (struct du_rect){GRID_X + (i % CHARMAP_COLS) * CELL, GRID_Y + (i / CHARMAP_COLS) * CELL, CELL, CELL}; }
void charmap_select(struct charmap_app *a, int i)
{
    if (i < 0) i = 0;
    if (i >= CHARMAP_COUNT) i = CHARMAP_COUNT - 1;
    int old=a->selected; a->selected = i;
    char text[64]; int ch = CHARMAP_FIRST + i;
    snprintf(text, sizeof(text), "U+%04X   %s%c%s   decimal %d", ch, ch == ' ' ? "(space" : "'", ch == ' ' ? ')' : ch, ch == ' ' ? "" : "'", ch);
    du_set_text(du_get(a->ui, CM_STATUS), text);
    du_invalidate(a->ui,charmap_cell(old)); du_invalidate(a->ui,charmap_cell(i));
    du_invalidate(a->ui,(struct du_rect){GRID_X+CHARMAP_COLS*CELL-64,GRID_Y+rows()*CELL+8,64,64}); du_invalidate_widget(a->ui,CM_STATUS);
}
void charmap_paint(struct du_ui *u, void *user)
{
    struct charmap_app *a = user; struct du_canvas *c = &u->canvas;
    struct du_rect grid = {GRID_X - 1, GRID_Y - 1, CHARMAP_COLS * CELL + 1, rows() * CELL + 1};
    du_fill(c, grid, DU_WINDOW);
    for (int i = 0; i < CHARMAP_COUNT; i++) {
        struct du_rect r = charmap_cell(i);
        du_outline(c, (struct du_rect){r.x - 1, r.y - 1, r.w + 1, r.h + 1}, DU_SHADOW);
        char s[2] = {(char)(CHARMAP_FIRST + i), 0};
        int sel = i == a->selected;
        if (sel) du_fill(c, r, DU_SELECT);
        du_text(c, r.x + (r.w - du_text_width(s)) / 2, r.y + (r.h - DU_GLYPH_H) / 2, s, sel ? DU_SELECT_INK : DU_INK);
    }
    /* Enlarged preview of the selected character in the theme's large face. */
    struct du_rect box = {GRID_X + CHARMAP_COLS * CELL - 64, GRID_Y + rows() * CELL + 8, 64, 64};
    du_well(c, box, DU_WINDOW);
    char s[2] = {(char)(CHARMAP_FIRST + a->selected), 0};
    int font = desk_theme->font_large;
    du_text_font(c, font, box.x + (box.w - du_text_width_font(font, s)) / 2, box.y + (box.h - desk_font_cap(font)) / 2, s, DU_INK);
}
static const struct du_menu_item help_template[] = { {"&About Character Map", CM_M_ABOUT, 0, 0, 0} };
void charmap_attach(struct charmap_app *a, struct du_ui *u)
{
    a->ui = u; u->event = charmap_event; u->user = a; u->paint = charmap_paint;
    memcpy(a->help_items, help_template, sizeof(help_template));
    a->menus[0] = (struct du_menu){"&Help", a->help_items, 1};
    u->menus = a->menus; u->menu_count = 1;
    du_add(u, DU_LABEL, CM_LABEL, (struct du_rect){8, 24, 200, 18}, "Font: Inter (CiukiOS UI)");
    du_add(u, DU_TOOLBUTTON, CM_GRID, (struct du_rect){GRID_X, GRID_Y, CHARMAP_COLS * CELL, rows() * CELL}, "");
    int y = GRID_Y + rows() * CELL + 10;
    du_add(u, DU_LABEL, CM_LABEL + 1, (struct du_rect){8, y, 140, 18}, "Characters to copy:");
    du_add(u, DU_EDIT, CM_FIELD, (struct du_rect){8, y + 20, 200, 22}, "");
    du_add(u, DU_BUTTON, CM_SELECT, (struct du_rect){214, y + 20, 64, 22}, "&Select");
    du_add(u, DU_BUTTON, CM_COPY, (struct du_rect){214, y - 6, 64, 22}, "&Copy");
    du_add(u, DU_STATUS, CM_STATUS, (struct du_rect){0, CHARMAP_H - 20, CHARMAP_W, 20}, "");
    charmap_select(a, 'A' - CHARMAP_FIRST);
    u->focus = -1;
}
static void append(struct charmap_app *a)
{
    struct du_widget *f = du_get(a->ui, CM_FIELD);
    size_t n = strlen(f->text);
    if (n + 1 >= sizeof(f->text)) return;
    f->text[n] = (char)(CHARMAP_FIRST + a->selected); f->text[n + 1] = 0;
    f->cursor = f->anchor = n + 1;
}
void charmap_event(struct du_ui *u, const struct du_event *e, void *user)
{
    struct charmap_app *a = user;
    switch (e->type) {
    case DU_EV_ACTIVATE:
        if (e->id == CM_GRID) {
            int col = (u->mouse_x - GRID_X) / CELL, row = (u->mouse_y - GRID_Y) / CELL, i = row * CHARMAP_COLS + col;
            if (col >= 0 && col < CHARMAP_COLS && row >= 0 && i < CHARMAP_COUNT) {
                /* A second click on the selected character adds it, like a double click on Windows. */
                if (i == a->selected) append(a);
                charmap_select(a, i);
            }
            u->focus = -1;
        } else if (e->id == CM_SELECT) { append(a); u->focus = -1; }
        else if (e->id == CM_COPY) { du_clipboard_set(du_get(u, CM_FIELD)->text); u->focus = -1; }
        else if (e->id == CM_M_ABOUT)
            du_message(u, 1, "About Character Map", "CiukiOS Character Map - Classic computing, one woof at a time.\nThe characters of the system font.\nCode: MIT.",
                       DU_DLG_ICON_INFO, NULL, 0, 0, 0);
        break;
    case DU_EV_KEY: {
        int d = e->key == DU_KEY_LEFT ? -1 : e->key == DU_KEY_RIGHT ? 1 : e->key == DU_KEY_UP ? -CHARMAP_COLS : e->key == DU_KEY_DOWN ? CHARMAP_COLS : 0;
        if (d) charmap_select(a, a->selected + d);
        else if (e->key == DU_KEY_ENTER || e->key == DU_KEY_SPACE) append(a);
        else if (e->ch > 32 && e->ch < 127) charmap_select(a, (int)e->ch - CHARMAP_FIRST);
        else return;
        break;
    }
    default: return;
    }
    du_invalidate_widget(u,CM_FIELD);
}
