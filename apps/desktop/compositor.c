/* SPDX-License-Identifier: MIT
 * Integer-only CPU compositor. All drawing is clipped before pixel access. */
#include "desktop.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* The host benchmark supplies this macro; native builds contain no timers. */
#ifndef DESK_PROFILE
#define DESK_PROFILE(name, ...) do { __VA_ARGS__; } while (0)
#endif
/* The shell (shell.c) is linked only into /Programs/desktop; the demo and the
 * libdeskui programs reuse this file for glyphs and frames without it. */
extern void desk_shell_background(struct desktop *, struct desk_box) __attribute__((weak));
extern void desk_shell_topbar(struct desktop *, struct desk_box) __attribute__((weak));
extern void desk_shell_overlay(struct desktop *, struct desk_box) __attribute__((weak));
extern void desk_internal_paint(struct desktop *, int, struct desk_box, struct desk_box) __attribute__((weak));
extern const uint32_t *desk_shell_window_icon(struct desktop *, int, int) __attribute__((weak));
#include "catalog.h"
int desk_intersect(struct desk_box a, struct desk_box b, struct desk_box *out)
{
    int64_t x = a.x > b.x ? a.x : b.x, y = a.y > b.y ? a.y : b.y;
    int64_t ax = (int64_t)a.x+a.w, bx = (int64_t)b.x+b.w;
    int64_t ay = (int64_t)a.y+a.h, by = (int64_t)b.y+b.h;
    int64_t r = ax < bx ? ax : bx, t = ay < by ? ay : by;
    if (a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0 || r <= x || t <= y) return 0;
    *out = (struct desk_box){(int32_t)x,(int32_t)y,(int32_t)(r-x),(int32_t)(t-y)};
    return 1;
}
struct desk_box desk_frame(uint32_t w, uint32_t h, int32_t x, int32_t y)
{ return (struct desk_box){x,y,(int32_t)w+DESK_BORDER*2,(int32_t)h+DESK_TITLE+DESK_BORDER}; }
struct desk_box desk_content(const struct desk_client *c)
{ return (struct desk_box){c->frame.x+DESK_BORDER,c->frame.y+DESK_TITLE,c->frame.w-2*DESK_BORDER,c->collapsed ? 0 : c->frame.h-DESK_TITLE-DESK_BORDER}; }
/* The frame plus its theme's drop shadow: the area a frame change repaints. */
struct desk_box desk_outer(const struct desk_client *c)
{
    int32_t s=desk_theme->shadow_size;
    if (!s) return c->frame;
    return (struct desk_box){c->frame.x-s+s/3,c->frame.y-s+s/2,c->frame.w+2*s,c->frame.h+2*s};
}
/* Title-bar hit boxes: theme.c desk_title_button_box, the same in every theme. */
static struct desk_box title_box(const struct desk_client *c, unsigned button)
{
    int b[4];
    desk_title_button_box(c->frame.x,c->frame.y,c->frame.w,button,b);
    return (struct desk_box){b[0],b[1],b[2],b[3]};
}
struct desk_box desk_close_box(const struct desk_client *c) { return title_box(c,DESK_TB_CLOSE); }
struct desk_box desk_min_box(const struct desk_client *c) { return title_box(c,DESK_TB_MINIMIZE); }
/* Zoom button, left of minimise (the old function and theme flag names are retained). */
struct desk_box desk_shade_box(const struct desk_client *c) { return title_box(c,DESK_TB_COLLAPSE); }
/* The title button (DESK_TB_*) of the topmost window under the pointer, as
 * index*4+k+1 (k: 0 close, 1 minimise, 2 zoom), or 0: hover tracking. */
static int inside(struct desk_box b, int32_t x, int32_t y)
{ return x>=b.x && y>=b.y && (int64_t)x<(int64_t)b.x+b.w && (int64_t)y<(int64_t)b.y+b.h; }
int desk_title_code(const struct desktop *d, int32_t x, int32_t y)
{
    /* desk_hit's rule (topmost visible window), kept here: the toolkit
     * programs link this file without input.c. */
    int i=-1;
    for (int k=0; k<DESK_CLIENTS; k++) if (d->clients[k].window && !d->clients[k].minimized &&
        inside(d->clients[k].frame,x,y) && (i<0 || d->clients[k].order>d->clients[i].order)) i=k;
    if (i<0) return 0;
    for (int k=0; k<3; k++) {
        if (k && !d->shell) break;
        struct desk_box b=title_box(&d->clients[i],1u<<k);
        if (b.w>0 && inside(b,x,y)) return i*4+k+1;
    }
    return 0;
}
struct desk_box desk_title_code_box(const struct desktop *d, int code)
{
    if (code<=0 || (code-1)/4>=DESK_CLIENTS || !d->clients[(code-1)/4].window) return (struct desk_box){0,0,0,0};
    return title_box(&d->clients[(code-1)/4],1u<<((code-1)%4));
}
void desk_collapse(struct desktop *d, int i)
{
    if (i<0 || i>=DESK_CLIENTS || !d->clients[i].window) return;
    struct desk_client *c=&d->clients[i];
    desk_damage_add(&d->damage,desk_outer(c),d->width,d->height);
    c->collapsed=!c->collapsed;
    if (c->collapsed) { c->shade_height = c->frame.h; c->frame.h = DESK_TITLE; }
    else c->frame.h = c->shade_height;
    desk_damage_add(&d->damage,desk_outer(c),d->width,d->height);
    if (d->shell) desk_damage_add(&d->damage,desk_taskbar(d),d->width,d->height);
}
struct desk_box desk_taskbar(const struct desktop *d)
{ return (struct desk_box){0,(int32_t)d->height-DESK_BAR,(int32_t)d->width,DESK_BAR}; }
struct desk_box desk_task_button(const struct desktop *d, int index)
{
    unsigned count=0, position=0;
    if (index<0 || index>=DESK_CLIENTS || !d->clients[index].window) return (struct desk_box){0,0,0,0};
    for (int i=0; i<DESK_CLIENTS; i++) if (d->clients[i].window) {
        if (i<index) position++;
        count++;
    }
    int32_t available=(int32_t)d->width-DESK_TASK_X-SH_TRAY_W-12;   /* left margin, tray */
    if (available<=0 || !count) return (struct desk_box){0,0,0,0};
    int32_t w=available/(int32_t)count;
    if (w>160) w=160;
    int32_t left=DESK_TASK_X+(int32_t)position*w;
    return (struct desk_box){left,(int32_t)d->height-DESK_BAR+3,w,26};
}
int32_t desk_menu_row(const struct desktop *d)
{ (void)d; return SH_START_ROW; }
/* The Ciuki (Start) button at the left end of the top bar. */
struct desk_box desk_start_button(const struct desktop *d)
{ (void)d; return (struct desk_box){DESK_START_X,DESK_START_Y,DESK_START_W,DESK_START_H}; }
/* The Start menu's box (open or not): shell.c drops it from the start
 * button, under the top bar, kept above the taskbar on short screens. */
