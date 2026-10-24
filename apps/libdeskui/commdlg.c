/* SPDX-License-Identifier: MIT
 * G16 common dialogs: Font, Color, Browse for Folder, Find and Replace.
 * Modal panels inside the client window like every toolkit dialog; behaviour
 * and sources in docs/design/common-dialogs.md. The basic-colour table is the
 * one of the Windows common colour dialog (as in Wine's comdlg32/colordlg.c,
 * LGPL 2.1); the HSL arithmetic is Microsoft KB Q29240 (HLSMAX 240). */
#include "internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define BTN_W 75
#define BTN_H 23
#define CD_BUTTON (-1000)               /* the first two dialog items close the dialog (dialog.c ID_BUTTON) */

/* Truncating copy: names longer than the field are cut, never refused. */
static void copy_str(char *dst, size_t size, const char *src)
{
    size_t n = strlen(src);
    if (n >= size) n = size - 1;
    memcpy(dst, src, n); dst[n] = 0;
}
static unsigned lower(unsigned c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static void trim(char *s)
{
    char *start = s; while (*start == ' ') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    size_t n = strlen(s); while (n && s[n - 1] == ' ') s[--n] = 0;
}
static int ci_prefix(const char *text, const char *prefix)
{
    for (; *prefix; text++, prefix++) if (lower((unsigned char)*text) != lower((unsigned char)*prefix)) return 0;
    return 1;
}
static void damage(struct du_ui *u) { du_dialog_damage(u); }
static int ci_cmp(const char *a, const char *b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) { a++; b++; }
    return (int)lower((unsigned char)*a) - (int)lower((unsigned char)*b);
}

/* ======================================================================= */
/* Find and Replace                                                        */
/* ======================================================================= */
static int word_char(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c >= 0x80; }

int du_find_text(const char *text, size_t length, size_t from, const struct du_find *f, size_t *lo, size_t *hi)
{
    size_t n = strlen(f->what);
    if (!n || n > length) return 0;
    int down = (f->flags & DU_FIND_DOWN) != 0, whole = (f->flags & DU_FIND_WHOLE) != 0, match_case = (f->flags & DU_FIND_CASE) != 0;
    int64_t i = down ? (int64_t)from : from >= n ? (int64_t)(from - n) : -1;
    for (; i >= 0 && (size_t)i + n <= length; i += down ? 1 : -1) {
        size_t k = 0;
        for (; k < n; k++) {
            unsigned a = (unsigned char)text[(size_t)i + k], b = (unsigned char)f->what[k];
            if (match_case ? a != b : lower(a) != lower(b)) break;
        }
        if (k < n) continue;
        if (whole && (((size_t)i && word_char((unsigned char)text[(size_t)i - 1])) ||
                      ((size_t)i + n < length && word_char((unsigned char)text[(size_t)i + n])))) continue;
        *lo = (size_t)i; *hi = (size_t)i + n;
        return 1;
    }
    return 0;
}

enum { FD_NEXT, FD_CANCEL, FD_WHAT_L, FD_WHAT, FD_WITH_L, FD_WITH, FD_WHOLE, FD_CASE, FD_GROUP, FD_UP, FD_DOWN, FD_REPLACE, FD_ALL, FD_STATUS };
enum { FID_WHAT = -10300, FID_WITH, FID_WHOLE, FID_CASE, FID_UP, FID_DOWN, FID_REPLACE, FID_ALL, FID_STATUS };
struct find_state { du_find_fn fn; void *ctx; int replace; };

static struct du_find find_gather(struct du_dialog *d, int replace)
{
    struct du_find f; memset(&f, 0, sizeof(f));
    copy_str(f.what, sizeof(f.what), d->items[FD_WHAT].text);
    if (replace) copy_str(f.with, sizeof(f.with), d->items[FD_WITH].text);
    f.flags = (d->items[FD_DOWN].value ? DU_FIND_DOWN : 0) | (d->items[FD_WHOLE].value ? DU_FIND_WHOLE : 0) | (d->items[FD_CASE].value ? DU_FIND_CASE : 0);
    if (replace) f.flags |= DU_FIND_DOWN;       /* Replace has no direction: it searches forward */
    return f;
}

void du_find_status(struct du_ui *u, const char *text)
{
    if (u->dialog.kind != DU_DLG_FIND) return;
    du_set_text(&u->dialog.items[FD_STATUS], text); du_invalidate(u, u->dialog.items[FD_STATUS].box);
}

static void find_enable(struct du_dialog *d)
{
    int empty = !d->items[FD_WHAT].text[0];
    d->items[FD_NEXT].disabled = (uint8_t)empty;
    d->items[FD_REPLACE].disabled = d->items[FD_ALL].disabled = (uint8_t)empty;
}

static void find_run(struct du_ui *u, int action)
{
    struct du_dialog *d = &u->dialog; struct find_state *s = d->state;
    if (!d->items[FD_WHAT].text[0]) return;
    struct du_find f = find_gather(d, s->replace);
    du_find_status(u, "");
    if (s->fn) s->fn(u, action, &f, s->ctx);
    if (u->dialog.kind == DU_DLG_FIND) damage(u);
}

static int find_accept(struct du_ui *u, int button, void *arg)
{
    (void)arg;
    if (button != 0) return 0;               /* Cancel: close */
    find_run(u, DU_FIND_NEXT);               /* Enter or Find Next: the dialog stays open */
    return 1;
}

static int find_event(struct du_ui *u, const struct du_event *e)
{
    struct du_dialog *d = &u->dialog;
    if (e->type == DU_EV_CHANGE && e->id == FID_WHAT) { find_enable(d); du_find_status(u, ""); du_invalidate(u, d->box); return 1; }
    if (e->type == DU_EV_ACTIVATE && e->id == FID_REPLACE) { find_run(u, DU_FIND_REPLACE); return 1; }
    if (e->type == DU_EV_ACTIVATE && e->id == FID_ALL) { find_run(u, DU_FIND_REPLACE_ALL); return 1; }
    return 0;
}

