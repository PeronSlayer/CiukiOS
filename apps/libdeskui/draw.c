/* SPDX-License-Identifier: MIT
 * Clipped integer drawing primitives. Text, bevels and faces follow the
 * current theme (apps/desktop/theme.c). */
#include "deskui.h"
#include "../desktop/sounds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef DESK_PROFILE
#define DESK_PROFILE(name, ...) do { __VA_ARGS__; } while (0)
#endif

/* Integer row painter in compositor.c, also linked into libdeskui. */
extern void desk_shadow_rows(struct desk_paint *, int, int, int, int, int, uint32_t);

int du_text_wrap(struct du_canvas *c, struct du_rect box, const char *text, uint32_t ink)
{
    int lines = 0, pitch = desk_font_line(desk_theme->font_ui) + 1;
    const char *s = text ? text : "";
    struct du_rect old = c ? c->clip : box;
    if (c) c->clip = du_intersection(old, box);
    do {
        char line[512]; size_t n = 0, word = 0, used = 0;
        const char *start = s;
        while (*s && *s != '\n' && n + 5 < sizeof(line)) {
            desk_utf8_decode(s, &used); if (!used) used = 1;
            memcpy(line + n, s, used); line[n + used] = 0;
            if (du_text_width(line) > box.w && n) break;
            if (*s == ' ' || *s == '\t') word = n;
            n += used; s += used;
        }
        if (*s && *s != '\n' && word) { n = word; s = start + word; }
        line[n] = 0;
        if (c) {
            int y=box.y+lines*pitch;
            if (!strncmp(line,"---",3)) {
                du_hline(c,box.x,y+3,box.w,DU_SHADOW);
                du_hline(c,box.x,y+4,box.w,DU_HILITE);
            } else {
                char *tab=strchr(line,'\t');
                if (tab) {
                    *tab=0; du_text(c,box.x,y,line,ink);
                    int col=du_text_width(line)+8; if (col<84) col=84;
                    du_text_fit(c,box.x+col,y,box.w-col,tab+1,ink);
                } else du_text(c,box.x,y,line,ink);
            }
        }
        lines++;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\n') { s++; if (!*s) { lines++; break; } }
    } while (*s);
    if (c) c->clip = old;
    return lines;
}

void du_message_size(const char *text, int icon, unsigned buttons, int max_width, int *w, int *h)
{
    int room = max_width < 480 ? max_width : 480;
    int iw = icon ? 46 : 0, widest = 0;
    const char *s = text;
    do {
        char line[512]; const char *end = strchr(s, '\n');
        size_t n = end ? (size_t)(end-s) : strlen(s);
        if (n >= sizeof(line)) n = sizeof(line)-1;
        memcpy(line,s,n); line[n]=0;
        int width = du_text_width(line); if (width>widest) widest=width;
        if (!end) break;
        s=end+1;
    } while (*s);
    *w = widest + iw + 32;
    int row = (int)buttons * 81 - 6 + 32;
    if (*w < row) *w = row;
    if (*w < 300) *w = 300;
    if (*w > room) *w = room;
    int content = du_text_wrap(NULL,(struct du_rect){0,0,*w-iw-32,0},text,0) *
                  (desk_font_line(desk_theme->font_ui)+1);
    if (icon && content < 32) content=32;
    *h = 16 + content + 18 + 23 + 12;
}

const char *du_message_icon_name(int icon)
{
    /* Tango 0.8.90's question symbol is help-browser (the existing
     * dialog:question mapping); there is no dialog-question asset. */
    static const char *const names[] = {NULL,"dialog-information","dialog-warning","dialog-error","help-browser"};
    return icon>0 && icon<5 ? names[icon] : NULL;
}
unsigned du_message_sound(int icon)
{
    return icon == DU_DLG_ICON_ERROR || icon == DU_DLG_ICON_WARNING ? SOUND_ERROR :
           icon == DU_DLG_ICON_QUESTION ? SOUND_QUESTION : SOUND_INFORMATION;
}

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

