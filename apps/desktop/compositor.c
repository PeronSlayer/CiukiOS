/* SPDX-License-Identifier: MIT
 * Integer-only CPU compositor. All drawing is clipped before pixel access. */
#include "desktop.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rows.h"
#include "../../assets/cursors/cursors.h"
#include "../../assets/cursors/paint.h"
/* libdeskui also links this unit for glyphs/frames without the desktop server. */
extern void desk_gfx_complete(struct desktop *,int,int,uint64_t,uint64_t) __attribute__((weak));
/* The host benchmark supplies this macro; native builds contain no timers. */
#ifndef DESK_PROFILE
#define DESK_PROFILE(name, ...) do { __VA_ARGS__; } while (0)
#endif
/* The shell (shell.c) is linked only into /Programs/desktop; the demo and the
 * libdeskui programs reuse this file for glyphs and frames without it. */
extern void desk_shell_background(struct desktop *, struct desk_box) __attribute__((weak));
extern void desk_shell_topbar(struct desktop *, struct desk_box) __attribute__((weak));
extern void desk_shell_overlay(struct desktop *, struct desk_box) __attribute__((weak));
extern void desk_shell_start(struct desktop *, struct desk_box) __attribute__((weak));
extern void desk_internal_paint(struct desktop *, int, struct desk_box, struct desk_box) __attribute__((weak));
extern const uint32_t *desk_shell_window_icon(struct desktop *, int, int) __attribute__((weak));
#include "catalog.h"
extern struct desk_box desk_shell_notification_bounds(const struct desktop *) __attribute__((weak));
int desk_notification_intersects(const struct desktop *d, struct desk_box box)
{
    struct desk_box ignored;
    return d->sh.notification.active && desk_shell_notification_bounds &&
        desk_intersect(box,desk_shell_notification_bounds(d),&ignored);
}
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
{ if ((c->v3_style&3)==DESK_V3_POPUP) return c->frame;
  return (struct desk_box){c->frame.x+DESK_BORDER,c->frame.y+DESK_TITLE,c->frame.w-2*DESK_BORDER,c->collapsed ? 0 : c->frame.h-DESK_TITLE-DESK_BORDER}; }
/* The frame plus its theme's drop shadow: the area a frame change repaints. */
struct desk_box desk_outer(const struct desk_client *c)
{
    if ((c->v3_style&3)==DESK_V3_POPUP) return c->frame;
    int32_t s=desk_theme->shadow_size;
    if (!s) return c->frame;
    return (struct desk_box){c->frame.x-s+s/3,c->frame.y-s+s/2,c->frame.w+2*s,c->frame.h+2*s};
}
/* Title-bar hit boxes: theme.c desk_title_button_box, the same in every theme. */
static struct desk_box title_box(const struct desk_client *c, unsigned button)
{
    if ((c->v3_style&3)==DESK_V3_POPUP || ((c->v3_style&3)==DESK_V3_TOOL && button!=DESK_TB_CLOSE)) return (struct desk_box){0};
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
    if (d->shell && desk_start_contains(d,x,y)) return 0;
    /* desk_hit's rule (topmost visible window), kept here: the toolkit
     * programs link this file without input.c. */
    int i=-1;
    for (int k=0; k<DESK_CLIENTS; k++) if (d->clients[k].window && !d->clients[k].minimized &&
        inside(d->clients[k].frame,x,y) && (i<0 || d->clients[k].order>d->clients[i].order)) i=k;
    if (i<0) return 0;
    for (int k=0; k<3; k++) {
        if (k && !d->shell) break;
        if (k && d->clients[i].owner_id) continue;
        if (k==2 && !d->clients[i].resizable) continue;
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
/* Shared top-bar tray geometry: non-shell programs also
 * link this compositor for frames and glyphs. Text is measured in the UI face. */
void desk_shell_clock_text_at(const struct desktop *d, uint64_t s, char *out, size_t size)
{
    unsigned h = (unsigned)(s / 3600 % 24), m = (unsigned)(s / 60 % 60), sec = (unsigned)(s % 60);
    if (d->sh.cfg.clock_24h) {
        if (d->sh.cfg.clock_seconds) snprintf(out, size, "%02u:%02u:%02u", h, m, sec);
        else snprintf(out, size, "%02u:%02u", h, m);
    } else {
        unsigned h12 = h % 12 ? h % 12 : 12;
        if (d->sh.cfg.clock_seconds) snprintf(out, size, "%u:%02u:%02u %s", h12, m, sec, h < 12 ? "AM" : "PM");
        else snprintf(out, size, "%u:%02u %s", h12, m, h < 12 ? "AM" : "PM");
    }
}
int desk_shell_clock_width_at(const struct desktop *d, uint64_t second)
{
    char text[24]; desk_shell_clock_text_at(d, second, text, sizeof(text));
    int w = desk_text_measure(desk_theme->font_ui, text);
    /* Reserve a stable second line for the ISO date, including rollover. */
    int date = desk_text_measure(desk_theme->font_ui, "0000-00-00");
    return w > date ? w : date;
}
static int tray_width_at(const struct desktop *d, uint64_t second)
{
    unsigned icons = 3 + (d->sh.media.removable_mask != 0);
    return 2 * SH_TRAY_MARGIN + desk_shell_clock_width_at(d, second) +
           (int)icons * (SH_TRAY_ICON_SIZE + SH_TRAY_GAP);
}
struct desk_box desk_shell_tray_box(const struct desktop *d)
{
    int w = tray_width_at(d, d->app_test ? 12 * 3600 : d->realtime_second);
    return (struct desk_box){(int32_t)d->width - w - 4, 4, w, DESK_BAR - 7};
}
struct desk_box desk_shell_clock_box(const struct desktop *d)
{
    int w = desk_shell_clock_width_at(d, d->app_test ? 12 * 3600 : d->realtime_second);
    struct desk_box t = desk_shell_tray_box(d);
    return (struct desk_box){t.x + t.w - SH_TRAY_MARGIN - w, t.y, w, t.h};
}
struct desk_box desk_shell_tray_icon_box(const struct desktop *d, enum sh_tray_icon icon)
{
    if (icon < SH_TRAY_VOLUME || icon > SH_TRAY_MEDIA ||
        (icon == SH_TRAY_MEDIA && !d->sh.media.removable_mask)) return (struct desk_box){0, 0, 0, 0};
    struct desk_box c = desk_shell_clock_box(d);
    int slot = icon == SH_TRAY_NOTIFICATION ? 0 : icon == SH_TRAY_MEDIA ? 3 : icon + 1;
    return (struct desk_box){c.x - (slot + 1) * (SH_TRAY_ICON_SIZE + SH_TRAY_GAP),
                            c.y + (c.h - SH_TRAY_ICON_SIZE) / 2, SH_TRAY_ICON_SIZE, SH_TRAY_ICON_SIZE};
}
struct desk_box desk_task_button(const struct desktop *d, int index)
{
    unsigned count=0, position=0;
    if (index<0 || index>=DESK_CLIENTS || !desk_task_visible(&d->clients[index])) return (struct desk_box){0,0,0,0};
    for (int i=0; i<DESK_CLIENTS; i++) if (desk_task_visible(&d->clients[i])) {
        if (i<index) position++;
        count++;
    }
    int32_t available=(int32_t)d->width-2*DESK_TASK_X;
    if (available<=0 || !count) return (struct desk_box){0,0,0,0};
    int32_t w=available/(int32_t)count;
    if (w>160) w=160;
    int32_t left=DESK_TASK_X+(int32_t)position*w;
    return (struct desk_box){left,(int32_t)d->height-DESK_BAR+3,w,DESK_BAR-6};
}
int32_t desk_menu_row(const struct desktop *d)
{ (void)d; return SH_START_ROW; }
/* The Ciuki (Start) button at the left end of the top bar. */
struct desk_box desk_start_button(const struct desktop *d)
{ (void)d; return (struct desk_box){DESK_START_X,DESK_START_Y,DESK_START_W,DESK_START_H}; }
struct desk_box desk_start_bounds(const struct desktop *d)
{
    struct desk_box b=desk_start_button(d);
    b.h+=DESK_START_SHADOW;
    return b;
}
int desk_start_contains(const struct desktop *d, int32_t x, int32_t y)
{
    struct desk_box b=desk_start_button(d);
    if (!inside(b,x,y)) return 0;
    int dx=2*(x-b.x)+1-b.w, dy=2*(y-b.y)+1-b.h;
    return dx*dx+dy*dy<=b.w*b.w;
}
int desk_shell_chrome_intersects(const struct desktop *d, struct desk_box box)
{
    struct desk_box ignored;
    return desk_notification_intersects(d,box) ||
        (d->shell && desk_intersect(box,desk_start_bounds(d),&ignored));
}
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
        int y0=(yy-y)*DESK_PORTRAIT_SIZE/size, y1=(yy-y+1)*DESK_PORTRAIT_SIZE/size;
        for (int32_t xx=0; xx<b.w; xx++) {
            int x0=(b.x+xx-x)*DESK_PORTRAIT_SIZE/size, x1=(b.x+xx-x+1)*DESK_PORTRAIT_SIZE/size;
            uint32_t r=0,g=0,blue=0,n=0;
            for (int sy=y0; sy<y1; sy++) for (int sx=x0; sx<x1; sx++) {
                uint32_t v=d->portrait[sy*DESK_PORTRAIT_SIZE+sx];
                r+=(v>>16)&255; g+=(v>>8)&255; blue+=v&255; n++;
            }
            dst[xx]=(r/n)<<16 | (g/n)<<8 | blue/n;
        }
    }
}
/* Frames are opaque away from their rounded corners. Keep per-window bounded
 * caption/edge rasters from the shared theme painter and copy those spans on
 * moves/exposure. Corners still blend over the current scene. Nothing in
 * theme.c is duplicated here; title, icon bytes and every style input key
 * the cache. Small/unusual frames and allocation failure use the painter. */