int du_find_dialog(struct du_ui *u, int id, int replace, const struct du_find *initial, du_find_fn fn, void *ctx)
{
    unsigned flags = initial ? initial->flags : DU_FIND_DOWN;
    du_dialog_begin(u, DU_DLG_FIND, id, replace ? "Replace" : "Find", "");
    struct du_dialog *d = &u->dialog;
    struct find_state *s = calloc(1, sizeof(*s));
    if (!s) { du_dialog_close(u); return -ENOMEM; }
    s->fn = fn; s->ctx = ctx; s->replace = replace != 0;
    d->state = s; d->free_state = free;
    d->buttons = 2; d->cancel = 1; d->focus = FD_WHAT; d->accept = find_accept; d->on_event = find_event;
    int w = 404, h = replace ? 204 : 180;
    du_dialog_place(u, w, h);
    struct du_rect b = d->box; int y = du_dialog_body_y(d) + 14, bx = b.x + b.w - 14 - 96, ex = b.x + 100;
    du_dialog_item(u, FD_NEXT, DU_BUTTON, CD_BUTTON, "&Find Next")->box = (struct du_rect){bx, y - 2, 96, BTN_H};
    du_dialog_item(u, FD_CANCEL, DU_BUTTON, CD_BUTTON - 1, replace ? "Close" : "Cancel")->box = (struct du_rect){bx, y - 2 + (replace ? 3 : 1) * 28, 96, BTN_H};
    du_dialog_item(u, FD_WHAT_L, DU_LABEL, 0, "Fi&nd what:")->box = (struct du_rect){b.x + 14, y, 82, 22};
    struct du_widget *what = du_dialog_item(u, FD_WHAT, DU_EDIT, FID_WHAT, initial ? initial->what : "");
    what->box = (struct du_rect){ex, y, bx - 10 - ex, 22}; what->anchor = 0;
    int row = y + 30;
    du_dialog_item(u, FD_WITH_L, DU_LABEL, 0, "Re&place with:")->box = (struct du_rect){b.x + 14, row, 82, 22};
    struct du_widget *with = du_dialog_item(u, FD_WITH, DU_EDIT, FID_WITH, initial ? initial->with : "");
    with->box = (struct du_rect){ex, row, bx - 10 - ex, 22};
    with->hidden = !replace; d->items[FD_WITH_L].hidden = !replace;
    du_dialog_item(u, FD_REPLACE, DU_BUTTON, FID_REPLACE, "&Replace")->box = (struct du_rect){bx, y - 2 + 28, 96, BTN_H};
    du_dialog_item(u, FD_ALL, DU_BUTTON, FID_ALL, "Replace &All")->box = (struct du_rect){bx, y - 2 + 56, 96, BTN_H};
    d->items[FD_REPLACE].hidden = d->items[FD_ALL].hidden = !replace;
    int cy = row + (replace ? 34 : 32);
    struct du_widget *whole = du_dialog_item(u, FD_WHOLE, DU_CHECKBOX, FID_WHOLE, "Match &whole word only");
    whole->box = (struct du_rect){b.x + 14, cy, 170, 20}; whole->value = (flags & DU_FIND_WHOLE) != 0;
    struct du_widget *cs = du_dialog_item(u, FD_CASE, DU_CHECKBOX, FID_CASE, "Match &case");
    cs->box = (struct du_rect){b.x + 14, cy + 24, 170, 20}; cs->value = (flags & DU_FIND_CASE) != 0;
    struct du_widget *group = du_dialog_item(u, FD_GROUP, DU_GROUP, 0, "Direction");
    group->box = (struct du_rect){b.x + 196, cy - 8, 96, 56};
    struct du_widget *up = du_dialog_item(u, FD_UP, DU_RADIO, FID_UP, "&Up");
    up->box = (struct du_rect){b.x + 206, cy + 8, 60, 18}; up->total = 3041; up->value = !(flags & DU_FIND_DOWN);
    struct du_widget *down = du_dialog_item(u, FD_DOWN, DU_RADIO, FID_DOWN, "&Down");
    down->box = (struct du_rect){b.x + 206, cy + 26, 60, 18}; down->total = 3041; down->value = (flags & DU_FIND_DOWN) != 0;
    if (replace) group->hidden = up->hidden = down->hidden = 1;   /* Replace has no direction */
    du_dialog_item(u, FD_STATUS, DU_LABEL, 0, "")->box = (struct du_rect){b.x + 14, b.y + b.h - 3 - 24, b.w - 28, 16};
    find_enable(d);
    return 0;
}

/* ======================================================================= */
/* Colour                                                                  */
/* ======================================================================= */
#define HLSMAX 240
#define RGBMAX 255
static const uint32_t basic_bgr[48] = {
    0x008080FF, 0x0080FFFF, 0x0080FF80, 0x0080FF00, 0x00FFFF80, 0x00FF8000, 0x00C080FF, 0x00FF80FF,
    0x000000FF, 0x0000FFFF, 0x0000FF80, 0x0040FF00, 0x00FFFF00, 0x00C08000, 0x00C08080, 0x00FF00FF,
    0x00404080, 0x004080FF, 0x0000FF00, 0x00808000, 0x00804000, 0x00FF8080, 0x00400080, 0x008000FF,
    0x00000080, 0x000080FF, 0x00008000, 0x00408000, 0x00FF0000, 0x00A00000, 0x00800080, 0x00FF0080,
    0x00000040, 0x00004080, 0x00004000, 0x00404000, 0x00800000, 0x00400000, 0x00400040, 0x00800040,
    0x00000000, 0x00008080, 0x00408080, 0x00808080, 0x00808040, 0x00C0C0C0, 0x00400040, 0x00FFFFFF};
static uint32_t basic_rgb(unsigned i) { uint32_t c = basic_bgr[i]; return ((c & 0xff) << 16) | (c & 0xff00) | ((c >> 16) & 0xff); }

void du_rgb_to_hsl(uint32_t rgb, unsigned *h, unsigned *s, unsigned *l)
{
    int r = (int)((rgb >> 16) & 0xff), g = (int)((rgb >> 8) & 0xff), b = (int)(rgb & 0xff);
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int L = (((mx + mn) * HLSMAX) + RGBMAX) / (2 * RGBMAX), S, H;
    if (mx == mn) { S = 0; H = HLSMAX * 2 / 3; }
    else {
        if (L <= HLSMAX / 2) S = (((mx - mn) * HLSMAX) + ((mx + mn) / 2)) / (mx + mn);
        else S = (((mx - mn) * HLSMAX) + ((2 * RGBMAX - mx - mn) / 2)) / (2 * RGBMAX - mx - mn);
        int rd = (((mx - r) * (HLSMAX / 6)) + ((mx - mn) / 2)) / (mx - mn);
        int gd = (((mx - g) * (HLSMAX / 6)) + ((mx - mn) / 2)) / (mx - mn);
        int bd = (((mx - b) * (HLSMAX / 6)) + ((mx - mn) / 2)) / (mx - mn);
        if (r == mx) H = bd - gd; else if (g == mx) H = HLSMAX / 3 + rd - bd; else H = HLSMAX * 2 / 3 + gd - rd;
        if (H < 0) H += HLSMAX;
        if (H >= HLSMAX) H -= HLSMAX;
    }
    *h = (unsigned)H; *s = (unsigned)S; *l = (unsigned)L;
}

static int hue_to_rgb(int n1, int n2, int hue)
{
    if (hue < 0) hue += HLSMAX;
    if (hue > HLSMAX) hue -= HLSMAX;
    if (hue < HLSMAX / 6) return n1 + (((n2 - n1) * hue + (HLSMAX / 12)) / (HLSMAX / 6));
    if (hue < HLSMAX / 2) return n2;
    if (hue < (HLSMAX * 2) / 3) return n1 + (((n2 - n1) * (((HLSMAX * 2) / 3) - hue) + (HLSMAX / 12)) / (HLSMAX / 6));
    return n1;
}

uint32_t du_hsl_to_rgb(unsigned hh, unsigned ss, unsigned ll)
{
    int H = (int)hh, S = (int)ss, L = (int)ll, R, G, B;
    if (S > HLSMAX) S = HLSMAX;
    if (L > HLSMAX) L = HLSMAX;
    if (!S) R = G = B = (L * RGBMAX) / HLSMAX;
    else {
        int m2 = L <= HLSMAX / 2 ? (L * (HLSMAX + S) + (HLSMAX / 2)) / HLSMAX : L + S - ((L * S) + (HLSMAX / 2)) / HLSMAX, m1 = 2 * L - m2;
        R = (hue_to_rgb(m1, m2, H + HLSMAX / 3) * RGBMAX + (HLSMAX / 2)) / HLSMAX;
        G = (hue_to_rgb(m1, m2, H) * RGBMAX + (HLSMAX / 2)) / HLSMAX;
        B = (hue_to_rgb(m1, m2, H - HLSMAX / 3) * RGBMAX + (HLSMAX / 2)) / HLSMAX;
    }
    if (R > RGBMAX) R = RGBMAX;
    if (G > RGBMAX) G = RGBMAX;
    if (B > RGBMAX) B = RGBMAX;
    if (R < 0) R = 0;
    if (G < 0) G = 0;
    if (B < 0) B = 0;
    return ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
}

int du_color_parse(const char *text, uint32_t *rgb)
{
    if (!text || text[0] != '#' || strlen(text) != 7) return -EINVAL;
    char *end; unsigned long v = strtoul(text + 1, &end, 16);
    if (*end) return -EINVAL;
    *rgb = (uint32_t)v;
    return 0;
}

/* Labels precede the control their mnemonic selects; each channel is label, edit, up-down. */
enum { CL_OK, CL_CANCEL, CL_BASIC_L, CL_BASIC, CL_CUSTOM_L, CL_CUSTOM, CL_DEFINE, CL_SPECTRUM, CL_LUM, CL_PREVIEW, CL_SOLID_L,
       CL_CH, CL_ADD = CL_CH + 18, CL_ITEMS };
