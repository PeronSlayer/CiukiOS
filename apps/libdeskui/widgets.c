/* SPDX-License-Identifier: MIT
 * Widget registry, input dispatch, focus/release, timers and simple widgets. */
#include "internal.h"
#include "textarea.h"
#include <stdio.h>
#include <string.h>

static char clipboard[DU_TEXT_MAX];
void du_clipboard_set(const char *s) { snprintf(clipboard, sizeof(clipboard), "%s", s ? s : ""); }
const char *du_clipboard_get(void) { return clipboard; }

void du_emit(struct du_ui *u, struct du_event e)
{
    if (!e.x && !e.y) { e.x = u->mouse_x; e.y = u->mouse_y; }
    if (!e.modifiers) e.modifiers = u->modifiers;
    u->dirty = 1;
    if (u->event) u->event(u, &e, u->user);
}

void du_post(struct du_ui *u, enum du_event_type type, int id, int index)
{ du_emit(u, (struct du_event){.type = type, .id = id, .index = index}); }

void du_init(struct du_ui *u, uint32_t *pixels, int width, int height)
{
    memset(u, 0, sizeof(*u));
    u->canvas = (struct du_canvas){pixels, width, height, {0, 0, width, height}};
    u->focus = u->capture = u->hover = u->menu = u->popup_owner = -1;
    u->last_click_widget = -1; u->focused_window = 1; u->dirty = 1;
}

struct du_widget *du_add(struct du_ui *u, enum du_kind kind, int id, struct du_rect r, const char *text)
{
    if (u->count == DU_WIDGETS || du_get(u, id)) return NULL;
    struct du_widget *w = &u->widgets[u->count++];
    memset(w, 0, sizeof(*w));
    w->kind = kind; w->id = id; w->box = r; w->vertical = 1; w->sort = -1; w->focus_rect = 1;
    du_set_text(w, text);
    return w;
}

struct du_widget *du_get(struct du_ui *u, int id)
{
    for (unsigned i = 0; i < u->count; i++) if (u->widgets[i].id == id) return &u->widgets[i];
    return NULL;
}

void du_set_text(struct du_widget *w, const char *s)
{
    snprintf(w->text, sizeof(w->text), "%s", s ? s : "");
    w->cursor = w->anchor = strlen(w->text); w->offset = 0;
}

int du_focusable(const struct du_widget *w)
{
    return !w->hidden && !w->disabled && (w->kind == DU_EDIT || w->kind == DU_LISTVIEW ||
           w->kind == DU_TREE || w->kind == DU_BUTTON || w->kind == DU_TABS || w->kind == DU_TEXTAREA);
}

int du_hit(const struct du_ui *u, int x, int y)
{
    for (unsigned i = u->count; i > 0; i--) {
        const struct du_widget *w = &u->widgets[i - 1];
        if (!w->hidden && du_contains(w->box, x, y)) return (int)i - 1;
    }
    return -1;
}

void du_focus(struct du_ui *u, int id)
{
    for (unsigned i = 0; i < u->count; i++) if (u->widgets[i].id == id) {
        if (u->focus != (int)i) { u->focus = (int)i; u->dirty = 1; }
        return;
    }
}

int du_focused(const struct du_ui *u)
{ return u->focus >= 0 && (unsigned)u->focus < u->count ? u->widgets[u->focus].id : 0; }

void du_release(struct du_ui *u)
{
    memset(u->keys, 0, sizeof(u->keys));
    u->modifiers = u->buttons = 0; u->menu_armed = 0;
    if (u->capture >= 0 && (unsigned)u->capture < u->count) {
        struct du_widget *w = &u->widgets[u->capture];
        w->pressed = 0; w->drag = 0;
    }
    for (unsigned i = 0; i < u->dialog.count; i++) { u->dialog.items[i].pressed = 0; u->dialog.items[i].drag = 0; }
    u->dialog.dragging = 0;
    u->capture = -1; u->dirty = 1;
}

void du_window_focus(struct du_ui *u, int focused)
{
    if (!focused) { du_release(u); du_menu_close(u); }
    if (u->focused_window != focused) {
        u->focused_window = focused;
        du_post(u, DU_EV_FOCUS, 0, focused);
    }
    u->dirty = 1;
}

unsigned du_key_char(const struct du_ui *u, unsigned code)
{
    static const char plain[] = "\0\0" "1234567890-=\0\0" "qwertyuiop[]\0\0" "asdfghjkl;'`\0\\" "zxcvbnm,./";
    static const char shift[] = "\0\0" "!@#$%^&*()_+\0\0" "QWERTYUIOP{}\0\0" "ASDFGHJKL:\"~\0|" "ZXCVBNM<>?";
    if (u->modifiers & (DU_CTRL | DU_ALT)) return 0;
    if (code == DU_KEY_SPACE) return ' ';
    if (code >= sizeof(plain) - 1 || !plain[code]) return 0;
    unsigned ch = (unsigned char)((u->modifiers & DU_SHIFT) ? shift[code] : plain[code]);
    if (u->caps && ch >= 'a' && ch <= 'z') ch -= 32;
    else if (u->caps && ch >= 'A' && ch <= 'Z') ch += 32;
    return ch;
}

/* ---- timers ---- */
int du_timer_start(struct du_ui *u, int id, uint64_t interval, int repeat)
{
    if (!interval || UINT64_MAX - u->now < interval) return -1;
    int slot = -1;
    for (int i = 0; i < DU_TIMERS; i++) {
        if (u->timers[i].active && u->timers[i].id == id) { slot = i; break; }
        if (!u->timers[i].active && slot < 0) slot = i;
    }
    if (slot < 0) return -1;
    u->timers[slot] = (struct du_timer){u->now + interval, interval, id, 1, repeat};
    return 0;
}