enum { CAPTION_CACHES=DESK_CLIENTS, CAPTION_MAX_WIDTH=2048 };
struct caption_cache {
    uint32_t *pixels;
    const struct desk_theme *theme;
    int w, h;
    struct desk_frame_style style;
    char title[DESK_TITLE_MAX+1];
    uint32_t icon[16*16];
};
struct desk_render_cache {
    unsigned width, height;
    struct caption_cache captions[CAPTION_CACHES];
    uint32_t pixels[];
};
static void render_cache_prepare(struct desktop *d)
{
    if (d->render_cache_prepared) return;
    d->render_cache_prepared=1;
    unsigned w=d->width<CAPTION_MAX_WIDTH ? d->width : CAPTION_MAX_WIDTH;
    unsigned h=d->height<CAPTION_MAX_WIDTH ? d->height : CAPTION_MAX_WIDTH;
    size_t stride=(size_t)w*(DESK_TITLE+DESK_BORDER)+2u*DESK_BORDER*h;
    struct desk_render_cache *cache=malloc(sizeof(*cache)+CAPTION_CACHES*stride*sizeof(*cache->pixels));
    if (!cache) return; /* Keep the painter fallback; never allocate on a client event. */
    memset(cache,0,sizeof(*cache)); cache->width=w; cache->height=h;
    for (unsigned i=0;i<CAPTION_CACHES;i++) cache->captions[i].pixels=cache->pixels+i*stride;
    d->render_cache=cache;
}

