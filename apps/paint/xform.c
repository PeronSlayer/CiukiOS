/* SPDX-License-Identifier: MIT
 * Image > Stretch/Skew on an indexed picture (integers only): the Windows 98
 * Paint order, stretch first, then the horizontal skew, then the vertical
 * skew. Stretch is 1-500 percent; skew is -89 to 89 degrees. Nearest-pixel
 * sampling keeps every output pixel an index of the 144-colour palette. */
#include "xform.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* tan(d degrees) * 1024, d = 0..89. */
static const int tan1024[90] = {
    0, 18, 36, 54, 72, 90, 108, 126, 144, 162,
    181, 199, 218, 236, 255, 274, 294, 313, 333, 353,
    373, 393, 414, 435, 456, 477, 499, 522, 544, 568,
    591, 615, 640, 665, 691, 717, 744, 772, 800, 829,
    859, 890, 922, 955, 989, 1024, 1060, 1098, 1137, 1178,
    1220, 1265, 1311, 1359, 1409, 1462, 1518, 1577, 1639, 1704,
    1774, 1847, 1926, 2010, 2100, 2196, 2300, 2412, 2534, 2668,
    2813, 2974, 3152, 3349, 3571, 3822, 4107, 4435, 4818, 5268,
    5807, 6465, 7286, 8340, 9743, 11704, 14644, 19539, 29324, 58665
};
static int tan_of(int degrees) { return degrees >= 0 ? tan1024[degrees] : -tan1024[-degrees]; }
static int fits(long w, long h)
{ return w > 0 && h > 0 && w <= PAINT_MAX_AXIS && h <= PAINT_MAX_AXIS && (unsigned long)w * (unsigned long)h <= PAINT_MAX_PIXELS; }
static void fill(struct paint_image *im, uint8_t c) { memset(im->pixels, c, (size_t)im->w * (size_t)im->h); }

int paint_stretch_skew(const struct paint_image *src, struct paint_image *dst, int sx, int sy, int skew_h, int skew_v, uint8_t background)
{
    if (!src || !dst || !src->pixels || sx < PAINT_STRETCH_MIN || sx > PAINT_STRETCH_MAX || sy < PAINT_STRETCH_MIN || sy > PAINT_STRETCH_MAX ||
        skew_h < -PAINT_SKEW_MAX || skew_h > PAINT_SKEW_MAX || skew_v < -PAINT_SKEW_MAX || skew_v > PAINT_SKEW_MAX) return -EINVAL;
    long w = ((long)src->w * sx + 50) / 100, h = ((long)src->h * sy + 50) / 100;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    int th = tan_of(skew_h), tv = tan_of(skew_v);
    /* The horizontal skew leans the top (positive: to the right) by |tan| * height. */
    long extra_x = (long)(th < 0 ? -th : th) * (h - 1) / 1024;
    long ow = w + extra_x;
    /* The vertical skew lifts the right edge (positive: up) by |tan| * width. */
    long extra_y = (long)(tv < 0 ? -tv : tv) * (ow - 1) / 1024;
    long oh = h + extra_y;
    if (!fits(ow, oh)) return -EFBIG;
    struct paint_image mid = {(int)w, (int)h, malloc((size_t)w * (size_t)h)};
    if (!mid.pixels) return -ENOMEM;
    for (long y = 0; y < h; y++) {
        const uint8_t *row = src->pixels + (size_t)(y * src->h / h) * (size_t)src->w;
        for (long x = 0; x < w; x++) mid.pixels[(size_t)y * (size_t)w + (size_t)x] = row[x * src->w / w];
    }
    struct paint_image out = {(int)ow, (int)oh, malloc((size_t)ow * (size_t)oh)};
    if (!out.pixels) { free(mid.pixels); return -ENOMEM; }
    fill(&out, background);
    for (long x = 0; x < ow; x++) {
        /* Rows of the horizontally skewed picture, then the column's vertical offset. */
        long lift = tv >= 0 ? (long)tv * x / 1024 : extra_y - (long)(-tv) * x / 1024;   /* rows from the bottom of the output */
        long top = oh - h - lift;   /* where row 0 of the skewed picture lands in this column */
        for (long y = 0; y < h; y++) {
            long shift = th >= 0 ? (long)th * (h - 1 - y) / 1024 : extra_x - (long)(-th) * (h - 1 - y) / 1024;
            long sx_in = x - shift;
            long dy = top + y;
            if (sx_in < 0 || sx_in >= w || dy < 0 || dy >= oh) continue;
            out.pixels[(size_t)dy * (size_t)ow + (size_t)x] = mid.pixels[(size_t)y * (size_t)w + (size_t)sx_in];
        }
    }
    free(mid.pixels);
    *dst = out;
    return 0;
}
