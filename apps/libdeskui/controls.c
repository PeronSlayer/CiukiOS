/* SPDX-License-Identifier: MIT
 * G16 common controls: combo box, trackbar, up-down, tooltips, edit undo.
 * Behaviour and sources: docs/design/common-dialogs.md. Integer-only, no
 * allocation; transient overlays (combo list, tooltip) are drawn last. */
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COMBO_BTN 17
#define COMBO_ROWS 8
#define TB_THUMB_W 11
#define TB_THUMB_L 21
#define TIP_INITIAL_NS 500000000ull    /* TTDT_INITIAL at the default 500 ms double-click time */
#define TIP_AUTOPOP_NS 5000000000ull   /* TTDT_AUTOPOP: ten times the initial delay */
#define TIP_RESHOW_NS  100000000ull    /* TTDT_RESHOW: one fifth */
#define REPEAT_FIRST_NS 400000000ull
#define REPEAT_NEXT_NS  100000000ull
#define TIP_BACK 0xffffe1u             /* COLOR_INFOBK, Windows 98 default scheme */
#define TIP_INK  0x000000u             /* COLOR_INFOTEXT */

static int in_dialog(const struct du_ui *u, const struct du_widget *w)
{ return w >= u->dialog.items && w < u->dialog.items + DU_DIALOG_ITEMS; }

/* The widget array a control belongs to, for buddy lookup. */
static struct du_widget *sibling(struct du_ui *u, const struct du_widget *from, int id)
{
    struct du_widget *items = u->widgets; unsigned count = u->count;
    if (in_dialog(u, from)) { items = u->dialog.items; count = u->dialog.count; }
    for (unsigned i = 0; i < count; i++) if (items[i].id == id) return &items[i];
    return NULL;
}

static void clear_capture(struct du_ui *u, const struct du_widget *w)
{
    if (in_dialog(u, w)) { if (u->dialog.capture >= 0 && &u->dialog.items[u->dialog.capture] == w) u->dialog.capture = -1; }
    else if (u->capture >= 0 && (unsigned)u->capture < u->count && &u->widgets[u->capture] == w) u->capture = -1;
}

void du_set_tip(struct du_widget *w, const char *text)
{ snprintf(w->tip, sizeof(w->tip), "%s", text ? text : ""); }

/* ---- combo box -------------------------------------------------------- */
void du_combo_set(struct du_widget *w, const struct du_item *items, unsigned count, int selected)
{
    w->items = items; w->count = count; w->scroll = 0;
    w->value = selected < (int)count ? selected : -1;
    if (w->editable && w->value >= 0 && items[w->value].text[0]) du_set_text(w, items[w->value].text[0]);
}

const char *du_combo_text(const struct du_widget *w)
{
    if (w->editable) return w->text;
    return w->items && w->value >= 0 && (unsigned)w->value < w->count && w->items[w->value].text[0] ? w->items[w->value].text[0] : "";
}

static struct du_rect combo_list(const struct du_ui *u, const struct du_widget *w)
{
    int rows = (int)w->count < COMBO_ROWS ? (int)w->count : COMBO_ROWS;
    if (rows < 1) rows = 1;
    struct du_rect r = {w->box.x, w->box.y + w->box.h - 1, w->box.w, rows * DU_ROW + 4};
    if (r.y + r.h > u->canvas.height && w->box.y + 1 - r.h >= 0) r.y = w->box.y + 1 - r.h;   /* no room below: drop up */
    if (r.y + r.h > u->canvas.height) r.h = ((u->canvas.height - r.y - 4) / DU_ROW) * DU_ROW + 4;
    if (r.h < DU_ROW + 4) r.h = DU_ROW + 4;
    return r;
}
static int combo_rows(const struct du_ui *u, const struct du_widget *w)
{ struct du_rect r = combo_list(u, w); return (r.h - 4) / DU_ROW; }
static struct du_sb combo_sb(const struct du_ui *u, const struct du_widget *w)
{
    struct du_rect r = combo_list(u, w);
    return (struct du_sb){{r.x + r.w - 17, r.y + 1, 16, r.h - 2}, 1, w->scroll, (int)w->count, combo_rows(u, w)};
}
static void combo_show(struct du_ui *u, struct du_widget *w, int index)
{
    int rows = combo_rows(u, w);
    if (index < w->scroll) w->scroll = index;
    if (index >= w->scroll + rows) w->scroll = index - rows + 1;
    w->scroll = du_sb_clamp(w->scroll, (int)w->count, rows);
}

void du_combo_drop(struct du_ui *u, struct du_widget *w, int open)
{
    if (!open) {
        if (u->combo_open == w) { du_invalidate(u, combo_list(u, w)); u->combo_open = NULL; u->combo_pressed = 0; w->pressed = 0; du_invalidate(u, w->box); }
        return;
    }
    if (!w->count || w->disabled) return;
    if (u->combo_open && u->combo_open != w) du_combo_drop(u, u->combo_open, 0);
    du_tip_hide(u);
    u->combo_open = w; u->combo_hot = w->value >= 0 ? w->value : w->suggestions==2 ? -1 : 0; u->combo_pressed = 0;
    combo_show(u, w, u->combo_hot);
    du_invalidate(u, combo_list(u, w)); du_invalidate(u, w->box);
}

