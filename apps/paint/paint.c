/* SPDX-License-Identifier: MIT
 * CiukPaint native port of src/apps/paint.c. Drawing, shape previews, saved-row
 * undo, selections, transforms, tool glyphs and option geometry preserve the
 * 0.8 implementation. Flat heap images and libdeskui replace DOS/XMS and CAPP.
 * One editor per process, with the same implementation in the host harness. */
#include "paint.h"
#include "../libdeskui/textarea.h"
#include "default_font.h"
#include "../libdeskui/job.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef u8 *fptr;
#define MAX_W 2048
#define MAX_H 1200
#define DEF_W 512
#define DEF_H 384
#define NCOL PAINT_COLORS
#define WHITE 80
#define BLACK 79
static struct du_ui *ui;
static void (*quit_cb)(void *);
static void (*title_cb)(void *, const char *);
static void *callback_host;
static void fmt_u32(char *out, u32 n) { sprintf(out, "%u", (unsigned)n); }
static void no_memory(void);
static void layout(void);
static void open_finish(void);
static void clamp_scroll(void);
static void finish_pending(void);
static void drop_selection(void);
static void set_zoom(int z, int x, int y);
static int zoom_step(int dir);
static void context_menu(int x, int y);

static int iabs(int v) { return v < 0 ? -v : v; }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static long isqrt(long n)
{
    long x = 0, b = 1L << 30;
    if (n <= 0) return 0;
    while (b > n) b >>= 2;
    while (b) {
        if (n >= x + b) { n -= x + b; x = (x >> 1) + b; } else x >>= 1;
        b >>= 2;
    }
    return x;
}
static u16 seed = 12345;
static int rnd(int n) { seed = seed * 25173u + 13849u; return (int)((seed >> 4) % (u16)n); }

static const u8 logo_pal[192] = {
    63, 60, 50, 63, 58, 49, 62, 57, 48, 62, 57, 47, 62, 57, 47, 62, 56, 45, 61, 56, 47, 62, 54, 42,
    60, 54, 45, 61, 53, 41, 61, 52, 38, 61, 50, 34, 58, 53, 44, 58, 51, 41, 58, 50, 39, 56, 50, 41,
    60, 48, 31, 60, 47, 29, 60, 46, 26, 59, 46, 27, 59, 45, 26, 59, 44, 26, 58, 44, 26, 58, 44, 25,
    57, 48, 36, 57, 44, 27, 54, 48, 39, 52, 45, 35, 57, 43, 25, 55, 41, 24, 53, 42, 28, 47, 42, 34,
    53, 40, 24, 52, 37, 21, 48, 37, 25, 48, 35, 21, 49, 34, 19, 46, 32, 18, 46, 30, 18, 47, 29, 19,
    42, 37, 30, 40, 33, 24, 44, 30, 17, 38, 30, 22, 44, 29, 15, 43, 27, 15, 38, 27, 16, 31, 27, 22,
    43, 24, 21, 33, 24, 15, 35, 20, 15, 31, 18, 11, 26, 23, 19, 25, 20, 15, 24, 18, 14, 22, 16, 12,
    18, 16, 14, 16, 13, 12, 14, 11, 10, 11, 10, 10, 10, 9, 9, 9, 8, 8, 7, 7, 7, 4, 4, 5,
};
static const u8 icon_pal[192] = {
    63, 63, 63, 63, 63, 63, 62, 62, 62, 61, 61, 61, 61, 61, 61, 60, 60, 60, 60, 60, 60, 59, 59, 59,
    59, 59, 59, 58, 59, 59, 58, 58, 58, 58, 57, 55, 57, 58, 56, 56, 56, 56, 56, 56, 56, 55, 56, 55,
    55, 55, 55, 54, 54, 55, 53, 54, 53, 55, 54, 48, 52, 52, 52, 51, 51, 51, 52, 51, 45, 54, 48, 40,
    48, 50, 51, 47, 50, 39, 47, 48, 49, 46, 47, 33, 46, 46, 46, 45, 46, 43, 40, 45, 50, 43, 45, 33,
    44, 43, 41, 42, 42, 38, 47, 38, 28, 41, 41, 40, 39, 40, 41, 38, 38, 38, 38, 40, 29, 37, 37, 36,
    32, 40, 45, 33, 37, 36, 36, 36, 35, 32, 35, 35, 34, 34, 33, 33, 33, 31, 24, 33, 45, 31, 32, 30,
    35, 29, 18, 28, 28, 27, 24, 28, 35, 26, 26, 23, 28, 25, 20, 22, 23, 19, 21, 21, 20, 37, 13, 11,
    16, 27, 42, 17, 25, 37, 16, 21, 31, 19, 21, 8, 12, 17, 28, 16, 17, 15, 9, 13, 19, 5, 5, 5,
};
static const u8 desk_default[48] = {
    9, 10, 12,  13, 18, 30,  18, 32, 23,  13, 23, 24,
    37, 15, 16,  31, 24, 37,  37, 29, 15,  51, 52, 53,
    28, 30, 34,  27, 36, 48,  24, 41, 38,  39, 49, 48,
    49, 25, 23,  43, 34, 46,  57, 47, 27,  61, 61, 60 };
static u8 pal[NCOL * 3];                     /* 6-bit RGB of every index */
static void load_palette(void)
{
    memcpy(pal, desk_default, 48);
    memcpy(pal + 48, logo_pal, 192);
    memcpy(pal + 240, icon_pal, 192);
}
static u16 cdist(int i, int r, int g, int b)
{
    int dr = pal[i * 3] - r, dg = pal[i * 3 + 1] - g, db = pal[i * 3 + 2] - b;
    return (u16)(2 * dr * dr) + (u16)(4 * dg * dg) + (u16)(3 * db * db);
}
/* The nearest index to a 6-bit colour. The scheme's colours (0-15) change
 * with the Control Panel, so they win only when clearly closer. */
static u8 nearest(int r, int g, int b)
{
    u16 best = 0xFFFF, d;
    int i, bi = 16;
    for (i = 16; i < NCOL; i++) {
        d = cdist(i, r, g, b);
        if (d < best) { best = d; bi = i; if (!d) return (u8)i; }
    }
    for (i = 0; i < 16; i++) {
        d = cdist(i, r, g, b);
        if ((u32)d * 2 < best || !d) { best = d / 2; bi = i; }
    }
    return (u8)bi;
}

/* The colour box: 28 swatches, the nearest indices to these colours. */
#define NSWATCH 28
static const u8 swatch_rgb[NSWATCH * 3] = {
    0, 0, 0,   30, 30, 30,  34, 6, 6,    34, 26, 10,  10, 28, 14,  8, 24, 24,  10, 14, 32,
    30, 18, 38, 16, 24, 40,  22, 24, 8,   42, 26, 12,  46, 20, 12,  36, 28, 20,  20, 24, 28,
    63, 63, 63, 48, 48, 48,  50, 12, 12,  60, 50, 22,  22, 42, 26,  26, 44, 44,  28, 40, 56,
    44, 34, 48, 40, 50, 50,  52, 28, 26,  60, 48, 32,  63, 60, 50,  46, 46, 34,  56, 56, 56 };
static u8 swatch[NSWATCH];
static u8 fg = BLACK, bg = WHITE;
static void choose_swatches(void)
{
    int i, k;
    for (i = 0; i < NSWATCH; i++) {
        u8 c = nearest(swatch_rgb[i * 3], swatch_rgb[i * 3 + 1], swatch_rgb[i * 3 + 2]);
        for (k = 0; k < i; k++) if (swatch[k] == c) break;
        if (k < i) {                            /* taken: the next closest */
            u16 best = 0xFFFF, d;
            int j;
            for (j = 16; j < NCOL; j++) {
                for (k = 0; k < i && swatch[k] != j; k++) ;
                if (k < i) continue;
                d = cdist(j, swatch_rgb[i * 3], swatch_rgb[i * 3 + 1], swatch_rgb[i * 3 + 2]);
                if (d < best) { best = d; c = (u8)j; }
            }
        }
        swatch[i] = c;
    }
}

/* Flat, bounded indexed images. Palette is fixed independently of UI theme. */
static struct paint_image cv, old, pool, flt, clip;
static void img_free(struct paint_image *m) { free(m->pixels); memset(m, 0, sizeof(*m)); }
static int img_alloc(struct paint_image *m, int w, int h)
{
    if (w < 1 || h < 1 || w > PAINT_MAX_AXIS || h > PAINT_MAX_AXIS || (unsigned)w * (unsigned)h > PAINT_MAX_PIXELS) return 0;
    u8 *p = malloc((size_t)w * (size_t)h);
    if (!p) return 0;
    *m = (struct paint_image){w, h, p}; return 1;
}
static fptr rowp(struct paint_image *m, int y) { return m->pixels + (size_t)y * (size_t)m->w; }
static void img_fill(struct paint_image *m, u8 c) { memset(m->pixels, c, (size_t)m->w * (size_t)m->h); }
#define U_NONE 0
#define U_ROWS 1
#define U_IMAGE 2
static u16 slot_of[PAINT_MAX_AXIS], row_of[PAINT_MAX_AXIS];
static int nslots, op_open, op_ok, undo_kind, undone, dirty;
static void pool_free(void) { img_free(&pool); }
static int pool_cap(void) { return pool.w == cv.w ? pool.h : 0; }
static int pool_grow(void)
{
    if (pool.w != cv.w) pool_free();
    int rows = imin(cv.h, pool.h + 32);
    if (rows <= pool.h) return 0;
    u8 *p = realloc(pool.pixels, (size_t)rows * (size_t)cv.w);
    if (!p) return 0;
    pool = (struct paint_image){cv.w, rows, p}; return 1;
}
static void pool_put(int k, int y) { memcpy(rowp(&pool, k), rowp(&cv, y), (size_t)cv.w); }
static void pool_get(int k, int y) { memcpy(rowp(&cv, y), rowp(&pool, k), (size_t)cv.w); }
static void pool_trim(void) { if (!nslots) pool_free(); }

static void drop_undo(void)
{
    int i;
    for (i = 0; i < nslots; i++) slot_of[row_of[i]] = 0;
    nslots = 0;
    if (undo_kind == U_IMAGE) img_free(&old);
    undo_kind = U_NONE;
    undone = 0;
}
static int alloc_retry(struct paint_image *m, int w, int h)
{
    if (img_alloc(m, w, h)) return 1;
    if (!op_open) { if (undo_kind == U_ROWS) drop_undo(); pool_trim(); }
    return img_alloc(m, w, h);
}

static uint64_t image_revision;
static int dx0, dy0, dx1, dy1 = -1;          /* image area to show again */
static int bx0, by0, bx1, by1 = -1;          /* what the preview drew */
static void dmg_add(int x0, int y0, int x1, int y1)
{
    image_revision++;
    if (dy1 < dy0 || dx1 < dx0) { dx0 = x0; dy0 = y0; dx1 = x1; dy1 = y1; return; }
    if (x0 < dx0) dx0 = x0;
    if (y0 < dy0) dy0 = y0;
    if (x1 > dx1) dx1 = x1;
    if (y1 > dy1) dy1 = y1;
}
static void dmg_all(void) { dmg_add(0, 0, cv.w - 1, cv.h - 1); }

/* An operation. A preview (shapes, selections, text) draws again at each
 * move over the rows it saved; a row there is no room to save is left out
 * until the end, when final_render draws it all once more without undo. */
static int op_preview, skipped;
static void (*final_render)(void);
static int begin_op(int preview)
{
    drop_undo();
    op_open = 1; op_ok = 1; op_preview = preview; skipped = 0;
    final_render = 0;
    bx0 = by0 = 0; bx1 = by1 = -1;
    return 1;
}
static int touch(int y)                      /* 0: leave this row alone */
{
    u16 k;
    if (!op_open || !op_ok || slot_of[y]) return 1;
    if (nslots >= pool_cap() && !pool_grow()) {
        if (op_preview) { skipped = 1; return 0; }
        op_ok = 0;
        return 1;
    }
    k = nslots;
    pool_put(k, y);
    row_of[k] = y;
    slot_of[y] = k + 1;
    nslots++;
    return 1;
}
static void restore_all(void)
{
    int k;
    for (k = 0; k < nslots; k++)
        pool_get(k, row_of[k]);
    if (by1 >= by0) dmg_add(bx0, by0, bx1, by1);
    bx0 = by0 = 0; bx1 = by1 = -1;
}
static char status[100];
static void commit_op(void)
{
    int k;
    if (!op_open) return;
    if (skipped && final_render) {
        restore_all();
        for (k = 0; k < nslots; k++) slot_of[row_of[k]] = 0;
        nslots = 0;
        op_ok = 0; skipped = 0;
        final_render();
    }
    op_open = 0;
    if (nslots || !op_ok) dirty = 1;
    if (op_ok && nslots) { undo_kind = U_ROWS; undone = 0; }
    else {
        if (!op_ok) strcpy(status, "There was not enough memory to keep an undo copy.");
        drop_undo();
    }
}
static void cancel_op(void)
{
    if (!op_open) return;
    restore_all();
    op_open = 0;
    drop_undo();
}
static void swap_rows(void)
{
    for (int k = 0; k < nslots; k++) {
        fptr a = rowp(&cv, row_of[k]), b = rowp(&pool, k);
        for (int i = 0; i < cv.w; i++) { u8 t = a[i]; a[i] = b[i]; b[i] = t; }
    }
}

/* Drawing into the picture                                            */
static int pen_replace = -1;                 /* >= 0: only this colour changes */
static void hspan(int x0, int x1, int y, u8 c)
{
    fptr p;
    int x;
    if (y < 0 || y >= cv.h) return;
    if (x0 > x1) { x = x0; x0 = x1; x1 = x; }
    if (x0 < 0) x0 = 0;
    if (x1 >= cv.w) x1 = cv.w - 1;
    if (x0 > x1 || !touch(y)) return;
    if (pen_replace >= 0) {
        p = rowp(&cv, y);
        for (x = x0; x <= x1; x++) if (p[x] == (u8)pen_replace) p[x] = c;
    } else memset(rowp(&cv, y) + x0, c, (size_t)(x1 - x0 + 1));
    dmg_add(x0, y, x1, y);
    if (by1 < by0) { bx0 = x0; by0 = y; bx1 = x1; by1 = y; }
    else {
        if (x0 < bx0) bx0 = x0;
        if (y < by0) by0 = y;
        if (x1 > bx1) bx1 = x1;
        if (y > by1) by1 = y;
    }
}
static void plot(int x, int y, u8 c) { hspan(x, x, y, c); }
static u8 pixel(int x, int y)
{
    if (x < 0 || y < 0 || x >= cv.w || y >= cv.h) return bg;
    return rowp(&cv, y)[x];
}
static void fill_rect(int x0, int y0, int x1, int y1, u8 c)
{
    int y;
    for (y = y0; y <= y1; y++) hspan(x0, x1, y, c);
}