void du_timer_stop(struct du_ui *u, int id)
{ for (int i = 0; i < DU_TIMERS; i++) if (u->timers[i].id == id) u->timers[i].active = 0; }

void du_tick(struct du_ui *u, uint64_t now)
{
    if (now > u->now) u->now = now;
    for (int i = 0; i < DU_TIMERS; i++) {
        struct du_timer *t = &u->timers[i];
        if (!t->active || u->now < t->deadline) continue;
        int id = t->id;
        if (t->repeat) t->deadline = UINT64_MAX - u->now < t->interval ? UINT64_MAX : u->now + t->interval;
        else t->active = 0;
        du_emit(u, (struct du_event){.type = DU_EV_TICK, .id = id});
    }
}

uint64_t du_next_deadline(const struct du_ui *u)
{
    uint64_t next = UINT64_MAX;
    for (int i = 0; i < DU_TIMERS; i++) if (u->timers[i].active && u->timers[i].deadline < next) next = u->timers[i].deadline;
    return next;
}

/* ---- edit ---- */
static size_t sel_lo(const struct du_widget *w) { return w->cursor < w->anchor ? w->cursor : w->anchor; }
static size_t sel_hi(const struct du_widget *w) { return w->cursor > w->anchor ? w->cursor : w->anchor; }

void du_edit_insert(struct du_widget *w, const char *s)
{
    size_t a = sel_lo(w), b = sel_hi(w), len = strlen(w->text), n = 0;
    while (s[n] && s[n] != '\n' && s[n] != '\r') n++;
    if (len - (b - a) + n >= sizeof(w->text)) return;
    memmove(w->text + a + n, w->text + b, len - b + 1);
    memcpy(w->text + a, s, n);
    w->cursor = w->anchor = a + n;
}

static int text_x(const struct du_widget *w) { return w->box.x + 4 + (w->icon ? 20 : 0); }

void du_edit_press(struct du_widget *w, int x, int extend)
{
    int cell = (x - text_x(w) + DU_CHAR_W / 2) / DU_CHAR_W + (int)w->offset;
    size_t at = 0;
    for (int n = 0; n < cell && w->text[at]; n++) at = du_utf8_next(w->text, at);
    w->cursor = at;
    if (!extend) w->anchor = at;
}

int du_edit_key(struct du_ui *u, struct du_widget *w, unsigned code)
{
    size_t len = strlen(w->text);
    int shift = (u->modifiers & DU_SHIFT) != 0, ctrl = (u->modifiers & DU_CTRL) != 0;
    unsigned ch = du_key_char(u, code);
    if (ch) { char s[2] = {(char)ch, 0}; du_edit_insert(w, s); }
    else if (ctrl && !(u->modifiers & DU_ALT) && code == 0x1e) { w->anchor = 0; w->cursor = len; }
    else if (ctrl && (code == 0x2e || code == 0x2d)) {
        size_t a = sel_lo(w), b = sel_hi(w);
        if (b > a) {
            char copy[DU_TEXT_MAX]; memcpy(copy, w->text + a, b - a); copy[b - a] = 0;
            du_clipboard_set(copy);
            if (code == 0x2d) du_edit_insert(w, "");
        }
    } else if (ctrl && code == 0x2f) du_edit_insert(w, du_clipboard_get());
    else if (code == DU_KEY_LEFT || code == DU_KEY_RIGHT) {
        if (!shift && w->cursor != w->anchor) w->cursor = code == DU_KEY_LEFT ? sel_lo(w) : sel_hi(w);
        else w->cursor = code == DU_KEY_LEFT ? du_utf8_prev(w->text, w->cursor) : du_utf8_next(w->text, w->cursor);
        if (!shift) w->anchor = w->cursor;
    } else if (code == DU_KEY_HOME || code == DU_KEY_END) {
        w->cursor = code == DU_KEY_HOME ? 0 : len;
        if (!shift) w->anchor = w->cursor;
    } else if (code == DU_KEY_BACKSPACE || code == DU_KEY_DELETE) {
        if (w->cursor == w->anchor)
            w->anchor = code == DU_KEY_BACKSPACE ? du_utf8_prev(w->text, w->cursor) : du_utf8_next(w->text, w->cursor);
        du_edit_insert(w, "");
    } else if (code == DU_KEY_ENTER) { du_post(u, DU_EV_COMMIT, w->id, 0); return 1; }
    else if (code == DU_KEY_ESC) { du_post(u, DU_EV_CANCEL, w->id, 0); return 1; }
    else return 0;
    if (ch || code == DU_KEY_BACKSPACE || code == DU_KEY_DELETE || (ctrl && (code == 0x2d || code == 0x2f)))
        du_post(u, DU_EV_CHANGE, w->id, 0);
    u->dirty = 1;
    return 1;
}