struct desk_box desk_menu_box(const struct desktop *d)
{
    if (d->sh.levels && d->sh.menus[0].kind == SH_MENU_START) return d->sh.menus[0].box;
    int32_t h = 6 + 6 * SH_START_ROW + SH_SEPARATOR, y = DESK_BAR, limit = (int32_t)d->height - DESK_BAR;
    if (y + h > limit) y = limit - h;
    return (struct desk_box){DESK_START_X, y < 0 ? 0 : y, 184, h};
}
static struct desk_box unite(struct desk_box a, struct desk_box b)
{
    int32_t x = a.x < b.x ? a.x : b.x, y = a.y < b.y ? a.y : b.y;
    int32_t r = a.x+a.w > b.x+b.w ? a.x+a.w : b.x+b.w;
    int32_t t = a.y+a.h > b.y+b.h ? a.y+a.h : b.y+b.h;
    return (struct desk_box){x,y,r-x,t-y};
}
void desk_damage_add(struct desk_damage *d, struct desk_box b, uint32_t w, uint32_t h)
{
    if (!desk_intersect(b, (struct desk_box){0,0,(int32_t)w,(int32_t)h}, &b)) return;
    for (unsigned i = 0; i < d->count;) {
        struct desk_box a = d->boxes[i], overlap, bound=unite(a,b);
        uint64_t area=(uint64_t)a.w*a.h+(uint64_t)b.w*b.h;
        if (desk_intersect(a,b,&overlap)) area-=(uint64_t)overlap.w*overlap.h;
        /* Merge only rectangular unions. A diagonal drag or a narrow menu
         * touching a wide bar must not fill the clean corners of its bounds. */
        if ((uint64_t)bound.w*bound.h==area) {
            b = bound; d->boxes[i] = d->boxes[--d->count]; i = 0;
        } else i++;
    }
    struct desk_box pending[DESK_DAMAGE_MAX], next[DESK_DAMAGE_MAX];
    unsigned count=1; pending[0]=b;
    for (unsigned i=0;i<d->count && count;i++) {
        unsigned used=0;
        for (unsigned k=0;k<count;k++) {
            struct desk_box a=pending[k], cut;
            if (!desk_intersect(a,d->boxes[i],&cut)) {
                if (used==DESK_DAMAGE_MAX) goto saturated;
                next[used++]=a; continue;
            }
            struct desk_box parts[4]={{a.x,a.y,a.w,cut.y-a.y},
                {a.x,cut.y+cut.h,a.w,a.y+a.h-cut.y-cut.h},
                {a.x,cut.y,cut.x-a.x,cut.h},
                {cut.x+cut.w,cut.y,a.x+a.w-cut.x-cut.w,cut.h}};
            for (unsigned j=0;j<4;j++) if (parts[j].w>0 && parts[j].h>0) {
                if (used==DESK_DAMAGE_MAX) goto saturated;
                next[used++]=parts[j];
            }
        }
        count=used; memcpy(pending,next,count*sizeof(*pending));
    }
    if (d->count+count>DESK_DAMAGE_MAX) goto saturated;
    memcpy(d->boxes+d->count,pending,count*sizeof(*pending)); d->count+=count;
    return;
saturated:
    for (unsigned i=0;i<d->count;i++) b=unite(b,d->boxes[i]);
    d->count=1; d->boxes[0]=b;
}
static void fill(struct desktop *d, struct desk_box b, struct desk_box clip, uint32_t colour)
{
    if (!desk_intersect(b,clip,&b)) return;
    int stride=(int)d->width;
    uint32_t *first=d->pixels+(size_t)b.y*stride+b.x;
    for (int32_t y=0; y<b.h; y++) {
        uint32_t *row=first+(size_t)y*stride;
        if (y && b.w>=16) memcpy(row,first,(size_t)b.w*sizeof(*row));
        else for (int32_t x=0; x<b.w; x++) row[x]=colour;
    }
}
/* Original compact 5x7 bitmap glyphs; no third-party font dependency. */
static const uint8_t font[128][7] = {
    ['A']={14,17,17,31,17,17,17}, ['B']={30,17,17,30,17,17,30},
    ['C']={14,17,16,16,16,17,14}, ['D']={30,17,17,17,17,17,30},
    ['E']={31,16,16,30,16,16,31}, ['F']={31,16,16,30,16,16,16},
    ['G']={14,17,16,23,17,17,15}, ['H']={17,17,17,31,17,17,17},
    ['I']={14,4,4,4,4,4,14}, ['J']={7,2,2,2,18,18,12},
    ['K']={17,18,20,24,20,18,17}, ['L']={16,16,16,16,16,16,31},
    ['M']={17,27,21,21,17,17,17}, ['N']={17,25,21,19,17,17,17},
    ['O']={14,17,17,17,17,17,14}, ['P']={30,17,17,30,16,16,16},
    ['Q']={14,17,17,17,21,18,13}, ['R']={30,17,17,30,20,18,17},
    ['S']={15,16,16,14,1,1,30}, ['T']={31,4,4,4,4,4,4},
    ['U']={17,17,17,17,17,17,14}, ['V']={17,17,17,17,17,10,4},
    ['W']={17,17,17,21,21,21,10}, ['X']={17,17,10,4,10,17,17},
    ['Y']={17,17,10,4,4,4,4}, ['Z']={31,1,2,4,8,16,31},
    ['a']={0,0,14,1,15,17,15}, ['b']={16,16,30,17,17,17,30},
    ['c']={0,0,14,17,16,17,14}, ['d']={1,1,15,17,17,17,15},
    ['e']={0,0,14,17,31,16,14}, ['f']={6,8,8,30,8,8,8},
    ['g']={0,15,17,17,15,1,14}, ['h']={16,16,30,17,17,17,17},
    ['i']={4,0,12,4,4,4,14}, ['j']={2,0,6,2,2,18,12},
    ['k']={16,16,18,20,24,20,18}, ['l']={12,4,4,4,4,4,14},
    ['m']={0,0,26,21,21,21,21}, ['n']={0,0,30,17,17,17,17},
    ['o']={0,0,14,17,17,17,14}, ['p']={0,30,17,17,30,16,16},
    ['q']={0,15,17,17,15,1,1}, ['r']={0,0,22,25,16,16,16},
    ['s']={0,0,15,16,14,1,30}, ['t']={8,8,30,8,8,9,6},
    ['u']={0,0,17,17,17,19,13}, ['v']={0,0,17,17,17,10,4},
    ['w']={0,0,17,17,21,21,10}, ['x']={0,0,17,10,4,10,17},
    ['y']={0,17,17,17,15,1,14}, ['z']={0,0,31,2,4,8,31},
    ['0']={14,17,19,21,25,17,14}, ['1']={4,12,4,4,4,4,14},
    ['2']={14,17,1,2,4,8,31}, ['3']={30,1,1,14,1,1,30},
    ['4']={2,6,10,18,31,2,2}, ['5']={31,16,16,30,1,1,30},
    ['6']={14,16,16,30,17,17,14}, ['7']={31,1,2,4,8,8,8},
    ['8']={14,17,17,14,17,17,14}, ['9']={14,17,17,15,1,1,14},
    ['!']={4,4,4,4,4,0,4}, ['?']={14,17,1,2,4,0,4},
    ['.']={0,0,0,0,0,0,4}, [',']={0,0,0,0,0,4,8},
    [':']={0,4,4,0,4,4,0}, [';']={0,4,4,0,4,4,8},
    ['-']={0,0,0,31,0,0,0}, ['_']={0,0,0,0,0,0,31},
    ['/']={1,2,2,4,8,8,16}, ['\\']={16,8,8,4,2,2,1},
    ['(']={2,4,8,8,8,4,2}, [')']={8,4,2,2,2,4,8},
    ['[']={14,8,8,8,8,8,14}, [']']={14,2,2,2,2,2,14},
    ['+']={0,4,4,31,4,4,0}, ['=']={0,0,31,0,31,0,0},
    ['<']={2,4,8,16,8,4,2}, ['>']={8,4,2,1,2,4,8},
    ['"']={10,10,10,0,0,0,0}, ['\'']={4,4,4,0,0,0,0},
    ['#']={10,31,10,10,31,10,0}, ['%']={17,2,4,8,17,0,0},
    ['&']={12,18,20,8,21,18,13}, ['*']={0,21,14,31,14,21,0},
    ['@']={14,17,23,21,23,16,14}, ['$']={4,15,20,14,5,30,4},
    ['^']={4,10,17,0,0,0,0}, ['`']={8,4,0,0,0,0,0},
    ['{']={2,4,4,8,4,4,2}, ['}']={8,4,4,2,4,4,8},
    ['|']={4,4,4,4,4,4,4}, ['~']={0,0,9,22,0,0,0}
};
uint8_t desk_glyph_row(unsigned c, unsigned row)
{ return row < 7 ? font[c < 128 ? c : '?'][row] : 0; }
static struct desk_paint paint_of(struct desktop *d, struct desk_box clip)
{ return (struct desk_paint){d->pixels,(int)d->width,(int)d->height,clip.x,clip.y,clip.w,clip.h}; }
struct shadow_weight { uint32_t rb, g, inverse; };
static void shadow_span(uint32_t *dst, int n, const struct shadow_weight *weight)
{
    /* Two 16-bit lanes for red/blue. Every lane stays <= 65152, so no
     * carry crosses lanes. For 0<=v<=65152, (v+1+(v>>8))>>8 == v/255;
     * this preserves desk_mix's +127 rounding without integer division. */
    unsigned inv=weight->inverse;
    uint32_t fg_rb=weight->rb, fg_g=weight->g;
    for (int x=0; x<n; x++) {
        uint32_t old=dst[x], rb=(old&0xff00ffu)*inv+fg_rb;
        uint32_t g=((old>>8)&255u)*inv+fg_g;
        rb=(rb+0x00010001u+((rb>>8)&0xff00ffu))>>8;
        g=(g+1+(g>>8))>>8;
        dst[x]=(rb&0xff00ffu)|(g<<8);
    }
}
/* Shared with du_shadow (draw.c). Keep the theme's exact integer falloff,
 * but calculate dy once per row and alpha once per distance pair. The
 * opaque box is removed from the row before any blending or division. */
