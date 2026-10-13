/* SPDX-License-Identifier: MIT */
#include <ciuki/font.h>
#include <string.h>
static int paint(void *ctx, int x, int y, const struct ciuki_font_bitmap *b)
{ (void)ctx; (void)x; (void)y; return b->coverage ? 0 : -1; }
int main(void)
{
    struct ciuki_font_face *face; struct ciuki_font_metrics metrics;
    const char text[] = "Classic computing, one woof at a time."; int32_t width, painted;
    if (ciuki_font_open("/System/fonts/LiberationSans-Regular.ttf", &face)) return 1;
    int result = ciuki_font_get_metrics(face, &metrics) || !metrics.units_per_em ||
                 ciuki_font_measure(face, 853, text, strlen(text), &width) ||
                 ciuki_font_paint(face, 853, text, strlen(text), 0, 20, paint, 0, &painted) || width != painted;
    ciuki_font_close(face); return result;
}
