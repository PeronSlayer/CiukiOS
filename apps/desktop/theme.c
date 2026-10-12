/* SPDX-License-Identifier: MIT
 * Themes, settings file, anti-aliased text and integer paint helpers. */
#include "theme.h"
#include <stdio.h>
#include <string.h>
#include "ui_fonts.h"

/* Built-in themes. Platinum is the default: platinum greys with a blue
 * accent, pinstriped active title bars, gradient bevels, rounded window tops
 * and a soft shadow. Ciuki Blue: gradient title bars in Ciuki's slate blue
 * with the portrait's tan as accent. Classic 98: flat 3-D greys, navy
 * gradient captions and aliased text. Frame geometry (border 3, title 24)
 * is the same in every theme, so hit areas never move with the theme. */
static const struct desk_theme themes[] = {
    {
        .name = "Platinum", .description = "Platinum greys, pinstriped title bars and a blue accent",
        .desktop = 0x375564u, .desktop_label = 0xf6dfb6u,
        .topbar = 0xdcdcdcu, .topbar_edge = 0x8a8a8au, .topbar_text = 0x161616u, .topbar_accent = 0x31639cu,
        .taskbar = 0xd6d6d6u, .taskbar_text = 0x161616u, .start = 0xf2f2f2u, .start_open = 0xb9b9b9u,
        .start_text = 0x161616u, .task_active = 0xc4c4c4u, .task_active_text = 0x000000u,
        .face = 0xddddddu, .light = 0xeeeeeeu, .hilite = 0xffffffu, .shadow = 0x9a9a9au, .dark = 0x666666u,
        .outline = 0x3a3a3au, .face_top = 0xf7f7f7u, .face_bottom = 0xd2d2d2u, .hover = 0xe6eef8u,
        .window = 0xffffffu, .ink = 0x111111u, .gray = 0x8a8a8au, .select = 0x31639cu, .select_ink = 0xffffffu,
        .accent = 0x31639cu, .focus = 0x7c9ccbu,
        .frame = 0x2a2a2au, .frame_inactive = 0x7a7a7au,
        .title = {0xeeeeeeu, 0xd4d4d4u}, .title_inactive = {0xe6e6e6u, 0xe6e6e6u},
        .title_text = 0x111111u, .title_text_inactive = 0x8a8a8au,
        .stripe_light = 0xffffffu, .stripe_dark = 0x9c9c9cu, .drop_shadow = 0x000000u,
        .title_style = DESK_TITLE_PINSTRIPE, .title_center = 1, .control_style = DESK_CONTROLS_PLATINUM,
        .focus_style = DESK_FOCUS_RING, .text_aa = 1, .radius = 5, .shadow_size = 6, .button = 13,
        .icon_small = 16, .icon_toolbar = 22, .icon_large = 32,
        .font_ui = DESK_FONT_UI, .font_bold = DESK_FONT_BOLD, .font_title = DESK_FONT_TITLE,
        .font_mono = DESK_FONT_MONO, .font_mono_bold = DESK_FONT_MONO_BOLD, .font_large = DESK_FONT_LARGE,
    },
    {
        .name = "Ciuki Blue", .description = "Slate-blue gradient captions with Ciuki's tan accent",
        .desktop = 0x2b4a63u, .desktop_label = 0xf6dfb6u,
        .topbar = 0x203a50u, .topbar_edge = 0x0f1e2au, .topbar_text = 0xf2f2f2u, .topbar_accent = 0xf0c27au,
        .taskbar = 0xd4dce4u, .taskbar_text = 0x101a24u, .start = 0xecbf7cu, .start_open = 0xc99a52u,
        .start_text = 0x2a1a08u, .task_active = 0xb9c6d3u, .task_active_text = 0x000000u,
        .face = 0xd4dce4u, .light = 0xe6ecf2u, .hilite = 0xffffffu, .shadow = 0x8394a6u, .dark = 0x3f4f60u,
        .outline = 0x2a3846u, .face_top = 0xf2f5f8u, .face_bottom = 0xc6d0dbu, .hover = 0xfbf0dcu,
        .window = 0xffffffu, .ink = 0x101a24u, .gray = 0x8394a6u, .select = 0x2a5f8fu, .select_ink = 0xffffffu,
        .accent = 0xe0a24au, .focus = 0xe0a24au,
        .frame = 0x1f2a36u, .frame_inactive = 0x6d7c8cu,
        .title = {0x163652u, 0x5b97c6u}, .title_inactive = {0x7d8b99u, 0xb8c3ceu},
        .title_text = 0xffffffu, .title_text_inactive = 0xeef1f4u,
        .stripe_light = 0xffffffu, .stripe_dark = 0x8394a6u, .drop_shadow = 0x000000u,
        .title_style = DESK_TITLE_GRADIENT, .title_center = 0, .control_style = DESK_CONTROLS_PLATINUM,
        .focus_style = DESK_FOCUS_RING, .text_aa = 1, .radius = 4, .shadow_size = 5, .button = 15,
        .icon_small = 16, .icon_toolbar = 22, .icon_large = 32,
        .font_ui = DESK_FONT_UI, .font_bold = DESK_FONT_BOLD, .font_title = DESK_FONT_TITLE,
        .font_mono = DESK_FONT_MONO, .font_mono_bold = DESK_FONT_MONO_BOLD, .font_large = DESK_FONT_LARGE,
    },
    {
        .name = "Classic 98", .description = "Flat 3-D greys, navy captions and crisp aliased text",
        .desktop = 0x008080u, .desktop_label = 0xffffffu,
        .topbar = 0xc0c0c0u, .topbar_edge = 0x808080u, .topbar_text = 0x000000u, .topbar_accent = 0x000080u,
        .taskbar = 0xc0c0c0u, .taskbar_text = 0x000000u, .start = 0xc0c0c0u, .start_open = 0xb4b4b4u,
        .start_text = 0x000000u, .task_active = 0xe0e0e0u, .task_active_text = 0x000000u,
        .face = 0xc0c0c0u, .light = 0xdfdfdfu, .hilite = 0xffffffu, .shadow = 0x808080u, .dark = 0x000000u,
        .outline = 0x000000u, .face_top = 0xc0c0c0u, .face_bottom = 0xc0c0c0u, .hover = 0xc0c0c0u,
        .window = 0xffffffu, .ink = 0x000000u, .gray = 0x808080u, .select = 0x000080u, .select_ink = 0xffffffu,
        .accent = 0x000080u, .focus = 0x000000u,
        .frame = 0x000000u, .frame_inactive = 0x000000u,
        .title = {0x000080u, 0x1084d0u}, .title_inactive = {0x808080u, 0xc0c0c0u},
        .title_text = 0xffffffu, .title_text_inactive = 0xc0c0c0u,
        .stripe_light = 0xffffffu, .stripe_dark = 0x808080u, .drop_shadow = 0x000000u,
        .title_style = DESK_TITLE_GRADIENT, .title_center = 0, .control_style = DESK_CONTROLS_CLASSIC,
        .focus_style = DESK_FOCUS_DOTTED, .text_aa = 0, .radius = 0, .shadow_size = 0, .button = 16,
        .icon_small = 16, .icon_toolbar = 22, .icon_large = 32,
        .font_ui = DESK_FONT_UI, .font_bold = DESK_FONT_BOLD, .font_title = DESK_FONT_BOLD,
        .font_mono = DESK_FONT_MONO, .font_mono_bold = DESK_FONT_MONO_BOLD, .font_large = DESK_FONT_LARGE,
    },
};
#define THEMES (int)(sizeof(themes) / sizeof(themes[0]))
const struct desk_theme *desk_theme = &themes[0];

