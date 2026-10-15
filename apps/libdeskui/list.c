/* SPDX-License-Identifier: MIT
 * List view (Details, Large Icons, List) and tree view. */
#include "internal.h"
#include <string.h>
#include <stdlib.h>

#define HEADER_H 18
#define CELL_W 76
#define CELL_H 68
#define SB 16

struct layout {
    struct du_rect body, area, header;
    struct du_sb v, h;
    int has_v, has_h, rows, cols, visible, colw;
};

static int name_width(const struct du_widget *w)
{
    int widest = 0;
    for (unsigned i = 0; i < w->count; i++) {
        int t = w->items[i].text[0] ? du_text_width(w->items[i].text[0]) : 0;
        if (t > widest) widest = t;
    }
    widest += 22 + 12;
    return widest < 90 ? 90 : widest > 240 ? 240 : widest;
}

static struct layout compute(const struct du_widget *w)
{
    struct layout l; memset(&l, 0, sizeof(l));
    l.body = (struct du_rect){w->box.x + 2, w->box.y + 2, w->box.w - 4, w->box.h - 4};
    l.area = l.body;
    int count = (int)w->count;
    if (w->kind == DU_LISTVIEW && w->view == DU_DETAILS) {
        l.header = (struct du_rect){l.body.x, l.body.y, l.body.w, HEADER_H};
        l.area.y += HEADER_H; l.area.h -= HEADER_H;
    }
    if (w->kind == DU_TREE || w->view == DU_DETAILS) {
        l.visible = l.area.h / DU_ROW; if (l.visible < 1) l.visible = 1;
        l.has_v = count > l.visible;
        if (l.has_v) { l.area.w -= SB; if (w->view == DU_DETAILS && w->kind == DU_LISTVIEW) l.header.w -= SB; }
        l.rows = count; l.cols = 1;
        l.v = (struct du_sb){{l.area.x + l.area.w, l.body.y, SB, l.body.h}, 1, w->scroll, count, l.visible};
    } else if (w->view == DU_ICONS) {
        l.cols = l.area.w / CELL_W; if (l.cols < 1) l.cols = 1;
        int rows = (count + l.cols - 1) / l.cols;
        l.visible = l.area.h / CELL_H; if (l.visible < 1) l.visible = 1;
        if (rows > l.visible) {
            l.has_v = 1; l.area.w -= SB;
            l.cols = l.area.w / CELL_W; if (l.cols < 1) l.cols = 1;
            rows = (count + l.cols - 1) / l.cols;
        }
        l.rows = rows;
        l.v = (struct du_sb){{l.area.x + l.area.w, l.body.y, SB, l.body.h}, 1, w->scroll, rows, l.visible};
    } else {
        l.colw = name_width(w);
        l.rows = l.area.h / DU_ROW; if (l.rows < 1) l.rows = 1;
        int cols = (count + l.rows - 1) / l.rows;
        if (cols * l.colw > l.area.w) {
            l.has_h = 1; l.area.h -= SB;
            l.rows = l.area.h / DU_ROW; if (l.rows < 1) l.rows = 1;
            cols = (count + l.rows - 1) / l.rows;
        }
        l.cols = cols;
        l.visible = l.area.w / l.colw; if (l.visible < 1) l.visible = 1;
        l.h = (struct du_sb){{l.body.x, l.area.y + l.area.h, l.body.w, SB}, 0, w->hscroll, cols, l.visible};
    }
    return l;
}

struct du_rect du_list_body(const struct du_widget *w) { return compute(w).area; }

int du_list_wheel(struct du_widget *w, int detents)
{
    struct layout l=compute(w);
    int *offset=l.has_h ? &w->hscroll : &w->scroll;
    int next=du_wheel_value(*offset,detents,l.has_h ? l.cols : l.rows,l.visible);
    int changed=next!=*offset;
    *offset=next;
    return changed;
}

int du_list_page(const struct du_widget *w)
{
    struct layout l = compute(w);
    if (w->kind == DU_TREE || w->view == DU_DETAILS) return l.visible;
    if (w->view == DU_ICONS) return l.visible * l.cols;
    return l.rows;
}