static void xrgb_row(uint32_t *restrict dst, const uint32_t *restrict src, int w)
{
    uint32_t used=w ? src[0]|src[w-1] : 0;
    if (!(used&0xff000000u)) for (int k=1;k+1<w;k++) used|=src[k];
    if (!(used&0xff000000u)) { desk_row_copy(dst,src,(size_t)w); return; }
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

static void frame_border(struct desktop *d, struct desk_paint *p, int index, struct desk_box f,
                         const struct desk_frame_style *s)
{
    int pad=desk_theme->radius+1;
    if (pad<1) pad=1;
    if (!d->render_cache || f.w<=2*pad || (unsigned)f.w>d->render_cache->width ||
        f.h<=DESK_TITLE+3 || (unsigned)f.h>d->render_cache->height) {
        desk_paint_frame_border(p,f.x,f.y,f.w,f.h,s); return;
    }
    struct caption_cache *c=&d->render_cache->captions[index%CAPTION_CACHES];
    const char *title=s->title ? s->title : "";
    int same=c->pixels && c->theme==desk_theme && c->w==f.w && c->h==f.h &&
        c->style.active==s->active && c->style.alert==s->alert && c->style.collapsed==s->collapsed && c->style.maximized==s->maximized &&
        c->style.buttons==s->buttons && c->style.hot==s->hot && c->style.pressed==s->pressed &&
        !strcmp(c->title,title) && !!c->style.icon==!!s->icon &&
        (!s->icon || !memcmp(c->icon,s->icon,sizeof(c->icon)));
    if (!same) {
        d->chrome_rebuilds++;
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
static uint64_t time_now(struct desktop *d);
static void client_rows(struct desktop *d, struct desk_client *c, struct desk_box clip)
{
    struct desk_box content=desk_content(c), old={content.x,content.y,(int)c->info.width,(int)c->info.height},b;
    if (c->pixels && content.w>0 && content.h>0 &&
        (content.w!=(int)c->info.width || content.h!=(int)c->info.height)) {
        if (desk_intersect(content,clip,&b)) desk_scale_rows(d->pixels+(size_t)b.y*d->width+b.x,d->width,
            c->pixels,c->info.width,c->info.width,c->info.height,(unsigned)content.w,(unsigned)content.h,
            (unsigned)(b.x-content.x),(unsigned)(b.y-content.y),(unsigned)b.w,(unsigned)b.h);
        return;
    }
    if (!c->pixels || !desk_intersect(content,old,&b) || !desk_intersect(b,clip,&b)) return;
    struct desk_box region=c->gfx_box, scaled;
    region.x+=content.x; region.y+=content.y;
    int has_scene=c->gfx_width && c->gfx_height && desk_intersect(b,region,&scaled);
    for (int y=b.y;y<b.y+b.h;y++) {
        uint32_t *dst=d->pixels+(size_t)y*d->width+b.x;
        const uint32_t *src=c->pixels+(size_t)(y-content.y)*c->info.width+b.x-content.x;
        if (has_scene && y>=scaled.y && y<scaled.y+scaled.h) {
            int left=scaled.x-b.x, right=left+scaled.w;
            if (left) xrgb_row(dst,src,left);
            if (right<b.w) xrgb_row(dst+right,src+right,b.w-right);
        } else xrgb_row(dst,src,b.w);
    }
    int timed=has_scene && c->gfx_timing && c->gfx_pending && !c->gfx_reply_pending;
    uint64_t begin=timed ? time_now(d) : 0;
    if (has_scene) desk_scale_rows(d->pixels+(size_t)scaled.y*d->width+scaled.x,d->width,
        c->pixels+(size_t)c->gfx_box.y*c->info.width+c->gfx_box.x,c->info.width,
        c->gfx_width,c->gfx_height,(unsigned)region.w,(unsigned)region.h,
        (unsigned)(scaled.x-region.x),(unsigned)(scaled.y-region.y),(unsigned)scaled.w,(unsigned)scaled.h);
    if (timed) c->gfx_stats.convert_ns+=time_now(d)-begin;
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
    if ((c->v3_style&3)==DESK_V3_POPUP) { client_rows(d,c,clip); return; }
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
        .active=i==d->focus && !c->unresponsive, .alert=c->unresponsive, .collapsed=c->collapsed, .maximized=c->zoomed,
        .buttons=d->shell && !c->owner_id && (c->v3_style&3)!=DESK_V3_TOOL ? DESK_TB_CLOSE|DESK_TB_MINIMIZE|(c->resizable ? DESK_TB_COLLAPSE : 0) : DESK_TB_CLOSE,
        .hot=hot, .pressed=hot && code==d->title_down && (d->buttons&1u) ? hot : 0};
    DESK_PROFILE("captions", {
        if (c->internal || c->collapsed) desk_paint_frame(&p,f.x,f.y,f.w,f.h,&s);
        else frame_border(d,&p,i,f,&s);
    });
    struct desk_box content = desk_content(c), b;
    if (c->collapsed) return;
    if (c->internal) {
        if (desk_internal_paint && desk_intersect(content,clip,&b)) desk_internal_paint(d,i,content,b);
        return;
    }
    /* A pending layout scales its retained backing across the entire well. */
    client_rows(d,c,clip);
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
         !strcmp(name,"control") || !strcmp(name,"ciukide") || !strcmp(name,"ciukide-app") || !strcmp(name,"terminal") || desk_catalog_find(name); }
const char *desk_program_label(const char *name)
{
    const struct desk_catalog_entry *e=desk_catalog_find(name);
    return !strcmp(name,"demo") ? "Ciuki Demo" : !strcmp(name,"files") ? "Files" : !strcmp(name,"tasks") ? "Task Manager" :
           !strcmp(name,"terminal") ? "Terminal" : !strcmp(name,"ciuknote") ? "CiukNote" : !strcmp(name,"ciukide") ? "CiukIDE" : !strcmp(name,"control") ? "Control Panel" : e ? e->label : name;
}
int desk_catalog_takes_file(const char *name)
{ const struct desk_catalog_entry *e=desk_catalog_find(name); return e && e->takes_file; }
const char *desk_app_program(int kind) { return kind==DESK_APP_NOTE ? "ciuknote" : "files"; }
int desk_program_kind(const char *name)
{ return !strcmp(name,"files") ? DESK_APP_FILES : !strcmp(name,"ciuknote") ? DESK_APP_NOTE : !strcmp(name,"control") ? DESK_APP_CONTROL :
         !strcmp(name,"ciukide") ? DESK_APP_IDE : !strcmp(name,"terminal") ? DESK_APP_TERMINAL : 0; }
void desk_shell_draw(struct desktop *d, struct desk_box clip)
{
    /* Shared shaded surface; the
     * shell (shell.c) adds the task buttons, the tray, menus and popups.
     * The Ciuki (Start) button lives in the top bar (desk_shell_topbar). */
    const struct desk_theme *t=desk_theme;
    struct desk_box bar=desk_taskbar(d);
    struct desk_paint p=paint_of(d,clip);
    desk_paint_bar(&p,bar.x,bar.y,bar.w,bar.h,t->taskbar,d->lfb_bytes_per_pixel==2);
    if (desk_shell_overlay) desk_shell_overlay(d,clip);
}
_Static_assert(sizeof(desk_cursors)/sizeof(desk_cursors[0])==DESK_CURSOR_PRECISION+1,"generic cursor inventory");
_Static_assert(DESK_CURSOR_ERASER+1==DESK_CURSOR_COUNT,"paint cursor inventory");
static struct desk_cursor_raster cursor_raster(const struct desktop *d)
{
    if (d->v3_cursor_id) {
        const struct desk_client *c=&d->clients[d->v3_cursor_slot]; unsigned id=d->v3_cursor_id-1;
        return (struct desk_cursor_raster){.hot_x=c->v3_hot_x[id],.hot_y=c->v3_hot_y[id],.width=32,.height=32};
    }
    unsigned shape=d->cursor_shape;
    if (shape<=DESK_CURSOR_PRECISION) return desk_cursors[shape];
    if (shape>=DESK_CURSOR_COUNT) return desk_cursors[DESK_CURSOR_ARROW];
    struct desk_tool_cursor tool=desk_tool_cursor(shape);
    struct desk_cursor_raster raster={(unsigned)tool.hot_x,(unsigned)tool.hot_y,32,32,{0},{0}};
    memcpy(raster.mask,tool.mask,sizeof(raster.mask));
    memcpy(raster.image,tool.image,sizeof(raster.image));
    return raster;
}
static struct desk_box pointer_box(const struct desktop *d)
{
    struct desk_cursor_raster r=cursor_raster(d);
    return (struct desk_box){d->mouse_x-(int)r.hot_x,d->mouse_y-(int)r.hot_y,(int)r.width,(int)r.height};
}
/* Exact horizontal opaque-region sweep. At most N+1 spans survive N
 * occluders on a row; geometry boundaries join rows into bands. No region
 * fragmentation can force painting beneath an opaque window. */
static void visible_paint(struct desktop *d, struct desk_box clip,
                          const int *windows, unsigned count,
                          void (*paint)(struct desktop *, int, struct desk_box), int index)
{
    for (int y=clip.y; y<clip.y+clip.h;) {
        int bottom=clip.y+clip.h;
        struct desk_dirty_row spans[DESK_CLIENTS+1]={{clip.x,clip.x+clip.w}};
        unsigned n=1;
        for (unsigned k=0;k<count;k++) {
            struct desk_client *c=&d->clients[windows[k]];
            if (!c->internal && !c->collapsed && !c->pixels) continue;
            struct desk_box f=c->frame;
            int boundaries[3]={f.y,f.y+desk_theme->radius,f.y+f.h};
            for (unsigned j=0;j<3;j++) if (boundaries[j]>y && boundaries[j]<bottom) bottom=boundaries[j];
            if (y<f.y || y>=f.y+f.h) continue;
            int pad=y<f.y+desk_theme->radius ? desk_theme->radius+1 : 0;
            int left=f.x+pad, right=f.x+f.w-pad;
            if (left>=right) continue;
            for (unsigned j=0;j<n;) {
                struct desk_dirty_row a=spans[j];
                if (right<=a.left || left>=a.right) { j++; continue; }
                if (left>a.left && right<a.right) {
                    memmove(spans+j+2,spans+j+1,(n-j-1)*sizeof(*spans));
                    spans[j].right=left; spans[j+1]=(struct desk_dirty_row){right,a.right};
                    n++; j+=2;
                } else if (left>a.left) { spans[j++].right=left; }
                else if (right<a.right) { spans[j++].left=right; }
                else { memmove(spans+j,spans+j+1,(--n-j)*sizeof(*spans)); }
            }
        }
        for (unsigned k=0;k<n;k++) paint(d,index,(struct desk_box){spans[k].left,y,spans[k].right-spans[k].left,bottom-y});
        y=bottom;
    }
}
static void paint_background(struct desktop *d, int index, struct desk_box clip)
{
    (void)index;
    if (d->shell && desk_shell_background) DESK_PROFILE("background_rows", desk_shell_background(d,clip));
    else { fill(d,clip,clip,desk_theme->desktop); portrait(d,clip); }
}
static void visible_window(struct desktop *d, int index, struct desk_box clip)
{ DESK_PROFILE("windows", window(d,index,clip)); }
static void scene(struct desktop *d, struct desk_box clip)
{
    if (!desk_intersect(clip,(struct desk_box){0,0,(int32_t)d->width,(int32_t)d->height},&clip)) return;
    /* A client-only update writes its opaque backing store directly. Higher
     * windows' shadows and shell overlays still need the ordinary painter. */
    if (!d->animation.active && !d->menu_open && !d->sh.levels && !d->sh.volume_open && !d->sh.network_open && !d->sh.media_open && !d->sh.tooltip && !desk_shell_chrome_intersects(d,clip) && !d->sh.switcher &&
        clip.y>=DESK_BAR && (!d->shell || clip.y+clip.h<=(int)d->height-DESK_BAR)) {
        for (int i=0;i<DESK_CLIENTS;i++) {
            struct desk_client *c=&d->clients[i];
            if (!c->window || c->minimized || c->collapsed || c->internal || !c->pixels) continue;
            struct desk_box content=desk_content(c), b;
            if (c->info.width<(unsigned)content.w || c->info.height<(unsigned)content.h ||
                !desk_intersect(content,clip,&b) || memcmp(&b,&clip,sizeof(b))) continue;
            int clear=1;
            for (int j=0;j<DESK_CLIENTS;j++) if (d->clients[j].window && !d->clients[j].minimized &&
                d->clients[j].order>c->order && desk_intersect(desk_outer(&d->clients[j]),clip,&b)) { clear=0; break; }
            if (clear) { DESK_PROFILE("windows",client_rows(d,c,clip)); return; }
        }
    }
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
    /* Cache maintenance does not write pixels, even with a covered desktop. */
    if (d->shell && desk_shell_background) desk_shell_background(d,(struct desk_box){0,0,0,0});
    visible_paint(d,clip,windows,count,paint_background,-1);
    desk_paint_bar(&p,0,0,(int)d->width,DESK_BAR,t->topbar,d->lfb_bytes_per_pixel==2);
    if (d->shell && desk_shell_topbar) DESK_PROFILE("bars_menus", desk_shell_topbar(d,clip));
    else {
        portrait_at(d,clip,8,6,20);
        desk_paint_text(&p,t->font_bold,34,20,"CiukiOS",t->topbar_text);   /* no tagline in the top bar (owner, 2026-10-12) */
    }
    char clock[24];
    uint64_t s = d->now_ns / 1000000000ull;
    snprintf(clock,sizeof(clock),"%02u:%02u:%02u",(unsigned)(s/3600%100),(unsigned)(s/60%60),(unsigned)(s%60));
    if (!d->shell) desk_paint_text(&p,t->font_ui,(int)d->width-12-desk_text_measure(t->font_ui,clock),20,clock,t->topbar_text);
    for (unsigned n=0; n<count; n++)
        visible_paint(d,clip,windows+n+1,count-n-1,visible_window,windows[n]);
    if (d->shell) DESK_PROFILE("bars_menus", desk_shell_draw(d,clip));
    if (d->animation.active) {
        struct desk_box b=d->animation.box;
        struct desk_paint p=paint_of(d,clip);
        for (int k=0;k<3;k++) {
            uint32_t ink=k==1 ? 0xffffffu : 0x101010u;
            desk_paint_fill(&p,b.x+k,b.y+k,b.w-2*k,1,ink);
            desk_paint_fill(&p,b.x+k,b.y+b.h-1-k,b.w-2*k,1,ink);
            desk_paint_fill(&p,b.x+k,b.y+k,1,b.h-2*k,ink);
            desk_paint_fill(&p,b.x+b.w-1-k,b.y+k,1,b.h-2*k,ink);
        }
    }
    if (d->shell && desk_shell_start) DESK_PROFILE("bars_menus", desk_shell_start(d,clip));
}
static void cursor(struct desktop *d, struct desk_box clip)
{
    struct desk_box b;
    struct desk_box box=pointer_box(d);
    struct desk_cursor_raster r=cursor_raster(d);
    if (d->hw_cursor || d->v3_cursor_hidden || !desk_intersect(box,clip,&b)) return;
    uint32_t *first_dst=d->pixels+(size_t)b.y*d->width+b.x;
    int stride=(int)d->width;
    for (int y=0; y<b.h; y++) {
        uint32_t *dst=first_dst+(size_t)y*stride;
        for (int x=0; x<b.w; x++) {
            if (d->v3_cursor_id) {
                unsigned px=(unsigned)(b.x+x-box.x), py=(unsigned)(b.y+y-box.y);
                uint32_t colour=d->clients[d->v3_cursor_slot].v3_cursor_pixels[d->v3_cursor_id-1][py*32+px];
                unsigned alpha=colour>>24, inv=255-alpha;
                unsigned red=(((colour>>16)&255)*alpha+((dst[x]>>16)&255)*inv+127)/255;
                unsigned green=(((colour>>8)&255)*alpha+((dst[x]>>8)&255)*inv+127)/255;
                unsigned blue=((colour&255)*alpha+(dst[x]&255)*inv+127)/255;
                dst[x]=(red<<16)|(green<<8)|blue; continue;
            }
            uint32_t bit=0x80000000u>>(b.x+x-box.x);
            unsigned row=(unsigned)(b.y+y-box.y);
            if (r.mask[row]&bit) dst[x]=r.image[row]&bit ? 0xffffffu : 0x101010u;
        }
    }
}
/* Reference/capture renderer: the runtime keeps the pointer out of the scene. */
void desk_composite(struct desktop *d, struct desk_box clip)
{
    if (!desk_intersect(clip,(struct desk_box){0,0,(int32_t)d->width,(int32_t)d->height},&clip)) return;
    render_cache_prepare(d);
    scene(d,clip); DESK_PROFILE("cursor", cursor(d,clip));
}
void desk_render_release(struct desktop *d)
{
    free(d->render_cache); d->render_cache=NULL; d->render_cache_prepared=0;
    d->scanout_valid=d->frame_valid=0;
    d->copy_pending=0; d->move_damage.count=0;
    free(d->rows); d->rows=NULL; d->row_height=0;
    free(d->sh.background); d->sh.background=NULL; d->sh.background_valid=0;
    d->cursor_valid=0; d->cursor_dirty=1; d->present_damage.count=0;
}
int desk_frame_due(const struct desktop *d, uint64_t now)
{ return now>=d->next_frame_ns && (d->damage.count || d->move_damage.count || d->present_damage.count || d->cursor_dirty); }
static uint64_t time_now(struct desktop *d) { return d->time_ns ? d->time_ns() : 0; }
static int accel_call(struct desktop *d, struct ciuki_accel_request *q)
{
    q->size=sizeof(*q); q->generation=d->accel_generation;
    return d->accel ? d->accel(d->display,q) : -ENOSYS;
}
void desk_accel_reset(struct desktop *d, uint32_t generation)
{
    d->scanout_valid=0; d->move_damage.count=0;
    d->hw_cursor=0; d->cursor_dirty=1; d->copy_pending=0;
    d->accel_generation=generation;
    struct ciuki_accel_request q={.operation=CIUKI_ACCEL_QUERY};
    int caps=accel_call(d,&q);
    d->accel_caps=caps>0 ? (uint32_t)caps : 0;
    d->cursor_loaded=DESK_CURSOR_COUNT;
    if ((d->accel_caps&2u) && !d->v3_cursor_id && !d->v3_cursor_hidden) {
        q=(struct ciuki_accel_request){.operation=CIUKI_ACCEL_CURSOR_SET,.colour=0xffffff,.background=0x101010};
        struct desk_cursor_raster r=cursor_raster(d);
        memcpy(q.cursor_mask,r.mask,sizeof(q.cursor_mask)); memcpy(q.cursor_image,r.image,sizeof(q.cursor_image));
        if (!accel_call(d,&q)) { d->hw_cursor=1; d->cursor_loaded=d->cursor_shape; }
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
        memmove(d->pixels+((uint32_t)r->dst_y+y)*d->width+(uint32_t)r->dst_x,
                d->pixels+((uint32_t)r->src_y+y)*d->width+(uint32_t)r->src_x,r->width*4);
    }
}
static int screen_copy(struct desktop *d, const struct ciuki_rect *r)
{
    struct ciuki_accel_request q={.operation=CIUKI_ACCEL_COPY,.rect=*r};
    if (accel_call(d,&q)) {
        d->scanout_valid=0;
        q.operation=CIUKI_ACCEL_QUERY;
        int caps=accel_call(d,&q);
        d->accel_caps=caps>0 ? (uint32_t)caps : 0;
        if (!(d->accel_caps&2u)) d->hw_cursor=0;
        desk_damage_add(&d->damage,(struct desk_box){0,0,(int32_t)d->width,(int32_t)d->height},d->width,d->height);
        return -1;
    }
    d->presents++; d->frame_stats.presents++;
    return 0;
}
/* A scroll is accelerated only after comparing every translated client pixel
 * with the last completed RAM frame. Samples merely reject candidates early. */
static int scroll_plan(struct desktop *d, struct ciuki_rect *out)
{
    if (!(d->accel_caps&1u) || !d->hw_cursor || !d->scanout_valid ||
        !d->damage.count || d->menu_open || d->sh.levels || d->sh.volume_open || d->sh.network_open || d->sh.media_open ||
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
        if (!visible || !damaged || desk_shell_chrome_intersects(d,b)) continue;
        for (int delta=1; delta<=64 && delta<b.h/2; delta++) for (int dir=-1; dir<=1; dir+=2) {
            int sy=dir<0 ? delta : 0, dy=dir<0 ? 0 : delta, same=1, changed=0;
            const uint32_t *a=c->pixels+(uint32_t)dy*c->info.width;
            const uint32_t *old=d->pixels+(uint32_t)(b.y+sy)*d->width+(uint32_t)b.x;
            if ((a[0]&0xffffffu)!=old[0] || (a[b.w/2]&0xffffffu)!=old[b.w/2] || (a[b.w-1]&0xffffffu)!=old[b.w-1]) continue;
            const uint32_t *at=d->pixels+(uint32_t)(b.y+dy)*d->width+(uint32_t)b.x;
            if ((a[0]&0xffffffu)==at[0] && (a[b.w/2]&0xffffffu)==at[b.w/2] && (a[b.w-1]&0xffffffu)==at[b.w-1]) continue;
            for (int y=0; y<b.h-delta && same; y++) for (int x=0; x<b.w; x++) {
                uint32_t v=c->pixels[(uint32_t)(dy+y)*c->info.width+(uint32_t)x]&0xffffffu;
                if (v!=d->pixels[(uint32_t)(b.y+sy+y)*d->width+(uint32_t)(b.x+x)]) { same=0; break; }
                if (v!=d->pixels[(uint32_t)(b.y+dy+y)*d->width+(uint32_t)(b.x+x)]) changed=1;
            }
            if (same && changed) {
                struct ciuki_rect r={b.x,b.y+sy,b.x,b.y+dy,(uint32_t)b.w,(uint32_t)(b.h-delta)};
                *out=r; return 1; /* one candidate per frame */
            }
        }
    }
    return 0;
}
/* Present the composite surface directly. There is no second frame to copy
 * or compare. COPY's exact destination is excluded from upload rows by the
 * caller; FILL is used only after checking every pixel in its span. */
static int present_box_raw(struct desktop *d, struct desk_box b)
{
    if (!(d->accel_caps&1u) || !d->accel || !d->scanout_valid) {
        struct ciuki_rect r={b.x,b.y,b.x,b.y,(uint32_t)b.w,(uint32_t)b.h};
        if (d->ops.present(d->display,d->output,&r)) return -1;
        d->frame_stats.lfb_bytes+=(uint64_t)b.w*b.h*(d->lfb_bytes_per_pixel ? d->lfb_bytes_per_pixel : 4);
        d->presents++; d->frame_stats.presents++; return 0;
    }
    for (int y=b.y;y<b.y+b.h;) {
        const uint32_t *row=d->pixels+(size_t)y*d->width+b.x;
        int solid=b.w>=64;
        for (int x=1;solid && x<b.w;x++) if (row[x]!=row[0]) solid=0;
        /* Composition is complete, and the preceding span was presented in
         * this call. Equal non-solid rows can copy that scanout span without
         * retaining another RAM frame. A failure uses the normal full retry. */
        if (!solid && b.w>=64 && y>b.y &&
            !memcmp(row,row-d->width,(size_t)b.w*4)) {
            struct ciuki_rect r={b.x,y-1,b.x,y,(uint32_t)b.w,1};
            if (screen_copy(d,&r)) return -1;
            y++; continue;
        }
        int bottom=y+1;
        if (solid) {
            for (;bottom<b.y+b.h;bottom++) {
                const uint32_t *next=d->pixels+(size_t)bottom*d->width+b.x;
                int same=1;
                for (int x=0;x<b.w;x++) if (next[x]!=row[0]) { same=0; break; }
                if (!same) break;
            }
        } else {
            /* Upload distinct rows together, stopping before a duplicate so
             * its now-presented predecessor can supply a screen copy. */
            for (;bottom<b.y+b.h;bottom++) {
                const uint32_t *next=d->pixels+(size_t)bottom*d->width+b.x;
                if (b.w>=64 && !memcmp(next,next-d->width,(size_t)b.w*4)) break;
            }
        }
        struct ciuki_rect r={b.x,y,b.x,y,(uint32_t)b.w,(uint32_t)(bottom-y)};
        int filled=0;
        if (solid) {
            struct ciuki_accel_request q={.operation=CIUKI_ACCEL_FILL,.colour=row[0],.rect=r};
            if (!accel_call(d,&q)) filled=1;
            else {
                /* A failed engine may have changed part of scanout. Retain
                 * a full software recovery frame rather than trusting it. */
                d->scanout_valid=0;
                desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,(int)d->height},d->width,d->height);
                return -1;
            }
        }
        if (!filled) {
            if (d->ops.present(d->display,d->output,&r)) return -1;
            d->frame_stats.lfb_bytes+=(uint64_t)r.width*r.height*(d->lfb_bytes_per_pixel ? d->lfb_bytes_per_pixel : 4);
        }
        d->presents++; d->frame_stats.presents++; y=bottom;
    }
    return 0;
}
/* Attribute actual present calls intersecting each outstanding demo window.
 * A merged upload can include other damaged pixels; never estimate PCI time
 * from bytes or count that duration as additional compositor work. */