#define CL_CH_L(i) (CL_CH + 3 * (i))
#define CL_ED(i) (CL_CH + 3 * (i) + 1)
#define CL_UD(i) (CL_CH + 3 * (i) + 2)
enum { CID_DEFINE = -10400, CID_ADD, CID_BASIC, CID_CUSTOM, CID_SPECTRUM, CID_LUM, CID_PREVIEW, CID_ED = -10420, CID_UD = -10440 };
#define CELL_W 24
#define CELL_H 20
#define SPEC_W 150
#define SPEC_H 120
#define LUM_W 14
#define COLOR_W0 222
#define COLOR_W1 500
struct color_state {
    uint32_t rgb, own[16], *custom;
    unsigned h, s, l;
    int sel_basic, sel_custom, expanded, prevent_full, next_custom, hold;
};

static struct color_state *color_of(struct du_ui *u) { return u->dialog.state; }
static const unsigned channel_max[6] = {239, HLSMAX, HLSMAX, 255, 255, 255};   /* hue, sat, lum, red, green, blue */

static void color_show_values(struct du_ui *u, int skip)
{
    struct du_dialog *d = &u->dialog; struct color_state *c = color_of(u);
    unsigned v[6] = {c->h, c->s, c->l, (c->rgb >> 16) & 0xff, (c->rgb >> 8) & 0xff, c->rgb & 0xff};
    for (int i = 0; i < 6; i++) {
        d->items[CL_UD(i)].value = (int)v[i];
        if (i == skip) continue;
        char t[8]; snprintf(t, sizeof(t), "%u", v[i]);
        if (strcmp(d->items[CL_ED(i)].text, t)) { du_set_text(&d->items[CL_ED(i)], t); du_invalidate(u, d->items[CL_ED(i)].box); }
    }
    du_invalidate(u, d->box);
}

static void color_set_rgb(struct du_ui *u, uint32_t rgb, int skip)
{
    struct color_state *c = color_of(u);
    c->rgb = rgb & 0xffffff;
    du_rgb_to_hsl(c->rgb, &c->h, &c->s, &c->l);
    color_show_values(u, skip);
}
static void color_set_hsl(struct du_ui *u, unsigned h, unsigned s, unsigned l, int skip)
{
    struct color_state *c = color_of(u);
    c->h = h; c->s = s; c->l = l;
    c->rgb = du_hsl_to_rgb(h, s, l);
    color_show_values(u, skip);
}

static void color_layout(struct du_ui *u)
{
    struct du_dialog *d = &u->dialog; struct color_state *c = color_of(u);
    struct du_rect b = d->box; int x = b.x + 14, y = du_dialog_body_y(d) + 10, xr = x + CELL_W * 8 + 22, vis = c->expanded;
    d->items[CL_BASIC_L].box = (struct du_rect){x, y, 192, 15};
    d->items[CL_BASIC].box = (struct du_rect){x, y + 16, CELL_W * 8, CELL_H * 6};
    d->items[CL_CUSTOM_L].box = (struct du_rect){x, y + 144, 192, 15};
    d->items[CL_CUSTOM].box = (struct du_rect){x, y + 160, CELL_W * 8, CELL_H * 2};
    d->items[CL_OK].box = (struct du_rect){x, y + 208, BTN_W, BTN_H};
    d->items[CL_CANCEL].box = (struct du_rect){x + 117, y + 208, BTN_W, BTN_H};
    d->items[CL_DEFINE].box = (struct du_rect){x, y + 236, CELL_W * 8, BTN_H};
    d->items[CL_SPECTRUM].box = (struct du_rect){xr, y, SPEC_W, SPEC_H};
    d->items[CL_LUM].box = (struct du_rect){xr + SPEC_W + 8, y, LUM_W, SPEC_H};
    d->items[CL_PREVIEW].box = (struct du_rect){xr, y + 132, 72, 44};
    d->items[CL_SOLID_L].box = (struct du_rect){xr, y + 180, 72, 15};
    d->items[CL_ADD].box = (struct du_rect){xr, y + 236, 180, BTN_H};
    for (int i = 0; i < 6; i++) {
        int col = i / 3, row = i % 3, lx = xr + 80 + col * 100, ly = y + 132 + row * 28;
        d->items[CL_CH_L(i)].box = (struct du_rect){lx, ly, 34, 22};
        d->items[CL_ED(i)].box = (struct du_rect){lx + 36, ly, 32, 22};
        d->items[CL_UD(i)].box = (struct du_rect){lx + 68, ly, 16, 22};
    }
    for (int i = CL_SPECTRUM; i < CL_ITEMS; i++) d->items[i].hidden = (uint8_t)!vis;
    d->items[CL_DEFINE].disabled = (uint8_t)(c->prevent_full || vis);
}

static void color_resize(struct du_ui *u)
{
    struct color_state *c = color_of(u);
    du_dialog_place(u, c->expanded ? COLOR_W1 : COLOR_W0, 3 + 24 + 10 + 236 + BTN_H + 14);
    color_layout(u);
    damage(u);
}

static int color_accept(struct du_ui *u, int button, void *arg)
{
    (void)arg;
    if (button) return 0;
    char text[12]; snprintf(text, sizeof(text), "#%06X", (unsigned)color_of(u)->rgb);
    du_dialog_finish(u, 0, text);
    return 1;
}

static void color_cell(struct du_ui *u, int custom, int x, int y)
{
    struct du_dialog *d = &u->dialog; struct color_state *c = color_of(u);
    struct du_rect r = d->items[custom ? CL_CUSTOM : CL_BASIC].box;
    int col = (x - r.x) / CELL_W, row = (y - r.y) / CELL_H;
    if (col < 0 || col > 7 || row < 0 || row >= (custom ? 2 : 6)) return;
    int i = row * 8 + col;
    if (custom) { c->sel_custom = i; c->sel_basic = -1; color_set_rgb(u, c->custom[i], -1); }
    else { c->sel_basic = i; c->sel_custom = -1; color_set_rgb(u, basic_rgb((unsigned)i), -1); }
}

static void color_spectrum(struct du_ui *u, int x, int y)
{
    struct du_dialog *d = &u->dialog; struct color_state *c = color_of(u);
    struct du_rect r = d->items[CL_SPECTRUM].box;
    int px = x - r.x, py = y - r.y;
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px > r.w - 1) px = r.w - 1;
    if (py > r.h - 1) py = r.h - 1;
    c->sel_basic = c->sel_custom = -1;
    color_set_hsl(u, (unsigned)(px * 239 / (r.w - 1)), (unsigned)(HLSMAX - py * HLSMAX / (r.h - 1)), c->l, -1);
}
static void color_lum(struct du_ui *u, int y)
{
    struct du_dialog *d = &u->dialog; struct color_state *c = color_of(u);
    struct du_rect r = d->items[CL_LUM].box;
    int py = y - r.y; if (py < 0) py = 0; if (py > r.h - 1) py = r.h - 1;
    c->sel_basic = c->sel_custom = -1;
    color_set_hsl(u, c->h, c->s, (unsigned)(HLSMAX - py * HLSMAX / (r.h - 1)), -1);
}

