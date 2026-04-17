/*
 * CiukiOS 2D software rasterizer — M-V2.1.
 *
 * Draws into the video driver backbuffer. All primitives clip to
 * framebuffer bounds and to the current clip rect (if active), then
 * commit a single dirty-rect update per call.
 */

#include "gfx2d.h"
#include "video.h"
#include "types.h"

/* ---------- Clip state ---------- */

static u32 g_clip_active;
static i32 g_clip_x0, g_clip_y0, g_clip_x1, g_clip_y1; /* exclusive upper */

static inline i32 i32_min(i32 a, i32 b) { return a < b ? a : b; }
static inline i32 i32_max(i32 a, i32 b) { return a > b ? a : b; }
static inline i32 iabs(i32 v) { return v < 0 ? -v : v; }

void gfx2d_set_clip(u32 x, u32 y, u32 w, u32 h) {
    i32 fbw = (i32)video_width_px();
    i32 fbh = (i32)video_height_px();
    g_clip_x0 = (i32)x;
    g_clip_y0 = (i32)y;
    g_clip_x1 = (i32)(x + w);
    g_clip_y1 = (i32)(y + h);
    if (g_clip_x0 < 0) g_clip_x0 = 0;
    if (g_clip_y0 < 0) g_clip_y0 = 0;
    if (g_clip_x1 > fbw) g_clip_x1 = fbw;
    if (g_clip_y1 > fbh) g_clip_y1 = fbh;
    g_clip_active = 1U;
}

void gfx2d_clear_clip(void) {
    g_clip_active = 0U;
}

/* Compute the effective clip (clip rect AND fb bounds). */
static void gfx2d_effective_clip(i32 *x0, i32 *y0, i32 *x1, i32 *y1) {
    *x0 = 0;
    *y0 = 0;
    *x1 = (i32)video_width_px();
    *y1 = (i32)video_height_px();
    if (g_clip_active) {
        if (g_clip_x0 > *x0) *x0 = g_clip_x0;
        if (g_clip_y0 > *y0) *y0 = g_clip_y0;
        if (g_clip_x1 < *x1) *x1 = g_clip_x1;
        if (g_clip_y1 < *y1) *y1 = g_clip_y1;
    }
}

/* ---------- Primitives ---------- */

void gfx2d_pixel(u32 x, u32 y, u32 rgb) {
    i32 cx0, cy0, cx1, cy1;
    gfx2d_effective_clip(&cx0, &cy0, &cx1, &cy1);
    if ((i32)x < cx0 || (i32)y < cy0 || (i32)x >= cx1 || (i32)y >= cy1) return;
    video_put_pixel(x, y, rgb);
}

void gfx2d_hline(u32 x, u32 y, u32 w, u32 rgb) {
    i32 cx0, cy0, cx1, cy1;
    i32 ix = (i32)x, iy = (i32)y;
    i32 iw = (i32)w;
    gfx2d_effective_clip(&cx0, &cy0, &cx1, &cy1);
    if (iy < cy0 || iy >= cy1) return;
    if (ix < cx0) { iw -= (cx0 - ix); ix = cx0; }
    if (ix + iw > cx1) iw = cx1 - ix;
    if (iw <= 0) return;
    video_fill_rect((u32)ix, (u32)iy, (u32)iw, 1U, rgb);
}

void gfx2d_vline(u32 x, u32 y, u32 h, u32 rgb) {
    i32 cx0, cy0, cx1, cy1;
    i32 ix = (i32)x, iy = (i32)y;
    i32 ih = (i32)h;
    gfx2d_effective_clip(&cx0, &cy0, &cx1, &cy1);
    if (ix < cx0 || ix >= cx1) return;
    if (iy < cy0) { ih -= (cy0 - iy); iy = cy0; }
    if (iy + ih > cy1) ih = cy1 - iy;
    if (ih <= 0) return;
    video_fill_rect((u32)ix, (u32)iy, 1U, (u32)ih, rgb);
}

