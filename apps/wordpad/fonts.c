/* SPDX-License-Identifier: MIT
 * WordPad text measuring and drawing on libfont: the installed Liberation
 * faces (/System/fonts), metric-compatible with Arial, Times New Roman and
 * Courier New. A face that cannot be opened falls back to the toolkit's UI
 * face at a size close to the requested one, so a text is always shown. */
#include "fonts.h"
#include <ciuki/font.h>
#include <stdio.h>
#include <string.h>

static const char *font_dir = "/System/fonts/";
void wp_fonts_directory(const char *dir) { font_dir = dir; }
static struct ciuki_font_face *faces[WP_FAMILIES][4];
static int tried[WP_FAMILIES][4];

static struct ciuki_font_face *face_for(unsigned family, unsigned flags)
{
    if (family >= WP_FAMILIES) family = WP_SANS;
    unsigned style = (flags & WP_BOLD ? 1u : 0u) | (flags & WP_ITALIC ? 2u : 0u);
    if (!tried[family][style]) {
        static const char *const names[] = {"Sans", "Serif", "Mono"};
        static const char *const styles[] = {"Regular", "Bold", "Italic", "BoldItalic"};
        char path[160];
        tried[family][style] = 1;
        snprintf(path, sizeof(path), "%sLiberation%s-%s.ttf", font_dir, names[family], styles[style]);
        if (ciuki_font_open(path, &faces[family][style])) faces[family][style] = NULL;
    }
    return faces[family][style];
}
static unsigned em_of(unsigned size_hp) { return size_hp * 128u / 3u; }

static int fonts_width(void *ctx, unsigned family, unsigned flags, unsigned size_hp, const char *s, size_t n)
{
    (void)ctx;
    struct ciuki_font_face *f = face_for(family, flags);
    int32_t advance;
    if (f && !ciuki_font_measure(f, em_of(size_hp), s, n, &advance)) return (int)advance;
    /* Fallback: the toolkit face is about 7 pixels per character at 10 pt. */
    int chars = 0;
    for (size_t i = 0; i < n; i++) chars += ((unsigned char)s[i] & 0xc0) != 0x80;
    return chars * (int)size_hp * 22;
}
static void fonts_vmetrics(void *ctx, unsigned family, unsigned flags, unsigned size_hp, int *ascent, int *descent)
{
    (void)ctx;
    struct ciuki_font_face *f = face_for(family, flags);
    struct ciuki_font_size_metrics m;
    if (f && !ciuki_font_get_size_metrics(f, em_of(size_hp), &m)) {
        *ascent = (m.ascent + 63) >> 6; *descent = (-m.descent + 63) >> 6;
        if (*descent < 0) *descent = 0;
        return;
    }
    int px = (int)size_hp * 2 / 3;
    *ascent = px * 3 / 4; *descent = px / 4 + 1;
}
/* du_text_face wants a NUL-terminated string: draw in short chunks. */
static void fonts_draw(void *ctx, struct du_canvas *c, unsigned family, unsigned flags, unsigned em, int x, int baseline,
                       const char *s, size_t n, uint32_t rgb)
{
    (void)ctx;
    struct ciuki_font_face *f = face_for(family, flags);
    char chunk[256];
    while (n) {
        size_t k = n < sizeof(chunk) - 1 ? n : sizeof(chunk) - 1;
        while (k < n && k > 1 && ((unsigned char)s[k] & 0xc0) == 0x80) k--;   /* never split a character */
        memcpy(chunk, s, k); chunk[k] = 0;
        int32_t advance = 0;
        if (f) {
            if (du_text_face(c, f, em, x, baseline, chunk, rgb) || ciuki_font_measure(f, em, chunk, k, &advance)) advance = 0;
        } else {
            du_text(c, x, baseline - DU_GLYPH_H, chunk, rgb);
            advance = du_text_width(chunk) * 64;
        }
        x += (advance + 32) >> 6;
        s += k; n -= k;
    }
}
const struct wp_draw_ops *wp_fonts(void)
{
    static const struct wp_draw_ops ops = {{NULL, fonts_width, fonts_vmetrics}, fonts_draw};
    return &ops;
}