static struct du_rect cell(const struct du_widget *w, const struct layout *l, unsigned i)
{
    if (w->kind == DU_TREE || w->view == DU_DETAILS)
        return (struct du_rect){l->area.x, l->area.y + ((int)i - w->scroll) * DU_ROW, l->area.w, DU_ROW};
    if (w->view == DU_ICONS)
        return (struct du_rect){l->area.x + (int)i % l->cols * CELL_W, l->area.y + ((int)i / l->cols - w->scroll) * CELL_H, CELL_W, CELL_H};
    return (struct du_rect){l->area.x + ((int)i / l->rows - w->hscroll) * l->colw, l->area.y + (int)i % l->rows * DU_ROW, l->colw, DU_ROW};
}

static int tree_x(const struct du_widget *w, const struct layout *l, unsigned depth)
{ (void)w; return l->area.x + 9 + (int)depth * 16; }

/* Two centred lines that fit CELL_W - 6 pixels for Large Icons labels. */
static unsigned wrap(const char *s, char lines[2][64])
{
    int font = desk_theme->font_ui, room = CELL_W - 6;
    lines[0][0] = lines[1][0] = 0;
    if (desk_text_measure(font, s) <= room) { strncpy(lines[0], s, 63); lines[0][63] = 0; return 1; }
    size_t fit = desk_text_fit_chars(font, s, room);
    size_t at = 0, n = 0, brk = 0, brk_chars = 0;
    while (s[at] && n < fit) {
        if (s[at] == ' ' || s[at] == '.' || s[at] == '-' || s[at] == '_') { brk = at + 1; brk_chars = n + 1; }
        at = du_utf8_next(s, at); n++;
    }
    if (!brk || brk_chars < fit / 2) brk = at;
    size_t first = brk < 63 ? brk : 63;
    memcpy(lines[0], s, first); lines[0][first] = 0;
    const char *rest = s + brk;
    if (desk_text_measure(font, rest) <= room) { strncpy(lines[1], rest, 63); lines[1][63] = 0; }
    else {
        static const char dots[] = "\xe2\x80\xa6";
        size_t k = 0, m = 0, keep = desk_text_fit_chars(font, rest, room - desk_text_measure(font, dots));
        while (rest[k] && m < keep && k < 56) { k = du_utf8_next(rest, k); m++; }
        memcpy(lines[1], rest, k); strcpy(lines[1] + k, dots);
    }
    return 2;
}

static struct du_rect label_rect(const struct du_widget *w, const struct layout *l, unsigned i)
{
    struct du_rect r = cell(w, l, i);
    const char *name = w->items[i].text[0] ? w->items[i].text[0] : "";
    int tw = du_text_width(name);
    if (w->kind == DU_TREE) {
        int x = tree_x(w, l, w->items[i].depth) + 8 + 16 + 3;
        return (struct du_rect){x - 2, r.y + 1, tw + 4, DU_ROW - 2};
    }
    if (w->view == DU_ICONS) {
        char lines[2][64]; unsigned n = wrap(name, lines);
        int lw = du_text_width(lines[0]); if (n > 1 && du_text_width(lines[1]) > lw) lw = du_text_width(lines[1]);
        return (struct du_rect){r.x + (CELL_W - lw) / 2 - 2, r.y + 38, lw + 4, (int)n * 13 + 3};
    }
    int limit = (w->view == DU_DETAILS ? (w->column_count ? w->columns[0].width : r.w) : l->colw) - 26;
    if (tw > limit) tw = limit;
    return (struct du_rect){r.x + 20, r.y + 1, tw + 4, DU_ROW - 2};
}

struct du_rect du_list_label(const struct du_widget *w, unsigned index)
{
    struct layout l = compute(w);
    if (index >= w->count) return (struct du_rect){l.area.x, l.area.y, 0, 0};
    return label_rect(w, &l, index);
}

