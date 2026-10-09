/* CiukPaint for the CiukiOS desktop: a bitmap painter with the classic tool
 * set (rectangular selection, eraser, fill, colour picker, magnifier, pencil,
 * brush, airbrush, text, line, curve, rectangle, polygon, ellipse, rounded
 * rectangle), one level of undo and redo, a clipboard, zoom and 8/24-bit BMP
 * files. The picture is 8-bit indices of the desktop's colours (0-15 the
 * colour scheme, 16-79 the Ciuki portrait's, 80-143 the icons') kept in DOS
 * blocks of whole rows; the shell draws it with ui_bitmap.
 *
 * Undo: an operation saves each row the first time it changes it (a pool of
 * rows in DOS memory); undo swaps those rows back, so redo swaps them again.
 * Shapes, selections and text are previews: each pointer move puts the saved
 * rows back and draws the shape again into the picture.
 *
 * The right button draws with the background colour. The shell reports only
 * its press, so a right drag follows the pointer by polling (EV_POLL). */
#include "app.h"

#define PATH_MAX 80
#define MAXSEG   24
#define MAX_W    2048
#define MAX_H    1200
#define DEF_W    512
#define DEF_H    384
#define NCOL     144
#define WHITE    80          /* the icon palette's white */
#define BLACK    79          /* the portrait palette's black */

typedef u8 __far *fptr;
static fptr FP(u16 s, u16 o) { return (fptr)(((u32)s << 16) | o); }

static void ffill(u16 seg, u16 off, u16 n, u8 v);
#pragma aux ffill = "push es" "mov es,dx" "rep stosb" "pop es" \
    parm [dx] [di] [cx] [al] modify exact [di cx];
static void fcopy(u16 dseg, u16 doff, u16 sseg, u16 soff, u16 n);
#pragma aux fcopy = "push ds" "push es" "mov es,dx" "mov ds,ax" "rep movsb" "pop es" "pop ds" \
    parm [dx] [di] [ax] [si] [cx] modify exact [di si cx];

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

/* ------------------------------------------------------------------ */
/* Colours                                                             */
/* The portrait's and the icons' colours (src/com/ciuki_logo_palette.inc and
 * src/com/ui_icons_palette.inc; scripts/qemu_test_ciukpaint.py checks them). */
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
static char sys_font[13];                    /* DESKTOP.CFG's font, "" built-in */
static void load_palette(void)
{
    u8 cfg[72];
    int f, n = 0;
    mem_copy(pal, desk_default, 48);
    mem_copy(pal + 48, logo_pal, 192);
    mem_copy(pal + 240, icon_pal, 192);
    sys_font[0] = 0;
    f = dos_open("\\SYSTEM\\UI\\DESKTOP.CFG", 0);
    if (f >= 0) { n = dos_read(f, cfg, 72); dos_close(f); }
    if (n == 72 && !mem_cmp(cfg, "CUI1", 4)) {
        mem_copy(pal, cfg + 4, 48);
        str_ncopy(sys_font, (char *)cfg + 57, 13);
    }
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

/* ------------------------------------------------------------------ */
/* Pictures: rows in DOS blocks of at most 64 KB                       */
struct img { int w, h, rps, n; u16 seg[MAXSEG]; };
static struct img cv, old, pool, flt, clip;   /* canvas, before a resize, undo rows, floating selection, clipboard */
static int img_rps(int w) { u16 r = 65535u / (u16)w; return r > 4000 ? 4000 : (int)r; }
static u16 paras(u32 bytes) { return (u16)((bytes + 15) >> 4); }
static void img_free(struct img *m)
{
    int i;
    for (i = 0; i < m->n; i++) if (m->seg[i]) dos_free(m->seg[i]);
    m->n = 0; m->w = m->h = 0;
}
static int img_alloc(struct img *m, int w, int h)
{
    int i, rows;
    m->n = 0;
    m->w = w; m->h = h; m->rps = img_rps(w);
    if ((h + m->rps - 1) / m->rps > MAXSEG) { m->w = m->h = 0; return 0; }
    for (i = 0; i * m->rps < h; i++) {
        rows = imin(h - i * m->rps, m->rps);
        m->seg[i] = dos_alloc(paras((u32)rows * (u16)w));
        if (!m->seg[i]) { img_free(m); return 0; }
        m->n = i + 1;
    }
    return 1;
}
static u16 rseg(struct img *m, int y) { return m->seg[y / m->rps]; }
static u16 roff(struct img *m, int y) { return (u16)(y % m->rps) * (u16)m->w; }
static fptr rowp(struct img *m, int y) { return FP(rseg(m, y), roff(m, y)); }
static void img_fill(struct img *m, u8 c)
{
    int y;
    for (y = 0; y < m->h; y++) ffill(rseg(m, y), roff(m, y), m->w, c);
}

/* ------------------------------------------------------------------ */
/* Undo: rows saved by the operation in progress (or the last one)     */
#define U_NONE  0
#define U_ROWS  1
#define U_IMAGE 2
/* In one DOS block: row -> its slot + 1, then slot -> row. */
static u16 tabs;
#define slot_of ((u16 __far *)FP(tabs, 0))
#define row_of  ((u16 __far *)FP(tabs, MAX_H * 2))
static int nslots, op_open, op_ok, undo_kind, undone, dirty;
/* The pool lives in XMS when there is some (one handle, a row for every
 * picture row, moved through rowbuf), else in DOS blocks of 16 KB. */
#define ROWBUF_BYTES 3072
#define ROWBUF_OFF (MAX_H * 4)
#define rowbuf FP(tabs, ROWBUF_OFF)
static u16 xms_seg, xms_off, xms_h, xms_stride;
static int xms_state;                        /* 0 unknown, 1 present, 2 none */
#pragma pack(push, 1)
static struct { u32 len; u16 sh; u32 so; u16 dh; u32 dof; } xm;
#pragma pack(pop)
static u16 xms(u16 ax, u16 dx)
{
    struct regs r;
    mem_set(&r, 0, sizeof r);
    r.ax = ax; r.dx = dx; r.si = (u16)&xm; r.ds = r.es = app_seg();
    far_regs(xms_seg, xms_off, &r);
    return ax == 0x0900 ? (r.ax == 1 ? r.dx : 0) : r.ax;
}
static u32 far_addr(u16 seg, u16 off) { return ((u32)seg << 16) | off; }
/* Move a row between XMS slot k and conventional memory seg:off. */
static void xmove(int to_xms, int k, u16 seg, u16 off)
{
    u32 conv = far_addr(seg, off), ext = (u32)k * xms_stride;
    xm.len = xms_stride;
    if (to_xms) { xm.sh = 0; xm.so = conv; xm.dh = xms_h; xm.dof = ext; }
    else { xm.sh = xms_h; xm.so = ext; xm.dh = 0; xm.dof = conv; }
    xms(0x0B00, 0);
}
static void pool_free(void)
{
    if (xms_h) xms(0x0A00, xms_h);
    xms_h = 0;
    img_free(&pool);
}
static int pool_cap(void) { return xms_h ? (pool.h == cv.h && pool.w == cv.w ? cv.h : 0) : imin(pool.n * pool.rps, cv.h); }
static int pool_grow(void)
{
    int rows;
    u16 s;
    if (pool.w != cv.w || (xms_h && pool.h != cv.h)) { pool_free(); pool.w = cv.w; pool.rps = imax(1, img_rps(cv.w) / 4); }  /* 16 KB blocks: they fit in more gaps */
    if (!xms_state) {
        struct regs r;
        mem_set(&r, 0, sizeof r);
        r.ax = 0x4300;
        intr(0x2F, &r);
        xms_state = 2;
        if ((r.ax & 0xFF) == 0x80) {
            mem_set(&r, 0, sizeof r);
            r.ax = 0x4310;
            intr(0x2F, &r);
            xms_seg = r.es; xms_off = r.bx; xms_state = 1;
        }
    }
    if (xms_state == 1 && !xms_h && !pool.n) {
        xms_stride = (cv.w + 1) & ~1;
        xms_h = xms(0x0900, (u16)(((u32)xms_stride * cv.h + 1023) >> 10));
        if (xms_h) { pool.h = cv.h; return 1; }
    }
    if (xms_h || pool.n >= MAXSEG || pool_cap() >= cv.h) return 0;
    rows = imin(cv.h - pool.n * pool.rps, pool.rps);
    s = dos_alloc(paras((u32)rows * (u16)cv.w));
    if (!s) return 0;
    pool.seg[pool.n++] = s;
    pool.h = pool_cap();
    return 1;
}
/* Save canvas row y in slot k, or put it back (the row goes through rowbuf
 * for XMS, whose moves are of even lengths). */
static void pool_put(int k, int y)
{
    if (!xms_h) { fcopy(rseg(&pool, k), roff(&pool, k), rseg(&cv, y), roff(&cv, y), cv.w); return; }
    fcopy(tabs, ROWBUF_OFF, rseg(&cv, y), roff(&cv, y), cv.w);
    xmove(1, k, tabs, ROWBUF_OFF);
}
static void pool_get(int k, int y)
{
    if (!xms_h) { fcopy(rseg(&cv, y), roff(&cv, y), rseg(&pool, k), roff(&pool, k), cv.w); return; }
    xmove(0, k, tabs, ROWBUF_OFF);
    fcopy(rseg(&cv, y), roff(&cv, y), tabs, ROWBUF_OFF, cv.w);
}
/* Give back the pool's blocks that hold no saved row. */
static void pool_trim(void)
{
    while (pool.n > 0 && (pool.n - 1) * pool.rps >= nslots) dos_free(pool.seg[--pool.n]);
    if (!pool.n && !xms_h) pool.w = 0;
}
static void drop_undo(void)
{
    int i;
    for (i = 0; i < nslots; i++) slot_of[row_of[i]] = 0;
    nslots = 0;
    if (undo_kind == U_IMAGE) img_free(&old);
    undo_kind = U_NONE;
    undone = 0;
}
static int alloc_retry(struct img *m, int w, int h)
{
    if (img_alloc(m, w, h)) return 1;
    if (!op_open) { if (undo_kind == U_ROWS) drop_undo(); pool_trim(); }
    return img_alloc(m, w, h);
}

static int dx0, dy0, dx1, dy1 = -1;          /* image area to show again */
static int bx0, by0, bx1, by1 = -1;          /* what the preview drew */
static void dmg_add(int x0, int y0, int x1, int y1)
{
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
        if (!op_ok) str_copy(status, "There was not enough memory to keep an undo copy.");
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
    int k, y;
    u16 i;
    for (k = 0; k < nslots; k++) {
        y = row_of[k];
        if (xms_h) {
            xmove(0, k, tabs, ROWBUF_OFF);
            xmove(1, k, rseg(&cv, y), roff(&cv, y));   /* reads one byte past an odd row */
            fcopy(rseg(&cv, y), roff(&cv, y), tabs, ROWBUF_OFF, cv.w);
        } else {
            fptr a = rowp(&cv, y), b = rowp(&pool, k);
            for (i = 0; i < (u16)cv.w; i++) { u8 t = a[i]; a[i] = b[i]; b[i] = t; }
        }
    }
}

/* ------------------------------------------------------------------ */
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
    } else ffill(rseg(&cv, y), roff(&cv, y) + x0, x1 - x0 + 1, c);
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