static int color_event(struct du_ui *u, const struct du_event *e)
{
    struct du_dialog *d = &u->dialog; struct color_state *c = color_of(u);
    if (e->type == DU_EV_POINTER && (e->index == DU_POINTER_DOWN || (e->index == DU_POINTER_MOVE && e->key == 1))) {
        if (e->index == DU_POINTER_DOWN && e->key != 1) return 1;
        if (e->index == DU_POINTER_DOWN) c->hold = e->id;
        if (e->index == DU_POINTER_MOVE && c->hold != e->id) return 1;
        if (e->id == CID_BASIC) color_cell(u, 0, e->x, e->y);
        else if (e->id == CID_CUSTOM) color_cell(u, 1, e->x, e->y);
        else if (e->id == CID_SPECTRUM) color_spectrum(u, e->x, e->y);
        else if (e->id == CID_LUM) color_lum(u, e->y);
        return 1;
    }
    if (e->type == DU_EV_POINTER) { if (e->index == DU_POINTER_UP || e->index == DU_POINTER_CANCEL) c->hold = 0; return 1; }
    if (e->type == DU_EV_ACTIVATE && e->id == CID_DEFINE) { c->expanded = 1; color_resize(u); return 1; }
    if (e->type == DU_EV_ACTIVATE && e->id == CID_ADD) {
        c->custom[c->next_custom] = c->rgb; c->sel_custom = c->next_custom; c->sel_basic = -1;
        c->next_custom = (c->next_custom + 1) % 16; du_invalidate(u, d->items[CL_CUSTOM].box); return 1;
    }
    if (e->type == DU_EV_CHANGE && e->id >= CID_ED - 5 && e->id <= CID_ED) {      /* an edit changed */
        int i = CID_ED - e->id;
        char *end; unsigned long v = strtoul(d->items[CL_ED(i)].text, &end, 10);
        if (v > channel_max[i]) v = channel_max[i];
        c->sel_basic = c->sel_custom = -1;
        if (i < 3) { unsigned hsl[3] = {c->h, c->s, c->l}; hsl[i] = (unsigned)v; color_set_hsl(u, hsl[0], hsl[1], hsl[2], i); }
        else { uint32_t ch[3] = {(c->rgb >> 16) & 0xff, (c->rgb >> 8) & 0xff, c->rgb & 0xff}; ch[i - 3] = (uint32_t)v; color_set_rgb(u, (ch[0] << 16) | (ch[1] << 8) | ch[2], i); }
        return 1;
    }
    if (e->type == DU_EV_CHANGE && e->id >= CID_UD - 5 && e->id <= CID_UD) {      /* an up-down changed */
        int i = CID_UD - e->id; unsigned v = (unsigned)e->index;
        c->sel_basic = c->sel_custom = -1;
        if (i < 3) { unsigned hsl[3] = {c->h, c->s, c->l}; hsl[i] = v; color_set_hsl(u, hsl[0], hsl[1], hsl[2], -1); }
        else { uint32_t ch[3] = {(c->rgb >> 16) & 0xff, (c->rgb >> 8) & 0xff, c->rgb & 0xff}; ch[i - 3] = v; color_set_rgb(u, (ch[0] << 16) | (ch[1] << 8) | ch[2], -1); }
        return 1;
    }
    return 0;
}

static int color_key(struct du_ui *u, unsigned code)
{
    struct du_dialog *d = &u->dialog; struct color_state *c = color_of(u);
    int f = d->focus; int custom = f == CL_CUSTOM;
    if ((f != CL_BASIC && f != CL_CUSTOM) || (code != DU_KEY_LEFT && code != DU_KEY_RIGHT && code != DU_KEY_UP && code != DU_KEY_DOWN)) return 0;
    int *sel = custom ? &c->sel_custom : &c->sel_basic, rows = custom ? 2 : 6;
    int i = *sel < 0 ? 0 : *sel, col = i % 8, row = i / 8;
    if (*sel >= 0) {
        if (code == DU_KEY_LEFT && col) col--; else if (code == DU_KEY_RIGHT && col < 7) col++;
        else if (code == DU_KEY_UP && row) row--; else if (code == DU_KEY_DOWN && row < rows - 1) row++;
    }
    *sel = row * 8 + col; if (custom) c->sel_basic = -1; else c->sel_custom = -1;
    color_set_rgb(u, custom ? c->custom[*sel] : basic_rgb((unsigned)*sel), -1);
    return 1;
}

static void color_frame_cell(struct du_canvas *cv, struct du_rect r, uint32_t rgb, int selected)
{
    du_fill(cv, (struct du_rect){r.x + 2, r.y + 2, r.w - 4, r.h - 4}, rgb);
    du_edge(cv, (struct du_rect){r.x + 1, r.y + 1, r.w - 2, r.h - 2}, DU_SUNKEN_THIN);
    if (selected) { du_outline(cv, r, DU_INK); du_outline(cv, (struct du_rect){r.x + 1, r.y + 1, r.w - 2, r.h - 2}, DU_INK); }
}

static void color_draw(struct du_ui *u)
{
    struct du_dialog *d = &u->dialog; struct color_state *c = color_of(u); struct du_canvas *cv = &u->canvas;
    struct du_rect keep = cv->clip;
    for (int i = 0; i < 48; i++) {
        struct du_rect r = d->items[CL_BASIC].box; r = (struct du_rect){r.x + (i % 8) * CELL_W, r.y + (i / 8) * CELL_H, CELL_W, CELL_H};
        color_frame_cell(cv, r, basic_rgb((unsigned)i), i == c->sel_basic);
    }
    for (int i = 0; i < 16; i++) {
        struct du_rect r = d->items[CL_CUSTOM].box; r = (struct du_rect){r.x + (i % 8) * CELL_W, r.y + (i / 8) * CELL_H, CELL_W, CELL_H};
        color_frame_cell(cv, r, c->custom[i], i == c->sel_custom);
    }
    if (d->focus == CL_BASIC && u->focused_window) du_dotted(cv, d->items[CL_BASIC].box);
    if (d->focus == CL_CUSTOM && u->focused_window) du_dotted(cv, d->items[CL_CUSTOM].box);
    if (c->expanded) {
        struct du_rect sp = d->items[CL_SPECTRUM].box, lb = d->items[CL_LUM].box;
        struct du_rect vis = du_intersection(keep, (struct du_rect){sp.x, sp.y, sp.w + 8 + lb.w, sp.h});
        for (int y = vis.y; y < vis.y + vis.h; y++) {
            for (int x = vis.x; x < vis.x + vis.w && x < sp.x + sp.w; x++) {
                int px = x - sp.x, py = y - sp.y;
                if (px < 0 || py < 0 || py >= sp.h) continue;
                du_hline(cv, x, y, 1, du_hsl_to_rgb((unsigned)(px * 239 / (sp.w - 1)), (unsigned)(HLSMAX - py * HLSMAX / (sp.h - 1)), 120));
            }
            if (y >= lb.y && y < lb.y + lb.h) du_hline(cv, lb.x, y, lb.w, du_hsl_to_rgb(c->h, c->s, (unsigned)(HLSMAX - (y - lb.y) * HLSMAX / (lb.h - 1))));
        }
        du_edge(cv, (struct du_rect){sp.x - 2, sp.y - 2, sp.w + 4, sp.h + 4}, DU_SUNKEN_THIN);
        du_edge(cv, (struct du_rect){lb.x - 2, lb.y - 2, lb.w + 4, lb.h + 4}, DU_SUNKEN_THIN);
        /* The marks of the current hue/saturation and luminance. */
        int mx = sp.x + (int)(c->h * (unsigned)(sp.w - 1) / 239), my = sp.y + (int)((HLSMAX - c->s) * (unsigned)(sp.h - 1) / HLSMAX);
        du_hline(cv, mx - 5, my, 4, DU_INK); du_hline(cv, mx + 2, my, 4, DU_INK); du_vline(cv, mx, my - 5, 4, DU_INK); du_vline(cv, mx, my + 2, 4, DU_INK);
        int ly = lb.y + (int)((HLSMAX - c->l) * (unsigned)(lb.h - 1) / HLSMAX);
        du_triangle(cv, lb.x + lb.w + 5, ly, 2, 4, DU_INK);
        struct du_rect pv = d->items[CL_PREVIEW].box;
        du_fill(cv, pv, c->rgb); du_edge(cv, pv, DU_SUNKEN);
    }
    cv->clip = keep;
}

static void color_free(void *state) { free(state); }