static int present_box(struct desktop *d, struct desk_box b)
{
    unsigned mask=0;
    for (int i=0;i<DESK_CLIENTS;i++) {
        struct desk_client *c=&d->clients[i];struct desk_box cut;
        if (c->gfx_timing && c->gfx_pending && !c->gfx_reply_pending &&
            !c->minimized && !c->collapsed && desk_intersect(b,desk_content(c),&cut)) mask|=1u<<i;
    }
    uint64_t begin=mask ? time_now(d) : 0;
    int result=present_box_raw(d,b);
    if (mask) {
        uint64_t elapsed=time_now(d)-begin;
        for (int i=0;i<DESK_CLIENTS;i++) if (mask&(1u<<i)) d->clients[i].gfx_stats.copy_ns+=elapsed;
    }
    return result;
}
static void rows_clear(struct desktop *d)
{
    for (uint32_t y=0;y<d->height;y++) d->rows[y].count=0;
}
static void rows_add(struct desktop *d, struct desk_box box)
{
    if (!desk_intersect(box,(struct desk_box){0,0,(int)d->width,(int)d->height},&box)) return;
    for (int y=box.y;y<box.y+box.h;y++) {
        struct desk_dirty_runs *r=&d->rows[y];
        int left=box.x, right=box.x+box.w;
        unsigned i=0;
        while (i<r->count && r->spans[i].right<left) i++;
        unsigned first=i;
        while (i<r->count && r->spans[i].left<=right) {
            if (r->spans[i].left<left) left=r->spans[i].left;
            if (r->spans[i].right>right) right=r->spans[i].right;
            i++;
        }
        /* There are at most DESK_DAMAGE_MAX source boxes in this pass. */
        memmove(r->spans+first+1,r->spans+i,(r->count-i)*sizeof(*r->spans));
        r->count=r->count-(i-first)+1;
        r->spans[first]=(struct desk_dirty_row){left,right};
    }
}
/* Equal adjacent span lists form a band. A copied destination splits its
 * spans without filling the clean gaps between separate text updates. */