int du_list_hit(const struct du_widget *w, int x, int y)
{
    struct layout l = compute(w);
    if (!du_contains(l.area, x, y)) return -1;
    for (unsigned i = 0; i < w->count; i++) {
        struct du_rect r = cell(w, &l, i);
        if (r.y > l.area.y + l.area.h || r.x > l.area.x + l.area.w) continue;
        if (!du_contains(r, x, y)) continue;
        if (w->kind == DU_TREE) return x >= tree_x(w, &l, w->items[i].depth) + 6 ? (int)i : -1;
        if (w->view == DU_ICONS) {
            struct du_rect icon = {r.x + (CELL_W - 32) / 2, r.y + 4, 32, 32};
            return du_contains(icon, x, y) || du_contains(label_rect(w, &l, i), x, y) ? (int)i : -1;
        }
        if (w->view == DU_LIST) { struct du_rect lab = label_rect(w, &l, i); return x < lab.x + lab.w + 4 ? (int)i : -1; }
        return (int)i;
    }
    return -1;
}

int du_tree_toggle_hit(const struct du_widget *w, int x, int y)
{
    if (w->kind != DU_TREE) return -1;
    struct layout l = compute(w);
    int i = -1;
    for (unsigned n = 0; n < w->count; n++) if (du_contains(cell(w, &l, n), x, y) && du_contains(l.area, x, y)) i = (int)n;
    if (i < 0 || !w->items[i].children) return -1;
    struct du_rect r = cell(w, &l, (unsigned)i);
    int cx = tree_x(w, &l, w->items[i].depth), cy = r.y + DU_ROW / 2;
    return x >= cx - 6 && x <= cx + 6 && y >= cy - 6 && y <= cy + 6 ? i : -1;
}

int du_header_hit(const struct du_widget *w, int x, int y)
{
    if (w->kind != DU_LISTVIEW || w->view != DU_DETAILS) return -1;
    struct layout l = compute(w);
    if (!du_contains(l.header, x, y)) return -1;
    int at = l.header.x;
    for (unsigned c = 0; c < w->column_count; c++) {
        if (x >= at && x < at + w->columns[c].width) return (int)c;
        at += w->columns[c].width;
    }
    return -1;
}

void du_list_ensure(struct du_widget *w, unsigned index)
{
    if (index >= w->count) return;
    struct layout l = compute(w);
    if (w->kind == DU_TREE || w->view == DU_DETAILS) {
        if ((int)index < w->scroll) w->scroll = (int)index;
        if ((int)index >= w->scroll + l.visible) w->scroll = (int)index - l.visible + 1;
        w->scroll = du_sb_clamp(w->scroll, (int)w->count, l.visible);
    } else if (w->view == DU_ICONS) {
        int row = (int)index / l.cols;
        if (row < w->scroll) w->scroll = row;
        if (row >= w->scroll + l.visible) w->scroll = row - l.visible + 1;
        w->scroll = du_sb_clamp(w->scroll, l.rows, l.visible);
    } else {
        int col = (int)index / l.rows;
        if (col < w->hscroll) w->hscroll = col;
        if (col >= w->hscroll + l.visible) w->hscroll = col - l.visible + 1;
        w->hscroll = du_sb_clamp(w->hscroll, l.cols, l.visible);
    }
}

int du_list_target(const struct du_widget *w, unsigned code)
{
    if (!w->count) return -1;
    struct layout l = compute(w);
    int n = (int)w->active, last = (int)w->count - 1, page = du_list_page(w);
    int step = 0;
    int icons = w->kind == DU_LISTVIEW && w->view == DU_ICONS, list = w->kind == DU_LISTVIEW && w->view == DU_LIST;
    switch (code) {
    case DU_KEY_UP: step = icons ? -l.cols : -1; break;
    case DU_KEY_DOWN: step = icons ? l.cols : 1; break;
    case DU_KEY_LEFT: if (!icons && !list) return -1; step = icons ? -1 : -l.rows; break;
    case DU_KEY_RIGHT: if (!icons && !list) return -1; step = icons ? 1 : l.rows; break;
    case DU_KEY_PGUP: step = -(page > 1 ? page - 1 : 1); break;
    case DU_KEY_PGDN: step = page > 1 ? page - 1 : 1; break;
    case DU_KEY_HOME: return 0;
    case DU_KEY_END: return last;
    default: return -1;
    }
    n += step;
    if (n < 0) n = code == DU_KEY_PGUP || !(icons || list) ? 0 : (int)w->active;
    if (n > last) n = code == DU_KEY_PGDN || !(icons || list) || (list && code == DU_KEY_RIGHT) ? last : (int)w->active;
    return n;
}