void gfx2d_line(i32 x0, i32 y0, i32 x1, i32 y1, u32 rgb) {
    /* Bresenham with per-pixel clip test. Fine for thin lines. */
    i32 cx0, cy0, cx1, cy1;
    i32 dx, dy, sx, sy, err, e2;
    gfx2d_effective_clip(&cx0, &cy0, &cx1, &cy1);

    dx = iabs(x1 - x0);
    dy = -iabs(y1 - y0);
    sx = x0 < x1 ? 1 : -1;
    sy = y0 < y1 ? 1 : -1;
    err = dx + dy;

    for (;;) {
        if (x0 >= cx0 && x0 < cx1 && y0 >= cy0 && y0 < cy1) {
            video_put_pixel((u32)x0, (u32)y0, rgb);
        }
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx2d_rect(u32 x, u32 y, u32 w, u32 h, u32 rgb) {
    if (w == 0U || h == 0U) return;
    gfx2d_hline(x, y, w, rgb);
    if (h > 1U) gfx2d_hline(x, y + h - 1U, w, rgb);
    if (h > 2U) {
        gfx2d_vline(x, y + 1U, h - 2U, rgb);
        if (w > 1U) gfx2d_vline(x + w - 1U, y + 1U, h - 2U, rgb);
    }
}

void gfx2d_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 rgb) {
    i32 cx0, cy0, cx1, cy1;
    i32 ix = (i32)x, iy = (i32)y, iw = (i32)w, ih = (i32)h;
    gfx2d_effective_clip(&cx0, &cy0, &cx1, &cy1);
    if (ix < cx0) { iw -= (cx0 - ix); ix = cx0; }
    if (iy < cy0) { ih -= (cy0 - iy); iy = cy0; }
    if (ix + iw > cx1) iw = cx1 - ix;
    if (iy + ih > cy1) ih = cy1 - iy;
    if (iw <= 0 || ih <= 0) return;
    video_fill_rect((u32)ix, (u32)iy, (u32)iw, (u32)ih, rgb);
}

void gfx2d_circle(i32 cx, i32 cy, u32 r, u32 rgb) {
    /* Midpoint circle algorithm (outline). */
    i32 x = (i32)r;
    i32 y = 0;
    i32 err = 1 - x;

    if (r == 0U) { gfx2d_pixel((u32)cx, (u32)cy, rgb); return; }

    while (x >= y) {
        gfx2d_pixel((u32)(cx + x), (u32)(cy + y), rgb);
        gfx2d_pixel((u32)(cx + y), (u32)(cy + x), rgb);
        gfx2d_pixel((u32)(cx - x), (u32)(cy + y), rgb);
        gfx2d_pixel((u32)(cx - y), (u32)(cy + x), rgb);
        gfx2d_pixel((u32)(cx - x), (u32)(cy - y), rgb);
        gfx2d_pixel((u32)(cx - y), (u32)(cy - x), rgb);
        gfx2d_pixel((u32)(cx + x), (u32)(cy - y), rgb);
        gfx2d_pixel((u32)(cx + y), (u32)(cy - x), rgb);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x + 1);
        }
    }
}

void gfx2d_fill_circle(i32 cx, i32 cy, u32 r, u32 rgb) {
    /* Scan-line fill using the midpoint algorithm. */
    i32 x = (i32)r;
    i32 y = 0;
    i32 err = 1 - x;

    if (r == 0U) { gfx2d_pixel((u32)cx, (u32)cy, rgb); return; }

    while (x >= y) {
        /* Two slabs of horizontal spans: width 2x+1 and width 2y+1. */
        gfx2d_hline((u32)(cx - x), (u32)(cy + y), (u32)(2 * x + 1), rgb);
        gfx2d_hline((u32)(cx - x), (u32)(cy - y), (u32)(2 * x + 1), rgb);
        gfx2d_hline((u32)(cx - y), (u32)(cy + x), (u32)(2 * y + 1), rgb);
        gfx2d_hline((u32)(cx - y), (u32)(cy - x), (u32)(2 * y + 1), rgb);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x + 1);
        }
    }
}

void gfx2d_tri(i32 x0, i32 y0, i32 x1, i32 y1, i32 x2, i32 y2, u32 rgb) {
    gfx2d_line(x0, y0, x1, y1, rgb);
    gfx2d_line(x1, y1, x2, y2, rgb);
    gfx2d_line(x2, y2, x0, y0, rgb);
}

/* Filled triangle: top-flat / bottom-flat split. */
static void tri_flat_bottom(i32 x0, i32 y0, i32 x1, i32 y1, i32 x2, i32 y2, u32 rgb) {
    /* y0 < y1 == y2; x1 left, x2 right (or swapped). */
    (void)y2;
    i32 i32dy = y1 - y0;
    if (i32dy <= 0) {
        gfx2d_hline((u32)i32_min(i32_min(x0, x1), x2),
                    (u32)y0,
                    (u32)(i32_max(i32_max(x0, x1), x2)
                          - i32_min(i32_min(x0, x1), x2) + 1),
                    rgb);
        return;
    }
    /* 1/slope left / right in 16.16 fixed point. */
    i32 invsl_l = ((x1 - x0) << 16) / i32dy;
    i32 invsl_r = ((x2 - x0) << 16) / i32dy;
    i32 cur_l = x0 << 16;
    i32 cur_r = x0 << 16;
    for (i32 sy = y0; sy <= y1; sy++) {
        i32 lx = cur_l >> 16;
        i32 rx = cur_r >> 16;
        if (lx > rx) { i32 t = lx; lx = rx; rx = t; }
        if (rx >= lx) {
            gfx2d_hline((u32)lx, (u32)sy, (u32)(rx - lx + 1), rgb);
        }
        cur_l += invsl_l;
        cur_r += invsl_r;
    }
}