static void combo_choose_now(struct du_ui *u, struct du_widget *w, int index)
{
    if (index < 0 || (unsigned)index >= w->count) return;
    w->value = index;
    if (w->editable) {
        const char *s = w->items[index].text[0] ? w->items[index].text[0] : "";
        du_set_text(w, s); w->anchor = 0;   /* the text is selected, as when a list entry is picked */
    }
    du_invalidate(u, w->box);
    du_post(u, DU_EV_SELECT, w->id, index);
}
/* Called from the overlay, outside the dialog's own event capture. */
static void combo_choose(struct du_ui *u, struct du_widget *w, int index)
{
    if (in_dialog(u, w)) du_dialog_route(u, combo_choose_now, w, index);
    else combo_choose_now(u, w, index);
}

static int combo_find(const struct du_widget *w, int from, unsigned ch)
{
    if (ch >= 'A' && ch <= 'Z') ch += 32;
    for (unsigned n = 1; n <= w->count; n++) {
        int i = (from + (int)n) % (int)w->count; const char *s = w->items[i].text[0];
        unsigned c = s ? (unsigned char)s[0] : 0;
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c == ch) return i;
    }
    return -1;
}

int du_combo_key(struct du_ui *u, struct du_widget *w, unsigned code)
{
    int up = code == DU_KEY_UP, down = code == DU_KEY_DOWN;
    if (code == DU_KEY_F4 || (down && (u->modifiers & DU_ALT))) {
        if (w->suggestions) du_post(u,DU_EV_CHANGE,w->id,1);
        du_combo_drop(u, w, 1); return 1;
    }
    if (w->suggestions && (up || down)) { du_combo_drop(u,w,1); return 1; }
    if (w->count && (up || down || code == DU_KEY_HOME || code == DU_KEY_END ||
                     (!w->editable && (code == DU_KEY_LEFT || code == DU_KEY_RIGHT || code == DU_KEY_PGUP || code == DU_KEY_PGDN)))) {
        int cur = w->value, next = cur;
        if (code == DU_KEY_HOME && !w->editable) next = 0;
        else if (code == DU_KEY_END && !w->editable) next = (int)w->count - 1;
        else if (up || code == DU_KEY_LEFT) next = cur > 0 ? cur - 1 : cur < 0 ? 0 : 0;
        else if (down || code == DU_KEY_RIGHT) next = cur < (int)w->count - 1 ? cur + 1 : cur;
        else if (code == DU_KEY_PGUP) next = cur > 8 ? cur - 8 : 0;
        else if (code == DU_KEY_PGDN) next = cur + 8 < (int)w->count ? cur + 8 : (int)w->count - 1;
        else return du_edit_key(u, w, code);   /* Home and End edit the text of an editable box */
        if (next != cur) combo_choose_now(u, w, next);
        return 1;
    }
    if (w->editable) return du_edit_key(u, w, code);
    unsigned saved = u->modifiers; u->modifiers &= ~DU_ALT;
    unsigned ch = du_key_char(u, code); u->modifiers = saved;
    if (ch && w->count) {
        int hit = combo_find(w, w->value, ch);
        if (hit >= 0 && hit != w->value) combo_choose_now(u, w, hit);
        return 1;
    }
    if (code == DU_KEY_ENTER) { du_post(u, DU_EV_COMMIT, w->id, w->value); return 1; }
    return 0;
}

int du_combo_press(struct du_ui *u, struct du_widget *w, int button, int x, int y)
{
    (void)y;
    if (button != 1) return 1;
    int on_button = x >= w->box.x + w->box.w - COMBO_BTN - 2 || !w->editable;
    if (!on_button) {      /* caret placement in the edit field */
        size_t anchor = w->anchor;
        du_edit_press(w, x, 1);
        du_select_click(&w->selection, w->text, w->cursor, (u->modifiers & DU_SHIFT) != 0, u->now, x, y, &anchor, &w->cursor);
        w->anchor = anchor; w->drag = 1; du_invalidate(u, w->box);
        return 1;
    }
    clear_capture(u, w);   /* the open list takes the whole interaction, including the release */
    if (u->combo_open == w) du_combo_drop(u, w, 0);
    else {
        if (w->suggestions) du_post(u,DU_EV_CHANGE,w->id,1);
        w->pressed = 1; du_combo_drop(u, w, 1); u->combo_pressed = 1;
    }
    return 1;
}

