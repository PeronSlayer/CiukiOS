/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include <ciuki/font.h>
#include <assert.h>
#include <stdio.h>
#include <time.h>
static double seconds(void)
{ struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC, &t)); return t.tv_sec + t.tv_nsec / 1e9; }
static int glyph(void *ctx, int x, int y, const struct ciuki_font_bitmap *b)
{ (void)ctx; (void)x; (void)y; (void)b; return 0; }
int main(int argc, char **argv)
{
    assert(argc == 2); struct ciuki_font_face *face;
    assert(!ciuki_font_open(argv[1], &face));
    char text[95]; for (unsigned i = 0; i < sizeof(text); i++) text[i] = (char)(32 + i);
    int32_t advance; double cold, hot, start = seconds();
    for (unsigned i = 0; i < 100; i++) {
        ciuki_font_cache_clear(face);
        assert(!ciuki_font_paint(face, 853, text, sizeof(text), 0, 20, glyph, NULL, &advance));
    }
    cold = (seconds() - start) * 1e6 / (100 * sizeof(text)); start = seconds();
    for (unsigned i = 0; i < 1000; i++) assert(!ciuki_font_paint(face, 853, text, sizeof(text), 0, 20, glyph, NULL, &advance));
    hot = (seconds() - start) * 1e6 / (1000 * sizeof(text));
    printf("{\"pt\":10,\"dpi\":96,\"em_26_6\":853,\"cold_us_per_glyph\":%.4f,\"warm_us_per_glyph\":%.4f,\"cold_glyphs\":9500,\"warm_glyphs\":95000}\n", cold, hot);
    ciuki_font_close(face); return 0;
}
