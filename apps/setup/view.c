/* SPDX-License-Identifier: MIT
 * A sunken, keyboard-scrollable list of wrapped paragraphs and selectable
 * one-line entries (licence documents, the installation summary, the rescue
 * inventory). Painted by Setup inside a focusable DU_CANVAS widget. */
#include "setup.h"
#include "../libdeskui/internal.h"
#include "../desktop/theme.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define PAD 6
#define BAR 16

const char *st_pool(struct st_app *a, const char *fmt, ...)
{
    if (a->pool_used >= sizeof(a->pool) - 1) return "";
    char *out = a->pool + a->pool_used;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(out, sizeof(a->pool) - a->pool_used, fmt, ap);
    va_end(ap);
    if (n < 0) { *out = 0; return out; }
    size_t used = (size_t)n + 1;
    if (used > sizeof(a->pool) - a->pool_used) used = sizeof(a->pool) - a->pool_used;
    a->pool_used += used;
    return out;
}

void st_view_reset(struct st_view *v, int id, struct du_rect box)
{
    memset(v, 0, sizeof(*v));
    v->id = id; v->box = box; v->selected = -1;
}

void st_view_add(struct st_view *v, const char *text, int entry, int font, uint32_t ink, int mark)
{
    if (v->count >= sizeof(v->lines) / sizeof(v->lines[0])) return;
    v->lines[v->count++] = (struct st_line){.text = text, .entry = (uint8_t)entry, .font = (uint8_t)font,
                                            .mark = (uint8_t)mark, .ink = ink};
}

static int line_height(int font) { return desk_font_line(font) + 1; }
static int text_width(const struct st_view *v, const struct st_line *l)
{ return v->box.w - 2 * PAD - 4 - BAR - (l->mark ? 18 : 0); }

void st_view_layout(struct st_view *v)
{
    int y = PAD;
    for (unsigned i = 0; i < v->count; i++) {
        struct st_line *l = &v->lines[i];
        int lh = line_height(l->font);
        l->y = y;
        l->h = l->entry ? lh + 2 : st_text_wrap(NULL, l->font, (struct du_rect){0, 0, text_width(v, l), 0}, lh, l->text, 0);
        if (!l->h) l->h = lh;
        y += l->h + (l->entry ? 0 : 3);
    }
    v->content = y + PAD;
    if (v->selected < 0) for (unsigned i = 0; i < v->count; i++) if (v->lines[i].entry) { v->selected = (int)i; break; }
}

static int visible_height(const struct st_view *v) { return v->box.h - 4; }
static int max_scroll(const struct st_view *v) { int m = v->content - visible_height(v); return m > 0 ? m : 0; }
static void clamp(struct st_view *v) { if (v->scroll > max_scroll(v)) v->scroll = max_scroll(v); if (v->scroll < 0) v->scroll = 0; }
static void reveal(struct st_view *v, int index)
{
    if (index < 0) return;
    const struct st_line *l = &v->lines[index];
    if (l->y - PAD < v->scroll) v->scroll = l->y - PAD;
    if (l->y + l->h + PAD > v->scroll + visible_height(v)) v->scroll = l->y + l->h + PAD - visible_height(v);
    clamp(v);
}

void st_view_paint(struct du_canvas *c, const struct st_view *v, int focused)
{
    struct du_rect b = v->box;
    du_well(c, b, DU_WINDOW);
    struct du_rect inner = {b.x + 2, b.y + 2, b.w - 4, b.h - 4}, old = c->clip;
    int bar = v->content > visible_height(v);
    struct du_rect area = inner;
    if (bar) area.w -= BAR;
    c->clip = du_intersection(old, area);
    for (unsigned i = 0; i < v->count; i++) {
        const struct st_line *l = &v->lines[i];
        int y = inner.y + l->y - v->scroll;
        if (y + l->h < inner.y || y > inner.y + inner.h) continue;
        int x = inner.x + PAD, lh = line_height(l->font);
        uint32_t ink = l->ink;
        if (l->entry && (int)i == v->selected) {
            struct du_rect row = {inner.x + 2, y, area.w - 4, l->h};
            du_fill(c, row, focused ? DU_SELECT : desk_mix(DU_SELECT, DU_WINDOW, 60));
            ink = focused ? DU_SELECT_INK : DU_INK;
            if (focused) du_focus_ring(c, row);
        }
        if (l->mark) { st_mark_paint(c, x, y + (lh - 12) / 2, 12, (enum st_mark)(l->mark - 1)); x += 18; }
        if (l->entry) {
            struct du_rect clip = c->clip;
            c->clip = du_intersection(clip, (struct du_rect){x, y, area.x + area.w - x - 4, l->h});
            st_text_line(c, l->font, x + 2, y + 1, lh, l->text, ink);
            c->clip = clip;
        } else st_text_wrap(c, l->font, (struct du_rect){x, y, text_width(v, l), l->h}, lh, l->text, ink);
    }
    c->clip = old;
    if (bar) {
        struct du_sb sb = {{inner.x + inner.w - BAR, inner.y, BAR, inner.h}, 1, v->scroll, v->content, visible_height(v)};
        du_sb_draw(c, &sb, 0);
    }
    if (focused && v->selected < 0) du_focus_ring(c, (struct du_rect){b.x + 2, b.y + 2, b.w - 4 - (bar ? BAR : 0), b.h - 4});
}

static int step_entry(const struct st_view *v, int from, int direction)
{
    for (int i = from + direction; i >= 0 && i < (int)v->count; i += direction) if (v->lines[i].entry) return i;
    return -1;
}

int st_view_key(struct st_view *v, unsigned code)
{
    int page = visible_height(v) - 20, next;
    switch (code) {
    case DU_KEY_UP: case DU_KEY_DOWN: {
        int direction = code == DU_KEY_UP ? -1 : 1;
        next = v->selected >= 0 ? step_entry(v, v->selected, direction) : -1;
        /* Entries scroll the list to themselves; paragraphs scroll by a line. */
        if (next >= 0 && (direction > 0 || v->lines[next].y >= v->scroll - 40)) { v->selected = next; reveal(v, next); }
        else { v->scroll += direction * 15; clamp(v); }
        return 1;
    }
    case DU_KEY_PGUP: v->scroll -= page; clamp(v); return 1;
    case DU_KEY_PGDN: v->scroll += page; clamp(v); return 1;
    case DU_KEY_HOME: v->scroll = 0; next = step_entry(v, -1, 1); if (next >= 0) v->selected = next; clamp(v); return 1;
    case DU_KEY_END: v->scroll = max_scroll(v); next = step_entry(v, (int)v->count, -1); if (next >= 0) v->selected = next; return 1;
    }
    return 0;
}

int st_view_click(struct st_view *v, int x, int y)
{
    if (!du_contains(v->box, x, y)) return -1;
    int at = y - v->box.y - 2 + v->scroll;
    for (unsigned i = 0; i < v->count; i++)
        if (v->lines[i].entry && at >= v->lines[i].y && at < v->lines[i].y + v->lines[i].h) { v->selected = (int)i; return (int)i; }
    return -1;
}