void du_draw_combo(struct du_canvas *c, struct du_widget *w, int focused, int window_focused)
{
    struct du_rect b = w->box;
    struct du_rect btn = {b.x + b.w - COMBO_BTN - 2, b.y + 2, COMBO_BTN, b.h - 4};
    int platinum = desk_theme->control_style == DESK_CONTROLS_PLATINUM;
    if (w->editable) {
        struct du_rect whole = w->box;
        w->box.w -= COMBO_BTN; du_draw_edit(c, w, focused && window_focused); w->box = whole;
        btn = (struct du_rect){b.x + b.w - COMBO_BTN, b.y, COMBO_BTN, b.h};
    } else {
        du_well(c, b, w->disabled ? DU_FACE : DU_WINDOW);
        struct du_rect field = {b.x + 2, b.y + 2, b.w - COMBO_BTN - 4, b.h - 4};
        struct du_rect old = c->clip; c->clip = du_intersection(old, field);
        int ty = b.y + (b.h - DU_GLYPH_H) / 2;
        if (focused && window_focused && !w->disabled) {
            du_fill(c, field, DU_SELECT);
            du_text(c, b.x + 5, ty, du_combo_text(w), DU_SELECT_INK);
        } else if (w->disabled) du_text_disabled(c, b.x + 5, ty, du_combo_text(w));
        else du_text(c, b.x + 5, ty, du_combo_text(w), DU_INK);
        c->clip = old;
    }
    int down = w->pressed && !w->disabled;
    if (platinum) du_raised(c, btn, down, 0);
    else { du_fill(c, btn, DU_FACE); du_edge(c, btn, down ? DU_PRESSED : DU_BUTTON_EDGE); }
    du_triangle(c, btn.x + btn.w / 2 + down, btn.y + btn.h / 2 + down, 1, 4, w->disabled ? DU_GRAY : DU_INK);
}

static void draw_combo_list(struct du_ui *u)
{
    struct du_widget *w = u->combo_open;
    if (!w) return;
    struct du_canvas *c = &u->canvas; struct du_rect r = combo_list(u, w), keep = c->clip;
    c->clip = du_intersection(keep, r);
    if (c->clip.w <= 0 || c->clip.h <= 0) { c->clip = keep; return; }
    du_fill(c, r, DU_WINDOW); du_outline(c, r, DU_DARK);
    int rows = combo_rows(u, w), bar = (int)w->count > rows;
    int text_w = r.w - 4 - (bar ? 17 : 0);
    for (int n = 0; n < rows && w->scroll + n < (int)w->count; n++) {
        int i = w->scroll + n;
        struct du_rect row = {r.x + 2, r.y + 2 + n * DU_ROW, text_w, DU_ROW};
        int hot = i == u->combo_hot;
        if (hot) du_fill(c, row, DU_SELECT);
        struct du_rect old = c->clip; c->clip = du_intersection(old, row);
        int tx = row.x + 3;
        if (w->items[i].icon16) { du_icon_blit(c, tx, row.y + (DU_ROW - 16) / 2, w->items[i].icon16, 16, DU_ICON_NORMAL); tx += 20; }   /* Look in lists drives and folders with their icons */
        du_text(c, tx, row.y + (DU_ROW - DU_GLYPH_H) / 2, w->items[i].text[0] ? w->items[i].text[0] : "", hot ? DU_SELECT_INK : DU_INK);
        c->clip = old;
    }
    if (bar) { struct du_sb s = combo_sb(u, w); du_sb_draw(c, &s, u->combo_pressed == 3 ? 3 : 0); }
    c->clip = keep;
}

static int combo_item_at(struct du_ui *u, int x, int y)
{
    struct du_widget *w = u->combo_open; struct du_rect r = combo_list(u, w);
    int bar = (int)w->count > combo_rows(u, w);
    if (x < r.x + 2 || x >= r.x + r.w - 2 - (bar ? 17 : 0) || y < r.y + 2 || y >= r.y + r.h - 2) return -1;
    int i = w->scroll + (y - r.y - 2) / DU_ROW;
    return i < (int)w->count ? i : -1;
}

static void combo_set_hot(struct du_ui *u, int hot)
{
    if (hot == u->combo_hot) return;
    u->combo_hot = hot; du_invalidate(u, combo_list(u, u->combo_open));
}

void du_combo_overlay_key(struct du_ui *u, unsigned code)
{
    struct du_widget *w = u->combo_open; if (!w) return;
    int rows = combo_rows(u, w), last = (int)w->count - 1, hot = u->combo_hot;
    if (code == DU_KEY_ESC || code == DU_KEY_F4 || ((u->modifiers & DU_ALT) && code == DU_KEY_DOWN)) { du_combo_drop(u, w, 0); return; }
    if (code == DU_KEY_ENTER || (code == DU_KEY_SPACE && !w->suggestions)) {
        int pick = hot; du_combo_drop(u, w, 0);
        if (pick<0 && w->suggestions) du_post(u,DU_EV_COMMIT,w->id,0);
        else combo_choose(u, w, pick);
        return;
    }
    if (code == DU_KEY_TAB) { du_combo_drop(u, w, 0); return; }
    if (w->suggestions && code!=DU_KEY_UP && code!=DU_KEY_DOWN && code!=DU_KEY_PGUP && code!=DU_KEY_PGDN) {
        du_edit_key(u,w,code); return;
    }
    if (code == DU_KEY_UP) hot--; else if (code == DU_KEY_DOWN) hot++;
    else if (code == DU_KEY_PGUP) hot -= rows - 1; else if (code == DU_KEY_PGDN) hot += rows - 1;
    else if (code == DU_KEY_HOME) hot = 0; else if (code == DU_KEY_END) hot = last;
    else {
        unsigned saved = u->modifiers; u->modifiers &= ~DU_ALT;
        unsigned ch = du_key_char(u, code); u->modifiers = saved;
        int f = ch ? combo_find(w, hot, ch) : -1;
        if (f < 0) return;
        hot = f;
    }
    if (hot < 0) hot = 0;
    if (hot > last) hot = last;
    combo_show(u, w, hot); combo_set_hot(u, hot); du_invalidate(u, combo_list(u, w));
}