/* ---- drawing ---- */
static void dotted_v(struct du_canvas *c, int x, int y0, int y1)
{ for (int y = y0; y < y1; y++) if (!(y & 1)) du_fill(c, (struct du_rect){x, y, 1, 1}, DU_SHADOW); }
static void dotted_h(struct du_canvas *c, int x0, int x1, int y)
{ for (int x = x0; x < x1; x++) if (!(x & 1)) du_fill(c, (struct du_rect){x, y, 1, 1}, DU_SHADOW); }

static void draw_label(struct du_canvas *c, struct du_rect lab, const char *text, int width, int selected, int active,
                       int focused, int window, int inactive)
{
    uint32_t ink = DU_INK;
    if (selected && focused && window) { du_fill(c, lab, DU_SELECT); ink = DU_SELECT_INK; }
    else if (selected && inactive) du_fill(c, lab, DU_FACE);
    du_text_fit(c, lab.x + 2, lab.y + (lab.h - DU_GLYPH_H) / 2, width, text, ink);
    if (active && focused && window) du_dotted(c, lab);
}

static void draw_header(struct du_canvas *c, const struct du_widget *w, const struct layout *l)
{
    int x = l->header.x, platinum = desk_theme->control_style == DESK_CONTROLS_PLATINUM;
    struct du_rect old = c->clip;
    c->clip = du_intersection(old, l->header);
    for (unsigned i = 0; i <= w->column_count; i++) {
        int width = i < w->column_count ? w->columns[i].width : l->header.x + l->header.w - x;
        if (width <= 0) break;
        struct du_rect b = {x, l->header.y, width, HEADER_H};
        if (platinum) {
            struct desk_paint p = {c->pixels, c->width, c->height, c->clip.x, c->clip.y, c->clip.w, c->clip.h};
            int sorted = w->sort == (int)i;
            desk_paint_vgradient(&p, b.x, b.y, b.w, b.h, sorted ? desk_mix(DU_ACCENT, desk_theme->face_top, 40) : desk_theme->face_top,
                                 sorted ? desk_mix(DU_ACCENT, desk_theme->face_bottom, 60) : desk_theme->face_bottom);
            du_hline(c, b.x, b.y + b.h - 1, b.w, DU_SHADOW);
            du_vline(c, b.x + b.w - 1, b.y + 3, b.h - 6, DU_SHADOW); du_vline(c, b.x, b.y + 3, b.h - 6, DU_HILITE);
        } else { du_fill(c, b, DU_FACE); du_edge(c, b, DU_BUTTON_EDGE); }
        if (i < w->column_count) {
            struct du_rect keep = c->clip; c->clip = du_intersection(keep, (struct du_rect){b.x + 2, b.y, b.w - 4, b.h});
            const char *t = w->columns[i].title ? w->columns[i].title : "";
            int tw = du_text_width(t), tx = w->columns[i].right ? b.x + b.w - 8 - tw : b.x + 6;
            du_text(c, tx, b.y + (HEADER_H - DU_GLYPH_H) / 2, t, DU_INK);
            if (w->sort == (int)i) {
                int ax = w->columns[i].right ? tx - 10 : tx + tw + 8;
                if (ax + 4 < b.x + b.w - 3) du_triangle(c, ax, b.y + 9, w->descending ? 1 : 0, 4, DU_SHADOW);
            }
            c->clip = keep;
        }
        x += width;
    }
    c->clip = old;
}

