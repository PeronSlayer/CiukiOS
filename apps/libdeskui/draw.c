/* SPDX-License-Identifier: MIT
 * Clipped integer drawing primitives. Glyphs come from the compositor. */
#include "deskui.h"
#include <stdio.h>
#include <string.h>

extern uint8_t desk_glyph_row(unsigned, unsigned);

int du_contains(struct du_rect r, int x, int y)
{
    return r.w > 0 && r.h > 0 && x >= r.x && y >= r.y &&
           (int64_t)x < (int64_t)r.x + r.w && (int64_t)y < (int64_t)r.y + r.h;
}

struct du_rect du_intersection(struct du_rect a, struct du_rect b)
{
    int64_t x = a.x > b.x ? a.x : b.x, y = a.y > b.y ? a.y : b.y;
    int64_t right = (int64_t)a.x + a.w, bottom = (int64_t)a.y + a.h;
    if ((int64_t)b.x + b.w < right) right = (int64_t)b.x + b.w;
    if ((int64_t)b.y + b.h < bottom) bottom = (int64_t)b.y + b.h;
    if (a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0 || right <= x || bottom <= y)
        return (struct du_rect){0, 0, 0, 0};
    return (struct du_rect){(int)x, (int)y, (int)(right - x), (int)(bottom - y)};
}

static struct du_rect visible(const struct du_canvas *c, struct du_rect r)
{
    return du_intersection(du_intersection(r, c->clip), (struct du_rect){0, 0, c->width, c->height});
}

void du_fill(struct du_canvas *c, struct du_rect r, uint32_t colour)
{
    r = visible(c, r);
    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t *row = c->pixels + (size_t)y * (size_t)c->width + (size_t)r.x;
        for (int x = 0; x < r.w; x++) row[x] = colour;
    }
}

void du_hline(struct du_canvas *c, int x, int y, int w, uint32_t colour)
{ du_fill(c, (struct du_rect){x, y, w, 1}, colour); }

void du_vline(struct du_canvas *c, int x, int y, int h, uint32_t colour)
{ du_fill(c, (struct du_rect){x, y, 1, h}, colour); }

void du_outline(struct du_canvas *c, struct du_rect r, uint32_t colour)
{
    if (r.w <= 0 || r.h <= 0) return;
    du_hline(c, r.x, r.y, r.w, colour); du_hline(c, r.x, r.y + r.h - 1, r.w, colour);
    du_vline(c, r.x, r.y, r.h, colour); du_vline(c, r.x + r.w - 1, r.y, r.h, colour);
}

static void pair(struct du_canvas *c, struct du_rect r, uint32_t tl, uint32_t br)
{
    if (r.w <= 0 || r.h <= 0) return;
    du_hline(c, r.x, r.y, r.w - 1, tl); du_vline(c, r.x, r.y, r.h - 1, tl);
    du_hline(c, r.x, r.y + r.h - 1, r.w, br); du_vline(c, r.x + r.w - 1, r.y, r.h, br);
}

void du_edge(struct du_canvas *c, struct du_rect r, enum du_edge e)
{
    struct du_rect in = {r.x + 1, r.y + 1, r.w - 2, r.h - 2};
    switch (e) {
    case DU_RAISED: pair(c, r, DU_LIGHT, DU_DARK); pair(c, in, DU_HILITE, DU_SHADOW); break;
    case DU_BUTTON_EDGE: pair(c, r, DU_HILITE, DU_DARK); pair(c, in, DU_LIGHT, DU_SHADOW); break;
    case DU_SUNKEN: pair(c, r, DU_SHADOW, DU_HILITE); pair(c, in, DU_DARK, DU_LIGHT); break;
    case DU_PRESSED: du_outline(c, r, DU_DARK); du_outline(c, in, DU_SHADOW); break;
    case DU_RAISED_THIN: pair(c, r, DU_HILITE, DU_SHADOW); break;
    case DU_SUNKEN_THIN: pair(c, r, DU_SHADOW, DU_HILITE); break;
    case DU_ETCHED: pair(c, r, DU_SHADOW, DU_HILITE); pair(c, in, DU_HILITE, DU_SHADOW); break;
    }
}