int du_color_dialog(struct du_ui *u, int id, uint32_t initial, uint32_t *custom, unsigned flags)
{
    du_dialog_begin(u, DU_DLG_COLOR, id, "Color", "");
    struct du_dialog *d = &u->dialog;
    struct color_state *c = calloc(1, sizeof(*c));
    if (!c) { du_dialog_close(u); return -ENOMEM; }
    c->custom = custom ? custom : c->own;
    if (!custom) for (int i = 0; i < 16; i++) c->own[i] = 0xffffff;
    c->sel_basic = c->sel_custom = -1; c->expanded = (flags & DU_COLOR_FULL_OPEN) != 0 && !(flags & DU_COLOR_PREVENT_FULL);
    c->prevent_full = (flags & DU_COLOR_PREVENT_FULL) != 0;
    d->state = c; d->free_state = color_free;
    d->buttons = 2; d->cancel = 1; d->focus = CL_BASIC; d->accept = color_accept;
    d->on_event = color_event; d->on_key = color_key; d->on_draw = color_draw;
    du_dialog_item(u, CL_OK, DU_BUTTON, CD_BUTTON, "OK");
    du_dialog_item(u, CL_CANCEL, DU_BUTTON, CD_BUTTON - 1, "Cancel");
    du_dialog_item(u, CL_DEFINE, DU_BUTTON, CID_DEFINE, "&Define Custom Colors >>");
    du_dialog_item(u, CL_BASIC_L, DU_LABEL, 0, "&Basic colors:");
    du_dialog_item(u, CL_BASIC, DU_CANVAS, CID_BASIC, "");
    du_dialog_item(u, CL_CUSTOM_L, DU_LABEL, 0, "&Custom colors:");
    du_dialog_item(u, CL_CUSTOM, DU_CANVAS, CID_CUSTOM, "");
    du_dialog_item(u, CL_SPECTRUM, DU_CANVAS, CID_SPECTRUM, "");
    du_dialog_item(u, CL_LUM, DU_CANVAS, CID_LUM, "");
    du_dialog_item(u, CL_PREVIEW, DU_CANVAS, CID_PREVIEW, "")->disabled = 1;
    du_dialog_item(u, CL_SOLID_L, DU_LABEL, 0, "Color");
    static const char *const names[6] = {"Hu&e:", "&Sat:", "&Lum:", "&Red:", "&Green:", "Bl&ue:"};
    /* Items between the channels and the Add button are created in index order below. */
    for (int i = 0; i < 6; i++) {
        du_dialog_item(u, (unsigned)CL_CH_L(i), DU_LABEL, 0, names[i]);
        du_dialog_item(u, (unsigned)CL_ED(i), DU_EDIT, CID_ED - i, "0")->anchor = 0;
        struct du_widget *ud = du_dialog_item(u, (unsigned)CL_UD(i), DU_UPDOWN, CID_UD - i, "");
        ud->box = (struct du_rect){0, 0, 16, 22};
        du_updown_set(ud, 0, (int)channel_max[i], 0, 0);
        ud->buddy = CID_ED - i;
    }
    du_dialog_item(u, CL_ADD, DU_BUTTON, CID_ADD, "&Add to Custom Colors");
    du_dialog_place(u, c->expanded ? COLOR_W1 : COLOR_W0, 3 + 24 + 10 + 236 + BTN_H + 14);
    color_layout(u);
    color_set_rgb(u, initial, -1);
    damage(u);
    return 0;
}

/* ======================================================================= */
/* Font                                                                    */
/* ======================================================================= */
enum { FN_OK, FN_CANCEL, FN_FACE_L, FN_FACE, FN_FACES, FN_STYLE_L, FN_STYLE, FN_STYLES, FN_SIZE_L, FN_SIZE, FN_SIZES,
       FN_EFFECTS, FN_STRIKE, FN_UNDER, FN_COLOR_L, FN_COLOR, FN_SAMPLE_G, FN_SAMPLE, FN_STATUS, FN_ITEMS };
enum { NID_FACE = -10500, NID_FACES, NID_STYLE, NID_STYLES, NID_SIZE, NID_SIZES, NID_STRIKE, NID_UNDER, NID_COLOR, NID_SAMPLE };
#define FACE_MAX 32
struct font_state {
    struct du_font_choice c;
    char faces[FACE_MAX][64]; struct du_item face_rows[FACE_MAX]; unsigned face_count;
    struct du_item style_rows[4]; struct du_item size_rows[16]; char size_text[16][8]; struct du_item color_rows[16];
    du_sample_fn sample; void *sample_ctx;
};
static const char *const style_names[4] = {"Regular", "Italic", "Bold", "Bold Italic"};
static const unsigned style_values[4] = {0, DU_FONT_ITALIC, DU_FONT_BOLD, DU_FONT_BOLD | DU_FONT_ITALIC};
static const unsigned size_values[16] = {8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72};
static const struct { const char *name; uint32_t rgb; } font_colors[16] = {
    {"Black", 0x000000}, {"Maroon", 0x800000}, {"Green", 0x008000}, {"Olive", 0x808000}, {"Navy", 0x000080}, {"Purple", 0x800080},
    {"Teal", 0x008080}, {"Gray", 0x808080}, {"Silver", 0xc0c0c0}, {"Red", 0xff0000}, {"Lime", 0x00ff00}, {"Yellow", 0xffff00},
    {"Blue", 0x0000ff}, {"Fuchsia", 0xff00ff}, {"Aqua", 0x00ffff}, {"White", 0xffffff}};

void du_font_format(char *out, size_t size, const struct du_font_choice *c)
{
    snprintf(out, size, "%s|%u|%u|%u|%u|%06X", c->family, c->style, c->points, c->underline, c->strikeout, (unsigned)(c->color & 0xffffff));
}

int du_font_parse(const char *text, struct du_font_choice *out)
{
    if (!text) return -EINVAL;
    const char *bar = strchr(text, '|');
    if (!bar || (size_t)(bar - text) >= sizeof(out->family)) return -EINVAL;
    struct du_font_choice c; memset(&c, 0, sizeof(c));
    memcpy(c.family, text, (size_t)(bar - text));
    unsigned style, points, under, strike, color;
    if (sscanf(bar + 1, "%u|%u|%u|%u|%x", &style, &points, &under, &strike, &color) != 5) return -EINVAL;
    c.style = style; c.points = points; c.underline = under; c.strikeout = strike; c.color = color;
    *out = c;
    return 0;
}

static struct font_state *font_of(struct du_ui *u) { return u->dialog.state; }
static int style_index(unsigned style) { return (style & DU_FONT_BOLD) ? ((style & DU_FONT_ITALIC) ? 3 : 2) : ((style & DU_FONT_ITALIC) ? 1 : 0); }

static void font_select_row(struct du_widget *list, unsigned row)
{
    for (unsigned i = 0; i < list->count; i++) ((struct du_item *)list->items)[i].selected = i == row;
    list->active = row; du_list_ensure(list, row);
}

static void font_sync(struct du_ui *u)
{
    struct du_dialog *d = &u->dialog; struct font_state *f = font_of(u);
    du_set_text(&d->items[FN_FACE], f->c.family);
    du_set_text(&d->items[FN_STYLE], style_names[style_index(f->c.style)]);
    font_select_row(&d->items[FN_STYLES], (unsigned)style_index(f->c.style));
    char t[12]; snprintf(t, sizeof(t), "%u", f->c.points); du_set_text(&d->items[FN_SIZE], t);
    unsigned srow = 0; int sfound = 0;
    for (unsigned i = 0; i < 16; i++) if (size_values[i] == f->c.points) { srow = i; sfound = 1; }
    if (sfound) font_select_row(&d->items[FN_SIZES], srow);
    unsigned frow = 0; int ffound = 0;
    for (unsigned i = 0; i < f->face_count; i++) if (!strcmp(f->faces[i], f->c.family)) { frow = i; ffound = 1; }
    if (ffound) font_select_row(&d->items[FN_FACES], frow);
    d->items[FN_STRIKE].value = f->c.strikeout != 0; d->items[FN_UNDER].value = f->c.underline != 0;
    unsigned crow = 0; for (unsigned i = 0; i < 16; i++) if (font_colors[i].rgb == (f->c.color & 0xffffff)) crow = i;
    d->items[FN_COLOR].value = (int)crow;
    du_invalidate(u, d->box);
}