void desk_shadow_rows(struct desk_paint *p, int x, int y, int w, int h, int size, uint32_t colour)
{
    enum { MAX_SIZE=16 };
    /* Windows and menus have different radii. Two 3 KiB tables retain both
     * falloffs; foreground products are constant for each distance pair. */
    static struct {
        int size; uint32_t colour;
        struct shadow_weight weights[MAX_SIZE][MAX_SIZE];
    } cache[2];
    static unsigned victim;
    if (size<=0) return;
    if (size>MAX_SIZE) { desk_paint_shadow(p,x,y,w,h,size,colour); return; }
    int ox=x+size/3, oy=y+size/2;
    struct desk_box b, clip={p->cx,p->cy,p->cw,p->ch};
    if (!desk_intersect(clip,(struct desk_box){0,0,p->width,p->height},&clip) ||
        !desk_intersect((struct desk_box){ox-size,oy-size,w+2*size,h+2*size},clip,&b)) return;
    unsigned slot=0;
    while (slot<2 && (cache[slot].size!=size || cache[slot].colour!=colour)) slot++;
    if (slot==2) {
        slot=victim++%2; cache[slot].size=size; cache[slot].colour=colour;
        for (int dy=0; dy<size; dy++) for (int dx=0; dx<size; dx++) {
            unsigned a=72u*(unsigned)(size-dx)*(unsigned)(size-dy)/(unsigned)(size*size);
            a=a*a/72u;
            cache[slot].weights[dy][dx]=(struct shadow_weight){
                (colour&0xff00ffu)*a+0x007f007fu, ((colour>>8)&255u)*a+127u, 255-a};
        }
    }
    uint32_t *first_row=p->pixels+(size_t)b.y*p->width+b.x;
    int stride=p->width;
    for (int yy=b.y; yy<b.y+b.h; yy++) {
        uint32_t *row=first_row+(size_t)(yy-b.y)*stride;
        int dy=yy<oy ? oy-yy : yy>=oy+h ? yy-(oy+h-1) : 0;
        if (dy>=size) continue;
        int ends[4]={b.x,b.x+b.w,b.x+b.w,b.x+b.w};
        if (yy>=y && yy<y+h) {
            ends[1]=x<b.x ? b.x : x>b.x+b.w ? b.x+b.w : x;
            ends[2]=x+w<b.x ? b.x : x+w>b.x+b.w ? b.x+b.w : x+w;
        }
        for (int span=0; span<2; span++) {
            int first=ends[span*2], last=ends[span*2+1];
            for (int xx=first; xx<last;) {
                int dx=xx<ox ? ox-xx : xx>=ox+w ? xx-(ox+w-1) : 0;
                if (dx>=size) { xx++; continue; }
                int n=!dx ? (last<ox+w ? last : ox+w)-xx : 1;
                /* Alpha zero still clears X, matching the shared painter. */
                shadow_span(row+xx-b.x,n,&cache[slot].weights[dy][dx]);
                xx+=n;
            }
        }
    }
}
/* Legacy entry (scale 1: UI face, scale 2: large face); y is the top of the
 * old 7-row cell, kept so every caller's vertical placement holds. */
void desk_text(struct desktop *d, struct desk_box clip, int32_t x, int32_t y,
                 const char *s, int scale, uint32_t colour)
{
    struct desk_paint p=paint_of(d,clip);
    if (scale>1) desk_paint_text(&p,desk_theme->font_large,x,y+15,s,colour);
    else desk_paint_text(&p,desk_theme->font_ui,x,y+8,s,colour);
}
static void portrait_at(struct desktop *d, struct desk_box clip, int32_t x, int32_t y, int32_t size);
static void portrait(struct desktop *d, struct desk_box clip)
{
    if (!d->portrait) return;
    int32_t x0 = ((int32_t)d->width-DESK_PORTRAIT_SIZE)/2;
    int32_t y0 = DESK_BAR + ((int32_t)d->height-DESK_BAR-DESK_PORTRAIT_SIZE-36)/2;
    if (y0 < DESK_BAR) y0 = DESK_BAR;
    struct desk_box b;
    /* The approved portrait is shown unchanged; its converted background is
     * the Platinum desktop colour, so other themes blend its edge pixels. */
    if (desk_intersect((struct desk_box){x0,y0,DESK_PORTRAIT_SIZE,DESK_PORTRAIT_SIZE},clip,&b)) {
        uint32_t *first_dst=d->pixels+(size_t)b.y*d->width+b.x;
        const uint32_t *first_src=d->portrait+(b.y-y0)*DESK_PORTRAIT_SIZE+b.x-x0;
        int stride=(int)d->width;
        uint32_t background=desk_theme->desktop;
        for (int32_t y=0; y<b.h; y++) {
            uint32_t *dst=first_dst+(size_t)y*stride;
            const uint32_t *src=first_src+(size_t)y*DESK_PORTRAIT_SIZE;
            for (int32_t x=0; x<b.w; x++) {
                uint32_t v=src[x];
                dst[x]=v==DESK_BACKGROUND ? background : v;
            }
        }
    }
    struct desk_paint p=paint_of(d,clip);
    int font=desk_theme->font_large, w=desk_text_measure(font,"CiukiOS");
    desk_paint_text(&p,font,((int32_t)d->width-w)/2,y0+DESK_PORTRAIT_SIZE+28,"CiukiOS",desk_theme->desktop_label);
}
static void portrait_at(struct desktop *d, struct desk_box clip, int32_t x, int32_t y, int32_t size)
{
    struct desk_box b;
    if (!d->portrait || size<=0 || !desk_intersect((struct desk_box){x,y,size,size},clip,&b)) return;
    uint32_t *first_dst=d->pixels+(size_t)b.y*d->width+b.x;
    int stride=(int)d->width;
    for (int32_t yy=b.y; yy<b.y+b.h; yy++) {
        uint32_t *dst=first_dst+(size_t)(yy-b.y)*stride;
        const uint32_t *src=d->portrait+((yy-y)*DESK_PORTRAIT_SIZE/size)*DESK_PORTRAIT_SIZE;
        for (int32_t xx=0; xx<b.w; xx++) dst[xx]=src[(b.x+xx-x)*DESK_PORTRAIT_SIZE/size];
    }
}
/* Frames are opaque away from their rounded corners. Keep four bounded
 * caption/edge rasters from the shared theme painter and copy those spans on
 * moves/exposure. Corners still blend over the current scene. Nothing in
 * theme.c is duplicated here; title, icon bytes and every style input key
 * the cache. Small/unusual frames and allocation failure use the painter. */