void du_dotted(struct du_canvas *c, struct du_rect r)
{
    /* Windows draws focus as an inverting dotted frame; invert explicitly. */
    struct du_rect v = visible(c, r);
    for (int y = v.y; y < v.y + v.h; y++) for (int x = v.x; x < v.x + v.w; x++) {
        int edge = y == r.y || y == r.y + r.h - 1 || x == r.x || x == r.x + r.w - 1;
        if (edge && !((x + y) & 1)) {
            uint32_t *p = c->pixels + (size_t)y * (size_t)c->width + (size_t)x;
            *p = ~*p & 0xffffffu;
        }
    }
}

void du_checker(struct du_canvas *c, struct du_rect r, uint32_t a, uint32_t b)
{
    struct du_rect v = visible(c, r);
    for (int y = v.y; y < v.y + v.h; y++) for (int x = v.x; x < v.x + v.w; x++)
        c->pixels[(size_t)y * (size_t)c->width + (size_t)x] = ((x + y) & 1) ? a : b;
}

void du_triangle(struct du_canvas *c, int cx, int cy, int dir, int size, uint32_t colour)
{
    /* dir: 0 up, 1 down, 2 left, 3 right; size = rows from apex to base. */
    for (int i = 0; i < size; i++) {
        if (dir == 0) du_hline(c, cx - i, cy - size / 2 + i, 2 * i + 1, colour);
        else if (dir == 1) du_hline(c, cx - i, cy + size / 2 - i, 2 * i + 1, colour);
        else if (dir == 2) du_vline(c, cx - size / 2 + i, cy - i, 2 * i + 1, colour);
        else du_vline(c, cx + size / 2 - i, cy - i, 2 * i + 1, colour);
    }
}

size_t du_utf8_prev(const char *s, size_t n)
{
    if (!n) return 0;
    do n--; while (n && ((unsigned char)s[n] & 0xc0) == 0x80);
    return n;
}

size_t du_utf8_next(const char *s, size_t n)
{
    if (!s[n]) return n;
    n++;
    while (((unsigned char)s[n] & 0xc0) == 0x80) n++;
    return n;
}

size_t du_cells(const char *s)
{
    size_t n = 0;
    for (; *s; s++) if (((unsigned char)*s & 0xc0) != 0x80) n++;
    return n;
}

int du_text_width(const char *s) { return (int)du_cells(s) * DU_CHAR_W; }

static void glyph(struct du_canvas *c, int x, int y, unsigned ch, uint32_t colour)
{
    /* The compositor's g, p, q and y sit on the baseline; one row lower
     * they read as descenders. Same bitmaps, deterministic placement. */
    int drop = ch == 'g' || ch == 'p' || ch == 'q' || ch == 'y';
    struct du_rect cell = visible(c, (struct du_rect){x, y, 5, DU_GLYPH_H + 1});
    if (!cell.w || !cell.h) return;
    for (unsigned row = 0; row < DU_GLYPH_H; row++) {
        uint8_t bits = desk_glyph_row(ch, row);
        int py = y + (int)row + drop;
        for (unsigned col = 0; col < 5; col++)
            if (bits & (1u << (4 - col)) && du_contains(cell, x + (int)col, py))
                c->pixels[(size_t)py * (size_t)c->width + (size_t)(x + (int)col)] = colour;
    }
}

void du_text_n(struct du_canvas *c, int x, int y, const char *s, size_t cells, uint32_t colour)
{
    int64_t at = x;
    for (size_t n = 0; *s && n < cells; s++) {
        unsigned ch = (unsigned char)*s;
        if ((ch & 0xc0) == 0x80) continue;
        if (at >= (int64_t)c->clip.x + c->clip.w) break;
        if (at + 5 >= c->clip.x && at > -32768 && at < 32768) glyph(c, (int)at, y, ch >= 128 ? '?' : ch, colour);
        at += DU_CHAR_W; n++;
    }
}

void du_text(struct du_canvas *c, int x, int y, const char *s, uint32_t colour)
{ du_text_n(c, x, y, s, (size_t)-1, colour); }

void du_text_fit(struct du_canvas *c, int x, int y, int width, const char *s, uint32_t colour)
{
    size_t cells = du_cells(s), room = width > 0 ? (size_t)width / DU_CHAR_W : 0;
    if (cells <= room) { du_text(c, x, y, s, colour); return; }
    if (room <= 3) { du_text_n(c, x, y, "...", room, colour); return; }
    du_text_n(c, x, y, s, room - 3, colour);
    du_text(c, x + (int)(room - 3) * DU_CHAR_W, y, "...", colour);
}

