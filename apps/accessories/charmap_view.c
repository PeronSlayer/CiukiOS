/* SPDX-License-Identifier: MIT
 * Character Map window: font and subset lists, the scrolling grid, "Go to
 * Unicode", the enlarged preview, the Alt+0nnn keystroke and "Characters to
 * copy". */
#include "charmap.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#define GRID_X 218
#define GRID_Y 46
#define CELL 20
#define LEFT_W 200
struct du_rect charmap_cell(int i) { return (struct du_rect){GRID_X + (i % CHARMAP_COLS) * CELL, GRID_Y + (i / CHARMAP_COLS) * CELL, CELL, CELL}; }
static const struct du_rect preview_box = {532, 212, 64, 64};

static void draw_char(struct charmap_app *a, struct du_canvas *c, struct du_rect box, uint32_t cp, int large, uint32_t ink)
{
    char s[5]; charmap_utf8(cp, s);
    if (!a->face) {
        if (large) {
            int font = desk_theme->font_large;
            du_text_font(c, font, box.x + (box.w - du_text_width_font(font, s)) / 2, box.y + (box.h - desk_font_cap(font)) / 2, s, ink);
        } else du_text(c, box.x + (box.w - du_text_width(s)) / 2, box.y + (box.h - DU_GLYPH_H) / 2, s, ink);
        return;
    }
    unsigned em = (large ? 40u : 15u) * 64u; int32_t adv = 0;
    if (ciuki_font_measure(a->face, em, s, strlen(s), &adv)) return;
    du_text_face(c, a->face, em, box.x + (box.w - (int)(adv >> 6)) / 2, box.y + box.h - (large ? 14 : 5), s, ink);
}
static void status(struct charmap_app *a)
{
    char text[128], key[16], s[5];
    charmap_utf8(a->selected, s);
    if (charmap_covered(a, a->selected)) snprintf(text, sizeof(text), "U+%04X   %s%s%s   decimal %u   %s", (unsigned)a->selected, a->selected == ' ' ? "(space)" : a->selected < 0x100 ? "'" : "", a->selected < 0x100 && a->selected != ' ' ? s : "", a->selected < 0x100 && a->selected != ' ' ? "'" : "", (unsigned)a->selected, charmap_subsets[a->subset].name);
    else snprintf(text, sizeof(text), "U+%04X   not in this font   %s", (unsigned)a->selected, charmap_subsets[a->subset].name);
    du_set_text(du_get(a->ui, CM_STATUS), text);
    charmap_keystroke(a->selected, key, sizeof(key));
    snprintf(text, sizeof(text), "Keystroke: %s", key[0] ? key : "none");
    du_set_text(du_get(a->ui, CM_KEYSTROKE), text);
}
static void refresh(struct charmap_app *a)
{
    status(a);
    du_invalidate(a->ui, (struct du_rect){GRID_X - 1, GRID_Y - 1, CHARMAP_COLS * CELL + 2, CHARMAP_ROWS * CELL + 2});
    du_invalidate(a->ui, preview_box);
    du_invalidate_widget(a->ui, CM_STATUS); du_invalidate_widget(a->ui, CM_KEYSTROKE); du_invalidate_widget(a->ui, CM_SCROLL);
}
void charmap_paint(struct du_ui *u, void *user)
{
    struct charmap_app *a = user; struct du_canvas *c = &u->canvas;
    const struct charmap_subset *sub = &charmap_subsets[a->subset];
    struct du_rect grid = {GRID_X - 1, GRID_Y - 1, CHARMAP_COLS * CELL + 1, CHARMAP_ROWS * CELL + 1};
    du_fill(c, grid, DU_WINDOW);
    for (int i = 0; i < CHARMAP_CELLS; i++) {
        struct du_rect r = charmap_cell(i);
        uint32_t cp = a->top + (uint32_t)i;
        du_outline(c, (struct du_rect){r.x - 1, r.y - 1, r.w + 1, r.h + 1}, DU_SHADOW);
        if (cp > sub->last) { du_fill(c, r, DU_FACE); continue; }
        int sel = cp == a->selected;
        if (sel) du_fill(c, r, DU_SELECT);
        if (charmap_covered(a, cp)) draw_char(a, c, r, cp, 0, sel ? DU_SELECT_INK : DU_INK);
    }
    du_well(c, preview_box, DU_WINDOW);
    if (charmap_covered(a, a->selected)) draw_char(a, c, preview_box, a->selected, 1, DU_INK);
}
static const struct du_menu_item help_template[] = { {"&About Character Map", CM_M_ABOUT, 0, 0, 0} };