int du_commands(struct du_canvas *c, const struct ciuki_batch_record *commands, unsigned count,
                const void *payload, const uint32_t *source, unsigned sw, unsigned sh)
{
    if (!c || !c->pixels || (!commands && count) || count>64) return -EINVAL;
    /* Validate the whole local batch before touching its private canvas. */
    for (unsigned n=0;n<count;n++) {
        const struct ciuki_batch_record *r=&commands[n];
        if (r->dw>16384 || r->dh>16384 || r->clip_count || r->record_bytes!=128) return -EINVAL;
        if (r->opcode!=CIUKI_CMD_MONO && r->opcode!=CIUKI_CMD_INVERT && r->opcode!=CIUKI_CMD_KEYED) return -EOPNOTSUPP;
        if (r->opcode==CIUKI_CMD_MONO && (!payload || r->sx<0 || r->sy<0 || r->stride<(r->sw+7)/8 ||
            !r->sw || !r->sh || (uint64_t)(r->sh-1)*r->stride+(r->sw+7)/8>r->data_bytes)) return -EINVAL;
        if (r->opcode==CIUKI_CMD_KEYED && (!source || source==c->pixels)) return -EINVAL;
    }
    for (unsigned n=0;n<count;n++) {
        const struct ciuki_batch_record *r=&commands[n];
        struct du_rect v=visible(c,(struct du_rect){r->dx,r->dy,(int)r->dw,(int)r->dh});
        for (int y=v.y;y<v.y+v.h;y++) for (int x=v.x;x<v.x+v.w;x++) {
            uint32_t *p=c->pixels+(size_t)y*c->width+x;
            int64_t u=(int64_t)r->sx+x-r->dx, t=(int64_t)r->sy+y-r->dy;
            if (r->opcode==CIUKI_CMD_INVERT) *p^=0xffffffu;
            else if (r->opcode==CIUKI_CMD_KEYED) {
                if (u<0 || t<0 || u>=sw || t>=sh) continue;
                uint32_t value=source[(size_t)t*sw+u]&0xffffffu;
                if (value!=(r->key&0xffffffu)) *p=value;
            } else {
                if (u<0 || t<0 || u>=r->sw || t>=r->sh) continue;
                const uint8_t *data=(const uint8_t *)payload+r->data_offset;
                int bit=data[t*r->stride+u/8]&(128u>>(u%8));
                if (bit || !(r->flags&CIUKI_CMD_TRANSPARENT_BG)) *p=(bit ? r->foreground : r->background)&0xffffffu;
            }
        }
    }
    return 0;
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
    /* Invert the pixels of the dotted focus frame. */
    struct du_rect v = visible(c, r);
    struct ciuki_batch_record batch[8]; unsigned count=0;
    for (int y = v.y; y < v.y + v.h; y++) for (int x = v.x; x < v.x + v.w; x++) {
        int edge = y == r.y || y == r.y + r.h - 1 || x == r.x || x == r.x + r.w - 1;
        if (edge && !((x + y) & 1)) {
            uint32_t *p = c->pixels + (size_t)y * (size_t)c->width + (size_t)x;
            if (*p&0xff000000u) { *p=~*p&0xffffffu; continue; }
            batch[count++]=(struct ciuki_batch_record){.opcode=CIUKI_CMD_INVERT,.record_bytes=128,.dx=x,.dy=y,.dw=1,.dh=1};
            if (count==8) { (void)du_commands(c,batch,count,NULL,NULL,0,0); count=0; }
        }
    }
    if (count) (void)du_commands(c,batch,count,NULL,NULL,0,0);
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
            /* Binary text can use MSB-left expansion exactly. Antialiased
             * glyphs keep their established coverage/mixing path. */
            if (!aa && g->w<=64 && g->h<=64) {
                uint8_t bits[512]={0}; unsigned stride=(g->w+7)/8;
                for (unsigned yy=0;yy<g->h;yy++) for (unsigned xx=0;xx<g->w;xx++)
                    if (f->alpha[g->offset+yy*g->w+xx]>=104) bits[yy*stride+xx/8]|=128u>>(xx%8);
                struct ciuki_batch_record mono={.opcode=CIUKI_CMD_MONO,.record_bytes=128,.flags=CIUKI_CMD_TRANSPARENT_BG,
                    .dx=gx,.dy=gy,.dw=g->w,.dh=g->h,.sw=g->w,.sh=g->h,.foreground=colour,.stride=stride,
                    .data_bytes=g->h ? (g->h-1)*stride+(g->w+7)/8 : 0};
                if (!du_commands(c,&mono,1,bits,NULL,0,0)) { x+=g->advance; if (x>p.cx+p.cw+64) break; continue; }
            }
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
    if (mode==DU_ICON_NORMAL) {
        int binary=1; uint32_t key=0;
        for (int i=0;i<size*size;i++) if ((argb[i]>>24)!=0 && (argb[i]>>24)!=255) { binary=0; break; }
        if (binary) {
            /* At most 4096 opaque colours: a missing RGB key exists among
             * 4097 candidates. Preserve original approved icon pixels. */
            for (;key<=4096;key++) {
                int found=0; for (int i=0;i<size*size;i++) if ((argb[i]>>24)==255 && (argb[i]&0xffffffu)==key) { found=1; break; }
                if (!found) break;
            }
            /* One keyed row at a time from a stack row: no heap allocation on the
             * paint path (the footprint gate counts every allocation after the
             * first frame). Rows are independent, so a validation refusal can
             * only happen on the first row and leaves the canvas untouched. */
            uint32_t keyed[64]; int first=1, refused=0;
            for (int row=v.y-y; row<v.y-y+v.h; row++) {
                for (int i=0;i<size;i++) { uint32_t px=argb[row*size+i]; keyed[i]=(px>>24) ? px&0xffffffu : key; }
                struct ciuki_batch_record record={.opcode=CIUKI_CMD_KEYED,.record_bytes=128,.dx=x,.dy=y+row,.dw=size,.dh=1,.key=key};
                int error=du_commands(c,&record,1,NULL,keyed,size,1);
                if (error) { if (first) refused=1; break; }
                first=0;
            }
            if (!refused) return;
        }
    }
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

void du_progress_paint(struct du_canvas *c, struct du_rect b, int value, int total, uint64_t phase)
{
    int inner = b.w - 4, filled = total > 0 ? (int)((int64_t)inner * (value > total ? total : value) / total) : 0;
    if (total<=0) {
        du_well(c,b,DU_FACE);
        int room=b.w-8, pulse=room<48 ? room : 48;
        if (pulse<=0) return;
        int offset=(int)((phase/80u)%(unsigned)(room-pulse+1));
        du_fill(c,(struct du_rect){b.x+4+offset,b.y+4,pulse,b.h-8},DU_ACCENT);
        return;
    }
    if (desk_theme->control_style == DESK_CONTROLS_PLATINUM) {
        /* Platinum: rounded groove, accent bar with a light top half. */
        du_well(c, b, desk_mix(DU_SHADOW, DU_FACE, 70));
        struct desk_paint p = {c->pixels, c->width, c->height, c->clip.x, c->clip.y, c->clip.w, c->clip.h};
        if (filled > 0) {
            int h = b.h - 4;
            desk_paint_vgradient(&p, b.x + 2, b.y + 2, filled, h / 2, desk_mix(0xffffffu, DU_ACCENT, 120), desk_mix(0xffffffu, DU_ACCENT, 40));
            desk_paint_vgradient(&p, b.x + 2, b.y + 2 + h / 2, filled, h - h / 2, DU_ACCENT, desk_mix(0x000000u, DU_ACCENT, 50));
            for (int x = 10; x < filled; x += 12) du_vline(c, b.x + 2 + x, b.y + 2, h, desk_mix(0xffffffu, DU_ACCENT, 50));
        }
        return;
    }
    du_fill(c, b, DU_FACE); du_edge(c, b, DU_SUNKEN_THIN);
    for (int x = 0; x < filled; x += 10) {
        int bw = filled - x < 8 ? filled - x : 8;
        du_fill(c, (struct du_rect){b.x + 2 + x, b.y + 2, bw, b.h - 4}, DU_SELECT);
    }
}
static int selection_class(unsigned char ch)
{
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') return 0;
    return ch >= 128 || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '_' ? 1 : 2;
}

void du_text_unit(const char *text, size_t at, unsigned granularity, size_t *lo, size_t *hi)
{
    size_t len = strlen(text);
    if (at > len) at = len;
    while (at && at < len && ((unsigned char)text[at] & 0xc0) == 0x80) at--;
    *lo = *hi = at;
    if (granularity <= 1 || !len) return;
    if (granularity == 3) {
        while (*lo && text[*lo - 1] != '\n') --*lo;
        while (*hi < len && text[*hi] != '\n') ++*hi;
        if (*hi < len) ++*hi;
        return;
    }
    if (at == len || text[at] == '\n') {
        if (at && text[at-1] != '\n') at = du_utf8_prev(text, at);
        else { *lo = at; *hi = at < len ? at + 1 : at; return; }
    }
    *lo = at; *hi = du_utf8_next(text, at);
    int cls = selection_class((unsigned char)text[at]);
    while (*lo && text[*lo - 1] != '\n') {
        size_t prev = du_utf8_prev(text, *lo);
        if (selection_class((unsigned char)text[prev]) != cls) break;
        *lo = prev;
    }
    while (*hi < len && text[*hi] != '\n' && selection_class((unsigned char)text[*hi]) == cls)
        *hi = du_utf8_next(text, *hi);
}

void du_select_click(struct du_selection *s, const char *text, size_t at, int extend,
                     uint64_t ns, int x, int y, size_t *anchor, size_t *cursor)
{
    int near = x - s->x <= 4 && s->x - x <= 4 && y - s->y <= 4 && s->y - y <= 4;
    s->clicks = !extend && s->ns && ns >= s->ns && ns - s->ns <= 500000000ull && near && s->clicks < 3 ? s->clicks + 1 : 1;
    s->ns = ns ? ns : 1; s->x = x; s->y = y;
    du_text_unit(text, at, s->clicks, &s->lo, &s->hi);
    if (!extend) *anchor = s->lo;
    *cursor = s->hi;
}

void du_select_drag(const struct du_selection *s, const char *text, size_t at, size_t *anchor, size_t *cursor)
{
    size_t lo, hi, start, end, ignored;
    /* Input can edit the buffer while the pointer is captured. Saved unit
     * boundaries must still lie in the current buffer. */
    du_text_unit(text, s->lo, 1, &start, &ignored);
    du_text_unit(text, s->hi, 1, &end, &ignored);
    du_text_unit(text, at, s->clicks, &lo, &hi);
    if (s->clicks <= 1) *cursor = lo;
    else if (at < start) { *anchor = end; *cursor = lo; }
    else { *anchor = start; *cursor = hi > end ? hi : end; }
}