void du_draw_edit(struct du_canvas *c, struct du_widget *w, int focused)
{
    struct du_rect b = w->box;
    if (w->def) { du_fill(c, b, DU_WINDOW); du_outline(c, b, DU_DARK); }
    else { du_fill(c, b, w->disabled ? DU_FACE : DU_WINDOW); du_edge(c, b, DU_SUNKEN); }
    struct du_rect old = c->clip;
    c->clip = du_intersection(old, (struct du_rect){b.x + 2, b.y + 2, b.w - 4, b.h - 4});
    if (w->icon) du_icon(c, b.x + 3, b.y + (b.h - 16) / 2, w->icon, 16, DU_ICON_NORMAL);
    int x = text_x(w), y = b.y + (b.h - DU_GLYPH_H) / 2;
    size_t room = (size_t)((b.x + b.w - 3 - x) / DU_CHAR_W), cursor_cells = 0;
    if (!room) room = 1;
    for (size_t i = 0; i < w->cursor && w->text[i]; i = du_utf8_next(w->text, i)) cursor_cells++;
    if (cursor_cells < w->offset) w->offset = cursor_cells;
    if (cursor_cells > w->offset + room - 1) w->offset = cursor_cells - room + 1;
    size_t start = 0;
    for (size_t n = 0; n < w->offset && w->text[start]; n++) start = du_utf8_next(w->text, start);
    size_t lo = 0, hi = 0, cells = 0;
    for (size_t i = 0; w->text[i]; i = du_utf8_next(w->text, i), cells++) {
        if (i == sel_lo(w)) lo = cells;
        if (i == sel_hi(w)) hi = cells;
    }
    if (sel_lo(w) >= strlen(w->text)) lo = cells;
    if (sel_hi(w) >= strlen(w->text)) hi = cells;
    uint32_t ink = w->disabled ? DU_GRAY : DU_INK;
    du_text(c, x, y, w->text + start, ink);
    if (focused && hi > lo) {
        size_t a = lo > w->offset ? lo : w->offset;
        if (hi > a) {
            struct du_rect r = {x + (int)(a - w->offset) * DU_CHAR_W - 1, y - 2, (int)(hi - a) * DU_CHAR_W + 1, DU_GLYPH_H + 4};
            du_fill(c, r, DU_SELECT);
            struct du_rect keep = c->clip; c->clip = du_intersection(keep, r);
            du_text(c, x, y, w->text + start, DU_SELECT_INK);
            c->clip = keep;
        }
    }
    if (focused) du_vline(c, x + (int)(cursor_cells - w->offset) * DU_CHAR_W - 1, y - 2, DU_GLYPH_H + 4, DU_INK);
    c->clip = old;
}

/* ---- buttons, status, progress, scrollbars ---- */
void du_draw_button(struct du_canvas *c, const struct du_widget *w, int focused)
{
    struct du_rect b = w->box;
    int down = w->pressed && w->hot;
    du_fill(c, b, DU_FACE);
    if (down) du_edge(c, b, DU_PRESSED);
    else if (w->def || focused) { du_outline(c, b, DU_DARK); du_edge(c, (struct du_rect){b.x + 1, b.y + 1, b.w - 2, b.h - 2}, DU_BUTTON_EDGE); }
    else du_edge(c, b, DU_BUTTON_EDGE);
    int tw = du_text_width(w->text) - (strchr(w->text, '&') ? DU_CHAR_W : 0);
    int iw = w->icon ? w->icon_size + 4 : 0;
    int x = b.x + (b.w - tw - iw) / 2 + down, y = b.y + (b.h - DU_GLYPH_H) / 2 + down;
    if (w->icon) du_icon(c, x, b.y + (b.h - w->icon_size) / 2 + down, w->icon, w->icon_size, w->disabled ? DU_ICON_DISABLED : DU_ICON_NORMAL);
    if (w->disabled) { du_label(c, x + iw + 1, y + 1, w->text, DU_HILITE, 1); du_label(c, x + iw, y, w->text, DU_GRAY, 1); }
    else du_label(c, x + iw, y, w->text, DU_INK, 1);
    if (focused) du_dotted(c, (struct du_rect){b.x + 4, b.y + 4, b.w - 8, b.h - 8});
}

static void draw_toolbutton(struct du_canvas *c, const struct du_widget *w)
{
    struct du_rect b = w->box;
    int down = w->pressed && w->hot && !w->disabled;
    int split = w->dropdown > 0 ? w->dropdown : 0;
    struct du_rect main = {b.x, b.y, b.w - split, b.h};
    if (down && w->drag != 2) du_edge(c, main, DU_SUNKEN_THIN);
    else if ((w->hot || w->drag == 2) && !w->disabled) du_edge(c, main, DU_RAISED_THIN);
    if (split) {
        struct du_rect arrow = {b.x + b.w - split, b.y, split, b.h};
        if (w->drag == 2) du_edge(c, arrow, DU_SUNKEN_THIN);
        else if (w->hot && !w->disabled) du_edge(c, arrow, DU_RAISED_THIN);
        du_triangle(c, arrow.x + split / 2, b.y + b.h / 2, 1, 3, w->disabled ? DU_GRAY : DU_INK);
    }
    int off = down && w->drag != 2;
    int size = w->icon_size ? w->icon_size : 22;
    int has_text = w->text[0] != 0;
    int ix = main.x + (main.w - size) / 2 + off, iy = has_text ? b.y + 3 + off : b.y + (b.h - size) / 2 + off;
    du_icon(c, ix, iy, w->icon, size, w->disabled ? DU_ICON_DISABLED : DU_ICON_NORMAL);
    if (has_text) {
        int tx = main.x + (main.w - du_text_width(w->text)) / 2 + off, ty = b.y + size + 7 + off;
        if (w->disabled) du_text_disabled(c, tx, ty, w->text); else du_text(c, tx, ty, w->text, DU_INK);
    }
}

static void draw_status(struct du_canvas *c, const struct du_widget *w)
{
    struct du_rect b = w->box; int x = b.x;
    du_fill(c, b, DU_FACE);
    const char *s = w->text;
    for (int i = 0; i < 4; i++) {
        int width = w->panels[i] > 0 ? w->panels[i] : b.x + b.w - x;
        if (width <= 0) break;
        struct du_rect panel = {x, b.y + 2, width - 2, b.h - 2};
        du_edge(c, panel, DU_SUNKEN_THIN);
        const char *end = strchr(s, '\t');
        char part[256]; size_t n = end ? (size_t)(end - s) : strlen(s);
        if (n >= sizeof(part)) n = sizeof(part) - 1;
        memcpy(part, s, n); part[n] = 0;
        struct du_rect old = c->clip; c->clip = du_intersection(old, panel);
        du_text_fit(c, panel.x + 4, panel.y + (panel.h - DU_GLYPH_H) / 2, panel.w - 6, part, DU_INK);
        c->clip = old;
        x += width;
        if (!end || w->panels[i] <= 0) break;
        s = end + 1;
    }
}

