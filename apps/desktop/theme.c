/* SPDX-License-Identifier: MIT
 * Themes, settings file, anti-aliased text and integer paint helpers. */
#include "theme.h"
#include <stdio.h>
#include <string.h>
#include "ui_fonts.h"

/* Built-in themes. Ciuki Blue is the default. Platinum: platinum greys with a blue
 * accent, pinstriped active title bars over a soft vertical gradient,
 * gradient bevels, rounded window tops and a soft shadow. Ciuki Blue:
 * horizontal slate-blue title bars with raised buttons and the portrait's tan
 * as accent. title[0]/title[1] are the caption's top and bottom colours
 * (Blue: left and right). Frame geometry (border 3, title 24, title-button hit
 * boxes) is the same in every theme, so hit areas never move with the theme. */
static const struct desk_theme themes[] = {
    {
        .name = "Platinum", .description = "Platinum greys, pinstriped title bars and a blue accent",
        .desktop = 0x375564u, .desktop_label = 0xf6dfb6u,
        .topbar = 0xdcdcdcu, .topbar_edge = 0x8a8a8au, .topbar_text = 0x161616u, .topbar_accent = 0x31639cu,
        .taskbar = 0xbfbfbfu, .taskbar_text = 0x161616u, .start = 0xf2f2f2u, .start_open = 0xb9b9b9u,
        .start_text = 0x161616u, .task_active = 0xc4c4c4u, .task_active_text = 0x000000u,
        .face = 0xddddddu, .light = 0xeeeeeeu, .hilite = 0xffffffu, .shadow = 0x9a9a9au, .dark = 0x666666u,
        .outline = 0x3a3a3au, .face_top = 0xf7f7f7u, .face_bottom = 0xd2d2d2u, .hover = 0xe6eef8u,
        .window = 0xffffffu, .ink = 0x111111u, .gray = 0x8a8a8au, .select = 0x31639cu, .select_ink = 0xffffffu,
        .accent = 0x31639cu, .focus = 0x7c9ccbu,
        .frame = 0x2a2a2au, .frame_inactive = 0x7a7a7au,
        .title = {0xf5f5f5u, 0xd3d3d3u}, .title_inactive = {0xf3f3f3u, 0xe4e4e4u},
        .title_text = 0x111111u, .title_text_inactive = 0x8a8a8au,
        .stripe_light = 0xffffffu, .stripe_dark = 0x9c9c9cu, .drop_shadow = 0x000000u,
        .title_style = DESK_TITLE_PINSTRIPE, .title_center = 1, .control_style = DESK_CONTROLS_PLATINUM,
        .focus_style = DESK_FOCUS_RING, .text_aa = 1, .radius = 5, .shadow_size = 6, .button = 15,
        .icon_small = 16, .icon_toolbar = 22, .icon_large = 32,
        .font_ui = DESK_FONT_UI, .font_bold = DESK_FONT_BOLD, .font_title = DESK_FONT_TITLE,
        .font_mono = DESK_FONT_MONO, .font_mono_bold = DESK_FONT_MONO_BOLD, .font_large = DESK_FONT_LARGE,
    },
    {
        .name = "Ciuki Blue", .description = "Blue gradient captions, cool bevels and Ciuki's tan accent",
        .desktop = 0x2b4a63u, .desktop_label = 0xf6dfb6u,
        .topbar = 0x203a50u, .topbar_edge = 0x0f1e2au, .topbar_text = 0xf5f8fbu, .topbar_accent = 0xf0c27au,
        .taskbar = 0xb8c9d9u, .taskbar_text = 0x101a24u, .start = 0xe5c189u, .start_open = 0xd6ae6cu,
        .start_text = 0x2a1a08u, .task_active = 0xb8cbddu, .task_active_text = 0x101a24u,
        .face = 0xdce4ecu, .light = 0xedf2f7u, .hilite = 0xffffffu, .shadow = 0x8193a5u, .dark = 0x41566bu,
        .outline = 0x2b3e50u, .face_top = 0xf5f8fbu, .face_bottom = 0xcbd7e3u, .hover = 0xe3eef8u,
        .window = 0xffffffu, .ink = 0x101a24u, .gray = 0x526475u, .select = 0x245780u, .select_ink = 0xffffffu,
        .accent = 0xa96f24u, .focus = 0x37658du,
        .frame = 0x24384du, .frame_inactive = 0x65798cu,
        .title = {0x173f70u, 0x3f719du}, .title_inactive = {0x8c9eafu, 0xc3ced8u},
        .title_text = 0xffffffu, .title_text_inactive = 0x233443u,
        .stripe_light = 0xffffffu, .stripe_dark = 0x8394a6u, .drop_shadow = 0x000000u,
        .title_style = DESK_TITLE_HGRADIENT, .title_center = 0, .control_style = DESK_CONTROLS_PLATINUM,
        .focus_style = DESK_FOCUS_RING, .text_aa = 1, .radius = 4, .shadow_size = 5, .button = 15,
        .icon_small = 16, .icon_toolbar = 22, .icon_large = 32,
        .font_ui = DESK_FONT_UI, .font_bold = DESK_FONT_BOLD, .font_title = DESK_FONT_TITLE,
        .font_mono = DESK_FONT_MONO, .font_mono_bold = DESK_FONT_MONO_BOLD, .font_large = DESK_FONT_LARGE,
    },
};
#define THEMES (int)(sizeof(themes) / sizeof(themes[0]))
const struct desk_theme *desk_theme = &themes[1];

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