enum { CAPTION_CACHES=4, CAPTION_MAX_WIDTH=2048 };
static struct caption_cache {
    uint32_t *pixels;
    const struct desk_theme *theme;
    int w, h;
    struct desk_frame_style style;
    char title[DESK_TITLE_MAX+1];
    uint32_t icon[16*16];
} captions[CAPTION_CACHES];

static void xrgb_row(uint32_t *restrict dst, const uint32_t *restrict src, int w)
{
    typedef uint64_t unaligned_pair __attribute__((aligned(1),may_alias));
    int x=0;
    for (; x+8<=w; x+=8) {
        /* Rows need only four-byte alignment. Integer pairs keep the exact
         * zero X byte without SSE/MMX; i686 splits them into ordinary words. */
        for (int k=0;k<8;k+=2)
            *(unaligned_pair *)(dst+x+k)=*(const unaligned_pair *)(src+x+k)&0x00ffffff00ffffffull;
    }
    for (; x<w; x++) dst[x]=src[x]&0xffffffu;
}

static void frame_border(struct desk_paint *p, int index, struct desk_box f,
                         const struct desk_frame_style *s)
{
    int pad=desk_theme->radius+1;
    if (pad<1) pad=1;
    if (f.w<=2*pad || f.w>CAPTION_MAX_WIDTH || f.h<=DESK_TITLE+3 || f.h>CAPTION_MAX_WIDTH) {
        desk_paint_frame_border(p,f.x,f.y,f.w,f.h,s); return;
    }
    struct caption_cache *c=&captions[index%CAPTION_CACHES];
    const char *title=s->title ? s->title : "";
    int same=c->pixels && c->theme==desk_theme && c->w==f.w && c->h==f.h &&
        c->style.active==s->active && c->style.alert==s->alert && c->style.collapsed==s->collapsed &&
        c->style.buttons==s->buttons && c->style.hot==s->hot && c->style.pressed==s->pressed &&
        !strcmp(c->title,title) && !!c->style.icon==!!s->icon &&
        (!s->icon || !memcmp(c->icon,s->icon,sizeof(c->icon)));
    if (!same) {
        if (!c->pixels || c->w!=f.w || c->h!=f.h) {
            size_t count=(size_t)f.w*(DESK_TITLE+DESK_BORDER)+2u*DESK_BORDER*f.h;
            uint32_t *next=malloc(count*sizeof(*next));
            if (!next) { desk_paint_frame_border(p,f.x,f.y,f.w,f.h,s); return; }
            free(c->pixels); c->pixels=next;
        }
        c->w=f.w; c->h=f.h; c->theme=desk_theme; c->style=*s;
        snprintf(c->title,sizeof(c->title),"%s",title);
        if (s->icon) memcpy(c->icon,s->icon,sizeof(c->icon));
        c->style.title=c->title; c->style.icon=s->icon ? c->icon : NULL;
        /* The face covers every cached pixel before text/button blending.
         * Exclude all corner pixels, whose result depends on the underlay. */
        struct desk_paint raster={c->pixels,f.w,DESK_TITLE,0,0,f.w,DESK_TITLE};
        memset(c->pixels,0,(size_t)f.w*DESK_TITLE*sizeof(*c->pixels));
        desk_paint_frame_border(&raster,0,0,f.w,f.h,s);
        /* Compact side/bottom strips, rather than a whole window raster. */
        uint32_t *left=c->pixels+(size_t)f.w*DESK_TITLE;
        uint32_t *right=left+(size_t)DESK_BORDER*f.h;
        uint32_t *bottom=right+(size_t)DESK_BORDER*f.h;
        raster=(struct desk_paint){left,DESK_BORDER,f.h,0,DESK_TITLE,DESK_BORDER,f.h-DESK_TITLE};
        desk_paint_frame_border(&raster,0,0,f.w,f.h,s);
        raster.pixels=right;
        desk_paint_frame_border(&raster,DESK_BORDER-f.w,0,f.w,f.h,s);
        raster=(struct desk_paint){bottom,f.w,DESK_BORDER,0,0,f.w,DESK_BORDER};
        desk_paint_frame_border(&raster,0,DESK_BORDER-f.h,f.w,f.h,s);
    }
    /* Only the tiny rounded corners depend on the current underlay. A
     * short frame gives the identical top corner without walking body rows. */
    struct desk_box strips[2]={{f.x,f.y,pad,desk_theme->radius},
        {f.x+f.w-pad,f.y,pad,desk_theme->radius}};
    struct desk_box clip={p->cx,p->cy,p->cw,p->ch}, b;
    for (unsigned k=0; k<2; k++) if (desk_intersect(strips[k],clip,&b)) {
        struct desk_paint edge=*p;
        edge.cx=b.x; edge.cy=b.y; edge.cw=b.w; edge.ch=b.h;
        desk_paint_frame_border(&edge,f.x,f.y,f.w,DESK_TITLE+4,s);
    }
    for (int y=0; y<DESK_TITLE; y++) {
        int inset=y<desk_theme->radius ? pad : 0;
        if (desk_intersect((struct desk_box){f.x+inset,f.y+y,f.w-2*inset,1},clip,&b))
            memcpy(p->pixels+(size_t)b.y*p->width+b.x,
                   c->pixels+(size_t)y*f.w+b.x-f.x,(size_t)b.w*sizeof(*c->pixels));
    }
    struct desk_box body[3]={{f.x,f.y+DESK_TITLE,DESK_BORDER,f.h-DESK_TITLE},
        {f.x+f.w-DESK_BORDER,f.y+DESK_TITLE,DESK_BORDER,f.h-DESK_TITLE},
        {f.x,f.y+f.h-DESK_BORDER,f.w,DESK_BORDER}};
    const uint32_t *srcs[3]={c->pixels+(size_t)f.w*DESK_TITLE,
        c->pixels+(size_t)f.w*DESK_TITLE+(size_t)DESK_BORDER*f.h,
        c->pixels+(size_t)f.w*DESK_TITLE+2u*DESK_BORDER*f.h};
    for (unsigned k=0; k<3; k++) if (desk_intersect(body[k],clip,&b)) {
        int stride=k==2 ? f.w : DESK_BORDER;
        int sy=k==2 ? b.y-body[k].y : b.y-f.y;
        const uint32_t *first_src=srcs[k]+(size_t)sy*stride+b.x-body[k].x;
        uint32_t *first_dst=p->pixels+(size_t)b.y*p->width+b.x;
        int dst_stride=p->width;
        for (int y=0; y<b.h; y++) {
            uint32_t *dst=first_dst+(size_t)y*dst_stride;
            const uint32_t *src=first_src+(size_t)y*stride;
            if (k==2) memcpy(dst,src,(size_t)b.w*sizeof(*dst));
            else for (int x=0; x<b.w; x++) dst[x]=src[x];
        }
    }
}
static void window(struct desktop *d, int i, struct desk_box clip)
{
    /* Every window (programs, the DOS window, the built-in About/Run/Shut
     * Down/message windows) goes through theme.c desk_paint_frame, the
     * painter libdeskui's dialogs use as well. */
    struct desk_client *c = &d->clients[i];
    const struct desk_theme *t = desk_theme;
    struct desk_box outer=desk_outer(c), area, f=c->frame;
    if (!desk_intersect(outer,clip,&area)) return;
    struct desk_paint p=paint_of(d,area);
    if (t->shadow_size) DESK_PROFILE("shadows", desk_shadow_rows(&p,f.x,f.y,f.w,f.h,t->shadow_size,t->drop_shadow));
    if (!desk_intersect(f,clip,&clip)) return;
    p=paint_of(d,clip);
    /* Hover and pressed title buttons follow the pointer (input.c tracks
     * the damage): the button under it, held while the left button is down. */
    int code=desk_title_code(d,d->mouse_x,d->mouse_y);
    unsigned hot=code && (code-1)/4==i ? 1u<<((code-1)%4) : 0;
    struct desk_frame_style s={
        .title=c->closing ? "Closing..." : c->unresponsive ? "Not responding" : c->title,
        .icon=desk_shell_window_icon ? desk_shell_window_icon(d,i,16) : NULL,
        .active=i==d->focus && !c->unresponsive, .alert=c->unresponsive, .collapsed=c->collapsed,
        .buttons=d->shell ? DESK_TB_CLOSE|DESK_TB_MINIMIZE|DESK_TB_COLLAPSE : DESK_TB_CLOSE,
        .hot=hot, .pressed=hot && code==d->title_down && (d->buttons&1u) ? hot : 0};
    DESK_PROFILE("captions", {
        if (c->internal || c->collapsed) desk_paint_frame(&p,f.x,f.y,f.w,f.h,&s);
        else frame_border(&p,i,f,&s);
    });
    struct desk_box content = desk_content(c), b;
    if (c->collapsed) return;
    if (c->internal) {
        if (desk_internal_paint && desk_intersect(content,clip,&b)) desk_internal_paint(d,i,content,b);
        return;
    }
    /* While resizing, clip the previous client surface to the new content
     * well and clear only the newly exposed space. */
    struct desk_box old = {content.x,content.y,(int32_t)c->info.width,(int32_t)c->info.height};
    fill(d,(struct desk_box){content.x+old.w,content.y,content.w-old.w,content.h},clip,t->face);
    fill(d,(struct desk_box){content.x,content.y+old.h,content.w,content.h-old.h},clip,t->face);
    if (desk_intersect(content,old,&b) && desk_intersect(b,clip,&b)) {
        uint32_t *first_dst=d->pixels+(size_t)b.y*d->width+b.x;
        const uint32_t *first_src=c->pixels+(size_t)(b.y-content.y)*c->info.width+b.x-content.x;
        int dst_stride=(int)d->width, src_stride=(int)c->info.width;
        for (int32_t y=0; y<b.h; y++) {
            uint32_t *dst=first_dst+(size_t)y*dst_stride;
            const uint32_t *src=first_src+(size_t)y*src_stride;
            /* The surface is XRGB: preserve the historical zero X byte,
             * even when a client stores arbitrary padding there. */
            xrgb_row(dst,src,b.w);
        }
    }
    if (c->resizable && !c->zoomed) {
        /* A visible grip in the 3 px bottom/right frame, outside app pixels. */
        for (int k=0; k<3; k++) {
            int gx=f.x+f.w-5-k*4, gy=f.y+f.h-5-k*4;
            if (gx >= f.x+DESK_BORDER)
                desk_paint_fill(&p,gx,f.y+f.h-3,2,1,t->dark);
            if (gy >= f.y+DESK_TITLE)
                desk_paint_fill(&p,f.x+f.w-3,gy,1,2,t->dark);
        }
    }
}
/* Programs that open a desktop window receive their channel at fd 3. */
int desk_program_graphical(const char *name)
{ return !strcmp(name,"demo") || !strcmp(name,"dosbox") || !strcmp(name,"files") || !strcmp(name,"tasks") || !strcmp(name,"ciuknote") ||
         !strcmp(name,"control") || !strcmp(name,"ciukide") || !strcmp(name,"ciukide-app") || desk_catalog_find(name); }