/* Pens: a dot, a square, a disc or a diagonal of pen_size pixels. */
#define PEN_DOT    0
#define PEN_SQUARE 1
#define PEN_ROUND  2
#define PEN_SLASH  3
#define PEN_BACK   4
static int pen_kind, pen_size = 1;
static void disc(int cx, int cy, int d, u8 c)
{
    int j, i, yy, xx, first, last;
    long dd = (long)d * d;
    for (j = 0; j < d; j++) {
        yy = 2 * j - (d - 1);
        first = -1; last = -1;
        for (i = 0; i < d; i++) {
            xx = 2 * i - (d - 1);
            if ((long)xx * xx + (long)yy * yy <= dd + d / 2) { if (first < 0) first = i; last = i; }
        }
        if (first >= 0) hspan(cx - d / 2 + first, cx - d / 2 + last, cy - d / 2 + j, c);
    }
}
static void stamp(int x, int y, u8 c)
{
    int i, s = pen_size, h = s / 2;
    switch (pen_kind) {
    case PEN_DOT: plot(x, y, c); break;
    case PEN_SQUARE: fill_rect(x - h, y - h, x - h + s - 1, y - h + s - 1, c); break;
    case PEN_ROUND: if (s <= 2) fill_rect(x, y, x + s - 1, y + s - 1, c); else disc(x, y, s, c); break;
    case PEN_SLASH: for (i = 0; i < s; i++) hspan(x - h + i, x - h + i + 1, y + h - i, c); break;
    case PEN_BACK: for (i = 0; i < s; i++) hspan(x - h + i, x - h + i + 1, y - h + i, c); break;
    }
}
static void line(int x0, int y0, int x1, int y1, u8 c)
{
    int dx = iabs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -iabs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    for (;;) {
        stamp(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Shapes by rows: the span of row y of a rectangle, ellipse or rounded
 * rectangle filling the box x0..x1, y0..y1. */
#define SH_RECT    0
#define SH_ELLIPSE 1
#define SH_RRECT   2
static int shape_span(int kind, int x0, int y0, int x1, int y1, int r, int y, int *a, int *b)
{
    int w = x1 - x0 + 1, h = y1 - y0 + 1, j = y - y0, inset = 0;
    if (y < y0 || y > y1 || w <= 0 || h <= 0) return 0;
    if (kind == SH_ELLIPSE) {
        long v = 2L * j + 1 - h, s = isqrt((long)h * h - v * v), q = (long)w * s / h;
        inset = q >= w ? 0 : (int)((w - q) / 2);
    } else if (kind == SH_RRECT && r > 0) {
        int k = j < r ? j : (h - 1 - j < r ? h - 1 - j : -1);
        if (k >= 0) {
            long vv = 2L * (r - k) - 1, s = isqrt(4L * r * r - vv * vv);
            inset = (int)((2L * r - s + 1) / 2);
        }
    }
    *a = x0 + inset; *b = x1 - inset;
    return *a <= *b;
}
#define ST_OUTLINE 0
#define ST_BOTH    1
#define ST_SOLID   2
static int line_w = 1, fill_style;
/* The outline is the shape minus its erosion by a diamond of the line width,
 * so a 4-connected fill cannot leak through it. */
static void draw_shape(int kind, int x0, int y0, int x1, int y1, u8 cl, u8 cf)
{
    int y, a, b, c, d, t, j, r = 0, lw = line_w, ok;
    if (x0 > x1) { t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { t = y0; y0 = y1; y1 = t; }
    if (kind == SH_RRECT) r = imin(12, imin(x1 - x0 + 1, y1 - y0 + 1) / 2);
    for (y = imax(y0, 0); y <= imin(y1, cv.h - 1); y++) {
        if (!shape_span(kind, x0, y0, x1, y1, r, y, &a, &b)) continue;
        if (fill_style == ST_SOLID) { hspan(a, b, y, cl); continue; }
        c = a + lw; d = b - lw; ok = 1;
        for (j = 1; j <= lw && ok; j++) {
            int p, q;
            if (!shape_span(kind, x0, y0, x1, y1, r, y - j, &p, &q)) ok = 0;
            else { c = imax(c, p + lw - j); d = imin(d, q - lw + j); }
            if (!shape_span(kind, x0, y0, x1, y1, r, y + j, &p, &q)) ok = 0;
            else { c = imax(c, p + lw - j); d = imin(d, q - lw + j); }
        }
        if (ok && c <= d) {
            hspan(a, c - 1, y, cl);
            hspan(d + 1, b, y, cl);
            if (fill_style == ST_BOTH) hspan(c, d, y, cf);
        } else hspan(a, b, y, cl);
    }
}

/* Polygons: vertices, outline with the line pen, even-odd fill. */
#define MAXPT 64
static int ptx[MAXPT], pty[MAXPT], npt;
static void poly_fill(int n, u8 c)
{
    int y, i, j, k, m, ymin = 32767, ymax = -32767, xs[MAXPT];
    for (i = 0; i < n; i++) { ymin = imin(ymin, pty[i]); ymax = imax(ymax, pty[i]); }
    ymin = imax(ymin, 0); ymax = imin(ymax, cv.h - 1);
    for (y = ymin; y <= ymax; y++) {
        m = 0;
        for (i = 0; i < n; i++) {
            j = (i + 1) % n;
            if ((pty[i] <= y && pty[j] > y) || (pty[j] <= y && pty[i] > y))
                xs[m++] = ptx[i] + (int)((long)(y - pty[i]) * (ptx[j] - ptx[i]) / (pty[j] - pty[i]));
        }
        for (i = 1; i < m; i++) for (k = i; k > 0 && xs[k] < xs[k - 1]; k--) { j = xs[k]; xs[k] = xs[k - 1]; xs[k - 1] = j; }
        for (i = 0; i + 1 < m; i += 2) hspan(xs[i], xs[i + 1], y, c);
    }
}
static void set_line_pen(void)
{
    pen_kind = line_w <= 1 ? PEN_DOT : PEN_ROUND;
    pen_size = line_w;
}

/* Curves: cubic Bezier by halving (coordinates in eighths). */
static int fx8(long v) { return (int)((v + (v >= 0 ? 4 : -4)) / 8); }
static void bezier(long x0, long y0, long x1, long y1, long x2, long y2, long x3, long y3, int depth, u8 c)
{
    long ax, ay, bx, by, cx, cy, dx, dy, ex, ey, mx, my;
    if (!depth) { line(fx8(x0), fx8(y0), fx8(x3), fx8(y3), c); return; }
    ax = (x0 + x1) / 2; ay = (y0 + y1) / 2;
    bx = (x1 + x2) / 2; by = (y1 + y2) / 2;
    cx = (x2 + x3) / 2; cy = (y2 + y3) / 2;
    dx = (ax + bx) / 2; dy = (ay + by) / 2;
    ex = (bx + cx) / 2; ey = (by + cy) / 2;
    mx = (dx + ex) / 2; my = (dy + ey) / 2;
    bezier(x0, y0, ax, ay, dx, dy, mx, my, depth - 1, c);
    bezier(mx, my, ex, ey, cx, cy, x3, y3, depth - 1, c);
}

/* Four-connected scanline fill. Grow the pending stack rather than silently
 * losing areas when the legacy 8192-point stack was full. */
static void flood(int x, int y, u8 c)
{
    if (x < 0 || y < 0 || x >= cv.w || y >= cv.h || pixel(x, y) == c) return;
    size_t cap = 8192, n = 1;
    u32 *stack = malloc(cap * sizeof(*stack));
    if (!stack) { cancel_op(); no_memory(); return; }
    u8 target = pixel(x, y);
    stack[0] = (u32)y * (u32)cv.w + (u32)x;
    while (n) {
        u32 at = stack[--n]; y = (int)(at / (u32)cv.w); x = (int)(at % (u32)cv.w);
        fptr row = rowp(&cv, y);
        if (row[x] != target) continue;
        int left = x, right = x;
        while (left && row[left - 1] == target) left--;
        while (right + 1 < cv.w && row[right + 1] == target) right++;
        hspan(left, right, y, c);
        for (int yy = y - 1; yy <= y + 1; yy += 2) {
            if (yy < 0 || yy >= cv.h) continue;
            int in = 0; row = rowp(&cv, yy);
            for (int i = left; i <= right; i++) {
                if (row[i] == target) {
                    if (!in) {
                        if (n == cap) {
                            size_t next = cap * 2;
                            u32 *p = realloc(stack, next * sizeof(*stack));
                            if (!p) { free(stack); cancel_op(); no_memory(); return; }
                            stack = p; cap = next;
                        }
                        stack[n++] = (u32)yy * (u32)cv.w + (u32)i;
                    }
                    in = 1;
                } else in = 0;
            }
        }
    }
    free(stack);
}

static int air_size = 1;                     /* 0 small, 1 medium, 2 large */
static void spray(int x, int y, u8 c)
{
    int r = 4 + air_size * 4, n = r + 2, dx, dy;
    while (n--) {
        do { dx = rnd(2 * r + 1) - r; dy = rnd(2 * r + 1) - r; } while (dx * dx + dy * dy > r * r);
        plot(x + dx, y + dy, c);
    }
}
static void spray_line(int x0,int y0,int x1,int y1,u8 c)
{
    int dx=iabs(x1-x0),dy=iabs(y1-y0),sx=x0<x1?1:-1,sy=y0<y1?1:-1,err=dx-dy;
    for (;;) {
        spray(x0,y0,c);
        if (x0==x1 && y0==y1) break;
        int e2=2*err;
        if (e2>-dy) { err-=dy; x0+=sx; }
        if (e2<dx) { err+=dx; y0+=sy; }
    }
}

/* Original CFN glyphs, scale 1..4, regular/bold and opaque/transparent. */
#define TEXT_MAX 200
static struct du_textarea text_edit;
static char *text;
#define text_len ((int)text_edit.len)
static int text_on, tx, ty, text_scale = 1, text_bold, opaque = 1;
static u8 fnt[3135];
static int fnt_synth, fnt_loaded;
static char font_file[256];
static const char font_dir[] = "/system/fonts/";
static int font_load(void)
{
    fnt_loaded = 0; fnt_synth = 0;
    char p[DU_TEXT_MAX]; u8 h[64];
    snprintf(p, sizeof(p), "%s%s", font_dir, font_file[0] ? font_file : "CIUKIOS.CFN");
    FILE *f = fopen(p, "rb");
    if (f) {
        if (fread(h, 1, 64, f) == 64 && !memcmp(h, "CIUKFNT1", 8)) {
            unsigned flags = h[44] | ((unsigned)h[45] << 8);
            fnt_synth = text_bold && !(flags & 1);
            if (!fseek(f, 64 + (text_bold && (flags & 1) ? 3135 : 0), SEEK_SET))
                fnt_loaded = fread(fnt, 1, 3135, f) == 3135;
        }
        fclose(f);
    }
    if (!fnt_loaded) {
        memcpy(fnt, paint_default_font + (text_bold ? 3135 : 0), sizeof(fnt));
        fnt_synth = 0; fnt_loaded = 1;
    }
    return fnt_loaded;
}
static int glyph_w(char ch)
{
    if (ch < 32 || ch > 126) ch = '?';
    return fnt[(unsigned char)ch - 32] + fnt_synth;
}
static void text_extent(int *w, int *h, int *last_w)
{
    int i, lw = 0, lines = 1;
    *w = 0;
    for (i = 0; i < text_len; i++) {
        if (text[i] == '\n') { lines++; lw = 0; continue; }
        lw += glyph_w(text[i]);
        if (lw > *w) *w = lw;
    }
    *w *= text_scale;
    *h = lines * 16 * text_scale;
    *last_w = lw * text_scale;
}
static void text_position(int p, int *x, int *y)
{
    int i;
    *x = tx; *y = ty;
    for (i = 0; i < p; i++) {
        if (text[i] == '\n') { *x = tx; *y += 16 * text_scale; }
        else *x += glyph_w(text[i]) * text_scale;
    }
}
static int text_at(int x, int y)
{
    int p = 0, px = tx, py = ty, w;
    if (y < ty) return 0;
    while (p < text_len) {
        if (y < py + 16 * text_scale) {
            if (text[p] == '\n') return p;
            w = glyph_w(text[p]) * text_scale;
            if (x < px + (w + 1) / 2) return p;
            px += w;
        } else if (text[p] == '\n') { px = tx; py += 16 * text_scale; }
        p++;
    }
    return p;
}
static int text_contains(int x, int y)
{
    int w, h, last;
    text_extent(&w, &h, &last);
    return x >= tx && x < tx + imax(w, 8) && y >= ty && y < ty + h;
}
static void draw_glyph(int x, int y, char ch, u8 c)
{
    int r, col, start, k, s = text_scale, gw;
    u16 base, bits;
    if (ch < 32 || ch > 126) ch = '?';
    base = 95 + (u16)(ch - 32) * 32;
    gw = imin(16, glyph_w(ch));
    for (r = 0; r < 16; r++) {
        bits = (u16)(fnt[base + r * 2] | ((u16)fnt[base + r * 2 + 1] << 8));
        if (fnt_synth) bits |= bits >> 1;
        col = 0;
        while (col < gw) {
            if (!(bits & (0x8000u >> col))) { col++; continue; }
            start = col;
            while (col < gw && (bits & (0x8000u >> col))) col++;
            for (k = 0; k < s; k++) hspan(x + start * s, x + col * s - 1, y + r * s + k, c);
        }
    }
}
static void text_render(void)
{
    int i, x, y, w, h, lw;
    restore_all();
    if (!fnt_loaded) return;
    text_extent(&w, &h, &lw);
    if (opaque && w) fill_rect(tx, ty, tx + w - 1, ty + h - 1, bg);
    x = tx; y = ty;
    for (i = 0; i < text_len; i++) {
        if (text[i] == '\n') { x = tx; y += 16 * text_scale; continue; }
        draw_glyph(x, y, text[i], fg);
        x += glyph_w(text[i]) * text_scale;
    }
}
static void text_finish(void)
{
    if (!text_on) return;
    text_on = 0;
    if (!text_len) { cancel_op(); return; }
    text[text_len] = 0;
    commit_op();
}

/* The selection; a floating one is lifted into flt and drawn over the */
/* picture at each move (the preview of the operation that lifted it). */
static int sel_on, selx, sely, selw, selh, floating, hole, holex, holey, holew, holeh;
static struct paint_image selection_mask;
static int free_select;
#define LASSO_POINTS 256
static int lassox[LASSO_POINTS],lassoy[LASSO_POINTS],lasso_count;
#define MASKED_PIXEL 255u
static void lasso_point(int x,int y)
{
    x=imax(0,imin(cv.w-1,x)); y=imax(0,imin(cv.h-1,y));
    if (lasso_count && lassox[lasso_count-1]==x && lassoy[lasso_count-1]==y)return;
    if (lasso_count==LASSO_POINTS) {
        for (int i=0;i<LASSO_POINTS/2;i++) { lassox[i]=lassox[2*i]; lassoy[i]=lassoy[2*i]; }
        lasso_count=LASSO_POINTS/2;
    }
    lassox[lasso_count]=x; lassoy[lasso_count++]=y;
    int x0=x,x1=x,y0=y,y1=y;
    for (int i=0;i<lasso_count;i++) { x0=imin(x0,lassox[i]); x1=imax(x1,lassox[i]); y0=imin(y0,lassoy[i]); y1=imax(y1,lassoy[i]); }
    selx=x0; sely=y0; selw=x1-x0+1; selh=y1-y0+1;
}
static void lasso_segment(int x0,int y0,int x1,int y1)
{
    int dx=iabs(x1-x0),dy=iabs(y1-y0),sx=x0<x1?1:-1,sy=y0<y1?1:-1,err=dx-dy;
    for (;;) {
        lasso_point(x0,y0);
        if (x0==x1 && y0==y1) break;
        int e2=2*err;
        if (e2>-dy) { err-=dy; x0+=sx; }
        if (e2<dx) { err+=dx; y0+=sy; }
    }
}
static void lasso_finish(void)
{
    if (lasso_count<3) { sel_on=0; lasso_count=0; return; }
    if (!img_alloc(&selection_mask,selw,selh)) { no_memory(); sel_on=0; lasso_count=0; return; }
    memset(selection_mask.pixels,0,(size_t)selw*selh);
    int selected=0;
    for (int y=0;y<selh;y++) {
        int cuts[LASSO_POINTS],count=0,scan=2*(sely+y)+1;
        for (int i=0,j=lasso_count-1;i<lasso_count;j=i++) {
            int ya=2*lassoy[i],yb=2*lassoy[j];
            if ((ya>scan)==(yb>scan))continue;
            int cut=2*lassox[i]+(scan-ya)*(lassox[j]-lassox[i])/(lassoy[j]-lassoy[i]);
            int at=count++;
            while (at && cuts[at-1]>cut) { cuts[at]=cuts[at-1]; at--; }
            cuts[at]=cut;
        }
        for (int i=0;i+1<count;i+=2) {
            int left=imax(0,cuts[i]/2-selx),right=imin(selw,cuts[i+1]/2-selx);
            if (right>left) { memset(rowp(&selection_mask,y)+left,1,(size_t)(right-left)); selected=1; }
        }
    }
    lasso_count=0;
    if (!selected) { img_free(&selection_mask); sel_on=0; }
}
static u8 hole_c;
static void mark(int x0, int x1, int y)
{
    dmg_add(x0, y, x1, y);
    if (by1 < by0) { bx0 = x0; by0 = y; bx1 = x1; by1 = y; }
    else { bx0 = imin(bx0, x0); by0 = imin(by0, y); bx1 = imax(bx1, x1); by1 = imax(by1, y); }
}
static void sel_damage(void)
{
    if (sel_on) dmg_add(selx - 1, sely - 1, selx + selw, sely + selh);
}
static void float_render(void)
{
    int y, x, x0, x1, cy;
    fptr s, d;
    restore_all();
    if (hole) {
        if (!selection_mask.pixels) fill_rect(holex, holey, holex + holew - 1, holey + holeh - 1, hole_c);
        else for (y=0;y<holeh;y++) for (x=0;x<holew;x++)
            if (selection_mask.pixels[y*holew+x])plot(holex+x,holey+y,hole_c);
    }
    for (y = 0; y < flt.h; y++) {
        cy = sely + y;
        if (cy < 0 || cy >= cv.h) continue;
        x0 = imax(0, -selx); x1 = imin(flt.w, cv.w - selx);
        if (x0 >= x1 || !touch(cy)) continue;
        {
            s = rowp(&flt, y); d = rowp(&cv, cy);
            for (x = x0; x < x1; x++) if (s[x] != MASKED_PIXEL && (opaque || s[x] != bg)) d[selx + x] = s[x];
        }
        mark(selx + x0, selx + x1 - 1, cy);
    }
}
static void no_memory(void);
static int lift(int copy)
{
    int y;
    if (floating) return 1;
    if (!sel_on || selw <= 0 || selh <= 0) return 0;
    if (!alloc_retry(&flt, selw, selh)) { no_memory(); return 0; }
    for (y = 0; y < selh; y++)
        memcpy(rowp(&flt, y), rowp(&cv, sely + y) + selx, (size_t)selw);
    if (selection_mask.pixels) for (y=0;y<selh;y++) for (int x=0;x<selw;x++)
        if (!selection_mask.pixels[y*selw+x])rowp(&flt,y)[x]=MASKED_PIXEL;
    begin_op(1);
    final_render = float_render;
    floating = 1;
    hole = !copy; holex = selx; holey = sely; holew = selw; holeh = selh; hole_c = bg;
    float_render();
    return 1;
}
static void drop_selection(void)
{
    sel_damage();
    if (floating) {
        commit_op();
        img_free(&flt);
        floating = 0;
        }
    sel_on = 0;
    img_free(&selection_mask); lasso_count=0;
}
/* Copy the selection (or the floating pixels) to the clipboard. */
static int copy_selection(void)
{
    int y;
    struct paint_image *src = floating ? &flt : &cv;
    int x0 = floating ? 0 : selx, y0 = floating ? 0 : sely;
    if (!sel_on) return 0;
    img_free(&clip);
    if (!alloc_retry(&clip, selw, selh)) { no_memory(); return 0; }
    for (y = 0; y < selh; y++)
        memcpy(rowp(&clip, y), rowp(src, y0 + y) + x0, (size_t)selw);
    if (!floating && selection_mask.pixels) for (y=0;y<selh;y++) for (int x=0;x<selw;x++)
        if (!selection_mask.pixels[y*selw+x])rowp(&clip,y)[x]=MASKED_PIXEL;
    return 1;
}
static void clear_selection(void)
{
    if (!sel_on) return;
    sel_damage();
    if (floating) {
        img_free(&flt);
        flt.h = 0;
        float_render();                        /* the hole alone */
        floating = 0;
        commit_op();
    } else {
        begin_op(0);
        if (!selection_mask.pixels) fill_rect(selx, sely, selx + selw - 1, sely + selh - 1, bg);
        else for (int y=0;y<selh;y++) for (int x=0;x<selw;x++)
            if (selection_mask.pixels[y*selw+x])plot(selx+x,sely+y,bg);
        commit_op();
    }
    sel_on = 0;
    img_free(&selection_mask); lasso_count=0;
}

/* ------------------------------------------------------------------ */
/* Whole-picture changes                                               */
static u8 inv[NCOL];
static void make_inverse(void)
{
    int i;
    for (i = 0; i < NCOL; i++) inv[i] = nearest(63 - pal[i * 3], 63 - pal[i * 3 + 1], 63 - pal[i * 3 + 2]);
}
static int scx, scy;
/* A new canvas replaces the picture; the old one is the undo copy. */
static void replace_canvas(struct paint_image *n)
{
    old = cv;
    cv = *n;
    undo_kind = U_IMAGE; undone = 0; dirty = 1;
    sel_on = 0;
    scx = scy = 0;
}
static int make_room(void)
{
    if (op_open) return 0;
    drop_undo();
    pool_free();
    return 1;
}
/* 1 flip horizontal, 2 flip vertical, 3 rotate 90, 4 rotate 180, 5 rotate 270, 6 invert */
static void rotate_into(struct paint_image *src, struct paint_image *dst, int kind)
{
    int x, y;
    fptr d;
    for (y = 0; y < dst->h; y++) {
        d = rowp(dst, y);
        for (x = 0; x < dst->w; x++)
            d[x] = kind == 3 ? rowp(src, src->h - 1 - x)[y] : rowp(src, x)[src->w - 1 - y];
    }
}
static void transform_rows(struct paint_image *m, int kind, int canvas)
{
    int y, x, n;
    fptr a, b;
    u8 t;
    if (kind == 4) { transform_rows(m, 1, canvas); transform_rows(m, 2, canvas); return; }
    if (kind == 2) {
        for (y = 0; y < m->h / 2; y++) {
            if (canvas) { touch(y); touch(m->h - 1 - y); }
            a = rowp(m, y); b = rowp(m, m->h - 1 - y);
            for (x = 0; x < m->w; x++) { t = a[x]; a[x] = b[x]; b[x] = t; }
        }
        return;
    }
    for (y = 0; y < m->h; y++) {
        if (canvas && !touch(y)) continue;
        a = rowp(m, y);
        if (kind == 1) for (x = 0, n = m->w - 1; x < n; x++, n--) { t = a[x]; a[x] = a[n]; a[n] = t; }
        else for (x = 0; x < m->w; x++) a[x] = a[x] < NCOL ? inv[a[x]] : a[x];
    }
}
static void transform(int kind)
{
    struct paint_image n;
    if (kind == 6) make_inverse();
    if (sel_on) {
        if (!lift(0)) return;
        if (kind == 3 || kind == 5) {
            if (!img_alloc(&n, flt.h, flt.w)) { no_memory(); return; }
            rotate_into(&flt, &n, kind);
            img_free(&flt);
            flt = n;
            selx += (selw - flt.w) / 2; sely += (selh - flt.h) / 2;
            sel_damage();
            selw = flt.w; selh = flt.h;
        } else transform_rows(&flt, kind, 0);
        sel_damage();
        float_render();
        return;
    }
    if (kind == 3 || kind == 5) {
        if (op_open) return;
        if (!img_alloc(&n, cv.h, cv.w)) { no_memory(); return; }
        make_room();
        rotate_into(&cv, &n, kind);
        replace_canvas(&n);
        dmg_all();
        return;
    }
    begin_op(0);
    transform_rows(&cv, kind, 1);
    dmg_all();
    commit_op();
}
static int resize_canvas(int w, int h)
{
    struct paint_image n;
    int y;
    if (op_open) return 0;
    if (!img_alloc(&n, w, h)) { no_memory(); return 0; }
    make_room();
    img_fill(&n, bg);
    for (y = 0; y < imin(h, cv.h); y++) memcpy(rowp(&n, y), rowp(&cv, y), (size_t)imin(w, cv.w));
    replace_canvas(&n);
    return 1;
}

static const char *const tool_names[NTOOLS] = {
    "Select", "Eraser", "Fill", "Pick Colour", "Magnifier", "Pencil", "Brush", "Airbrush",
    "Text", "Line", "Curve", "Rectangle", "Polygon", "Ellipse", "Rounded Rectangle" };
static const char *const tool_hints[NTOOLS] = {
    "Drag to select a rectangle; drag inside it to move it (Ctrl: a copy).",
    "Erases to the background colour. Right button: only the foreground colour.",
    "Fills a closed area with the colour.",
    "Takes a colour from the picture.",
    "Left click: closer. Right click: farther.",
    "Draws freehand lines one pixel wide.",
    "Paints with the brush shape chosen below.",
    "Sprays the colour; hold the button to spray more.",
    "Click where the text goes, then type. Click again to place it.",
    "Draws a straight line with the width chosen below.",
    "Draw a line, then drag twice to bend it.",
    "Draws a rectangle in the style chosen below.",
    "Drag the first side, click each corner, double-click to close.",
    "Draws an ellipse in the style chosen below.",
    "Draws a rectangle with rounded corners." };
/* Tool box pictures, 16 x 16: . face  # ink  + shadow  o paper  p rose
 * y yellow  w brown  b blue  l light. */
static const char *const glyph_src[NTOOLS] = {
    /* select */
    "................"
    "................"
    "..##.##.##.###.."
    "..#..........#.."
    "................"
    "..#..........#.."
    "..#..........#.."
    "................"
    "..#..........#.."
    "..#..........#.."
    "................"
    "..#..........#.."
    "..#..........#.."
    "..##.##.##.##..."
    "................"
    "................",
    /* eraser */
    "................"
    "................"
    "................"
    "......#########."
    ".....#ooooooo#.."
    ".....#ooooooo#.."
    "....#ooooooo#..."
    "....#ooooooo#..."
    "...##########..."
    "...#ppppppp#...."
    "..##pppppp#....."
    "..#ppppppp#....."
    ".#########......"
    ".+++++++++......"
    "................"
    "................",
    /* fill */
    "................"
    ".........#..#..."
    "........#o##.#.."
    "......##oll#..#."
    ".....#ooll#l#..."
    "....#ollllll#..."
    "...bollllllll#.."
    "..b.#lllllllll#."
    "..b.#llllllll#.."
    ".b...#llllll#..."
    ".bb...#llll#...."
    ".bb....#ll#....."
    ".b.....#l#......"
    ".b......#......."
    "................"
    "................",
    /* pick */
    "................"
    "...........###.."
    "..........#####."
    "..........#####."
    ".........######."
    "........#o####.."
    ".......#ooo#...."
    "......#obo#....."
    ".....#obo#......"
    "....#obo#......."
    "...#obo#........"
    "....#o#........."
    "...#.#.........."
    "..b............."
    ".#.............."
    "................",
    /* zoom */
    "................"
    "....####........"
    "...#llll#......."
    "..#lollll#......"
    ".#lollllll#....."
    ".#lollllll#....."
    ".#llllllll#....."
    ".#llllllll#....."
    "..#llllll#......"
    "...#llll#.w....."
    "....####.#ww...."
    "..........#ww..."
    "...........#ww.."
    "............#ww."
    ".............#ww"
    "..............w.",
    /* pencil */
    "................"
    "................"
    "...........pp..."
    "..........#opp.."
    ".........#oyypp."
    "........#oyyyyp."
    ".......#oyyyy#.."
    "......#oyyyy#..."
    ".....#oyyyy#...."
    "....#oyyyy#....."
    "....##yyy#......"
    "...#ww#y#......."
    "...#ww##........"
    "..#w##.........."
    ".###............"
    "................",
    /* brush */
    "................"
    ".............w#."
    "............w#w."
    "...........w#w.."
    "..........w#w..."
    ".........w#w...."
    ".......#w#w....."
    "......#+#w......"
    ".....#+++#......"
    "....###+#......."
    "...#####........"
    "..#####........."
    "..####.........."
    ".####..........."
    ".#.............."
    "................",
    /* air */
    "................"
    "....b.....#....."
    "......b..####..."
    "..b......####..."
    ".....b...####..."
    ".b......######.."
    "...b..b.#o+++#.."
    "........#o+++#.."
    ".b..b...#o+++#.."
    "........#o+++#.."
    "........#o+++#.."
    "........#o+++#.."
    "........#o+++#.."
    "........#o+++#.."
    "........######.."
    "................",
    /* text */
    "................"
    "................"
    ".......##......."
    ".......###......"
    "......##.#......"
    "......##.##....."
    "......##.##....."
    ".....##...##...."
    ".....##...##...."
    "....########...."
    "....##.....##..."
    "....##.....##..."
    "...##.......##.."
    "..####.....####."
    "................"
    "................",
    /* line */
    "................"
    "................"
    ".............#.."
    "............#..."
    "...........#...."
    "..........#....."
    ".........#......"
    "........#......."
    ".......#........"
    "......#........."
    ".....#.........."
    "....#..........."
    "...#............"
    "..#............."
    "................"
    "................",
    /* curve */
    "................"
    "................"
    "................"
    ".............#.."
    ".............#.."
    ".............#.."
    "......##.....#.."
    ".....#..#....#.."
    "....#....#..#..."
    "....#.....##...."
    "...#............"
    "...#............"
    "..#............."
    "................"
    "................"
    "................",
    /* rect */
    "................"
    "................"
    "................"
    "..############.."
    "..#..........#.."
    "..#..........#.."
    "..#..........#.."
    "..#..........#.."
    "..#..........#.."
    "..#..........#.."
    "..#..........#.."
    "..#..........#.."
    "..############.."
    "................"
    "................"
    "................",
    /* poly */
    "................"
    "................"
    "........##......"
    "....####..#....."
    "..##.......#...."
    "...##......#...."
    ".....#......#..."
    "......#......#.."
    ".....#......#..."
    ".....#......#..."
    "....#......#...."
    "...##.....#....."
    ".....###..#....."
    "........##......"
    "................"
    "................",
    /* ellipse */
    "................"
    "................"
    "................"
    ".....######....."
    "...##......##..."
    "..#..........#.."
    ".#............#."
    ".#............#."
    ".#............#."
    ".#............#."
    "..#..........#.."
    "...##......##..."
    ".....######....."
    "................"
    "................"
    "................",
    /* rrect */
    "................"
    "................"
    "................"
    "...##########..."
    "..#..........#.."
    ".#............#."
    ".#............#."
    ".#............#."
    ".#............#."
    ".#............#."
    ".#............#."
    "..#..........#.."
    "...##########..."
    "................"
    "................"
    "................",
};
static int tool = T_PENCIL, prev_tool = T_PENCIL, tool_hot = -1;
static int eraser_size = 1, brush_kind = PEN_ROUND, brush_size = 1, zoom = 1;
static const int eraser_px[4] = { 4, 6, 8, 10 };
static const int brush_px[3] = { 8, 5, 2 };
static const int zooms[5] = { 1, 2, 4, 6, 8 };

/* Client coordinates: same 60px toolbox, 44px palette, 20px status. */
#define TB_W 60
#define CB_H 44
#define SB_H 20
#define OPT_W 52
#define OPT_H 80
static int show_tools = 1, show_colors = 1, show_status = 1, show_grid;
static int W, H, tby, optx, opty, cby, sby, vx, vy, vw, vh;
static int partial_layout;
static int ptr_x = -1, ptr_y;
static int page_w(void) { return imax(1, (vw - 8) / zoom); }
static int page_h(void) { return imax(1, (vh - 8) / zoom); }
static void clamp_scroll(void)
{
    scx = imax(0, imin(scx, cv.w - page_w()));
    scy = imax(0, imin(scy, cv.h - page_h()));
}
static int i2sx(int ix) { return vx + 4 + (ix - scx) * zoom; }
static int i2sy(int iy) { return vy + 4 + (iy - scy) * zoom; }
static int s2i(int d, int sc) { return sc + (d >= 0 ? d / zoom : -((zoom - 1 - d) / zoom)); }
static void to_img(int sx, int sy, int *x, int *y) { *x = s2i(sx - vx - 4, scx); *y = s2i(sy - vy - 4, scy); }
static void flush(void)
{
    if (dx1>=dx0 && dy1>=dy0) {
        struct du_rect r={vx+4+(dx0-scx)*zoom,vy+4+(dy0-scy)*zoom,
                          (dx1-dx0+1)*zoom,(dy1-dy0+1)*zoom};
        du_invalidate(ui,du_intersection(r,(struct du_rect){vx,vy,vw,vh}));
    }
    dx0=dy0=0; dx1=dy1=-1;
}
static void scroll_canvas(int old_x, int old_y)
{
    clamp_scroll(); int dx=(old_x-scx)*zoom,dy=(old_y-scy)*zoom;
    if (!dx && !dy) return;
    struct du_rect viewport={vx+4,vy+4,vw-4,vh-4};
    du_scroll(ui,viewport,dx,dy);
    /* The brush preview stays at the pointer while the image moves. */
    if (tool==T_BRUSH || tool==T_ERASER || tool==T_AIR) {
        int radius=20*zoom;
        struct du_rect r={ui->mouse_x-radius,ui->mouse_y-radius,2*radius+1,2*radius+1};
        du_invalidate(ui,du_intersection(viewport,r)); r.x+=dx;r.y+=dy;
        du_invalidate(ui,du_intersection(viewport,r));
    }
}

static char path[DU_TEXT_MAX], status_title[DESK_TITLE_MAX + 1];
static void update_title(void)
{
    const char *name = strrchr(path, '/'); name = name ? name + 1 : path;
    snprintf(status_title, sizeof(status_title), "%.70s - CiukPaint", path[0] ? name : "Untitled");
    for (char *p=status_title; *p; p++) if ((unsigned char)*p<32 || (unsigned char)*p>126) *p='?';
    if (title_cb) title_cb(callback_host, status_title);
}
static void message(const char *msg) { du_message(ui, 999, "CiukPaint", msg, DU_DLG_ICON_INFO, NULL, 0, 0, 0); }
static void no_memory(void) { message("There is not enough memory. Make the picture smaller or close other programs."); }

static int curve_stage, drag, drag_mode;
static void finish_pending(void);

static int new_image(int w, int h)
{
    struct paint_image next = {0};
    if (!img_alloc(&next, w, h)) { no_memory(); return 0; }
    finish_pending(); drop_selection(); drop_undo(); pool_free(); img_free(&cv);
    cv = next; img_fill(&cv, WHITE);
    image_revision++; path[0] = 0; dirty = 0; scx = scy = 0; update_title(); return 1;
}
static void set_zoom(int z, int cx, int cy)
{
    int i;
    if (z < 1) z = 1;
    if (z > 8) z = 8;
    for (i = 0; i < 5 && zooms[i] < z; i++) ;
    z = zooms[i < 5 ? i : 4];
    if (z == zoom) return;
    zoom = z;
    layout();
    scx = cx - page_w() / 2; scy = cy - page_h() / 2;
    clamp_scroll();

}
static int zoom_step(int dir)
{
    int i;
    for (i = 0; i < 5 && zooms[i] != zoom; i++) ;
    i += dir;
    return zooms[i < 0 ? 0 : i > 4 ? 4 : i];
}
static void select_tool(int t)
{
    if (t == tool) return;
    finish_pending();
    if (t != T_SELECT) drop_selection();
    if (tool != T_PICK && tool != T_ZOOM) prev_tool = tool;
    tool = t;
    status[0] = 0;
}

static void undo(int redo)
{
    if (!redo && op_open) {                 /* the operation in progress */
        if (text_on) { text_on = 0; }
        if (floating) { img_free(&flt); floating = 0; }
        npt = 0; curve_stage = 0;
        sel_on = 0;
        cancel_op();
        img_free(&selection_mask); lasso_count=0;
        dmg_all();
        return;
    }
    if (!undo_kind || undone != redo) return;
    sel_on = 0; img_free(&selection_mask); lasso_count=0;
    if (undo_kind == U_ROWS) swap_rows();
    else { struct paint_image t = old; old = cv; cv = t; clamp_scroll(); }
    undone = !redo;
    dirty = 1;
    dmg_all();
}
static void paste(void)
{
    int y;
    if (!clip.pixels) return;
    finish_pending();
    drop_selection();
    if (!alloc_retry(&flt, clip.w, clip.h)) { no_memory(); return; }
    for (y = 0; y < clip.h; y++) memcpy(rowp(&flt, y), rowp(&clip, y), (size_t)clip.w);
    begin_op(1);
    final_render = float_render;
    floating = 1; hole = 0;
    sel_on = 1; selx = scx; sely = scy; selw = clip.w; selh = clip.h;
    if (tool != T_SELECT) { prev_tool = tool; tool = T_SELECT; }
    float_render();
    sel_damage();

}

/* Pointer on the picture                                              */
#define DM_NONE   0
#define DM_FREE   1
#define DM_SHAPE  2
#define DM_NEWSEL 3
#define DM_MOVE   4
#define DM_CURVE  5
#define DM_POLY   6
static int ax, ay, lx, ly, offx, offy, drag_btn;
static int cvx[4], cvy[4], rbx, rby;         /* curve points; polygon rubber point */
static u8 c1, c2;
static uint64_t poly_tick;
static int shape_btn;             /* the button that started a curve or polygon */
static void set_tool_pen(void)
{
    pen_replace = -1;
    if (tool == T_PENCIL) { pen_kind = PEN_DOT; pen_size = 1; }
    else if (tool == T_BRUSH) { pen_kind = brush_kind; pen_size = brush_px[brush_size]; if (pen_size == 2 && brush_kind == PEN_ROUND) pen_kind = PEN_SQUARE; }
    else if (tool == T_ERASER) { pen_kind = PEN_SQUARE; pen_size = eraser_px[eraser_size]; if (drag_btn == 2) pen_replace = fg; }
    else set_line_pen();
}
static void constrain(int *x, int *y, int line45)
{
    int dx = *x - ax, dy = *y - ay, m;
    if (!(ui->modifiers & DU_SHIFT)) return;
    if (line45) {
        if (iabs(dx) > 2 * iabs(dy)) { *y = ay; return; }
        if (iabs(dy) > 2 * iabs(dx)) { *x = ax; return; }
    }
    m = imin(iabs(dx), iabs(dy));
    *x = ax + (dx < 0 ? -m : m);
    *y = ay + (dy < 0 ? -m : m);
}
static int fsx, fsy;                         /* the pointer of the last shape preview */
static void render_shape(int x, int y)
{
    fsx = x; fsy = y;
    restore_all();
    set_line_pen();
    constrain(&x, &y, tool == T_LINE);
    lx = x; ly = y;
    if (tool == T_LINE) line(ax, ay, x, y, c1);
    else draw_shape(tool == T_RECT ? SH_RECT : tool == T_ELLIPSE ? SH_ELLIPSE : SH_RRECT, ax, ay, x, y, c1, c2);
}
static void shape_again(void) { render_shape(fsx, fsy); }
static void render_curve(void)
{
    restore_all();
    set_line_pen();
    bezier(cvx[0] * 8L, cvy[0] * 8L, cvx[1] * 8L, cvy[1] * 8L, cvx[2] * 8L, cvy[2] * 8L,
           cvx[3] * 8L, cvy[3] * 8L, 6, c1);
}
static void render_poly(int close)
{
    int i;
    restore_all();
    set_line_pen();
    if (close) {
        if (fill_style != ST_OUTLINE) poly_fill(npt, fill_style == ST_SOLID ? c1 : c2);
        if (fill_style != ST_SOLID) for (i = 0; i < npt; i++) line(ptx[i], pty[i], ptx[(i + 1) % npt], pty[(i + 1) % npt], c1);
        return;
    }
    for (i = 0; i + 1 < npt; i++) line(ptx[i], pty[i], ptx[i + 1], pty[i + 1], c1);
    line(ptx[npt - 1], pty[npt - 1], rbx, rby, c1);
}
static void poly_closed(void) { render_poly(1); }
static void poly_finish(void)
{
    if (!npt) return;
    if (npt >= 2) {
        final_render = poly_closed;
        render_poly(1);
        commit_op();

    } else cancel_op();
    npt = 0;
}
static void curve_finish(void)
{
    if (!curve_stage) return;
    commit_op();
    curve_stage = 0;
}
static void finish_pending(void)
{
    text_finish();
    poly_finish();
    curve_finish();
}

static int inside_sel(int x, int y)
{
    if (!sel_on || x<selx || x>=selx+selw || y<sely || y>=sely+selh)return 0;
    if (floating)return rowp(&flt,y-sely)[x-selx]!=MASKED_PIXEL;
    return !selection_mask.pixels || rowp(&selection_mask,y-sely)[x-selx];
}
static void tool_down(int x, int y, int btn)
{
    u8 c;
    c1 = btn == 1 ? fg : bg; c2 = btn == 1 ? bg : fg;
    ax = lx = x; ay = ly = y;
    drag_btn = btn;
    drag_mode = DM_NONE;
    set_tool_pen();
    switch (tool) {
    case T_SELECT:
        if (inside_sel(x, y)) {
            if (!floating && !lift((ui->modifiers & DU_CTRL) != 0)) return;
            offx = x - selx; offy = y - sely;
            drag_mode = DM_MOVE;
        } else {
            drop_selection();
            x = imax(0, imin(x, cv.w - 1)); y = imax(0, imin(y, cv.h - 1));
            ax = x; ay = y;
            sel_on = 1; selx = x; sely = y; selw = selh = 1;
            if (free_select)lasso_point(x,y);
            drag_mode = DM_NEWSEL;
        }
        break;
    case T_ERASER:
        begin_op(0); stamp(x, y, bg); drag_mode = DM_FREE; break;
    case T_PENCIL: case T_BRUSH:
        begin_op(0); stamp(x, y, c1); drag_mode = DM_FREE; break;
    case T_AIR:
        begin_op(0); spray(x, y, c1); drag_mode = DM_FREE; break;
    case T_FILL:
        begin_op(0); flood(x, y, c1); commit_op();

        return;
    case T_PICK:
        if (x < 0 || y < 0 || x >= cv.w || y >= cv.h) return;
        c = pixel(x, y);
        if (btn == 1) fg = c; else bg = c;

        tool = prev_tool;

        return;
    case T_ZOOM:
        set_zoom(zoom_step(btn == 1 ? 1 : -1), x, y);
        tool = prev_tool;

        return;
    case T_TEXT:
        if (text_on) { text_finish(); return; }
        if (!fnt_loaded) font_load();
        begin_op(1);
        final_render = text_render;
        du_ta_set_text(&text_edit, "", 0);
        text_on = 1; tx = x; ty = y;

        return;
    case T_LINE: case T_RECT: case T_ELLIPSE: case T_RRECT:
        begin_op(1);
        final_render = shape_again;
        drag_mode = DM_SHAPE;
        render_shape(x, y);
        break;
    case T_CURVE:
        if (!curve_stage) {
            begin_op(1);
            final_render = render_curve;
            cvx[0] = cvx[1] = cvx[2] = cvx[3] = x; cvy[0] = cvy[1] = cvy[2] = cvy[3] = y;
            shape_btn = btn;
        } else {
            c1 = shape_btn == 1 ? fg : bg;
            cvx[curve_stage] = x; cvy[curve_stage] = y;
            if (curve_stage == 1) { cvx[2] = x; cvy[2] = y; }
        }
        drag_mode = DM_CURVE;
        render_curve();
        break;
    case T_POLY:
        if (!npt) {
            begin_op(1);
            ptx[0] = x; pty[0] = y; npt = 1; shape_btn = btn;
            rbx = x; rby = y;
            poly_tick = (ui->now / 1000000ull);
            render_poly(0);
            drag_mode = DM_POLY;
            break;
        }
        c1 = shape_btn == 1 ? fg : bg; c2 = shape_btn == 1 ? bg : fg;
        if (((ui->now / 1000000ull) - poly_tick < (unsigned)500 && iabs(x - rbx) <= 3 && iabs(y - rby) <= 3) ||
            (npt > 2 && iabs(x - ptx[0]) <= 3 && iabs(y - pty[0]) <= 3) || npt >= MAXPT - 1) {
            poly_finish();
            return;
        }
        poly_tick = (ui->now / 1000000ull);
        rbx = x; rby = y;
        render_poly(0);
        drag_mode = DM_POLY;
        break;
    }
    drag = btn;
}
static void tool_move(int x, int y)
{
    ptr_x = x; ptr_y = y;
    switch (drag_mode) {
    case DM_FREE:
        set_tool_pen();
        if (tool == T_AIR) spray_line(lx,ly,x,y,c1);
        else line(lx, ly, x, y, tool == T_ERASER ? bg : c1);
        lx = x; ly = y;
        break;
    case DM_SHAPE: render_shape(x, y); break;
    case DM_NEWSEL:
        sel_damage();
        x = imax(0, imin(x, cv.w - 1)); y = imax(0, imin(y, cv.h - 1));
        if (free_select) { lasso_segment(lx,ly,x,y); lx=x; ly=y; }
        else {
            selx = imin(ax, x); sely = imin(ay, y);
            selw = iabs(x - ax) + 1; selh = iabs(y - ay) + 1;
        }
        sel_damage();
        break;
    case DM_MOVE:
        if (x - offx == selx && y - offy == sely) break;
        sel_damage();
        selx = x - offx; sely = y - offy;
        sel_damage();
        float_render();
        break;
    case DM_CURVE:
        if (!curve_stage) { cvx[3] = cvx[2] = x; cvy[3] = cvy[2] = y; }
        else { cvx[curve_stage] = x; cvy[curve_stage] = y; if (curve_stage == 1) { cvx[2] = x; cvy[2] = y; } }
        render_curve();
        break;
    case DM_POLY: rbx = x; rby = y; render_poly(0); break;
    }
}
static void tool_up(int x, int y)
{
    int m = drag_mode;
    tool_move(x, y);
    drag = 0;
    drag_mode = DM_NONE;
    switch (m) {
    case DM_FREE: commit_op(); pen_replace = -1;  break;
    case DM_SHAPE: commit_op();  break;
    case DM_NEWSEL:
        if (free_select)lasso_finish();
        else if (selw < 2 && selh < 2) sel_on = 0;
        break;
    case DM_MOVE:

        break;
    case DM_CURVE:
        if (++curve_stage == 3) curve_finish();
        break;
    case DM_POLY:
        if (npt < MAXPT - 1 && (npt == 1 || rbx != ptx[npt - 1] || rby != pty[npt - 1])) { ptx[npt] = rbx; pty[npt] = rby; npt++; }
        break;
    }

}

/* Native menus. Scan codes match libdeskui's keymap/accelerator handling. */
#define ITEM(label, id, key, mod) {label, id, key, mod, 0}
#define SEP {"", 0, 0, 0, DU_ITEM_SEPARATOR}
static struct du_menu_item file_items[] = {
    ITEM("&New", M_NEW, 0x31, DU_CTRL), ITEM("&Open...", M_OPEN, 0x18, DU_CTRL),
    ITEM("&Save", M_SAVE, 0x1f, DU_CTRL), ITEM("Save &As...", M_SAVEAS, 0, 0), SEP,
    ITEM("E&xit", M_EXIT, DU_KEY_F4, DU_ALT) };
static struct du_menu_item edit_items[] = {
    ITEM("&Undo", M_UNDO, 0x2c, DU_CTRL), ITEM("&Repeat", M_REDO, 0x15, DU_CTRL), SEP,
    ITEM("Cu&t", M_CUT, 0x2d, DU_CTRL), ITEM("&Copy", M_COPY, 0x2e, DU_CTRL),
    ITEM("&Paste", M_PASTE, 0x2f, DU_CTRL), ITEM("C&lear Selection", M_CLEARSEL, DU_KEY_DELETE, 0),
    ITEM("Select &All", M_SELALL, 0x1e, DU_CTRL) };
static struct du_menu_item view_items[] = {
    ITEM("&Tool Box", M_TOOLBOX, 0x14, DU_CTRL), ITEM("&Colour Box", M_COLORBOX, 0x26, DU_CTRL),
    ITEM("&Status Bar", M_STATUS, 0, 0), SEP, ITEM("Zoom &In", M_ZOOMIN, DU_KEY_PGDN, DU_CTRL),
    ITEM("Zoom &Out", M_ZOOMOUT, DU_KEY_PGUP, DU_CTRL), ITEM("&1x (Actual Size)", M_Z1, 0, 0),
    ITEM("&2x", M_Z2, 0, 0), ITEM("&4x", M_Z4, 0, 0), ITEM("&6x", M_Z6, 0, 0), ITEM("&8x", M_Z8, 0, 0), SEP,
    ITEM("Show &Grid", M_GRID, 0x22, DU_CTRL), ITEM("Text &Font...", M_FONT, 0, 0) };
static struct du_menu_item image_items[] = {
    ITEM("&Flip/Rotate...", M_FLIP, 0x13, DU_CTRL), ITEM("&Invert Colours", M_INVERT, 0x17, DU_CTRL),
    ITEM("&Attributes...", M_ATTR, 0x12, DU_CTRL), ITEM("&Clear Image", M_CLEARIMG, 0, 0), SEP,
    ITEM("&Draw Opaque", M_OPAQUE, 0, 0) };
static struct du_menu_item help_items[] = {ITEM("&Help Topics", M_HELP, DU_KEY_F1, 0), SEP, ITEM("&About CiukPaint", M_ABOUT, 0, 0)};
#define MENU(label, items) {label, items, sizeof(items) / sizeof(items[0])}
static struct du_menu menus[] = {MENU("&File", file_items), MENU("&Edit", edit_items), MENU("&View", view_items), MENU("&Image", image_items), MENU("&Help", help_items)};
static struct du_menu_item context_items[] = {
    ITEM("Cu&t", M_CUT, 0, 0), ITEM("&Copy", M_COPY, 0, 0), ITEM("&Paste", M_PASTE, 0, 0),
    ITEM("C&lear Selection", M_CLEARSEL, 0, 0), ITEM("Select &All", M_SELALL, 0, 0), SEP,
    ITEM("&Flip/Rotate...", M_FLIP, 0, 0), ITEM("&Invert Colours", M_INVERT, 0, 0) };
static void flag(struct du_menu_item *m, unsigned bit, int yes) { if (yes) m->flags |= bit; else m->flags &= ~bit; }
static void update_menus(void)
{
    flag(&edit_items[0], DU_ITEM_DISABLED, !(op_open || (undo_kind && !undone)));
    flag(&edit_items[1], DU_ITEM_DISABLED, !(undo_kind && undone && !op_open));
    for (int i = 3; i <= 6; i++) flag(&edit_items[i], DU_ITEM_DISABLED,
        i == 5 ? (text_on ? !du_clipboard_get()[0] : !clip.pixels) : (text_on ? !du_ta_has_selection(&text_edit) : !sel_on));
    for (int i = 0; i < 5; i++) context_items[i].flags = edit_items[i + 3].flags;
    flag(&view_items[0], DU_ITEM_CHECKED, show_tools); flag(&view_items[1], DU_ITEM_CHECKED, show_colors);
    flag(&view_items[2], DU_ITEM_CHECKED, show_status);
    for (int i = 0; i < 5; i++) flag(&view_items[6 + i], DU_ITEM_CHECKED, zoom == zooms[i]);
    flag(&view_items[12], DU_ITEM_CHECKED, show_grid); flag(&view_items[12], DU_ITEM_DISABLED, zoom < 4);
    flag(&image_items[5], DU_ITEM_CHECKED, opaque);
}
static void context_menu(int x, int y) { update_menus(); du_popup(ui, context_items, 8, x, y, P_CANVAS); }

static void layout(void)
{
    W = ui->canvas.width; H = ui->canvas.height;
    int top = 20, bottom = H;
    sby = bottom - SB_H; if (show_status) bottom = sby;
    cby = bottom - CB_H; if (show_colors) bottom = cby;
    tby = top; optx = 4; opty = tby + 4 + 8 * 26 + 4;
    vx = (show_tools ? TB_W : 0) + 1; vy = top + 1;
    vw = imax(8, W - 16 - vx); vh = imax(8, bottom - 16 - vy);
    clamp_scroll();
    struct du_widget *canvas=du_get(ui,P_CANVAS);
    canvas->box = (struct du_rect){vx, vy, vw, vh};
    static const unsigned cursors[NTOOLS] = {
        DESK_CURSOR_CROSSHAIR, DESK_CURSOR_ERASER, DESK_CURSOR_FILL, DESK_CURSOR_PICK,
        DESK_CURSOR_ZOOM, DESK_CURSOR_PENCIL, DESK_CURSOR_BRUSH, DESK_CURSOR_PRECISION,
        DESK_CURSOR_TEXT, DESK_CURSOR_CROSSHAIR, DESK_CURSOR_CROSSHAIR, DESK_CURSOR_CROSSHAIR,
        DESK_CURSOR_CROSSHAIR, DESK_CURSOR_CROSSHAIR, DESK_CURSOR_CROSSHAIR
    };
    canvas->pointer_cursor=cursors[tool];
    canvas->cursor_box=(struct du_rect){vx+4,vy+4,imax(0,imin(cv.w-scx,(vw-4)/zoom))*zoom,
        imax(0,imin(cv.h-scy,(vh-4)/zoom))*zoom};
    struct du_widget *hs = du_get(ui, P_HSCROLL), *vs = du_get(ui, P_VSCROLL);
    hs->box = (struct du_rect){vx, vy + vh, vw, 16}; hs->vertical = 0;
    hs->total = cv.w; hs->page = page_w(); hs->value = scx;
    vs->box = (struct du_rect){vx + vw, vy, 16, vh}; vs->vertical = 1;
    vs->total = cv.h; vs->page = page_h(); vs->value = scy;
    struct du_widget *st = du_get(ui, P_STATUS);
    st->box = (struct du_rect){0, sby, W, SB_H}; st->hidden = !show_status;
    st->panels[0] = imax(1, W - 212); st->panels[1] = 104;
    char hint[100], position[32] = "", size[40];
    snprintf(hint, sizeof(hint), "%s", status[0] ? status : tool==T_SELECT && free_select ?
             "Drag to select a free-form area; drag inside it to move (Ctrl: copy)." : tool_hints[tool]);
    if (ptr_x >= 0) snprintf(position, sizeof(position), "%d, %d", ptr_x, ptr_y);
    snprintf(size, sizeof(size), "%d x %d", sel_on ? selw : cv.w, sel_on ? selh : cv.h);
    char status_text[sizeof(st->text)];
    snprintf(status_text,sizeof(status_text),"%s\t%s\t%s",hint,position,size);
    if (strcmp(st->text,status_text)) { du_set_text(st,status_text); if (!st->hidden) du_invalidate(ui,st->box); }
    for (int i = 0; i < NTOOLS; i++) {
        struct du_widget *w = du_get(ui, P_TOOL + i);
        w->hidden = !show_tools; w->box = (struct du_rect){4 + (i & 1) * 26, top + 4 + (i >> 1) * 26, 25, 25};
    }
    struct du_widget *opt = du_get(ui, P_OPTIONS);
    opt->hidden = !show_tools; opt->box = (struct du_rect){optx, opty, OPT_W, OPT_H};
    for (int i = 0; i < NSWATCH; i++) {
        struct du_widget *w = du_get(ui, P_SWATCH + i);
        w->hidden = !show_colors; w->box = (struct du_rect){50 + (i % 14) * 17, cby + 6 + (i / 14) * 18, 16, 16};
    }
    update_menus();
    if (!partial_layout) ui->dirty=1;
    flush();
}
static void relayout(struct du_ui *u, void *arg) { (void)u; (void)arg; layout(); }

/* Preserve the original option and glyph drawings, using native theme roles. */
#define C_FACE DU_FACE
#define C_PAPER DU_WINDOW
#define C_SHADOW DU_SHADOW
#define C_INK DU_INK
#define C_TITLE DU_SELECT
#define C_LIGHT DU_LIGHT
#define C_RED 0xa33e3au
#define C_YELLOW 0xeac263u
#define C_BLUE 0x638bbau
#define C_BROWN 0xa87442u
#define C_GREEN 0x4d8659u
#define C_CYAN 0x69b2b2u
#define C_ROSE 0xbc888fu
static void ui_rect(int x, int y, int w, int h, uint32_t color) { du_fill(&ui->canvas, (struct du_rect){x,y,w,h}, color); }
static void ui_text(int x, int y, const char *str, uint32_t color) { du_text(&ui->canvas, x, y, str, color); }
static int ui_measure(const char *str) { return du_text_width(str); }
static void sunken(int x, int y, int w, int h, uint32_t fill)
{ du_fill(&ui->canvas, (struct du_rect){x,y,w,h}, fill); du_edge(&ui->canvas, (struct du_rect){x,y,w,h}, DU_SUNKEN_THIN); }

static const int brush_kinds[4] = {PEN_ROUND, PEN_SQUARE, PEN_SLASH, PEN_BACK};
static void opt_row(int i, int rh, int on)   /* highlight of an option row */
{
    if (on) ui_rect(optx + 2, opty + 2 + i * rh, OPT_W - 4, rh, C_TITLE);
}
static void paint_options(void)
{
    int ox = optx + 2, oy = opty + 2, i, k, on; uint32_t c;
    sunken(optx, opty, OPT_W, OPT_H, C_FACE);
    switch (tool) {
    case T_LINE: case T_CURVE:
        for (i = 0; i < 5; i++) {
            on = line_w == i + 1;
            opt_row(i, 15, on);
            ui_rect(ox + 6, oy + i * 15 + 7 - i / 2, 36, i + 1, on ? C_PAPER : C_INK);
        }
        break;
    case T_RECT: case T_POLY: case T_ELLIPSE: case T_RRECT:
        for (i = 0; i < 3; i++) {
            on = fill_style == i;
            opt_row(i, 25, on);
            c = on ? C_PAPER : C_INK;
            if (i < 2) {
                ui_rect(ox + 7, oy + i * 25 + 5, 34, 15, c);
                ui_rect(ox + 8, oy + i * 25 + 6, 32, 13, i ? C_SHADOW : (on ? C_TITLE : C_FACE));
            } else ui_rect(ox + 7, oy + i * 25 + 5, 34, 15, C_SHADOW);
        }
        break;
    case T_ERASER:
        for (i = 0; i < 4; i++) {
            on = eraser_size == i;
            opt_row(i, 19, on);
            k = eraser_px[i];
            ui_rect(ox + 24 - k / 2, oy + i * 19 + 9 - k / 2, k, k, on ? C_PAPER : C_INK);
        }
        break;
    case T_ZOOM:
        for (i = 0; i < 5; i++) {
            char t[4];
            on = zoom == zooms[i];
            opt_row(i, 15, on);
            fmt_u32(t, (u32)zooms[i]); strcat(t, "x");
            ui_text(ox + 24 - ui_measure(t) / 2, oy + i * 15 - 1, t, on ? C_PAPER : C_INK);
        }
        break;
    case T_BRUSH:
        for (i = 0; i < 12; i++) {
            int cx = ox + (i % 3) * 16 + 8, cy = oy + (i / 3) * 19 + 9, kind = brush_kinds[i / 3], s = brush_px[i % 3];
            on = brush_kind == kind && brush_size == i % 3;
            if (on) ui_rect(cx - 8, cy - 9, 16, 19, C_TITLE);
            c = on ? C_PAPER : C_INK;
            if (kind == PEN_ROUND) { ui_rect(cx - s / 2 + 1, cy - s / 2, imax(1, s - 2), s, c); ui_rect(cx - s / 2, cy - s / 2 + 1, s, imax(1, s - 2), c); }
            else if (kind == PEN_SQUARE) ui_rect(cx - s / 2, cy - s / 2, s, s, c);
            else for (k = 0; k < s; k++) ui_rect(cx - s / 2 + k, kind == PEN_SLASH ? cy + s / 2 - k : cy - s / 2 + k, 1, 1, c);
        }
        break;
    case T_AIR:
        for (i = 0; i < 3; i++) {
            static const signed char dots[24] = { 0,0, 2,1, -2,2, 1,-3, -3,-1, 3,3, -1,4, 4,-2, -4,3, 2,-4, -2,-4, 4,1 };
            int r = 2 + i * 2;
            on = air_size == i;
            opt_row(i, 25, on);
            for (k = 0; k < 12; k++) ui_rect(ox + 24 + dots[k * 2] * r / 4, oy + i * 25 + 12 + dots[k * 2 + 1] * r / 4, 1, 1, on ? C_PAPER : C_INK);
        }
        break;
    case T_SELECT:
        for (i=0;i<2;i++) {
            on=free_select==i; opt_row(i,18,on);
            ui_text(ox+3,oy+i*18+4,i?"Free":"Rect",on?C_PAPER:C_INK);
        }
        for (i=0;i<2;i++) {
            on=opaque==!i; if(on)ui_rect(ox,oy+36+i*20,OPT_W-4,20,C_TITLE);
            ui_text(ox+3,oy+40+i*20,i?"Clear":"Opaque",on?C_PAPER:C_INK);
        }
        break;
    case T_TEXT:
        for (i = 0; i < 2; i++) {
            on = opaque == !i;
            opt_row(i, 38, on);
            if (!i) ui_rect(ox + 6, oy + 7, 36, 24, C_PAPER);
            ui_rect(ox + 10, oy + i * 38 + 11, 14, 12, C_RED);
            ui_rect(ox + 20, oy + i * 38 + 16, 16, 11, C_BLUE);
        }
        break;
    }
}

static uint32_t glyph_color(char ch)
{
    switch (ch) {
    case '#': return C_INK;
    case '+': return C_SHADOW;
    case 'o': return C_PAPER;
    case 'r': return C_RED;
    case 'y': return C_YELLOW;
    case 'b': return C_BLUE;
    case 'w': return C_BROWN;
    case 'g': return C_GREEN;
    case 'c': return C_CYAN;
    case 'l': return C_LIGHT;
    case 'p': return C_ROSE;
    }
    return C_FACE;
}
static uint32_t rgb(u8 c)
{
    unsigned r = pal[c * 3], g = pal[c * 3 + 1], b = pal[c * 3 + 2];
    return (((r << 2) | (r >> 4)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 2) | (b >> 4));
}
static void marquee(int x, int y, int w, int h)
{
    struct du_rect r = {x,y,w,h}; du_outline(&ui->canvas, r, DU_WINDOW); du_dotted(&ui->canvas, r);
}
static int selection_member(int x,int y)
{
    if(x<0 || y<0 || x>=selw || y>=selh)return 0;
    return floating?rowp(&flt,y)[x]!=MASKED_PIXEL:selection_mask.pixels?rowp(&selection_mask,y)[x]!=0:1;
}
static void selection_outline(void)
{
    if(!sel_on)return;
    if(lasso_count) {
        for(int i=1;i<lasso_count;i++) {
            int x=i2sx(lassox[i-1]),y=i2sy(lassoy[i-1]),xx=i2sx(lassox[i]),yy=i2sy(lassoy[i]);
            int dx=iabs(xx-x),sx=x<xx?1:-1,dy=-iabs(yy-y),sy=y<yy?1:-1,err=dx+dy;
            for(;;) {
                ui_rect(x,y,1,1,(x+y)&1?DU_INK:DU_WINDOW);
                if(x==xx && y==yy)break;
                int e=2*err; if(e>=dy) { err+=dy; x+=sx; } if(e<=dx) { err+=dx; y+=sy; }
            }
        }
        return;
    }
    if(!selection_mask.pixels) { marquee(i2sx(selx)-1,i2sy(sely)-1,selw*zoom+2,selh*zoom+2); return; }
    for(int y=imax(0,scy-sely);y<imin(selh,scy-sely+(vh-4)/zoom);y++)
        for(int x=imax(0,scx-selx);x<imin(selw,scx-selx+(vw-4)/zoom);x++) {
            if(!selection_member(x,y))continue;
            int sx=i2sx(selx+x),sy=i2sy(sely+y); uint32_t ink=(x+y)&1?DU_INK:DU_WINDOW;
            if(!selection_member(x-1,y))ui_rect(sx,sy,1,zoom,ink);
            if(!selection_member(x+1,y))ui_rect(sx+zoom-1,sy,1,zoom,ink);
            if(!selection_member(x,y-1))ui_rect(sx,sy,zoom,1,ink);
            if(!selection_member(x,y+1))ui_rect(sx,sy+zoom-1,zoom,1,ink);
        }
}
static int footprint_member(int kind,int size,int x,int y)
{
    int h=size/2;
    if (kind==PEN_SLASH) {
        int i=h-y;
        return i>=0 && i<size && x>=-h+i && x<=-h+i+1;
    }
    if (kind==PEN_BACK) {
        int i=y+h;
        return i>=0 && i<size && x>=-h+i && x<=-h+i+1;
    }
    if (x < -h || y < -h || x >= size-h || y >= size-h) return 0;
    if (kind!=PEN_ROUND) return 1;
    int xx=2*(x+h)-(size-1), yy=2*(y+h)-(size-1);
    return xx*xx+yy*yy<=size*size+size/2;
}
static void footprint(void)
{
    struct du_widget *w=du_get(ui,P_CANVAS);
    if (!ui->focused_window || ui->dialog.kind || ui->menu>=0 || ui->popup ||
        !du_contains(w->cursor_box,ui->mouse_x,ui->mouse_y)) return;
    if (tool!=T_BRUSH && tool!=T_ERASER && tool!=T_AIR) return;
    int cx,cy; to_img(ui->mouse_x,ui->mouse_y,&cx,&cy);
    int size=tool==T_ERASER?eraser_px[eraser_size]:brush_px[brush_size];
    int kind=tool==T_ERASER?PEN_SQUARE:brush_kind;
    if (size==2 && kind==PEN_ROUND)kind=PEN_SQUARE;
    int radius=4+air_size*4,extent=tool==T_AIR?radius+1:size+1;
    struct du_canvas *c=&ui->canvas; struct du_rect saved=c->clip;
    c->clip=du_intersection(saved,w->cursor_box);
    for (int y=-extent; y<=extent; y++) for (int x=-extent; x<=extent; x++) {
        int inside=tool==T_AIR?x*x+y*y<=radius*radius:footprint_member(kind,size,x,y);
        if (!inside)continue;
        static const int dx[4]={-1,1,0,0},dy[4]={0,0,-1,1};
        int border[4];
        for (int edge=0; edge<4; edge++) {
            int nx=x+dx[edge],ny=y+dy[edge];
            border[edge]=!(tool==T_AIR?nx*nx+ny*ny<=radius*radius:footprint_member(kind,size,nx,ny));
        }
        for(int yy=0;yy<zoom;yy++)for(int xx=0;xx<zoom;xx++)
            if((border[0] && !xx) || (border[1] && xx==zoom-1) || (border[2] && !yy) || (border[3] && yy==zoom-1)) {
                int sx=i2sx(cx+x)+xx,sy=i2sy(cy+y)+yy;
                if(du_contains(c->clip,sx,sy))c->pixels[(size_t)sy*c->width+sx]^=0xffffffu;
            }
    }
    c->clip=saved;
}
static void render(struct du_ui *u, void *arg)
{
    (void)arg; struct du_canvas *c = &u->canvas; struct du_rect saved = c->clip;
    c->clip = du_intersection(saved, (struct du_rect){vx,vy,vw,vh});
    du_fill(c, c->clip, DU_SHADOW);
    int cols = imax(0, imin(cv.w - scx, (vw - 4) / zoom));
    int rows = imax(0, imin(cv.h - scy, (vh - 4) / zoom));
    /* DIB-style clipped row writes. No per-pixel toolkit calls and no
     * palette conversion outside the update region. */
    struct du_rect image=du_intersection(c->clip,(struct du_rect){vx+4,vy+4,cols*zoom,rows*zoom});
    for (int y=image.y;y<image.y+image.h;y++) {
        uint32_t *dst=c->pixels+(size_t)y*c->width;
        int sy=scy+(y-vy-4)/zoom;
        for (int x=image.x;x<image.x+image.w;x++) dst[x]=rgb(pixel(scx+(x-vx-4)/zoom,sy));
    }
    if (show_grid && zoom >= 4) {
        for (int x = 1; x < cols; x++) ui_rect(vx + 4 + x * zoom, vy + 4, 1, rows * zoom, DU_SHADOW);
        for (int y = 1; y < rows; y++) ui_rect(vx + 4, vy + 4 + y * zoom, cols * zoom, 1, DU_SHADOW);
    }
    selection_outline();
    if (text_on) {
        int w,h,last,cx,cy; text_extent(&w,&h,&last);
        marquee(i2sx(tx) - 2, i2sy(ty) - 2, imax(w,8) * zoom + 4, h * zoom + 4);
        size_t a,b; du_ta_selection(&text_edit,&a,&b);
        int px = tx, py = ty;
        for (int i = 0; i < text_len; i++) {
            int cell = text[i] == '\n' ? 4 : glyph_w(text[i]);
            if ((size_t)i >= a && (size_t)i < b) {
                ui_rect(i2sx(px),i2sy(py),cell * text_scale * zoom,16 * text_scale * zoom,DU_SELECT);
                if (text[i] != '\n') {
                    unsigned char ch = (unsigned char)text[i]; if (ch < 32 || ch > 126) ch = '?';
                    int base = 95 + (ch - 32) * 32;
                    for (int r = 0; r < 16; r++) {
                        unsigned bits = fnt[base + r * 2] | ((unsigned)fnt[base + r * 2 + 1] << 8);
                        if (fnt_synth) bits |= bits >> 1;
                        for (int col = 0; col < imin(16,cell); col++) if (bits & (0x8000u >> col))
                            ui_rect(i2sx(px + col * text_scale),i2sy(py + r * text_scale),text_scale * zoom,text_scale * zoom,DU_SELECT_INK);
                    }
                }
            }
            if (text[i] == '\n') { px = tx; py += 16 * text_scale; } else px += cell * text_scale;
        }
        if (!du_ta_has_selection(&text_edit) && u->focused_window && !u->dialog.kind && du_focused(u)==P_CANVAS) {
            text_position((int)text_edit.cursor,&cx,&cy);
            ui_rect(i2sx(cx), i2sy(cy), imax(1,zoom), 16 * text_scale * zoom, DU_INK);
        }
    }
    footprint();
    c->clip = saved;
    du_outline(c, (struct du_rect){vx - 1,vy - 1,vw + 17,vh + 17}, DU_SHADOW);
    if (show_tools && saved.x<TB_W && saved.x+saved.w>0 && saved.y<imax(vy+vh+17,opty+OPT_H) && saved.y+saved.h>tby) {
        du_fill(c, (struct du_rect){0, tby, TB_W, vh + 17}, DU_FACE);
        for (int i = 0; i < NTOOLS; i++) {
            struct du_widget *w = du_get(u,P_TOOL+i); int x=w->box.x,y=w->box.y;
            if (i == tool) sunken(x,y,25,25,DU_FACE);
            else if (i == tool_hot) du_raised(c,w->box,0,1);
            for (int k=0;k<256;k++) if (glyph_src[i][k] != '.')
                ui_rect(x+4+(i==tool)+k%16,y+4+(i==tool)+k/16,1,1,glyph_color(glyph_src[i][k]));
        }
        paint_options();
    }
    if (show_colors && saved.y<cby+CB_H && saved.y+saved.h>cby) {
        ui_rect(0,cby,W,CB_H,DU_FACE); ui_rect(0,cby,W,1,DU_HILITE);
        sunken(6,cby+5,36,35,DU_FACE);
        ui_rect(19,cby+17,18,18,DU_INK); ui_rect(20,cby+18,16,16,rgb(bg));
        ui_rect(11,cby+10,18,18,DU_INK); ui_rect(12,cby+11,16,16,rgb(fg));
        for (int i=0;i<NSWATCH;i++) {
            struct du_rect r=du_get(u,P_SWATCH+i)->box;
            sunken(r.x,r.y,16,16,DU_FACE); ui_rect(r.x+2,r.y+2,13,13,rgb(swatch[i]));
        }
    }
}

/* Modal controls on libdeskui. The accept hook reads values before release. */
enum { D_CHANGES=500, D_OPEN, D_SAVE, D_OVERWRITE, D_FORMAT, D_ATTR, D_FLIP, D_FONT };
enum { A_NONE, A_NEW, A_OPEN, A_EXIT, A_DROP };
static char dropped_path[DU_TEXT_MAX];   /* fx-19: a picture dropped on the window */
static int pending, closing, candidate_w, candidate_h, candidate_transform;
static enum paint_format save_format = PAINT_BMP8, candidate_format;
static char pending_path[DU_TEXT_MAX];
static struct du_item font_rows[32];
static char font_files[32][256], font_names[32][40];
static unsigned nfonts, candidate_font;
static int candidate_scale, candidate_bold;
static void file_dialog(int save);
static void run_pending(void);
static void error_message(int r)
{
    if (r == -ENOMEM) no_memory();
    else if (r == -EFBIG) message("This picture is too large. Maximum: 2048 x 1200 pixels (either orientation).");
    else if (r == -EINVAL) message("Cannot read this picture. CiukPaint opens BMP and PNG files.");
    else if (r) message(strerror(-r));
}
static void custom_dialog(int id, const char *title, int w, int h)
{
    static const char *const buttons[] = {"OK", "Cancel"};
    du_message(ui,id,title,"",0,buttons,2,0,1);
    struct du_dialog *d=&ui->dialog;
    w=imin(w,ui->canvas.width-8); h=imin(h,ui->canvas.height-8);
    d->box=(struct du_rect){(ui->canvas.width-w)/2,(ui->canvas.height-h)/2,w,h};
    d->items[0].box=(struct du_rect){d->box.x+w-174,d->box.y+h-34,75,23};
    d->items[1].box=(struct du_rect){d->box.x+w-93,d->box.y+h-34,75,23};
    d->focus=2;
}
static struct du_widget *control(unsigned index, enum du_kind kind, const char *label, int x, int y, int w, int h)
{
    struct du_dialog *d=&ui->dialog; struct du_widget *it=&d->items[index];
    memset(it,0,sizeof(*it)); it->kind=kind; it->id=-8000-(int)index; it->total=1;
    it->box=(struct du_rect){d->box.x+x,d->box.y+y,w,h}; du_set_text(it,label);
    if (index>=d->count) d->count=index+1;
    return it;
}
static int number(const char *s, int max)
{
    char *end; long value=strtol(s,&end,10);
    return *s && !*end && value>0 && value<=max ? (int)value : 0;
}
static int accept_controls(struct du_ui *u, int button, void *arg)
{
    (void)arg; struct du_dialog *d=&u->dialog;
    if (button == d->cancel) return 0;
    if (d->id == D_ATTR) {
        if (button==2) { du_set_text(&d->items[3],"512"); du_set_text(&d->items[5],"384"); return 1; }
        candidate_w=number(d->items[3].text,MAX_W); candidate_h=number(d->items[5].text,MAX_H);
        if (!candidate_w || !candidate_h) {
            du_set_text(&d->items[6],"Width: 1-2048. Height: 1-1200."); return 1;
        }
    } else if (d->id==D_FLIP) {
        for (int i=0;i<5;i++) if (d->items[2+i].value) candidate_transform=i+1;
    } else if (d->id==D_FORMAT) {
        for (int i=0;i<3;i++) if (d->items[2+i].value) candidate_format=(enum paint_format)i;
    } else if (d->id==D_FONT) {
        candidate_font=d->items[2].active; candidate_scale=number(d->items[4].text,4);
        candidate_bold=d->items[5].value;
        if (!candidate_scale) { du_set_text(&d->items[3],"Scale (1-4):"); return 1; }
    }
    return 0;
}
static void attributes_dialog(void)
{
    /* Default is a third button, matching the original Attributes panel. */
    custom_dialog(D_ATTR,"Attributes",400,210);
    struct du_dialog *d=&ui->dialog;
    d->items[2]=d->items[0]; d->items[2].id=-1002; du_set_text(&d->items[2],"&Default");
    d->items[0].box.x-=80; d->items[1].box.x-=80;
    d->items[2].box.x=d->box.x+d->box.w-93;
    d->buttons=3; d->cancel=1;
    /* Put the third button last among button slots, fields start at 3. */
    char width[12],height[12]; snprintf(width,sizeof(width),"%d",cv.w); snprintf(height,sizeof(height),"%d",cv.h);
    control(3,DU_EDIT,width,92,60,70,22); control(4,DU_LABEL,"&Height:",174,60,56,22);
    control(5,DU_EDIT,height,235,60,70,22); control(6,DU_LABEL,"Units: pixels. Colours: the original palette.",14,108,370,22);
    control(7,DU_LABEL,"&Width:",14,60,70,22);
    d->focus=3; d->accept=accept_controls;
}
static void flip_dialog(void)
{
    custom_dialog(D_FLIP,"Flip and Rotate",350,210);
    static const char *const labels[]={"Flip &horizontal","Flip &vertical","Rotate &90 degrees","Rotate &180 degrees","Rotate &270 degrees"};
    for (int i=0;i<5;i++) { struct du_widget *w=control(2+(unsigned)i,DU_RADIO,labels[i],20,36+i*23,280,20); w->value=i==0; }
    ui->dialog.accept=accept_controls;
}
static void format_dialog(void)
{
    custom_dialog(D_FORMAT,"Save As - File Format",350,170);
    static const char *const labels[]={"Original 8-bit bitmap (*.bmp)","24-bit bitmap (*.bmp)","PNG image (*.png)"};
    for (int i=0;i<3;i++) { struct du_widget *w=control(2+(unsigned)i,DU_RADIO,labels[i],20,38+i*25,310,22); w->value=i==(int)save_format; }
    ui->dialog.accept=accept_controls;
}
static void font_dialog(void)
{
    nfonts=1; snprintf(font_files[0],sizeof(font_files[0]),"CIUKIOS.CFN"); snprintf(font_names[0],sizeof(font_names[0]),"CiukiOS Sans");
    DIR *dir=opendir(font_dir); struct dirent *entry;
    while (dir && (entry=readdir(dir)) && nfonts<32) {
        size_t n=strlen(entry->d_name);
        if (n<4 || strcmp(entry->d_name+n-4,".CFN") || !strcmp(entry->d_name,"CIUKIOS.CFN")) continue;
        snprintf(font_files[nfonts],sizeof(font_files[0]),"%s",entry->d_name);
        char p[DU_TEXT_MAX]; u8 header[64]; snprintf(p,sizeof(p),"%s%s",font_dir,entry->d_name);
        FILE *f=fopen(p,"rb");
        if (!f) continue;
        int valid=fread(header,1,64,f)==64 && !memcmp(header,"CIUKFNT1",8); fclose(f);
        if (!valid) continue;
        memcpy(font_names[nfonts],header+8,32); font_names[nfonts][32]=0; nfonts++;
    }
    if (dir) closedir(dir);
    unsigned active=0;
    for (unsigned i=0;i<nfonts;i++) {
        font_rows[i]=(struct du_item){.text={font_names[i]}};
        if (!strcmp(font_files[i],font_file)) active=i;
    }
    custom_dialog(D_FONT,"Text Font",380,268);
    struct du_widget *list=control(2,DU_LISTVIEW,"",14,38,352,122);
    list->items=font_rows; list->count=nfonts; list->active=active; list->view=DU_LIST; list->focus_rect=1;
    du_list_ensure(list,active);
    control(3,DU_LABEL,"Scale (1-4):",14,180,94,22);
    char scale[8]; snprintf(scale,sizeof(scale),"%d",text_scale);
    control(4,DU_EDIT,scale,112,180,45,22);
    control(5,DU_CHECKBOX,"&Bold",196,180,110,22)->value=text_bold;
    ui->dialog.accept=accept_controls;
}
static void file_dialog(int save)
{
    char folder[DU_TEXT_MAX]; snprintf(folder,sizeof(folder),"%s",path[0]?path:"/Documents/");
    char *slash=strrchr(folder,'/'); if (slash==folder) folder[1]=0; else if (slash) *slash=0;
    const char *base=strrchr(path,'/'); base=base?base+1:path;
    char name[DU_TEXT_MAX]; snprintf(name,sizeof(name),"%s",path[0]?base:"untitled");
    if (save) {
        char *dot=strrchr(name,'.'); if (dot) *dot=0;
        size_t len=strlen(name);
        if (len+4>=sizeof(name)) { error_message(-ENAMETOOLONG); return; }
        strcat(name,save_format==PAINT_PNG?".png":".bmp");
    }
    if (du_file_dialog(ui,save?D_SAVE:D_OPEN,save,save?"Save As (BMP or PNG)":"Open (BMP or PNG)",folder,
                       save?name:"",NULL,NULL)) no_memory();
}
static void run_pending(void)
{
    int action=pending; pending=A_NONE;
    if (action==A_NEW) new_image(cv.w,cv.h);
    else if (action==A_OPEN) file_dialog(0);
    else if (action==A_EXIT) { closing=1; if (quit_cb) quit_cb(callback_host); }
    else if (action==A_DROP) error_message(paint_open(dropped_path));
}
static void ask_then(int action)
{
    if (drag) tool_up(lx,ly);
    finish_pending(); drop_selection();
    if (!dirty) { pending=action; run_pending(); return; }
    pending=action;
    static const char *const buttons[]={"Yes","No","Cancel"};
    du_message(ui,D_CHANGES,"CiukPaint","Save changes to the picture?",DU_DLG_ICON_QUESTION,buttons,3,0,2);
}
static void do_save(const char *p)
{
    int r=paint_save(p,save_format);
    if (r) { pending=A_NONE; error_message(r); } else if (pending) run_pending();
}
static void dialog_result(const struct du_event *e)
{
    if (e->id==D_CHANGES) {
        if (e->index==2) pending=A_NONE;
        else if (e->index==1) run_pending();
        else if (!path[0]) file_dialog(1); else do_save(path);
    } else if (e->id==D_OPEN) { if (!e->index) error_message(paint_open(e->text)); }
    else if (e->id==D_SAVE) {
        if (e->index) { pending=A_NONE; return; }
        const char *ext=strrchr(e->text,'.');
        if (!ext) {
            int n=snprintf(pending_path,sizeof(pending_path),"%s%s",e->text,save_format==PAINT_PNG?".png":".bmp");
            if (n<0 || (size_t)n>=sizeof(pending_path)) { pending=A_NONE; error_message(-ENAMETOOLONG); return; }
        } else {
            snprintf(pending_path,sizeof(pending_path),"%s",e->text);
            if (!strcmp(ext,".png") || !strcmp(ext,".PNG")) save_format=PAINT_PNG;
            else if (!strcmp(ext,".bmp") || !strcmp(ext,".BMP")) { if (save_format==PAINT_PNG) save_format=PAINT_BMP8; }
            else { pending=A_NONE; message("Use the .bmp or .png extension."); return; }
        }
        struct stat st;
        if (!stat(pending_path,&st)) {
            static const char *const buttons[]={"Yes","No"};
            du_message(ui,D_OVERWRITE,"Save As","This file exists. Replace it?",DU_DLG_ICON_QUESTION,buttons,2,1,1);
        } else do_save(pending_path);
    } else if (e->id==D_OVERWRITE) { if (!e->index) do_save(pending_path); else file_dialog(1); }
    else if (e->id==D_FORMAT) { if (!e->index) { save_format=candidate_format; file_dialog(1); } }
    else if (e->id==D_ATTR && !e->index) { if (candidate_w!=cv.w || candidate_h!=cv.h) resize_canvas(candidate_w,candidate_h); }
    else if (e->id==D_FLIP && !e->index) transform(candidate_transform);
    else if (e->id==D_FONT && !e->index) {
        if (candidate_font<nfonts) snprintf(font_file,sizeof(font_file),"%s",font_files[candidate_font]);
        text_scale=candidate_scale; text_bold=candidate_bold; font_load(); if (text_on) text_render();
    }
    layout();
}
void paint_command(int id)
{
    /* Edit accelerators belong to the text field while text is being typed. */
    if (text_on && (id==M_CUT || id==M_COPY || id==M_PASTE || id==M_SELALL || id==M_CLEARSEL)) {
        unsigned code=id==M_CUT?0x2d:id==M_COPY?0x2e:id==M_PASTE?0x2f:id==M_SELALL?0x1e:DU_KEY_DELETE;
        du_ta_key(&text_edit,code,id==M_CLEARSEL?0:DU_CTRL); text_render(); layout(); return;
    }
    switch (id) {
    case M_NEW: ask_then(A_NEW); break;
    case M_OPEN: ask_then(A_OPEN); break;
    case M_SAVE: finish_pending(); drop_selection(); if (!path[0]) file_dialog(1); else do_save(path); break;
    case M_SAVEAS: finish_pending(); drop_selection(); format_dialog(); break;
    case M_EXIT: ask_then(A_EXIT); break;
    case M_UNDO: undo(0); break; case M_REDO: undo(1); break;
    case M_CUT: if (sel_on && copy_selection()) clear_selection(); break;
    case M_COPY: if (sel_on) copy_selection(); break; case M_PASTE: paste(); break;
    case M_CLEARSEL: clear_selection(); break;
    case M_SELALL: finish_pending(); drop_selection(); select_tool(T_SELECT); sel_on=1; selx=sely=0; selw=cv.w; selh=cv.h; break;
    case M_TOOLBOX: show_tools=!show_tools; break; case M_COLORBOX: show_colors=!show_colors; break;
    case M_STATUS: show_status=!show_status; break;
    case M_ZOOMIN: case M_ZOOMOUT: set_zoom(zoom_step(id==M_ZOOMIN?1:-1),scx+page_w()/2,scy+page_h()/2); break;
    case M_Z1: case M_Z2: case M_Z4: case M_Z6: case M_Z8: set_zoom(zooms[id-M_Z1],scx+page_w()/2,scy+page_h()/2); break;
    case M_GRID: if (zoom>=4) show_grid=!show_grid; break;
    case M_FONT: font_dialog(); break; case M_FLIP: finish_pending(); flip_dialog(); break;
    case M_INVERT: finish_pending(); transform(6); break;
    case M_ATTR: finish_pending(); drop_selection(); attributes_dialog(); break;
    case M_CLEARIMG: finish_pending(); drop_selection(); begin_op(0); fill_rect(0,0,cv.w-1,cv.h-1,bg); commit_op(); break;
    case M_OPAQUE: opaque=!opaque; if (floating) float_render(); if (text_on) text_render(); break;
    case M_HELP:
        message("Choose a tool; its options appear below the tool box.\nLeft: foreground. Right: background. Palette: left/right click.\nSelect: choose Rect or Free, then drag the selection.\nDrag inside to move (Ctrl: copy). Arrows move it.\nRight-click a selection for the edit menu.\nText: click, type, click outside to place. Esc cancels.\nCurve: draw a line, then drag twice to bend it.\nPolygon: drag first side, click corners, double-click to close.\nRight eraser: erase only the foreground colour.\nCtrl+Z Undo, Ctrl+Y Repeat, Ctrl+X/C/V Cut/Copy/Paste.\nCtrl+A Select All, Del Clear, Ctrl+I Invert, Ctrl+R Rotate.\nCtrl+E Attributes, Ctrl+T/L Tool/Colour Box, Ctrl+G Grid.\nCtrl+PgDn/PgUp Zoom. Arrows scroll; PgUp/PgDn page."); break;
    case M_ABOUT:
        du_message(ui,999,"About CiukPaint","CiukPaint\nClassic computing, one woof at a time.\nOriginal CiukiOS bitmap editor, native desktop edition.\nBMP (1/4/8/24/32-bit) and PNG import.\nOriginal 8-bit BMP, 24-bit BMP and PNG save.\nOne undo/redo level. Maximum 2048 x 1200 pixels.",DU_DLG_ICON_INFO,NULL,0,0,0);
        du_dialog_icon(ui,du_icon_argb(DU_ICON_CIUKI,32)); break;
    }
    layout();
}

static int text_drag;
static void option_click(int sx, int sy)
{
    int dx=sx-optx-2, dy=sy-opty-2;
    if (dx<0 || dy<0 || dx>=OPT_W-4 || dy>=OPT_H-4) return;
    switch (tool) {
    case T_LINE: case T_CURVE: if (dy/15<5) line_w=dy/15+1; break;
    case T_RECT: case T_POLY: case T_ELLIPSE: case T_RRECT: if (dy/25<3) fill_style=dy/25; break;
    case T_ERASER: eraser_size=imin(3,dy/19); break;
    case T_ZOOM: if (dy/15<5) set_zoom(zooms[dy/15],scx+page_w()/2,scy+page_h()/2); break;
    case T_BRUSH: if (dy/19<4) { brush_kind=brush_kinds[dy/19]; brush_size=imin(2,dx/16); } break;
    case T_AIR: if (dy/25<3) air_size=dy/25; break;
    case T_SELECT:
        if (dy<36) { drop_selection(); free_select=dy>=18; }
        else { opaque=dy<56; if (floating)float_render(); }
        break;
    case T_TEXT: opaque=dy<38; if (floating) float_render(); if (text_on) text_render(); break;
    default: return;
    }
}
static void nudge(int dx, int dy)
{
    if (!floating && !lift(0)) return;
    selx+=dx; sely+=dy; float_render();
}
static void cancel_pointer(void)
{
    if (drag) {
        /* Capture loss cancels previews; freehand strokes already visible
         * are committed, as a release at the last known point. */
        if (drag_mode==DM_FREE) commit_op();
        else if (drag_mode!=DM_MOVE && drag_mode!=DM_NEWSEL) cancel_op();
        if (drag_mode==DM_CURVE) curve_stage=0;
        if (drag_mode==DM_POLY) npt=0;
        if (drag_mode==DM_NEWSEL) { sel_on=0; lasso_count=0; img_free(&selection_mask); }
        drag=0; drag_mode=DM_NONE; pen_replace=-1;
    }
    text_drag=0; du_timer_stop(ui,T_AIR);
}
static void editor_key(const struct du_event *e)
{
    unsigned key=e->key;
    if (key==DU_KEY_ESC) {
        if (text_on) { text_on=0; cancel_op(); }
        else if (npt) { npt=0; cancel_op(); }
        else if (curve_stage) { curve_stage=0; cancel_op(); }
        else drop_selection();
        cancel_pointer(); layout(); return;
    }
    if (text_on) {
        int handled=0;
        if (!(e->modifiers & DU_CTRL) && (key==DU_KEY_UP || key==DU_KEY_DOWN || key==DU_KEY_HOME || key==DU_KEY_END)) {
            int at=(int)text_edit.cursor,x,y;
            if (key==DU_KEY_HOME) while (at && text[at-1]!='\n') at--;
            else if (key==DU_KEY_END) while (at<text_len && text[at]!='\n') at++;
            else { text_position(at,&x,&y); at=text_at(x,y+(key==DU_KEY_UP?-16:16)*text_scale); }
            du_ta_select(&text_edit,e->modifiers & DU_SHIFT?text_edit.anchor:(size_t)at,(size_t)at); handled=1;
        } else handled=du_ta_key(&text_edit,key,e->modifiers);
        if (!handled && e->ch && !(e->modifiers & DU_CTRL)) handled=du_ta_text(&text_edit,e->ch>=32 && e->ch<=126?e->ch:'?');
        if (handled) { text_render(); layout(); return; }
    }
    if (key==DU_KEY_MENU || (key==DU_KEY_F10 && e->modifiers==DU_SHIFT)) { context_menu(vx+40,vy+40); return; }
    if (drag) return;
    int old_x=scx,old_y=scy;
    int dx=0,dy=0,step=imax(1,32/zoom);
    if (key==DU_KEY_LEFT) dx=-1; else if (key==DU_KEY_RIGHT) dx=1;
    else if (key==DU_KEY_UP) dy=-1; else if (key==DU_KEY_DOWN) dy=1;
    else if (key==DU_KEY_PGUP) scy-=page_h(); else if (key==DU_KEY_PGDN) scy+=page_h();
    else if (key==DU_KEY_HOME) scx=scy=0;
    else if (key==DU_KEY_END) { scx=cv.w; scy=cv.h; }
    if (sel_on && (dx || dy)) nudge(dx,dy); else { scx+=dx*step; scy+=dy*step; scroll_canvas(old_x,old_y); }
    partial_layout=1; layout(); partial_layout=0;
}
static void event(struct du_ui *u, const struct du_event *e, void *arg)
{
    (void)arg;
    if (e->type==DU_EV_DIALOG) { dialog_result(e); return; }
    /* fx-19 WM_DROPFILES, as Windows Paint: open the first dropped picture
     * after the question about unsaved changes. */
    if (e->type==DU_EV_DROPFILES) {
        if (!u->dialog.kind && du_drop_file(0,dropped_path,sizeof(dropped_path))>0 && dropped_path[0]) ask_then(A_DROP);
        return;
    }
    if (e->type==DU_EV_ACTIVATE) { paint_command(e->id); return; }
    if (e->type==DU_EV_CHANGE && (e->id==P_HSCROLL || e->id==P_VSCROLL)) {
        int ox=scx,oy=scy;
        if (e->id==P_HSCROLL) scx=e->index; else scy=e->index;
        scroll_canvas(ox,oy);
        partial_layout=1; layout(); partial_layout=0; return;
    }
    if (e->type==DU_EV_KEY) { editor_key(e); return; }
    if (e->type==DU_EV_TICK && e->id==T_AIR && drag && tool==T_AIR && drag_mode==DM_FREE) { spray(lx,ly,c1); flush(); return; }
    if (e->type!=DU_EV_POINTER) return;
    if (e->index==DU_POINTER_CANCEL) { cancel_pointer(); return; }
    int btn=(int)e->key;
    if (e->id>=P_TOOL && e->id<P_TOOL+NTOOLS) {
        int next=du_contains(du_get(u,e->id)->box,e->x,e->y) ? e->id-P_TOOL : -1;
        if (next!=tool_hot) {
            if (tool_hot>=0) du_invalidate_widget(u,P_TOOL+tool_hot);
            tool_hot=next; if (next>=0) du_invalidate_widget(u,P_TOOL+next);
        }
        if (e->index==DU_POINTER_DOWN) { select_tool(e->id-P_TOOL); layout(); }
        return;
    }
    if (tool_hot>=0) { du_invalidate_widget(u,P_TOOL+tool_hot); tool_hot=-1; }
    if (e->index==DU_POINTER_DOWN && e->id>=P_SWATCH && e->id<P_SWATCH+NSWATCH) {
        if (btn==1) fg=swatch[e->id-P_SWATCH]; else if (btn==2) bg=swatch[e->id-P_SWATCH];
        if (floating && !opaque) float_render(); if (text_on) text_render(); layout(); return;
    }
    if (e->id==P_OPTIONS) { if (e->index==DU_POINTER_DOWN) { option_click(e->x,e->y); layout(); } return; }
    if (e->id!=P_CANVAS) return;
    int old_x=ptr_x,old_y=ptr_y;
    if (e->index==DU_POINTER_MOVE && u->capture<0 && !du_contains(du_get(u,P_CANVAS)->box,e->x,e->y)) {
        /* Erase the departing preview while retaining the last image
         * coordinates in the status panel, as before update regions. */
        if (ptr_x>=0 && (tool==T_BRUSH || tool==T_ERASER || tool==T_AIR)) {
            int radius=(tool==T_AIR ? 6+air_size*4 : 12)*zoom;
            du_invalidate(u,du_intersection((struct du_rect){vx,vy,vw,vh},(struct du_rect){
                vx+4+(ptr_x-scx)*zoom-radius,vy+4+(ptr_y-scy)*zoom-radius,2*radius+zoom,2*radius+zoom}));
        }
        return;
    }
    int x,y; to_img(e->x,e->y,&x,&y);
    /* Capture can extend beyond the client; bounded coordinates keep all
     * drawing arithmetic inside the supported image domain. */
    x=imax(-PAINT_MAX_AXIS,imin(x,2*PAINT_MAX_AXIS)); y=imax(-PAINT_MAX_AXIS,imin(y,2*PAINT_MAX_AXIS));
    ptr_x=x>=0 && x<cv.w && y>=0 && y<cv.h?x:-1; ptr_y=y;
    if (e->index==DU_POINTER_DOWN) {
        if (btn!=1 && btn!=2) return;
        status[0]=0;
        if (text_on && btn==1 && text_contains(x,y)) {
            size_t at=(size_t)text_at(x,y);
            du_select_click(&text_edit.selection,text,at,(u->modifiers&DU_SHIFT)!=0,
                            u->now,e->x,e->y,&text_edit.anchor,&text_edit.cursor);
            text_drag=1; layout(); return;
        }
        if (btn==2 && (tool==T_SELECT || inside_sel(x,y)) && !text_on) { context_menu(e->x,e->y); return; }
        tool_down(x,y,btn);
        if (drag && tool==T_AIR) du_timer_start(u,T_AIR,55000000ull,1);
    } else if (text_drag) {
        du_select_drag(&text_edit.selection,text,(size_t)text_at(x,y),&text_edit.anchor,&text_edit.cursor);
        if (e->index==DU_POINTER_UP) text_drag=0;
    } else if (e->index==DU_POINTER_MOVE) { if (drag) tool_move(x,y); }
    else if (e->index==DU_POINTER_UP && drag==btn) { tool_up(x,y); du_timer_stop(u,T_AIR); }
    /* Freehand strokes already carry exact image damage. Selection/text
     * overlays have their own retained preview bounds. */
    if (tool==T_SELECT || text_drag || text_on) du_invalidate(u,(struct du_rect){vx,vy,vw,vh});
    if (tool==T_BRUSH || tool==T_ERASER || tool==T_AIR) {
        int radius=(tool==T_AIR ? 6+air_size*4 : 12)*zoom;
        if (old_x>=0) du_invalidate(u,(struct du_rect){vx+4+(old_x-scx)*zoom-radius,vy+4+(old_y-scy)*zoom-radius,2*radius+zoom,2*radius+zoom});
        if (ptr_x>=0) du_invalidate(u,(struct du_rect){vx+4+(ptr_x-scx)*zoom-radius,vy+4+(ptr_y-scy)*zoom-radius,2*radius+zoom,2*radius+zoom});
    }
    partial_layout=1; layout(); partial_layout=0;
}
int paint_attach(struct du_ui *u)
{
    if (ui) return -EBUSY;
    if (!u || u->count) return -EINVAL;
    partial_layout=0; dx0=dy0=0; dx1=dy1=-1;
    ui=u; quit_cb=NULL; title_cb=NULL; callback_host=NULL;
    fg=BLACK; bg=WHITE; tool=prev_tool=T_PENCIL; tool_hot=-1; zoom=1;
    line_w=pen_size=1; fill_style=0; brush_kind=PEN_ROUND; brush_size=eraser_size=air_size=1;
    text_scale=1; text_bold=0; opaque=1; font_file[0]=0;
    show_tools=show_colors=show_status=1; show_grid=0; ptr_x=-1;
    pending=closing=text_drag=drag=drag_mode=curve_stage=npt=0;
    save_format=PAINT_BMP8; status[0]=0; seed=12345;
    memset(slot_of,0,sizeof(slot_of)); nslots=op_open=undo_kind=undone=dirty=0;
    sel_on=floating=text_on=free_select=lasso_count=0; pen_replace=-1;
    load_palette(); choose_swatches(); font_load();
    if (du_ta_init(&text_edit,TEXT_MAX)) { ui=NULL; return -ENOMEM; }
    text=text_edit.text;
    if (!img_alloc(&cv,DEF_W,DEF_H)) { du_ta_free(&text_edit); ui=NULL; return -ENOMEM; }
    img_fill(&cv,WHITE); path[0]=0; scx=scy=0;
    u->menus=menus; u->menu_count=5; u->event=event; u->paint=render; u->layout=relayout; u->user=NULL;
    if (!du_add(u,DU_CANVAS,P_CANVAS,(struct du_rect){0,0,1,1},"")) goto full;
    du_add(u,DU_SCROLLBAR,P_HSCROLL,(struct du_rect){0,0,1,1},"");
    du_add(u,DU_SCROLLBAR,P_VSCROLL,(struct du_rect){0,0,1,1},"");
    du_add(u,DU_STATUS,P_STATUS,(struct du_rect){0,0,1,1},"");
    for (int i=0;i<NTOOLS;i++) if (!du_add(u,DU_CANVAS,P_TOOL+i,(struct du_rect){0,0,1,1},tool_names[i])) goto full;
    for (int i=0;i<NSWATCH;i++) if (!du_add(u,DU_CANVAS,P_SWATCH+i,(struct du_rect){0,0,1,1},"")) goto full;
    if (!du_add(u,DU_CANVAS,P_OPTIONS,(struct du_rect){0,0,1,1},"")) goto full;
    layout(); du_focus(u,P_CANVAS); update_title(); return 0;
full:
    paint_detach(); return -ENOSPC;
}
void paint_callbacks(void (*quit)(void *), void (*title)(void *, const char *), void *host)
{ quit_cb=quit; title_cb=title; callback_host=host; update_title(); }
void paint_detach(void)
{
    if (!ui) return;
    open_finish();
    cancel_pointer(); du_dialog_close(ui); du_menu_close(ui);
    img_free(&cv); img_free(&old); pool_free(); img_free(&flt); img_free(&clip); img_free(&selection_mask); du_ta_free(&text_edit);
    ui->event=NULL; ui->paint=NULL; ui->layout=NULL; ui->menus=NULL; ui->menu_count=ui->count=0;
    ui=NULL; quit_cb=NULL; title_cb=NULL; callback_host=NULL; text=NULL;
}
static int install_image(const char *p, struct paint_image next)
{
    finish_pending(); drop_selection(); drop_undo(); pool_free(); img_free(&cv);
    image_revision++; cv=next; snprintf(path,sizeof(path),"%s",p); dirty=0; scx=scy=0;
    const char *ext=strrchr(p,'.'); save_format=ext && (!strcmp(ext,".png") || !strcmp(ext,".PNG"))?PAINT_PNG:PAINT_BMP8;
    update_title(); layout(); return 0;
}
static struct { struct du_job job; struct paint_image image; char path[PAINT_PATH_MAX]; uint8_t palette[PAINT_COLORS*3]; int error; uint64_t revision; } opening;
static void read_image(void *unused)
{ (void)unused; opening.error=paint_read_image(opening.path,opening.palette,&opening.image); }
int paint_open_begin(const char *p)
{
    if (opening.job.started) return -EBUSY;
    if (strlen(p)>=sizeof(opening.path)) return -ENAMETOOLONG;
    opening.revision=image_revision;
    strcpy(opening.path,p); memcpy(opening.palette,pal,sizeof(opening.palette)); opening.image=(struct paint_image){0};
    return du_job_start_ui(&opening.job,read_image,NULL,ui);
}
int paint_open_poll(void)
{
    if (!du_job_ready(&opening.job)) return 0;
    du_job_finish(&opening.job);
    if (opening.error) return opening.error;
    if (opening.revision!=image_revision) { img_free(&opening.image); return -EBUSY; }
    struct paint_image image=opening.image; opening.image=(struct paint_image){0};
    install_image(opening.path,image); return 1;
}
int paint_open(const char *p)
{
    if (strlen(p)>=sizeof(path)) return -ENAMETOOLONG;
    struct paint_image next={0}; int r=paint_read_image(p,pal,&next);
    return r ? r : install_image(p,next);
}
static void open_finish(void)
{ du_job_finish(&opening.job); img_free(&opening.image); }
int paint_save(const char *p, enum paint_format format)
{
    if (strlen(p)>=sizeof(path)) return -ENAMETOOLONG;
    char name[DU_TEXT_MAX]; snprintf(name,sizeof(name),"%s",p);
    finish_pending(); drop_selection();
    int r=paint_write_image(name,format,&cv,pal);
    if (!r) { snprintf(path,sizeof(path),"%s",name); save_format=format; dirty=0; update_title(); layout(); }
    return r;
}
int paint_request_close(void)
{ if (closing) return 0; if (dirty || op_open) { ask_then(A_EXIT); return 1; } return 0; }
void paint_snapshot(struct paint_state *s)
{
    *s=(struct paint_state){.width=cv.w,.height=cv.h,.tool=tool,.zoom=zoom,.scroll_x=scx,.scroll_y=scy,.dirty=dirty,
        .can_undo=op_open || (undo_kind && !undone),.can_redo=undo_kind && undone && !op_open,
        .selection=sel_on,.floating=floating,.text=text_on,.operation=op_open,.selection_x=selx,.selection_y=sely,
        .free_select=free_select,.text_cursor=text_edit.cursor,.text_anchor=text_edit.anchor,.text_bytes=text_edit.len,
        .selection_w=selw,.selection_h=selh,.foreground=fg,.background=bg,
        .view={vx,vy,vw,vh},.options={optx,opty,OPT_W,OPT_H},.colors={0,cby,W,CB_H}};
}
uint8_t paint_pixel(int x,int y) { return pixel(x,y); }
uint32_t paint_rgb(uint8_t index) { return index<NCOL?rgb(index):0; }
const char *paint_path(void) { return path; }