static void draw_progress(struct du_canvas *c, const struct du_widget *w)
{
    struct du_rect b = w->box;
    du_fill(c, b, DU_FACE); du_edge(c, b, DU_SUNKEN_THIN);
    int inner = b.w - 4, filled = w->total > 0 ? (int)((int64_t)inner * (w->value > w->total ? w->total : w->value) / w->total) : 0;
    for (int x = 0; x < filled; x += 10) {
        int bw = filled - x < 8 ? filled - x : 8;
        du_fill(c, (struct du_rect){b.x + 2 + x, b.y + 2, bw, b.h - 4}, DU_SELECT);
    }
}

int du_sb_clamp(int value, int total, int page)
{
    int max = total > page ? total - page : 0;
    return value < 0 ? 0 : value > max ? max : value;
}

static void sb_parts(const struct du_sb *s, int *start, int *track, int *thumb, int *at)
{
    int length = s->vertical ? s->box.h : s->box.w, arrow = s->vertical ? s->box.w : s->box.h;
    if (arrow * 2 > length) arrow = length / 2;
    *start = arrow; *track = length - 2 * arrow;
    int total = s->total > 0 ? s->total : 1, page = s->page > 0 ? s->page : 1;
    *thumb = total > page ? (int)((int64_t)*track * page / total) : *track;
    if (*thumb < 8) *thumb = *track < 8 ? *track : 8;
    int range = total - page, value = du_sb_clamp(s->value, total, page);
    *at = range > 0 ? (int)((int64_t)(*track - *thumb) * value / range) : 0;
}

void du_sb_draw(struct du_canvas *c, const struct du_sb *s, int pressed)
{
    int start, track, thumb, at;
    sb_parts(s, &start, &track, &thumb, &at);
    struct du_rect b = s->box, dec, inc, tr, th;
    if (s->vertical) {
        dec = (struct du_rect){b.x, b.y, b.w, start}; inc = (struct du_rect){b.x, b.y + b.h - start, b.w, start};
        tr = (struct du_rect){b.x, b.y + start, b.w, track}; th = (struct du_rect){b.x, b.y + start + at, b.w, thumb};
    } else {
        dec = (struct du_rect){b.x, b.y, start, b.h}; inc = (struct du_rect){b.x + b.w - start, b.y, start, b.h};
        tr = (struct du_rect){b.x + start, b.y, track, b.h}; th = (struct du_rect){b.x + start + at, b.y, thumb, b.h};
    }
    du_checker(c, tr, DU_HILITE, DU_FACE);
    if (pressed == 2) du_fill(c, (struct du_rect){tr.x, tr.y, s->vertical ? tr.w : th.x - tr.x, s->vertical ? th.y - tr.y : tr.h}, DU_DARK);
    if (pressed == 4) du_fill(c, s->vertical ? (struct du_rect){tr.x, th.y + th.h, tr.w, tr.y + tr.h - th.y - th.h}
                                             : (struct du_rect){th.x + th.w, tr.y, tr.x + tr.w - th.x - th.w, tr.h}, DU_DARK);
    int enabled = s->total > s->page;
    struct du_rect arrows[2] = {dec, inc};
    for (int i = 0; i < 2; i++) {
        struct du_rect a = arrows[i];
        du_fill(c, a, DU_FACE);
        du_edge(c, a, pressed == (i ? 5 : 1) ? DU_PRESSED : DU_BUTTON_EDGE);
        int off = pressed == (i ? 5 : 1);
        int dir = s->vertical ? (i ? 1 : 0) : (i ? 3 : 2);
        uint32_t ink = enabled ? DU_INK : DU_GRAY;
        if (!enabled) du_triangle(c, a.x + a.w / 2 + 1, a.y + a.h / 2 + 1, dir, 4, DU_HILITE);
        du_triangle(c, a.x + a.w / 2 + off, a.y + a.h / 2 + off, dir, 4, ink);
    }
    if (enabled && track > 0) { du_fill(c, th, DU_FACE); du_edge(c, th, DU_BUTTON_EDGE); }
}

int du_sb_part(const struct du_sb *s, int x, int y)
{
    if (!du_contains(s->box, x, y)) return 0;
    int start, track, thumb, at;
    sb_parts(s, &start, &track, &thumb, &at);
    int p = s->vertical ? y - s->box.y : x - s->box.x;
    if (p < start) return 1;
    if (p >= start + track) return 5;
    if (s->total <= s->page) return 0;
    if (p < start + at) return 2;
    if (p < start + at + thumb) return 3;
    return 4;
}

int du_sb_apply(const struct du_sb *s, int part)
{
    int step = part == 1 ? -1 : part == 5 ? 1 : part == 2 ? -(s->page > 1 ? s->page - 1 : 1) : part == 4 ? (s->page > 1 ? s->page - 1 : 1) : 0;
    return du_sb_clamp(s->value + step, s->total, s->page);
}

int du_sb_grab(const struct du_sb *s, int x, int y)
{
    int start, track, thumb, at;
    sb_parts(s, &start, &track, &thumb, &at);
    return (s->vertical ? y - s->box.y : x - s->box.x) - start - at;
}

int du_sb_drag(const struct du_sb *s, int x, int y, int grab)
{
    int start, track, thumb, at;
    sb_parts(s, &start, &track, &thumb, &at);
    int p = (s->vertical ? y - s->box.y : x - s->box.x) - start - grab, room = track - thumb;
    if (room <= 0) return 0;
    if (p < 0) p = 0;
    if (p > room) p = room;
    return du_sb_clamp((int)((int64_t)p * (s->total - s->page) / room), s->total, s->page);
}