const char *desk_program_label(const char *name)
{
    const struct desk_catalog_entry *e=desk_catalog_find(name);
    return !strcmp(name,"demo") ? "Ciuki Demo" : !strcmp(name,"files") ? "Files" : !strcmp(name,"tasks") ? "Task Manager" :
           !strcmp(name,"ciuknote") ? "CiukNote" : !strcmp(name,"ciukide") ? "CiukIDE" : !strcmp(name,"control") ? "Control Panel" : e ? e->label : name;
}
int desk_catalog_takes_file(const char *name)
{ const struct desk_catalog_entry *e=desk_catalog_find(name); return e && e->takes_file; }
const char *desk_app_program(int kind) { return kind==DESK_APP_NOTE ? "ciuknote" : "files"; }
int desk_program_kind(const char *name)
{ return !strcmp(name,"files") ? DESK_APP_FILES : !strcmp(name,"ciuknote") ? DESK_APP_NOTE : !strcmp(name,"control") ? DESK_APP_CONTROL :
         !strcmp(name,"ciukide") ? DESK_APP_IDE : 0; }
void desk_shell_draw(struct desktop *d, struct desk_box clip)
{
    /* Taskbar in the current theme: flat face with a light top edge; the
     * shell (shell.c) adds the task buttons, the tray, menus and popups.
     * The Ciuki (Start) button lives in the top bar (desk_shell_topbar). */
    const struct desk_theme *t=desk_theme;
    struct desk_box bar=desk_taskbar(d);
    struct desk_paint p=paint_of(d,clip);
    fill(d,bar,clip,t->taskbar);
    desk_paint_fill(&p,bar.x,bar.y,bar.w,1,t->light);
    desk_paint_fill(&p,bar.x,bar.y+1,bar.w,1,t->hilite);
    if (desk_shell_overlay) desk_shell_overlay(d,clip);
}
/* Arrow pointer, 11x17: '#' outline, 'o' fill. */
static const char *const arrow[17] = {
    "#..........", "##.........", "#o#........", "#oo#.......", "#ooo#......", "#oooo#.....",
    "#ooooo#....", "#oooooo#...", "#ooooooo#..", "#oooooooo#.", "#ooooo#####", "#oo#oo#....",
    "#o#.#oo#...", "##..#oo#...", "#....#oo#..", ".....#oo#..", "......##..."};
/* Remove only client wells known to be opaque. Frames, corners, shadows,
 * internal windows and exposed space while resizing are never occluders.
 * Bound subdivision to the existing damage capacity and fall back to the
 * original clip if the scene is unusually fragmented. */