static void font_status(struct du_ui *u, const char *text)
{ du_set_text(&u->dialog.items[FN_STATUS], text); du_invalidate(u, u->dialog.items[FN_STATUS].box); }

static int font_accept(struct du_ui *u, int button, void *arg)
{
    (void)arg;
    if (button) return 0;
    struct du_dialog *d = &u->dialog; struct font_state *f = font_of(u);
    struct du_font_choice c = f->c;
    char face[64]; copy_str(face, sizeof(face), d->items[FN_FACE].text); trim(face);
    char *end; unsigned long pts = strtoul(d->items[FN_SIZE].text, &end, 10);
    if (!face[0]) { font_status(u, "Choose a font name."); return 1; }
    if (!d->items[FN_SIZE].text[0] || *end || pts < 1 || pts > 1638) { font_status(u, "The font size must be between 1 and 1638."); return 1; }
    snprintf(c.family, sizeof(c.family), "%s", face); c.points = (unsigned)pts;
    char text[160]; du_font_format(text, sizeof(text), &c);
    du_dialog_finish(u, 0, text);
    return 1;
}

static void font_prefix_select(struct du_ui *u, int list_slot, int edit_slot)
{
    struct du_dialog *d = &u->dialog;
    struct du_widget *list = &d->items[list_slot];
    for (unsigned i = 0; i < list->count; i++)
        if (list->items[i].text[0] && ci_prefix(list->items[i].text[0], d->items[edit_slot].text) && d->items[edit_slot].text[0]) { font_select_row(list, i); return; }
}

static int font_event(struct du_ui *u, const struct du_event *e)
{
    struct du_dialog *d = &u->dialog; struct font_state *f = font_of(u);
    if (e->type == DU_EV_OPEN) { (void)font_accept(u, 0, NULL); return 1; }   /* Enter in a list is the default button */
    if (e->type == DU_EV_SELECT && e->index >= 0) {
        if (e->id == NID_FACES && (unsigned)e->index < f->face_count) { copy_str(f->c.family, sizeof(f->c.family), f->faces[e->index]); du_set_text(&d->items[FN_FACE], f->c.family); font_select_row(&d->items[FN_FACES], (unsigned)e->index); }
        else if (e->id == NID_STYLES && e->index < 4) { f->c.style = style_values[e->index]; du_set_text(&d->items[FN_STYLE], style_names[e->index]); font_select_row(&d->items[FN_STYLES], (unsigned)e->index); }
        else if (e->id == NID_SIZES && e->index < 16) { f->c.points = size_values[e->index]; du_set_text(&d->items[FN_SIZE], f->size_text[e->index]); font_select_row(&d->items[FN_SIZES], (unsigned)e->index); }
        else if (e->id == NID_COLOR && e->index < 16) f->c.color = font_colors[e->index].rgb;
        else return 0;
        font_status(u, ""); du_invalidate(u, d->box); return 1;
    }
    if (e->type == DU_EV_CHANGE) {
        if (e->id == NID_FACE) { copy_str(f->c.family, sizeof(f->c.family), d->items[FN_FACE].text); font_prefix_select(u, FN_FACES, FN_FACE); }
        else if (e->id == NID_STYLE) font_prefix_select(u, FN_STYLES, FN_STYLE);
        else if (e->id == NID_SIZE) {
            char *end; unsigned long v = strtoul(d->items[FN_SIZE].text, &end, 10);
            if (!*end && v >= 1 && v <= 1638) f->c.points = (unsigned)v;
            for (unsigned i = 0; i < 16; i++) if (size_values[i] == f->c.points) font_select_row(&d->items[FN_SIZES], i);
        }
        else if (e->id == NID_STRIKE) f->c.strikeout = e->index != 0;
        else if (e->id == NID_UNDER) f->c.underline = e->index != 0;
        else return 0;
        font_status(u, ""); du_invalidate(u, d->box); return 1;
    }
    return 0;
}

static void font_draw(struct du_ui *u)
{
    struct du_dialog *d = &u->dialog; struct font_state *f = font_of(u); struct du_canvas *cv = &u->canvas;
    struct du_rect r = d->items[FN_SAMPLE].box, keep = cv->clip;
    du_fill(cv, r, DU_WINDOW); du_edge(cv, r, DU_SUNKEN);
    cv->clip = du_intersection(keep, (struct du_rect){r.x + 2, r.y + 2, r.w - 4, r.h - 4});
    if (f->sample) f->sample(cv, r, &f->c, f->sample_ctx);
    else {
        const char *text = "AaBbYyZz";
        int font = (f->c.style & DU_FONT_BOLD) ? desk_theme->font_bold : desk_theme->font_ui;
        int tw = du_text_width_font(font, text), x = r.x + (r.w - tw) / 2, y = r.y + (r.h - DU_GLYPH_H) / 2;
        du_text_font(cv, font, x, y, text, f->c.color | 0);
        if (f->c.underline) du_hline(cv, x, y + DU_GLYPH_H + 2, tw, f->c.color);
        if (f->c.strikeout) du_hline(cv, x, y + DU_GLYPH_H / 2, tw, f->c.color);
    }
    cv->clip = keep;
}

