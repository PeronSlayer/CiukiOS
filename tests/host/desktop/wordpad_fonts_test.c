/* WordPad on the real Liberation faces (libfont + FreeType): measuring, vertical metrics, drawing and layout agree.
 * SPDX-License-Identifier: MIT */
#include "fonts.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned ink(const uint32_t *p, int w, int h) { unsigned n = 0; for (int i = 0; i < w * h; i++) n += (p[i] & 0xffffff) != 0xffffff; return n; }
int main(int argc, char **argv)
{
    assert(argc == 2);
    wp_fonts_directory(argv[1]);
    const struct wp_draw_ops *f = wp_fonts();
    const struct wp_text_ops *t = &f->text;
    /* Proportional sans: i is narrower than m; bold wider than regular; mono is fixed; serif differs from sans */
    int wi = t->width(t->ctx, WP_SANS, 0, 20, "iiii", 4), wm = t->width(t->ctx, WP_SANS, 0, 20, "mmmm", 4);
    assert(wi > 0 && wm > wi * 2);
    assert(t->width(t->ctx, WP_SANS, WP_BOLD, 20, "Hello", 5) > t->width(t->ctx, WP_SANS, 0, 20, "Hello", 5));
    int m1 = t->width(t->ctx, WP_MONO, 0, 20, "iiiii", 5), m2 = t->width(t->ctx, WP_MONO, 0, 20, "mmmmm", 5);
    assert(m1 == m2 && m1 > 0);
    /* Courier New metrics: 0.6 em per character, 10 pt = 13.33 px: 8 px per character */
    assert(m1 / 5 >= 8 * 64 - 64 && m1 / 5 <= 8 * 64 + 64);
    assert(t->width(t->ctx, WP_SERIF, 0, 20, "Hello", 5) != t->width(t->ctx, WP_SANS, 0, 20, "Hello", 5));
    /* size scales the advance */
    int w20 = t->width(t->ctx, WP_SANS, 0, 20, "Hello", 5), w40 = t->width(t->ctx, WP_SANS, 0, 40, "Hello", 5);
    assert(w40 > w20 * 17 / 10 && w40 < w20 * 23 / 10);   /* hinted advances round per size */
    /* UTF-8 and the empty string */
    assert(t->width(t->ctx, WP_SANS, 0, 20, "\xc3\xa8", 2) > 0 && t->width(t->ctx, WP_SANS, 0, 20, "", 0) == 0);
    int a, d, a2, d2;
    t->vmetrics(t->ctx, WP_SANS, 0, 20, &a, &d); t->vmetrics(t->ctx, WP_SANS, 0, 40, &a2, &d2);
    assert(a >= 10 && a <= 14 && d >= 2 && d <= 5 && a2 > a * 3 / 2 && d2 >= d);
    /* drawing paints grey coverage inside the box, nowhere else, with a 700-byte piece split into chunks */
    enum { W = 400, H = 60 };
    static uint32_t px[W * H];
    for (int i = 0; i < W * H; i++) px[i] = 0xffffff;
    struct du_canvas c = {px, W, H, {0, 0, W, H}};
    f->draw(NULL, &c, WP_SANS, 0, 853, 10, 30, "The quick brown fox", 19, 0x000000);
    unsigned n = ink(px, W, H); assert(n > 100);
    unsigned grey = 0; for (int i = 0; i < W * H; i++) { uint32_t v = px[i] & 0xff; grey += v != 0 && v != 0xff; } assert(grey > 50);
    for (int y = 0; y < H; y++) for (int x = 0; x < 8; x++) assert((px[y * W + x] & 0xffffff) == 0xffffff);
    static char longtext[700]; memset(longtext, 'a', sizeof(longtext));
    for (int i = 0; i < W * H; i++) px[i] = 0xffffff;
    f->draw(NULL, &c, WP_MONO, 0, 853, -4000, 30, longtext, sizeof(longtext), 0x000000);   /* clipped far left: must not crash */
    f->draw(NULL, &c, WP_SANS, WP_BOLD | WP_ITALIC, 853, 10, 30, "x\xc3\xa8y", 4, 0xff0000);
    /* layout and hit testing on the real metrics, over every width */
    struct wp_doc doc; assert(!wp_init(&doc));
    const char *text = "The quick brown fox jumps over the lazy dog. Pack my box with five dozen liquor jugs.\tTabbed \xc3\xa8 text and a very long word: Pneumonoultramicroscopicsilicovolcanoconiosis\vsoft break";
    assert(!wp_insert(&doc, 0, text, strlen(text), NULL));
    struct wp_cstyle big = {1, 30, -1, WP_BOLD | WP_ITALIC, 0};
    assert(!wp_style_chars(&doc, 20, 50, &big));
    struct wp_layout l; wp_layout_init(&l);
    for (int width = 60; width <= 500; width += 41) {
        assert(!wp_layout_build(&l, &doc, t, width, 0));
        for (size_t pos = 0; pos <= doc.len; pos = wp_next(&doc, pos)) {
            int x, y, h; wp_layout_caret(&l, &doc, t, pos, &x, &y, &h);
            size_t back = wp_layout_hit(&l, &doc, t, x, y + h / 2);
            if (back != pos) {
                int x2, y2, h2; wp_layout_caret(&l, &doc, t, back, &x2, &y2, &h2);
                if (x2 != x || y2 != y) { printf("width %d pos %zu -> %zu (%d,%d) vs (%d,%d)\n", width, pos, back, x, y, x2, y2); assert(0); }
            }
            if (pos == doc.len) break;
        }
        for (unsigned i = 0; i < l.n; i++) assert(l.lines[i].h >= 10 && l.lines[i].width <= width + 2 || l.lines[i].end - l.lines[i].start <= 8);
    }
    wp_layout_free(&l); wp_free(&doc);
    puts("PASS wordpad fonts");
    return 0;
}