static void add_font(struct charmap_app *a, const char *label, const char *file)
{
    if (a->font_count == CHARMAP_FONTS_MAX) return;
    struct charmap_font f; memset(&f, 0, sizeof(f));
    snprintf(f.label, sizeof(f.label), "%s", label); snprintf(f.file, sizeof(f.file), "%s", file);
    unsigned at = a->font_count++;
    while (at > 1 && strcmp(f.label, a->fonts[at - 1].label) < 0) { a->fonts[at] = a->fonts[at - 1]; at--; }
    a->fonts[at] = f;
}
static void enumerate(struct charmap_app *a)
{
    const char *dir = a->fonts_path ? a->fonts_path : "/System/fonts";
    a->font_count = 0;
    add_font(a, "Inter (CiukiOS UI)", "");
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *entry;
    while ((entry = readdir(d))) {
        size_t n = strlen(entry->d_name);
        char path[512]; struct ciuki_font_face *face; struct ciuki_font_metrics m;
        if (n < 5 || n >= sizeof(a->fonts[0].file) || strcasecmp(entry->d_name + n - 4, ".ttf")) continue;
        snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
        if (ciuki_font_open(path, &face)) continue;
        int got = ciuki_font_get_metrics(face, &m); ciuki_font_close(face);
        if (got) continue;
        char label[96]; snprintf(label, sizeof(label), "%s %s", m.family, m.style);
        add_font(a, label, entry->d_name);
    }
    closedir(d);
}
int charmap_set_font(struct charmap_app *a, unsigned index)
{
    int rc = 0;
    ciuki_font_close(a->face); a->face = NULL;
    if (index >= a->font_count) index = 0;
    if (index) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", a->fonts_path ? a->fonts_path : "/System/fonts", a->fonts[index].file);
        if (ciuki_font_open(path, &a->face)) { a->face = NULL; index = 0; rc = -1; }
    }
    a->font_selected = index;
    for (unsigned i = 0; i < a->font_count; i++) a->font_items[i].selected = i == index;
    return rc;
}
static void list_sync(struct charmap_app *a)
{
    struct du_widget *f = du_get(a->ui, CM_FONTS), *s = du_get(a->ui, CM_SUBSETS);
    if (f) { f->active = a->font_selected; du_list_ensure(f, a->font_selected); du_invalidate(a->ui, f->box); }
    if (s) { s->active = a->subset; du_list_ensure(s, a->subset); du_invalidate(a->ui, s->box); }
}
void charmap_attach(struct charmap_app *a, struct du_ui *u)
{
    a->ui = u; u->event = charmap_event; u->user = a; u->paint = charmap_paint;
    memcpy(a->help_items, help_template, sizeof(help_template));
    a->menus[0] = (struct du_menu){"&Help", a->help_items, 1};
    u->menus = a->menus; u->menu_count = 1;
    enumerate(a);
    for (unsigned i = 0; i < a->font_count; i++) a->font_items[i] = (struct du_item){.text = {a->fonts[i].label}};
    for (unsigned i = 0; i < charmap_subset_count; i++) a->subset_items[i] = (struct du_item){.text = {charmap_subsets[i].name}};
    du_add(u, DU_LABEL, CM_LABEL, (struct du_rect){8, 24, LEFT_W, 16}, "Font:");
    struct du_widget *w = du_add(u, DU_LISTVIEW, CM_FONTS, (struct du_rect){8, 42, LEFT_W, 92}, "");
    w->items = a->font_items; w->count = a->font_count; w->view = DU_LIST; w->column_count = 1; w->columns[0] = (struct du_column){"", LEFT_W - 20, 0};
    w->inactive_selection = 1;
    du_add(u, DU_LABEL, CM_LABEL + 2, (struct du_rect){8, 138, LEFT_W, 16}, "Unicode subset:");
    w = du_add(u, DU_LISTVIEW, CM_SUBSETS, (struct du_rect){8, 156, LEFT_W, 92}, "");
    w->items = a->subset_items; w->count = charmap_subset_count; w->view = DU_LIST; w->column_count = 1; w->columns[0] = (struct du_column){"", LEFT_W - 20, 0};
    w->inactive_selection = 1;
    du_add(u, DU_LABEL, CM_LABEL + 3, (struct du_rect){8, 254, 70, 18}, "Go to U+");
    du_add(u, DU_EDIT, CM_GOTO, (struct du_rect){78, 252, 84, 22}, "");
    du_add(u, DU_TOOLBUTTON, CM_GRID, (struct du_rect){GRID_X, GRID_Y, CHARMAP_COLS * CELL, CHARMAP_ROWS * CELL}, "");
    w = du_add(u, DU_SCROLLBAR, CM_SCROLL, (struct du_rect){GRID_X + CHARMAP_COLS * CELL + 1, GRID_Y, 17, CHARMAP_ROWS * CELL}, "");
    w->vertical = 1;
    int y = GRID_Y + CHARMAP_ROWS * CELL + 8;
    du_add(u, DU_LABEL, CM_LABEL + 1, (struct du_rect){GRID_X, y, 160, 18}, "Characters to copy:");
    du_add(u, DU_EDIT, CM_FIELD, (struct du_rect){GRID_X, y + 20, 232, 22}, "");
    du_add(u, DU_BUTTON, CM_SELECT, (struct du_rect){GRID_X + 240, y + 20, 64, 22}, "&Select");
    du_add(u, DU_BUTTON, CM_COPY, (struct du_rect){GRID_X + 240, y + 46, 64, 22}, "&Copy");
    du_add(u, DU_LABEL, CM_KEYSTROKE, (struct du_rect){GRID_X, y + 48, 232, 18}, "");
    du_add(u, DU_STATUS, CM_STATUS, (struct du_rect){0, CHARMAP_H - 20, CHARMAP_W, 20}, "");
    charmap_set_font(a, 0);
    charmap_set_subset(a, 0);
    charmap_select(a, 'A');
    status(a); list_sync(a);
    u->focus = -1;
}
void charmap_detach(struct charmap_app *a) { ciuki_font_close(a->face); a->face = NULL; }
static void append(struct charmap_app *a)
{
    struct du_widget *f = du_get(a->ui, CM_FIELD);
    char s[5]; size_t k = charmap_utf8(a->selected, s), n = strlen(f->text);
    if (n + k + 1 >= sizeof(f->text)) return;
    memcpy(f->text + n, s, k + 1);
    f->cursor = f->anchor = n + k;
}
static void select_font(struct charmap_app *a, unsigned index)
{
    int failed = charmap_set_font(a, index);
    charmap_select(a, a->selected);
    refresh(a); list_sync(a);
    if (failed) du_set_text(du_get(a->ui, CM_STATUS), "The font file could not be read; showing the built-in face.");
}
static void go_to(struct charmap_app *a, const char *text)
{
    char *end; unsigned long cp = strtoul(text, &end, 16);
    if (end == text || *end || cp > 0x10ffff || charmap_goto(a, (uint32_t)cp)) {
        du_set_text(du_get(a->ui, CM_STATUS), "Not in a listed Unicode subset (hexadecimal code, for example 00E9).");
        return;
    }
    refresh(a); list_sync(a);
}
void charmap_event(struct du_ui *u, const struct du_event *e, void *user)
{
    struct charmap_app *a = user;
    switch (e->type) {
    case DU_EV_ACTIVATE:
        if (e->id == CM_GRID) {
            int col = (u->mouse_x - GRID_X) / CELL, row = (u->mouse_y - GRID_Y) / CELL;
            uint32_t cp = a->top + (uint32_t)(row * CHARMAP_COLS + col);
            if (u->mouse_x >= GRID_X && u->mouse_y >= GRID_Y && col >= 0 && col < CHARMAP_COLS && row >= 0 && row < CHARMAP_ROWS &&
                cp <= charmap_subsets[a->subset].last) {
                /* A second click on the selected character adds it, like a double click on Windows. */
                if (cp == a->selected) append(a);
                charmap_select(a, cp);
                refresh(a);
            }
            u->focus = -1;
        } else if (e->id == CM_SELECT) { append(a); u->focus = -1; }
        else if (e->id == CM_COPY) { du_clipboard_set(du_get(u, CM_FIELD)->text); u->focus = -1; }
        else if (e->id == CM_M_ABOUT)
            du_message(u, 1, "About Character Map", "CiukiOS Character Map - Classic computing, one woof at a time.\nAny installed font, Unicode subsets, Go to Unicode.\nCode: MIT.",
                       DU_DLG_ICON_INFO, NULL, 0, 0, 0);
        break;
    case DU_EV_SELECT:
        if (e->id == CM_FONTS && e->index >= 0 && (unsigned)e->index < a->font_count && (unsigned)e->index != a->font_selected) select_font(a, (unsigned)e->index);
        else if (e->id == CM_SUBSETS && e->index >= 0 && (unsigned)e->index < charmap_subset_count && (unsigned)e->index != a->subset) {
            charmap_set_subset(a, (unsigned)e->index); refresh(a);
        }
        break;
    case DU_EV_CHANGE:
        if (e->id == CM_SCROLL) { charmap_scroll(a, e->index - (int)((a->top - charmap_subsets[a->subset].first) / CHARMAP_COLS)); refresh(a); }
        break;
    case DU_EV_COMMIT:
        if (e->id == CM_GOTO) go_to(a, e->text ? e->text : du_get(u, CM_GOTO)->text);
        break;
    case DU_EV_KEY: {
        uint32_t cur = a->selected; const struct charmap_subset *s = &charmap_subsets[a->subset];
        if (e->key == DU_KEY_LEFT) charmap_select(a, cur > s->first ? cur - 1 : cur);
        else if (e->key == DU_KEY_RIGHT) charmap_select(a, cur + 1);
        else if (e->key == DU_KEY_UP) charmap_select(a, cur >= s->first + CHARMAP_COLS ? cur - CHARMAP_COLS : s->first);
        else if (e->key == DU_KEY_DOWN) charmap_select(a, cur + CHARMAP_COLS);
        else if (e->key == DU_KEY_PGUP) { charmap_scroll(a, -CHARMAP_ROWS); charmap_select(a, cur >= CHARMAP_COLS * CHARMAP_ROWS + s->first ? cur - CHARMAP_COLS * CHARMAP_ROWS : s->first); }
        else if (e->key == DU_KEY_PGDN) { charmap_scroll(a, CHARMAP_ROWS); charmap_select(a, cur + CHARMAP_COLS * CHARMAP_ROWS); }
        else if (e->key == DU_KEY_HOME) charmap_select(a, s->first);
        else if (e->key == DU_KEY_END) charmap_select(a, s->last);
        else if (e->key == DU_KEY_ENTER || e->key == DU_KEY_SPACE) append(a);
        else if (e->ch > 32 && e->ch < 127) charmap_goto(a, e->ch);
        else return;
        refresh(a); list_sync(a);
        break;
    }
    default: return;
    }
    du_invalidate_widget(u, CM_FIELD);
}