const char *desk_theme_preference(const char *name)
{
    static int logged;
    if (!strcmp(name, "Classic 98")) {
        if (!logged) {
            fprintf(stderr, "desktop: saved theme Classic 98 removed; using Ciuki Blue\n");
            logged = 1;
        }
        return DESK_THEME_DEFAULT;
    }
    for (int i = 0; i < THEMES; i++) if (!strcmp(themes[i].name, name)) return themes[i].name;
    return NULL;
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

static uint32_t surface_pixel(uint32_t colour, int x, int y, int rgb565)
{
    if (!rgb565) return colour;
    static const uint8_t bayer[4][4] = {{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
    unsigned threshold = 2 * bayer[(unsigned)y & 3][(unsigned)x & 3] + 1;
    uint32_t out = 0;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        unsigned bits = shift == 8 ? 2 : 3, step = 1u << bits;
        unsigned v = (colour >> shift) & 255, level = v >> bits;
        /* Preserve the kernel's truncating RGB565 levels; round the
         * fractional remainder spatially, saturating at the top level. */
        if ((v & (step - 1)) * 32 > threshold * step && level < (255u >> bits)) level++;
        out |= (level << bits) << shift;
    }
    return out;
}

void desk_paint_bar(struct desk_paint *p, int x, int y, int w, int h, uint32_t base, int rgb565)
{
    int vx=x, vy=y, vw=w, vh=h;
    if (!clip(p,&vx,&vy,&vw,&vh)) return;
    uint32_t top=desk_mix(0xffffffu,base,20), bottom=desk_mix(0,base,20);
    for (int yy=vy; yy<vy+vh; yy++) {
        int row=yy-y;
        uint32_t face=desk_mix(bottom,top,h>1 ? (unsigned)(row*255/(h-1)) : 0);
        if (!row) face=desk_mix(0xffffffu,face,28);
        else if (row==h-1) face=desk_mix(0,face,28);
        else if (row<=2) face=desk_mix(0,face,row==1 ? 8 : 4);
        uint32_t *dst=p->pixels+(size_t)yy*p->width;
        for (int xx=vx; xx<vx+vw; xx++) dst[xx]=surface_pixel(face,xx,yy,rgb565);
    }
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
        /* The opaque window covers this entire span. Skip it once instead
         * of visiting every client pixel on each drag/partial composite. */
        if (c >= x && c < x + w && r >= y && r < y + h) { c=x+w-1; continue; }
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
    return desk_paint_text_n_aa(p,font,x,baseline,s,chars,colour,desk_theme->text_aa);
}
int desk_paint_text_n_aa(struct desk_paint *p, int font, int x, int baseline, const char *s, size_t chars, uint32_t colour, int aa)
{
    const struct desk_font *f = desk_font(font);
    int start = x;
    for (size_t n = 0; *s && n < chars; n++) {
        size_t used; unsigned c = desk_utf8_decode(s, &used);
        s += used;
        const struct desk_glyph *g = &f->glyphs[glyph_index(c)];
        int gx = x + g->left, gy = baseline + g->top, w = g->w, h = g->h;
        int vx = gx, vy = gy, vw = w, vh = h;
        /* fx-15: the glyph's own box decides, not the pen: a negative left
         * bearing ('j', 'p') reaches left of the pen, and a clip ending
         * between the box and the pen must still receive those pixels, or
         * a repainted strip differs from a full repaint. */
        if (w && h && gx < p->cx + p->cw && clip(p, &vx, &vy, &vw, &vh)) {
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

int desk_paint_text_ellipsis(struct desk_paint *p, int font, int x, int baseline,
                             const char *s, int width, uint32_t colour)
{
    if (width<=0) return 0;
    struct desk_paint clipped=*p;
    int vx=x, vy=p->cy, vw=width, vh=p->ch;
    if (!clip(p,&vx,&vy,&vw,&vh)) return 0;
    clipped.cx=vx; clipped.cy=vy; clipped.cw=vw; clipped.ch=vh;
    if (desk_text_measure(font,s)<=width) return desk_paint_text(&clipped,font,x,baseline,s,colour);
    int ellipsis=desk_text_advance(font,0x2026);
    if (ellipsis>width) return 0;
    size_t chars=desk_text_fit_chars(font,s,width-ellipsis);
    int used=desk_paint_text_n(&clipped,font,x,baseline,s,chars,colour);
    return used+desk_paint_text(&clipped,font,x+used,baseline,"\xe2\x80\xa6",colour);
}

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

/* ---- rounded buttons and icons ---- */
static unsigned round_cover(int col, int row, int w, int h, int r)
{
    /* Coverage (0..16) of pixel (col, row) by the rounded rectangle
     * [0, w) x [0, h) of radius r: 4x4 samples, in eighths of a pixel. */
    if (col < 0 || row < 0 || col >= w || row >= h) return 0;
    if (r <= 0) return 16;
    int cx = col < r ? r : col >= w - r ? w - r : -1, cy = row < r ? r : row >= h - r ? h - r : -1;
    if (cx < 0 || cy < 0) return 16;
    unsigned n = 0;
    int64_t limit = (int64_t)(8 * r) * (8 * r);
    for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
        int64_t dx = (int64_t)col * 8 + sx * 2 + 1 - (int64_t)cx * 8, dy = (int64_t)row * 8 + sy * 2 + 1 - (int64_t)cy * 8;
        n += dx * dx + dy * dy <= limit;
    }
    return n;
}

void desk_paint_task(struct desk_paint *p, int x, int y, int w, int h, unsigned state, int rgb565)
{
    int vx=x, vy=y, vw=w, vh=h;
    if (!clip(p,&vx,&vy,&vw,&vh)) return;
    const struct desk_theme *t=desk_theme;
    int active=(state & DESK_TASK_ACTIVE)!=0, hot=(state & DESK_TASK_HOT)!=0;
    uint32_t base=active ? desk_mix(0,t->task_active,20) : t->taskbar;
    if (state & DESK_TASK_MINIMIZED) base=desk_mix(t->hilite,base,56);
    if (hot) base=desk_mix(t->hover,base,112);
    uint32_t top=active ? desk_mix(t->shadow,base,36) : desk_mix(t->hilite,base,48);
    uint32_t bottom=active ? base : desk_mix(t->shadow,base,24);
    uint32_t edge=desk_mix(t->outline,base,active ? 140 : hot ? 115 : 90);
    int radius=w<6 ? w/2 : h<6 ? h/2 : 3;
    for (int yy=vy; yy<vy+vh; yy++) for (int xx=vx; xx<vx+vw; xx++) {
        int col=xx-x, row=yy-y;
        unsigned outer=round_cover(col,row,w,h,radius);
        if (!outer) continue;
        unsigned inner=round_cover(col-1,row-1,w-2,h-2,radius>0 ? radius-1 : 0);
        if (inner>outer) inner=outer;
        uint32_t face=desk_mix(bottom,top,h>1 ? (unsigned)(row*255/(h-1)) : 0);
        if (row==1) face=desk_mix(active ? t->shadow : t->hilite,face,90);
        if (row==h-2) face=desk_mix(active ? t->hilite : t->shadow,face,55);
        if (active && row>=h-3 && row<=h-2 && col>=4 && col<w-4) face=t->accent;
        /* Disjoint coverage weights: compose once so the face and border
         * cannot darken each other's partially covered corner pixels. */
        uint32_t ink=inner ? desk_mix(face,edge,inner*255/outer) : edge;
        uint32_t *dst=p->pixels+(size_t)yy*p->width+xx;
        *dst=surface_pixel(desk_mix(ink,*dst,outer*255/16),xx,yy,rgb565);
    }
}

void desk_paint_round_rect(struct desk_paint *p, int x, int y, int w, int h, int r,
                           uint32_t top, uint32_t bottom, uint32_t outline, unsigned outline_alpha)
{
    if (w <= 0 || h <= 0) return;
    if (2 * r > h) r = h / 2;
    if (2 * r > w) r = w / 2;
    if (r < 0) r = 0;
    int vx = x, vy = y, vw = w, vh = h;
    if (!clip(p, &vx, &vy, &vw, &vh)) return;
    for (int yy = vy; yy < vy + vh; yy++) {
        int row = yy - y;
        uint32_t face = desk_mix(bottom, top, h > 1 ? (unsigned)(row * 255 / (h - 1)) : 0);
        uint32_t *dst = p->pixels + (size_t)yy * (size_t)p->width;
        for (int xx = vx; xx < vx + vw; xx++) {
            int col = xx - x;
            unsigned outer = round_cover(col, row, w, h, r);
            if (!outer) continue;
            unsigned inner = round_cover(col - 1, row - 1, w - 2, h - 2, r > 0 ? r - 1 : 0);
            if (inner > outer) inner = outer;
            uint32_t v = dst[xx];
            if (inner) v = desk_mix(face, v, inner * 255 / 16);
            if (outer > inner && outline_alpha) v = desk_mix(outline, v, (outer - inner) * outline_alpha / 16);
            dst[xx] = v;
        }
    }
}

/* Translucent rounded rectangle (desktop icon selection): the fill blended
 * at fill_alpha, the 1 px outline at outline_alpha, anti-aliased corners. */
void desk_paint_round_blend(struct desk_paint *p, int x, int y, int w, int h, int r,
                            uint32_t fill, unsigned fill_alpha, uint32_t outline, unsigned outline_alpha)
{
    if (w <= 0 || h <= 0) return;
    if (2 * r > h) r = h / 2;
    if (2 * r > w) r = w / 2;
    if (r < 0) r = 0;
    int vx = x, vy = y, vw = w, vh = h;
    if (!clip(p, &vx, &vy, &vw, &vh)) return;
    for (int yy = vy; yy < vy + vh; yy++) {
        uint32_t *dst = p->pixels + (size_t)yy * (size_t)p->width;
        for (int xx = vx; xx < vx + vw; xx++) {
            unsigned outer = round_cover(xx - x, yy - y, w, h, r);
            if (!outer) continue;
            unsigned inner = round_cover(xx - x - 1, yy - y - 1, w - 2, h - 2, r > 0 ? r - 1 : 0);
            if (inner > outer) inner = outer;
            uint32_t v = dst[xx];
            if (inner) v = desk_mix(fill, v, inner * fill_alpha / 16);
            if (outer > inner) v = desk_mix(outline, v, (outer - inner) * outline_alpha / 16);
            dst[xx] = v;
        }
    }
}

void desk_paint_icon(struct desk_paint *p, int x, int y, const uint32_t *argb, int size)
{
    int vx = x, vy = y, vw = size, vh = size;
    if (!argb || size <= 0 || !clip(p, &vx, &vy, &vw, &vh)) return;
    for (int yy = vy; yy < vy + vh; yy++) for (int xx = vx; xx < vx + vw; xx++) {
        uint32_t s = argb[(yy - y) * size + (xx - x)], *dst = p->pixels + (size_t)yy * (size_t)p->width + (size_t)xx;
        if (s >> 24) *dst = desk_mix(s, *dst, s >> 24);
    }
}

/* ---- title-bar buttons ----
 * Platinum and Ciuki Blue: a square of the theme's size with crisp 7x7
 * glyphs (an anti-aliased cross, a bar, a roll-up/down triangle), hover and
 * pressed faces. Inactive
 * frames keep their buttons in a quiet, flat form. */
static unsigned glyph_alpha(int kind, int i, int j)
{
    /* 7x7 masks; i column, j row. */
    switch (kind) {
    case DESK_GLYPH_CLOSE: {
        int d1 = i > j ? i - j : j - i, d2 = i + j > 6 ? i + j - 6 : 6 - i - j, d = d1 < d2 ? d1 : d2;
        return d == 0 ? 255 : d == 1 ? 132 : 0;
    }
    case DESK_GLYPH_MINIMIZE: return j >= 5 ? 255 : 0;
    case DESK_GLYPH_RESTORE:
        return ((i>=2 && (j==0 || j==1 || (j<=4 && i==6))) ||
            (j>=2 && i<=4 && (i==0 || i==4 || j==2 || j==3 || j==6))) ? 255 : 0;
    case DESK_GLYPH_COLLAPSE: return j >= 1 && j <= 4 && (i - 3 <= j - 1 && 3 - i <= j - 1) ? 255 : 0;   /* roll up */
    case DESK_GLYPH_EXPAND: return j >= 2 && j <= 5 && (i - 3 <= 5 - j && 3 - i <= 5 - j) ? 255 : 0;     /* roll down */
    default: return i == 0 || j == 0 || i == 6 || j == 6 || j == 1 ? 255 : 0;                               /* zoom */
    }
}

static void glyph(struct desk_paint *p, int x, int y, int kind, uint32_t ink, unsigned alpha)
{
    for (int j = 0; j < 7; j++) for (int i = 0; i < 7; i++) {
        unsigned a = glyph_alpha(kind, i, j) * alpha / 255;
        if (a) desk_paint_blend(p, x + i, y + j, 1, 1, ink, a);
    }
}

static void square_edge(struct desk_paint *p, int x, int y, int s, uint32_t colour, unsigned alpha)
{
    /* A 1 px square outline whose corner pixels are half strength. */
    desk_paint_blend(p, x + 1, y, s - 2, 1, colour, alpha); desk_paint_blend(p, x + 1, y + s - 1, s - 2, 1, colour, alpha);
    desk_paint_blend(p, x, y + 1, 1, s - 2, colour, alpha); desk_paint_blend(p, x + s - 1, y + 1, 1, s - 2, colour, alpha);
    for (int k = 0; k < 4; k++) desk_paint_blend(p, x + (k & 1) * (s - 1), y + (k >> 1) * (s - 1), 1, 1, colour, alpha / 2);
}

void desk_paint_title_button(struct desk_paint *p, int x, int y, int w, int h, int kind, int active, int hot, int pressed)
{
    const struct desk_theme *t = desk_theme;
    if (w <= 0 || h <= 0) return;
    int s = t->button;
    if (s > w) s = w;
    if (s > h) s = h;
    if (s < 9) return;
    int bx = x + (w - s) / 2, by = y + (h - s) / 2, gx = bx + (s - 7) / 2, gy = by + (s - 7) / 2;
    if (t->title_style == DESK_TITLE_HGRADIENT) {
        /* Blue uses the same cool face and bevel as every push button.
         * Inactive windows retain readable, operable caption controls. */
        desk_paint_raised(p, bx, by, s, s, pressed, hot);
        glyph(p, gx + pressed, gy + pressed, kind, t->ink, 255);
        return;
    }
    if (t->title_style == DESK_TITLE_PINSTRIPE) {
        /* Platinum: a bevelled square on the pinstripes. */
        if (!active) {
            square_edge(p, bx, by, s, t->shadow, 150);
            glyph(p, gx, gy, kind, t->gray, 150);
            return;
        }
        uint32_t top = pressed ? t->shadow : t->hilite;
        uint32_t bottom = pressed ? t->face : hot ? desk_mix(t->focus, t->hover, 120) : t->face_bottom;
        if (hot && !pressed) top = desk_mix(t->hover, t->hilite, 110);
        desk_paint_vgradient(p, bx + 1, by + 1, s - 2, s - 2, top, bottom);
        if (pressed) {
            desk_paint_fill(p, bx + 1, by + 1, s - 2, 1, desk_mix(t->dark, top, 110));
            desk_paint_fill(p, bx + 1, by + 2, 1, s - 3, desk_mix(t->dark, top, 70));
        } else {
            desk_paint_fill(p, bx + 1, by + 1, s - 3, 1, t->hilite); desk_paint_fill(p, bx + 1, by + 1, 1, s - 3, t->hilite);
            desk_paint_fill(p, bx + 2, by + s - 2, s - 3, 1, desk_mix(t->shadow, bottom, 120));
            desk_paint_fill(p, bx + s - 2, by + 2, 1, s - 3, desk_mix(t->shadow, bottom, 120));
        }
        square_edge(p, bx, by, s, hot && !pressed ? desk_mix(t->accent, t->outline, 150) : t->outline, 255);
        glyph(p, gx + pressed, gy + pressed, kind, hot && !pressed ? desk_mix(t->accent, t->ink, 110) : t->ink, 255);
        return;
    }
    /* Gradient captions: a glassy square over the caption colour. */
    if (!active) {
        square_edge(p, bx, by, s, 0xffffffu, 90);
        glyph(p, gx, gy, kind, t->title_text_inactive, 150);
        return;
    }
    for (int r = 1; r < s - 1; r++) {
        unsigned a = pressed ? 0 : (unsigned)((hot ? 150 : 104) - r * (hot ? 70 : 64) / (s - 2));
        if (pressed) desk_paint_blend(p, bx + 1, by + r, s - 2, 1, 0x000000u, (unsigned)(90 - r * 50 / (s - 2)));
        else desk_paint_blend(p, bx + 1, by + r, s - 2, 1, 0xffffffu, a);
    }
    if (!pressed) desk_paint_blend(p, bx + 1, by + 1, s - 2, 1, 0xffffffu, 110);
    square_edge(p, bx, by, s, hot && !pressed ? t->accent : 0x000000u, hot && !pressed ? 255 : 120);
    glyph(p, gx + pressed, gy + pressed, kind, t->title_text, 255);
}

/* ---- window frames ---- */
static int corner_inset(int radius, int row)
{
    /* Rounded top corners: pixels a row of the frame skips at either side. */
    if (row < 0 || row >= radius) return 0;
    int d = 2 * radius - 2 * row - 1, v = 4 * radius * radius - d * d, r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return radius - (r + 1) / 2;
}
struct frame { struct desk_paint p; int x, y, w, h, radius; };
static void ffill(struct frame *f, int x, int y, int w, int h, uint32_t colour)
{
    for (int yy = y; yy < y + h; yy++) {
        int in = corner_inset(f->radius, yy - f->y), x0 = x, x1 = x + w;
        if (x0 < f->x + in) x0 = f->x + in;
        if (x1 > f->x + f->w - in) x1 = f->x + f->w - in;
        if (x1 > x0) desk_paint_fill(&f->p, x0, yy, x1 - x0, 1, colour);
    }
}
static void fvgradient(struct frame *f, int x, int y, int w, int h, uint32_t top, uint32_t bottom)
{
    for (int r = 0; r < h; r++) ffill(f, x, y + r, w, 1, desk_mix(bottom, top, h > 1 ? (unsigned)(r * 255 / (h - 1)) : 0));
}
static void frame_outline(struct frame *f, uint32_t c)
{
    /* 1 px outline; rounded top corners step one pixel at a time (8-connected)
     * and the pixel outside each step takes a faint tint of the outline. */
    int r = f->radius, x = f->x, y = f->y, w = f->w, h = f->h, prev = 0;
    for (int i = 0; i < r && i < h; i++) {
        int in = corner_inset(r, i);
        if (!i) desk_paint_fill(&f->p, x + in, y, w - 2 * in, 1, c);
        else {
            int run = prev - in > 1 ? prev - in : 1;
            desk_paint_fill(&f->p, x + in, y + i, run, 1, c);
            desk_paint_fill(&f->p, x + w - in - run, y + i, run, 1, c);
        }
        if (in > 0) {
            desk_paint_blend(&f->p, x + in - 1, y + i, 1, 1, c, 72);
            desk_paint_blend(&f->p, x + w - in, y + i, 1, 1, c, 72);
        }
        prev = in;
    }
    if (!r) desk_paint_fill(&f->p, x, y, w, 1, c);
    int top = r ? r : 1;
    desk_paint_fill(&f->p, x, y + top, 1, h - top, c);
    desk_paint_fill(&f->p, x + w - 1, y + top, 1, h - top, c);
    desk_paint_fill(&f->p, x, y + h - 1, w, 1, c);
}
static struct desk_paint sub(const struct desk_paint *p, int x, int y, int w, int h)
{
    struct desk_paint s = *p;
    int x0 = x > p->cx ? x : p->cx, y0 = y > p->cy ? y : p->cy;
    int x1 = x + w < p->cx + p->cw ? x + w : p->cx + p->cw, y1 = y + h < p->cy + p->ch ? y + h : p->cy + p->ch;
    s.cx = x0; s.cy = y0; s.cw = x1 > x0 ? x1 - x0 : 0; s.ch = y1 > y0 ? y1 - y0 : 0;
    return s;
}

static void fhgradient(struct frame *f, int x, int y, int w, int h, uint32_t left, uint32_t right)
{
    /* Keep the endpoints anchored to the complete caption when clipping
     * damage or rounded corners: partial redraws must use identical pixels. */
    for (int yy = y; yy < y + h; yy++) {
        int in = corner_inset(f->radius, yy - f->y);
        struct desk_paint p = sub(&f->p, f->x + in, yy, f->w - 2 * in, 1);
        desk_paint_hgradient(&p, x, yy, w, 1, left, right);
    }
}

void desk_title_button_box(int x, int y, int w, unsigned button, int box[4])
{
    /* Close at the right, then minimise and collapse, 2 px apart; the same
     * hit boxes in every theme (scripted coordinates never depend on it). */
    int cw = w - 2 * DESK_FRAME_BORDER;
    if (cw > DESK_FRAME_BUTTON) cw = DESK_FRAME_BUTTON;
    int close[4] = {x + w - DESK_FRAME_BORDER - cw, y + 3, cw, DESK_FRAME_BUTTON};
    int available = close[0] - x - DESK_FRAME_BORDER - 2;
    int mw = available <= 0 ? 0 : available < DESK_FRAME_BUTTON ? available : DESK_FRAME_BUTTON;
    int min[4] = {mw ? close[0] - mw - 2 : 0, mw ? close[1] : 0, mw, mw ? close[3] : 0};
    int shade[4] = {0, 0, 0, 0};
    if (mw > 0 && min[0] - x - DESK_FRAME_BORDER - 40 >= DESK_FRAME_BUTTON)
        shade[0] = min[0] - DESK_FRAME_BUTTON - 2, shade[1] = min[1], shade[2] = DESK_FRAME_BUTTON, shade[3] = min[3];
    const int *b = button == DESK_TB_CLOSE ? close : button == DESK_TB_MINIMIZE ? min : shade;
    if (button != DESK_TB_CLOSE && button != DESK_TB_MINIMIZE && button != DESK_TB_COLLAPSE) b = (const int[4]){0, 0, 0, 0};
    for (int k = 0; k < 4; k++) box[k] = b[k];
}

static void paint_frame(struct desk_paint *p, int x, int y, int w, int h, const struct desk_frame_style *s, int covered)
{
    const struct desk_theme *t = desk_theme;
    if (w <= 2 || h <= 2) return;
    struct frame f = {*p, x, y, w, h, t->radius};
    int platinum = t->title_style == DESK_TITLE_PINSTRIPE;
    int active = s->active, T = DESK_FRAME_TITLE;
    static const unsigned order[3] = {DESK_TB_CLOSE, DESK_TB_MINIMIZE, DESK_TB_COLLAPSE};
    int boxes[3][4], right = x + w - 4;   /* right: the title area ends 4 px before the first button */
    for (int k = 0; k < 3; k++) {
        desk_title_button_box(x, y, w, order[k], boxes[k]);
        if (!(s->buttons & order[k])) boxes[k][2] = 0;
        if (boxes[k][2] > 0 && boxes[k][0] - 3 < right) right = boxes[k][0] - 3;
    }
    const char *title = s->title ? s->title : "";
    int font = t->font_title, tw = desk_text_measure(font, title);
    uint32_t a0 = active ? t->title[0] : t->title_inactive[0], a1 = active ? t->title[1] : t->title_inactive[1];
    if (s->alert) {
        /* Blue's dark inactive text needs a light warning tint. */
        int light_alert = !active && t->title_style == DESK_TITLE_HGRADIENT;
        uint32_t warning = light_alert ? 0xefc8b9u : 0x9a3c2cu;
        unsigned alpha = light_alert ? 90 : platinum ? 60 : 140;
        a0 = desk_mix(warning, a0, alpha); a1 = desk_mix(warning, a1, alpha);
    }
    uint32_t ink = active ? t->title_text : t->title_text_inactive;
    if (!covered) ffill(&f, x, y, w, h, t->face);
    else {
        ffill(&f,x,y,w,T,t->face);
        ffill(&f,x,y+T,DESK_FRAME_BORDER,h-T,t->face);
        ffill(&f,x+w-DESK_FRAME_BORDER,y+T,DESK_FRAME_BORDER,h-T,t->face);
        ffill(&f,x,y+h-DESK_FRAME_BORDER,w,DESK_FRAME_BORDER,t->face);
    }
    /* Rounded frames: Platinum's vertical pinstripes or Blue's
     * left-to-right caption, with the same title-button hit boxes. */
    int horizontal = t->title_style == DESK_TITLE_HGRADIENT;
    if (horizontal) fhgradient(&f, x + 1, y + 1, w - 2, T - 2, a0, a1);
    else {
        fvgradient(&f, x + 1, y + 1, w - 2, T - 2, a0, a1);
        ffill(&f, x + 1, y + 1, w - 2, 1, platinum ? t->hilite : desk_mix(0xffffffu, a0, 80));
    }
    int left = x + 6, has_icon = !platinum && s->icon;
    if (has_icon) { desk_paint_icon(&f.p, x + 6, y + 4, s->icon, 16); left = x + 28; }
    int tx = t->title_center ? x + (w - tw) / 2 : left + 4;
    if (tx + tw > right - 6) tx = right - 6 - tw;
    if (tx < left + 4) tx = left + 4;
    if (platinum && active && !s->alert) {
        /* Pinstripes either side of the title: dark lines on rows 5, 7 .. 17
         * and light lines offset by a pixel between them, blended into the band. */
        for (int k = 0; k < 2; k++) {
            int sx = k ? tx + tw + 8 : x + 6, ex = k ? right : tx - 8;
            for (int r = 5; r <= 17 && ex > sx; r++) {
                if (r & 1) desk_paint_blend(&f.p, sx, y + r, ex - sx, 1, t->stripe_dark, 190);
                else desk_paint_blend(&f.p, sx + 1, y + r, ex - sx - 1, 1, t->stripe_light, 220);
            }
        }
    }
    struct desk_paint tp = sub(&f.p, left, y, right - left, T);
    uint32_t emboss = platinum ? desk_mix(t->hilite, a1, 200) : desk_mix(0x000000u, a1, 120);
    if (!horizontal) desk_paint_text(&tp, font, tx, y + 17, title, emboss);
    desk_paint_text(&tp, font, tx, y + 16, title, ink);

    for (int k = 0; k < 3; k++) {
        if (boxes[k][2] <= 0) continue;
        int kind = order[k] == DESK_TB_CLOSE ? DESK_GLYPH_CLOSE : order[k] == DESK_TB_MINIMIZE ? DESK_GLYPH_MINIMIZE :
                   s->collapsed ? DESK_GLYPH_EXPAND : s->maximized ? DESK_GLYPH_RESTORE : DESK_GLYPH_ZOOM;
        desk_paint_title_button(&f.p, boxes[k][0], boxes[k][1], boxes[k][2], boxes[k][3], kind, active,
                                s->hot == order[k], s->pressed == order[k]);
    }
    /* Side bevels, the line under the title and the content well. */
    int blue = t->title_style == DESK_TITLE_HGRADIENT;
    uint32_t well = blue ? t->shadow : platinum ? (active ? t->dark : t->shadow) : desk_mix(t->dark, a1, 120);
    desk_paint_fill(&f.p, x + 1, y + 2, 1, h - 3, t->hilite);
    desk_paint_fill(&f.p, x + w - 2, y + 2, 1, h - 3, active || blue ? t->shadow : t->light);
    desk_paint_fill(&f.p, x + 1, y + h - 2, w - 2, 1, active || blue ? t->shadow : t->light);
    desk_paint_fill(&f.p, x + 2, y + T - 1, w - 4, 1, well);
    if (!s->collapsed && h > T + 2) {
        uint32_t side = platinum ? well : t->shadow;
        desk_paint_fill(&f.p, x + 2, y + T, 1, h - T - 2, side);
        desk_paint_fill(&f.p, x + w - 3, y + T, 1, h - T - 2, side);
        desk_paint_fill(&f.p, x + 2, y + h - 3, w - 4, 1, side);
    }
    frame_outline(&f, active ? t->frame : t->frame_inactive);
}

void desk_paint_frame(struct desk_paint *p, int x, int y, int w, int h, const struct desk_frame_style *s)
{ paint_frame(p,x,y,w,h,s,0); }
void desk_paint_frame_border(struct desk_paint *p, int x, int y, int w, int h, const struct desk_frame_style *s)
{ paint_frame(p,x,y,w,h,s,1); }

/* ---- settings ----
 * desktop.cfg has one formatter, settings.c (cfg_format: the shell and the
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
        const char *canonical = desk_theme_preference(value);
        if (!canonical) continue;
        snprintf(name, size, "%s", canonical);
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
    if (!name[0] || desk_theme_use(name)) desk_theme_use(DESK_THEME_DEFAULT);
    return desk_theme != before;
}