/* ---- f2-33: tab control, history graph, group frame ---- */
static int tab_width(const struct du_widget *w, unsigned i)
{ return du_text_width(w->items[i].text[0] ? w->items[i].text[0] : "") + 16; }

int du_tabs_hit(const struct du_widget *w, int x, int y)
{
    if (w->kind != DU_TABS || y < w->box.y || y >= w->box.y + DU_TAB_H || x < w->box.x) return -1;
    int at = w->box.x + 2;
    for (unsigned i = 0; i < w->count; i++) {
        int width = tab_width(w, i);
        if (x >= at && x < at + width) return (int)i;
        at += width;
    }
    return -1;
}

static void draw_tabs(struct du_canvas *c, const struct du_widget *w, int focused)
{
    struct du_rect b = w->box;
    int base = b.y + DU_TAB_H - 1;
    struct du_rect page = {b.x, base, b.w, b.y + b.h - base};
    du_fill(c, page, DU_FACE);
    du_hline(c, page.x, page.y, page.w, DU_HILITE); du_vline(c, page.x, page.y, page.h, DU_HILITE);
    du_vline(c, page.x + page.w - 1, page.y, page.h, DU_DARK); du_vline(c, page.x + page.w - 2, page.y + 1, page.h - 2, DU_SHADOW);
    du_hline(c, page.x, page.y + page.h - 1, page.w, DU_DARK); du_hline(c, page.x + 1, page.y + page.h - 2, page.w - 2, DU_SHADOW);
    int at = b.x + 2;
    for (unsigned i = 0; i < w->count; i++) {
        int width = tab_width(w, i), sel = i == w->active;
        struct du_rect r = {at - (sel ? 2 : 0), b.y + (sel ? 0 : 2), width + (sel ? 4 : 0), DU_TAB_H - (sel ? 0 : 2)};
        du_fill(c, r, DU_FACE);
        du_hline(c, r.x + 2, r.y, r.w - 4, DU_HILITE); du_vline(c, r.x, r.y + 2, r.h - 2, DU_HILITE);
        du_fill(c, (struct du_rect){r.x + 1, r.y + 1, 1, 1}, DU_HILITE);
        du_vline(c, r.x + r.w - 1, r.y + 2, r.h - 2, DU_DARK); du_vline(c, r.x + r.w - 2, r.y + 1, r.h - 1, DU_SHADOW);
        if (sel) du_hline(c, r.x + 1, base, r.w - 3, DU_FACE);
        const char *t = w->items[i].text[0] ? w->items[i].text[0] : "";
        int tw = du_text_width(t), tx = r.x + (r.w - tw) / 2, ty = r.y + (DU_TAB_H - 2 - DU_GLYPH_H) / 2 + (sel ? 0 : -1);
        du_text(c, tx, ty, t, DU_INK);
        if (sel && focused) du_dotted(c, (struct du_rect){tx - 3, ty - 3, tw + 5, DU_GLYPH_H + 6});
        at += width;
    }
}

static int graph_y(const struct du_widget *w, unsigned v)
{ if (v > 100) v = 100; return w->box.y + w->box.h - 2 - (int)((int64_t)(w->box.h - 4) * v / 100); }

static void draw_graph(struct du_canvas *c, const struct du_widget *w)
{
    struct du_rect b = w->box;
    du_fill(c, b, 0x000000u);
    for (int y = b.y + b.h - 1; y >= b.y; y -= 12) du_hline(c, b.x, y, b.w, 0x00573fu);
    for (int x = b.x + b.w - 1; x >= b.x; x -= 12) du_vline(c, x, b.y, b.h, 0x00573fu);
    unsigned capacity = w->total > 1 ? (unsigned)w->total : 2, n = w->sample_count < capacity ? w->sample_count : capacity;
    if (!w->samples || !n) return;
    /* Newest sample at the right edge; integer Bresenham segments. */
    for (unsigned i = 0; i < n; i++) {
        unsigned slot = capacity - n + i;
        int x1 = b.x + (int)((int64_t)(b.w - 1) * slot / (capacity - 1)), y1 = graph_y(w, w->samples[w->sample_count - n + i]);
        if (!i) { du_fill(c, (struct du_rect){x1, y1, 1, 1}, 0x00ff00u); continue; }
        int x0 = b.x + (int)((int64_t)(b.w - 1) * (slot - 1) / (capacity - 1)), y0 = graph_y(w, w->samples[w->sample_count - n + i - 1]);
        int dx = x1 - x0, dy = y1 > y0 ? y1 - y0 : y0 - y1, sy = y0 < y1 ? 1 : -1, err = dx - dy;
        for (;;) {
            du_fill(c, (struct du_rect){x0, y0, 1, 1}, 0x00ff00u);
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; x0++; }
            if (e2 < dx) { err += dx; y0 += sy; }
        }
    }
}

static void draw_group(struct du_canvas *c, const struct du_widget *w)
{
    struct du_rect b = w->box;
    du_edge(c, (struct du_rect){b.x, b.y + DU_GLYPH_H / 2, b.w, b.h - DU_GLYPH_H / 2}, DU_ETCHED);
    if (w->text[0]) {
        int tw = du_text_width(w->text);
        du_fill(c, (struct du_rect){b.x + 6, b.y, tw + 6, DU_GLYPH_H + 1}, DU_FACE);
        du_label(c, b.x + 9, b.y, w->text, w->disabled ? DU_GRAY : DU_INK, 1);
    }
}

static void select_tab(struct du_ui *u, struct du_widget *w, int index)
{
    if (index < 0 || (unsigned)index >= w->count) return;
    w->active = (unsigned)index; u->dirty = 1;
    du_emit(u, (struct du_event){.type = DU_EV_SELECT, .id = w->id, .index = index});
}

static struct du_sb widget_sb(const struct du_widget *w)
{ return (struct du_sb){w->box, w->vertical, w->value, w->total, w->page}; }