int desk_theme_count(void) { return THEMES; }
const struct desk_theme *desk_theme_at(int i) { return i >= 0 && i < THEMES ? &themes[i] : NULL; }

static int same_name(const char *a, const char *b)
{
    for (;; a++, b++) {
        unsigned x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

const struct desk_theme *desk_theme_named(const char *name)
{
    for (int i = 0; name && i < THEMES; i++) if (same_name(themes[i].name, name)) return &themes[i];
    return NULL;
}

int desk_theme_use(const char *name)
{
    const struct desk_theme *t = desk_theme_named(name);
    if (!t) return -1;
    desk_theme = t;
    return 0;
}

/* ---- fonts ---- */
const struct desk_font *desk_font(int id) { return &desk_fonts[id >= 0 && id < DESK_FONTS ? id : DESK_FONT_UI]; }

static int glyph_index(unsigned c)
{
    if (c >= 0x20 && c < 0x7f) return (int)c - 0x20;
    if (c >= 0xa0 && c <= 0xff) return 95 + (int)c - 0xa0;
    if (c == 0x2022) return 191;
    if (c == 0x2026) return 192;
    return '?' - 0x20;
}

int desk_font_cap(int id) { return -desk_font(id)->glyphs['H' - 0x20].top; }
int desk_font_line(int id) { return desk_font(id)->ascent + desk_font(id)->descent; }
int desk_text_advance(int font, unsigned c) { return desk_font(font)->glyphs[glyph_index(c)].advance; }

unsigned desk_utf8_decode(const char *s, size_t *used)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned c = p[0], n = c < 0x80 ? 0 : c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (c >= 0x80 && c < 0xc0) { *used = 1; return '?'; }
    c = n == 0 ? c : n == 1 ? c & 0x1f : n == 2 ? c & 0x0f : c & 0x07;
    for (unsigned i = 1; i <= n; i++) {
        if ((p[i] & 0xc0) != 0x80) { *used = i; return '?'; }
        c = (c << 6) | (p[i] & 0x3f);
    }
    *used = n + 1;
    return c;
}

int desk_text_measure_n(int font, const char *s, size_t chars)
{
    int w = 0;
    for (size_t n = 0; *s && n < chars; n++) {
        size_t used; unsigned c = desk_utf8_decode(s, &used);
        w += desk_text_advance(font, c); s += used;
    }
    return w;
}

int desk_text_measure(int font, const char *s) { return desk_text_measure_n(font, s, (size_t)-1); }

size_t desk_text_fit_chars(int font, const char *s, int width)
{
    size_t n = 0; int w = 0;
    while (*s) {
        size_t used; unsigned c = desk_utf8_decode(s, &used);
        int a = desk_text_advance(font, c);
        if (w + a > width) break;
        w += a; s += used; n++;
    }
    return n;
}

/* ---- paint ---- */
uint32_t desk_mix(uint32_t a, uint32_t b, unsigned alpha)
{
    if (alpha >= 255) return a & 0xffffffu;
    if (!alpha) return b & 0xffffffu;
    uint32_t out = 0;
    for (unsigned shift = 0; shift < 24; shift += 8)
        out |= ((((a >> shift) & 255) * alpha + ((b >> shift) & 255) * (255 - alpha) + 127) / 255) << shift;
    return out;
}

static int clip(const struct desk_paint *p, int *x, int *y, int *w, int *h)
{
    int64_t x0 = *x, y0 = *y, x1 = (int64_t)*x + *w, y1 = (int64_t)*y + *h;
    int64_t cx0 = p->cx > 0 ? p->cx : 0, cy0 = p->cy > 0 ? p->cy : 0;
    int64_t cx1 = (int64_t)p->cx + p->cw, cy1 = (int64_t)p->cy + p->ch;
    if (cx1 > p->width) cx1 = p->width;
    if (cy1 > p->height) cy1 = p->height;
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    if (*w <= 0 || *h <= 0 || x1 <= x0 || y1 <= y0) return 0;
    *x = (int)x0; *y = (int)y0; *w = (int)(x1 - x0); *h = (int)(y1 - y0);
    return 1;
}

void desk_paint_fill(struct desk_paint *p, int x, int y, int w, int h, uint32_t colour)
{
    if (!clip(p, &x, &y, &w, &h)) return;
    for (int r = y; r < y + h; r++) {
        uint32_t *row = p->pixels + (size_t)r * (size_t)p->width + (size_t)x;
        for (int i = 0; i < w; i++) row[i] = colour;
    }
}

void desk_paint_blend(struct desk_paint *p, int x, int y, int w, int h, uint32_t colour, unsigned alpha)
{
    if (!clip(p, &x, &y, &w, &h)) return;
    for (int r = y; r < y + h; r++) {
        uint32_t *row = p->pixels + (size_t)r * (size_t)p->width + (size_t)x;
        for (int i = 0; i < w; i++) row[i] = desk_mix(colour, row[i], alpha);
    }
}

void desk_paint_vgradient(struct desk_paint *p, int x, int y, int w, int h, uint32_t top, uint32_t bottom)
{
    for (int r = 0; r < h; r++)
        desk_paint_fill(p, x, y + r, w, 1, desk_mix(bottom, top, h > 1 ? (unsigned)(r * 255 / (h - 1)) : 0));
}

void desk_paint_hgradient(struct desk_paint *p, int x, int y, int w, int h, uint32_t left, uint32_t right)
{
    int x0 = x, y0 = y, w0 = w, h0 = h;
    if (!clip(p, &x0, &y0, &w0, &h0)) return;
    for (int i = x0; i < x0 + w0; i++)
        desk_paint_fill(p, i, y0, 1, h0, desk_mix(right, left, w > 1 ? (unsigned)((i - x) * 255 / (w - 1)) : 0));
}

void desk_paint_shadow(struct desk_paint *p, int x, int y, int w, int h, int size, uint32_t colour)
{
    /* Soft drop shadow of the box offset by (size/3, size/2), falling off
     * linearly over `size` pixels; the box itself is left to its owner. */
    if (size <= 0) return;
    int ox = x + size / 3, oy = y + size / 2;
    int bx = ox - size, by = oy - size, bw = w + 2 * size, bh = h + 2 * size;
    int vx = bx, vy = by, vw = bw, vh = bh;
    if (!clip(p, &vx, &vy, &vw, &vh)) return;
    unsigned peak = 72;
    for (int r = vy; r < vy + vh; r++) for (int c = vx; c < vx + vw; c++) {
        if (c >= x && c < x + w && r >= y && r < y + h) continue;
        int dx = c < ox ? ox - c : c >= ox + w ? c - (ox + w - 1) : 0;
        int dy = r < oy ? oy - r : r >= oy + h ? r - (oy + h - 1) : 0;
        if (dx >= size || dy >= size) continue;
        unsigned a = peak * (unsigned)(size - dx) * (unsigned)(size - dy) / (unsigned)(size * size);
        a = a * a / peak;                       /* ease out: softer outer edge */
        uint32_t *px = p->pixels + (size_t)r * (size_t)p->width + (size_t)c;
        *px = desk_mix(colour, *px, a);
    }
}

int desk_paint_text_n(struct desk_paint *p, int font, int x, int baseline, const char *s, size_t chars, uint32_t colour)
{
    const struct desk_font *f = desk_font(font);
    int start = x, aa = desk_theme->text_aa;
    for (size_t n = 0; *s && n < chars; n++) {
        size_t used; unsigned c = desk_utf8_decode(s, &used);
        s += used;
        const struct desk_glyph *g = &f->glyphs[glyph_index(c)];
        int gx = x + g->left, gy = baseline + g->top, w = g->w, h = g->h;
        int vx = gx, vy = gy, vw = w, vh = h;
        if (w && h && x < p->cx + p->cw && clip(p, &vx, &vy, &vw, &vh)) {
            for (int r = vy; r < vy + vh; r++) {
                const uint8_t *src = f->alpha + g->offset + (size_t)(r - gy) * (size_t)w + (size_t)(vx - gx);
                uint32_t *dst = p->pixels + (size_t)r * (size_t)p->width + (size_t)vx;
                for (int i = 0; i < vw; i++) {
                    unsigned a = src[i];
                    if (!aa) a = a >= 104 ? 255 : 0;
                    if (a) dst[i] = desk_mix(colour, dst[i], a);
                }
            }
        }
        x += g->advance;
        if (x > p->cx + p->cw + 64) break;
    }
    return x - start;
}

int desk_paint_text(struct desk_paint *p, int font, int x, int baseline, const char *s, uint32_t colour)
{ return desk_paint_text_n(p, font, x, baseline, s, (size_t)-1, colour); }

void desk_paint_pinstripes(struct desk_paint *p, int x, int y, int w, int h, uint32_t light, uint32_t dark)
{
    /* Platinum: pairs of one dark and one light line on every other row. */
    for (int r = 0; r + 1 < h; r += 2) {
        desk_paint_fill(p, x, y + r, w, 1, dark);
        desk_paint_fill(p, x + 1, y + r + 1, w - 1, 1, light);
    }
}

static void rounded_outline(struct desk_paint *p, int x, int y, int w, int h, uint32_t c)
{
    desk_paint_fill(p, x + 1, y, w - 2, 1, c); desk_paint_fill(p, x + 1, y + h - 1, w - 2, 1, c);
    desk_paint_fill(p, x, y + 1, 1, h - 2, c); desk_paint_fill(p, x + w - 1, y + 1, 1, h - 2, c);
}

void desk_paint_raised(struct desk_paint *p, int x, int y, int w, int h, int pressed, int hot)
{
    const struct desk_theme *t = desk_theme;
    if (w <= 0 || h <= 0) return;
    if (t->control_style == DESK_CONTROLS_CLASSIC) {
        desk_paint_fill(p, x, y, w, h, t->face);
        if (pressed) {
            desk_paint_fill(p, x, y, w, 1, t->dark); desk_paint_fill(p, x, y, 1, h, t->dark);
            desk_paint_fill(p, x, y + h - 1, w, 1, t->dark); desk_paint_fill(p, x + w - 1, y, 1, h, t->dark);
            desk_paint_fill(p, x + 1, y + 1, w - 2, 1, t->shadow); desk_paint_fill(p, x + 1, y + 1, 1, h - 2, t->shadow);
            return;
        }
        desk_paint_fill(p, x, y, w - 1, 1, t->hilite); desk_paint_fill(p, x, y, 1, h - 1, t->hilite);
        desk_paint_fill(p, x, y + h - 1, w, 1, t->dark); desk_paint_fill(p, x + w - 1, y, 1, h, t->dark);
        desk_paint_fill(p, x + 1, y + 1, w - 3, 1, t->light); desk_paint_fill(p, x + 1, y + 1, 1, h - 3, t->light);
        desk_paint_fill(p, x + 1, y + h - 2, w - 2, 1, t->shadow); desk_paint_fill(p, x + w - 2, y + 1, 1, h - 2, t->shadow);
        return;
    }
    /* Platinum: rounded outline, vertical gradient, inner light and shade. */
    uint32_t top = pressed ? t->shadow : hot ? desk_mix(t->hover, t->face_top, 160) : t->face_top;
    uint32_t bottom = pressed ? t->face : hot ? desk_mix(t->hover, t->face_bottom, 110) : t->face_bottom;
    desk_paint_vgradient(p, x + 1, y + 1, w - 2, h - 2, top, bottom);
    rounded_outline(p, x, y, w, h, t->outline);
    if (w > 4 && h > 4) {
        if (!pressed) {
            desk_paint_fill(p, x + 1, y + 1, w - 3, 1, t->hilite); desk_paint_fill(p, x + 1, y + 1, 1, h - 3, t->hilite);
            desk_paint_fill(p, x + 2, y + h - 2, w - 3, 1, desk_mix(t->shadow, bottom, 150));
            desk_paint_fill(p, x + w - 2, y + 2, 1, h - 3, desk_mix(t->shadow, bottom, 150));
        } else {
            desk_paint_fill(p, x + 1, y + 1, w - 2, 1, desk_mix(t->dark, top, 120));
            desk_paint_fill(p, x + 1, y + 1, 1, h - 2, desk_mix(t->dark, top, 120));
        }
    }
}

void desk_paint_title_button(struct desk_paint *p, int x, int y, int s, int kind, int active, int pressed, int disabled)
{
    const struct desk_theme *t = desk_theme;
    uint32_t ink = disabled ? t->gray : t->ink;
    if (t->title_style == DESK_TITLE_PINSTRIPE) {
        /* Platinum: a square well with a bevelled face; plain when inactive. */
        if (!active) { rounded_outline(p, x, y, s, s, t->shadow); return; }
        desk_paint_fill(p, x, y, s, s, t->dark);
        desk_paint_fill(p, x, y, s - 1, 1, t->shadow); desk_paint_fill(p, x, y, 1, s - 1, t->shadow);
        desk_paint_fill(p, x + 1, y + s - 1, s - 1, 1, t->hilite); desk_paint_fill(p, x + s - 1, y + 1, 1, s - 1, t->hilite);
        if (pressed) desk_paint_vgradient(p, x + 2, y + 2, s - 4, s - 4, t->shadow, t->face);
        else {
            desk_paint_vgradient(p, x + 2, y + 2, s - 4, s - 4, t->hilite, t->face_bottom);
            desk_paint_fill(p, x + 2, y + s - 3, s - 4, 1, desk_mix(t->shadow, t->face_bottom, 160));
            desk_paint_fill(p, x + s - 3, y + 2, 1, s - 4, desk_mix(t->shadow, t->face_bottom, 160));
        }
        int c = s / 2, m = s / 4 + 1;
        if (kind == DESK_GLYPH_CLOSE) {
            for (int i = 0; i <= s - 2 * m - 1; i++) {
                desk_paint_fill(p, x + m + i, y + m + i, 1, 1, ink);
                desk_paint_fill(p, x + s - 1 - m - i, y + m + i, 1, 1, ink);
            }
        } else if (kind == DESK_GLYPH_MINIMIZE) desk_paint_fill(p, x + m, y + s - m - 1, s - 2 * m, 1, ink);
        else if (kind == DESK_GLYPH_COLLAPSE) {
            desk_paint_fill(p, x + m, y + c - 2, s - 2 * m, 1, ink); desk_paint_fill(p, x + m, y + c + 1, s - 2 * m, 1, ink);
        } else {
            desk_paint_fill(p, x + m, y + m, s - 2 * m, 1, ink); desk_paint_fill(p, x + m, y + s - m - 1, s - 2 * m, 1, ink);
            desk_paint_fill(p, x + m, y + m, 1, s - 2 * m, ink); desk_paint_fill(p, x + s - m - 1, y + m, 1, s - 2 * m, ink);
        }
        return;
    }
    int w = s + 1, h = s - 1;   /* Windows-class caption buttons are slightly wider than tall */
    desk_paint_raised(p, x, y, w, h, pressed, 0);
    int ox = x + pressed + (w - 8) / 2, oy = y + pressed + (h - 7) / 2;
    if (kind == DESK_GLYPH_CLOSE) {
        for (int i = 0; i < 7; i++) {
            desk_paint_fill(p, ox + i, oy + i, 2, 1, ink); desk_paint_fill(p, ox + 6 - i, oy + i, 2, 1, ink);
        }
    } else if (kind == DESK_GLYPH_MINIMIZE) desk_paint_fill(p, ox + 1, oy + 5, 6, 2, ink);
    else if (kind == DESK_GLYPH_COLLAPSE) {
        for (int i = 0; i < 4; i++) desk_paint_fill(p, ox + 4 - i, oy + 1 + i, 1 + 2 * i, 1, ink);
        desk_paint_fill(p, ox + 1, oy + 6, 7, 1, ink);
    } else {
        desk_paint_fill(p, ox, oy, 8, 2, ink); desk_paint_fill(p, ox, oy, 1, 7, ink);
        desk_paint_fill(p, ox + 7, oy, 1, 7, ink); desk_paint_fill(p, ox, oy + 6, 8, 1, ink);
    }
}

/* ---- settings ----
 * desktop.cfg has one writer, settings.c (cfg_format: the shell and the
 * Control Panel). The theme engine only reads its "theme" key, with the line
 * rules of cfg_parse: blank lines and '#' comments are skipped, spaces round
 * key and value are trimmed, the last valid line wins, and a value that is
 * not a theme name is ignored (cfg_parse keeps the default for it). */
static size_t span(const char *s, size_t a, size_t b, size_t *end)
{
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
    *end = b;
    return a;
}

int desk_settings_theme(const char *text, size_t length, char *name, size_t size)
{
    int found = 0;
    for (size_t at = 0; at < length;) {
        size_t stop = at, k0, k1, v0, v1;
        while (stop < length && text[stop] != '\n') stop++;
        size_t line = at;
        at = stop + 1;
        k0 = span(text, line, stop, &k1);
        if (k0 == k1 || text[k0] == '#') continue;
        size_t eq = k0;
        while (eq < k1 && text[eq] != '=') eq++;
        if (eq == k1) continue;
        k0 = span(text, k0, eq, &k1);
        v0 = span(text, eq + 1, stop, &v1);
        if (k1 - k0 != 5 || memcmp(text + k0, "theme", 5) || v1 - v0 >= size || v1 - v0 >= 64) continue;
        char value[64];
        memcpy(value, text + v0, v1 - v0); value[v1 - v0] = 0;
        int known = 0;
        for (int i = 0; i < THEMES; i++) known |= !strcmp(themes[i].name, value);   /* exact, as cfg_theme_index */
        if (!known) continue;
        snprintf(name, size, "%s", value);
        found = 1;
    }
    return found ? 0 : -1;
}

int desk_theme_poll(const char *path, uint64_t now)
{
    /* The file is small (CFG_TEXT_MAX); re-read it every two seconds and
     * compare the theme name, so a same-size rewrite within one second is
     * still seen. Missing file or key: the default theme. */
    static uint64_t next; static int started;
    static char last[32] = "\001";
    static char text[4096];
    if (started && now < next) return 0;
    started = 1; next = now + 2000000000ull;
    char name[32] = "";
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t n = fread(text, 1, sizeof(text), f);
        if (!ferror(f) && desk_settings_theme(text, n, name, sizeof(name))) name[0] = 0;
        fclose(f);
    }
    if (!strcmp(name, last)) return 0;
    snprintf(last, sizeof(last), "%s", name);
    const struct desk_theme *before = desk_theme;
    if (!name[0] || desk_theme_use(name)) desk_theme = &themes[0];
    return desk_theme != before;
}