void du_text_disabled(struct du_canvas *c, int x, int y, const char *s)
{ du_text(c, x + 1, y + 1, s, DU_HILITE); du_text(c, x, y, s, DU_GRAY); }

void du_label(struct du_canvas *c, int x, int y, const char *s, uint32_t colour, int underline)
{
    char plain[256]; size_t n = 0; int mark = -1, cells = 0;
    for (; *s && n + 1 < sizeof(plain); s++) {
        if (*s == '&' && s[1] && s[1] != '&') { mark = cells; continue; }
        if (*s == '&' && s[1] == '&') s++;
        if (((unsigned char)*s & 0xc0) != 0x80) cells++;
        plain[n++] = *s;
    }
    plain[n] = 0;
    du_text(c, x, y, plain, colour);
    if (underline && mark >= 0) du_hline(c, x + mark * DU_CHAR_W, y + DU_GLYPH_H + 1, 5, colour);
}

static uint32_t mix(uint32_t a, uint32_t b, unsigned alpha)
{
    uint32_t out = 0;
    for (unsigned shift = 0; shift < 24; shift += 8)
        out |= ((((a >> shift) & 255) * alpha + ((b >> shift) & 255) * (255 - alpha) + 127) / 255) << shift;
    return out;
}

void du_icon(struct du_canvas *c, int x, int y, const uint32_t *argb, int size, enum du_icon_mode mode)
{
    if (!argb || size <= 0 || size > 64) return;
    struct du_rect v = visible(c, (struct du_rect){x, y, size, size});
    for (int py = v.y; py < v.y + v.h; py++) for (int px = v.x; px < v.x + v.w; px++) {
        uint32_t s = argb[(py - y) * size + (px - x)];
        unsigned alpha = s >> 24;
        if (!alpha) continue;
        uint32_t rgb = s & 0xffffffu;
        if (mode == DU_ICON_DISABLED) {
            unsigned l = (((rgb >> 16) & 255) * 77 + ((rgb >> 8) & 255) * 150 + (rgb & 255) * 29) >> 8;
            l = (l + 2 * 0xc0) / 3;
            rgb = (l << 16) | (l << 8) | l;
            alpha = alpha * 3 / 4;
        } else if (mode == DU_ICON_GHOST) alpha /= 2;
        uint32_t *p = c->pixels + (size_t)py * (size_t)c->width + (size_t)px;
        *p = mix(rgb, *p, alpha);
        if (mode == DU_ICON_SELECTED) *p = mix(DU_SELECT, *p, 128);
    }
}

void du_format_key(char *out, size_t size, unsigned key, unsigned modifiers)
{
    static const struct { unsigned code; const char *name; } names[] = {
        {DU_KEY_ENTER, "Enter"}, {DU_KEY_BACKSPACE, "Backspace"}, {DU_KEY_DELETE, "Del"},
        {DU_KEY_ESC, "Esc"}, {DU_KEY_TAB, "Tab"}, {DU_KEY_SPACE, "Space"},
        {DU_KEY_LEFT, "Left"}, {DU_KEY_RIGHT, "Right"}, {DU_KEY_UP, "Up"}, {DU_KEY_DOWN, "Down"},
        {DU_KEY_HOME, "Home"}, {DU_KEY_END, "End"}, {DU_KEY_PGUP, "PgUp"}, {DU_KEY_PGDN, "PgDn"},
        {DU_KEY_INSERT, "Ins"}, {0x57, "F11"}, {0x58, "F12"}};
    static const char letters[] = "\0\0" "1234567890-=\0\0" "QWERTYUIOP[]\0\0" "ASDFGHJKL;'`\0\\" "ZXCVBNM,./";
    char name[16] = "";
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (names[i].code == key) snprintf(name, sizeof(name), "%s", names[i].name);
    if (!name[0] && key >= DU_KEY_F1 && key <= DU_KEY_F10) snprintf(name, sizeof(name), "F%u", key - DU_KEY_F1 + 1);
    if (!name[0] && key < sizeof(letters) && letters[key]) { name[0] = letters[key]; name[1] = 0; }
    snprintf(out, size, "%s%s%s%s", modifiers & DU_CTRL ? "Ctrl+" : "", modifiers & DU_ALT ? "Alt+" : "",
             modifiers & DU_SHIFT ? "Shift+" : "", name);
    if (!name[0]) out[0] = 0;
}