static void tri_flat_top(i32 x0, i32 y0, i32 x1, i32 y1, i32 x2, i32 y2, u32 rgb) {
    /* y0 == y1 < y2. */
    (void)y1;
    i32 i32dy = y2 - y0;
    if (i32dy <= 0) {
        gfx2d_hline((u32)i32_min(i32_min(x0, x1), x2),
                    (u32)y0,
                    (u32)(i32_max(i32_max(x0, x1), x2)
                          - i32_min(i32_min(x0, x1), x2) + 1),
                    rgb);
        return;
    }
    i32 invsl_l = ((x2 - x0) << 16) / i32dy;
    i32 invsl_r = ((x2 - x1) << 16) / i32dy;
    i32 cur_l = x0 << 16;
    i32 cur_r = x1 << 16;
    for (i32 sy = y0; sy <= y2; sy++) {
        i32 lx = cur_l >> 16;
        i32 rx = cur_r >> 16;
        if (lx > rx) { i32 t = lx; lx = rx; rx = t; }
        if (rx >= lx) {
            gfx2d_hline((u32)lx, (u32)sy, (u32)(rx - lx + 1), rgb);
        }
        cur_l += invsl_l;
        cur_r += invsl_r;
    }
}

void gfx2d_fill_tri(i32 x0, i32 y0, i32 x1, i32 y1, i32 x2, i32 y2, u32 rgb) {
    /* Sort vertices by y ascending (insertion sort). */
    i32 tx, ty;
    if (y1 < y0) { tx = x0; ty = y0; x0 = x1; y0 = y1; x1 = tx; y1 = ty; }
    if (y2 < y0) { tx = x0; ty = y0; x0 = x2; y0 = y2; x2 = tx; y2 = ty; }
    if (y2 < y1) { tx = x1; ty = y1; x1 = x2; y1 = y2; x2 = tx; y2 = ty; }

    if (y1 == y2) {
        tri_flat_bottom(x0, y0, x1, y1, x2, y2, rgb);
    } else if (y0 == y1) {
        tri_flat_top(x0, y0, x1, y1, x2, y2, rgb);
    } else {
        /* General case: split at y = y1 into flat-bottom + flat-top. */
        i32 mx = x0 + (((i32)(y1 - y0) * (i32)(x2 - x0)) / (i32)(y2 - y0));
        tri_flat_bottom(x0, y0, x1, y1, mx, y1, rgb);
        tri_flat_top(x1, y1, mx, y1, x2, y2, rgb);
    }
}

void gfx2d_blit(const u32 *src, u32 sw, u32 sh, u32 stride, u32 dx, u32 dy) {
    i32 cx0, cy0, cx1, cy1;
    i32 idx = (i32)dx, idy = (i32)dy;
    i32 src_x = 0, src_y = 0;
    i32 iw = (i32)sw, ih = (i32)sh;
    gfx2d_effective_clip(&cx0, &cy0, &cx1, &cy1);
    if (idx < cx0) { src_x = cx0 - idx; iw -= src_x; idx = cx0; }
    if (idy < cy0) { src_y = cy0 - idy; ih -= src_y; idy = cy0; }
    if (idx + iw > cx1) iw = cx1 - idx;
    if (idy + ih > cy1) ih = cy1 - idy;
    if (iw <= 0 || ih <= 0) return;

    for (i32 y = 0; y < ih; y++) {
        const u32 *row = src + ((u64)(src_y + y) * stride) + src_x;
        video_blit_row((u32)idx, (u32)(idy + y), row, (u32)iw);
    }
}