void du_combo_overlay_motion(struct du_ui *u)
{
    struct du_widget *w = u->combo_open; if (!w) return;
    if (u->combo_pressed == 3) {
        struct du_sb s = combo_sb(u, w); int v = du_sb_drag(&s, u->mouse_x, u->mouse_y, w->drag_offset);
        if (v != w->scroll) { w->scroll = v; du_invalidate(u, combo_list(u, w)); }
        return;
    }
    int i = combo_item_at(u, u->mouse_x, u->mouse_y);
    if (i >= 0) combo_set_hot(u, i);
}

void du_combo_overlay_button(struct du_ui *u, int button, int down)
{
    struct du_widget *w = u->combo_open; if (!w) return;
    struct du_rect list = combo_list(u, w);
    int x = u->mouse_x, y = u->mouse_y;
    if (down) {
        if (!du_contains(list, x, y)) { du_combo_drop(u, w, 0); return; }
        if (button != 1) return;
        struct du_sb s = combo_sb(u, w); int part = du_contains(s.box, x, y) ? du_sb_part(&s, x, y) : 0;
        if (part == 3) { u->combo_pressed = 3; w->drag_offset = du_sb_grab(&s, x, y); }
        else if (part) { int v = du_sb_apply(&s, part); if (v != w->scroll) { w->scroll = v; du_invalidate(u, list); } }
        else { int i = combo_item_at(u, x, y); if (i >= 0) combo_set_hot(u, i); }
        return;
    }
    if (button != 1) return;
    w->pressed = 0;
    if (u->combo_pressed == 3) { u->combo_pressed = 1; return; }
    int i = combo_item_at(u, x, y);
    if (i >= 0) { du_combo_drop(u, w, 0); combo_choose(u, w, i); }
}

void du_combo_overlay_wheel(struct du_ui *u, int detents)
{
    struct du_widget *w = u->combo_open; if (!w) return;
    int next = du_wheel_value(w->scroll, detents, (int)w->count, combo_rows(u, w));
    if (next != w->scroll) { w->scroll = next; du_invalidate(u, combo_list(u, w)); }
}

/* ---- trackbar ---------------------------------------------------------- */
static int tb_vertical(const struct du_widget *w) { return w->box.h > w->box.w; }
static int tb_line(const struct du_widget *w) { return w->line > 0 ? w->line : 1; }
static int tb_big(const struct du_widget *w)
{ if (w->big > 0) return w->big; int r = w->hi - w->lo; r = r / 5; return r > 0 ? r : 1; }   /* Wine's default page size is a fifth of the range */
static int tb_clamp(const struct du_widget *w, int v) { return v < w->lo ? w->lo : v > w->hi ? w->hi : v; }

void du_trackbar_set(struct du_widget *w, int lo, int hi, int value, int line, int big, int tick)
{
    if (hi < lo) { int t = lo; lo = hi; hi = t; }
    w->lo = lo; w->hi = hi; w->line = line; w->big = big; w->tick = tick; w->value = tb_clamp(w, value);
}

/* Thumb and channel rectangles; len is the travel of the thumb centre. */
static void tb_geometry(const struct du_widget *w, struct du_rect *thumb, struct du_rect *channel, int *origin, int *len)
{
    struct du_rect b = w->box; int span = w->hi - w->lo;
    if (!tb_vertical(w)) {
        *origin = b.x + TB_THUMB_W / 2 + 1; *len = b.w - TB_THUMB_W - 2;
        int pos = *origin + (span > 0 ? (int)((int64_t)(w->value - w->lo) * *len / span) : 0);
        int top = w->tick > 0 ? b.y + 2 : b.y + (b.h - TB_THUMB_L) / 2;
        *thumb = (struct du_rect){pos - TB_THUMB_W / 2, top, TB_THUMB_W, TB_THUMB_L};
        *channel = (struct du_rect){b.x + 2, top + 8, b.w - 4, 4};
    } else {
        *origin = b.y + TB_THUMB_W / 2 + 1; *len = b.h - TB_THUMB_W - 2;
        int pos = *origin + (span > 0 ? (int)((int64_t)(w->value - w->lo) * *len / span) : 0);
        int left = w->tick > 0 ? b.x + 2 : b.x + (b.w - TB_THUMB_L) / 2;
        *thumb = (struct du_rect){left, pos - TB_THUMB_W / 2, TB_THUMB_L, TB_THUMB_W};
        *channel = (struct du_rect){left + 8, b.y + 2, 4, b.h - 4};
    }
}