static unsigned visible_boxes(struct desktop *d, struct desk_box clip,
                              const int *windows, unsigned count, struct desk_box *boxes)
{
    struct desk_box next[DESK_DAMAGE_MAX];
    unsigned n=1; boxes[0]=clip;
    for (unsigned k=0; k<count && n; k++) {
        struct desk_client *c=&d->clients[windows[k]];
        if (c->internal || c->collapsed || !c->pixels) continue;
        struct desk_box well=desk_content(c), opaque;
        if (!desk_intersect(well,(struct desk_box){well.x,well.y,(int32_t)c->info.width,(int32_t)c->info.height},&opaque)) continue;
        unsigned used=0;
        for (unsigned i=0; i<n; i++) {
            struct desk_box a=boxes[i], cut;
            if (!desk_intersect(a,opaque,&cut)) {
                if (used==DESK_DAMAGE_MAX) { boxes[0]=clip; return 1; }
                next[used++]=a; continue;
            }
            struct desk_box parts[4]={{a.x,a.y,a.w,cut.y-a.y},
                {a.x,cut.y+cut.h,a.w,a.y+a.h-cut.y-cut.h},
                {a.x,cut.y,cut.x-a.x,cut.h},
                {cut.x+cut.w,cut.y,a.x+a.w-cut.x-cut.w,cut.h}};
            for (unsigned j=0; j<4; j++) if (parts[j].w>0 && parts[j].h>0) {
                if (used==DESK_DAMAGE_MAX) { boxes[0]=clip; return 1; }
                next[used++]=parts[j];
            }
        }
        n=used; memcpy(boxes,next,n*sizeof(*boxes));
    }
    return n;
}
static void scene(struct desktop *d, struct desk_box clip)
{
    if (!desk_intersect(clip,(struct desk_box){0,0,(int32_t)d->width,(int32_t)d->height},&clip)) return;
    const struct desk_theme *t=desk_theme;
    struct desk_paint p=paint_of(d,clip);
    int windows[DESK_CLIENTS]; unsigned count=0;
    uint64_t last=0;
    for (unsigned n=0; n<DESK_CLIENTS; n++) {
        int next=-1;
        for (int i=0; i<DESK_CLIENTS; i++) if (d->clients[i].window && !d->clients[i].minimized && d->clients[i].order>last &&
            (next<0 || d->clients[i].order<d->clients[next].order)) next=i;
        if (next<0) break;
        last=d->clients[next].order; windows[count++]=next;
    }
    struct desk_box boxes[DESK_DAMAGE_MAX];
    unsigned visible=visible_boxes(d,clip,windows,count,boxes);
    if (d->shell && desk_shell_background) {
        /* Also retain shell binding/cache maintenance when a clip is fully
         * covered, without copying any background pixels into the scene. */
        DESK_PROFILE("background_rows", {
            if (!visible) desk_shell_background(d,(struct desk_box){0,0,0,0});
            for (unsigned k=0; k<visible; k++) desk_shell_background(d,boxes[k]);
        });
    } else for (unsigned k=0; k<visible; k++) {
        fill(d,boxes[k],boxes[k],t->desktop); portrait(d,boxes[k]);
    }
    /* Top bar: a Platinum-style menu bar with the Ciuki mark. */
    fill(d,(struct desk_box){0,0,(int32_t)d->width,DESK_BAR},clip,t->topbar);
    desk_paint_fill(&p,0,DESK_BAR-1,(int)d->width,1,t->topbar_edge);
    desk_paint_fill(&p,0,0,(int)d->width,1,desk_mix(0xffffffu,t->topbar,160));
    if (d->shell && desk_shell_topbar) DESK_PROFILE("bars_menus", desk_shell_topbar(d,clip));
    else {
        portrait_at(d,clip,8,6,20);
        desk_paint_text(&p,t->font_bold,34,20,"CiukiOS",t->topbar_text);   /* no tagline in the top bar (owner, 2026-10-12) */
    }
    char clock[24];
    uint64_t s = d->now_ns / 1000000000ull;
    snprintf(clock,sizeof(clock),"%02u:%02u:%02u",(unsigned)(s/3600%100),(unsigned)(s/60%60),(unsigned)(s%60));
    if (!d->shell) desk_paint_text(&p,t->font_ui,(int)d->width-12-desk_text_measure(t->font_ui,clock),20,clock,t->topbar_text);
    for (unsigned n=0; n<count; n++) {
        visible=visible_boxes(d,clip,windows+n+1,count-n-1,boxes);
        for (unsigned k=0; k<visible; k++) DESK_PROFILE("windows", window(d,windows[n],boxes[k]));
    }
    if (d->shell) DESK_PROFILE("bars_menus", desk_shell_draw(d,clip));
}
static void cursor(struct desktop *d, struct desk_box clip)
{
    struct desk_box b;
    if (d->hw_cursor || !desk_intersect((struct desk_box){d->mouse_x,d->mouse_y,11,17},clip,&b)) return;
    uint32_t *first_dst=d->pixels+(size_t)b.y*d->width+b.x;
    int stride=(int)d->width;
    for (int y=0; y<b.h; y++) {
        uint32_t *dst=first_dst+(size_t)y*stride;
        const char *src=arrow[b.y+y-d->mouse_y]+b.x-d->mouse_x;
        for (int x=0; x<b.w; x++) {
            char k=src[x];
            if (k!='.') dst[x]=k=='#' ? 0x101010u : 0xffffffu;
        }
    }
}
/* Reference/capture renderer: the runtime keeps this arrow out of the scene. */
void desk_composite(struct desktop *d, struct desk_box clip)
{
    if (!desk_intersect(clip,(struct desk_box){0,0,(int32_t)d->width,(int32_t)d->height},&clip)) return;
    scene(d,clip); DESK_PROFILE("cursor", cursor(d,clip));
}
void desk_render_release(struct desktop *d)
{
    for (unsigned i=0; i<CAPTION_CACHES; i++) { free(captions[i].pixels); captions[i]=(struct caption_cache){0}; }
    free(d->accel_front); d->accel_front=NULL; d->accel_front_valid=0;
    d->copy_pending=0;
    free(d->scene); d->scene=NULL; d->scene_bytes=0;
    free(d->sh.background); d->sh.background=NULL; d->sh.background_valid=0;
    d->cursor_valid=0; d->cursor_dirty=1; d->present_damage.count=0;
}
int desk_frame_due(const struct desktop *d, uint64_t now)
{ return now>=d->next_frame_ns && (d->damage.count || d->present_damage.count || d->cursor_dirty); }
static uint64_t time_now(struct desktop *d) { return d->time_ns ? d->time_ns() : 0; }
static void copy_box(uint32_t *dst, const uint32_t *src, uint32_t width, struct desk_box b)
{
    for (int y=b.y;y<b.y+b.h;y++)
        memcpy(dst+(size_t)y*width+b.x,src+(size_t)y*width+b.x,(size_t)b.w*4);
}
static int accel_call(struct desktop *d, struct ciuki_accel_request *q)
{
    q->size=sizeof(*q); q->generation=d->accel_generation;
    return d->accel ? d->accel(d->display,q) : -ENOSYS;
}
void desk_accel_reset(struct desktop *d, uint32_t generation)
{
    free(d->accel_front); d->accel_front=NULL; d->accel_front_valid=0;
    d->hw_cursor=0; d->cursor_dirty=1; d->copy_pending=0;
    d->accel_generation=generation;
    struct ciuki_accel_request q={.operation=CIUKI_ACCEL_QUERY};
    int caps=accel_call(d,&q);
    d->accel_caps=caps>0 ? (uint32_t)caps : 0;
    if (d->accel_caps&1u) d->accel_front=calloc((size_t)d->width*d->height,4);
    if (!d->accel_front) d->accel_caps&=~1u;
    if (d->accel_caps&2u) {
        q=(struct ciuki_accel_request){.operation=CIUKI_ACCEL_CURSOR_SET,.colour=0xffffff,.background=0x101010};
        for (unsigned y=0; y<17; y++) for (unsigned x=0; x<11; x++) {
            if (arrow[y][x]!='.') q.cursor_mask[y]|=0x80000000u>>x;
            if (arrow[y][x]=='o') q.cursor_image[y]|=0x80000000u>>x;
        }
        if (!accel_call(d,&q)) d->hw_cursor=1;
    }
    desk_damage_add(&d->damage,(struct desk_box){0,0,(int32_t)d->width,(int32_t)d->height},d->width,d->height);
}
void desk_cursor_motion(struct desktop *d)
{
    d->cursor_dirty=1;
}
static void ram_copy(struct desktop *d, const struct ciuki_rect *r)
{
    for (uint32_t n=0; n<r->height; n++) {
        uint32_t y=r->dst_y>r->src_y ? r->height-1-n : n;
        memmove(d->accel_front+((uint32_t)r->dst_y+y)*d->width+(uint32_t)r->dst_x,
                d->accel_front+((uint32_t)r->src_y+y)*d->width+(uint32_t)r->src_x,r->width*4);
    }
}
static int screen_copy(struct desktop *d, const struct ciuki_rect *r)
{
    struct ciuki_accel_request q={.operation=CIUKI_ACCEL_COPY,.rect=*r};
    if (accel_call(d,&q)) {
        d->accel_front_valid=0;
        q.operation=CIUKI_ACCEL_QUERY;
        int caps=accel_call(d,&q);
        d->accel_caps=caps>0 ? (uint32_t)caps : 0;
        if (!(d->accel_caps&2u)) d->hw_cursor=0;
        desk_damage_add(&d->damage,(struct desk_box){0,0,(int32_t)d->width,(int32_t)d->height},d->width,d->height);
        return -1;
    }
    ram_copy(d,r); d->presents++; d->frame_stats.presents++;
    return 0;
}
/* A scroll is accelerated only after comparing every translated client pixel
 * with the last completed RAM frame. Samples merely reject candidates early. */