/* Flood fill (4-connected) with a stack of points in a DOS block. */
static void flood(int x, int y, u8 c)
{
    u16 st = dos_alloc(0x800), n = 0, max = 0x2000;
    u16 __far *s;
    u8 t;
    int lx, rx, i, yy, in;
    fptr row;
    if (x < 0 || y < 0 || x >= cv.w || y >= cv.h) return;
    t = pixel(x, y);
    if (t == c) { if (st) dos_free(st); return; }
    if (!st) { app_sound(5); return; }
    s = (u16 __far *)FP(st, 0);
    s[0] = x; s[1] = y; n = 1;
    while (n) {
        n--;
        x = s[n * 2]; y = s[n * 2 + 1];
        row = rowp(&cv, y);
        if (row[x] != t) continue;
        lx = x; while (lx > 0 && row[lx - 1] == t) lx--;
        rx = x; while (rx < cv.w - 1 && row[rx + 1] == t) rx++;
        hspan(lx, rx, y, c);
        for (yy = y - 1; yy <= y + 1; yy += 2) {
            if (yy < 0 || yy >= cv.h) continue;
            row = rowp(&cv, yy);
            in = 0;
            for (i = lx; i <= rx; i++) {
                if (row[i] == t) {
                    if (!in && n < max) { s[n * 2] = i; s[n * 2 + 1] = yy; n++; }
                    in = 1;
                } else in = 0;
            }
        }
    }
    dos_free(st);
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

/* ------------------------------------------------------------------ */
/* Text: an installed CiukiOS font (\SYSTEM\FONTS\*.CFN, see            */
/* assets/fonts/README.md), 16 rows a glyph, scaled 1-4 times.          */
#define TEXT_MAX 200
static char text[TEXT_MAX + 1];
static struct field text_edit;
#define text_len text_edit.len
static int text_on, tx, ty, text_scale = 1, text_bold;
static int opaque = 1;                       /* selections and text cover what is below */
static u16 fnt;                              /* the style: 95 widths, then 95 glyphs of 16 words */
static char font_file[13];
static int fnt_synth, fnt_loaded;
static const char font_dir[] = "C:\\SYSTEM\\FONTS\\";
static int font_load(void)
{
    char p[40];
    u8 h[64];
    int f, n = 0;
    u16 flags;
    fnt_loaded = 0;
    if (!fnt) fnt = dos_alloc(paras(3135));
    if (!fnt) return 0;
    str_copy(p, font_dir);
    str_cat(p, font_file[0] ? font_file : "CIUKIOS.CFN");
    f = dos_open(p, 0);
    if (f < 0 && font_file[0]) {
        font_file[0] = 0;
        str_copy(p, font_dir); str_cat(p, "CIUKIOS.CFN");
        f = dos_open(p, 0);
    }
    if (f < 0) return 0;
    if (dos_read(f, h, 64) == 64 && !mem_cmp(h, "CIUKFNT1", 8)) {
        flags = h[44] | (h[45] << 8);
        fnt_synth = text_bold && !(flags & 1);
        dos_seek(f, 64L + (text_bold && (flags & 1) ? 3135 : 0), 0);
        n = dos_read_far(f, fnt, 0, 3135);
    }
    dos_close(f);
    fnt_loaded = n == 3135;
    return fnt_loaded;
}
static int glyph_w(char ch)
{
    if (ch < 32 || ch > 126) ch = '?';
    return peek8(fnt, ch - 32) + fnt_synth;
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
static int text_key(int key)
{
    int s = KEY_SCAN(key), ch = KEY_CHAR(key), p, x, y;
    if (ch == 13) return field_insert(&text_edit, "\n");
    if ((!ch || ch == 0xE0) && !(HOST.shift & SH_CTRL) &&
        (s == K_UP || s == K_DOWN || s == K_HOME || s == K_END)) {
        p = text_edit.cursor;
        if (s == K_HOME) while (p && text[p - 1] != '\n') p--;
        else if (s == K_END) while (p < text_len && text[p] != '\n') p++;
        else {
            text_position(p, &x, &y);
            p = text_at(x, y + (s == K_UP ? -16 : 16) * text_scale);
        }
        text_edit.click_valid = 0;
        field_select(&text_edit, MOUSE_DOWN, p, HOST.shift);
        text_edit.drag = text_edit.click_valid = 0;
        return 1;
    }
    return field_key(&text_edit, key, HOST.shift);
}
static void draw_glyph(int x, int y, char ch, u8 c)
{
    int r, col, start, k, s = text_scale, gw;
    u16 base, bits;
    if (ch < 32 || ch > 126) ch = '?';
    base = 95 + (u16)(ch - 32) * 32;
    gw = imin(16, glyph_w(ch));
    for (r = 0; r < 16; r++) {
        bits = peek16(fnt, base + r * 2);
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
    app_log("[PAINT] text", text);
}

/* ------------------------------------------------------------------ */
/* The selection; a floating one is lifted into flt and drawn over the */
/* picture at each move (the preview of the operation that lifted it). */
static int sel_on, selx, sely, selw, selh, floating, hole, holex, holey, holew, holeh;
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
    if (hole) fill_rect(holex, holey, holex + holew - 1, holey + holeh - 1, hole_c);
    for (y = 0; y < flt.h; y++) {
        cy = sely + y;
        if (cy < 0 || cy >= cv.h) continue;
        x0 = imax(0, -selx); x1 = imin(flt.w, cv.w - selx);
        if (x0 >= x1 || !touch(cy)) continue;
        if (opaque) fcopy(rseg(&cv, cy), roff(&cv, cy) + selx + x0, rseg(&flt, y), roff(&flt, y) + x0, x1 - x0);
        else {
            s = rowp(&flt, y); d = rowp(&cv, cy);
            for (x = x0; x < x1; x++) if (s[x] != bg) d[selx + x] = s[x];
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
        fcopy(rseg(&flt, y), roff(&flt, y), rseg(&cv, sely + y), roff(&cv, sely + y) + selx, selw);
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
        app_log("[PAINT] placed", 0);
    }
    sel_on = 0;
}
/* Copy the selection (or the floating pixels) to the clipboard. */
static int copy_selection(void)
{
    int y;
    struct img *src = floating ? &flt : &cv;
    int x0 = floating ? 0 : selx, y0 = floating ? 0 : sely;
    char t[24];
    if (!sel_on) return 0;
    img_free(&clip);
    if (!alloc_retry(&clip, selw, selh)) { no_memory(); return 0; }
    for (y = 0; y < selh; y++)
        fcopy(rseg(&clip, y), roff(&clip, y), rseg(src, y0 + y), roff(src, y0 + y) + x0, selw);
    fmt_u32(t, (u32)selw); str_cat(t, "x"); fmt_u32(t + str_len(t), (u32)selh);
    app_log("[PAINT] copy", t);
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
        fill_rect(selx, sely, selx + selw - 1, sely + selh - 1, bg);
        commit_op();
    }
    sel_on = 0;
    app_log("[PAINT] cleared selection", 0);
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
static void replace_canvas(struct img *n)
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
static void rotate_into(struct img *src, struct img *dst, int kind)
{
    int x, y;
    fptr d;
    for (y = 0; y < dst->h; y++) {
        d = rowp(dst, y);
        for (x = 0; x < dst->w; x++)
            d[x] = kind == 3 ? rowp(src, src->h - 1 - x)[y] : rowp(src, x)[src->w - 1 - y];
    }
}
static void transform_rows(struct img *m, int kind, int canvas)
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
static const char *const transform_names[] = { "", "flip horizontal", "flip vertical", "rotate 90",
                                                "rotate 180", "rotate 270", "invert" };
static void transform(int kind)
{
    struct img n;
    if (kind == 6) make_inverse();
    app_log("[PAINT] image", transform_names[kind]);
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
        if (!make_room()) return;
        if (!img_alloc(&n, cv.h, cv.w)) { no_memory(); return; }
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
    struct img n;
    int y;
    if (!make_room()) return 0;
    if (!img_alloc(&n, w, h)) { no_memory(); return 0; }
    img_fill(&n, bg);
    for (y = 0; y < imin(h, cv.h); y++) fcopy(rseg(&n, y), roff(&n, y), rseg(&cv, y), roff(&cv, y), imin(w, cv.w));
    replace_canvas(&n);
    return 1;
}

/* ------------------------------------------------------------------ */
/* BMP files                                                           */
static char path[PATH_MAX];
static void put16(u8 *d, u16 v) { d[0] = (u8)v; d[1] = (u8)(v >> 8); }
static void put32(u8 *d, u32 v) { put16(d, (u16)v); put16(d + 2, (u16)(v >> 16)); }
static u16 get16(const u8 *d) { return d[0] | (d[1] << 8); }
static u32 get32(const u8 *d) { return get16(d) | ((u32)get16(d + 2) << 16); }
static u8 dac8(u8 v) { return (u8)((v << 2) | (v >> 4)); }
static void update_title(void);

/* 8-bit indexed, bottom-up, the palette of the indices it uses. */
static int save_bmp(const char *p)
{
    u8 used[NCOL], map[NCOL], h[54];
    int n = 0, i, x, y, f, r = 0;
    u16 rowbytes;
    u32 off;
    fptr row;
    load_palette();
    mem_set(used, 0, NCOL);
    for (y = 0; y < cv.h; y++) {
        row = rowp(&cv, y);
        for (x = 0; x < cv.w; x++) if (row[x] < NCOL) used[row[x]] = 1;
    }
    for (i = 0; i < NCOL; i++) if (used[i]) map[i] = (u8)n++;
    if (!n) { used[WHITE] = 1; map[WHITE] = 0; n = 1; }
    rowbytes = (cv.w + 3) & ~3;
    off = 54 + (u32)n * 4;
    mem_set(h, 0, 54);
    h[0] = 'B'; h[1] = 'M';
    put32(h + 2, off + (u32)rowbytes * cv.h);
    put32(h + 10, off);
    put32(h + 14, 40);
    put32(h + 18, cv.w);
    put32(h + 22, cv.h);
    put16(h + 26, 1);
    put16(h + 28, 8);
    put32(h + 34, (u32)rowbytes * cv.h);
    put32(h + 38, 2835);
    put32(h + 42, 2835);
    put32(h + 46, n);
    put32(h + 50, n);
    f = dos_create(p);
    if (f < 0) return f;
    if (dos_write(f, h, 54) != 54) r = -112;
    for (i = 0, n = 0; i < NCOL && !r; i++) {
        if (!used[i]) continue;
        rowbuf[n++] = dac8(pal[i * 3 + 2]);
        rowbuf[n++] = dac8(pal[i * 3 + 1]);
        rowbuf[n++] = dac8(pal[i * 3]);
        rowbuf[n++] = 0;
    }
    if (!r && dos_write_far(f, tabs, ROWBUF_OFF, n) != n) r = -112;
    for (y = cv.h - 1; y >= 0 && !r; y--) {
        row = rowp(&cv, y);
        for (x = 0; x < cv.w; x++) rowbuf[x] = row[x] < NCOL ? map[row[x]] : 0;
        for (; x < (int)rowbytes; x++) rowbuf[x] = 0;
        i = dos_write_far(f, tabs, ROWBUF_OFF, rowbytes);
        if (i < 0) r = i;
        else if (i != (int)rowbytes) r = -112;
    }
    dos_close(f);
    if (r) return r;
    str_ncopy(path, p, PATH_MAX);
    dirty = 0;
    update_title();
    app_log("[PAINT] saved", p);
    return 0;
}

/* 1, 4, 8, 24 and 32-bit uncompressed BMP files; colours become the
 * nearest indices. 0 ok, < 0 a DOS error, 1 not a BMP CiukPaint reads,
 * 2 too large, 3 not enough memory. */
static u16 cache;
static u8 map_rgb(u8 r, u8 g, u8 b)
{
    u16 key;
    u8 v;
    if (!cache) return nearest(r >> 2, g >> 2, b >> 2);
    key = ((u16)(r >> 3) << 10) | ((u16)(g >> 3) << 5) | (b >> 3);
    v = peek8(cache, key);
    if (v == 0xFF) { v = nearest(r >> 2, g >> 2, b >> 2); poke8(cache, key, v); }
    return v;
}
static int load_bmp(const char *p)
{
    u8 h[54], pmap[256];
    int f, bpp, top, w, hh, y, x, i, k, ncol, r = 0, step;
    u32 off, hsize, comp, rowbytes, pos;
    long height;
    struct img n;
    fptr d;
    char t[24];
    f = dos_open(p, 0);
    if (f < 0) return f;
    if (dos_read(f, h, 54) != 54 || h[0] != 'B' || h[1] != 'M') { dos_close(f); return 1; }
    off = get32(h + 10); hsize = get32(h + 14);
    w = (int)get32(h + 18); height = (long)get32(h + 22);
    bpp = get16(h + 28); comp = get32(h + 30); ncol = (int)get32(h + 46);
    top = height < 0;
    if (top) height = -height;
    if (hsize < 40 || get16(h + 26) != 1 || (comp && !(comp == 3 && bpp == 32)) ||
        (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32) || get32(h + 18) == 0 || !height) {
        dos_close(f); return 1;
    }
    if (get32(h + 18) > MAX_W || height > MAX_H) { dos_close(f); return 2; }
    hh = (int)height;
    load_palette();
    if (bpp <= 8) {
        if (ncol <= 0 || ncol > (1 << bpp)) ncol = 1 << bpp;
        mem_set(pmap, WHITE, 256);
        dos_seek(f, 14 + hsize, 0);
        if (dos_read_far(f, tabs, ROWBUF_OFF, ncol * 4) != ncol * 4) { dos_close(f); return 1; }
        for (i = 0; i < ncol; i++) pmap[i] = nearest(rowbuf[i * 4 + 2] >> 2, rowbuf[i * 4 + 1] >> 2, rowbuf[i * 4] >> 2);
    }
    if (!make_room()) { dos_close(f); return 3; }
    if (!img_alloc(&n, w, hh)) {
        img_free(&cv);
        if (!img_alloc(&n, w, hh)) {
            if (img_alloc(&cv, DEF_W, DEF_H)) img_fill(&cv, WHITE);
            dos_close(f);
            return 3;
        }
    }
    cache = 0;
    if (bpp > 8) { cache = dos_alloc(0x800); if (cache) ffill(cache, 0, 0x8000u, 0xFF); }
    rowbytes = (((u32)w * bpp + 31) / 32) * 4;
    step = bpp / 8;
    dos_seek(f, off, 0);
    for (k = 0; k < hh; k++) {
        y = top ? k : hh - 1 - k;
        d = rowp(&n, y);
        x = 0;
        for (pos = 0; pos < rowbytes && !r; ) {
            u16 chunk = (u16)(rowbytes - pos > ROWBUF_BYTES ? ROWBUF_BYTES : rowbytes - pos);
            if (dos_read_far(f, tabs, ROWBUF_OFF, chunk) != (int)chunk) { r = 4; break; }
            pos += chunk;
            if (bpp > 8) {
                for (i = 0; i + step <= (int)chunk && x < w; i += step) d[x++] = map_rgb(rowbuf[i + 2], rowbuf[i + 1], rowbuf[i]);
            } else if (bpp == 8) {
                for (; x < w; x++) d[x] = pmap[rowbuf[x]];
            } else if (bpp == 4) {
                for (; x < w; x++) d[x] = pmap[(rowbuf[x >> 1] >> ((x & 1) ? 0 : 4)) & 15];
            } else {
                for (; x < w; x++) d[x] = pmap[(rowbuf[x >> 3] >> (7 - (x & 7))) & 1];
            }
        }
        if (r) { for (; k < hh; k++) { y = top ? k : hh - 1 - k; ffill(rseg(&n, y), roff(&n, y), w, WHITE); } break; }
    }
    dos_close(f);
    if (cache) dos_free(cache);
    cache = 0;
    img_free(&cv);
    cv = n;
    sel_on = 0; scx = scy = 0;
    str_ncopy(path, p, PATH_MAX);
    dirty = 0;
    update_title();
    app_log("[PAINT] opened", p);
    fmt_u32(t, (u32)w); str_cat(t, "x"); fmt_u32(t + str_len(t), (u32)hh);
    app_log("[PAINT] size", t);
    if (r) str_copy(status, "The file ends early: the missing part is white.");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Tools                                                               */
#define T_SELECT  0
#define T_ERASER  1
#define T_FILL    2
#define T_PICK    3
#define T_ZOOM    4
#define T_PENCIL  5
#define T_BRUSH   6
#define T_AIR     7
#define T_TEXT    8
#define T_LINE    9
#define T_CURVE   10
#define T_RECT    11
#define T_POLY    12
#define T_ELLIPSE 13
#define T_RRECT   14
#define NTOOLS    15
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

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
#define TB_W  60
#define CB_H  44
#define SB_H  20
#define OPT_W 52
#define OPT_H 80
static int show_tools = 1, show_colors = 1, show_status = 1, show_grid;
static int X0, Y0, W, H, tbx, tby, optx, opty, cby, sby, vx, vy, vw, vh;
static void layout(void)
{
    int top, bottom, left;
    X0 = HOST.x + 3; Y0 = HOST.y + TITLE_H; W = HOST.w - 6; H = HOST.h - TITLE_H - 3;
    top = Y0 + MENUBAR_H; bottom = Y0 + H; left = X0;
    if (show_status) { sby = bottom - SB_H; bottom = sby; }
    if (show_colors) { cby = bottom - CB_H; bottom = cby; }
    if (show_tools) { tbx = X0; tby = top; left = X0 + TB_W; optx = tbx + 4; opty = tby + 4 + 8 * 26 + 4; }
    vx = left + 1; vy = top + 1;
    vw = imax(24, X0 + W - 16 - vx); vh = imax(24, bottom - 16 - vy);
}
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
static int in_view(int sx, int sy) { return sx >= vx && sx < vx + vw && sy >= vy && sy < vy + vh; }

/* Show the changed part of the picture (and the status bar) again. */
static int ptr_x = -1, ptr_y, ui_changed;
static void damage_status(void) { if (show_status) ui_damage(X0, sby, W, SB_H); }
static void flush(void)
{
    int x0, y0, x1, y1;
    if (dy1 >= dy0 && dx1 >= dx0) {
        x0 = imax(i2sx(dx0) - 2, vx); y0 = imax(i2sy(dy0) - 2, vy);
        x1 = imin(i2sx(dx1 + 1) + 2, vx + vw); y1 = imin(i2sy(dy1 + 1) + 2, vy + vh);
        if (x1 > x0 && y1 > y0) ui_damage(x0, y0, x1 - x0, y1 - y0);
    }
    dx0 = dy0 = 0; dx1 = dy1 = -1;
    damage_status();
}

/* ------------------------------------------------------------------ */
/* Menus                                                               */
enum {
    M_NEW = 1, M_OPEN, M_SAVE, M_SAVEAS, M_EXIT, M_UNDO, M_REDO, M_CUT, M_COPY, M_PASTE,
    M_CLEARSEL, M_SELALL, M_TOOLBOX, M_COLORBOX, M_STATUS, M_ZOOMIN, M_ZOOMOUT, M_Z1, M_Z2,
    M_Z4, M_Z6, M_Z8, M_GRID, M_FONT, M_FLIP, M_INVERT, M_ATTR, M_CLEARIMG, M_OPAQUE,
    M_HELP, M_ABOUT
};
static struct menu_item file_items[] = {
    { "&New", "Ctrl+N", M_NEW, 0 }, { "&Open...", "Ctrl+O", M_OPEN, 0 },
    { "&Save", "Ctrl+S", M_SAVE, 0 }, { "Save &As...", 0, M_SAVEAS, 0 },
    { "", 0, 0, MI_SEP }, { "E&xit", "Alt+F4", M_EXIT, 0 } };
static struct menu_item edit_items[] = {
    { "&Undo", "Ctrl+Z", M_UNDO, 0 }, { "&Repeat", "Ctrl+Y", M_REDO, 0 }, { "", 0, 0, MI_SEP },
    { "Cu&t", "Ctrl+X", M_CUT, 0 }, { "&Copy", "Ctrl+C", M_COPY, 0 }, { "&Paste", "Ctrl+V", M_PASTE, 0 },
    { "C&lear Selection", "Del", M_CLEARSEL, 0 }, { "Select &All", "Ctrl+A", M_SELALL, 0 } };
static struct menu_item view_items[] = {
    { "&Tool Box", "Ctrl+T", M_TOOLBOX, 0 }, { "&Colour Box", "Ctrl+L", M_COLORBOX, 0 },
    { "&Status Bar", 0, M_STATUS, 0 }, { "", 0, 0, MI_SEP },
    { "Zoom &In", "Ctrl+PgDn", M_ZOOMIN, 0 }, { "Zoom &Out", "Ctrl+PgUp", M_ZOOMOUT, 0 },
    { "&1x (Actual Size)", 0, M_Z1, 0 }, { "&2x", 0, M_Z2, 0 }, { "&4x", 0, M_Z4, 0 },
    { "&6x", 0, M_Z6, 0 }, { "&8x", 0, M_Z8, 0 }, { "", 0, 0, MI_SEP },
    { "Show &Grid", "Ctrl+G", M_GRID, 0 }, { "Text &Font...", 0, M_FONT, 0 } };
static struct menu_item image_items[] = {
    { "&Flip/Rotate...", "Ctrl+R", M_FLIP, 0 }, { "&Invert Colours", "Ctrl+I", M_INVERT, 0 },
    { "&Attributes...", "Ctrl+E", M_ATTR, 0 }, { "&Clear Image", 0, M_CLEARIMG, 0 },
    { "", 0, 0, MI_SEP }, { "&Draw Opaque", 0, M_OPAQUE, 0 } };
static struct menu_item help_items[] = {
    { "&Help Topics", "F1", M_HELP, 0 }, { "", 0, 0, MI_SEP }, { "&About CiukPaint", 0, M_ABOUT, 0 } };
static struct menu menus[] = {
    { "&File", file_items, 6 }, { "&Edit", edit_items, 8 }, { "&View", view_items, 14 },
    { "&Image", image_items, 6 }, { "&Help", help_items, 3 } };
static struct menubar bar = { menus, 5, 0, 0, 0, -1, 0 };
static struct menu_item ctx_items[] = {
    { "Cu&t", 0, M_CUT, 0 }, { "&Copy", 0, M_COPY, 0 }, { "&Paste", 0, M_PASTE, 0 },
    { "C&lear Selection", 0, M_CLEARSEL, 0 }, { "Select &All", 0, M_SELALL, 0 }, { "", 0, 0, MI_SEP },
    { "&Flip/Rotate...", 0, M_FLIP, 0 }, { "&Invert Colours", 0, M_INVERT, 0 } };
static struct popup ctx;
static void set_flag(struct menu_item *it, int f, int on) { if (on) it->flags |= f; else it->flags &= ~f; }
static void update_menus(void)
{
    int i;
    set_flag(&edit_items[0], MI_DISABLED, !(op_open || (undo_kind && !undone)));
    set_flag(&edit_items[1], MI_DISABLED, !(undo_kind && undone && !op_open));
    set_flag(&edit_items[3], MI_DISABLED, !sel_on);
    set_flag(&edit_items[4], MI_DISABLED, !sel_on);
    set_flag(&edit_items[5], MI_DISABLED, !clip.n);
    set_flag(&edit_items[6], MI_DISABLED, !sel_on);
    for (i = 0; i < 5; i++) ctx_items[i].flags = edit_items[i + 3].flags;
    set_flag(&view_items[0], MI_CHECKED, show_tools);
    set_flag(&view_items[1], MI_CHECKED, show_colors);
    set_flag(&view_items[2], MI_CHECKED, show_status);
    for (i = 0; i < 5; i++) set_flag(&view_items[6 + i], MI_CHECKED, zoom == zooms[i]);
    set_flag(&view_items[12], MI_CHECKED, show_grid);
    set_flag(&view_items[12], MI_DISABLED, zoom < 4);
    set_flag(&image_items[5], MI_CHECKED, opaque);
}

/* ------------------------------------------------------------------ */
/* Dialogs                                                             */
enum { D_NONE, D_MSG, D_SAVE_PROMPT, D_OPEN, D_SAVEAS, D_OVERWRITE, D_ATTR, D_FLIP, D_FONT,
       D_HELP, D_ABOUT };
enum { A_NONE, A_NEW, A_OPEN, A_EXIT, A_OPEN_PATH };
static struct dialog dlg;
static int dlg_kind, pending, closing;
static char pending_path[PATH_MAX];
static struct dctl dc[24];
static struct field f1, f2;
static char b1[PATH_MAX], b2[8], b3[8], dl0[64], dl1[64];
static char msg_buf[200];
static void ctl(int i, int type, int x, int y, int w, int h, const char *t, int id)
{
    mem_set(&dc[i], 0, sizeof dc[i]);
    dc[i].type = type; dc[i].x = x; dc[i].y = y; dc[i].w = w; dc[i].h = h;
    dc[i].text = t; dc[i].id = id;
}
static void message(const char *text)
{
    msgbox(&dlg, "CiukPaint", text, "OK");
    dlg_kind = D_MSG;
    app_log("[PAINT] message", text);
}
static void log_memory(void)
{
    char t[16];
    fmt_u32(t, (u32)dos_largest() * 16);
    app_log("[PAINT] free memory", t);
}
static void no_memory(void)
{
    app_sound(5);
    log_memory();
    message("There is not enough memory for this.\nMake the picture smaller, or close\nother programs, and try again.");
}
static const char *file_name(const char *p)
{
    const char *n = p;
    while (*p) { if (*p == '\\' || *p == ':') n = p + 1; p++; }
    return n;
}
static void update_title(void)
{
    str_copy(app_title, path[0] ? file_name(path) : "Untitled");
    str_cat(app_title, " - CiukPaint");
}
static void save_prompt(int action)
{
    str_copy(msg_buf, "Save changes to ");
    str_cat(msg_buf, path[0] ? file_name(path) : "Untitled");
    str_cat(msg_buf, "?");
    msgbox(&dlg, "CiukPaint", msg_buf, "Yes|No|Cancel");
    dlg_kind = D_SAVE_PROMPT;
    pending = action;
    app_log("[PAINT] dialog", "save changes");
}
static void wh_text(char *d, int w, int h)
{
    fmt_u32(d, (u32)w); str_cat(d, " x "); fmt_u32(d + str_len(d), (u32)h);
}
static void attributes_dialog(void)
{
    char t[8];
    fmt_u32(t, (u32)cv.w); field_set(&f1, b2, 6, t);
    fmt_u32(t, (u32)cv.h); field_set(&f2, b3, 6, t);
    f1.sel = 1;
    str_copy(dl0, "Picture: "); wh_text(dl0 + 9, cv.w, cv.h); str_cat(dl0, " pixels");
    str_copy(dl1, "Free memory: "); fmt_size(dl1 + 13, (u32)dos_largest() * 16);
    ctl(0, DC_LABEL, 0, 0, 0, 16, dl0, 0);
    ctl(1, DC_LABEL, 0, 20, 0, 16, dl1, 0);
    ctl(2, DC_LABEL, 0, 54, 0, 16, "&Width:", 0);
    ctl(3, DC_FIELD, 58, 50, 70, 22, 0, 0); dc[3].field = &f1;
    ctl(4, DC_LABEL, 146, 54, 0, 16, "&Height:", 0);
    ctl(5, DC_FIELD, 208, 50, 70, 22, 0, 0); dc[5].field = &f2;
    ctl(6, DC_LABEL, 0, 86, 0, 16, "Units: pixels. Colours: the desktop palette.", 0);
    ctl(7, DC_BUTTON, 300, 0, 84, 24, "OK", 1);
    ctl(8, DC_BUTTON, 300, 30, 84, 24, "Cancel", 2);
    ctl(9, DC_BUTTON, 300, 60, 84, 24, "&Default", 3);
    dialog_show(&dlg, "Attributes", dc, 10, 400, 112, 1, 2);
    dlg.focus = 3;
    dlg_kind = D_ATTR;
    app_log("[PAINT] dialog", "Attributes");
}
static void flip_dialog(void)
{
    ctl(0, DC_GROUP, 0, 0, 260, 120, "Flip or rotate", 0);
    ctl(1, DC_RADIO, 12, 22, 0, 17, "Flip &horizontal", 0); dc[1].group = 1; dc[1].value = 1;
    ctl(2, DC_RADIO, 12, 44, 0, 17, "Flip &vertical", 0); dc[2].group = 1;
    ctl(3, DC_RADIO, 12, 66, 0, 17, "&Rotate by angle", 0); dc[3].group = 1;
    ctl(4, DC_RADIO, 36, 92, 0, 17, "&90", 0); dc[4].group = 2; dc[4].value = 1;
    ctl(5, DC_RADIO, 100, 92, 0, 17, "&180", 0); dc[5].group = 2;
    ctl(6, DC_RADIO, 172, 92, 0, 17, "&270", 0); dc[6].group = 2;
    ctl(7, DC_BUTTON, 276, 6, 84, 24, "OK", 1);
    ctl(8, DC_BUTTON, 276, 36, 84, 24, "Cancel", 2);
    dialog_show(&dlg, "Flip and Rotate", dc, 9, 376, 126, 1, 2);
    dlg_kind = D_FLIP;
    app_log("[PAINT] dialog", "Flip and Rotate");
}
#define NFONTS 10
static char font_files[NFONTS][13], font_names[NFONTS][32];
static int nfonts;
static void font_dialog(void)
{
    struct dos_find fd;
    char p[40];
    u8 h[40];
    int r, i, k, fh, cur = 0;
    nfonts = 0;
    str_copy(p, font_dir); str_cat(p, "*.CFN");
    dos_set_dta(&fd);
    for (r = dos_find_first(p, A_ARCH | A_RDONLY); r >= 0 && nfonts < NFONTS; r = dos_find_next())
        str_ncopy(font_files[nfonts++], fd.name, 13);
    if (!nfonts) { message("No font was found in C:\\SYSTEM\\FONTS."); return; }
    for (i = 0; i < nfonts; i++) {
        str_copy(p, font_dir); str_cat(p, font_files[i]);
        str_copy(font_names[i], font_files[i]);
        fh = dos_open(p, 0);
        if (fh >= 0) {
            if (dos_read(fh, h, 40) == 40 && !mem_cmp(h, "CIUKFNT1", 8)) str_ncopy(font_names[i], (char *)h + 8, 32);
            dos_close(fh);
        }
        if (!str_icmp(font_files[i], font_file[0] ? font_file : "CIUKIOS.CFN")) cur = i;
    }
    k = 0;
    ctl(k++, DC_LABEL, 0, 0, 0, 16, "&Font:", 0);
    for (i = 0; i < nfonts; i++, k++) {
        ctl(k, DC_RADIO, (i % 2) * 210, 22 + (i / 2) * 22, 0, 17, font_names[i], 0);
        dc[k].group = 1; dc[k].value = i == cur;
    }
    r = 22 + ((nfonts + 1) / 2) * 22 + 8;
    ctl(k++, DC_GROUP, 0, r, 300, 46, "Size", 0);
    for (i = 0; i < 4; i++, k++) {
        static const char *const sizes[4] = { "&1x", "&2x", "&3x", "&4x" };
        ctl(k, DC_RADIO, 12 + i * 70, r + 20, 0, 17, sizes[i], 0);
        dc[k].group = 2; dc[k].value = text_scale == i + 1;
    }
    ctl(k, DC_CHECK, 316, r + 20, 0, 17, "&Bold", 0); dc[k].value = text_bold; k++;
    ctl(k++, DC_BUTTON, 250, r + 58, 84, 24, "OK", 1);
    ctl(k++, DC_BUTTON, 340, r + 58, 84, 24, "Cancel", 2);
    dialog_show(&dlg, "Text Font", dc, k, 436, r + 88, 1, 2);
    dlg_kind = D_FONT;
    app_log("[PAINT] dialog", "Text Font");
}
static void font_result(void)
{
    int i, k = 1;
    for (i = 0; i < nfonts; i++) if (dc[k + i].value) str_copy(font_file, font_files[i]);
    k += nfonts + 1;
    for (i = 0; i < 4; i++) if (dc[k + i].value) text_scale = i + 1;
    text_bold = dc[k + 4].value;
    font_load();
    app_log("[PAINT] font", font_file[0] ? font_file : "CIUKIOS.CFN");
    if (text_on) text_render();
}
static const char *const help_lines[] = {
    "Choose a tool in the tool box; its options are shown below it.",
    "Left button: the foreground colour. Right button: the background colour.",
    "Colour box: a left click sets the foreground, a right click the background.",
    "Select: drag to select, drag inside to move it (Ctrl+drag: a copy).",
    "Right-click a selection, or with Select, for the edit commands.",
    "Text: click, type, then click again to place it. Esc cancels it.",
    "Curve: draw a line, then drag twice to bend it.",
    "Polygon: drag the first side, click each corner, double-click to close.",
    "Eraser with the right button: only the foreground colour is erased.",
    "Shapes use the width chosen for the Line tool.",
    "Ctrl+Z Undo  Ctrl+Y Repeat  Ctrl+X Cut  Ctrl+C Copy  Ctrl+V Paste",
    "Ctrl+A Select All  Del Clear  Ctrl+I Invert  Ctrl+R Flip/Rotate",
    "Ctrl+E Attributes  Ctrl+T Tool Box  Ctrl+L Colour Box  Ctrl+G Grid",
    "Ctrl+PgDn / Ctrl+PgUp zoom. Arrows scroll, or move a selection." };
#define HELP_LINES 14
static void help_dialog(void)
{
    int i;
    for (i = 0; i < HELP_LINES; i++) ctl(i, DC_LABEL, 0, i * 18, 0, 16, help_lines[i], 0);
    ctl(i, DC_BUTTON, 236, HELP_LINES * 18 + 10, 84, 24, "OK", 1);
    dialog_show(&dlg, "CiukPaint Help", dc, HELP_LINES + 1, 570, HELP_LINES * 18 + 40, 1, 1);
    dlg_kind = D_HELP;
    app_log("[PAINT] dialog", "Help");
}
static void about_dialog(void)
{
    ctl(0, DC_LABEL, 50, 0, 0, 16, "CiukPaint", 0);
    ctl(1, DC_LABEL, 50, 20, 0, 16, "Version 0.8.3", 0);
    ctl(2, DC_LABEL, 50, 40, 0, 16, "A modern Retro OS", 0);
    ctl(3, DC_LABEL, 50, 68, 0, 16, "Pictures up to 2048 x 1200 pixels, as memory allows.", 0);
    ctl(4, DC_LABEL, 50, 88, 0, 16, "Opens 1, 4, 8, 24 and 32-bit BMP files; saves 8-bit BMP.", 0);
    ctl(5, DC_LABEL, 50, 108, 0, 16, "The right button draws with the background colour.", 0);
    ctl(6, DC_BUTTON, 190, 138, 84, 24, "OK", 1);
    dialog_show(&dlg, "About CiukPaint", dc, 7, 470, 168, 1, 1);
    dlg_kind = D_ABOUT;
    app_log("[PAINT] dialog", "About CiukPaint");
}

/* ---- Open / Save As (as CiukNote's) ---- */
#define FD_MAX 160
static struct { char name[13]; u8 dir; } fd_list[FD_MAX];
static int fd_count, fd_sel, fd_top, fd_all;
static char fd_dir[PATH_MAX];
static unsigned fd_click_tick;
static int fd_click_item = -1;
#define FD_LIST_Y 30
#define FD_LIST_W 440
#define FD_ROWS   9
static int fd_less(int a, int b)
{
    if (fd_list[a].dir != fd_list[b].dir) return fd_list[a].dir > fd_list[b].dir;
    return str_icmp(fd_list[a].name, fd_list[b].name) < 0;
}
static void fd_load(void)
{
    char pat[PATH_MAX + 8];
    struct dos_find f;
    int r, i, j;
    fd_count = 0; fd_sel = -1; fd_top = 0;
    str_copy(pat, fd_dir);
    if (pat[str_len(pat) - 1] != '\\') str_cat(pat, "\\");
    str_cat(pat, "*.*");
    dos_set_dta(&f);
    for (r = dos_find_first(pat, A_DIR); r >= 0 && fd_count < FD_MAX; r = dos_find_next()) {
        int len = str_len(f.name);
        if (f.attr & A_VOLUME) continue;
        if (f.name[0] == '.' && (f.name[1] == 0 || (f.name[1] == '.' && f.name[2] == 0))) continue;
        if (!(f.attr & A_DIR) && !fd_all && (len < 4 || str_icmp(f.name + len - 4, ".BMP"))) continue;
        str_copy(fd_list[fd_count].name, f.name);
        fd_list[fd_count].dir = (f.attr & A_DIR) ? 1 : 0;
        fd_count++;
    }
    for (i = 1; i < fd_count; i++)
        for (j = i; j > 0 && fd_less(j, j - 1); j--) {
            char t[14];
            u8 d;
            str_copy(t, fd_list[j].name); d = fd_list[j].dir;
            str_copy(fd_list[j].name, fd_list[j - 1].name); fd_list[j].dir = fd_list[j - 1].dir;
            str_copy(fd_list[j - 1].name, t); fd_list[j - 1].dir = d;
        }
}
static void fd_open(int save)
{
    if (path[0]) {
        const char *n = file_name(path);
        str_ncopy(fd_dir, path, (int)(n - path) + 1);
        if (str_len(fd_dir) > 3 && fd_dir[str_len(fd_dir) - 1] == '\\') fd_dir[str_len(fd_dir) - 1] = 0;
        field_set(&f1, b1, PATH_MAX - 4, save ? n : "");
    } else {
        if (!fd_dir[0]) {
            fd_dir[0] = (char)('A' + dos_get_drive());
            fd_dir[1] = ':'; fd_dir[2] = '\\';
            dos_getcwd(0, fd_dir + 3);
        }
        field_set(&f1, b1, PATH_MAX - 4, save ? "untitled.bmp" : "");
    }
    f1.sel = 1;
    fd_all = 0;
    fd_load();
    ctl(0, DC_LABEL, 0, 4, 0, 16, "Look in:", 0);
    ctl(1, DC_LABEL, 64, 4, 0, 16, fd_dir, 0);
    ctl(2, DC_BUTTON, 380, 0, 60, 22, "&Up", 5);
    ctl(3, DC_LABEL, 0, FD_LIST_Y + FD_ROWS * 18 + 12, 0, 16, "File &name:", 0);
    ctl(4, DC_FIELD, 96, FD_LIST_Y + FD_ROWS * 18 + 8, 250, 22, 0, 0); dc[4].field = &f1;
    ctl(5, DC_LABEL, 0, FD_LIST_Y + FD_ROWS * 18 + 40, 0, 16, "Files of type:", 0);
    ctl(6, DC_RADIO, 96, FD_LIST_Y + FD_ROWS * 18 + 40, 0, 17, "&Bitmap (*.bmp)", 0);
    dc[6].group = 1; dc[6].value = 1;
    ctl(7, DC_RADIO, 250, FD_LIST_Y + FD_ROWS * 18 + 40, 0, 17, "All &Files", 0);
    dc[7].group = 1;
    ctl(8, DC_BUTTON, 360, FD_LIST_Y + FD_ROWS * 18 + 6, 80, 24, save ? "&Save" : "&Open", 1);
    ctl(9, DC_BUTTON, 360, FD_LIST_Y + FD_ROWS * 18 + 64, 80, 24, "Cancel", 2);
    dialog_show(&dlg, save ? "Save As" : "Open", dc, 10, 456, FD_LIST_Y + FD_ROWS * 18 + 94, 1, 2);
    app_log("[PAINT] dialog", save ? "Save As" : "Open");
    dlg.focus = 4;
    dlg_kind = save ? D_SAVEAS : D_OPEN;
}
static void fd_list_origin(int *x, int *y)
{
    *x = dlg.x + 8;
    *y = dlg.y + DIALOG_TITLE_H + 6 + FD_LIST_Y;
}
static void fd_draw(void)
{
    int x, y, i;
    fd_list_origin(&x, &y);
    ui_inset(x, y, FD_LIST_W, FD_ROWS * 18 + 4);
    for (i = 0; i < FD_ROWS && fd_top + i < fd_count; i++) {
        int k = fd_top + i, ry = y + 2 + i * 18, c = C_INK;
        if (k == fd_sel) { ui_rect(x + 2, ry, FD_LIST_W - 20, 18, C_TITLE); c = C_PAPER; }
        if (fd_list[k].dir) {
            ui_rect(x + 6, ry + 5, 12, 9, C_YELLOW);
            ui_rect(x + 6, ry + 3, 5, 2, C_YELLOW);
        } else {
            ui_rect(x + 6, ry + 3, 13, 11, C_SHADOW);
            ui_rect(x + 7, ry + 4, 11, 9, C_PAPER);
            ui_rect(x + 8, ry + 9, 4, 3, C_GREEN);
            ui_rect(x + 12, ry + 7, 4, 5, C_BLUE);
            ui_rect(x + 14, ry + 5, 2, 2, C_YELLOW);
        }
        ui_text(x + 24, ry + 1, fd_list[k].name, c);
    }
    draw_scroll(x + FD_LIST_W - 18, y + 2, FD_ROWS * 18, fd_top, fd_count, FD_ROWS);
}
static void fd_enter(const char *name)
{
    if (!str_cmp(name, "..")) {
        int n = str_len(fd_dir);
        while (n > 3 && fd_dir[n - 1] != '\\') n--;
        if (n > 3) n--;
        fd_dir[n] = 0;
        if (n == 2) { fd_dir[2] = '\\'; fd_dir[3] = 0; }
    } else {
        if (fd_dir[str_len(fd_dir) - 1] != '\\') str_cat(fd_dir, "\\");
        str_cat(fd_dir, name);
    }
    fd_load();
}
static void run_pending(void);
static void open_error(int r)
{
    if (r == 1) message("CiukPaint cannot read this file.\nIt opens uncompressed BMP files\n(1, 4, 8, 24 and 32-bit colour).");
    else if (r == 2) message("This picture is too large for CiukPaint.\nThe limit is 2048 x 1200 pixels.");
    else if (r == 3) no_memory();
    else if (r) message(dos_error_text(r));
}
static void do_save(const char *p)
{
    int r = save_bmp(p);
    if (r) { pending = A_NONE; message(dos_error_text(r)); return; }
    if (pending) run_pending();
}
static void fd_accept(void)
{
    char full[PATH_MAX + 16];
    int i, has_dot = 0, attr;
    const char *n = f1.text;
    if (!n[0]) return;
    for (i = 0; n[i]; i++) {
        if (n[i] == '*' || n[i] == '?') { fd_all = str_icmp(n, "*.bmp") != 0; dc[6].value = !fd_all; dc[7].value = fd_all; fd_load(); return; }
        if (n[i] == '.') has_dot = 1;
        if (n[i] == '\\') has_dot = 0;
    }
    if (n[1] == ':' || n[0] == '\\') str_ncopy(full, n, PATH_MAX);
    else {
        str_copy(full, fd_dir);
        if (full[str_len(full) - 1] != '\\') str_cat(full, "\\");
        str_cat(full, n);
    }
    for (i = 0; full[i]; i++) full[i] = to_upper(full[i]);
    attr = dos_get_attr(full);
    if (attr >= 0 && (attr & A_DIR)) {
        str_ncopy(fd_dir, full, PATH_MAX);
        f1.text[0] = 0; f1.len = f1.cursor = 0;
        fd_load();
        return;
    }
    if (dlg_kind == D_OPEN) {
        int r;
        if (attr < 0) {
            str_copy(msg_buf, full);
            str_cat(msg_buf, "\nThe file was not found.\nCheck the name and try again.");
            message(msg_buf);
            return;
        }
        dlg.open = 0;
        dlg_kind = D_NONE;
        r = load_bmp(full);
        open_error(r);
        return;
    }
    if (!has_dot && !fd_all) str_cat(full, ".BMP");
    if (str_len(full) >= PATH_MAX) { message("The name is too long."); return; }
    if (dos_get_attr(full) >= 0) {
        str_ncopy(pending_path, full, PATH_MAX);
        str_copy(msg_buf, file_name(full));
        str_cat(msg_buf, " already exists.\nDo you want to replace it?");
        msgbox(&dlg, "Save As", msg_buf, "Yes|No");
        dlg_kind = D_OVERWRITE;
        return;
    }
    dlg.open = 0;
    dlg_kind = D_NONE;
    do_save(full);
}
static int fd_mouse(int kind, int sx, int sy)
{
    int x, y, i;
    fd_list_origin(&x, &y);
    if (kind != MOUSE_DOWN) return 0;
    if (sx >= x + FD_LIST_W - 18 && sx < x + FD_LIST_W - 2 && sy >= y + 2 && sy < y + 2 + FD_ROWS * 18) {
        long p = scroll_hit(x + FD_LIST_W - 18, y + 2, FD_ROWS * 18, sy, fd_count, FD_ROWS);
        if (p == -1 && fd_top > 0) fd_top--;
        else if (p == -2 && fd_top + FD_ROWS < fd_count) fd_top++;
        else if (p >= 16) fd_top = (int)(p - 16);
        return 1;
    }
    if (sx < x || sx >= x + FD_LIST_W - 18 || sy < y + 2 || sy >= y + 2 + FD_ROWS * 18) return 0;
    i = fd_top + (sy - y - 2) / 18;
    if (i >= fd_count) return 1;
    if (i == fd_click_item && HOST.ticks - fd_click_tick < (unsigned)HOST.dblclick) {
        fd_click_item = -1;
        if (fd_list[i].dir) { fd_enter(fd_list[i].name); return 1; }
        field_set(&f1, b1, PATH_MAX - 4, fd_list[i].name);
        fd_accept();
        return 1;
    }
    fd_click_item = i;
    fd_click_tick = HOST.ticks;
    fd_sel = i;
    if (!fd_list[i].dir) field_set(&f1, b1, PATH_MAX - 4, fd_list[i].name);
    dlg.focus = 4;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Picture commands                                                    */
static int curve_stage, drag, drag_mode;
static void finish_pending(void);
static void log_size(const char *what, int w, int h)
{
    char t[24];
    fmt_u32(t, (u32)w); str_cat(t, "x"); fmt_u32(t + str_len(t), (u32)h);
    app_log(what, t);
}
static int new_image(int w, int h)
{
    finish_pending();
    drop_selection();
    drop_undo();
    pool_free();
    if (cv.w != w || cv.h != h) {
        img_free(&cv);
        if (!img_alloc(&cv, w, h) && !img_alloc(&cv, DEF_W, DEF_H) && !img_alloc(&cv, 320, 240)) return 0;
    }
    img_fill(&cv, WHITE);
    path[0] = 0;
    dirty = 0;
    scx = scy = 0;
    update_title();
    log_size("[PAINT] new", cv.w, cv.h);
    return 1;
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
    ui_changed = 1;
    { char t[4]; fmt_u32(t, (u32)zoom); app_log("[PAINT] zoom", t); }
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
    app_log("[PAINT] tool", tool_names[t]);
}
static void run_pending(void)
{
    int p = pending;
    pending = A_NONE;
    if (p == A_NEW) new_image(cv.w, cv.h);
    else if (p == A_OPEN) fd_open(0);
    else if (p == A_EXIT) { closing = 1; app_close(); }
    else if (p == A_OPEN_PATH) open_error(load_bmp(pending_path));
}
static void ask_then(int action)
{
    finish_pending();
    drop_selection();
    if (dirty) save_prompt(action);
    else { pending = action; run_pending(); }
}
static void save(void)
{
    finish_pending();
    drop_selection();
    if (!path[0]) { fd_open(1); return; }
    do_save(path);
}
static void undo(int redo)
{
    if (!redo && op_open) {                 /* the operation in progress */
        if (text_on) { text_on = 0; }
        if (floating) { img_free(&flt); floating = 0; }
        npt = 0; curve_stage = 0;
        sel_on = 0;
        cancel_op();
        dmg_all();
        app_log("[PAINT] undo", 0);
        return;
    }
    if (!undo_kind || undone != redo) return;
    sel_on = 0;
    if (undo_kind == U_ROWS) swap_rows();
    else { struct img t = old; old = cv; cv = t; clamp_scroll(); }
    undone = !redo;
    dirty = 1;
    dmg_all();
    app_log(redo ? "[PAINT] redo" : "[PAINT] undo", 0);
}
static void paste(void)
{
    int y;
    if (!clip.n) return;
    finish_pending();
    drop_selection();
    if (!alloc_retry(&flt, clip.w, clip.h)) { no_memory(); return; }
    for (y = 0; y < clip.h; y++) fcopy(rseg(&flt, y), roff(&flt, y), rseg(&clip, y), roff(&clip, y), clip.w);
    begin_op(1);
    final_render = float_render;
    floating = 1; hole = 0;
    sel_on = 1; selx = scx; sely = scy; selw = clip.w; selh = clip.h;
    if (tool != T_SELECT) { prev_tool = tool; tool = T_SELECT; }
    float_render();
    sel_damage();
    log_size("[PAINT] paste", selw, selh);
}
static void command(int id)
{
    ui_changed = 1;
    switch (id) {
    case M_NEW: ask_then(A_NEW); break;
    case M_OPEN: ask_then(A_OPEN); break;
    case M_SAVE: save(); break;
    case M_SAVEAS: finish_pending(); drop_selection(); fd_open(1); break;
    case M_EXIT: ask_then(A_EXIT); break;
    case M_UNDO: undo(0); break;
    case M_REDO: undo(1); break;
    case M_CUT: if (sel_on && copy_selection()) clear_selection(); break;
    case M_COPY: if (sel_on) copy_selection(); break;
    case M_PASTE: paste(); break;
    case M_CLEARSEL: clear_selection(); break;
    case M_SELALL:
        finish_pending(); drop_selection();
        if (tool != T_SELECT) { prev_tool = tool; tool = T_SELECT; }
        sel_on = 1; selx = sely = 0; selw = cv.w; selh = cv.h;
        app_log("[PAINT] select all", 0);
        break;
    case M_TOOLBOX: show_tools = !show_tools; break;
    case M_COLORBOX: show_colors = !show_colors; break;
    case M_STATUS: show_status = !show_status; break;
    case M_ZOOMIN: set_zoom(zoom_step(1), scx + page_w() / 2, scy + page_h() / 2); break;
    case M_ZOOMOUT: set_zoom(zoom_step(-1), scx + page_w() / 2, scy + page_h() / 2); break;
    case M_Z1: case M_Z2: case M_Z4: case M_Z6: case M_Z8:
        set_zoom(zooms[id - M_Z1], scx + page_w() / 2, scy + page_h() / 2); break;
    case M_GRID: if (zoom >= 4) show_grid = !show_grid; break;
    case M_FONT: font_dialog(); break;
    case M_FLIP: finish_pending(); flip_dialog(); break;
    case M_INVERT: finish_pending(); transform(6); break;
    case M_ATTR: finish_pending(); drop_selection(); attributes_dialog(); break;
    case M_CLEARIMG:
        finish_pending(); drop_selection();
        begin_op(0); fill_rect(0, 0, cv.w - 1, cv.h - 1, bg); commit_op();
        app_log("[PAINT] image", "clear");
        break;
    case M_OPAQUE: opaque = !opaque; if (floating) float_render(); if (text_on) text_render(); break;
    case M_HELP: help_dialog(); break;
    case M_ABOUT: about_dialog(); break;
    }
    clamp_scroll();
}
static void dialog_result(int r)
{
    int kind = dlg_kind;
    if (r < 0) return;
    ui_changed = 1;
    if (kind == D_MSG || kind == D_ABOUT || kind == D_HELP) { dlg_kind = D_NONE; return; }
    if (kind == D_SAVE_PROMPT) {
        dlg_kind = D_NONE;
        if (r == MB_CANCEL) { pending = A_NONE; return; }
        if (r == MB_NO) { dirty = 0; run_pending(); return; }
        if (!path[0]) { fd_open(1); return; }
        do_save(path);
        return;
    }
    if (kind == D_OPEN || kind == D_SAVEAS) {
        if (r == 2) { dlg_kind = D_NONE; pending = A_NONE; return; }
        if (r == 5) { fd_enter(".."); dlg.open = 1; return; }
        dlg.open = 1;
        if (fd_all != dc[7].value) { fd_all = dc[7].value; fd_load(); }
        fd_accept();
        return;
    }
    if (kind == D_OVERWRITE) {
        dlg_kind = D_NONE;
        if (r == MB_YES) do_save(pending_path);
        else fd_open(1);
        return;
    }
    if (kind == D_ATTR) {
        int w = parse_u16(f1.text), h = parse_u16(f2.text);
        if (r == 3) {
            field_set(&f1, b2, 6, "512"); field_set(&f2, b3, 6, "384");
            dlg.open = 1;
            return;
        }
        dlg_kind = D_NONE;
        if (r != 1) return;
        if (w < 1 || h < 1 || w > MAX_W || h > MAX_H) {
            message("Enter a width from 1 to 2048 and\na height from 1 to 1200 pixels.");
            return;
        }
        if ((w != cv.w || h != cv.h) && resize_canvas(w, h)) {
            dmg_all();
            log_size("[PAINT] attributes", w, h);
        }
        return;
    }
    if (kind == D_FLIP) {
        dlg_kind = D_NONE;
        if (r != 1) return;
        transform(dc[1].value ? 1 : dc[2].value ? 2 : dc[4].value ? 3 : dc[5].value ? 4 : 5);
        return;
    }
    if (kind == D_FONT) {
        dlg_kind = D_NONE;
        if (r == 1) font_result();
    }
}

/* ------------------------------------------------------------------ */
/* Pointer on the picture                                              */
#define DM_NONE   0
#define DM_FREE   1
#define DM_SHAPE  2
#define DM_NEWSEL 3
#define DM_MOVE   4
#define DM_CURVE  5
#define DM_POLY   6
static int ax, ay, lx, ly, offx, offy, drag_btn, last_mx, last_my;
static int cvx[4], cvy[4], rbx, rby;         /* curve points; polygon rubber point */
static u8 c1, c2;
static unsigned air_tick, poly_tick;
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
    if (!(HOST.shift & SH_SHIFT)) return;
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
        { char t[8]; fmt_u32(t, (u32)npt); app_log("[PAINT] draw Polygon", t); }
    } else cancel_op();
    npt = 0;
}
static void curve_finish(void)
{
    if (!curve_stage) return;
    commit_op();
    curve_stage = 0;
    app_log("[PAINT] draw Curve", 0);
}
static void finish_pending(void)
{
    text_finish();
    poly_finish();
    curve_finish();
}
static void log_draw(int x0, int y0, int x1, int y1)
{
    char t[48];
    fmt_u32(t, (u32)x0); str_cat(t, ","); fmt_u32(t + str_len(t), (u32)y0);
    str_cat(t, "-"); fmt_u32(t + str_len(t), (u32)x1); str_cat(t, ","); fmt_u32(t + str_len(t), (u32)y1);
    str_copy(msg_buf, "[PAINT] draw ");
    str_cat(msg_buf, tool_names[tool]);
    app_log(msg_buf, t);
}
static void context_menu(int sx, int sy)
{
    update_menus();
    popup_open(&ctx, ctx_items, 8, sx, sy, HOST.x + HOST.w - 4, HOST.y + HOST.h - 4);
    app_log("[PAINT] menu", "context");
}
static int inside_sel(int x, int y) { return sel_on && x >= selx && x < selx + selw && y >= sely && y < sely + selh; }
static void tool_down(int x, int y, int btn)
{
    u8 c;
    c1 = btn == 1 ? fg : bg; c2 = btn == 1 ? bg : fg;
    ax = lx = x; ay = ly = y;
    drag_btn = btn;
    last_mx = HOST.mx; last_my = HOST.my;
    drag_mode = DM_NONE;
    set_tool_pen();
    switch (tool) {
    case T_SELECT:
        if (inside_sel(x, y)) {
            if (!floating && !lift((HOST.shift & SH_CTRL) != 0)) return;
            offx = x - selx; offy = y - sely;
            drag_mode = DM_MOVE;
        } else {
            drop_selection();
            x = imax(0, imin(x, cv.w - 1)); y = imax(0, imin(y, cv.h - 1));
            ax = x; ay = y;
            sel_on = 1; selx = x; sely = y; selw = selh = 1;
            drag_mode = DM_NEWSEL;
        }
        break;
    case T_ERASER:
        begin_op(0); stamp(x, y, bg); drag_mode = DM_FREE; break;
    case T_PENCIL: case T_BRUSH:
        begin_op(0); stamp(x, y, c1); drag_mode = DM_FREE; break;
    case T_AIR:
        begin_op(0); spray(x, y, c1); air_tick = HOST.ticks; drag_mode = DM_FREE; break;
    case T_FILL:
        begin_op(0); flood(x, y, c1); commit_op();
        { char t[16]; fmt_u32(t, (u32)x); str_cat(t, ","); fmt_u32(t + str_len(t), (u32)y); app_log("[PAINT] fill", t); }
        return;
    case T_PICK:
        if (x < 0 || y < 0 || x >= cv.w || y >= cv.h) return;
        c = pixel(x, y);
        if (btn == 1) fg = c; else bg = c;
        { char t[8]; fmt_u32(t, (u32)c); app_log(btn == 1 ? "[PAINT] picked fg" : "[PAINT] picked bg", t); }
        tool = prev_tool;
        ui_changed = 1;
        return;
    case T_ZOOM:
        set_zoom(zoom_step(btn == 1 ? 1 : -1), x, y);
        tool = prev_tool;
        ui_changed = 1;
        return;
    case T_TEXT:
        if (text_on) { text_finish(); ui_changed = 1; return; }
        if (!fnt_loaded && !font_load()) { message("No font was found in C:\\SYSTEM\\FONTS."); return; }
        begin_op(1);
        final_render = text_render;
        field_set(&text_edit, text, sizeof text, "");
        text_on = 1; tx = x; ty = y;
        ui_changed = 1;
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
            poly_tick = HOST.ticks;
            render_poly(0);
            drag_mode = DM_POLY;
            break;
        }
        c1 = shape_btn == 1 ? fg : bg; c2 = shape_btn == 1 ? bg : fg;
        if ((HOST.ticks - poly_tick < (unsigned)HOST.dblclick && iabs(x - rbx) <= 3 && iabs(y - rby) <= 3) ||
            (npt > 2 && iabs(x - ptx[0]) <= 3 && iabs(y - pty[0]) <= 3) || npt >= MAXPT - 1) {
            poly_finish();
            return;
        }
        poly_tick = HOST.ticks;
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
        if (tool == T_AIR) spray(x, y, c1);
        else line(lx, ly, x, y, tool == T_ERASER ? bg : c1);
        lx = x; ly = y;
        break;
    case DM_SHAPE: render_shape(x, y); break;
    case DM_NEWSEL:
        sel_damage();
        x = imax(0, imin(x, cv.w - 1)); y = imax(0, imin(y, cv.h - 1));
        selx = imin(ax, x); sely = imin(ay, y);
        selw = iabs(x - ax) + 1; selh = iabs(y - ay) + 1;
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
    case DM_FREE: commit_op(); pen_replace = -1; log_draw(ax, ay, x, y); break;
    case DM_SHAPE: commit_op(); log_draw(ax, ay, lx, ly); break;
    case DM_NEWSEL:
        if (selw < 2 && selh < 2) sel_on = 0;
        else {
            char t[40];
            fmt_u32(t, (u32)selx); str_cat(t, ","); fmt_u32(t + str_len(t), (u32)sely); str_cat(t, " ");
            wh_text(t + str_len(t), selw, selh);
            app_log("[PAINT] select", t);
        }
        break;
    case DM_MOVE:
        { char t[16]; fmt_u32(t, (u32)imax(0, selx)); str_cat(t, ","); fmt_u32(t + str_len(t), (u32)imax(0, sely)); app_log("[PAINT] moved", t); }
        break;
    case DM_CURVE:
        if (++curve_stage == 3) curve_finish();
        break;
    case DM_POLY:
        if (npt < MAXPT - 1 && (npt == 1 || rbx != ptx[npt - 1] || rby != pty[npt - 1])) { ptx[npt] = rbx; pty[npt] = rby; npt++; }
        break;
    }
    ui_changed = 1;
}

/* ------------------------------------------------------------------ */
/* Painting                                                            */
static u8 cbuf[64], gbuf[256];
/* A rectangle of any palette index (VGA maps 16-143 through the shell). */
static void fillc(int x, int y, int w, int h, u8 c)
{
    if (c < 16) { ui_rect(x, y, w, h, c); return; }
    w = imin(w, 64);
    mem_set(cbuf, c, w);
    ui_bitmap(x, y, w, h, app_seg(), (unsigned)cbuf, 0, 1);
}
static void vrect(int x, int y, int w, int h, int c)       /* clipped to the view */
{
    int x1 = imin(x + w, vx + vw), y1 = imin(y + h, vy + vh);
    x = imax(x, vx); y = imax(y, vy);
    if (x1 > x && y1 > y) ui_rect(x, y, x1 - x, y1 - y, c);
}
static void marquee(int x, int y, int w, int h)
{
    int i;
    vrect(x, y, w, 1, C_PAPER); vrect(x, y + h - 1, w, 1, C_PAPER);
    vrect(x, y, 1, h, C_PAPER); vrect(x + w - 1, y, 1, h, C_PAPER);
    for (i = 0; i < w; i += 8) { vrect(x + i, y, imin(4, w - i), 1, C_INK); vrect(x + i, y + h - 1, imin(4, w - i), 1, C_INK); }
    for (i = 0; i < h; i += 8) { vrect(x, y + i, 1, imin(4, h - i), C_INK); vrect(x + w - 1, y + i, 1, imin(4, h - i), C_INK); }
}
static void paint_canvas(void)
{
    int cols, rows, sx0 = vx + 4, sy0 = vy + 4, y, k, n, iw, ih, i;
    clamp_scroll();
    cols = imax(0, imin(cv.w - scx, (vw - 4) / zoom));
    rows = imax(0, imin(cv.h - scy, (vh - 4) / zoom));
    iw = cols * zoom; ih = rows * zoom;
    ui_rect(vx, vy, vw, 4, C_SHADOW);
    ui_rect(vx, vy + 4, 4, vh - 4, C_SHADOW);
    ui_rect(sx0 + iw, sy0, vw - 4 - iw, vh - 4, C_SHADOW);
    ui_rect(sx0, sy0 + ih, iw, vh - 4 - ih, C_SHADOW);
    for (y = scy; y < scy + rows; y += n) {
        k = y / cv.rps;
        n = imin((k + 1) * cv.rps - y, scy + rows - y);
        ui_bitmap(sx0, sy0 + (y - scy) * zoom, cols, n, cv.seg[k], roff(&cv, y) + scx, cv.w, zoom);
    }
    if (show_grid && zoom >= 4) {
        for (i = 1; i < cols; i++) ui_rect(sx0 + i * zoom, sy0, 1, ih, C_SHADOW);
        for (i = 1; i < rows; i++) ui_rect(sx0, sy0 + i * zoom, iw, 1, C_SHADOW);
    }
    if (sel_on) marquee(i2sx(selx) - 1, i2sy(sely) - 1, selw * zoom + 2, selh * zoom + 2);
    if (text_on && fnt_loaded) {
        int w, h, lw, a, b, px = tx, py = ty, cell, r, col, gw;
        u16 bits, base;
        text_extent(&w, &h, &lw);
        marquee(i2sx(tx) - 2, i2sy(ty) - 2, imax(w, 8) * zoom + 4, h * zoom + 4);
        a = imin(text_edit.cursor, text_edit.anchor);
        b = imax(text_edit.cursor, text_edit.anchor);
        /* Selection is a UI overlay; never write these colours into cv. */
        for (i = 0; i < text_len; i++) {
            cell = text[i] == '\n' ? 4 : glyph_w(text[i]);
            if (i >= a && i < b) {
                vrect(i2sx(px), i2sy(py), cell * text_scale * zoom, 16 * text_scale * zoom, C_TITLE);
                if (text[i] != '\n') {
                    base = 95 + (u16)(text[i] - 32) * 32;
                    gw = imin(16, cell);
                    for (r = 0; r < 16; r++) {
                        bits = peek16(fnt, base + r * 2);
                        if (fnt_synth) bits |= bits >> 1;
                        for (col = 0; col < gw; col++) if (bits & (0x8000u >> col))
                            vrect(i2sx(px + col * text_scale), i2sy(py + r * text_scale),
                                  text_scale * zoom, text_scale * zoom, C_PAPER);
                    }
                }
            }
            if (text[i] == '\n') { px = tx; py += 16 * text_scale; }
            else px += cell * text_scale;
        }
        text_position(text_edit.cursor, &px, &py);
        vrect(i2sx(px), i2sy(py), imax(1, zoom), 16 * text_scale * zoom, C_INK);
    }
}
static void hscroll_draw(int x, int y, int w, int pos, int total, int page)
{
    int track = w - 32, tw, tx0, i, idle = total <= page || track < 8, c = idle ? C_SHADOW : C_INK;
    ui_rect(x, y, w, 16, C_FACE);
    if (!idle) ui_rect(x + 16, y, w - 32, 16, C_LIGHT);
    ui_bevel(x, y, 16, 16, C_FACE);
    ui_bevel(x + w - 16, y, 16, 16, C_FACE);
    for (i = 0; i < 4; i++) {
        ui_rect(x + 5 + i, y + 8 - i, 1, 1 + 2 * i, c);
        ui_rect(x + w - 6 - i, y + 8 - i, 1, 1 + 2 * i, c);
    }
    if (idle) return;
    tw = (int)((long)track * page / total);
    if (tw < 12) tw = 12;
    tx0 = (int)((long)(track - tw) * pos / (total - page));
    ui_bevel(x + 16 + tx0, y, tw, 16, C_FACE);
}
static u8 glyph_color(char ch)
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
static void tool_xy(int i, int *x, int *y) { *x = tbx + 4 + (i & 1) * 26; *y = tby + 4 + (i >> 1) * 26; }
static void sunken(int x, int y, int w, int h, int fill)
{
    ui_rect(x, y, w, h, C_PAPER);
    ui_rect(x, y, w - 1, h - 1, C_SHADOW);
    ui_rect(x + 1, y + 1, w - 2, h - 2, fill);
}
static void opt_row(int i, int rh, int on)   /* highlight of an option row */
{
    if (on) ui_rect(optx + 2, opty + 2 + i * rh, OPT_W - 4, rh, C_TITLE);
}
static void paint_options(void)
{
    int ox = optx + 2, oy = opty + 2, i, k, on, c;
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
            fmt_u32(t, (u32)zooms[i]); str_cat(t, "x");
            ui_text(ox + 24 - ui_measure(t) / 2, oy + i * 15 - 1, t, on ? C_PAPER : C_INK);
        }
        break;
    case T_BRUSH:
        for (i = 0; i < 12; i++) {
            int cx = ox + (i % 3) * 16 + 8, cy = oy + (i / 3) * 19 + 9, kind = PEN_ROUND + i / 3, s = brush_px[i % 3];
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
    case T_SELECT: case T_TEXT:
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
static void paint_tools(void)
{
    int i, x, y, bottom = show_colors ? cby : show_status ? sby : Y0 + H;
    const char *s;
    ui_rect(tbx, tby, TB_W, bottom - tby, C_FACE);
    for (i = 0; i < NTOOLS; i++) {
        int k;
        tool_xy(i, &x, &y);
        if (i == tool) sunken(x, y, 25, 25, C_FACE);
        else if (i == tool_hot) ui_bevel(x, y, 25, 25, C_FACE);
        s = glyph_src[i];
        for (k = 0; k < 256; k++) gbuf[k] = glyph_color(s[k]);
        ui_bitmap(x + 4 + (i == tool), y + 4 + (i == tool), 16, 16, app_seg(), (unsigned)gbuf, 16, 1);
    }
    paint_options();
}
static void swatch_xy(int i, int *x, int *y) { *x = X0 + 50 + (i % 14) * 17; *y = cby + 6 + (i / 14) * 18; }
static void paint_colors(void)
{
    int i, x, y;
    ui_rect(X0, cby, W, CB_H, C_FACE);
    ui_rect(X0, cby, W, 1, C_PAPER);
    sunken(X0 + 6, cby + 5, 36, 35, C_FACE);
    ui_rect(X0 + 19, cby + 17, 18, 18, C_INK);
    fillc(X0 + 20, cby + 18, 16, 16, bg);
    ui_rect(X0 + 11, cby + 10, 18, 18, C_INK);
    fillc(X0 + 12, cby + 11, 16, 16, fg);
    for (i = 0; i < NSWATCH; i++) {
        swatch_xy(i, &x, &y);
        ui_inset(x, y, 16, 16);
        fillc(x + 2, y + 2, 13, 13, swatch[i]);
    }
}
static void panel(int x, int y, int w, int h)
{
    ui_rect(x, y, w, 1, C_SHADOW); ui_rect(x, y, 1, h, C_SHADOW);
    ui_rect(x, y + h - 1, w, 1, C_PAPER); ui_rect(x + w - 1, y, 1, h, C_PAPER);
}
static void paint_status(void)
{
    char t[40];
    int pw = 104, px = X0 + W - 2 * pw - 4;
    ui_rect(X0, sby, W, SB_H, C_FACE);
    panel(X0 + 2, sby + 1, px - X0 - 4, SB_H - 1);
    draw_frame_text(X0 + 6, sby + 2, px - X0 - 12, status[0] ? status : tool_hints[tool], C_INK);
    panel(px, sby + 1, pw, SB_H - 1);
    if (ptr_x >= 0) {
        fmt_u32(t, (u32)ptr_x); str_cat(t, ", "); fmt_u32(t + str_len(t), (u32)ptr_y);
        ui_text(px + 6, sby + 2, t, C_INK);
    }
    panel(px + pw + 2, sby + 1, pw, SB_H - 1);
    t[0] = 0;
    if (sel_on) wh_text(t, selw, selh);
    else if (drag_mode == DM_SHAPE) wh_text(t, iabs(lx - ax) + 1, iabs(ly - ay) + 1);
    else wh_text(t, cv.w, cv.h);
    ui_text(px + pw + 8, sby + 2, t, C_INK);
}
static int geo[8];
static void paint(void)
{
    int g[8], i, sx, sy;
    layout();
    update_menus();
    bar.x = X0; bar.y = Y0; bar.w = W;
    paint_canvas();
    ui_rect(vx - 1, vy - 1, vw + 17, 1, C_SHADOW);
    ui_rect(vx - 1, vy - 1, 1, vh + 17, C_SHADOW);
    draw_scroll(vx + vw, vy, vh, scy, cv.h, page_h());
    hscroll_draw(vx, vy + vh, vw, scx, cv.w, page_w());
    ui_rect(vx + vw, vy + vh, 16, 16, C_FACE);
    if (show_tools) paint_tools();
    if (show_colors) paint_colors();
    if (show_status) paint_status();
    menubar_draw(&bar);
    if (ctx.open) popup_draw(&ctx);
    /* Where things are, for the test gates. */
    swatch_xy(0, &sx, &sy);
    g[0] = i2sx(0); g[1] = i2sy(0); g[2] = tbx; g[3] = tby; g[4] = optx; g[5] = opty; g[6] = sx; g[7] = sy;
    if (mem_cmp(g, geo, sizeof g)) {
        char t[64];
        mem_copy(geo, g, sizeof g);
        t[0] = 0;
        for (i = 0; i < 8; i++) { fmt_u32(t + str_len(t), (u32)imax(0, g[i])); if (i < 7) str_cat(t, " "); }
        app_log("[PAINT] geometry", t);
    }
}
static void paint_dialog(void)
{
    dialog_draw(&dlg);
    if (dlg_kind == D_OPEN || dlg_kind == D_SAVEAS) fd_draw();
    if (dlg_kind == D_ABOUT) ui_icon(dlg.x + 10, dlg.y + DIALOG_TITLE_H + 8, ICON_PAINT);
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
static int tool_at(int sx, int sy)
{
    int i, x, y;
    if (!show_tools) return -1;
    for (i = 0; i < NTOOLS; i++) {
        tool_xy(i, &x, &y);
        if (sx >= x && sx < x + 25 && sy >= y && sy < y + 25) return i;
    }
    return -1;
}
static int swatch_at(int sx, int sy)
{
    int i, x, y;
    if (!show_colors) return -1;
    for (i = 0; i < NSWATCH; i++) {
        swatch_xy(i, &x, &y);
        if (sx >= x && sx < x + 16 && sy >= y && sy < y + 16) return i;
    }
    return -1;
}
static void option_click(int sx, int sy)
{
    int dx = sx - optx - 2, dy = sy - opty - 2;
    char t[24];
    if (dx < 0 || dy < 0 || dx >= OPT_W - 4 || dy >= OPT_H - 4) return;
    switch (tool) {
    case T_LINE: case T_CURVE: if (dy / 15 < 5) line_w = dy / 15 + 1; break;
    case T_RECT: case T_POLY: case T_ELLIPSE: case T_RRECT: if (dy / 25 < 3) fill_style = dy / 25; break;
    case T_ERASER: eraser_size = imin(3, dy / 19); break;
    case T_ZOOM: if (dy / 15 < 5) set_zoom(zooms[dy / 15], scx + page_w() / 2, scy + page_h() / 2); break;
    case T_BRUSH: if (dy / 19 < 4) { brush_kind = PEN_ROUND + dy / 19; brush_size = imin(2, dx / 16); } break;
    case T_AIR: if (dy / 25 < 3) air_size = dy / 25; break;
    case T_SELECT: case T_TEXT:
        opaque = dy < 38;
        if (floating) float_render();
        if (text_on) text_render();
        break;
    default: return;
    }
    str_copy(t, "line ");
    fmt_u32(t + 5, (u32)line_w); str_cat(t, " fill ");
    fmt_u32(t + str_len(t), (u32)fill_style);
    app_log("[PAINT] option", t);
}
static void hscroll_click(int sx)
{
    int step = imax(1, 16 / zoom), pw = page_w();
    if (sx < vx + 16) scx -= step;
    else if (sx >= vx + vw - 16) scx += step;
    else if (cv.w > pw) scx = (int)((long)(sx - vx - 16) * (cv.w - pw) / imax(1, vw - 32));
    clamp_scroll();
}
static void vscroll_click(int sy)
{
    long p = scroll_hit(vx + vw, vy, vh, sy, cv.h, page_h());
    int step = imax(1, 16 / zoom);
    if (p == -1) scy -= step;
    else if (p == -2) scy += step;
    else if (p >= 16) scy = (int)(p - 16);
    clamp_scroll();
}
static int sb_drag;
static int on_mouse(int kind, int sx, int sy)
{
    int r, i, x, y, btn;
    layout();
    ui_changed = 0;
    if (dlg.open) {
        dialog_place(&dlg);
        if (dialog_mine(&dlg) && (dlg_kind == D_OPEN || dlg_kind == D_SAVEAS) && fd_mouse(kind, sx, sy)) return 1;
        r = dialog_mouse(&dlg, kind, sx, sy);
        if (r >= 0) dialog_result(r);
        return 1;
    }
    if (text_on && text_edit.drag && (kind == MOUSE_MOVE || kind == MOUSE_UP)) {
        to_img(sx, sy, &x, &y);
        field_select(&text_edit, kind, text_at(x, y), HOST.shift);
        ui_cursor(CURSOR_IBEAM);
        return 1;
    }
    if (drag == 1) {
        to_img(sx, sy, &x, &y);
        if (kind == MOUSE_MOVE) tool_move(x, y);
        else if (kind == MOUSE_UP) tool_up(x, y);
        flush();
        return 0;
    }
    if (drag) return 0;                        /* the right button draws */
    if (sb_drag) {
        if (kind == MOUSE_MOVE) { if (sb_drag == 1) vscroll_click(sy); else hscroll_click(sx); return 1; }
        if (kind == MOUSE_UP) sb_drag = 0;
        return 0;
    }
    if (ctx.open) {
        r = popup_mouse(&ctx, kind, sx, sy);
        if (r >= 0) command(r);
        return 1;
    }
    r = menubar_mouse(&bar, kind, sx, sy);
    if (r >= 0) { command(r); return 1; }
    if (r == -1) return 1;
    if (kind != MOUSE_DOWN && kind != MOUSE_RIGHT) return 0;
    btn = kind == MOUSE_DOWN ? 1 : 2;
    i = tool_at(sx, sy);
    if (i >= 0) { select_tool(i); return 1; }
    if (show_tools && sx >= optx && sx < optx + OPT_W && sy >= opty && sy < opty + OPT_H) { option_click(sx, sy); return 1; }
    i = swatch_at(sx, sy);
    if (i >= 0) {
        char t[8];
        if (btn == 1) fg = swatch[i]; else bg = swatch[i];
        fmt_u32(t, (u32)swatch[i]);
        app_log(btn == 1 ? "[PAINT] colour fg" : "[PAINT] colour bg", t);
        if (floating && !opaque) float_render();
        if (text_on) text_render();
        flush();
        return 1;
    }
    if (btn == 1 && sx >= vx + vw && sx < vx + vw + 16 && sy >= vy && sy < vy + vh) {
        vscroll_click(sy);
        if (sy >= vy + 16 && sy < vy + vh - 16) sb_drag = 1;
        return 1;
    }
    if (btn == 1 && sy >= vy + vh && sy < vy + vh + 16 && sx >= vx && sx < vx + vw) {
        hscroll_click(sx);
        if (sx >= vx + 16 && sx < vx + vw - 16) sb_drag = 2;
        return 1;
    }
    if (in_view(sx, sy)) {
        to_img(sx, sy, &x, &y);
        status[0] = 0;
        if (text_on && btn == 1 && text_contains(x, y)) {
            field_select(&text_edit, kind, text_at(x, y), HOST.shift);
            ui_cursor(CURSOR_IBEAM);
            return 1;
        }
        if (btn == 2 && (tool == T_SELECT || inside_sel(x, y)) && !text_on) { context_menu(sx, sy); return 1; }
        tool_down(x, y, btn);
        flush();
        return ui_changed;
    }
    return kind == MOUSE_DOWN;
}
static int on_hover(int b, int c)
{
    int sx = HOST.x + b, sy = HOST.y + TITLE_H + c, t, x, y;
    layout();
    ui_dirty = 0;
    if (dialog_mine(&dlg)) { dialog_hover(&dlg, sx, sy); return ui_dirty; }
    if (dlg.open) return 0;
    if (ctx.open) {
        popup_mouse(&ctx, MOUSE_HOVER, sx, sy);
        if (ui_dirty) ui_damage(ctx.x, ctx.y, ctx.w + 4, ctx.h + 4);
        return 0;
    }
    if (menubar_open(&bar)) {
        int act = bar.active;
        menubar_mouse(&bar, MOUSE_HOVER, sx, sy);
        if (bar.active != act) return 1;
        if (ui_dirty && bar.pop.open) ui_damage(bar.pop.x, bar.pop.y, bar.pop.w + 4, bar.pop.h + 4);
        return 0;
    }
    ui_cursor(CURSOR_ARROW);
    t = tool_at(sx, sy);
    if (t != tool_hot) {
        int bx, by;
        if (tool_hot >= 0) { tool_xy(tool_hot, &bx, &by); ui_damage(bx, by, 25, 25); }
        if (t >= 0) { tool_xy(t, &bx, &by); ui_damage(bx, by, 25, 25); }
        tool_hot = t;
    }
    if (in_view(sx, sy) && HOST.window == WIN_PAINT) {
        to_img(sx, sy, &x, &y);
        if (text_on && text_contains(x, y)) ui_cursor(CURSOR_IBEAM);
        if (x >= 0 && y >= 0 && x < cv.w && y < cv.h) {
            if (x != ptr_x || y != ptr_y) { ptr_x = x; ptr_y = y; damage_status(); }
            return 0;
        }
    }
    if (ptr_x >= 0) { ptr_x = -1; damage_status(); }
    return 0;
}
static int on_poll(void)
{
    int x, y;
    if (!drag) return 0;
    layout();
    if (drag == 2) {
        if (HOST.mx != last_mx || HOST.my != last_my) {
            last_mx = HOST.mx; last_my = HOST.my;
            to_img(HOST.mx, HOST.my, &x, &y);
            tool_move(x, y);
        }
        if (!(HOST.buttons & 2)) {
            to_img(HOST.mx, HOST.my, &x, &y);
            tool_up(x, y);
            flush();
            return 1;
        }
    }
    if (tool == T_AIR && drag_mode == DM_FREE && HOST.ticks != air_tick) {
        air_tick = HOST.ticks;
        spray(lx, ly, c1);
    }
    flush();
    return drag == 2 || tool == T_AIR ? 2 : 0;
}
static void nudge(int dx, int dy)
{
    if (!floating && !lift(0)) return;
    sel_damage();
    selx += dx; sely += dy;
    sel_damage();
    float_render();
}
static int on_key(int key)
{
    int s = KEY_SCAN(key), ch = KEY_CHAR(key), r, letter;
    layout();
    ui_changed = 1;
    if (dlg.open) {
        int k = dlg_kind;
        if ((k == D_OPEN || k == D_SAVEAS) && !ch && (s == K_UP || s == K_DOWN)) {
            if (s == K_UP && fd_sel > 0) fd_sel--;
            if (s == K_DOWN && fd_sel + 1 < fd_count) fd_sel++;
            if (fd_sel < 0 && fd_count) fd_sel = 0;
            if (fd_sel < fd_top) fd_top = fd_sel;
            if (fd_sel >= fd_top + FD_ROWS) fd_top = fd_sel - FD_ROWS + 1;
            if (fd_sel >= 0) field_set(&f1, b1, PATH_MAX - 4, fd_list[fd_sel].name);
            return 1;
        }
        if ((k == D_OPEN || k == D_SAVEAS) && ch == 13 && dlg.focus == 4 && fd_sel >= 0 &&
            fd_list[fd_sel].dir && !str_icmp(f1.text, fd_list[fd_sel].name)) {
            fd_enter(fd_list[fd_sel].name);
            f1.text[0] = 0; f1.len = f1.cursor = 0;
            return 1;
        }
        if ((k == D_OPEN || k == D_SAVEAS) && ch == 8 && dlg.focus == 4 && !f1.len) { fd_enter(".."); return 1; }
        r = dialog_key(&dlg, key, HOST.shift);
        if (r >= 0) dialog_result(r);
        return 1;
    }
    if (ctx.open) {
        r = popup_key(&ctx, key);
        if (r >= 0) command(r);
        return 1;
    }
    r = menubar_key(&bar, key, HOST.shift);
    if (r >= 0) { command(r); return 1; }
    if (r == -1) return 1;
    if (drag) return 1;
    if (key == 0x5D00) { context_menu(vx + 40, vy + 40); return 1; }
    if (text_on) {
        if (key == K_ESC) { text_on = 0; cancel_op(); flush(); return 1; }
        if (text_key(key)) { text_render(); flush(); return 1; }
    }
    letter = (HOST.shift & SH_CTRL) ? key_ctrl_letter(key) : 0;
    if (letter) {
        switch (letter) {
        case 'N': command(M_NEW); break;
        case 'O': command(M_OPEN); break;
        case 'S': command(M_SAVE); break;
        case 'Z': command(M_UNDO); break;
        case 'Y': command(M_REDO); break;
        case 'X': command(M_CUT); break;
        case 'C': command(M_COPY); break;
        case 'V': command(M_PASTE); break;
        case 'A': command(M_SELALL); break;
        case 'I': command(M_INVERT); break;
        case 'E': command(M_ATTR); break;
        case 'R': command(M_FLIP); break;
        case 'T': command(M_TOOLBOX); break;
        case 'L': command(M_COLORBOX); break;
        case 'G': command(M_GRID); break;
        }
        flush();
        return 1;
    }
    if (key == K_ESC) {
        if (npt) { npt = 0; cancel_op(); }
        else if (curve_stage) { curve_stage = 0; cancel_op(); }
        else drop_selection();
        flush();
        return 1;
    }
    if (!ch || ch == 0xE0) {
        int step = imax(1, 32 / zoom);
        switch (s) {
        case K_F1: command(M_HELP); return 1;
        case K_DEL: command(M_CLEARSEL); flush(); return 1;
        case 0x76: command(M_ZOOMIN); return 1;            /* Ctrl+PgDn */
        case 0x84: command(M_ZOOMOUT); return 1;           /* Ctrl+PgUp */
        case K_LEFT: if (sel_on) nudge(-1, 0); else scx -= step; break;
        case K_RIGHT: if (sel_on) nudge(1, 0); else scx += step; break;
        case K_UP: if (sel_on) nudge(0, -1); else scy -= step; break;
        case K_DOWN: if (sel_on) nudge(0, 1); else scy += step; break;
        case K_PGUP: scy -= page_h(); break;
        case K_PGDN: scy += page_h(); break;
        case K_HOME: scx = scy = 0; break;
        case K_END: scx = cv.w; scy = cv.h; break;
        default: return 0;
        }
        clamp_scroll();
        flush();
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
static void release_memory(void)
{
    img_free(&cv); img_free(&old); pool_free(); img_free(&flt); img_free(&clip);
    if (fnt) dos_free(fnt);
    fnt = 0; fnt_loaded = 0;
    nslots = 0;
    if (tabs) dos_free(tabs);
    tabs = 0;
    undo_kind = U_NONE; op_open = 0; floating = 0; sel_on = 0; text_on = 0; npt = 0; curve_stage = 0;
}
static int on_open(int a)
{
    char arg[PATH_MAX], t[8];
    int i;
    load_palette();
    choose_swatches();
    msg_buf[0] = 0;
    str_copy(msg_buf, "[PAINT] swatches");
    for (i = 0; i < NSWATCH; i++) { fmt_u32(t, (u32)swatch[i]); str_cat(msg_buf, " "); str_cat(msg_buf, t); }
    app_log(msg_buf, 0);
    if (!font_file[0] && sys_font[0]) str_copy(font_file, sys_font);
    if (!tabs) {
        tabs = dos_alloc(paras(ROWBUF_OFF + ROWBUF_BYTES));
        if (!tabs) { app_sound(5); app_close(); return 0; }
        ffill(tabs, 0, MAX_H * 4, 0);
    }
    if (!cv.n) {
        HDR_WIDTH = imin(HOST.screen_w - 80, 840);
        HDR_HEIGHT = imin(HOST.screen_h - 90, 660);
        if (!new_image(DEF_W, DEF_H)) { app_sound(5); app_close(); return 0; }
    }
    if (APP_ARG[0]) {
        str_ncopy(arg, APP_ARG, PATH_MAX);
        APP_ARG[0] = 0;
        if (a == 1 || !dirty) {
            int r = load_bmp(arg);
            if (r == -2 && a != 1) { str_ncopy(path, arg, PATH_MAX); for (i = 0; path[i]; i++) path[i] = to_upper(path[i]); }
            else if (a != 1) open_error(r);
        } else {
            str_ncopy(pending_path, arg, PATH_MAX);
            save_prompt(A_OPEN_PATH);
        }
    }
    update_title();
    log_memory();
    app_log("[PAINT] ready", 0);
    return 1;
}
static int paint_event(int ev, int a, int b, int c)
{
    switch (ev) {
    case EV_OPEN: return on_open(a);
    case EV_PAINT: paint(); return 0;
    case EV_KEY: return on_key(a);
    case EV_MOUSE: return on_mouse(a, HOST.x + b, HOST.y + TITLE_H + c);
    case EV_POLL: return on_poll();
    case EV_CLOSE:
        if (!closing) {
            finish_pending();
            drop_selection();
            if (dirty) { save_prompt(A_EXIT); return 1; }
        }
        release_memory();
        closing = 0;
        app_log("[PAINT] closed", 0);
        return 0;
    case EV_SUSPEND:
        if (dirty || op_open) return 1;         /* unsaved: the picture stays in memory */
        str_ncopy(APP_ARG, path, APP_ARG_BYTES);
        release_memory();
        return 0;
    }
    return 0;
}
int app_event(int ev, int a, int b, int c)
{
    int r, orig = ev;
    if (ev == EV_OPEN && a == 2) { dlg.win = 0; dialog_sync(&dlg); return 1; }
    r = dialog_pre(&dlg, WIN_PAINT, &ev, &a);
    if (r >= 0) return r;
    if (dialog_mine(&dlg) && ev == EV_PAINT) { paint_dialog(); return 0; }
    if (ev == EV_MOUSE && a == MOUSE_HOVER) return on_hover(b, c);
    r = paint_event(ev, a, b, c);
    /* The shell repaints the whole window for an answer of 1 only when
     * nothing else was damaged meanwhile (flush): say it explicitly. */
    if (r == 1 && (ev == EV_KEY || ev == EV_MOUSE)) ui_damage(HOST.x, HOST.y, HOST.w, HOST.h);
    r = dialog_post(&dlg, WIN_PAINT, ev, r);
    return orig == EV_CLOSE && ev != EV_CLOSE ? 0 : r;
}