void du_draw_list(struct du_canvas *c, struct du_widget *w, int focused, int window)
{
    struct layout l = compute(w);
    du_well(c, w->box, DU_WINDOW);
    if (w->view == DU_DETAILS) draw_header(c, w, &l);
    struct du_rect old = c->clip;
    c->clip = du_intersection(old, l.area);
    if (!w->count && w->text[0]) du_text(c, l.area.x + (l.area.w - du_text_width(w->text)) / 2, l.area.y + 12, w->text, DU_GRAY);
    for (unsigned i = 0; i < w->count; i++) {
        struct du_rect r = cell(w, &l, i);
        if (r.y + r.h <= l.area.y || r.y >= l.area.y + l.area.h || r.x + r.w <= l.area.x || r.x >= l.area.x + l.area.w) continue;
        const struct du_item *it = &w->items[i];
        int act = i == w->active;
        int sel = it->selected || (!w->selection_external && w->selection_active && act);
        enum du_icon_mode mode = it->ghost ? DU_ICON_GHOST : sel && focused && window ? DU_ICON_SELECTED : DU_ICON_NORMAL;
        const char *name = it->text[0] ? it->text[0] : "";
        if (w->view == DU_ICONS) {
            du_icon_blit(c, r.x + (CELL_W - 32) / 2, r.y + 4, it->icon32 ? it->icon32 : it->icon16, it->icon32 ? 32 : 16, mode);
            char lines[2][64]; unsigned n = wrap(name, lines);
            struct du_rect lab = label_rect(w, &l, i);
            uint32_t ink = DU_INK;
            if (sel && focused && window) { du_fill(c, lab, DU_SELECT); ink = DU_SELECT_INK; }
            else if (sel && w->inactive_selection) du_fill(c, lab, DU_FACE);
            for (unsigned k = 0; k < n; k++)
                du_text(c, r.x + (CELL_W - du_text_width(lines[k])) / 2, lab.y + 2 + (int)k * 13, lines[k], ink);
            if (act && focused && window) du_dotted(c, lab);
            continue;
        }
        du_icon_blit(c, r.x + 2, r.y + (DU_ROW - 16) / 2, it->icon16, 16, mode);
        struct du_rect lab = label_rect(w, &l, i);
        int limit = (w->view == DU_DETAILS && w->column_count ? w->columns[0].width : l.colw) - 26;
        draw_label(c, lab, name, limit, sel, act, focused, window, w->inactive_selection);
        if (w->view == DU_DETAILS) {
            int x = r.x;
            for (unsigned col = 0; col < w->column_count; col++) {
                int width = w->columns[col].width;
                if (col && it->text[col]) {
                    struct du_rect keep = c->clip;
                    c->clip = du_intersection(keep, (struct du_rect){x + 2, r.y, width - 4, r.h});
                    int tw = du_text_width(it->text[col]), room = width - 10;
                    if (tw > room) tw = room;
                    du_text_fit(c, w->columns[col].right ? x + width - 6 - tw : x + 6, r.y + (DU_ROW - DU_GLYPH_H) / 2,
                                room, it->text[col], DU_INK);
                    c->clip = keep;
                }
                x += width;
            }
        }
    }
    c->clip = old;
    if (l.has_v) { l.v.value = w->scroll; du_sb_draw(c, &l.v, w->drag >= 10 && w->drag < 20 ? w->drag - 10 : 0); }
    if (l.has_h) { l.h.value = w->hscroll; du_sb_draw(c, &l.h, w->drag >= 20 && w->drag < 30 ? w->drag - 20 : 0); }
    if (focused && window && desk_theme->focus_style == DESK_FOCUS_RING) du_focus_ring(c, w->box);
}

static int sibling_below(const struct du_widget *w, unsigned i, unsigned level)
{
    /* Does the ancestor line at `level` continue below row i? */
    for (unsigned j = i + 1; j < w->count; j++) {
        if (w->items[j].depth < level + 1) return 0;
        if (w->items[j].depth == level + 1) return 1;
    }
    return 0;
}