void du_draw_trackbar(struct du_canvas *c, struct du_widget *w, int focused)
{
    struct du_rect thumb, channel; int origin, len; tb_geometry(w, &thumb, &channel, &origin, &len);
    struct du_rect b = w->box;
    if (focused && desk_theme->focus_style == DESK_FOCUS_RING) du_focus_ring(c, b);
    else if (focused) du_dotted(c, b);
    du_fill(c, channel, DU_WINDOW); du_edge(c, channel, DU_SUNKEN);
    int span = w->hi - w->lo;
    if (w->tick > 0 && span > 0) {
        int vertical = tb_vertical(w);
        for (int v = w->lo;; v += w->tick) {
            if (v > w->hi) v = w->hi;
            int p = origin + (int)((int64_t)(v - w->lo) * len / span);
            if (!vertical) du_vline(c, p, thumb.y + TB_THUMB_L + 2, 3, w->disabled ? DU_GRAY : DU_DARK);
            else du_hline(c, thumb.x + TB_THUMB_L + 2, p, 3, w->disabled ? DU_GRAY : DU_DARK);
            if (v == w->hi) break;
        }
    }
    if (desk_theme->control_style == DESK_CONTROLS_PLATINUM) du_raised(c, thumb, w->pressed, 0);
    else { du_fill(c, thumb, DU_FACE); du_edge(c, thumb, DU_BUTTON_EDGE); }
}

static void tb_set(struct du_ui *u, struct du_widget *w, int v)
{
    v = tb_clamp(w, v);
    if (v == w->value) return;
    w->value = v; du_invalidate(u, w->box);
    du_post(u, DU_EV_CHANGE, w->id, v);
}

int du_trackbar_key(struct du_ui *u, struct du_widget *w, unsigned code)
{
    switch (code) {
    case DU_KEY_LEFT: case DU_KEY_UP: tb_set(u, w, w->value - tb_line(w)); return 1;
    case DU_KEY_RIGHT: case DU_KEY_DOWN: tb_set(u, w, w->value + tb_line(w)); return 1;
    case DU_KEY_PGUP: tb_set(u, w, w->value - tb_big(w)); return 1;
    case DU_KEY_PGDN: tb_set(u, w, w->value + tb_big(w)); return 1;
    case DU_KEY_HOME: tb_set(u, w, w->lo); return 1;
    case DU_KEY_END: tb_set(u, w, w->hi); return 1;
    default: return 0;
    }
}

int du_trackbar_wheel(struct du_ui *u, struct du_widget *w, int detents)
{
    int before = w->value; tb_set(u, w, w->value - detents * 3 * tb_line(w));
    return w->value != before;
}

/* Which way a click on the channel pages: -1 before the thumb, +1 after, 0 on it. */
static int tb_page_dir(const struct du_widget *w, int x, int y)
{
    struct du_rect thumb, channel; int origin, len; tb_geometry(w, &thumb, &channel, &origin, &len);
    int p = tb_vertical(w) ? y : x, lo = tb_vertical(w) ? thumb.y : thumb.x, hi = lo + TB_THUMB_W;
    return p < lo ? -1 : p >= hi ? 1 : 0;
}

static int tb_value_at(const struct du_widget *w, int x, int y, int grab)
{
    struct du_rect thumb, channel; int origin, len; tb_geometry(w, &thumb, &channel, &origin, &len);
    int p = (tb_vertical(w) ? y : x) - grab - origin, span = w->hi - w->lo;
    if (len <= 0) return w->lo;
    if (p < 0) p = 0;
    if (p > len) p = len;
    return w->lo + (int)(((int64_t)p * span * 2 + len) / (2 * (int64_t)len));
}

int du_trackbar_press(struct du_ui *u, struct du_widget *w, int button, int x, int y)
{
    if (button != 1) return 1;
    struct du_rect thumb, channel; int origin, len; tb_geometry(w, &thumb, &channel, &origin, &len);
    w->pressed = 1; w->hot = 1;
    if (du_contains(thumb, x, y)) {
        w->drag = 3; w->drag_offset = (tb_vertical(w) ? y - thumb.y : x - thumb.x) - TB_THUMB_W / 2;
        du_invalidate(u, w->box); return 1;
    }
    int dir = tb_page_dir(w, x, y);
    w->drag = 4;
    if (dir) {
        tb_set(u, w, w->value + dir * tb_big(w));
        u->repeat.w = w; u->repeat.dir = dir; u->repeat.start = u->now; u->repeat.step = 0;
        du_timer_start(u, DU_TIMER_REPEAT, REPEAT_FIRST_NS, 0);
    }
    du_invalidate(u, w->box);
    return 1;
}

void du_trackbar_motion(struct du_ui *u, struct du_widget *w, int x, int y)
{
    if (w->drag == 3) tb_set(u, w, tb_value_at(w, x, y, w->drag_offset));
}

void du_trackbar_release(struct du_ui *u, struct du_widget *w)
{
    int moved = w->drag == 3 || w->drag == 4;
    w->pressed = 0; w->drag = 0;
    du_timer_stop(u, DU_TIMER_REPEAT); u->repeat.w = NULL;
    du_invalidate(u, w->box);
    if (moved) du_post(u, DU_EV_COMMIT, w->id, w->value);
}

/* ---- up-down ----------------------------------------------------------- */
void du_updown_set(struct du_widget *w, int lo, int hi, int value, int wrap)
{
    w->lo = lo; w->hi = hi; w->wrap = wrap != 0;
    int mn = lo < hi ? lo : hi, mx = lo < hi ? hi : lo;
    w->value = value < mn ? mn : value > mx ? mx : value;
}