static int scroll_copy(struct desktop *d)
{
    if (!(d->accel_caps&1u) || !d->hw_cursor || !d->accel_front_valid ||
        !d->damage.count || d->menu_open || d->sh.levels || d->sh.volume_open ||
        d->sh.switcher || d->sh.tooltip || d->power.state) return 0;
    for (unsigned i=0; i<DESK_CLIENTS; i++) {
        struct desk_client *c=&d->clients[i];
        struct desk_box b=desk_content(c), ignored;
        if (!c->window || c->internal || c->minimized || c->collapsed || !c->pixels ||
            b.w<64 || b.h<32 || b.x<0 || b.y<0 || b.x+b.w>(int32_t)d->width || b.y+b.h>(int32_t)d->height ||
            b.w!=(int32_t)c->info.width || b.h!=(int32_t)c->info.height) continue;
        int visible=1, damaged=0;
        for (unsigned j=0; j<DESK_CLIENTS; j++) if (j!=i && d->clients[j].window && !d->clients[j].minimized &&
            d->clients[j].order>c->order && desk_intersect(desk_outer(&d->clients[j]),b,&ignored)) visible=0;
        for (unsigned j=0; j<d->damage.count; j++) if (desk_intersect(d->damage.boxes[j],b,&ignored)) damaged=1;
        if (!visible || !damaged) continue;
        for (int delta=1; delta<=64 && delta<b.h/2; delta++) for (int dir=-1; dir<=1; dir+=2) {
            int sy=dir<0 ? delta : 0, dy=dir<0 ? 0 : delta, same=1, changed=0;
            const uint32_t *a=c->pixels+(uint32_t)dy*c->info.width;
            const uint32_t *old=d->accel_front+(uint32_t)(b.y+sy)*d->width+(uint32_t)b.x;
            if ((a[0]&0xffffffu)!=old[0] || (a[b.w/2]&0xffffffu)!=old[b.w/2] || (a[b.w-1]&0xffffffu)!=old[b.w-1]) continue;
            const uint32_t *at=d->accel_front+(uint32_t)(b.y+dy)*d->width+(uint32_t)b.x;
            if ((a[0]&0xffffffu)==at[0] && (a[b.w/2]&0xffffffu)==at[b.w/2] && (a[b.w-1]&0xffffffu)==at[b.w-1]) continue;
            for (int y=0; y<b.h-delta && same; y++) for (int x=0; x<b.w; x++) {
                uint32_t v=c->pixels[(uint32_t)(dy+y)*c->info.width+(uint32_t)x]&0xffffffu;
                if (v!=d->accel_front[(uint32_t)(b.y+sy+y)*d->width+(uint32_t)(b.x+x)]) { same=0; break; }
                if (v!=d->accel_front[(uint32_t)(b.y+dy+y)*d->width+(uint32_t)(b.x+x)]) changed=1;
            }
            if (same && changed) {
                struct ciuki_rect r={b.x,b.y+sy,b.x,b.y+dy,(uint32_t)b.w,(uint32_t)(b.h-delta)};
                return screen_copy(d,&r); /* one candidate per frame */
            }
        }
    }
    return 0;
}
static int present_box(struct desktop *d, struct desk_box b)
{
    int rectangles=d->accel && (d->accel_caps&1u) && d->accel_front;
    if (!rectangles || !d->accel_front_valid) {
        struct ciuki_rect r={b.x,b.y,b.x,b.y,(uint32_t)b.w,(uint32_t)b.h};
        if (d->ops.present(d->display,d->output,&r)) return -1;
        d->frame_stats.lfb_bytes+=(uint64_t)r.width*r.height*(d->lfb_bytes_per_pixel ? d->lfb_bytes_per_pixel : 4);
        if (rectangles) DESK_PROFILE("present_front_copy", {
            for (int32_t y=b.y; y<b.y+b.h; y++)
                memcpy(d->accel_front+(uint32_t)y*d->width+(uint32_t)b.x,d->pixels+(uint32_t)y*d->width+(uint32_t)b.x,(uint32_t)b.w*4);
        });
        if (!rectangles) d->accel_front_valid=0;
        else if (!b.x && !b.y && b.w==(int32_t)d->width && b.h==(int32_t)d->height) d->accel_front_valid=1;
        d->presents++; d->frame_stats.presents++; return 0;
    }
    for (int32_t y=b.y; y<b.y+b.h;) {
        uint32_t *row=d->pixels+(uint32_t)y*d->width, *front=d->accel_front+(uint32_t)y*d->width;
        int32_t left=b.x, right=b.x+b.w;
        while (left<right && row[left]==front[left]) left++;
        while (right>left && row[right-1]==front[right-1]) right--;
        if (left==right) { y++; continue; }
        int solid=right-left>=64 && (d->accel_caps&1u);
        for (int32_t x=left+1; solid && x<right; x++) if (row[x]!=row[left]) solid=0;
        int32_t bottom=y+1;
        while (bottom<b.y+b.h) {
            uint32_t *next=d->pixels+(uint32_t)bottom*d->width, *old=d->accel_front+(uint32_t)bottom*d->width;
            int32_t l=b.x,r=b.x+b.w;
            while (l<r && next[l]==old[l]) l++;
            while (r>l && next[r-1]==old[r-1]) r--;
            if (l!=left || r!=right) break;
            int next_solid=r-l>=64 && (d->accel_caps&1u);
            for (int32_t x=l+1; next_solid && x<r; x++) if (next[x]!=next[l]) next_solid=0;
            if (next_solid!=solid || (solid && next[left]!=row[left])) break;
            bottom++;
        }
        if (solid) {
            struct ciuki_accel_request q={.operation=CIUKI_ACCEL_FILL,.colour=row[left],
                .rect={left,y,left,y,(uint32_t)(right-left),(uint32_t)(bottom-y)}};
            if (accel_call(d,&q)) {
                struct ciuki_rect r={left,y,left,y,(uint32_t)(right-left),(uint32_t)(bottom-y)};
                if (d->ops.present(d->display,d->output,&r)) return -1;
                d->frame_stats.lfb_bytes+=(uint64_t)r.width*r.height*(d->lfb_bytes_per_pixel ? d->lfb_bytes_per_pixel : 4);
            }
        } else {
            struct ciuki_rect r={left,y,left,y,(uint32_t)(right-left),(uint32_t)(bottom-y)};
            if (d->ops.present(d->display,d->output,&r)) return -1;
            d->frame_stats.lfb_bytes+=(uint64_t)r.width*r.height*(d->lfb_bytes_per_pixel ? d->lfb_bytes_per_pixel : 4);
        }
        DESK_PROFILE("present_front_copy", {
            for (int32_t i=y;i<bottom;i++)
                memcpy(d->accel_front+(uint32_t)i*d->width+(uint32_t)left,
                       d->pixels+(uint32_t)i*d->width+(uint32_t)left,(uint32_t)(right-left)*4);
        });
        d->presents++; d->frame_stats.presents++; y=bottom;
    }
    return 0;
}
int desk_redraw(struct desktop *d)
{
    if (!d->damage.count && !d->present_damage.count && !d->cursor_dirty) return 0;
    size_t bytes=(size_t)d->width*d->height*4;
    if (!d->scene || bytes!=d->scene_bytes) {
        uint32_t *next=malloc(bytes);
        if (!next) { d->next_frame_ns=time_now(d)+DESK_FRAME_NS; errno=ENOMEM; return -1; }
        free(d->scene); d->scene=next; d->scene_bytes=bytes;
        d->cursor_valid=0;
        desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,(int)d->height},d->width,d->height);
    }
    struct desk_frame_stats *s=&d->frame_stats;
    memset(s,0,sizeof(*s)); s->frames=1;
    uint64_t begin=time_now(d);
    if (d->accel) {
        struct ciuki_accel_request q={.operation=CIUKI_ACCEL_QUERY};
        int caps=accel_call(d,&q);
        d->accel_caps=caps>0 ? (uint32_t)caps : 0;
        if (d->hw_cursor && !(d->accel_caps&2u)) {
            d->hw_cursor=0; d->cursor_dirty=1;
            desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,(int)d->height},d->width,d->height);
        }
    }
    s->present_ns+=time_now(d)-begin;
    begin=time_now(d);
    uint32_t *output=d->pixels;
    d->pixels=d->scene;
    for (unsigned i=0;i<d->damage.count;i++) {
        struct desk_box b=d->damage.boxes[i];
        scene(d,b); s->scene_px+=(uint64_t)b.w*b.h;
    }
    d->pixels=output;
    /* No intermediate scene is published. Restore the old arrow before any
     * completed scene copies, then save the final underlay (including overlaps). */
    if (d->cursor_valid) {
        struct desk_box b=d->cursor_box;
        for (int y=0;y<b.h;y++)
            memcpy(output+(size_t)(b.y+y)*d->width+b.x,d->cursor_under+y*11,(size_t)b.w*4);
        if (d->cursor_dirty)
            desk_damage_add(&d->present_damage,b,d->width,d->height);
    }
    for (unsigned i=0;i<d->damage.count;i++) {
        struct desk_box b=d->damage.boxes[i];
        copy_box(output,d->scene,d->width,b);
        desk_damage_add(&d->present_damage,b,d->width,d->height);
    }
    struct desk_box b;
    if (!d->hw_cursor && desk_intersect((struct desk_box){d->mouse_x,d->mouse_y,11,17},
                       (struct desk_box){0,0,(int)d->width,(int)d->height},&b)) {
        for (int y=0;y<b.h;y++)
            memcpy(d->cursor_under+y*11,output+(size_t)(b.y+y)*d->width+b.x,(size_t)b.w*4);
        d->cursor_box=b; d->cursor_valid=1; cursor(d,b);
        if (d->cursor_dirty) desk_damage_add(&d->present_damage,b,d->width,d->height);
    } else d->cursor_valid=0;
    if (!d->hw_cursor) d->cursor_dirty=0;
    s->composite_ns=time_now(d)-begin;
    /* Enter the presentation phase only with a completed RAM frame. A
     * partially failed copy invalidates scanout; reconstruct it next frame
     * from full scene damage before attempting any upload. */
    begin=time_now(d);
    int copy_error=0;
    if (d->copy_pending) {
        if (d->accel_front && d->accel_front_valid && (d->accel_caps&1u)) copy_error=screen_copy(d,&d->copy_rect);
        d->copy_pending=0;
    } else copy_error=scroll_copy(d);
    if (copy_error) d->cursor_dirty=1;
    for (unsigned i=0;i<d->present_damage.count;i++)
        s->damage_px+=(uint64_t)d->present_damage.boxes[i].w*d->present_damage.boxes[i].h;
    unsigned done = 0;
    for (; !copy_error && done < d->present_damage.count; done++) {
        struct desk_box b = d->present_damage.boxes[done];
        if (present_box(d,b)) break;
    }
    int complete=!copy_error && done==d->present_damage.count;
    if (done) {
        d->present_damage.count -= done;
        memmove(d->present_damage.boxes,d->present_damage.boxes+done,d->present_damage.count*sizeof(d->present_damage.boxes[0]));
        if (complete) for (int i=0; i<DESK_CLIENTS; i++) {
            struct desk_client *c=&d->clients[i];
            if (c->launch_frame_pending && c->window && !c->minimized && !c->collapsed) {
                c->launch_frame_pending=0;
                if (d->launch_mark) d->launch_mark(CIUKI_LAUNCH_FRAME,c->pid,0);
            }
        }
    }
    if (complete) d->damage.count=0;
    /* A failed engine operation may have hidden the hardware pointer.
     * Retain a full software recovery frame until it is successfully uploaded. */
    if (d->hw_cursor) {
        struct ciuki_accel_request q={.operation=CIUKI_ACCEL_QUERY};
        int caps=accel_call(d,&q);
        d->accel_caps=caps>0 ? (uint32_t)caps : 0;
        if (!(d->accel_caps&2u)) {
            d->hw_cursor=0; d->cursor_dirty=1;
            desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,(int)d->height},d->width,d->height);
            complete=0;
        }
    }
    if (d->hw_cursor && d->cursor_dirty && complete) {
        struct ciuki_accel_request q={.operation=CIUKI_ACCEL_CURSOR_MOVE,
            .rect={0,0,d->mouse_x,d->mouse_y,0,0}};
        int error=accel_call(d,&q);
        q.operation=CIUKI_ACCEL_CURSOR_SHOW; q.colour=1;
        if (!error) error=accel_call(d,&q);
        if (error) {
            d->hw_cursor=0;
            desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,(int)d->height},d->width,d->height);
            complete=0;
        } else { d->cursor_dirty=0; d->cursor_x=d->mouse_x; d->cursor_y=d->mouse_y; }
    }
    s->present_ns+=time_now(d)-begin;
    s->input_ns=d->pending_input_ns; s->input_lag_ns=d->pending_input_lag_ns;
    d->pending_input_ns=d->pending_input_lag_ns=0;
    d->stats.frames++; d->stats.damage_px+=s->damage_px; d->stats.scene_px+=s->scene_px;
    d->stats.presents+=s->presents; d->stats.lfb_bytes+=s->lfb_bytes;
    d->stats.composite_ns+=s->composite_ns; d->stats.present_ns+=s->present_ns;
    d->next_frame_ns=time_now(d)+DESK_FRAME_NS;
    return complete ? 0 : -1;
}
uint64_t desk_launch_wait(const struct desktop *d, uint64_t now)
{
    /* A stalled app must not make the desktop poll faster forever. */
    for (int i=0; i<DESK_CLIENTS; i++) {
        const struct desk_client *c=&d->clients[i];
        if (c->channel>=0 && c->awaiting_frame && now>=c->launched_ns &&
            now-c->launched_ns<5000000000ull) return 1000000;
    }
    return 10000000;
}