void du_draw_tree(struct du_canvas *c, struct du_widget *w, int focused, int window)
{
    struct layout l = compute(w);
    du_well(c, w->box, DU_WINDOW);
    struct du_rect old = c->clip;
    c->clip = du_intersection(old, l.area);
    for (unsigned i = 0; i < w->count; i++) {
        struct du_rect r = cell(w, &l, i);
        if (r.y + r.h <= l.area.y || r.y >= l.area.y + l.area.h) continue;
        const struct du_item *it = &w->items[i];
        int d = it->depth, cy = r.y + DU_ROW / 2, cx = tree_x(w, &l, (unsigned)d);
        if (desk_theme->control_style != DESK_CONTROLS_PLATINUM) {   /* Platinum trees have no connector lines */
            for (int k = 0; k + 1 < d; k++) if (sibling_below(w, i, (unsigned)k)) dotted_v(c, tree_x(w, &l, (unsigned)k), r.y, r.y + r.h);
            if (d > 0) {
                int px = tree_x(w, &l, (unsigned)d - 1);
                dotted_v(c, px, r.y, sibling_below(w, i, (unsigned)d - 1) ? r.y + r.h : cy + 1);
                dotted_h(c, px, cx + 6, cy);
            }
            if (it->expanded && it->children) dotted_v(c, cx + 16, cy + 8, r.y + r.h);
        }
        if (it->children && desk_theme->control_style == DESK_CONTROLS_PLATINUM) {
            /* Platinum disclosure triangle: right when closed, down when open. */
            du_fill(c, (struct du_rect){cx - 5, cy - 5, 11, 11}, DU_WINDOW);
            du_triangle(c, cx, cy, it->expanded ? 1 : 3, 4, desk_mix(DU_INK, DU_SHADOW, 140));
        } else if (it->children) {
            struct du_rect box = {cx - 4, cy - 4, 9, 9};
            du_fill(c, box, DU_WINDOW); du_outline(c, box, DU_SHADOW);
            du_hline(c, cx - 2, cy, 5, DU_INK);
            if (!it->expanded) du_vline(c, cx, cy - 2, 5, DU_INK);
        }
        int sel = it->selected || (!w->selection_external && w->selection_active && i == w->active);
        du_icon_blit(c, cx + 8, r.y + (DU_ROW - 16) / 2, it->icon16, 16, sel && focused && window ? DU_ICON_SELECTED : DU_ICON_NORMAL);
        struct du_rect lab = label_rect(w, &l, i);
        draw_label(c, lab, it->text[0] ? it->text[0] : "", l.area.x + l.area.w - lab.x - 4, sel, i == w->active, focused, window, 1);
    }
    c->clip = old;
    if (l.has_v) { l.v.value = w->scroll; du_sb_draw(c, &l.v, w->drag >= 10 && w->drag < 20 ? w->drag - 10 : 0); }
    if (focused && window && desk_theme->focus_style == DESK_FOCUS_RING) du_focus_ring(c, w->box);
}

/* ---- input ---- */
static void select_event(struct du_ui *u, struct du_widget *w, int index, int keyboard, unsigned key)
{
    w->selection_active = index >= 0;
    du_emit(u, (struct du_event){.type = DU_EV_SELECT, .id = w->id, .index = index, .keyboard = (unsigned)keyboard, .key = key});
}

int du_list_key(struct du_ui *u, struct du_widget *w, unsigned code)
{
    if (w->kind == DU_TREE && w->count && (code == DU_KEY_LEFT || code == DU_KEY_RIGHT || code == 0x4e || code == 0x4a)) {
        const struct du_item *it = &w->items[w->active];
        int expand = code == DU_KEY_RIGHT || code == 0x4e;
        if (it->children && it->expanded != expand) { du_emit(u, (struct du_event){.type = DU_EV_CHANGE, .id = w->id, .index = (int)w->active}); return 1; }
        if (code == DU_KEY_RIGHT && it->children && w->active + 1 < w->count) {
            w->active++; du_list_ensure(w, w->active); select_event(u, w, (int)w->active, 1, code); return 1;
        }
        if (code == DU_KEY_LEFT && it->depth) {
            unsigned p = w->active;
            while (p > 0 && w->items[p].depth >= it->depth) p--;
            w->active = p; du_list_ensure(w, p); select_event(u, w, (int)p, 1, code); return 1;
        }
        return 1;
    }
    int target = du_list_target(w, code);
    if (target >= 0) {
        w->active = (unsigned)target; du_list_ensure(w, w->active);
        select_event(u, w, target, 1, code);
        return 1;
    }
    if (!w->count) return 0;
    if (code == DU_KEY_ENTER && !(u->modifiers & DU_ALT)) { du_emit(u, (struct du_event){.type = DU_EV_OPEN, .id = w->id, .index = (int)w->active, .keyboard = 1}); return 1; }
    if (code == DU_KEY_SPACE && (u->modifiers & DU_CTRL)) { select_event(u, w, (int)w->active, 1, code); return 1; }
    return 0;
}