static void updown_text(struct du_ui *u, struct du_widget *w)
{
    struct du_widget *e = w->buddy ? sibling(u, w, w->buddy) : NULL;
    if (!e) return;
    char text[16]; snprintf(text, sizeof(text), "%d", w->value);
    if (!strcmp(e->text, text)) return;
    du_set_text(e, text); du_invalidate(u, e->box);
}

int du_updown_attach(struct du_ui *u, int updown_id, int buddy_id)
{
    struct du_widget *w = du_get(u, updown_id), *e = du_get(u, buddy_id);
    if (!w || !e || w->kind != DU_UPDOWN) return -1;
    w->buddy = buddy_id;
    int arrow = w->box.w > 0 ? w->box.w : 16;
    e->box.w -= arrow;
    w->box = (struct du_rect){e->box.x + e->box.w, e->box.y, arrow, e->box.h};
    du_invalidate(u, (struct du_rect){e->box.x, e->box.y, e->box.w + arrow, e->box.h});
    updown_text(u, w);
    return 0;
}

int du_updown_value(struct du_ui *u, struct du_widget *w)
{
    struct du_widget *e = w->buddy ? sibling(u, w, w->buddy) : NULL;
    if (e) {
        char *end; long v = strtol(e->text, &end, 10);
        while (*end == ' ') end++;
        if (e->text[0] && !*end) {
            int mn = w->lo < w->hi ? w->lo : w->hi, mx = w->lo < w->hi ? w->hi : w->lo;
            w->value = v < mn ? mn : v > mx ? mx : (int)v;
        }
    }
    return w->value;
}

/* dir +1 moves toward hi (the "up" arrow when lo < hi). */
static void updown_step(struct du_ui *u, struct du_widget *w, int dir, int amount)
{
    int v = du_updown_value(u, w);
    int mn = w->lo < w->hi ? w->lo : w->hi, mx = w->lo < w->hi ? w->hi : w->lo;
    int64_t next = (int64_t)v + (int64_t)(w->hi >= w->lo ? dir : -dir) * amount;
    if (next > mx) next = w->wrap ? mn : mx;
    if (next < mn) next = w->wrap ? mx : mn;
    if ((int)next == w->value) { updown_text(u, w); return; }
    w->value = (int)next; updown_text(u, w); du_invalidate(u, w->box);
    du_post(u, DU_EV_CHANGE, w->id, w->value);
}

/* UDACCEL defaults: 1 per step at once, 5 after 2 s, 20 after 5 s. */
static int accel(uint64_t held_ns)
{ return held_ns >= 5000000000ull ? 20 : held_ns >= 2000000000ull ? 5 : 1; }

int du_updown_press(struct du_ui *u, struct du_widget *w, int button, int x, int y)
{
    if (button != 1) return 1;
    int up = y < w->box.y + w->box.h / 2;
    (void)x;
    w->pressed = 1; w->hot = 1; w->drag = up ? 1 : 2;
    struct du_widget *e = w->buddy ? sibling(u, w, w->buddy) : NULL;
    if (e && du_focusable(e)) { if (in_dialog(u, w)) { for (unsigned i = 0; i < u->dialog.count; i++) if (&u->dialog.items[i] == e) u->dialog.focus = (int)i; }
                                else du_focus(u, e->id); }
    updown_step(u, w, up ? 1 : -1, 1);
    u->repeat.w = w; u->repeat.dir = up ? 1 : -1; u->repeat.start = u->now; u->repeat.step = 1;
    du_timer_start(u, DU_TIMER_REPEAT, REPEAT_FIRST_NS, 0);
    du_invalidate(u, w->box);
    return 1;
}

void du_updown_release(struct du_ui *u, struct du_widget *w)
{
    w->pressed = 0; w->drag = 0;
    du_timer_stop(u, DU_TIMER_REPEAT); u->repeat.w = NULL;
    du_invalidate(u, w->box);
    struct du_widget *e = w->buddy ? sibling(u, w, w->buddy) : NULL;
    if (e && e->kind == DU_EDIT) { e->anchor = 0; e->cursor = strlen(e->text); du_invalidate(u, e->box); }   /* WM_LBUTTONUP selects the buddy's text */
}

void du_updown_motion(struct du_ui *u, struct du_widget *w, int x, int y)
{
    int hot = du_contains(w->box, x, y) && (y < w->box.y + w->box.h / 2 ? 1 : 2) == w->drag;
    if (hot != w->hot) { w->hot = (uint8_t)hot; du_invalidate(u, w->box); }
}

void du_draw_updown(struct du_canvas *c, struct du_widget *w)
{
    struct du_rect b = w->box;
    int half = b.h / 2, platinum = desk_theme->control_style == DESK_CONTROLS_PLATINUM;
    struct du_rect part[2] = {{b.x, b.y, b.w, half}, {b.x, b.y + half, b.w, b.h - half}};
    for (int i = 0; i < 2; i++) {
        int down = w->pressed && w->hot && w->drag == i + 1;
        if (platinum) du_raised(c, part[i], down, 0);
        else { du_fill(c, part[i], DU_FACE); du_edge(c, part[i], down ? DU_PRESSED : DU_BUTTON_EDGE); }
        du_triangle(c, part[i].x + part[i].w / 2 + down, part[i].y + part[i].h / 2 + down, i ? 1 : 0, 3, w->disabled ? DU_GRAY : DU_INK);
    }
}

