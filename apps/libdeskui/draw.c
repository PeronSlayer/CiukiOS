/* SPDX-License-Identifier: MIT
 * Clipped integer drawing primitives. Text, bevels and faces follow the
 * current theme (apps/desktop/theme.c). */
#include "deskui.h"
#include <stdio.h>
#include <string.h>
#ifndef DESK_PROFILE
#define DESK_PROFILE(name, ...) do { __VA_ARGS__; } while (0)
#endif

/* Integer row painter in compositor.c, also linked into libdeskui. */
extern void desk_shadow_rows(struct desk_paint *, int, int, int, int, int, uint32_t);

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

static uint32_t mix(uint32_t a, uint32_t b, unsigned alpha)
{
    if (alpha==255) return a&0xffffffu;
    if (!alpha) return b&0xffffffu;
    uint32_t rb=(a&0xff00ffu)*alpha+(b&0xff00ffu)*(255-alpha)+0x007f007fu;
    uint32_t g=((a>>8)&255u)*alpha+((b>>8)&255u)*(255-alpha)+127u;
    rb=(rb+0x00010001u+((rb>>8)&0xff00ffu))>>8;
    g=(g+1+(g>>8))>>8;
    return (rb&0xff00ffu)|(g<<8);
}

void du_fill(struct du_canvas *c, struct du_rect r, uint32_t colour)
{
    r = visible(c, r);
    if (r.w<=0 || r.h<=0) return;
    int stride=c->width;
    uint32_t *first=c->pixels+(size_t)r.y*(size_t)stride+(size_t)r.x;
    for (int y=0; y<r.h; y++) {
        uint32_t *row=first+(size_t)y*stride;
        if (y && r.w>=16) memcpy(row,first,(size_t)r.w*sizeof(*row));
        else for (int x=0; x<r.w; x++) row[x]=colour;
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
    int platinum = desk_theme->control_style == DESK_CONTROLS_PLATINUM;
    switch (e) {
    case DU_RAISED:
        if (platinum) { du_outline(c, r, desk_theme->outline); pair(c, in, DU_HILITE, DU_SHADOW); }
        else { pair(c, r, DU_LIGHT, DU_DARK); pair(c, in, DU_HILITE, DU_SHADOW); }
        break;
    case DU_BUTTON_EDGE: pair(c, r, DU_HILITE, DU_DARK); pair(c, in, DU_LIGHT, DU_SHADOW); break;
    case DU_SUNKEN:
        if (platinum) { pair(c, r, DU_SHADOW, DU_HILITE); pair(c, in, DU_DARK, DU_FACE); }
        else { pair(c, r, DU_SHADOW, DU_HILITE); pair(c, in, DU_DARK, DU_LIGHT); }
        break;
    case DU_PRESSED: du_outline(c, r, DU_DARK); du_outline(c, in, DU_SHADOW); break;
    case DU_RAISED_THIN: pair(c, r, DU_HILITE, DU_SHADOW); break;
    case DU_SUNKEN_THIN: pair(c, r, DU_SHADOW, DU_HILITE); break;
    case DU_ETCHED: pair(c, r, DU_SHADOW, DU_HILITE); pair(c, in, DU_HILITE, DU_SHADOW); break;
    }
}

static struct desk_paint painter(struct du_canvas *c)
{
    struct du_rect v = visible(c, c->clip);
    return (struct desk_paint){c->pixels, c->width, c->height, v.x, v.y, v.w, v.h};
}

void du_raised(struct du_canvas *c, struct du_rect r, int pressed, int hot)
{ struct desk_paint p = painter(c); desk_paint_raised(&p, r.x, r.y, r.w, r.h, pressed, hot); }

void du_shadow(struct du_canvas *c, struct du_rect r, int size)
{ struct desk_paint p = painter(c); DESK_PROFILE("shadows", desk_shadow_rows(&p, r.x, r.y, r.w, r.h, size, desk_theme->drop_shadow)); }

void du_well(struct du_canvas *c, struct du_rect r, uint32_t colour)
{
    du_fill(c, r, colour);
    if (desk_theme->control_style == DESK_CONTROLS_PLATINUM) {
        /* Platinum field: soft outer bevel, dark inner rim, light floor. */
        pair(c, r, desk_mix(DU_SHADOW, DU_FACE, 170), DU_HILITE);
        struct du_rect in = {r.x + 1, r.y + 1, r.w - 2, r.h - 2};
        pair(c, in, DU_DARK, desk_mix(DU_SHADOW, colour, 90));
    } else du_edge(c, r, DU_SUNKEN);
}

void du_focus_ring(struct du_canvas *c, struct du_rect r)
{
    if (desk_theme->focus_style == DESK_FOCUS_DOTTED) { du_dotted(c, r); return; }
    uint32_t ring = desk_theme->focus;
    du_outline(c, r, ring);
    du_outline(c, (struct du_rect){r.x + 1, r.y + 1, r.w - 2, r.h - 2}, desk_mix(ring, DU_HILITE, 170));
}

int du_theme_use(struct du_ui *u, const char *name)
{
    if (desk_theme_use(name)) return -1;
    if (u) u->dirty = 1;
    return 0;
}

const char *du_theme_name(void) { return desk_theme->name; }

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
    if (v.w <= 0 || v.h <= 0) return;
    uint32_t *first = c->pixels + (size_t)v.y * (size_t)c->width + (size_t)v.x;
    int stride=c->width;
    for (int y = 0; y < v.h; y++) {
        uint32_t *row=first+(size_t)y*stride;
        for (int x = 0; x < v.w; x++) row[x] = ((v.x + x + v.y + y) & 1) ? a : b;
    }
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

int du_text_width(const char *s) { return desk_text_measure(desk_theme->font_ui, s); }
int du_text_width_font(int font, const char *s) { return desk_text_measure(font, s); }

static void text_font_rows(struct du_canvas *c, int font, int x, int y, const char *s, size_t chars, uint32_t colour)
{
    if (x > c->clip.x + c->clip.w || y > c->clip.y + c->clip.h) return;
    struct desk_paint p = painter(c);
    const struct desk_font *f=desk_font(font);
    int baseline=y+desk_font_cap(font), aa=desk_theme->text_aa;
    for (size_t n=0; *s && n<chars; n++) {
        size_t used; unsigned ch=desk_utf8_decode(s,&used); s+=used;
        /* Same font coverage as theme.c: ASCII, Latin-1, bullet, ellipsis,
         * otherwise '?'. Font rasters and pen advances stay unchanged. */
        int index=ch>=0x20 && ch<0x7f ? (int)ch-0x20 : ch>=0xa0 && ch<=0xff ? 95+(int)ch-0xa0 :
                  ch==0x2022 ? 191 : ch==0x2026 ? 192 : '?'-0x20;
        const struct desk_glyph *g=&f->glyphs[index];
        int gx=x+g->left, gy=baseline+g->top;
        struct du_rect v=visible(c,(struct du_rect){gx,gy,g->w,g->h});
        if (v.w>0 && v.h>0 && x<p.cx+p.cw) {
            const uint8_t *first_src=f->alpha+g->offset+(size_t)(v.y-gy)*g->w+v.x-gx;
            uint32_t *first_dst=c->pixels+(size_t)v.y*c->width+v.x;
            int stride=c->width;
            for (int r=0; r<v.h; r++) {
                const uint8_t *src=first_src+(size_t)r*g->w;
                uint32_t *dst=first_dst+(size_t)r*stride;
                for (int col=0; col<v.w; col++) {
                    unsigned a=src[col];
                    if (!aa) a=a>=104 ? 255 : 0;
                    if (a) dst[col]=mix(colour,dst[col],a);
                }
            }
        }
        x+=g->advance;
        if (x>p.cx+p.cw+64) break;
    }
}

static void text_font_n(struct du_canvas *c, int font, int x, int y, const char *s, size_t chars, uint32_t colour)
{ DESK_PROFILE("text", text_font_rows(c,font,x,y,s,chars,colour)); }

void du_text_font(struct du_canvas *c, int font, int x, int y, const char *s, uint32_t colour)
{ text_font_n(c, font, x, y, s, (size_t)-1, colour); }

void du_text_n(struct du_canvas *c, int x, int y, const char *s, size_t cells, uint32_t colour)
{ text_font_n(c, desk_theme->font_ui, x, y, s, cells, colour); }

void du_text(struct du_canvas *c, int x, int y, const char *s, uint32_t colour)
{ du_text_n(c, x, y, s, (size_t)-1, colour); }

void du_text_fit(struct du_canvas *c, int x, int y, int width, const char *s, uint32_t colour)
{
    int font = desk_theme->font_ui;
    if (desk_text_measure(font, s) <= width) { du_text(c, x, y, s, colour); return; }
    static const char ellipsis[] = "\xe2\x80\xa6";
    int dots = desk_text_measure(font, ellipsis);
    size_t room = width > dots ? desk_text_fit_chars(font, s, width - dots) : 0;
    du_text_n(c, x, y, s, room, colour);
    du_text(c, x + desk_text_measure_n(font, s, room), y, ellipsis, colour);
}

void du_text_disabled(struct du_canvas *c, int x, int y, const char *s)
{ du_text(c, x + 1, y + 1, s, DU_HILITE); du_text(c, x, y, s, DU_GRAY); }

static void strip_mnemonic(const char *s, char *plain, size_t size, int *mark)
{
    size_t n = 0; int chars = 0;
    *mark = -1;
    for (; *s && n + 1 < size; s++) {
        if (*s == '&' && s[1] && s[1] != '&') { *mark = chars; continue; }
        if (*s == '&' && s[1] == '&') s++;
        if (((unsigned char)*s & 0xc0) != 0x80) chars++;
        plain[n++] = *s;
    }
    plain[n] = 0;
}

int du_label_width(const char *s)
{ char plain[256]; int mark; strip_mnemonic(s, plain, sizeof(plain), &mark); return du_text_width(plain); }

void du_label(struct du_canvas *c, int x, int y, const char *s, uint32_t colour, int underline)
{
    char plain[256]; int mark;
    strip_mnemonic(s, plain, sizeof(plain), &mark);
    du_text(c, x, y, plain, colour);
    if (underline && mark >= 0) {
        int font = desk_theme->font_ui, at = desk_text_measure_n(font, plain, (size_t)mark);
        size_t skip = 0;
        for (int k = 0; k < mark && plain[skip]; k++) skip = du_utf8_next(plain, skip);
        size_t used; unsigned ch = desk_utf8_decode(plain + skip, &used);
        int w = desk_text_advance(font, ch) - 1;
        du_hline(c, x + at, y + DU_GLYPH_H + 2, w > 1 ? w : 1, colour);
    }
}

void du_icon_blit(struct du_canvas *c, int x, int y, const uint32_t *argb, int size, enum du_icon_mode mode)
{
    if (!argb || size <= 0 || size > 64) return;
    struct du_rect v = visible(c, (struct du_rect){x, y, size, size});
    if (v.w <= 0 || v.h <= 0) return;
    uint32_t *first_dst = c->pixels + (size_t)v.y * (size_t)c->width + (size_t)v.x;
    const uint32_t *first_src = argb + (v.y - y) * size + v.x - x;
    int stride=c->width;
    for (int py = 0; py < v.h; py++) {
      uint32_t *dst=first_dst+(size_t)py*stride;
      const uint32_t *src=first_src+(size_t)py*size;
      for (int px = 0; px < v.w; px++) {
        uint32_t s = src[px];
        unsigned alpha = s >> 24;
        if (!alpha) continue;
        uint32_t rgb = s & 0xffffffu;
        if (mode == DU_ICON_DISABLED) {
            unsigned l = (((rgb >> 16) & 255) * 77 + ((rgb >> 8) & 255) * 150 + (rgb & 255) * 29) >> 8;
            l = (l + 2 * 0xc0) / 3;
            rgb = (l << 16) | (l << 8) | l;
            alpha = alpha * 3 / 4;
        } else if (mode == DU_ICON_GHOST) alpha /= 2;
        uint32_t *p = dst + px;
        *p = alpha == 255 ? rgb : mix(rgb, *p, alpha);
        if (mode == DU_ICON_SELECTED) *p = mix(DU_SELECT, *p, 128);
      }
    }
}

void du_icon_draw(struct du_canvas *c, int x, int y, const struct du_icon *icon, enum du_icon_mode mode)
{ if (icon) du_icon_blit(c, x, y, icon->argb, (int)icon->size, mode); }

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