void du_draw_widget(struct du_ui *u, struct du_widget *w, int focused)
{
    struct du_canvas *c = &u->canvas;
    switch (w->kind) {
    case DU_LABEL:
        if (w->disabled) du_text_disabled(c, w->box.x, w->box.y + (w->box.h - DU_GLYPH_H) / 2, w->text);
        else du_label(c, w->box.x, w->box.y + (w->box.h - DU_GLYPH_H) / 2, w->text, DU_INK, 1);
        break;
    case DU_HEADER:
        du_fill(c, w->box, DU_FACE); du_edge(c, w->box, DU_RAISED_THIN);
        du_text_fit(c, w->box.x + 5, w->box.y + (w->box.h - DU_GLYPH_H) / 2, w->box.w - 10, w->text, DU_INK);
        break;
    case DU_SEPARATOR:
        if (w->box.w <= 2) { du_vline(c, w->box.x, w->box.y, w->box.h, DU_SHADOW); du_vline(c, w->box.x + 1, w->box.y, w->box.h, DU_HILITE); }
        else { du_hline(c, w->box.x, w->box.y, w->box.w, DU_SHADOW); du_hline(c, w->box.x, w->box.y + 1, w->box.w, DU_HILITE); }
        break;
    case DU_BUTTON: du_draw_button(c, w, focused); break;
    case DU_TOOLBUTTON: draw_toolbutton(c, w); break;
    case DU_EDIT: du_draw_edit(c, w, focused && u->focused_window); break;
    case DU_LISTVIEW: du_draw_list(c, w, focused, u->focused_window); break;
    case DU_TREE: du_draw_tree(c, w, focused, u->focused_window); break;
    case DU_SCROLLBAR: { struct du_sb s = widget_sb(w); du_sb_draw(c, &s, w->pressed && w->hot ? w->drag : 0); break; }
    case DU_STATUS: draw_status(c, w); break;
    case DU_SPLITTER: du_fill(c, w->box, DU_FACE); break;
    case DU_PROGRESS: draw_progress(c, w); break;
    case DU_TABS: draw_tabs(c, w, focused && u->focused_window); break;
    case DU_GRAPH: draw_graph(c, w); break;
    case DU_GROUP: draw_group(c, w); break;
    case DU_TEXTAREA: du_ta_draw(c, w->box, (struct du_textarea *)w->data, focused && u->focused_window); break;
    }
}

void du_render(struct du_ui *u)
{
    struct du_canvas *c = &u->canvas;
    c->clip = (struct du_rect){0, 0, c->width, c->height};
    du_fill(c, c->clip, DU_FACE);
    for (unsigned i = 0; i < u->count; i++) {
        struct du_widget *w = &u->widgets[i];
        if (w->hidden) continue;
        c->clip = du_intersection((struct du_rect){0, 0, c->width, c->height}, w->box);
        du_draw_widget(u, w, u->focus == (int)i && !u->dialog.kind);
    }
    c->clip = (struct du_rect){0, 0, c->width, c->height};
    du_draw_menus(u);
    c->clip = (struct du_rect){0, 0, c->width, c->height};
    du_draw_dialog(u);
    c->clip = (struct du_rect){0, 0, c->width, c->height};
    u->dirty = 0;
}

/* ---- per-widget input ---- */
int du_widget_key(struct du_ui *u, struct du_widget *w, unsigned code)
{
    switch (w->kind) {
    case DU_EDIT: return du_edit_key(u, w, code);
    case DU_LISTVIEW: case DU_TREE: return du_list_key(u, w, code);
    case DU_TEXTAREA: {
        struct du_textarea *ta = w->data;
        if (!ta) return 0;
        du_ta_fit(ta, w->box);
        unsigned ch = du_key_char(u, code);
        if (!(ch ? du_ta_text(ta, ch) : du_ta_key(ta, code, u->modifiers))) return 0;
        du_post(u, DU_EV_CHANGE, w->id, 0);
        return 1;
    }
    case DU_BUTTON:
        if (code == DU_KEY_SPACE || code == DU_KEY_ENTER) { du_post(u, DU_EV_ACTIVATE, w->id, 0); return 1; }
        return 0;
    case DU_TABS:
        if (!w->count || (code != DU_KEY_LEFT && code != DU_KEY_RIGHT && code != DU_KEY_HOME && code != DU_KEY_END)) return 0;
        select_tab(u, w, code == DU_KEY_HOME ? 0 : code == DU_KEY_END ? (int)w->count - 1 :
                   (int)(w->active + (code == DU_KEY_LEFT ? w->count - 1 : 1)) % (int)w->count);
        return 1;
    default: return 0;
    }
}