int du_updown_buddy_key(struct du_ui *u, struct du_widget *e, unsigned code)
{
    if (code != DU_KEY_UP && code != DU_KEY_DOWN) return 0;
    struct du_widget *items = u->widgets; unsigned count = u->count;
    if (in_dialog(u, e)) { items = u->dialog.items; count = u->dialog.count; }
    for (unsigned i = 0; i < count; i++)
        if (items[i].kind == DU_UPDOWN && items[i].buddy == e->id && !items[i].disabled) {
            updown_step(u, &items[i], code == DU_KEY_UP ? 1 : -1, 1);
            return 1;
        }
    return 0;
}

/* The held-button repeat of an up-down or a trackbar page click. */
void du_repeat_tick(struct du_ui *u)
{
    struct du_widget *w = u->repeat.w;
    if (!w) return;
    if (w->kind == DU_UPDOWN) {
        if (!w->pressed || !w->hot) { du_timer_start(u, DU_TIMER_REPEAT, REPEAT_NEXT_NS, 0); return; }
        updown_step(u, w, u->repeat.dir, accel(u->now - u->repeat.start));
    } else if (w->kind == DU_TRACKBAR) {
        int dir = tb_page_dir(w, u->mouse_x, u->mouse_y);
        if (dir == u->repeat.dir) tb_set(u, w, w->value + dir * tb_big(w));
        else { u->repeat.w = NULL; return; }
    } else { u->repeat.w = NULL; return; }
    du_timer_start(u, DU_TIMER_REPEAT, REPEAT_NEXT_NS, 0);
}

/* ---- edit undo (EM_UNDO) ---------------------------------------------- */
enum { UNDO_TYPING = 1, UNDO_OTHER = 2, UNDO_DELETING = 3 };

void du_undo_note(struct du_ui *u, const struct du_widget *w, const char *old_text, size_t old_cursor, size_t old_anchor, int kind)
{
    /* Consecutive typing, or consecutive Backspace/Delete, is one undo step. */
    if (u->undo.w == w && u->undo.kind == kind && kind != UNDO_OTHER) return;
    u->undo.w = w; u->undo.kind = kind; u->undo.cursor = old_cursor; u->undo.anchor = old_anchor;
    snprintf(u->undo.text, sizeof(u->undo.text), "%s", old_text);
}

int du_edit_undo(struct du_ui *u, struct du_widget *w)
{
    if (w->disabled || u->undo.w != w) return 0;
    char now[DU_TEXT_MAX]; snprintf(now, sizeof(now), "%s", w->text);
    size_t cursor = w->cursor, anchor = w->anchor;
    snprintf(w->text, sizeof(w->text), "%s", u->undo.text);
    w->cursor = u->undo.cursor; w->anchor = u->undo.anchor;
    size_t len = strlen(w->text);
    if (w->cursor > len) w->cursor = len;
    if (w->anchor > len) w->anchor = len;
    snprintf(u->undo.text, sizeof(u->undo.text), "%s", now);
    u->undo.cursor = cursor; u->undo.anchor = anchor; u->undo.kind = UNDO_OTHER;   /* the next undo redoes */
    w->offset = 0;
    du_invalidate(u, w->box);
    du_post(u, DU_EV_CHANGE, w->id, 0);
    return 1;
}

/* ---- tooltips ---------------------------------------------------------- */
static void tip_box(struct du_ui *u)
{
    int w = du_text_width(u->tip.text) + 10, h = DU_GLYPH_H + 8;
    int x = u->mouse_x, y = u->mouse_y + 21;      /* below the pointer, clear of its image */
    if (x + w > u->canvas.width) x = u->canvas.width - w;
    if (y + h > u->canvas.height) y = u->mouse_y - h - 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    u->tip.box = (struct du_rect){x, y, w, h};
}

void du_tip_hide(struct du_ui *u)
{
    if (u->tip.visible) du_invalidate(u, u->tip.box);
    u->tip.visible = 0; u->tip.w = NULL; u->tip.due = UINT64_MAX;
}

/* The widget under the pointer (or NULL). A stationary pointer shows the tip
 * after the initial delay; moving to another tool while one is shown waits only
 * the reshow delay. */
static int tip_allowed(const struct du_ui *u)
{
    return u->focused_window && !u->whats_this && !u->help_tip.visible &&
        u->menu < 0 && !u->popup && !u->combo_open && u->capture < 0 &&
        (!u->dialog.kind || (!u->dialog.dragging && u->dialog.capture < 0));
}

