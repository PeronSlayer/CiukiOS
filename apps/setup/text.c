/* SPDX-License-Identifier: MIT
 * Setup text: wrapped rich text in the desktop's shared faces
 * (apps/desktop/theme.h). ST_B toggles bold; '\n' breaks the line. The
 * faces cover ASCII, Latin-1, U+2022 and U+2026 only (fonts contract), so
 * the approved copy's dashes (U+2013, U+2014) and the guillemet U+203A are
 * drawn as rules and chevrons with the face's metrics instead of the
 * missing-glyph '?'. The copy itself is kept verbatim. Status marks
 * (check, warning, stop, pending, active) are drawn, not typed. */
#include "setup.h"
#include "../desktop/theme.h"
#include <string.h>

static int bold_of(int font) { return font == DESK_FONT_UI ? DESK_FONT_BOLD : font == DESK_FONT_LABEL ? DESK_FONT_TITLE : font; }

static int seq_length(unsigned char c) { return c < 0xc0 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4; }

/* Draws (c set) or measures n bytes from the pen at (x, baseline). */
static int run(struct du_canvas *c, int font, int *bold, int x, int baseline, const char *s, size_t n, uint32_t ink)
{
    int x0 = x, f = *bold ? bold_of(font) : font;
    char buf[160]; size_t k = 0;
#define FLUSH() do { if (k) { buf[k] = 0; if (c) du_text_font(c, f, x, baseline - desk_font_cap(f), buf, ink); \
                               x += desk_text_measure(f, buf); k = 0; } } while (0)
    for (size_t i = 0; i < n;) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == 1) { FLUSH(); *bold = !*bold; f = *bold ? bold_of(font) : font; i++; continue; }
        size_t len = (size_t)seq_length(ch);
        if (i + len > n) len = n - i;
        unsigned cp = 0;
        if (len == 3 && ch == 0xe2) cp = 0x2000u | ((unsigned)((unsigned char)s[i + 1] & 0x3f) << 6) | ((unsigned char)s[i + 2] & 0x3f);
        if (cp == 0x2013 || cp == 0x2014 || cp == 0x203a) {
            FLUSH();
            int size = desk_font(f)->size, cap = desk_font_cap(f);
            int mid = baseline - (cap * 4 + 5) / 10, thick = f == bold_of(font) && *bold ? 2 : 1;
            int advance = cp == 0x2014 ? size : cp == 0x2013 ? size / 2 + 1 : size / 2;
            if (c && cp != 0x203a) du_fill(c, (struct du_rect){x + 1, mid, advance - 2, thick}, ink);
            if (c && cp == 0x203a)
                for (int r = -2; r <= 2; r++) du_fill(c, (struct du_rect){x + 1 + (2 - (r < 0 ? -r : r)), mid + r, thick, 1}, ink);
            x += advance; i += 3;
            continue;
        }
        if (k + len + 1 >= sizeof(buf)) FLUSH();
        memcpy(buf + k, s + i, len); k += len; i += len;
    }
    FLUSH();
#undef FLUSH
    return x - x0;
}

static int baseline_of(int font, int y, int line_h)
{
    const struct desk_font *f = desk_font(font);
    return y + (line_h - desk_font_line(font)) / 2 + f->ascent;
}

int st_text_width(int font, const char *s)
{
    int bold = 0;
    const char *e = strchr(s, '\n');
    return run(NULL, font, &bold, 0, 0, s, e ? (size_t)(e - s) : strlen(s), 0);
}

int st_text_line(struct du_canvas *c, int font, int x, int y, int line_h, const char *s, uint32_t ink)
{
    int bold = 0;
    const char *e = strchr(s, '\n');
    return run(c, font, &bold, x, baseline_of(font, y, line_h), s, e ? (size_t)(e - s) : strlen(s), ink);
}

int st_text_wrap(struct du_canvas *c, int font, struct du_rect box, int line_h, const char *s, uint32_t ink)
{
    struct du_rect old = c ? c->clip : box;
    if (c) c->clip = du_intersection(old, (struct du_rect){box.x, box.y, box.w + 2, 0x7fff});
    int lines = 0, bold = 0;
    const char *p = s;
    while (*p) {
        const char *q = p, *end = NULL;
        int w = 0, probe = bold;
        for (;;) {
            const char *spaces = q;
            while (*q == ' ') q++;
            const char *we = q;
            while (*we && *we != ' ' && *we != '\n') we++;
            if (we == q) break;
            int t = probe;
            int sw = end ? run(NULL, font, &t, 0, 0, spaces, (size_t)(q - spaces), 0) : 0;
            int ww = run(NULL, font, &t, 0, 0, q, (size_t)(we - q), 0);
            if (end && w + sw + ww > box.w) break;
            if (!end && ww > box.w) {
                /* A fingerprint or source hash must remain fully readable:
                 * wrap long words at UTF-8 boundaries, never ellipsize. */
                const char *cut = q;
                int cw = 0, state = probe;
                while (cut < we) {
                    size_t n = (size_t)seq_length((unsigned char)*cut);
                    if (n > (size_t)(we - cut)) n = (size_t)(we - cut);
                    int next = state;
                    int adv = run(NULL, font, &next, 0, 0, cut, n, 0);
                    if (cut > q && cw + adv > box.w) break;
                    cw += adv; state = next; cut += n;
                }
                end = cut; break;
            }
            w += sw + ww; probe = t; end = we; q = we;
        }
        if (end) run(c, font, &bold, box.x, baseline_of(font, box.y + lines * line_h, line_h), p, (size_t)(end - p), ink);
        lines++;
        p = end ? end : q;
        while (*p == ' ') p++;
        if (*p == '\n') p++;
    }
    if (c) c->clip = old;
    return lines * line_h;
}

void st_mark_paint(struct du_canvas *c, int x, int y, int size, enum st_mark m)
{
    int ox = x + (size - 12) / 2, oy = y + (size - 12) / 2;
    switch (m) {
    case ST_MARK_OK: {
        static const int8_t tick[][2] = {{1,5},{2,6},{3,7},{4,8},{5,7},{6,6},{7,5},{8,4},{9,3},{10,2}};
        for (unsigned k = 0; k < sizeof(tick) / sizeof(tick[0]); k++)
            du_fill(c, (struct du_rect){ox + tick[k][0], oy + tick[k][1], 1, 3}, 0x008000u);
        break;
    }
    case ST_MARK_WARN:
        for (int r = 0; r < 10; r++) du_fill(c, (struct du_rect){ox + 6 - r / 2, oy + 1 + r, 2 * (r / 2) + 1, 1}, 0xb06000u);
        break;
    case ST_MARK_STOP:
        for (int k = 0; k < 9; k++) {
            du_fill(c, (struct du_rect){ox + 2 + k, oy + 2 + k, 2, 1}, 0xc00000u);
            du_fill(c, (struct du_rect){ox + 9 - k, oy + 2 + k, 2, 1}, 0xc00000u);
        }
        break;
    case ST_MARK_ACTIVE:
        for (int r = 0; r < 9; r++) du_fill(c, (struct du_rect){ox + 3, oy + 1 + r, (r < 5 ? r : 8 - r) + 1, 1}, 0x000080u);
        break;
    case ST_MARK_INFO: case ST_MARK_PENDING:
        du_fill(c, (struct du_rect){ox + 5, oy + 5, 2, 2}, 0x808080u);
        break;
    }
}