void gfx2d_blit_masked(const u32 *src, u32 sw, u32 sh, u32 stride,
                       u32 dx, u32 dy, u32 key_rgb) {
    i32 cx0, cy0, cx1, cy1;
    i32 idx = (i32)dx, idy = (i32)dy;
    i32 src_x = 0, src_y = 0;
    i32 iw = (i32)sw, ih = (i32)sh;
    gfx2d_effective_clip(&cx0, &cy0, &cx1, &cy1);
    if (idx < cx0) { src_x = cx0 - idx; iw -= src_x; idx = cx0; }
    if (idy < cy0) { src_y = cy0 - idy; ih -= src_y; idy = cy0; }
    if (idx + iw > cx1) iw = cx1 - idx;
    if (idy + ih > cy1) ih = cy1 - idy;
    if (iw <= 0 || ih <= 0) return;

    for (i32 y = 0; y < ih; y++) {
        const u32 *row = src + ((u64)(src_y + y) * stride) + src_x;
        for (i32 x = 0; x < iw; x++) {
            u32 px = row[x] & 0x00FFFFFFU;
            if (px == (key_rgb & 0x00FFFFFFU)) continue;
            video_put_pixel((u32)(idx + x), (u32)(idy + y), px);
        }
    }
}

/* Alpha-over: dst = src*a + dst*(1-a). Approximates 0..255 as 0..256. */
void gfx2d_blit_alpha(const u32 *src_argb, u32 sw, u32 sh, u32 stride,
                      u32 dx, u32 dy) {
    i32 cx0, cy0, cx1, cy1;
    i32 idx = (i32)dx, idy = (i32)dy;
    i32 src_x = 0, src_y = 0;
    i32 iw = (i32)sw, ih = (i32)sh;
    gfx2d_effective_clip(&cx0, &cy0, &cx1, &cy1);
    if (idx < cx0) { src_x = cx0 - idx; iw -= src_x; idx = cx0; }
    if (idy < cy0) { src_y = cy0 - idy; ih -= src_y; idy = cy0; }
    if (idx + iw > cx1) iw = cx1 - idx;
    if (idy + ih > cy1) ih = cy1 - idy;
    if (iw <= 0 || ih <= 0) return;

    (void)iw; (void)ih; (void)src_x; (void)src_y;
    /* Alpha compositing requires reading back the framebuffer.
     * Stage2 does not currently expose a fb-read path (the backbuffer
     * store is considered opaque to external callers). For M-V2.1 we
     * fall back to masked blit (alpha=0 -> skip, else opaque write).
     * A follow-up in M-V2.5 will add video_read_pixel for true OVER.
     */
    for (i32 y = 0; y < ih; y++) {
        const u32 *row = src_argb + ((u64)(src_y + y) * stride) + src_x;
        for (i32 x = 0; x < iw; x++) {
            u32 px = row[x];
            u32 a = (px >> 24) & 0xFFU;
            if (a == 0U) continue;
            video_put_pixel((u32)(idx + x), (u32)(idy + y), px & 0x00FFFFFFU);
        }
    }
}

/* --- Visual regression test pattern --- */
void gfx2d_test_pattern(void) {
    u32 fbw = video_width_px();
    u32 fbh = video_height_px();
    if (fbw == 0U || fbh == 0U) return;

    /* Background gradient in 4 quadrants */
    gfx2d_fill_rect(0, 0, fbw / 2U, fbh / 2U, 0x00202030U);
    gfx2d_fill_rect(fbw / 2U, 0, fbw - fbw / 2U, fbh / 2U, 0x00302020U);
    gfx2d_fill_rect(0, fbh / 2U, fbw / 2U, fbh - fbh / 2U, 0x00203020U);
    gfx2d_fill_rect(fbw / 2U, fbh / 2U, fbw - fbw / 2U,
                    fbh - fbh / 2U, 0x00303030U);

    /* Diagonals */
    gfx2d_line(0, 0, (i32)fbw - 1, (i32)fbh - 1, 0x00FF8080U);
    gfx2d_line((i32)fbw - 1, 0, 0, (i32)fbh - 1, 0x0080FF80U);

    /* Outlined rect */
    gfx2d_rect(fbw / 4U, fbh / 4U, fbw / 2U, fbh / 2U, 0x0000FFFFU);

    /* Filled circle center */
    u32 r = (fbw < fbh ? fbw : fbh) / 10U;
    gfx2d_fill_circle((i32)(fbw / 2U), (i32)(fbh / 2U), r, 0x00FFFF00U);
    gfx2d_circle((i32)(fbw / 2U), (i32)(fbh / 2U), r + 8U, 0x00FFFFFFU);

    /* Filled triangle */
    gfx2d_fill_tri((i32)(fbw / 10U), (i32)(fbh * 8U / 10U),
                   (i32)(fbw / 2U),  (i32)(fbh / 10U),
                   (i32)(fbw * 9U / 10U), (i32)(fbh * 8U / 10U),
                   0x00FF00FFU);
}