static unsigned row_boxes(struct desktop *d, uint32_t *y, struct desk_box cut,
                          struct desk_box out[DESK_DAMAGE_MAX+1])
{
    while (*y<d->height && !d->rows[*y].count) ++*y;
    if (*y==d->height) return 0;
    int top=(int)*y, bottom=top+1;
    struct desk_dirty_runs *r=&d->rows[top];
    int in_cut=cut.w>0 && top>=cut.y && top<cut.y+cut.h;
    while (bottom<(int)d->height && d->rows[bottom].count==r->count &&
           !memcmp(d->rows[bottom].spans,r->spans,r->count*sizeof(*r->spans)) &&
           in_cut==(cut.w>0 && bottom>=cut.y && bottom<cut.y+cut.h)) bottom++;
    *y=(uint32_t)bottom;
    unsigned n=0;
    for (unsigned k=0;k<r->count;k++) {
        struct desk_dirty_row row=r->spans[k];
        if (!in_cut || cut.x>=row.right || cut.x+cut.w<=row.left)
            out[n++]=(struct desk_box){row.left,top,row.right-row.left,bottom-top};
        else {
            if (cut.x>row.left) out[n++]=(struct desk_box){row.left,top,cut.x-row.left,bottom-top};
            if (cut.x+cut.w<row.right) out[n++]=(struct desk_box){cut.x+cut.w,top,row.right-cut.x-cut.w,bottom-top};
        }
    }
    return n;
}
static void render_rows(struct desktop *d, struct desk_box cut)
{
    uint32_t y=0;
    while (y<d->height) {
        struct desk_box boxes[DESK_DAMAGE_MAX+1]; unsigned n=row_boxes(d,&y,cut,boxes);
        for (unsigned k=0;k<n;k++) {
            scene(d,boxes[k]); d->frame_stats.scene_px+=(uint64_t)boxes[k].w*boxes[k].h;
        }
    }
}
static int present_rows(struct desktop *d, struct desk_box cut)
{
    uint32_t y=0;
    while (y<d->height) {
        struct desk_box boxes[DESK_DAMAGE_MAX+1]; unsigned n=row_boxes(d,&y,cut,boxes);
        for (unsigned k=0;k<n;k++) {
            d->frame_stats.damage_px+=(uint64_t)boxes[k].w*boxes[k].h;
            if (present_box(d,boxes[k])) return -1;
        }
    }
    return 0;
}
static int rows_cover(struct desktop *d, struct desk_box box)
{
    for (int y=box.y;y<box.y+box.h;y++) {
        const struct desk_dirty_runs *r=&d->rows[y];
        int covered=0;
        for (unsigned k=0;k<r->count;k++)
            if (r->spans[k].left<=box.x && r->spans[k].right>=box.x+box.w) { covered=1; break; }
        if (!covered) return 0;
    }
    return 1;
}
static void latency_record(struct desktop *d, uint64_t latency)
{
    struct desk_frame_stats *s=&d->stats;
    uint64_t bucket=latency/1000000ull;
    if (bucket>=DESK_LATENCY_BUCKETS) bucket=DESK_LATENCY_BUCKETS-1;
    d->latency_buckets[bucket]++; s->completed++;
    if (latency>s->frame_max_ns) s->frame_max_ns=latency;
    uint64_t rank=s->completed-s->completed/20, count=0;
    for (unsigned i=0;i<DESK_LATENCY_BUCKETS;i++) {
        count+=d->latency_buckets[i];
        if (count>=rank) {
            s->frame_p95_ns=i==DESK_LATENCY_BUCKETS-1 ? s->frame_max_ns : (i+1)*1000000ull;
            break;
        }
    }
}
void desk_stats_sample(struct desktop *d)
{
    d->sample=d->stats;
    memset(&d->stats,0,sizeof(d->stats));
    memset(d->latency_buckets,0,sizeof(d->latency_buckets));
}
int desk_redraw(struct desktop *d)
{
    if (d->v3_lease_state) return 0;
    if ((d->v3_cursor_id || d->v3_cursor_hidden) && d->hw_cursor) {
        struct ciuki_accel_request hide={.operation=CIUKI_ACCEL_CURSOR_SHOW,.colour=0};
        (void)accel_call(d,&hide); d->hw_cursor=0;
    }
    if (!d->damage.count && !d->move_damage.count && !d->present_damage.count && !d->cursor_dirty) return 0;
    /* A RESYNC/damage can precede the mode-counter service turn. Refresh
     * the existing grant query before any generation-tagged engine request;
     * an unchanged layout can still have a new framebuffer lease. */
    if (d->display_info) {
        struct ciuki_framebuffer_info fb;
        if (d->display_info(d->display,&fb)) return -1;
        if (fb.width!=d->width || fb.height!=d->height) { errno=EAGAIN; return -1; }
        if (fb.generation!=d->accel_generation) {
            d->lfb_bytes_per_pixel=(fb.bpp+7)/8;
            desk_accel_reset(d,fb.generation);
        }
    }
    uint64_t frame_begin=time_now(d);
    render_cache_prepare(d);
    if (!d->rows || d->row_height!=d->height) {
        struct desk_dirty_runs *next=malloc((size_t)d->height*sizeof(*next));
        if (!next) { d->next_frame_ns=frame_begin+DESK_FRAME_NS; errno=ENOMEM; return -1; }
        free(d->rows); d->rows=next; d->row_height=d->height;
        d->frame_valid=d->scanout_valid=0; d->cursor_valid=0;
        desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,(int)d->height},d->width,d->height);
    }
    struct desk_frame_stats *s=&d->frame_stats;
    memset(s,0,sizeof(*s)); s->frames=1;
    uint64_t begin=time_now(d);
    if (d->accel) {
        struct ciuki_accel_request q={.operation=CIUKI_ACCEL_QUERY};
        int caps=accel_call(d,&q); d->accel_caps=caps>0 ? (uint32_t)caps : 0;
        if (d->hw_cursor && !(d->accel_caps&2u)) {
            d->hw_cursor=0; d->cursor_dirty=1; d->scanout_valid=0;
            desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,(int)d->height},d->width,d->height);
        }
    }
    s->present_ns+=time_now(d)-begin;
    begin=time_now(d);
    if (d->cursor_valid) {
        struct desk_box b=d->cursor_box;
        for (int y=0;y<b.h;y++)
            memcpy(d->pixels+(size_t)(b.y+y)*d->width+b.x,d->cursor_under+y*32,(size_t)b.w*4);
        if (d->cursor_dirty) desk_damage_add(&d->present_damage,b,d->width,d->height);
    }
    struct ciuki_rect copy=d->copy_rect;
    int moving=d->copy_pending && d->frame_valid;
    int engine_copy=moving && d->accel && (d->accel_caps&1u) && d->hw_cursor && d->scanout_valid;
    int scrolling=!moving && scroll_plan(d,&copy);
    engine_copy|=scrolling;
    struct desk_box copied={copy.dst_x,copy.dst_y,(int)copy.width,(int)copy.height};
    if (moving) {
        DESK_PROFILE("windows", ram_copy(d,&copy));
        rows_clear(d);
        for (unsigned i=0;i<d->move_damage.count;i++) rows_add(d,d->move_damage.boxes[i]);
        render_rows(d,copied);
    }
    rows_clear(d);
    for (unsigned i=0;i<d->damage.count;i++) rows_add(d,d->damage.boxes[i]);
    render_rows(d,(struct desk_box){0});
    d->frame_valid=1;
    struct desk_box b;
    if (!d->hw_cursor && desk_intersect(pointer_box(d),
                       (struct desk_box){0,0,(int)d->width,(int)d->height},&b)) {
        for (int y=0;y<b.h;y++)
            memcpy(d->cursor_under+y*32,d->pixels+(size_t)(b.y+y)*d->width+b.x,(size_t)b.w*4);
        d->cursor_box=b; d->cursor_valid=1; DESK_PROFILE("cursor", cursor(d,b));
        if (d->cursor_dirty) desk_damage_add(&d->present_damage,b,d->width,d->height);
    } else d->cursor_valid=0;
    if (!d->hw_cursor) d->cursor_dirty=0;
    s->composite_ns=time_now(d)-begin;
    begin=time_now(d);
    int complete=1;
    if (engine_copy && screen_copy(d,&copy)) complete=0;
    /* Move exposure excludes the exact scanout copy. General damage (new
     * client pixels, changed captions) must still overwrite that destination. */
    if (complete && moving) {
        rows_clear(d);
        for (unsigned i=0;i<d->move_damage.count;i++) rows_add(d,d->move_damage.boxes[i]);
        if (present_rows(d,engine_copy ? copied : (struct desk_box){0})) complete=0;
    }
    if (complete) {
        rows_clear(d);
        for (unsigned i=0;i<d->damage.count;i++) rows_add(d,d->damage.boxes[i]);
        if (present_rows(d,scrolling ? copied : (struct desk_box){0})) complete=0;
    }
    if (complete) for (unsigned i=0;i<d->present_damage.count;i++) {
        struct desk_box dirty=d->present_damage.boxes[i];
        if (rows_cover(d,dirty) && !scrolling) continue;
        s->damage_px+=(uint64_t)dirty.w*dirty.h;
        if (present_box(d,dirty)) { complete=0; break; }
    }
    /* Faults may hide the hardware pointer. Reconstruct with a software
     * cursor before acknowledging either launch or a display-mode change. */
    if (d->hw_cursor) {
        struct ciuki_accel_request q={.operation=CIUKI_ACCEL_QUERY};
        int caps=accel_call(d,&q); d->accel_caps=caps>0 ? (uint32_t)caps : 0;
        if (!(d->accel_caps&2u)) { d->hw_cursor=0; d->cursor_dirty=1; complete=0; }
    }
    if (d->hw_cursor && d->cursor_dirty && complete) {
        if (d->cursor_loaded!=d->cursor_shape) {
            struct ciuki_accel_request q={.operation=CIUKI_ACCEL_CURSOR_SET,.colour=0xffffff,.background=0x101010};
            struct desk_cursor_raster r=cursor_raster(d);
            memcpy(q.cursor_mask,r.mask,sizeof(q.cursor_mask)); memcpy(q.cursor_image,r.image,sizeof(q.cursor_image));
            if (accel_call(d,&q)) { d->hw_cursor=0; complete=0; }
            else d->cursor_loaded=d->cursor_shape;
        }
    }
    if (d->hw_cursor && d->cursor_dirty && complete) {
        struct desk_box box=pointer_box(d);
        struct ciuki_accel_request q={.operation=CIUKI_ACCEL_CURSOR_MOVE,
            .rect={0,0,box.x,box.y,0,0}};
        int error=accel_call(d,&q); q.operation=CIUKI_ACCEL_CURSOR_SHOW; q.colour=1;
        if (!error) error=accel_call(d,&q);
        if (error) { d->hw_cursor=0; complete=0; }
        else { d->cursor_dirty=0; d->cursor_x=d->mouse_x; d->cursor_y=d->mouse_y; }
    }
    uint64_t end=time_now(d);
    s->present_ns+=end-begin;
    uint64_t input=d->frame_input_ns;
    if (complete) {
        for (int i=0;i<DESK_CLIENTS;i++) {
            struct desk_client *c=&d->clients[i];
            if (c->launch_frame_pending && c->window && !c->minimized && !c->collapsed) {
                uint64_t click=c->launch_input_ns ? c->launch_input_ns : c->launched_ns;
                if (click && (!input || click<input)) input=click;
                c->launch_frame_pending=0;
                if (d->launch_mark) d->launch_mark(CIUKI_LAUNCH_FRAME,c->pid,0);
            }
        }
        uint64_t start=input && input<=end ? input : frame_begin;
        s->frame_ns=end>=start ? end-start : 0; s->completed=1;
        latency_record(d,s->frame_ns); d->frame_input_ns=0;
        d->damage.count=d->present_damage.count=0; d->scanout_valid=1;
    } else {
        d->scanout_valid=0;
        if (d->accel) d->cursor_dirty=1;
        /* The copied source no longer exists in RAM; retries reconstruct. */
        for (unsigned i=0;i<d->move_damage.count;i++)
            desk_damage_add(&d->damage,d->move_damage.boxes[i],d->width,d->height);
        if (engine_copy || d->accel)
            desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,(int)d->height},d->width,d->height);
    }
    d->copy_pending=0; d->move_damage.count=0;
    s->input_ns=d->pending_input_ns; s->input_lag_ns=d->pending_input_lag_ns;
    d->pending_input_ns=d->pending_input_lag_ns=0;
    d->stats.frames++; d->stats.damage_px+=s->damage_px; d->stats.scene_px+=s->scene_px;
    d->stats.presents+=s->presents; d->stats.lfb_bytes+=s->lfb_bytes;
    d->stats.composite_ns+=s->composite_ns; d->stats.present_ns+=s->present_ns;
    uint64_t slot=frame_begin;
    if (d->next_frame_ns && d->next_frame_ns<=frame_begin)
        slot=d->next_frame_ns+(frame_begin-d->next_frame_ns)/DESK_FRAME_NS*DESK_FRAME_NS;
    uint64_t deadline=slot+DESK_FRAME_NS;
    /* Processing overruns measure work against a full frame budget. A new
     * update arriving just before a phase boundary is not an overrun. Idle
     * slots and wake-up lateness still preserve the absolute next-slot phase. */
    uint64_t budget=frame_begin+DESK_FRAME_NS;
    if (end>budget) { s->overruns=1;s->dropped=(end-budget)/DESK_FRAME_NS+1; }
    if (end>=deadline) deadline+=((end-deadline)/DESK_FRAME_NS+1)*DESK_FRAME_NS;
    d->stats.overruns+=s->overruns; d->stats.dropped+=s->dropped;
    s->frame_p95_ns=d->stats.frame_p95_ns; s->frame_max_ns=d->stats.frame_max_ns;
    d->next_frame_ns=deadline;
    if (complete && desk_gfx_complete) for (int i=0;i<DESK_CLIENTS;i++) {
        struct desk_client *c=&d->clients[i];
        if (c->gfx_pending) desk_gfx_complete(d,i,c->minimized || c->collapsed ? -EAGAIN : 0,end,deadline);
    }
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