int du_widget_press(struct du_ui *u, struct du_widget *w, int button, int x, int y)
{
    if (w->disabled) return 1;
    switch (w->kind) {
    case DU_BUTTON: case DU_TOOLBUTTON:
        if (button != 1) return 1;
        w->pressed = 1; w->hot = 1;
        w->drag = w->kind == DU_TOOLBUTTON && w->dropdown > 0 && x >= w->box.x + w->box.w - w->dropdown ? 2 : 1;
        if (w->drag == 2) { w->pressed = 0; du_post(u, DU_EV_CHANGE, w->id, 1); }
        return 1;
    case DU_EDIT:
        if (button == 1) {
            uint64_t now = u->now;
            if (u->last_click_widget == w->id && now - u->last_click_ns < DU_DOUBLE_CLICK_NS) { w->anchor = 0; w->cursor = strlen(w->text); }
            else { du_edit_press(w, x, (u->modifiers & DU_SHIFT) != 0); w->drag = 1; }
            u->last_click_widget = w->id; u->last_click_ns = now;
        } else if (button == 2) du_emit(u, (struct du_event){.type = DU_EV_CONTEXT, .id = w->id, .index = -1, .x = x, .y = y});
        return 1;
    case DU_TEXTAREA:
        if (button == 2) du_emit(u, (struct du_event){.type = DU_EV_CONTEXT, .id = w->id, .index = -1, .x = x, .y = y});
        else if (button == 1 && w->data && du_ta_mouse_down(w->data, w->box, x, y, u->modifiers, u->now)) du_post(u, DU_EV_CHANGE, w->id, 0);
        return 1;
    case DU_LISTVIEW: case DU_TREE: return du_list_press(u, w, button, x, y);
    case DU_SCROLLBAR: {
        if (button != 1) return 1;
        struct du_sb s = widget_sb(w); int part = du_sb_part(&s, x, y);
        w->pressed = 1; w->hot = 1; w->drag = part;
        if (part == 3) w->drag_offset = du_sb_grab(&s, x, y);
        else if (part) { int v = du_sb_apply(&s, part); if (v != w->value) { w->value = v; du_post(u, DU_EV_CHANGE, w->id, v); } }
        return 1;
    }
    case DU_SPLITTER:
        if (button == 1) { w->drag = 1; w->drag_offset = (w->vertical ? x - w->box.x : y - w->box.y); }
        return 1;
    case DU_TABS:
        if (button == 1) select_tab(u, w, du_tabs_hit(w, x, y));
        return 1;
    default: return 0;
    }
}

void du_widget_motion(struct du_ui *u, struct du_widget *w, int x, int y)
{
    switch (w->kind) {
    case DU_BUTTON: case DU_TOOLBUTTON: {
        int hot = du_contains(w->box, x, y);
        if (hot != w->hot) { w->hot = (uint8_t)hot; u->dirty = 1; }
        break;
    }
    case DU_EDIT: if (w->drag) { du_edit_press(w, x, 1); u->dirty = 1; } break;
    case DU_TEXTAREA: if (w->data && du_ta_mouse_move(w->data, w->box, x, y)) du_post(u, DU_EV_CHANGE, w->id, 0); break;
    case DU_LISTVIEW: case DU_TREE: du_list_motion(u, w, x, y); break;
    case DU_SCROLLBAR:
        if (w->drag == 3) {
            struct du_sb s = widget_sb(w); int v = du_sb_drag(&s, x, y, w->drag_offset);
            if (v != w->value) { w->value = v; du_post(u, DU_EV_CHANGE, w->id, v); }
        } else w->hot = (uint8_t)(du_sb_part(&(struct du_sb){w->box, w->vertical, w->value, w->total, w->page}, x, y) == w->drag);
        u->dirty = 1;
        break;
    case DU_SPLITTER:
        if (w->drag) du_post(u, DU_EV_CHANGE, w->id, w->vertical ? x - w->drag_offset : y - w->drag_offset);
        break;
    default: break;
    }
}

void du_widget_release(struct du_ui *u, struct du_widget *w, int button, int x, int y)
{
    if (button != 1) return;
    int fire = (w->kind == DU_BUTTON || w->kind == DU_TOOLBUTTON) && w->pressed && w->drag == 1 && du_contains(w->box, x, y) && !w->disabled;
    if (w->kind == DU_LISTVIEW || w->kind == DU_TREE) du_list_release(u, w);
    if (w->kind == DU_TEXTAREA && w->data) du_ta_mouse_up(w->data);
    w->pressed = 0; w->drag = 0; w->hot = (uint8_t)du_contains(w->box, x, y);
    u->dirty = 1;
    if (fire) du_post(u, DU_EV_ACTIVATE, w->id, 0);
}

/* ---- dispatch ---- */
static void focus_next(struct du_ui *u, int back)
{
    if (!u->count) return;
    int i = u->focus;
    for (unsigned n = 0; n < u->count; n++) {
        i = back ? (i <= 0 ? (int)u->count - 1 : i - 1) : (i + 1) % (int)u->count;
        if (du_focusable(&u->widgets[i])) { u->focus = i; u->dirty = 1; return; }
    }
}

static void key_down(struct du_ui *u, unsigned code)
{
    if (u->dialog.kind) { du_dialog_key(u, code); return; }
    if (u->menu >= 0 || u->popup) { du_menu_key(u, code); return; }
    if (code == DU_KEY_F10 && !(u->modifiers & ~DU_SHIFT)) {
        if (u->modifiers & DU_SHIFT) { /* Shift+F10: context menu of the focus */ }
        else { du_menu_open(u, 0, 1); return; }
    }
    if ((u->modifiers & DU_ALT) && !(u->modifiers & DU_CTRL)) {
        unsigned saved = u->modifiers; u->modifiers = 0;
        unsigned ch = du_key_char(u, code); u->modifiers = saved;
        if (ch && du_menu_mnemonic(u, ch)) return;
        /* f2-33: Alt plus a button's underlined (&) letter presses it. */
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        for (unsigned i = 0; ch && i < u->count; i++) {
            const struct du_widget *b = &u->widgets[i];
            const char *amp = b->kind == DU_BUTTON && !b->hidden && !b->disabled ? strchr(b->text, '&') : NULL;
            unsigned m = amp && amp[1] && amp[1] != '&' ? (unsigned char)amp[1] : 0;
            if (m >= 'A' && m <= 'Z') m += 32;
            if (m == ch) { du_post(u, DU_EV_ACTIVATE, b->id, 0); return; }
        }
    }
    struct du_widget *f = u->focus >= 0 && (unsigned)u->focus < u->count ? &u->widgets[u->focus] : NULL;
    if (f && f->hidden) f = NULL;
    if (f && f->kind == DU_EDIT && du_edit_key(u, f, code)) return;
    if (du_menu_accelerator(u, code)) return;
    /* f2-33: Ctrl+Tab, Ctrl+Shift+Tab, Ctrl+PgDn/PgUp cycle the first visible tab control. */
    if ((u->modifiers & DU_CTRL) && !(u->modifiers & DU_ALT) && (code == DU_KEY_TAB || code == DU_KEY_PGUP || code == DU_KEY_PGDN))
        for (unsigned i = 0; i < u->count; i++) {
            struct du_widget *t = &u->widgets[i];
            if (t->kind != DU_TABS || t->hidden || t->disabled || !t->count) continue;
            int back = code == DU_KEY_PGUP || (code == DU_KEY_TAB && (u->modifiers & DU_SHIFT));
            select_tab(u, t, (int)(t->active + (back ? t->count - 1 : 1)) % (int)t->count);
            return;
        }
    if (code == DU_KEY_TAB && !(u->modifiers & (DU_CTRL | DU_ALT)) && !(f && f->kind == DU_TEXTAREA)) { focus_next(u, (u->modifiers & DU_SHIFT) != 0); return; }
    if (f && (code == DU_KEY_MENU || (code == DU_KEY_F10 && u->modifiers == DU_SHIFT)) && (f->kind == DU_LISTVIEW || f->kind == DU_TREE)) {
        struct du_rect r = f->count && f->active < f->count ? du_list_label(f, f->active) : du_list_body(f);
        du_emit(u, (struct du_event){.type = DU_EV_CONTEXT, .id = f->id, .index = f->count ? (int)f->active : -1,
                                     .x = r.x + 8, .y = r.y + r.h, .keyboard = 1});
        return;
    }
    if (f && du_widget_key(u, f, code)) return;
    du_emit(u, (struct du_event){.type = DU_EV_KEY, .id = f ? f->id : 0, .key = code, .ch = du_key_char(u, code)});
}

