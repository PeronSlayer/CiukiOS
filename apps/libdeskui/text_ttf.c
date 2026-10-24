/* SPDX-License-Identifier: MIT
 * Scalable FreeType painting; UI text uses pinned pre-rasterised coverage. */
#include "deskui.h"
#include <ciuki/font.h>
#include <errno.h>
#include <string.h>
/* Standalone font-store probes may use this painter without a window theme. */
#pragma weak desk_theme
struct text_painter { struct du_canvas *canvas; uint32_t colour; };
static int glyph(void *ctx, int x, int y, const struct ciuki_font_bitmap *b)
{
    struct text_painter *p = ctx;
    struct du_canvas *c = p->canvas;
    /* Work in 64 bits before clipping: baseline/pen may be outside the canvas. */
    int64_t x0 = x, y0 = y, x1 = x0 + b->width, y1 = y0 + b->height;
    int64_t cx1 = (int64_t)c->clip.x + c->clip.w, cy1 = (int64_t)c->clip.y + c->clip.h;
    if (x0 < c->clip.x) x0 = c->clip.x;
    if (y0 < c->clip.y) y0 = c->clip.y;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    if (x1 > c->width) x1 = c->width;
    if (y1 > c->height) y1 = c->height;
    for (int64_t row = y0; row < y1; row++) {
        uint32_t *dst = c->pixels + (size_t)row * c->width;
        const uint8_t *src = b->coverage + (size_t)(row - y) * b->stride;
        for (int64_t col = x0; col < x1; col++) {
            unsigned a = src[col - x];
            if(&desk_theme && desk_theme && !desk_theme->text_aa) a=a>=104 ? 255 : 0;
            if (!a) continue;
            uint32_t old = dst[col], colour = 0;
            for (unsigned shift = 0; shift < 24; shift += 8)
                colour |= ((((p->colour >> shift) & 255u) * a + ((old >> shift) & 255u) * (255u - a) + 127u) / 255u) << shift;
            dst[col] = colour;
        }
    }
    return 0;
}
int du_text_face(struct du_canvas *c, struct ciuki_font_face *face, unsigned size, int x, int baseline, const char *s, uint32_t colour)
{
    if (!c || !c->pixels || c->width <= 0 || c->height <= 0 || !s) { errno = EINVAL; return -1; }
    struct text_painter p = {c, colour}; int32_t advance;
    return ciuki_font_paint(face, size, s, strlen(s), x, baseline, glyph, &p, &advance);
}
int du_text_ttf(struct du_canvas *c, const char *path, unsigned size, int x, int baseline, const char *s, uint32_t colour)
{
    struct ciuki_font_face *face;
    if (ciuki_font_open(path, &face)) return -1;
    int result = du_text_face(c, face, size, x, baseline, s, colour);
    ciuki_font_close(face); return result;
}