int du_font_dialog(struct du_ui *u, int id, const struct du_font_choice *initial,
                   const char *const *families, unsigned family_count, unsigned flags,
                   du_sample_fn sample, void *sample_ctx)
{
    static const char *const defaults[3] = {"Liberation Mono", "Liberation Sans", "Liberation Serif"};
    if (!families || !family_count) { families = defaults; family_count = 3; }
    du_dialog_begin(u, DU_DLG_FONT, id, "Font", "");
    struct du_dialog *d = &u->dialog;
    struct font_state *f = calloc(1, sizeof(*f));
    if (!f) { du_dialog_close(u); return -ENOMEM; }
    if (initial) f->c = *initial;
    else { snprintf(f->c.family, sizeof(f->c.family), "%s", families[0]); f->c.points = 10; }
    if (!f->c.points) f->c.points = 10;
    f->sample = sample; f->sample_ctx = sample_ctx;
    for (unsigned i = 0; i < family_count && i < FACE_MAX; i++) { snprintf(f->faces[i], sizeof(f->faces[i]), "%s", families[i] ? families[i] : ""); f->face_rows[i].text[0] = f->faces[i]; f->face_count++; }
    for (int i = 0; i < 4; i++) f->style_rows[i].text[0] = style_names[i];
    for (int i = 0; i < 16; i++) { snprintf(f->size_text[i], sizeof(f->size_text[i]), "%u", size_values[i]); f->size_rows[i].text[0] = f->size_text[i]; f->color_rows[i].text[0] = font_colors[i].name; }
    d->state = f; d->free_state = free;
    d->buttons = 2; d->cancel = 1; d->focus = FN_FACE; d->accept = font_accept; d->on_event = font_event; d->on_draw = font_draw;
    du_dialog_place(u, 490, 3 + 24 + 10 + 156 + 84 + 8 + 20 + 12);
    struct du_rect b = d->box; int y = du_dialog_body_y(d) + 10, x = b.x + 14;
    du_dialog_item(u, FN_OK, DU_BUTTON, CD_BUTTON, "OK")->box = (struct du_rect){b.x + b.w - 14 - BTN_W, y + 16, BTN_W, BTN_H};
    du_dialog_item(u, FN_CANCEL, DU_BUTTON, CD_BUTTON - 1, "Cancel")->box = (struct du_rect){b.x + b.w - 14 - BTN_W, y + 44, BTN_W, BTN_H};
    du_dialog_item(u, FN_FACE_L, DU_LABEL, 0, "&Font:")->box = (struct du_rect){x, y, 160, 15};
    du_dialog_item(u, FN_FACE, DU_EDIT, NID_FACE, f->c.family)->box = (struct du_rect){x, y + 16, 160, 22};
    struct du_widget *faces = du_dialog_item(u, FN_FACES, DU_TREE, NID_FACES, "");
    faces->box = (struct du_rect){x, y + 40, 160, 110}; faces->items = f->face_rows; faces->count = f->face_count; faces->inactive_selection = 1;
    du_dialog_item(u, FN_STYLE_L, DU_LABEL, 0, "Font st&yle:")->box = (struct du_rect){x + 168, y, 100, 15};
    du_dialog_item(u, FN_STYLE, DU_EDIT, NID_STYLE, "")->box = (struct du_rect){x + 168, y + 16, 100, 22};
    struct du_widget *styles = du_dialog_item(u, FN_STYLES, DU_TREE, NID_STYLES, "");
    styles->box = (struct du_rect){x + 168, y + 40, 100, 110}; styles->items = f->style_rows; styles->count = 4; styles->inactive_selection = 1;
    du_dialog_item(u, FN_SIZE_L, DU_LABEL, 0, "&Size:")->box = (struct du_rect){x + 276, y, 60, 15};
    du_dialog_item(u, FN_SIZE, DU_EDIT, NID_SIZE, "")->box = (struct du_rect){x + 276, y + 16, 80, 22};
    struct du_widget *sizes = du_dialog_item(u, FN_SIZES, DU_TREE, NID_SIZES, "");
    sizes->box = (struct du_rect){x + 276, y + 40, 80, 110}; sizes->items = f->size_rows; sizes->count = 16; sizes->inactive_selection = 1;
    int ey = y + 160;
    du_dialog_item(u, FN_EFFECTS, DU_GROUP, 0, "Effects")->box = (struct du_rect){x, ey, 160, 84};
    du_dialog_item(u, FN_STRIKE, DU_CHECKBOX, NID_STRIKE, "Stri&keout")->box = (struct du_rect){x + 10, ey + 16, 120, 18};
    du_dialog_item(u, FN_UNDER, DU_CHECKBOX, NID_UNDER, "&Underline")->box = (struct du_rect){x + 10, ey + 36, 120, 18};
    du_dialog_item(u, FN_COLOR_L, DU_LABEL, 0, "&Color:")->box = (struct du_rect){x + 10, ey + 58, 42, 20};
    struct du_widget *color = du_dialog_item(u, FN_COLOR, DU_COMBO, NID_COLOR, "");
    color->box = (struct du_rect){x + 54, ey + 56, 96, 22}; du_combo_set(color, f->color_rows, 16, 0);
    du_dialog_item(u, FN_SAMPLE_G, DU_GROUP, 0, "Sample")->box = (struct du_rect){x + 168, ey, 188, 84};
    du_dialog_item(u, FN_SAMPLE, DU_CANVAS, NID_SAMPLE, "")->box = (struct du_rect){x + 178, ey + 18, 168, 56};
    d->items[FN_SAMPLE].disabled = 1;
    du_dialog_item(u, FN_STATUS, DU_LABEL, 0, "")->box = (struct du_rect){x, b.y + b.h - 3 - 24, b.w - 28, 16};
    if (flags & DU_FONT_NO_EFFECTS) { d->items[FN_EFFECTS].hidden = d->items[FN_STRIKE].hidden = d->items[FN_UNDER].hidden = d->items[FN_COLOR_L].hidden = d->items[FN_COLOR].hidden = 1; }
    font_sync(u);
    return 0;
}

/* ======================================================================= */
/* Browse for Folder                                                       */
/* ======================================================================= */
#define NODE_MAX 384
enum { BF_OK, BF_CANCEL, BF_TREE, BF_NEW, BF_STATUS, BF_ITEMS };
enum { BID_TREE = -10600, BID_NEW = -10601 };
struct node { int parent; uint8_t depth, expanded, loaded, has_children; char name[72]; };
struct folder_state {
    struct node nodes[NODE_MAX]; unsigned count;
    unsigned visible[NODE_MAX], shown;       /* row -> node */
    struct du_item rows[NODE_MAX];
    char root[DU_TEXT_MAX];
    du_lister lister; void *lister_ctx; du_mkdir_fn make_dir; void *make_dir_ctx;
    int selected;                            /* node index */
};

static struct folder_state *folder_of(struct du_ui *u) { return u->dialog.state; }

static void node_path(const struct folder_state *s, int n, char *out, size_t size)
{
    int chain[64]; int depth = 0;
    for (int i = n; i > 0 && depth < 64; i = s->nodes[i].parent) chain[depth++] = i;
    snprintf(out, size, "%s", s->root);
    for (int k = depth - 1; k >= 0; k--) {
        size_t len = strlen(out);
        snprintf(out + len, size - len, "%s%s", len && out[len - 1] == '/' ? "" : "/", s->nodes[chain[k]].name);
    }
}

static int folder_load(struct du_ui *u, int n)
{
    struct folder_state *s = folder_of(u);
    if (s->nodes[n].loaded) return 0;
    s->nodes[n].loaded = 1;
    char path[DU_TEXT_MAX]; node_path(s, n, path, sizeof(path));
    struct du_file_entry *list = calloc(DU_FILE_ENTRIES, sizeof(*list));
    if (!list) return -ENOMEM;
    unsigned count = 0; int r = s->lister(s->lister_ctx, path, list, DU_FILE_ENTRIES, &count);
    if (r) { free(list); s->nodes[n].has_children = 0; return r; }
    /* Folders only, name order (case-insensitive) so the tree is deterministic. */
    unsigned dirs[DU_FILE_ENTRIES], nd = 0;
    for (unsigned i = 0; i < count; i++) if (list[i].directory) dirs[nd++] = i;
    for (unsigned i = 1; i < nd; i++) {
        unsigned key = dirs[i], j = i;
        while (j > 0 && ci_cmp(list[dirs[j - 1]].name, list[key].name) > 0) { dirs[j] = dirs[j - 1]; j--; }
        dirs[j] = key;
    }
    /* Children are inserted after the node and its already known descendants are not affected: only
     * unloaded nodes load, so the new nodes are appended and their parent index records the tree. */
    for (unsigned i = 0; i < nd && s->count < NODE_MAX; i++) {
        struct node *c = &s->nodes[s->count++];
        memset(c, 0, sizeof(*c));
        c->parent = n; c->depth = (uint8_t)(s->nodes[n].depth + 1); c->has_children = 1;
        copy_str(c->name, sizeof(c->name), list[dirs[i]].name);
    }
    s->nodes[n].has_children = nd != 0;
    free(list);
    return 0;
}

/* Depth-first rebuild of the visible rows. */
static void folder_rebuild(struct du_ui *u)
{
    struct du_dialog *d = &u->dialog; struct folder_state *s = folder_of(u);
    s->shown = 0;
    /* The children of a node are the nodes whose parent it is, in index order. */
    struct { int node; unsigned cursor; } walk[64]; int depth = 0;
    walk[0].node = 0; walk[0].cursor = 1;
    s->visible[s->shown++] = 0;
    while (depth >= 0) {
        int n = walk[depth].node;
        if (!s->nodes[n].expanded) { depth--; continue; }
        unsigned i = walk[depth].cursor;
        while (i < s->count && s->nodes[i].parent != n) i++;
        if (i >= s->count) { depth--; continue; }
        walk[depth].cursor = i + 1;
        if (s->shown < NODE_MAX) s->visible[s->shown++] = i;
        if (depth + 1 < 64) { depth++; walk[depth].node = (int)i; walk[depth].cursor = 1; }
    }
    int active = 0;
    for (unsigned k = 0; k < s->shown; k++) {
        struct node *n = &s->nodes[s->visible[k]];
        struct du_item *row = &s->rows[k]; memset(row, 0, sizeof(*row));
        row->text[0] = n->name; row->depth = n->depth; row->expanded = n->expanded; row->children = n->has_children;
        row->icon16 = du_icon_argb(du_icon_name(n->expanded ? "folder-open" : "type:folder"), 16);
        row->selected = (int)s->visible[k] == s->selected;
        if (row->selected) active = (int)k;
    }
    struct du_widget *tree = &d->items[BF_TREE];
    tree->items = s->rows; tree->count = s->shown; tree->active = (unsigned)active;
    du_list_ensure(tree, (unsigned)active);
    du_invalidate(u, tree->box);
}