void du_tip_motion(struct du_ui *u, struct du_widget *under)
{
    if (!tip_allowed(u)) { du_tip_hide(u); return; }
    if (under && (!under->tip[0] || under->hidden)) under = NULL;
    if (under != u->tip.w) {
        int was = u->tip.visible;
        du_tip_hide(u);
        u->tip.w = under; u->tip.since = u->now;
        u->tip.due = under ? u->now + (was ? TIP_RESHOW_NS : TIP_INITIAL_NS) : UINT64_MAX;
        return;
    }
    if (under && !u->tip.visible && u->tip.due != UINT64_MAX) u->tip.due = u->now + TIP_INITIAL_NS;   /* only a stationary pointer shows it */
}

void du_tip_tick(struct du_ui *u)
{
    /* G15/G15B guards also apply to tooltips armed before the UI changes. */
    if (!tip_allowed(u) || (u->tip.w && u->tip.w->hidden)) { du_tip_hide(u); return; }
    if (!u->tip.w || u->tip.due == UINT64_MAX || u->now < u->tip.due) return;
    if (u->tip.visible) {            /* autopop: gone until the pointer leaves the tool */
        du_invalidate(u, u->tip.box); u->tip.visible = 0; u->tip.due = UINT64_MAX; return;
    }
    snprintf(u->tip.text, sizeof(u->tip.text), "%s", u->tip.w->tip);
    tip_box(u); u->tip.visible = 1; u->tip.due = u->now + TIP_AUTOPOP_NS;
    du_invalidate(u, u->tip.box);
}

uint64_t du_tip_deadline(const struct du_ui *u)
{ return u->tip.w ? u->tip.due : UINT64_MAX; }

/* G15 What's This? popup: the help text of the control clicked in help mode,
 * kept until the next click or key (struct du_tip help_tip, context = 1). */
#define HELP_TIP_PAPER 0xffffe1u
#define HELP_TIP_MAX_W 210
void du_help_tip_hide(struct du_ui *u)
{
    if (u->help_tip.visible) du_invalidate(u, u->help_tip.box);
    u->help_tip.visible = u->help_tip.context = 0; u->help_tip.widget = -1; u->help_tip.text = NULL;
}

void du_help_tip_show(struct du_ui *u, const char *text)
{
    if (!text || !*text) return;
    int lines = du_text_wrap(NULL, (struct du_rect){0, 0, HELP_TIP_MAX_W - 12, 4096}, text, 0);
    int w = HELP_TIP_MAX_W, h = lines * (desk_font_line(desk_theme->font_ui) + 1) + 10;
    if (w > u->canvas.width) w = u->canvas.width;
    int x = u->mouse_x, y = u->mouse_y + 20;
    if (x + w > u->canvas.width) x = u->canvas.width - w;
    if (x < 0) x = 0;
    if (y + h > u->canvas.height) y = u->mouse_y - h - 2;
    if (y < 0) y = 0;
    u->help_tip.box = (struct du_rect){x, y, w, h}; u->help_tip.text = text;
    u->help_tip.visible = u->help_tip.context = 1; u->help_tip.widget = -1; u->help_tip.hide_ns = 0;
    du_invalidate(u, u->help_tip.box);
}

static void draw_help_tip(struct du_ui *u)
{
    struct du_canvas *c = &u->canvas; struct du_rect b = u->help_tip.box;
    if (!u->help_tip.visible || !u->help_tip.text) return;
    struct du_rect clip = du_intersection(c->clip, (struct du_rect){b.x, b.y, b.w + 3, b.h + 3});
    if (clip.w <= 0 || clip.h <= 0) return;
    struct du_rect saved = c->clip;
    c->clip = du_intersection(saved, b);
    du_fill(c, b, HELP_TIP_PAPER); du_outline(c, b, 0x000000u);
    du_text_wrap(c, (struct du_rect){b.x + 6, b.y + 5, b.w - 12, b.h - 8}, u->help_tip.text, 0x000000u);
    c->clip = saved;
}

static void draw_tip(struct du_ui *u)
{
    if (!u->tip.visible) return;
    struct du_canvas *c = &u->canvas; struct du_rect r = u->tip.box, keep = c->clip;
    c->clip = du_intersection(keep, r);
    if (c->clip.w > 0 && c->clip.h > 0) {
        du_fill(c, r, TIP_BACK); du_outline(c, r, TIP_INK);
        du_text(c, r.x + 5, r.y + (r.h - DU_GLYPH_H) / 2, u->tip.text, TIP_INK);
    }
    c->clip = keep;
}

void du_draw_overlays(struct du_ui *u)
{
    struct du_rect clip = u->canvas.clip;
    draw_combo_list(u); u->canvas.clip = clip;
    draw_tip(u); u->canvas.clip = clip;
    draw_help_tip(u); u->canvas.clip = clip;
}

/* Clear every pointer into a widget array that is about to disappear. */
void du_overlays_forget(struct du_ui *u, const struct du_widget *items, unsigned count)
{
    if (u->combo_open >= items && u->combo_open < items + count) { du_invalidate(u, combo_list(u, u->combo_open)); u->combo_open = NULL; u->combo_pressed = 0; }
    if (u->tip.w >= items && u->tip.w < items + count) du_tip_hide(u);
    if (u->repeat.w >= items && u->repeat.w < items + count) { u->repeat.w = NULL; du_timer_stop(u, DU_TIMER_REPEAT); }
    if (u->undo.w >= items && u->undo.w < items + count) u->undo.w = NULL;
}