int du_list_press(struct du_ui *u, struct du_widget *w, int button, int x, int y)
{
    struct layout l = compute(w);
    if (button == 1 && l.has_v && du_contains(l.v.box, x, y)) {
        l.v.value = w->scroll; int part = du_sb_part(&l.v, x, y);
        w->drag = 10 + part;
        if (part == 3) w->drag_offset = du_sb_grab(&l.v, x, y); else w->scroll = du_sb_apply(&l.v, part);
        u->dirty = 1; return 1;
    }
    if (button == 1 && l.has_h && du_contains(l.h.box, x, y)) {
        l.h.value = w->hscroll; int part = du_sb_part(&l.h, x, y);
        w->drag = 20 + part;
        if (part == 3) w->drag_offset = du_sb_grab(&l.h, x, y); else w->hscroll = du_sb_apply(&l.h, part);
        u->dirty = 1; return 1;
    }
    if (w->kind == DU_LISTVIEW && w->view == DU_DETAILS && du_contains(l.header, x, y)) {
        int at = l.header.x;
        for (unsigned c = 0; c < w->column_count; c++) {
            at += w->columns[c].width;
            if (button == 1 && x >= at - 3 && x <= at + 3) { w->drag = 30 + (int)c; w->drag_offset = x - w->columns[c].width; return 1; }
        }
        int col = du_header_hit(w, x, y);
        if (button == 1 && col >= 0) du_emit(u, (struct du_event){.type = DU_EV_SORT, .id = w->id, .index = col});
        return 1;
    }
    int toggle = du_tree_toggle_hit(w, x, y);
    if (toggle >= 0 && button == 1) { du_emit(u, (struct du_event){.type = DU_EV_CHANGE, .id = w->id, .index = toggle}); return 1; }
    int index = du_list_hit(w, x, y);
    if (button == 2) {
        if (index >= 0) w->active = (unsigned)index;
        du_emit(u, (struct du_event){.type = DU_EV_CONTEXT, .id = w->id, .index = index, .x = x, .y = y});
        return 1;
    }
    if (button != 1) return 1;
    int dbl = index >= 0 && u->last_click_widget == w->id && u->last_click_index == index &&
              u->now - u->last_click_ns < DU_DOUBLE_CLICK_NS &&
              x - u->last_click_x <= 4 && u->last_click_x - x <= 4 && y - u->last_click_y <= 4 && u->last_click_y - y <= 4;
    if (index >= 0) w->active = (unsigned)index;
    select_event(u, w, index, 0, 0);
    if (dbl) {
        u->last_click_widget = -1;
        du_emit(u, (struct du_event){.type = DU_EV_OPEN, .id = w->id, .index = index});
    } else {
        if (w->file_drag && index>=0) { w->drag=50; w->file_drag_x=x; w->file_drag_y=y; }
        u->last_click_widget = w->id; u->last_click_index = index; u->last_click_ns = u->now;
        u->last_click_x = x; u->last_click_y = y;
    }
    return 1;
}

void du_list_motion(struct du_ui *u, struct du_widget *w, int x, int y)
{
    struct layout l = compute(w);
    if (w->drag==50 && (abs(x-w->file_drag_x)>4 || abs(y-w->file_drag_y)>4)) { w->drag=51; u->last_click_widget=-1; }
    if (w->drag == 13 && l.has_v) { l.v.value = w->scroll; w->scroll = du_sb_drag(&l.v, x, y, w->drag_offset); u->dirty = 1; }
    else if (w->drag == 23 && l.has_h) { l.h.value = w->hscroll; w->hscroll = du_sb_drag(&l.h, x, y, w->drag_offset); u->dirty = 1; }
    else if (w->drag >= 30 && w->drag < 30 + DU_COLUMNS) {
        int width = x - w->drag_offset;
        w->columns[w->drag - 30].width = width < 24 ? 24 : width > 600 ? 600 : width;
        u->dirty = 1;
    }
}

void du_list_release(struct du_ui *u, struct du_widget *w)
{
    int drag=w->drag; w->drag=0; u->dirty=1;
    if (drag==51 && w->file_drag) du_emit(u,(struct du_event){.type=DU_EV_DRAG,.id=w->id,.index=(int)w->active,.x=u->mouse_x,.y=u->mouse_y});
}