static void folder_select(struct du_ui *u, int node)
{
    struct folder_state *s = folder_of(u);
    s->selected = node;
    du_set_text(&u->dialog.items[BF_STATUS], "");
    folder_rebuild(u);
}

static void folder_toggle(struct du_ui *u, int node)
{
    struct folder_state *s = folder_of(u);
    if (!s->nodes[node].expanded) folder_load(u, node);
    if (!s->nodes[node].has_children) return;
    s->nodes[node].expanded = !s->nodes[node].expanded;
    folder_rebuild(u);
}

static int folder_accept(struct du_ui *u, int button, void *arg)
{
    (void)arg;
    if (button) return 0;
    struct folder_state *s = folder_of(u);
    char path[DU_TEXT_MAX]; node_path(s, s->selected, path, sizeof(path));
    du_dialog_finish(u, 0, path);
    return 1;
}

static int folder_event(struct du_ui *u, const struct du_event *e)
{
    struct folder_state *s = folder_of(u);
    if (e->id == BID_TREE && e->index >= 0 && (unsigned)e->index < s->shown) {
        int node = (int)s->visible[e->index];
        if (e->type == DU_EV_SELECT) { folder_select(u, node); return 1; }
        if (e->type == DU_EV_CHANGE) { s->selected = node; folder_toggle(u, node); return 1; }
        if (e->type == DU_EV_OPEN) { s->selected = node; folder_toggle(u, node); return 1; }
    }
    if (e->type == DU_EV_ACTIVATE && e->id == BID_NEW) {
        char base[DU_TEXT_MAX]; node_path(s, s->selected, base, sizeof(base));
        folder_load(u, s->selected);
        char name[64] = "New Folder";
        for (unsigned n = 2; n < 1000; n++) {
            int clash = 0;
            for (unsigned i = 0; i < s->count; i++) if (s->nodes[i].parent == s->selected && i != (unsigned)s->selected && !ci_cmp(s->nodes[i].name, name)) clash = 1;
            if (!clash) break;
            snprintf(name, sizeof(name), "New Folder (%u)", n);
        }
        char path[DU_TEXT_MAX]; copy_str(path, sizeof(path), base);
        { size_t at = strlen(path); if (at && path[at - 1] != '/' && at + 1 < sizeof(path)) path[at++] = '/', path[at] = 0; copy_str(path + at, sizeof(path) - at, name); }
        int r = s->make_dir(s->make_dir_ctx, path);
        if (r || s->count >= NODE_MAX) { char t[96]; snprintf(t, sizeof(t), "The folder could not be created: %s", strerror(r < 0 ? -r : r ? r : ENOSPC)); du_set_text(&u->dialog.items[BF_STATUS], t); du_invalidate(u, u->dialog.items[BF_STATUS].box); return 1; }
        struct node *c = &s->nodes[s->count++];
        memset(c, 0, sizeof(*c)); c->parent = s->selected; c->depth = (uint8_t)(s->nodes[s->selected].depth + 1); c->loaded = 1;
        snprintf(c->name, sizeof(c->name), "%s", name);
        s->nodes[s->selected].has_children = 1; s->nodes[s->selected].expanded = 1;
        s->selected = (int)s->count - 1;
        folder_rebuild(u);
        return 1;
    }
    return 0;
}

static void folder_draw(struct du_ui *u)
{
    struct du_dialog *d = &u->dialog;
    du_text_wrap(&u->canvas, (struct du_rect){d->box.x + 14, du_dialog_body_y(d) + 12, d->box.w - 28, 34}, d->text, DU_INK);
}

static int fd_posix_mkdir(void *ctx, const char *path)
{ (void)ctx; return mkdir(path, 0777) ? -errno : 0; }

int du_folder_dialog(struct du_ui *u, int id, const struct du_folder_options *o)
{
    du_dialog_begin(u, DU_DLG_FOLDER, id, o->title ? o->title : "Browse For Folder", o->prompt ? o->prompt : "Select a folder:");
    struct du_dialog *d = &u->dialog;
    struct folder_state *s = calloc(1, sizeof(*s));
    if (!s) { du_dialog_close(u); return -ENOMEM; }
    s->lister = o->lister ? o->lister : du_posix_lister; s->lister_ctx = o->lister_ctx;
    s->make_dir = o->make_dir ? o->make_dir : fd_posix_mkdir; s->make_dir_ctx = o->make_dir_ctx;
    snprintf(s->root, sizeof(s->root), "%s", o->root && o->root[0] ? o->root : "/");
    while (strlen(s->root) > 1 && s->root[strlen(s->root) - 1] == '/') s->root[strlen(s->root) - 1] = 0;
    struct node *root = &s->nodes[s->count++];
    copy_str(root->name, sizeof(root->name), !strcmp(s->root, "/") ? "CiukiOS (C:)" : strrchr(s->root, '/') ? strrchr(s->root, '/') + 1 : s->root);
    root->has_children = 1;
    d->state = s; d->free_state = free;
    d->buttons = 2; d->cancel = 1; d->focus = BF_TREE; d->accept = folder_accept; d->on_event = folder_event; d->on_draw = folder_draw;
    du_dialog_place(u, 380, 360);
    struct du_rect b = d->box; int y = du_dialog_body_y(d) + 10;
    int row = b.y + b.h - 3 - 10 - BTN_H;
    du_dialog_item(u, BF_OK, DU_BUTTON, CD_BUTTON, "OK")->box = (struct du_rect){b.x + b.w - 14 - 2 * BTN_W - 6, row, BTN_W, BTN_H};
    du_dialog_item(u, BF_CANCEL, DU_BUTTON, CD_BUTTON - 1, "Cancel")->box = (struct du_rect){b.x + b.w - 14 - BTN_W, row, BTN_W, BTN_H};
    struct du_widget *tree = du_dialog_item(u, BF_TREE, DU_TREE, BID_TREE, "");
    tree->box = (struct du_rect){b.x + 14, y + 36, b.w - 28, row - 34 - (y + 36)}; tree->inactive_selection = 1;
    struct du_widget *mk = du_dialog_item(u, BF_NEW, DU_BUTTON, BID_NEW, "&Make New Folder");
    mk->box = (struct du_rect){b.x + 14, row, 120, BTN_H};
    mk->hidden = (o->flags & DU_FILE_NO_NEW_FOLDER) != 0;
    du_dialog_item(u, BF_STATUS, DU_LABEL, 0, "")->box = (struct du_rect){b.x + 14, row - 26, b.w - 28, 16};
    /* Open the chain down to the initial folder, then select it. */
    s->nodes[0].expanded = 1; folder_load(u, 0); s->selected = 0;
    if (o->initial && !strncmp(o->initial, s->root, strlen(s->root))) {
        const char *rest = o->initial + strlen(s->root);
        int at = 0;
        while (*rest) {
            while (*rest == '/') rest++;
            if (!*rest) break;
            char part[72]; size_t n = 0; while (rest[n] && rest[n] != '/' && n < sizeof(part) - 1) { part[n] = rest[n]; n++; }
            part[n] = 0; rest += n;
            folder_load(u, at);
            int hit = -1;
            for (unsigned i = 0; i < s->count; i++) if (s->nodes[i].parent == at && i != (unsigned)at && !ci_cmp(s->nodes[i].name, part)) { hit = (int)i; break; }
            if (hit < 0) break;
            s->nodes[at].expanded = 1; at = hit; s->selected = hit;
        }
    }
    folder_rebuild(u);
    return 0;
}