void du_input(struct du_ui *u, const struct ciuki_input_event *e)
{
    if (e->monotonic_ns > u->now) u->now = e->monotonic_ns;
    if (e->type == CIUKI_INPUT_RESYNC) { du_release(u); return; }
    if (e->type == CIUKI_INPUT_KEY) {
        if (e->code < 0 || e->code > 0x200) return;
        unsigned code = (unsigned)e->code;
        u->keys[code] = (uint8_t)(e->value != 0);
        u->modifiers = (u->keys[DU_KEY_LSHIFT] || u->keys[DU_KEY_RSHIFT] ? DU_SHIFT : 0) |
                       (u->keys[DU_KEY_LCTRL] || u->keys[DU_KEY_RCTRL] ? DU_CTRL : 0) |
                       (u->keys[DU_KEY_LALT] || u->keys[DU_KEY_RALT] ? DU_ALT : 0);
        if (code == DU_KEY_LALT || code == DU_KEY_RALT) {
            if (e->value == 1) u->menu_armed = 1;
            else if (!e->value && u->menu_armed && !u->dialog.kind) {
                u->menu_armed = 0;
                if (u->menu >= 0 || u->popup) du_menu_close(u); else if (u->menu_count) du_menu_open(u, 0, 1);
            }
            return;
        }
        if (!e->value) return;
        u->menu_armed = 0;
        if (code == DU_KEY_CAPS && e->value == 1) { u->caps = !u->caps; return; }
        if (code == DU_KEY_LSHIFT || code == DU_KEY_RSHIFT || code == DU_KEY_LCTRL || code == DU_KEY_RCTRL) return;
        key_down(u, code);
        return;
    }
    if (e->type == CIUKI_INPUT_TEXT) return; /* text derives from KEY events plus Shift/Caps */
    if (e->type == CIUKI_INPUT_MOTION) {
        u->mouse_x = e->value; u->mouse_y = e->value2;
        if (u->dialog.kind) { du_dialog_motion(u); return; }
        if (u->menu >= 0 || u->popup) { du_menu_motion(u); return; }
        if (u->capture >= 0 && (unsigned)u->capture < u->count) { du_widget_motion(u, &u->widgets[u->capture], u->mouse_x, u->mouse_y); return; }
        int hit = du_hit(u, u->mouse_x, u->mouse_y);
        if (hit != u->hover) {
            if (u->hover >= 0 && (unsigned)u->hover < u->count) { u->widgets[u->hover].hot = 0; u->dirty = 1; }
            u->hover = hit;
        }
        if (hit >= 0 && u->widgets[hit].kind == DU_TOOLBUTTON && !u->widgets[hit].hot && !u->widgets[hit].disabled) { u->widgets[hit].hot = 1; u->dirty = 1; }
        return;
    }
    if (e->type != CIUKI_INPUT_BUTTON || e->code < 1 || e->code > 3) return;
    int button = e->code;
    unsigned bit = 1u << (button - 1);
    if (e->value) u->buttons |= bit; else u->buttons &= ~bit;
    if (u->dialog.kind) { if (e->value) du_dialog_press(u, button); else du_dialog_release(u, button); return; }
    if (e->value) {
        if (du_menu_press(u, button)) return;
        int hit = du_hit(u, u->mouse_x, u->mouse_y);
        if (hit < 0) {
            if (button == 2) du_emit(u, (struct du_event){.type = DU_EV_CONTEXT, .id = 0, .index = -1});
            return;
        }
        struct du_widget *w = &u->widgets[hit];
        if (du_focusable(w)) { if (u->focus != hit) u->dirty = 1; u->focus = hit; }
        if (button == 1) u->capture = hit;
        du_widget_press(u, w, button, u->mouse_x, u->mouse_y);
    } else {
        if (du_menu_release(u, button)) return;
        if (u->capture >= 0 && (unsigned)u->capture < u->count) {
            int captured = u->capture;
            if (button == 1) u->capture = -1;
            du_widget_release(u, &u->widgets[captured], button, u->mouse_x, u->mouse_y);
        }
    }
}
