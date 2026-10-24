/* SPDX-License-Identifier: MIT
 * Integer-only CPU compositor. All drawing is clipped before pixel access. */
#include "desktop.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rows.h"
#include "../libdeskui/deskui.h"
#pragma weak du_commands
/* f2-95 (lead amendment): damage and clip regions are the X11 sample
 * server's region algorithms as maintained in pixman 0.46.4 (MIT,
 * third_party/pixman, pinned in its README.md), compiled into this unit
 * with internal linkage; nothing floating point is emitted. */
static void *region_malloc(size_t);
static void *region_realloc(void *, size_t);
static void region_free(void *);
#define CIUKI_PIXMAN_ALLOC region_malloc
#define CIUKI_PIXMAN_REALLOC region_realloc
#define CIUKI_PIXMAN_FREE region_free
/* Any allocator macros of the includer (host footprint harness) return
 * afterwards; only the region code sees the pool. */
#pragma push_macro("malloc")
#pragma push_macro("realloc")
#pragma push_macro("free")
#undef malloc
#undef realloc
#undef free
#include "../../third_party/pixman/pixman-region32.c"
#undef malloc
#undef realloc
#undef free
#pragma pop_macro("free")
#pragma pop_macro("realloc")
#pragma pop_macro("malloc")
/* Region data comes from a fixed pool: the desktop allocates nothing after
 * its first frame (f2-55/f2-60, tests/host/desktop/test_memory_footprint.py).
 * The compositor runs on the desktop's main thread only. A request beyond
 * the pool fails like an exhausted heap: pixman marks the region broken
 * and the callers below fall back to a covering box. */
#define REGION_SMALL_RECTS 32
#define REGION_LARGE_RECTS 512
#define REGION_SMALL_BLOCKS 64
#define REGION_LARGE_BLOCKS 4
#define REGION_BYTES(n) (sizeof(pixman_region32_data_t)+(size_t)(n)*sizeof(pixman_box32_t))
static long region_small[REGION_SMALL_BLOCKS][REGION_BYTES(REGION_SMALL_RECTS)/sizeof(long)+1];
static long region_large[REGION_LARGE_BLOCKS][REGION_BYTES(REGION_LARGE_RECTS)/sizeof(long)+1];
static uint64_t region_small_used; static unsigned region_large_used;
static void *region_malloc(size_t n)
{
    if (n<=sizeof(region_small[0])) for (unsigned i=0;i<REGION_SMALL_BLOCKS;i++)
        if (!(region_small_used&(1ull<<i))) { region_small_used|=1ull<<i; return region_small[i]; }
    if (n<=sizeof(region_large[0])) for (unsigned i=0;i<REGION_LARGE_BLOCKS;i++)
        if (!(region_large_used&(1u<<i))) { region_large_used|=1u<<i; return region_large[i]; }
    return NULL;
}
/* Pool index of p, or -1: small blocks 0..63, large blocks 64.. */
static int region_block(const void *p)
{
    uintptr_t a=(uintptr_t)p, small=(uintptr_t)region_small, large=(uintptr_t)region_large;
    if (a>=small && a<small+sizeof(region_small)) return (int)((a-small)/sizeof(region_small[0]));
    if (a>=large && a<large+sizeof(region_large)) return REGION_SMALL_BLOCKS+(int)((a-large)/sizeof(region_large[0]));
    return -1;
}
static size_t region_capacity(const void *p)
{
    int i=region_block(p);
    return i<0 ? 0 : i<REGION_SMALL_BLOCKS ? sizeof(region_small[0]) : sizeof(region_large[0]);
}
static void region_free(void *p)
{
    int i=region_block(p);
    if (i>=REGION_SMALL_BLOCKS) region_large_used&=~(1u<<(i-REGION_SMALL_BLOCKS));
    else if (i>=0) region_small_used&=~(1ull<<i);
}
static void *region_realloc(void *p, size_t n)
{
    if (!p) return region_malloc(n);
    size_t have=region_capacity(p);
    if (n<=have) return p;
    void *q=region_malloc(n);
    if (!q) return NULL; /* p stays valid, as with realloc */
    memcpy(q,p,have); region_free(p);
    return q;
}
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
extern unsigned desk_shell_occluders(const struct desktop *,struct desk_box *) __attribute__((weak));
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
    /* G15 WS_EX_CONTEXTHELP: the "?" takes the minimise slot and the window has no minimise or zoom. */
    int help=(c->v3_style&DESK_V3_CONTEXTHELP)!=0;
    if (button==DESK_TB_HELP ? !help : help && button!=DESK_TB_CLOSE) return (struct desk_box){0};
    int b[4];
    desk_title_button_box(c->frame.x,c->frame.y,c->frame.w,button,b);
    return (struct desk_box){b[0],b[1],b[2],b[3]};
}
struct desk_box desk_close_box(const struct desk_client *c) { return title_box(c,DESK_TB_CLOSE); }
/* The caption icon (WM-11): drawn in the themes whose caption has one (theme.c
 * paint_frame: not the Platinum pinstripes); a click on it opens the window menu. */
struct desk_box desk_caption_icon_box(const struct desktop *d, const struct desk_client *c)
{
    if (!d->shell || !c->window || (c->v3_style&3)==DESK_V3_POPUP || desk_theme->title_style==DESK_TITLE_PINSTRIPE || c->frame.w<56)
        return (struct desk_box){0};
    return (struct desk_box){c->frame.x+6,c->frame.y+4,16,16};
}
struct desk_box desk_min_box(const struct desk_client *c) { return title_box(c,DESK_TB_MINIMIZE); }
struct desk_box desk_help_box(const struct desk_client *c) { return title_box(c,DESK_TB_HELP); }   /* G15 */
/* Zoom button, left of minimise (the old function and theme flag names are retained). */
struct desk_box desk_shade_box(const struct desk_client *c) { return title_box(c,DESK_TB_COLLAPSE); }
/* The title button (DESK_TB_*) of the topmost window under the pointer, as
 * index*4+k+1 (k: 0 close, 1 minimise, 2 zoom, 3 help), or 0: hover tracking. */
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
    struct desk_box help=title_box(&d->clients[i],DESK_TB_HELP);   /* G15: k 3, also on owned dialogs */
    if (help.w>0 && inside(help,x,y)) return i*4+3+1;
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
    unsigned icons = 3 + (d->sh.cfg.layout_switch != 0) +
        ((d->sh.media.removable_mask & ~7u) != 0) +
        (d->sh.power_status.version == CIUKI_POWER_VERSION && !d->sh.power_meter_hidden) +
        (d->sh.resources.visible != 0);
    for (unsigned i=0;i<SH_PROGRAM_TRAY;i++) icons+=!!d->sh.program_tray[i].window;
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
    int has_media = (d->sh.media.removable_mask & ~7u) != 0;
    int has_power = d->sh.power_status.version == CIUKI_POWER_VERSION && !d->sh.power_meter_hidden;
    int has_resources = d->sh.resources.visible != 0;
    if (icon < SH_TRAY_VOLUME || icon > SH_TRAY_RESOURCES ||
        (icon == SH_TRAY_LANGUAGE && !d->sh.cfg.layout_switch) ||
        (icon == SH_TRAY_POWER && !has_power) ||
        (icon == SH_TRAY_RESOURCES && !has_resources) ||
        (icon == SH_TRAY_MEDIA && !has_media)) return (struct desk_box){0, 0, 0, 0};
    struct desk_box c = desk_shell_clock_box(d);
    int slot = icon == SH_TRAY_NOTIFICATION ? 0 :
        icon == SH_TRAY_MEDIA ? 3 : icon == SH_TRAY_POWER ? 3 + has_media :
        icon == SH_TRAY_RESOURCES ? 3 + has_media + has_power :
        icon == SH_TRAY_LANGUAGE ? 3 + has_media + has_power + has_resources :
        (int)icon + 1;
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
    if (d->sh.user_gate) return 1;
    struct desk_box ignored;
    if(d->shell && d->sh.cfg.mouse_trails)for(unsigned k=0;k<d->pointer_trail_count;k++)
        if(desk_intersect(box,(struct desk_box){d->pointer_trail[k].x-64,d->pointer_trail[k].y-64,128,128},&ignored))return 1;
    return desk_notification_intersects(d,box) ||
        (d->shell && desk_intersect(box,desk_start_bounds(d),&ignored)) ||
        /* fx-19: the translucent drag image and the desktop marquee. */
        (d->shell && d->sh.dnd.active && d->sh.dnd.image.w && desk_intersect(box,d->sh.dnd.image,&ignored)) ||
        (d->shell && d->sh.band);
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
static pixman_box32_t region_box(struct desk_box b)
{ return (pixman_box32_t){b.x,b.y,b.x+b.w,b.y+b.h}; }
static struct desk_box box_region(pixman_box32_t r)
{ return (struct desk_box){r.x1,r.y1,r.x2-r.x1,r.y2-r.y1}; }
/* f2-95: a damage list holds at most DESK_DAMAGE_MAX rectangles. Beyond
 * that, the two rectangles whose bounding box adds the least area are
 * joined (the joined box may overlap others: the frame's region union
 * removes overlaps), so scattered small damage never grows into the whole
 * screen the way a single bounding box did. */
static void damage_reduce(struct desk_damage *d, pixman_box32_t *r, unsigned n)
{
    while (n>DESK_DAMAGE_MAX) {
        unsigned bi=0, bj=1; uint64_t best=UINT64_MAX;
        for (unsigned i=0;i<n;i++) for (unsigned j=i+1;j<n;j++) {
            int32_t x1=r[i].x1<r[j].x1 ? r[i].x1 : r[j].x1, y1=r[i].y1<r[j].y1 ? r[i].y1 : r[j].y1;
            int32_t x2=r[i].x2>r[j].x2 ? r[i].x2 : r[j].x2, y2=r[i].y2>r[j].y2 ? r[i].y2 : r[j].y2;
            uint64_t grow=(uint64_t)(x2-x1)*(uint64_t)(y2-y1);
            uint64_t own=(uint64_t)(r[i].x2-r[i].x1)*(uint64_t)(r[i].y2-r[i].y1)+(uint64_t)(r[j].x2-r[j].x1)*(uint64_t)(r[j].y2-r[j].y1);
            grow=grow>own ? grow-own : 0;
            if (grow<best) { best=grow; bi=i; bj=j; }
        }
        pixman_box32_t a=r[bi], b=r[bj];
        r[bi]=(pixman_box32_t){a.x1<b.x1 ? a.x1 : b.x1,a.y1<b.y1 ? a.y1 : b.y1,a.x2>b.x2 ? a.x2 : b.x2,a.y2>b.y2 ? a.y2 : b.y2};
        r[bj]=r[--n];
    }
    for (unsigned i=0;i<n;i++) d->boxes[i]=box_region(r[i]);
    d->count=n;
}
void desk_damage_add(struct desk_damage *d, struct desk_box b, uint32_t w, uint32_t h)
{
    if (!desk_intersect(b, (struct desk_box){0,0,(int32_t)w,(int32_t)h}, &b)) return;
    pixman_box32_t boxes[DESK_DAMAGE_MAX+1];
    unsigned count=d->count<=DESK_DAMAGE_MAX ? d->count : DESK_DAMAGE_MAX;
    for (unsigned i=0;i<count;i++) boxes[i]=region_box(d->boxes[i]);
    boxes[count]=region_box(b);
    /* Region union (pixman_region32_init_rects validates the list into
     * disjoint y-x bands and coalesces equal bands). */
    pixman_region32_t region;
    if (!pixman_region32_init_rects(&region,boxes,(int)count+1)) {
        /* No memory for the region: one bounding box still covers it all. */
        pixman_box32_t all=boxes[0];
        for (unsigned i=1;i<=count;i++) {
            if (boxes[i].x1<all.x1) all.x1=boxes[i].x1;
            if (boxes[i].y1<all.y1) all.y1=boxes[i].y1;
            if (boxes[i].x2>all.x2) all.x2=boxes[i].x2;
            if (boxes[i].y2>all.y2) all.y2=boxes[i].y2;
        }
        d->count=1; d->boxes[0]=box_region(all);
        return;
    }
    int n; pixman_box32_t *rects=pixman_region32_rectangles(&region,&n);
    if (n<=(int)DESK_DAMAGE_MAX) {
        for (int i=0;i<n;i++) d->boxes[i]=box_region(rects[i]);
        d->count=(unsigned)n;
    } else damage_reduce(d,rects,(unsigned)n); /* rects belong to the region */
    pixman_region32_fini(&region);
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
/* The decorations a window is painted with: window() and the fx-15
 * reconcile share it, so a change of any input is a change of the record. */
static struct desk_frame_style frame_style(struct desktop *d, int i)
{
    const struct desk_client *c=&d->clients[i];
    /* Hover and pressed title buttons follow the pointer (input.c tracks
     * the damage): the button under it, held while the left button is down. */
    int code=desk_title_code(d,d->mouse_x,d->mouse_y);
    unsigned hot=code && (code-1)/4==i ? 1u<<((code-1)%4) : 0;
    return (struct desk_frame_style){
        .title=c->closing ? "Closing..." : c->unresponsive ? "Not responding" : c->title,
        .icon=desk_shell_window_icon ? desk_shell_window_icon(d,i,16) : NULL,
        .active=((i==d->focus) != !!c->flash) && !c->unresponsive, .alert=c->unresponsive, .collapsed=c->collapsed, .maximized=c->zoomed,
        .buttons=(c->v3_style&DESK_V3_CONTEXTHELP) && (c->v3_style&3)==DESK_V3_OVERLAPPED ? DESK_TB_CLOSE|DESK_TB_HELP :
            d->shell && !c->owner_id && (c->v3_style&3)!=DESK_V3_TOOL ? DESK_TB_CLOSE|DESK_TB_MINIMIZE|(c->resizable ? DESK_TB_COLLAPSE : 0) : DESK_TB_CLOSE,
        .hot=hot, .pressed=hot && code==d->title_down && (d->buttons&1u) ? hot : 0};
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
    struct desk_frame_style s=frame_style(d,i);
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
         !strcmp(name,"control") || !strcmp(name,"ciukide") || !strcmp(name,"ciukide-app") || !strcmp(name,"terminal") || desk_catalog_find(name) ||
         desk_application_find(name); }
const char *desk_program_label(const char *name)
{
    const struct desk_catalog_entry *e=desk_catalog_find(name);
    const struct desk_application_entry *a=desk_application_find(name);
    return !strcmp(name,"demo") ? "Ciuki Demo" : !strcmp(name,"files") ? "Files" : !strcmp(name,"tasks") ? "Task Manager" :
           !strcmp(name,"terminal") ? "Terminal" : !strcmp(name,"ciuknote") ? "CiukNote" : !strcmp(name,"ciukide") ? "CiukIDE" : !strcmp(name,"control") ? "Control Panel" :
           e ? e->label : a ? a->label : name;
}
/* The application record's description ("Web browser"), or NULL. */
const char *desk_program_description(const char *name)
{ const struct desk_application_entry *a=desk_application_find(name); return a ? a->description : NULL; }
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
_Static_assert(DESK_CURSOR_ERASER+1==DESK_CURSOR_OPEN_HAND,"paint cursor inventory");
_Static_assert(sizeof(desk_extra_cursors)/sizeof(desk_extra_cursors[0])==DESK_CURSOR_COUNT-DESK_CURSOR_OPEN_HAND,"extra cursor inventory");
static struct desk_cursor_raster cursor_raster(const struct desktop *d)
{
    if (d->v3_cursor_id) {
        const struct desk_client *c=&d->clients[d->v3_cursor_slot]; unsigned id=d->v3_cursor_id-1;
        return (struct desk_cursor_raster){.hot_x=c->v3_hot_x[id],.hot_y=c->v3_hot_y[id],.width=32,.height=32};
    }
    unsigned shape=d->cursor_shape;
    const struct desk_cursor_raster *generic=desk_generic_cursor(shape);
    if (generic) return *generic;
    if (shape>=DESK_CURSOR_COUNT) return desk_cursors[DESK_CURSOR_ARROW];
    struct desk_tool_cursor tool=desk_tool_cursor(shape);
    struct desk_cursor_raster raster={.hot_x=(unsigned)tool.hot_x,.hot_y=(unsigned)tool.hot_y,.width=32,.height=32};
    memcpy(raster.mask,tool.mask,sizeof(raster.mask));
    memcpy(raster.image,tool.image,sizeof(raster.image));
    return raster;
}
static uint32_t cursor_background(void)
{ return desk_cursors[0].shadow ? 0 : 0x101010u; }
static uint32_t cursor_display_rgb(const struct desktop *d, uint32_t rgb)
{
    const struct ciuki_framebuffer_info *f=&d->command_format;
    const unsigned sizes[]={f->blue_size,f->green_size,f->red_size};
    if (f->generation!=d->accel_generation) return rgb;
    uint32_t out=0;
    for (unsigned i=0;i<3;i++) {
        unsigned n=sizes[i]; if (!n || n>8) return rgb;
        unsigned m=(1u<<n)-1, v=((rgb>>(i*8))&255)>>(8-n);
        out|=((255*v+m/2)/m)<<(i*8);
    }
    return out;
}
static unsigned pointer_scale(const struct desktop *d)
{ return d->shell && !d->v3_cursor_id && d->sh.cfg.pointer_scheme==1 ? 2u : 1u; }
static struct desk_box pointer_box(const struct desktop *d)
{
    struct desk_cursor_raster r=cursor_raster(d);
    unsigned scale=pointer_scale(d);
    return (struct desk_box){d->mouse_x-(int)(r.hot_x*scale),d->mouse_y-(int)(r.hot_y*scale),(int)(r.width*scale),(int)(r.height*scale)};
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
static void damage_visible(struct desktop *d, int index, struct desk_box clip)
{
    (void)index;
    /* Split only at actual opaque bodies. Unlike their bounding shadows,
     * these pixels do not depend on the foreign surface below them. */
    if(d->shell && desk_shell_occluders) {
        struct desk_box boxes[SH_LEVELS+6];unsigned n=desk_shell_occluders(d,boxes);
        struct desk_damage list={.count=1,.boxes={clip}};
        for(unsigned k=0;k<n;k++) {
            struct desk_damage next={0};
            for(unsigned j=0;j<list.count;j++) {
                struct desk_box a=list.boxes[j],b;
                if(!desk_intersect(a,boxes[k],&b)) { desk_damage_add(&next,a,d->width,d->height);continue; }
                struct desk_box parts[]={{a.x,a.y,a.w,b.y-a.y},{a.x,b.y+b.h,a.w,a.y+a.h-b.y-b.h},
                    {a.x,b.y,b.x-a.x,b.h},{b.x+b.w,b.y,a.x+a.w-b.x-b.w,b.h}};
                for(unsigned p=0;p<4;p++)desk_damage_add(&next,parts[p],d->width,d->height);
            }
            list=next;
        }
        for(unsigned i=0;i<list.count;i++)desk_damage_add(&d->damage,list.boxes[i],d->width,d->height);
        return;
    }
    desk_damage_add(&d->damage,clip,d->width,d->height);
}
/* DAMAGE describes a client's backing store, not an instruction to upload
 * the covering window again. Retained pixels are used when it is exposed.
 * Keep shadow/corner pixels: they depend on the surface underneath them. */
static int window_damage(struct desktop *d, int index, struct desk_box box)
{
    struct desk_client *c=&d->clients[index];
    /* fx-15: windows are painted over the top bar (scene() draws it first;
     * v3 windows and popups may be placed there) and under the taskbar
     * and its overlays (drawn after the windows). Updates in the top-bar
     * rows were dropped and stayed stale on screen. */
    if (!c->window || c->minimized ||
        !desk_intersect(box,(struct desk_box){0,0,(int)d->width,
            (int)d->height-(d->shell ? DESK_BAR : 0)},&box)) return 0;
    int windows[DESK_CLIENTS]; unsigned count=0;
    for (int i=0;i<DESK_CLIENTS;i++) if (i!=index && d->clients[i].window &&
        !d->clients[i].minimized && d->clients[i].order>c->order) windows[count++]=i;
    /* Determine visibility independently of the accumulated damage: a
     * synchronous PRESENT can be covered while another window is dirty. */
    struct desk_damage saved=d->damage;
    d->damage.count=0;
    visible_paint(d,box,windows,count,damage_visible,index);
    struct desk_damage visible=d->damage;
    d->damage=saved;
    for (unsigned i=0;i<visible.count;i++) desk_damage_add(&d->damage,visible.boxes[i],d->width,d->height);
    return visible.count!=0;
}
int desk_client_damage(struct desktop *d,int index,struct desk_box box)
{
    struct desk_client *c=&d->clients[index];
    if(c->collapsed || !desk_intersect(box,desk_content(c),&box))return 0;
    /* Legacy/full-surface presents can repeat an identical frame. Prove
     * equality against the completed opaque RAM scene, stopping at the
     * first changed row. Overlays, scaling and pending geometry retain the
     * ordinary conservative damage path. The saved scene under a software
     * pointer is read without changing its ownership or drawing code. */
    struct desk_box content=desk_content(c),ignored;
    int comparable=d->frame_valid && !d->damage.count && !d->move_damage.count && !d->copy_pending &&
        !d->present_damage.count && c->pixels && !c->gfxdemo &&
        !c->awaiting_frame && !c->launch_frame_pending &&
        content.w==(int)c->info.width && content.h==(int)c->info.height &&
        box.x>=0 && box.y>=0 && box.x+box.w<=(int)d->width && box.y+box.h<=(int)d->height;
    for(int i=0;i<DESK_CLIENTS && comparable;i++)if(i!=index && d->clients[i].window &&
        !d->clients[i].minimized && d->clients[i].order>c->order &&
        desk_intersect(box,desk_outer(&d->clients[i]),&ignored))comparable=0;
    if(comparable && d->shell) {
        struct desk_box boxes[SH_LEVELS+6];unsigned n=desk_shell_occluders ? desk_shell_occluders(d,boxes) : 0;
        for(unsigned i=0;i<n;i++) {
            struct desk_box b=boxes[i];b.x-=6;b.y-=16;b.w+=12;b.h+=32;
            if(desk_intersect(box,b,&ignored))comparable=0;
        }
        if(desk_shell_chrome_intersects(d,box))comparable=0;
    }
    if(comparable) {
        int same=1;
        for(int y=box.y;y<box.y+box.h && same;y++) {
            const uint32_t *src=c->pixels+(size_t)(y-content.y)*c->info.width+(unsigned)(box.x-content.x);
            const uint32_t *old=d->pixels+(size_t)y*d->width+(unsigned)box.x;
            struct desk_box pointer;
            int covered=d->cursor_valid && desk_intersect((struct desk_box){box.x,y,box.w,1},d->cursor_box,&pointer);
            if(!covered && !memcmp(src,old,(size_t)box.w*4))continue;
            for(int x=0;x<box.w;x++) {
                d->stats.compare_px++;
                uint32_t previous=covered && box.x+x>=pointer.x && box.x+x<pointer.x+pointer.w ?
                    d->cursor_under[(y-d->cursor_box.y)*64+box.x+x-d->cursor_box.x] : old[x];
                if((src[x]&0xffffffu)!=previous) { same=0;break; }
            }
        }
        if(same)return 0;
    }
    return window_damage(d,index,box);
}
/* fx-2, the Windows 9x SetWindowPos rule: a change of stacking order
 * invalidates what the change uncovers. Every path that reorders windows
 * (focus raise, v3 restack of owned/topmost/internal bands, Alt+Tab, v3
 * window order) is reconciled here against the order the screen shows, so
 * a window that became higher is repainted above its new lower neighbours
 * before any of its own updates can reach those pixels. */
void desk_stack_damage(struct desktop *d)
{
    for (int i=0;i<DESK_CLIENTS;i++) {
        const struct desk_client *a=&d->clients[i];
        if (!a->window || a->minimized || !a->window_id || d->stack_window[i]!=a->window_id) continue;
        for (int j=i+1;j<DESK_CLIENTS;j++) {
            const struct desk_client *b=&d->clients[j]; struct desk_box overlap;
            if (!b->window || b->minimized || !b->window_id || d->stack_window[j]!=b->window_id) continue;
            if ((d->stack_order[i]<d->stack_order[j])==(a->order<b->order)) continue;
            if (desk_intersect(desk_outer(a),desk_outer(b),&overlap))
                desk_damage_add(&d->damage,overlap,d->width,d->height);
        }
    }
    for (int i=0;i<DESK_CLIENTS;i++) {
        const struct desk_client *c=&d->clients[i];
        d->stack_window[i]=c->window && !c->minimized ? c->window_id : 0;
        d->stack_order[i]=c->order;
    }
}
/* fx-15: the Windows 9x invalidation rules, applied to the state the last
 * composed frame showed rather than to each call site (the effect of
 * EndDeferWindowPos for every change of the frame). A window that appears,
 * disappears (close, crash, minimise, hide) or changes its outer box
 * invalidates its old and new outer boxes; a caption whose text, icon,
 * activation, alert, shade, zoom or buttons change is repainted (frame
 * edges with it, the content of a built-in window whose focus indicators
 * follow its activation); a hovered or pressed title button repaints only
 * itself. A move the compositor is copying (copy_pending) is already
 * described by its move damage. Runs before every frame decision. */
static void caption_parts(struct desktop *d, struct desk_box f)
{
    struct desk_box parts[4]={{f.x,f.y,f.w,DESK_TITLE},
        {f.x,f.y+DESK_TITLE,DESK_BORDER,f.h-DESK_TITLE},
        {f.x+f.w-DESK_BORDER,f.y+DESK_TITLE,DESK_BORDER,f.h-DESK_TITLE},
        {f.x,f.y+f.h-DESK_BORDER,f.w,DESK_BORDER}};
    for (unsigned k=0;k<4;k++) desk_damage_add(&d->damage,parts[k],d->width,d->height);
}
extern void desk_shell_reconcile(struct desktop *) __attribute__((weak));
void desk_scene_reconcile(struct desktop *d)
{
    for (int i=0;i<DESK_CLIENTS;i++) {
        struct desk_client *c=&d->clients[i];
        struct desk_shown *s=&d->shown[i];
        int shown=c->window && !c->minimized;
        struct desk_box outer=shown ? desk_outer(c) : (struct desk_box){0,0,0,0};
        int moving=d->copy_pending && d->copy_client==i, framed=shown && (c->v3_style&3)!=DESK_V3_POPUP;
        struct desk_frame_style st=framed ? frame_style(d,i) : (struct desk_frame_style){0};
        if (s->window_id!=c->window_id || s->shown!=shown ||
            (shown && !moving && memcmp(&s->outer,&outer,sizeof(outer)))) {
            if (s->shown) desk_damage_add(&d->damage,s->outer,d->width,d->height);
            if (shown) desk_damage_add(&d->damage,outer,d->width,d->height);
        } else if (framed) {
            int whole=strcmp(s->title,st.title ? st.title : "") || s->icon!=st.icon || s->buttons!=st.buttons ||
                s->active!=st.active || s->alert!=st.alert || s->collapsed!=st.collapsed || s->maximized!=st.maximized;
            if (whole) {
                caption_parts(d,c->frame);
                if (c->internal && s->active!=st.active)
                    desk_damage_add(&d->damage,desk_content(c),d->width,d->height);
            } else if (s->hot!=st.hot || s->pressed!=st.pressed) {
                unsigned buttons=s->hot|st.hot|s->pressed|st.pressed;
                for (unsigned b=DESK_TB_CLOSE;b<=DESK_TB_HELP;b<<=1) if (buttons&b)
                    desk_damage_add(&d->damage,title_box(c,b),d->width,d->height);
            }
        }
        s->window_id=c->window_id; s->shown=shown; s->outer=outer;
        if (framed) {
            snprintf(s->title,sizeof(s->title),"%s",st.title ? st.title : "");
            s->icon=st.icon; s->buttons=st.buttons; s->hot=st.hot; s->pressed=st.pressed;
            s->active=st.active; s->alert=st.alert; s->collapsed=st.collapsed; s->maximized=st.maximized;
        } else { s->title[0]=0; s->icon=NULL; s->buttons=s->hot=s->pressed=0; s->active=s->alert=s->collapsed=s->maximized=0; }
    }
    desk_stack_damage(d);
    if (d->shell && desk_shell_reconcile) desk_shell_reconcile(d);
}
void desk_caption_damage(struct desktop *d,int index)
{
    struct desk_box b=d->clients[index].frame;b.h=DESK_TITLE;
    window_damage(d,index,b);
    if(d->shell)desk_damage_add(&d->damage,desk_task_button(d,index),d->width,d->height);
}
static void visible_window(struct desktop *d, int index, struct desk_box clip)
{ DESK_PROFILE("windows", window(d,index,clip)); }
static void cursor(struct desktop *, struct desk_box);
static void saver_scene(struct desktop *d, struct desk_box clip)
{
    fill(d,clip,clip,0);
    uint64_t frame=d->now_ns/50000000ull;
    if (d->saver_kind==1 && d->portrait) {
        unsigned size=d->width<256 ? d->width : 256;
        if (size>d->height) size=d->height;
        unsigned wx=d->width-size, hy=d->height-size;
        unsigned x=wx ? (unsigned)(frame%((uint64_t)wx*2)) : 0;
        unsigned y=hy ? (unsigned)(frame%((uint64_t)hy*2)) : 0;
        if (x>wx) x=wx*2-x;
        if (y>hy) y=hy*2-y;
        /* Nearest-neighbour size conversion of the pinned approved raster. */
        struct desk_box b;
        if (size && desk_intersect((struct desk_box){(int)x,(int)y,(int)size,(int)size},clip,&b))
            for (int yy=b.y;yy<b.y+b.h;yy++) for (int xx=b.x;xx<b.x+b.w;xx++)
                d->pixels[(size_t)yy*d->width+xx]=d->portrait[((unsigned)yy-y)*DESK_PORTRAIT_SIZE/size*DESK_PORTRAIT_SIZE+
                    ((unsigned)xx-x)*DESK_PORTRAIT_SIZE/size];
    } else if (d->saver_kind==2 && d->width && d->height) {
        /* Fixed seeds, bounded integer projection; no allocations or FPU. */
        for (unsigned i=0;i<96;i++) {
            unsigned z=1+(unsigned)((i*53u+255u-(unsigned)(frame%255))%255);
            int x=(int)d->width/2+((int)(i*7919u%2049)-1024)*128/(int)z;
            int y=(int)d->height/2+((int)(i*3571u%1537)-768)*128/(int)z;
            unsigned c=255-z/2;
            fill(d,clip,(struct desk_box){x,y,z<64 ? 2 : 1,z<64 ? 2 : 1},c*0x010101u);
        }
    }
}
static void scene(struct desktop *d, struct desk_box clip)
{
    if (d->saver_active) { saver_scene(d,clip); return; }
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
    if(d->shell && d->sh.cfg.mouse_trails>1 && !d->v3_cursor_id && !d->v3_cursor_hidden) {
        int x=d->mouse_x,y=d->mouse_y;
        for(unsigned k=d->pointer_trail_count;k;k--) {
            d->mouse_x=d->pointer_trail[k-1].x;d->mouse_y=d->pointer_trail[k-1].y;
            cursor(d,clip);
        }
        d->mouse_x=x;d->mouse_y=y;
    }
    if (d->shell && desk_shell_start) DESK_PROFILE("bars_menus", desk_shell_start(d,clip));
}
static void cursor(struct desktop *d, struct desk_box clip)
{
    struct desk_box b;
    struct desk_box box=pointer_box(d);
    struct desk_cursor_raster r=cursor_raster(d);
    if (d->saver_active || d->hw_cursor || d->v3_cursor_hidden || !desk_intersect(box,clip,&b)) return;
    unsigned scale=pointer_scale(d);
    int inverted=d->shell && !d->v3_cursor_id && d->sh.cfg.pointer_scheme==2;
    if (!d->v3_cursor_id && scale==1 && !inverted && !r.shadow && du_commands) {
        uint8_t planes[256]={0}; unsigned stride=(r.width+7)/8, extent=stride*r.height;
        for (unsigned y=0;y<r.height;y++) for (unsigned x=0;x<r.width;x++) {
            unsigned bit=0x80000000u>>x;
            if (r.mask[y]&bit) planes[(r.image[y]&bit ? 0 : extent)+y*stride+x/8]|=128u>>(x%8);
        }
        struct ciuki_batch_record records[2];
        for (unsigned i=0;i<2;i++) records[i]=(struct ciuki_batch_record){.opcode=CIUKI_CMD_MONO,.record_bytes=128,
            .flags=CIUKI_CMD_TRANSPARENT_BG,.dx=box.x,.dy=box.y,.dw=r.width,.dh=r.height,.sw=r.width,.sh=r.height,
            .foreground=i ? cursor_background() : 0xffffffu,.data_offset=i*extent,.data_bytes=extent,.stride=stride};
        struct du_canvas canvas={d->pixels,(int)d->width,(int)d->height,{b.x,b.y,b.w,b.h}};
        if (!du_commands(&canvas,records,2,planes,NULL,0,0)) return;
    }
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
            unsigned column=(unsigned)(b.x+x-box.x)/scale;
            uint32_t bit=0x80000000u>>column;
            unsigned row=(unsigned)(b.y+y-box.y)/scale;
            if (r.mask[row]&bit) {
                if(inverted)dst[x]^=0xffffffu;
                else dst[x]=r.image[row]&bit ? 0xffffffu : cursor_background();
            }
            else if (r.shadow) {
                unsigned alpha=r.shadow[row*32+column], inv=255-alpha;
                if (!alpha) continue;
                uint32_t under=cursor_display_rgb(d,dst[x]);
                unsigned red=(((under>>16)&255)*inv+127)/255;
                unsigned green=(((under>>8)&255)*inv+127)/255;
                unsigned blue=((under&255)*inv+127)/255;
                dst[x]=(red<<16)|(green<<8)|blue;
            }
        }
    }
}
/* f2-99: a stationary software pointer only touches scene damage. Row bits
 * union overlapping rectangles before alpha blending, so a shadow is never
 * blended twice. A moved/changed pointer or a RAM copy restores the entire
 * old footprint before the copy and draws the entire new one afterwards. */
static void pointer_rows(struct desk_box box, int full, const pixman_region32_t *damage,
                         const pixman_region32_t *move, uint64_t rows[64])
{
    memset(rows,0,64*sizeof(*rows));
    if (full) {
        uint64_t bits=UINT64_MAX>>(64-box.w);
        for (int y=0;y<box.h;y++) rows[y]=bits;
        return;
    }
    const pixman_region32_t *regions[]={damage,move};
    for (unsigned k=0;k<2;k++) {
        int count; const pixman_box32_t *rects=pixman_region32_rectangles(regions[k],&count);
        for (int i=0;i<count;i++) {
            struct desk_box b;
            if (!desk_intersect(box,box_region(rects[i]),&b)) continue;
            uint64_t bits=(UINT64_MAX>>(64-b.w))<<(b.x-box.x);
            for (int y=b.y-box.y;y<b.y-box.y+b.h;y++) rows[y]|=bits;
        }
    }
}
static void pointer_under(struct desktop *d, struct desk_box box, const uint64_t rows[64], int save)
{
    for (int y=0;y<box.h;y++) {
        uint64_t bits=rows[y];
        for (int x=0;x<box.w;) {
            if (!(bits&(1ull<<x))) { x++; continue; }
            int left=x;
            do { x++; } while (x<box.w && (bits&(1ull<<x)));
            uint32_t *dst=d->pixels+(size_t)(box.y+y)*d->width+box.x+left;
            uint32_t *under=d->cursor_under+y*64+left;
            size_t bytes=(size_t)(x-left)*4;
            if (save) {
                memcpy(under,dst,bytes);
                DESK_PROFILE("cursor",cursor(d,((struct desk_box){box.x+left,box.y+y,x-left,1})));
            } else memcpy(dst,under,bytes);
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
    free(d->command_old_pixels); d->command_old_pixels=NULL;
    free(d->render_cache); d->render_cache=NULL; d->render_cache_prepared=0;
    d->scanout_valid=d->frame_valid=0;
    d->copy_pending=0; d->move_damage.count=0;
    free(d->sh.background); d->sh.background=NULL; d->sh.background_valid=0;
    d->cursor_valid=0; d->cursor_dirty=1; d->present_damage.count=0;
    for (unsigned i=0;i<DESK_VRAM_ENTRIES;i++) { free(d->vram[i].shadow); memset(&d->vram[i],0,sizeof(d->vram[i])); }
    d->vram_first=d->vram_end=0;
}
/* f2-95: continuous small damage without user input (an application's
 * clock, position label, meter, progress bar or throbber) is composed at
 * most every DESK_SMALL_FRAME_NS. Input within DESK_INPUT_ACTIVE_NS, the
 * pointer, window moves, launches, frame-paced presenters (gfx demo, DOS
 * windows through DESK_GFX_FRAME) and damage over a sixteenth of the screen
 * keep the display rate; an isolated update is never delayed (the cap only
 * spaces a frame from the previous completed one). */
int desk_small_damage(const struct desktop *d, uint64_t now)
{
    if (d->cursor_dirty || d->move_damage.count || d->present_damage.count || d->copy_pending ||
        d->frame_input_ns || (d->last_input_ns && now-d->last_input_ns<DESK_INPUT_ACTIVE_NS) || !d->damage.count)
        return 0;
    uint64_t area=0;
    for (unsigned i=0;i<d->damage.count;i++) area+=(uint64_t)d->damage.boxes[i].w*(uint64_t)d->damage.boxes[i].h;
    if (area*16>(uint64_t)d->width*d->height) return 0;
    for (int i=0;i<DESK_CLIENTS;i++) {
        const struct desk_client *c=&d->clients[i];
        if (c->gfx_pending || c->gl_present_request || c->awaiting_frame || c->launch_frame_pending ||
            (c->dosbox && c->window && !c->minimized && !c->collapsed)) return 0;
    }
    return 1;
}
uint64_t desk_frame_time(const struct desktop *d, uint64_t now)
{
    uint64_t due=d->next_frame_ns;
    if (d->last_frame_ns && desk_small_damage(d,now) && d->last_frame_ns+DESK_SMALL_FRAME_NS>due)
        due=d->last_frame_ns+DESK_SMALL_FRAME_NS;
    return due;
}
int desk_frame_due(const struct desktop *d, uint64_t now)
{
    return (d->damage.count || d->move_damage.count || d->present_damage.count || d->cursor_dirty) &&
        now>=desk_frame_time(d,now);
}
static uint64_t time_now(struct desktop *d) { return d->time_ns ? d->time_ns() : 0; }
static int accel_call(struct desktop *d, struct ciuki_accel_request *q)
{
    q->size=sizeof(*q); q->generation=d->accel_generation;
    return d->accel ? d->accel(d->display,q) : -ENOSYS;
}
/* f2-96: answers that leave the engine and its cursor exactly as they were
 * (fbdev: a mode change or another presenter owns the display, a stale
 * generation, a caught signal). The request is repeated, never treated as
 * an engine loss. */
static int accel_retry(int error) { return error==-EBUSY || error==-EAGAIN || error==-EINTR; }
/* QUERY; a busy or stale answer keeps the capabilities already known. */
static uint32_t accel_query(struct desktop *d)
{
    struct ciuki_accel_request q={.operation=CIUKI_ACCEL_QUERY};
    int caps=accel_call(d,&q);
    if (accel_retry(caps)) return d->accel_caps;
    return caps>0 ? (uint32_t)caps : 0;
}
static void command_query(struct desktop *d)
{
    if (!d->commands || !d->batch || !d->fence) return;
    struct ciuki_command_info info;
    int e=d->commands(d->display,d->accel_generation,&info);
    if (accel_retry(e)) return;
    d->command_caps=!e && info.version==1 && info.size==sizeof(info) ? info.capabilities : 0;
    d->command_native=!e ? info.native_commands : 0;
    d->colour_cursor_width=!e ? info.cursor_width : 0;
    d->colour_cursor_height=!e ? info.cursor_height : 0;
    if (!e && d->display_info && d->command_format.generation!=d->accel_generation) {
        struct ciuki_framebuffer_info format;
        if (!d->display_info(d->display,&format)) d->command_format=format;
    }
}
static int pointer_capable(const struct desktop *d)
{ return !(d->shell && (d->sh.cfg.pointer_scheme || d->sh.cfg.mouse_trails>1)) &&
    ((d->accel_caps&2u) || (d->command_caps&CIUKI_CAP_CURSOR_ARGB)); }
/* No caller retries a failed admitted batch. The authoritative RAM scene is
 * reconstructed after CPU safety, and unsafe fences suppress presentation. */
static int command_submit_targets(struct desktop *d, const struct ciuki_batch_record *records, unsigned count,
                           const void *payload, unsigned bytes, int video)
{
    unsigned targets=video ? 2 : 1;
    unsigned prefix=64+128*count+32*targets, padded=(bytes+7)&~7u, total=prefix+padded;
    if (!d->batch || !d->fence || d->recovering_fence || count>64 || total>CIUKI_BATCH_MAX_BYTES) return -EOPNOTSUPP;
    uint8_t *blob=calloc(1,total); if (!blob) return -ENOMEM;
    struct ciuki_batch_header *h=(void *)blob;
    *h=(struct ciuki_batch_header){CIUKI_BATCH_MAGIC,1,64,total,count,64,64+128*count,targets,prefix,padded,0,d->accel_generation,{0}};
    struct ciuki_batch_record *out=(void *)(blob+64);
    for (unsigned i=0;i<count;i++) {
        out[i]=records[i]; out[i].record_bytes=128;
        if (out[i].opcode!=CIUKI_CMD_SYNC && !out[i].dst) out[i].dst=1;
        if (out[i].data_bytes) out[i].data_offset+=prefix;
        if (out[i].clip_count) out[i].clip_offset+=prefix;
    }
    struct ciuki_batch_target target={.kind=CIUKI_TARGET_SCANOUT,.generation=d->accel_generation};
    memcpy(blob+h->targets_offset,&target,32);
    if (video) {
        target.kind=CIUKI_TARGET_VIDEO; target.handle=(uint32_t)d->display;
        memcpy(blob+h->targets_offset+32,&target,32);
    }
    if (bytes) memcpy(blob+prefix,payload,bytes);
    struct ciuki_batch_reply reply={0};
    int e=d->batch(d->display,blob,total,&reply);
    free(blob);
    if (reply.fence) {
        if (!(reply.flags&CIUKI_FENCE_CPU_SAFE)) d->recovering_fence=reply.fence;
        else {
            struct ciuki_batch_reply released;
            (void)d->fence(d->display,reply.fence,CIUKI_FENCE_RELEASE,0,&released);
        }
    }
    return e;
}
static int command_submit(struct desktop *d, const struct ciuki_batch_record *records, unsigned count,
                           const void *payload, unsigned bytes)
{ return command_submit_targets(d,records,count,payload,bytes,0); }
/* f2-96: one pointer on screen. CURSOR_SET hides the hardware cursor before
 * loading the shape (native cursor_set), so a loaded cursor is hidden
 * until the frame's MOVE and SHOW. */
static int pointer_load(struct desktop *d)
{
    struct ciuki_accel_request q={.operation=CIUKI_ACCEL_CURSOR_SET,.colour=0xffffff,.background=cursor_background()};
    struct desk_cursor_raster r=cursor_raster(d);
    if ((d->command_caps&CIUKI_CAP_CURSOR_ARGB) && r.width<=d->colour_cursor_width && r.height<=d->colour_cursor_height) {
        uint32_t *argb=calloc((size_t)r.width*r.height,4); if (!argb) return -ENOMEM;
        for (unsigned y=0;y<r.height;y++) for (unsigned x=0;x<r.width;x++) {
            uint32_t bit=0x80000000u>>x;
            if (r.mask[y]&bit) argb[y*r.width+x]=r.image[y]&bit ? 0xffffffffu : 0xff000000u|cursor_background();
            else if (r.shadow) argb[y*r.width+x]=(uint32_t)r.shadow[y*32+x]<<24;
        }
        struct ciuki_batch_record record={.opcode=CIUKI_CMD_CURSOR_ARGB_SET,.dw=r.width,.dh=r.height,
            .origin_x=(int)r.hot_x,.origin_y=(int)r.hot_y,.stride=r.width*4,.data_bytes=r.width*r.height*4};
        int error=command_submit(d,&record,1,argb,record.data_bytes); free(argb);
        if (!error) { d->hw_colour_cursor=1; d->hw_cursor=1; d->hw_cursor_shown=0; d->cursor_loaded=d->cursor_shape; d->cursor_dirty=1; }
        return error;
    }
    memcpy(q.cursor_mask,r.mask,sizeof(q.cursor_mask)); memcpy(q.cursor_image,r.image,sizeof(q.cursor_image));
    int error=accel_call(d,&q);
    if (!error) { d->hw_colour_cursor=0; d->hw_cursor=1; d->hw_cursor_shown=0; d->cursor_loaded=d->cursor_shape; d->cursor_dirty=1; }
    return error;
}
/* Hide a hardware cursor that no longer owns the pointer. A refusal other
 * than busy/stale means the engine is gone, and the kernel hides the cursor
 * whenever it stops or quarantines the engine (native stop/fault). */
static void pointer_hide(struct desktop *d)
{
    if (d->hw_cursor || !d->hw_cursor_shown) return;
    struct ciuki_accel_request q={.operation=CIUKI_ACCEL_CURSOR_SHOW,.colour=0};
    int error=d->hw_colour_cursor ? 0 : accel_call(d,&q);
    if (d->hw_colour_cursor) {
        struct ciuki_batch_record record={.opcode=CIUKI_CMD_CURSOR_SHOW};
        error=command_submit(d,&record,1,NULL,0);
    }
    if (d->recovering_fence) return;
    if (!error || !accel_retry(error)) d->hw_cursor_shown=0;
    else d->hw_cursor_retries++;
    d->cursor_dirty=1; /* draw the software pointer, or ask again next frame */
}
/* A cursor request failed for good: the software pointer takes over after
 * the hardware cursor is hidden; the engine is offered it again later. */
static void pointer_software(struct desktop *d, uint64_t now)
{
    d->hw_cursor=0; d->cursor_dirty=1; d->cursor_loaded=DESK_CURSOR_COUNT;
    d->hw_cursor_fallbacks++; d->hw_cursor_retry_ns=now+DESK_POINTER_RETRY_NS;
    pointer_hide(d);
}
/* f2-92: offscreen caches. Video memory below the scanout holds unchanged
 * surfaces; uploads go through the engine's host-to-screen write and each
 * use is an engine copy, after an exact comparison with the RAM frame. */
#define DESK_VRAM_MIN_W 64
#define DESK_VRAM_STEP_ROWS 16u /* about 64 KiB per idle turn at 1024 pixels */
#define DESK_VRAM_CHECK_NS 500000000ull
static void vram_free(struct desk_vram_entry *e)
{
    free(e->shadow);
    memset(e,0,sizeof(*e));
}
static void vram_query(struct desktop *d)
{
    d->vram_queries++;
    struct ciuki_accel_request q={.operation=CIUKI_ACCEL_OFFSCREEN};
    int rows=accel_call(d,&q);
    uint32_t first=rows>0 ? (uint32_t)rows&0xffffu : 0, end=rows>0 ? (uint32_t)rows>>16 : 0;
    if (end>first && first>=d->height) { d->vram_first=first; d->vram_end=end; }
    d->command_video_valid=0;
    if (end>first && first>=d->height && (d->command_caps&CIUKI_CAP_KEYED) &&
        (d->command_native&(1u<<CIUKI_CMD_WRITE))) {
        d->command_video_row=end-first-1; d->command_video_valid=1;
        d->vram_end=end-1; /* reserve one row, disjoint from legacy caches */
    }
}
static void vram_reset(struct desktop *d)
{
    for (unsigned i=0;i<DESK_VRAM_ENTRIES;i++) vram_free(&d->vram[i]);
    d->command_video_valid=0;
    d->vram_first=d->vram_end=0; d->vram_check_ns=d->vram_query_ns=0; d->vram_queries=d->vram_failures=0;
    if (d->accel && (d->accel_caps&1u)) vram_query(d);
}
void desk_accel_reset(struct desktop *d, uint32_t generation)
{
    d->scanout_valid=0; d->move_damage.count=0;
    d->hw_cursor=0; d->cursor_dirty=1; d->copy_pending=0;
    /* f2-96: a new generation follows a stopped engine (fbdev_change_begin)
     * or a display lease (display_present), both of which hide the cursor. */
    if (generation!=d->accel_generation) d->hw_cursor_shown=0;
    d->accel_generation=generation;
    command_query(d);
    struct ciuki_accel_request q={.operation=CIUKI_ACCEL_QUERY};
    int caps=accel_call(d,&q);
    d->accel_caps=caps>0 ? (uint32_t)caps : 0;
    if (!(d->accel_caps&2u) && !(d->command_caps&CIUKI_CAP_CURSOR_ARGB) && !accel_retry(caps)) d->hw_cursor_shown=0;
    vram_reset(d);
    d->cursor_loaded=DESK_CURSOR_COUNT;
    if (pointer_capable(d) && !d->v3_cursor_id && !d->v3_cursor_hidden) (void)pointer_load(d);
    desk_damage_add(&d->damage,(struct desk_box){0,0,(int32_t)d->width,(int32_t)d->height},d->width,d->height);
}
void desk_cursor_motion(struct desktop *d)
{
    d->cursor_dirty=1;
}
static void ram_copy(struct desktop *d, const struct ciuki_rect *r)
{
    d->frame_stats.copy_bytes+=(uint64_t)r->width*r->height*4;
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
        d->frame_stats.accel_fallback++;
        d->scanout_valid=0;
        d->accel_caps=accel_query(d);
        if (!pointer_capable(d)) d->hw_cursor=d->hw_cursor_shown=0; /* engine loss hid it */
        desk_damage_add(&d->damage,(struct desk_box){0,0,(int32_t)d->width,(int32_t)d->height},d->width,d->height);
        return -1;
    }
    d->presents++; d->frame_stats.presents++; d->frame_stats.accel_copies++;
    return 0;
}
static int region_cut(pixman_region32_t *region, struct desk_box b)
{
    if (b.w<=0 || b.h<=0) return 1;
    pixman_region32_t cut;
    pixman_region32_init_rect(&cut,b.x,b.y,(unsigned)b.w,(unsigned)b.h);
    int ok=pixman_region32_subtract(region,region,&cut);
    pixman_region32_fini(&cut);
    return ok;
}
/* Opaque frame interiors only; shadows and rounded corners still need
 * the retained surfaces below them. dx/dy translate a source clip into
 * destination coordinates. */
static int region_uncovered(struct desktop *d,pixman_region32_t *region,int index,int dx,int dy)
{
    for (int i=0;i<DESK_CLIENTS;i++) {
        struct desk_client *c=&d->clients[i];
        if (i==index || !c->window || c->minimized || c->order<=d->clients[index].order ||
            (!c->internal && !c->collapsed && !c->pixels)) continue;
        struct desk_box f=c->frame; f.x+=dx;f.y+=dy;
        int radius=desk_theme->radius;
        if (!region_cut(region,(struct desk_box){f.x+radius+1,f.y,f.w-2*(radius+1),radius}) ||
            !region_cut(region,(struct desk_box){f.x,f.y+radius,f.w,f.h-radius})) return 0;
    }
    if(d->shell && desk_shell_occluders) {
        struct desk_box boxes[SH_LEVELS+6];unsigned n=desk_shell_occluders(d,boxes);
        for(unsigned i=0;i<n;i++) {
            struct desk_box b=boxes[i];b.x+=dx;b.y+=dy;
            if(!region_cut(region,b))return 0;
        }
    }
    return 1;
}
static int copy_cut(pixman_region32_t *region,const struct ciuki_rect *r,struct desk_box b)
{
    struct desk_box source=b;source.x+=r->dst_x-r->src_x;source.y+=r->dst_y-r->src_y;
    return region_cut(region,b) && region_cut(region,source);
}
static int copy_region(struct desktop *d,pixman_region32_t *region,const struct ciuki_rect *r,int index)
{
    pixman_region32_init_rect(region,r->dst_x,r->dst_y,r->width,r->height);
    if(d->pointer_trail_count) {pixman_region32_clear(region);return 0;}
    for (int i=0;i<DESK_CLIENTS;i++) {
        struct desk_client *c=&d->clients[i];
        if (i==index || !c->window || c->minimized || c->order<=d->clients[index].order) continue;
        /* A higher shadow is translucent: recompute it rather than moving
         * its old blend along with the window underneath. */
        if (!copy_cut(region,r,desk_outer(c))) {
            pixman_region32_clear(region);return 0;
        }
    }
    /* fx-15: everything scene() paints above the windows: the taskbar and
     * its overlays, the zoom/minimise outline, the Start orb, notifications.
     * A copy may neither carry those pixels away nor cover them. */
    if (d->animation.active) {
        struct desk_box a=d->animation.box;
        struct desk_box edges[4]={{a.x,a.y,a.w,3},{a.x,a.y+a.h-3,a.w,3},{a.x,a.y,3,a.h},{a.x+a.w-3,a.y,3,a.h}};
        for (unsigned k=0;k<4;k++) if (!copy_cut(region,r,edges[k])) { pixman_region32_clear(region);return 0; }
    }
    if(d->shell) {
        struct desk_box boxes[SH_LEVELS+6];unsigned n=desk_shell_occluders ? desk_shell_occluders(d,boxes) : 0;
        if(!copy_cut(region,r,desk_taskbar(d))) { pixman_region32_clear(region);return 0; }
        for(unsigned i=0;i<n;i++) {
            struct desk_box b=boxes[i];
            if(desk_theme->shadow_size) { b.x-=6;b.y-=6;b.w+=12;b.h+=12; }
            if(!copy_cut(region,r,b)) { pixman_region32_clear(region);return 0; }
        }
        struct desk_box notice=desk_shell_notification_bounds ? desk_shell_notification_bounds(d) : (struct desk_box){0};
        if(d->sh.dnd.active && d->sh.dnd.image.w && !copy_cut(region,r,d->sh.dnd.image)) { pixman_region32_clear(region);return 0; }
        if(!copy_cut(region,r,desk_start_bounds(d)) ||
           (d->sh.notification.active && !copy_cut(region,r,notice))) {
            pixman_region32_clear(region);return 0;
        }
    }
    return pixman_region32_not_empty(region);
}
/* Banded regions are sorted top-to-bottom, left-to-right. Reverse them
 * when moving down/right so no rectangle overwrites another one's source. */
static int copy_each(struct desktop *d,const pixman_region32_t *region,const struct ciuki_rect *copy,int engine)
{
    int n;const pixman_box32_t *boxes=pixman_region32_rectangles(region,&n);
    int down=copy->dst_y>copy->src_y, right=copy->dst_x>copy->src_x;
    for (int done=0;done<n;) {
        int first=down ? n-done-1 : done, last=first+1;
        if (down) { while (first && boxes[first-1].y1==boxes[last-1].y1) first--; }
        else { while (last<n && boxes[last].y1==boxes[first].y1) last++; }
        for (int k=first;k<last;k++) {
            struct desk_box b=box_region(boxes[right ? last-1-(k-first) : k]);
            struct ciuki_rect r={b.x+copy->src_x-copy->dst_x,b.y+copy->src_y-copy->dst_y,
                b.x,b.y,(uint32_t)b.w,(uint32_t)b.h};
            if (engine) { if (screen_copy(d,&r)) return -1; }
            else ram_copy(d,&r);
        }
        done+=last-first;
    }
    return 0;
}
/* The shell background is the cached image itself; a window has its own
 * shadow (the masked client pixels that were uploaded). */
static const uint32_t *vram_source(struct desktop *d, const struct desk_vram_entry *e, uint32_t *stride)
{
    if (e->kind==DESK_VRAM_BACKGROUND) {
        if (!d->shell || !d->sh.background || !d->sh.background_valid || e->serial!=d->sh.background_serial ||
            e->width!=d->width || e->height!=d->height) return NULL;
        *stride=d->width; return d->sh.background;
    }
    *stride=e->width; return e->shadow;
}
/* A cached window: an ordinary, unscaled application surface whose composed
 * content is exactly its pixels without the X byte (client_rows). */
static int vram_window_ok(const struct desktop *d, const struct desk_client *c)
{
    struct desk_box b=desk_content(c);
    return c->window && !c->minimized && !c->collapsed && !c->internal && c->pixels &&
        (c->v3_style&3)!=DESK_V3_POPUP && !c->gfx_width && !c->gfx_height &&
        b.w==(int)c->info.width && b.h==(int)c->info.height && b.w>=DESK_VRAM_MIN_W && b.h>0 &&
        (uint32_t)b.w<=d->width;
}
/* Row y of box b served by one cache: entry index, source x and row. */
static int vram_lookup(struct desktop *d, struct desk_box b, int y, int32_t *sx, int32_t *sy)
{
    if (!d->vram_end || b.w<DESK_VRAM_MIN_W) return -1;
    const uint32_t *row=d->pixels+(size_t)y*d->width+b.x;
    for (int i=0;i<DESK_VRAM_ENTRIES;i++) {
        struct desk_vram_entry *e=&d->vram[i];
        uint32_t stride; const uint32_t *src;
        if (!e->valid || !(src=vram_source(d,e,&stride))) continue;
        int32_t ox=0, oy=0;
        if (e->kind==DESK_VRAM_WINDOW) {
            const struct desk_client *c=&d->clients[e->client];
            if (!vram_window_ok(d,c) || c->info.width!=e->width || c->info.height!=e->height) continue;
            struct desk_box cb=desk_content(c);
            ox=cb.x; oy=cb.y;
        }
        if (y<oy || y>=oy+(int)e->height || b.x<ox || b.x+b.w>ox+(int)e->width) continue;
        if (memcmp(row,src+(size_t)(y-oy)*stride+(b.x-ox),(size_t)b.w*4)) continue;
        *sx=b.x-ox; *sy=(int32_t)e->row+(y-oy);
        return i;
    }
    return -1;
}
static int vram_write(struct desktop *d, const uint32_t *src, uint32_t stride, uint32_t width, uint32_t height,
                      uint32_t first, uint32_t rows, uint32_t dst_row)
{
    struct ciuki_accel_request q={.operation=CIUKI_ACCEL_WRITE,
        .rect={0,(int32_t)first,0,(int32_t)dst_row,width,rows}};
    uintptr_t at=(uintptr_t)src;
    q.write.pixels_lo=(uint32_t)at; q.write.pixels_hi=(uint32_t)((uint64_t)at>>32);
    q.write.stride=stride*4u; q.write.source_width=width; q.write.source_height=height;
    int e=accel_call(d,&q);
    if (e) {
        /* Three failed uploads in a row end the caches until the next
         * accelerator reset; a quarantine also clears the capability. */
        d->stats.accel_fallback++;
        if (++d->vram_failures>=3) {
            for (unsigned i=0;i<DESK_VRAM_ENTRIES;i++) vram_free(&d->vram[i]);
            d->vram_first=d->vram_end=0;
        }
        return -1;
    }
    d->vram_failures=0; d->stats.accel_writes++;
    return 0;
}
/* Upload the next rows of an entry; a changed source restarts it. */
static int vram_upload(struct desktop *d, struct desk_vram_entry *e)
{
    uint32_t stride; const uint32_t *src=vram_source(d,e,&stride);
    if (!src) { e->valid=0; e->uploaded=0; return 0; }
    uint32_t rows=e->height-e->uploaded;
    if (rows>DESK_VRAM_STEP_ROWS) rows=DESK_VRAM_STEP_ROWS;
    if (vram_write(d,src+(size_t)e->uploaded*stride,stride,e->width,rows,0,rows,e->row+e->uploaded)) {
        if (e->kind) { e->valid=0; e->uploaded=0; }
        return 0;
    }
    e->uploaded+=rows;
    e->valid=e->uploaded==e->height;
    return !e->valid;
}
static void vram_check(struct desktop *, uint64_t, uint32_t);
int desk_accel_cache_step(struct desktop *d, uint64_t now)
{
    /* A busy first answer (the kernel's write self-test needs the display)
     * is asked again twice, 2 s apart; none at all is final. */
    if (d->accel && (d->accel_caps&1u) && !d->vram_end && d->vram_queries && d->vram_queries<3 &&
        now>=d->vram_query_ns) { d->vram_query_ns=now+2000000000ull; vram_query(d); }
    if (!d->accel || !(d->accel_caps&1u) || d->vram_end<=d->vram_first || !d->pixels) return 0;
    uint32_t next=d->vram_first;
    /* The background needs one full screen of rows; stale serials restart. */
    struct desk_vram_entry *bg=&d->vram[0];
    if (d->shell && d->sh.background && d->sh.background_valid && d->vram_end-next>=d->height) {
        if (bg->kind!=DESK_VRAM_BACKGROUND || bg->serial!=d->sh.background_serial) {
            vram_free(bg);
            *bg=(struct desk_vram_entry){.kind=DESK_VRAM_BACKGROUND,.row=next,.width=d->width,
                .height=d->height,.serial=d->sh.background_serial};
        }
        next+=d->height;
        if (!bg->valid) return vram_upload(d,bg);
    } else if (bg->kind) vram_free(bg);
    /* Windows, front-most first, while they fit; checked at most twice a
     * second (a full RAM comparison), uploaded once unchanged for a check. */
    if (now>=d->vram_check_ns) vram_check(d,now,next);
    for (int i=1;i<DESK_VRAM_ENTRIES;i++)
        if (d->vram[i].kind && !d->vram[i].valid && d->vram[i].dirty_ns+DESK_VRAM_CHECK_NS<=now)
            return vram_upload(d,&d->vram[i]);
    return 0;
}
static void vram_check(struct desktop *d, uint64_t now, uint32_t next)
{
    d->vram_check_ns=now+DESK_VRAM_CHECK_NS;
    int order[DESK_VRAM_ENTRIES-1], count=0; uint64_t last=UINT64_MAX;
    while (count<DESK_VRAM_ENTRIES-1) {
        int best=-1;
        for (int i=0;i<DESK_CLIENTS;i++) if (vram_window_ok(d,&d->clients[i]) && d->clients[i].order<last &&
            (best<0 || d->clients[i].order>d->clients[best].order)) best=i;
        if (best<0) break;
        last=d->clients[best].order; order[count++]=best;
    }
    for (int k=1;k<DESK_VRAM_ENTRIES;k++) {
        struct desk_vram_entry *e=&d->vram[k];
        int want=k-1<count ? order[k-1] : -1;
        const struct desk_client *c=want>=0 ? &d->clients[want] : NULL;
        if (!c || next+c->info.height>d->vram_end) { if (e->kind) vram_free(e); continue; }
        uint32_t w=c->info.width, h=c->info.height;
        if (e->kind!=DESK_VRAM_WINDOW || e->client!=want || e->width!=w || e->height!=h || e->row!=next) {
            vram_free(e);
            uint32_t *shadow=malloc((size_t)w*h*4);
            if (!shadow) { next+=h; continue; }
            *e=(struct desk_vram_entry){.kind=DESK_VRAM_WINDOW,.client=want,.row=next,.width=w,.height=h,
                .shadow=shadow,.dirty_ns=now,.order=c->order};
            for (uint32_t y=0;y<h;y++) xrgb_row(shadow+(size_t)y*w,c->pixels+(size_t)y*w,(int)w);
        } else {
            /* Exact comparison; a change replaces the shadow and waits. */
            uint32_t *row=malloc((size_t)w*4); int changed=0;
            if (!row) { next+=h; continue; }
            for (uint32_t y=0;y<h;y++) {
                xrgb_row(row,c->pixels+(size_t)y*w,(int)w);
                if (memcmp(row,e->shadow+(size_t)y*w,(size_t)w*4)) {
                    memcpy(e->shadow+(size_t)y*w,row,(size_t)w*4); changed=1;
                }
            }
            free(row);
            if (changed) { e->valid=0; e->uploaded=0; e->dirty_ns=now; }
        }
        next+=h;
    }
}
/* A scroll is accelerated only after comparing every translated client pixel
 * with the last completed RAM frame. Samples merely reject candidates early. */
static void region_load(struct desktop *,pixman_region32_t *,const struct desk_damage *);
static int scroll_plan(struct desktop *d, struct ciuki_rect *out, pixman_region32_t *copied)
{
    if (!(d->accel_caps&1u) || !d->hw_cursor || !d->scanout_valid ||
        !d->damage.count || d->power.state) return 0;
    pixman_region32_t dirty;region_load(d,&dirty,&d->damage);
    for (unsigned i=0; i<DESK_CLIENTS; i++) {
        struct desk_client *c=&d->clients[i];
        struct desk_box b=desk_content(c), ignored;
        if (!c->window || c->internal || c->minimized || c->collapsed || !c->pixels ||
            b.w<64 || b.h<32 || b.x<0 || b.y<0 || b.x+b.w>(int32_t)d->width || b.y+b.h>(int32_t)d->height ||
            b.w!=(int32_t)c->info.width || b.h!=(int32_t)c->info.height) continue;
        int damaged=0;
        for (unsigned j=0; j<d->damage.count; j++) if (desk_intersect(d->damage.boxes[j],b,&ignored)) damaged=1;
        if (!damaged) continue;
        for (int horizontal=0;horizontal<2;horizontal++)
        for (int delta=1; delta<=64 && delta<(horizontal ? b.w : b.h)/2; delta++) for (int dir=-1; dir<=1; dir+=2) {
            int sy=!horizontal && dir<0 ? delta : 0, dy=!horizontal && dir>0 ? delta : 0;
            int sx=horizontal && dir<0 ? delta : 0, dx=horizontal && dir>0 ? delta : 0, same=1, changed=0;
            struct ciuki_rect r={b.x+sx,b.y+sy,b.x+dx,b.y+dy,
                (uint32_t)(b.w-(horizontal ? delta : 0)),(uint32_t)(b.h-(horizontal ? 0 : delta))};
            pixman_region32_t candidate;
            if (!copy_region(d,&candidate,&r,(int)i)) { pixman_region32_fini(&candidate);continue; }
            if(!pixman_region32_intersect(&candidate,&candidate,&dirty) || !pixman_region32_not_empty(&candidate)) {
                pixman_region32_fini(&candidate);continue;
            }
            int n;const pixman_box32_t *boxes=pixman_region32_rectangles(&candidate,&n);
            /* Reject mismatched translations before walking every visible
             * pixel. Hidden rows and shadows never supply a copy source. */
            struct desk_box sample=box_region(boxes[0]);
            const uint32_t *a=c->pixels+(size_t)(sample.y-b.y)*c->info.width+(unsigned)(sample.x-b.x);
            const uint32_t *old=d->pixels+(size_t)(sample.y+sy-dy)*d->width+(unsigned)(sample.x+sx-dx);
            const uint32_t *at=d->pixels+(size_t)sample.y*d->width+(unsigned)sample.x;
            if ((a[0]&0xffffffu)==at[0] && (a[sample.w/2]&0xffffffu)==at[sample.w/2] &&
                (a[sample.w-1]&0xffffffu)==at[sample.w-1]) {
                pixman_region32_fini(&candidate);continue;
            }
            if ((a[0]&0xffffffu)!=old[0] || (a[sample.w/2]&0xffffffu)!=old[sample.w/2] ||
                (a[sample.w-1]&0xffffffu)!=old[sample.w-1]) same=0;
            for (int k=0;k<n && same;k++) {
                struct desk_box part=box_region(boxes[k]);
                for (int y=part.y;y<part.y+part.h && same;y++) for (int x=part.x;x<part.x+part.w;x++) {
                    d->frame_stats.compare_px++;
                    uint32_t v=c->pixels[(size_t)(y-b.y)*c->info.width+(unsigned)(x-b.x)]&0xffffffu;
                    if (v!=d->pixels[(size_t)(y+sy-dy)*d->width+(unsigned)(x+sx-dx)]) { same=0;break; }
                    if (v!=d->pixels[(size_t)y*d->width+(unsigned)x]) changed=1;
                }
            }
            if (same && changed && pixman_region32_copy(copied,&candidate)) {
                pixman_region32_fini(&candidate);pixman_region32_fini(&dirty);*out=r;return 1;
            }
            pixman_region32_fini(&candidate);
        }
    }
    pixman_region32_fini(&dirty);return 0;
}
/* Present the composite surface directly. There is no second frame to copy
 * or compare. COPY's exact destination is excluded from upload rows by the
 * caller; FILL is used only after checking every pixel in its span. */
static int engine_failed(struct desktop *d)
{
    /* A failed engine may have changed part of scanout. Retain a full
     * software recovery frame rather than trusting it. */
    d->frame_stats.accel_fallback++;
    d->scanout_valid=0;
    desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,(int)d->height},d->width,d->height);
    return -1;
}
/* Batch only when a native command family is advertised. Legacy adapters
 * retain their existing fill/copy/cache paths. Binary rows (including text)
 * use MONO; coverage/photographic rows retain exact XRGB uploads. */
static const uint32_t *command_previous(const struct desktop *d, struct desk_box b, int y)
{
    struct desk_box old=d->command_old_box;
    if (!d->command_old_pixels || b.x<old.x || b.x+b.w>old.x+old.w || y<old.y || y>=old.y+old.h) return NULL;
    return d->command_old_pixels+(size_t)(y-old.y)*old.w+b.x-old.x;
}
static int invert_spans(const uint32_t *old, const uint32_t *row, int width, int x, int y,
                        struct ciuki_batch_clip clips[16], unsigned *count)
{
    *count=0;
    for (int i=0;i<width;) {
        if (((old[i]^row[i])&0xffffffu)==0) { i++; continue; }
        if (((old[i]^row[i])&0xffffffu)!=0xffffffu || *count==16) return 0;
        int start=i++;
        while (i<width && ((old[i]^row[i])&0xffffffu)==0xffffffu) i++;
        clips[(*count)++]=(struct ciuki_batch_clip){x+start,y,(unsigned)(i-start),1};
    }
    return 1;
}
static int row_key(const struct desktop *d, const uint32_t *old, const uint32_t *row, int width, uint32_t *key)
{
    const struct ciuki_framebuffer_info *f=&d->command_format;
    unsigned r=f->red_size,g=f->green_size,b=f->blue_size;
    if (f->generation!=d->accel_generation || !r || !g || !b || r>8 || g>8 || b>8) return 0;
    unsigned palette=1u<<(r+g+b);
    for (unsigned candidate=0;candidate<palette && candidate<=(unsigned)width;candidate++) {
        unsigned red=(candidate>>(g+b))&((1u<<r)-1), green=(candidate>>b)&((1u<<g)-1), blue=candidate&((1u<<b)-1);
        uint32_t rgb=red<<(24-r)|green<<(16-g)|blue<<(8-b); int found=0;
        for (int x=0;x<width;x++) if (((row[x]^old[x])&0xffffffu) &&
            ((row[x]>>(24-r))&((1u<<r)-1))==red && ((row[x]>>(16-g))&((1u<<g)-1))==green &&
            ((row[x]>>(8-b))&((1u<<b)-1))==blue) { found=1; break; }
        if (!found) { *key=rgb; return 1; }
    }
    return 0;
}
static void command_capture_previous(struct desktop *d)
{
    free(d->command_old_pixels); d->command_old_pixels=NULL;
    if (!(d->command_caps&(CIUKI_CAP_INVERT|CIUKI_CAP_KEYED)) || !d->hw_cursor || !d->scanout_valid ||
        d->copy_pending || d->move_damage.count || d->damage.count!=1) return;
    struct desk_box b;
    if (!desk_intersect(d->damage.boxes[0],(struct desk_box){0,0,(int)d->width,(int)d->height},&b) ||
        (uint64_t)b.w*b.h*4>65536) return;
    d->command_old_pixels=malloc((size_t)b.w*b.h*4); if (!d->command_old_pixels) return;
    d->command_old_box=b;
    for (int y=0;y<b.h;y++) memcpy(d->command_old_pixels+(size_t)y*b.w,d->pixels+(size_t)(b.y+y)*d->width+b.x,(size_t)b.w*4);
}
static int present_box_commands(struct desktop *d, struct desk_box b)
{
    unsigned row_bytes=(((unsigned)b.w*4+7)&~7u), capacity=(row_bytes>256 ? row_bytes : 256)*32;
    uint8_t *payload=calloc(1,capacity);
    struct ciuki_batch_record *records=calloc(64,sizeof(*records));
    if (!payload || !records) { free(payload); free(records); return -ENOMEM; }
    for (int y=b.y;y<b.y+b.h;) {
        memset(payload,0,capacity);
        unsigned n=0, bytes=0; int video=0;
        for (unsigned rows=0;rows<32 && y<b.y+b.h;rows++,y++) {
            const uint32_t *row=d->pixels+(size_t)y*d->width+b.x, *old=command_previous(d,b,y);
            struct ciuki_batch_clip clips[16]; unsigned count=0; uint32_t key;
            if (old && (d->command_caps&CIUKI_CAP_INVERT) && invert_spans(old,row,b.w,b.x,y,clips,&count)) {
                if (count) {
                    records[n++]=(struct ciuki_batch_record){.opcode=CIUKI_CMD_INVERT,.dx=b.x,.dy=y,.dw=b.w,.dh=1,
                        .clip_offset=bytes,.clip_count=count};
                    memcpy(payload+bytes,clips,count*16); bytes+=count*16;
                }
                continue;
            }
            if (old && d->command_video_valid && (d->command_caps&CIUKI_CAP_KEYED) && row_key(d,old,row,b.w,&key)) {
                records[n++]=(struct ciuki_batch_record){.opcode=CIUKI_CMD_WRITE,.dst=2,.dy=(int)d->command_video_row,
                    .dw=b.w,.dh=1,.sw=b.w,.sh=1,.data_offset=bytes,.data_bytes=(unsigned)b.w*4,.stride=(unsigned)b.w*4};
                for (int x=0;x<b.w;x++) {
                    uint32_t value=((row[x]^old[x])&0xffffffu) ? row[x]&0xffffffu : key;
                    memcpy(payload+bytes+x*4,&value,4);
                }
                bytes+=row_bytes; video=1;
                records[n++]=(struct ciuki_batch_record){.opcode=CIUKI_CMD_KEYED,.src=2,.sy=(int)d->command_video_row,
                    .dx=b.x,.dy=y,.dw=b.w,.dh=1,.key=key};
                continue;
            }
            uint32_t fg=row[0]&0xffffffu, bg=fg; unsigned colours=1;
            for (int x=1;x<b.w;x++) if ((row[x]&0xffffffu)!=fg) {
                if (colours==1) { bg=row[x]&0xffffffu; colours=2; }
                else if ((row[x]&0xffffffu)!=bg) { colours=3; break; }
            }
            struct ciuki_batch_record *r=&records[n++];
            *r=(struct ciuki_batch_record){.dx=b.x,.dy=y,.dw=b.w,.dh=1};
            if (colours==1) { r->opcode=CIUKI_CMD_FILL; r->foreground=fg; continue; }
            r->sw=b.w; r->sh=1; r->data_offset=bytes;
            if (colours==2 && (d->command_caps&CIUKI_CAP_MONO)) {
                r->opcode=CIUKI_CMD_MONO; r->foreground=fg; r->background=bg;
                r->stride=r->data_bytes=((unsigned)b.w+7)/8;
                for (int x=0;x<b.w;x++) if ((row[x]&0xffffffu)==fg) payload[bytes+x/8]|=128u>>(x%8);
            } else {
                r->opcode=CIUKI_CMD_WRITE; r->stride=r->data_bytes=(unsigned)b.w*4;
                memcpy(payload+bytes,row,(size_t)b.w*4);
            }
            bytes+=(r->data_bytes+7)&~7u;
        }
        int e=n ? command_submit_targets(d,records,n,payload,bytes,video) : 0;
        if (e) { free(payload); free(records); return e; }
        if (n) { d->presents++; d->frame_stats.presents++; }
    }
    free(payload); free(records); return 0;
}
static int present_box_raw(struct desktop *d, struct desk_box b)
{
    if (d->batch && (d->command_caps&(CIUKI_CAP_MONO|CIUKI_CAP_INVERT|CIUKI_CAP_KEYED)))
        return present_box_commands(d,b) ? engine_failed(d) : 0;

    int engine=(d->accel_caps&1u) && d->accel;
    /* f2-92: offscreen-cache copies need no previous scanout; fills and
     * duplicate-row copies keep requiring a completed engine-era frame. */
    int scan=engine && d->scanout_valid, cached=engine && d->vram_end && b.w>=DESK_VRAM_MIN_W;
    if (!scan && !cached) {
        struct ciuki_rect r={b.x,b.y,b.x,b.y,(uint32_t)b.w,(uint32_t)b.h};
        if (d->ops.present(d->display,d->output,&r)) return -1;
        d->frame_stats.lfb_bytes+=(uint64_t)b.w*b.h*(d->lfb_bytes_per_pixel ? d->lfb_bytes_per_pixel : 4);
        d->presents++; d->frame_stats.presents++; return 0;
    }
    int32_t hit_x=0, hit_y=0;
    int hit=cached ? vram_lookup(d,b,b.y,&hit_x,&hit_y) : -1;
    for (int y=b.y;y<b.y+b.h;) {
        const uint32_t *row=d->pixels+(size_t)y*d->width+b.x;
        int solid=scan && b.w>=64;
        for (int x=1;solid && x<b.w;x++) if (row[x]!=row[0]) solid=0;
        if (!solid && hit>=0) {
            /* Consecutive rows served by the same cache: one engine copy. */
            int bottom=y+1; int32_t nx=0, ny=0;
            int next=-1;
            while (bottom<b.y+b.h && (next=vram_lookup(d,b,bottom,&nx,&ny))==hit &&
                   nx==hit_x && ny==hit_y+(bottom-y)) { bottom++; next=-1; }
            struct ciuki_accel_request q={.operation=CIUKI_ACCEL_COPY,
                .rect={hit_x,hit_y,b.x,y,(uint32_t)b.w,(uint32_t)(bottom-y)}};
            if (accel_call(d,&q)) return engine_failed(d);
            d->presents++; d->frame_stats.presents++; d->frame_stats.accel_copies++;
            d->frame_stats.cache_rows+=(uint64_t)(bottom-y);
            y=bottom;
            if (y<b.y+b.h) {
                if (next>=0 || (next=vram_lookup(d,b,y,&nx,&ny))>=0) { hit=next; hit_x=nx; hit_y=ny; }
                else hit=-1;
            }
            continue;
        }
        /* Composition is complete, and the preceding span was presented in
         * this call. Equal non-solid rows can copy that scanout span without
         * retaining another RAM frame. A failure uses the normal full retry. */
        if (!solid && scan && b.w>=64 && y>b.y &&
            !memcmp(row,row-d->width,(size_t)b.w*4)) {
            struct ciuki_rect r={b.x,y-1,b.x,y,(uint32_t)b.w,1};
            if (screen_copy(d,&r)) return -1;
            y++;
            if (cached && y<b.y+b.h) hit=vram_lookup(d,b,y,&hit_x,&hit_y);
            continue;
        }
        int bottom=y+1;
        hit=-1;
        if (solid) {
            for (;bottom<b.y+b.h;bottom++) {
                const uint32_t *next=d->pixels+(size_t)bottom*d->width+b.x;
                int same=1;
                for (int x=0;x<b.w;x++) if (next[x]!=row[0]) { same=0; break; }
                if (!same) break;
            }
        } else {
            /* Upload distinct rows together, stopping before a duplicate so
             * its now-presented predecessor can supply a screen copy, and
             * before a row an offscreen cache can supply. */
            for (;bottom<b.y+b.h;bottom++) {
                const uint32_t *next=d->pixels+(size_t)bottom*d->width+b.x;
                if (scan && b.w>=64 && !memcmp(next,next-d->width,(size_t)b.w*4)) break;
                if (cached && (hit=vram_lookup(d,b,bottom,&hit_x,&hit_y))>=0) break;
            }
        }
        if (bottom<b.y+b.h && hit<0 && cached && solid) hit=vram_lookup(d,b,bottom,&hit_x,&hit_y);
        struct ciuki_rect r={b.x,y,b.x,y,(uint32_t)b.w,(uint32_t)(bottom-y)};
        int filled=0;
        if (solid) {
            struct ciuki_accel_request q={.operation=CIUKI_ACCEL_FILL,.colour=row[0],.rect=r};
            if (accel_call(d,&q)) return engine_failed(d);
            filled=1; d->frame_stats.accel_fills++;
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
/* f2-95: a frame's damage as an X11 region: the list's union clipped to
 * the screen. Should the pool be exhausted, the list's covering box is used
 * (it needs no storage): a frame is never refused for region memory. */
static void region_load(struct desktop *d, pixman_region32_t *region, const struct desk_damage *list)
{
    pixman_box32_t boxes[DESK_DAMAGE_MAX], all={0,0,0,0}; unsigned n=0;
    for (unsigned i=0;i<list->count && i<DESK_DAMAGE_MAX;i++) {
        struct desk_box b;
        if (!desk_intersect(list->boxes[i],(struct desk_box){0,0,(int)d->width,(int)d->height},&b)) continue;
        boxes[n]=region_box(b);
        if (!n) all=boxes[0];
        else {
            if (boxes[n].x1<all.x1) all.x1=boxes[n].x1;
            if (boxes[n].y1<all.y1) all.y1=boxes[n].y1;
            if (boxes[n].x2>all.x2) all.x2=boxes[n].x2;
            if (boxes[n].y2>all.y2) all.y2=boxes[n].y2;
        }
        n++;
    }
    if (!pixman_region32_init_rects(region,boxes,(int)n))
        pixman_region32_init_rect(region,all.x1,all.y1,(unsigned)(all.x2-all.x1),(unsigned)(all.y2-all.y1));
}
/* Compose (present=0) or present (present=1) every rectangle of a region
 * except the cut, a destination the engine or the RAM copy already holds.
 * Without memory for the subtraction the whole region is used: the cut
 * holds the same composed pixels, so the superset is only redundant. */
static int region_each(struct desktop *d, const pixman_region32_t *region, const pixman_region32_t *cut, int present)
{
    pixman_region32_t clipped; const pixman_region32_t *use=region;
    pixman_region32_init(&clipped);
    if (cut && pixman_region32_subtract(&clipped,region,cut)) use=&clipped;
    int n, result=0; const pixman_box32_t *r=pixman_region32_rectangles(use,&n);
    for (int k=0;k<n && !result;k++) {
        struct desk_box b=box_region(r[k]);
        if (present) { d->frame_stats.damage_px+=(uint64_t)b.w*b.h; result=present_box(d,b); }
        else { scene(d,b); d->frame_stats.scene_px+=(uint64_t)b.w*b.h; }
    }
    pixman_region32_fini(&clipped);
    return result;
}
static int region_covers(const pixman_region32_t *region, struct desk_box box)
{
    pixman_box32_t b=region_box(box);
    return pixman_region32_contains_rectangle(region,&b)==PIXMAN_REGION_IN;
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
extern void desk_v3_present_complete(struct desktop *) __attribute__((weak));
int desk_redraw(struct desktop *d)
{
    if (d->recovering_fence) {
        struct ciuki_batch_reply result;
        int e=d->fence(d->display,d->recovering_fence,CIUKI_FENCE_WAIT,1,&result);
        if (e || !(result.flags&CIUKI_FENCE_CPU_SAFE)) return 0;
        (void)d->fence(d->display,d->recovering_fence,CIUKI_FENCE_RELEASE,0,&result);
        d->recovering_fence=0; d->hw_cursor=d->hw_cursor_shown=0; d->cursor_dirty=1;
        d->scanout_valid=0;
        desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,(int)d->height},d->width,d->height);
        command_query(d);
    }

    /* f2-95: while a leased program owns the display, retained damage waits
     * at the frame cadence (it is drawn when the lease ends). */
    if (d->v3_lease_state) { d->next_frame_ns=time_now(d)+DESK_FRAME_NS; return 0; }
    if (d->monitor_off) { d->next_frame_ns=time_now(d)+DESK_FRAME_NS; return 0; }
    if (d->saver_active) {
        d->hw_cursor=0; pointer_hide(d);
        if (d->hw_cursor_shown || d->recovering_fence) { d->next_frame_ns=time_now(d)+DESK_FRAME_NS; return 0; }
        struct desk_box b={0,0,(int)d->width,(int)d->height};
        saver_scene(d,b);
        struct ciuki_rect r={0,0,0,0,d->width,d->height};
        int e=d->ops.present(d->display,d->output,&r);
        if (!e) {
            d->presents++; d->damage.count=d->move_damage.count=d->present_damage.count=0;
            d->cursor_dirty=0;
        }
        d->next_frame_ns=time_now(d)+50000000ull;
        return e ? -1 : 0;
    }
    /* A shell caller can move the pointer while damaging the scene rather
     * than sending a motion event. Its old save-under still needs restoring
     * and both footprints need presentation, including the shadow. */
    if (d->cursor_valid && !d->cursor_dirty) {
        struct desk_box next, old=d->cursor_box;
        if (!desk_intersect(pointer_box(d),(struct desk_box){0,0,(int)d->width,(int)d->height},&next) ||
            next.x!=old.x || next.y!=old.y || next.w!=old.w || next.h!=old.h) d->cursor_dirty=1;
    }
    /* A client bitmap or a hidden pointer needs the software path; it draws
     * only once the hardware cursor is known hidden (f2-96). */
    if ((d->v3_cursor_id || d->v3_cursor_hidden) && d->hw_cursor) { d->hw_cursor=0; d->cursor_dirty=1; }
    if (!d->hw_cursor && d->hw_cursor_shown) pointer_hide(d);
    desk_scene_reconcile(d);
    if (!d->damage.count && !d->move_damage.count && !d->present_damage.count && !d->cursor_dirty) return 0;
    /* A RESYNC/damage can precede the mode-counter service turn. Refresh
     * the existing grant query before any generation-tagged engine request;
     * an unchanged layout can still have a new framebuffer lease. */
    if (d->display_info) {
        /* f2-95: a refused query (display owned elsewhere, mode change) is
         * retried at the frame cadence, never on every 1 ms service turn. */
        struct ciuki_framebuffer_info fb;
        if (d->display_info(d->display,&fb)) { int e=errno; d->next_frame_ns=time_now(d)+DESK_FRAME_NS; errno=e; return -1; }
        if (fb.width!=d->width || fb.height!=d->height) { d->next_frame_ns=time_now(d)+DESK_FRAME_NS; errno=EAGAIN; return -1; }
        if (fb.generation!=d->accel_generation) {
            d->lfb_bytes_per_pixel=(fb.bpp+7)/8;
            desk_accel_reset(d,fb.generation);
        }
    }
    uint64_t frame_begin=time_now(d);
    render_cache_prepare(d);
    if (!d->frame_valid) {
        /* First frame, or after a render release (mode change, lease end):
         * nothing in RAM or scanout can be trusted. */
        d->scanout_valid=0; d->cursor_valid=0;
        desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,(int)d->height},d->width,d->height);
    }
    struct desk_frame_stats *s=&d->frame_stats;
    memset(s,0,sizeof(*s)); s->frames=1;
    uint64_t begin=time_now(d);
    if (d->accel) {
        command_query(d);
        uint32_t had=d->accel_caps;
        d->accel_caps=accel_query(d);
        if ((had&1u) && !(d->accel_caps&1u)) {
            /* f2-92: the engine went away while in use (quarantine). */
            d->accel_lost=1;
            for (unsigned i=0;i<DESK_VRAM_ENTRIES;i++) d->vram[i].valid=0;
            d->vram_first=d->vram_end=0;
        }
        if (d->accel_caps&~had&3u) {
            /* f2-92: activation finished after the desktop started, with an
             * unchanged mode generation. Load the hardware cursor and learn
             * the offscreen rows exactly as for a new generation (one full
             * software frame), otherwise window moves never use the engine. */
            desk_accel_reset(d,d->accel_generation);
        } else if ((d->hw_cursor || d->hw_cursor_shown) && !pointer_capable(d)) {
            /* Engine loss: the kernel hid the cursor; software returns. */
            d->hw_cursor=0; pointer_hide(d); d->cursor_dirty=1; d->scanout_valid=0;
            desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,(int)d->height},d->width,d->height);
        } else if (!d->hw_cursor && pointer_capable(d) && !d->v3_cursor_id && !d->v3_cursor_hidden &&
                   frame_begin>=d->hw_cursor_retry_ns) {
            /* f2-96: the engine takes the pointer back (after a client
             * pointer or a failed request). The scanout still holds the
             * software pointer: no screen copy this frame; its footprint
             * is restored from RAM below (cursor_dirty). */
            int error=pointer_load(d);
            if (!error) d->scanout_valid=0;
            else if (!accel_retry(error)) d->hw_cursor_retry_ns=frame_begin+DESK_POINTER_RETRY_NS;
        }
        if (!d->hw_cursor && d->hw_cursor_shown) pointer_hide(d);
    }
    s->present_ns+=time_now(d)-begin;
    /* f2-95: the frame is the region of its damage; nothing outside it is
     * composed or presented. */
    pixman_region32_t move_region, damage_region;
    region_load(d,&move_region,&d->move_damage); region_load(d,&damage_region,&d->damage);
    begin=time_now(d);
    command_capture_previous(d);
    uint64_t pointer_damage[64];
    int pointer_full=d->cursor_dirty || d->copy_pending || !d->frame_valid || !d->cursor_valid ||
                     d->hw_cursor || d->hw_cursor_shown;
    if (d->cursor_valid) {
        struct desk_box b=d->cursor_box;
        pointer_rows(b,pointer_full,&damage_region,&move_region,pointer_damage);
        pointer_under(d,b,pointer_damage,0);
        if (d->cursor_dirty) desk_damage_add(&d->present_damage,b,d->width,d->height);
    }
    struct ciuki_rect copy=d->copy_rect;
    int moving=d->copy_pending && d->frame_valid;
    int engine_copy=moving && d->accel && (d->accel_caps&1u) && d->hw_cursor && d->scanout_valid;
    pixman_region32_t copied;pixman_region32_init(&copied);
    int scrolling=!moving && scroll_plan(d,&copy,&copied);
    engine_copy|=scrolling;
    if (moving) {
        pixman_region32_fini(&copied);
        copy_region(d,&copied,&copy,d->copy_client);
        /* A raise, close or client update can arrive after the move was
         * planned. Never copy a source whose ownership/content changed in
         * that batch, or overwrite pixels already scheduled for repaint. */
        for(unsigned i=0;i<d->damage.count;i++) {
            struct desk_box dirty=d->damage.boxes[i],source=dirty;
            source.x+=copy.dst_x-copy.src_x;source.y+=copy.dst_y-copy.src_y;
            if(!region_cut(&copied,dirty) || !region_cut(&copied,source)) {
                pixman_region32_clear(&copied);break;
            }
        }
        if (!region_uncovered(d,&move_region,d->copy_client,0,0)) {
            pixman_region32_fini(&move_region);region_load(d,&move_region,&d->move_damage);
        }
    }
    if (moving || scrolling) DESK_PROFILE("windows", copy_each(d,&copied,&copy,0));
    if (moving) region_each(d,&move_region,&copied,0);
    region_each(d,&damage_region,scrolling ? &copied : NULL,0);
    d->frame_valid=1;
    struct desk_box b;
    /* Never a software pointer while the hardware cursor may be visible. */
    int soft=!d->hw_cursor && !d->hw_cursor_shown;
    if (soft && desk_intersect(pointer_box(d),
                       (struct desk_box){0,0,(int)d->width,(int)d->height},&b)) {
        pointer_rows(b,pointer_full,&damage_region,&move_region,pointer_damage);
        pointer_under(d,b,pointer_damage,1);
        d->cursor_box=b; d->cursor_valid=1;
        if (d->cursor_dirty) desk_damage_add(&d->present_damage,b,d->width,d->height);
    } else d->cursor_valid=0;
    if (soft) d->cursor_dirty=0;
    s->composite_ns=time_now(d)-begin;
    begin=time_now(d);
    int complete=1;
    if (engine_copy && copy_each(d,&copied,&copy,1)) complete=0;
    /* Move exposure excludes the exact scanout copy. General damage (new
     * client pixels, changed captions) must still overwrite that destination. */
    if (complete && moving && region_each(d,&move_region,engine_copy ? &copied : NULL,1)) complete=0;
    if (complete && region_each(d,&damage_region,scrolling ? &copied : NULL,1)) complete=0;
    if (complete) for (unsigned i=0;i<d->present_damage.count;i++) {
        struct desk_box dirty=d->present_damage.boxes[i];
        if (region_covers(&damage_region,dirty) && !scrolling) continue;
        s->damage_px+=(uint64_t)dirty.w*dirty.h;
        if (present_box(d,dirty)) { complete=0; break; }
    }
    pixman_region32_fini(&copied);pixman_region32_fini(&move_region); pixman_region32_fini(&damage_region);
    /* Faults hide the hardware pointer (the kernel clears it when it stops
     * or quarantines the engine). Reconstruct with a software cursor before
     * acknowledging either launch or a display-mode change. */
    if (d->hw_cursor) {
        d->accel_caps=accel_query(d);
        if (!pointer_capable(d)) { d->hw_cursor=d->hw_cursor_shown=0; d->cursor_dirty=1; complete=0; }
    }
    if (d->hw_cursor && d->cursor_dirty && complete) {
        /* f2-96: busy/stale answers keep the hardware owner and retry on the
         * next frame (cursor_dirty stays set; the scanout is complete). Any
         * other refusal hides the hardware cursor before software draws. */
        int error=d->cursor_loaded!=d->cursor_shape ? pointer_load(d) : 0;
        struct desk_box box=pointer_box(d);
        struct ciuki_accel_request q={.operation=CIUKI_ACCEL_CURSOR_MOVE,.rect={0,0,box.x,box.y,0,0}};
        if (d->hw_colour_cursor) {
            struct ciuki_batch_record records[2]={{.opcode=CIUKI_CMD_CURSOR_MOVE,.dx=d->mouse_x,.dy=d->mouse_y},
                {.opcode=CIUKI_CMD_CURSOR_SHOW,.flags=CIUKI_CMD_SHOW}};
            if (!error) error=command_submit(d,records,2,NULL,0);
            if (!error) d->hw_cursor_shown=1;
        } else {
            if (!error) error=accel_call(d,&q);
            q.operation=CIUKI_ACCEL_CURSOR_SHOW; q.colour=1;
            if (!error && !(error=accel_call(d,&q))) d->hw_cursor_shown=1;
        }
        if (!error) { d->cursor_dirty=0; d->cursor_x=d->mouse_x; d->cursor_y=d->mouse_y; }
        else if (accel_retry(error)) d->hw_cursor_retries++;
        else { pointer_software(d,time_now(d)); complete=0; }
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
                if (d->first_frame) d->first_frame(d,c,end);
            }
        }
        uint64_t start=input && input<=end ? input : frame_begin;
        s->frame_ns=end>=start ? end-start : 0; s->completed=1;
        latency_record(d,s->frame_ns); d->frame_input_ns=0;
        d->damage.count=d->present_damage.count=0; d->scanout_valid=1;
        d->last_frame_ns=frame_begin;
    } else {
        d->scanout_valid=0;
        if (d->accel) d->cursor_dirty=1;
        /* The copied source no longer exists in RAM; retries reconstruct. */
        for (unsigned i=0;i<d->move_damage.count;i++)
            desk_damage_add(&d->damage,d->move_damage.boxes[i],d->width,d->height);
        /* f2-95: only an engine failure leaves scanout unknown, and those
         * paths (engine_failed, screen_copy) already damage the whole frame.
         * A refused CPU present or a lost hardware cursor keeps its damage:
         * the rows written before it are the composed pixels, so the retry
         * composes and presents the same region, not a full frame. */
    }
    d->copy_pending=0; d->move_damage.count=0;
    s->input_ns=d->pending_input_ns; s->input_lag_ns=d->pending_input_lag_ns;
    d->pending_input_ns=d->pending_input_lag_ns=0;
    d->stats.frames++; d->stats.damage_px+=s->damage_px; d->stats.scene_px+=s->scene_px;
    d->stats.presents+=s->presents; d->stats.lfb_bytes+=s->lfb_bytes;
    d->stats.accel_fills+=s->accel_fills; d->stats.accel_copies+=s->accel_copies;
    d->stats.accel_writes+=s->accel_writes; d->stats.accel_fallback+=s->accel_fallback;
    d->stats.cache_rows+=s->cache_rows;
    d->stats.copy_bytes+=s->copy_bytes;d->stats.compare_px+=s->compare_px;
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
    /* fx-26: a frame that ends after the next boundary lets the next frame
     * start at once instead of waiting for the boundary after (there is no
     * vertical retrace to lock to: VBE reports no refresh, DESK_FRAME_NS).
     * The lost slot turned every frame that started late in its slot
     * (scheduler wake-up lateness, the first motion after an idle slot) and
     * ran past the boundary into a 33 ms gap, halving a drag on the T23
     * (f441c5bd: 25-47 frames a second with the CPU 40-50 % idle). Windows
     * 98 draws as soon as the window moved. The next frame still starts at
     * least one slot after this one started (the rate cap), never later than
     * the old rule; the phase then follows that start. */
    if (end>deadline) deadline=end>budget ? end : budget;
    d->stats.overruns+=s->overruns; d->stats.dropped+=s->dropped;
    s->frame_p95_ns=d->stats.frame_p95_ns; s->frame_max_ns=d->stats.frame_max_ns;
    d->next_frame_ns=deadline;
    if (complete && desk_gfx_complete) for (int i=0;i<DESK_CLIENTS;i++) {
        struct desk_client *c=&d->clients[i];
        if (c->gfx_pending) desk_gfx_complete(d,i,c->minimized || c->collapsed ? -EAGAIN : 0,end,deadline);
    }
    if(complete && desk_v3_present_complete)desk_v3_present_complete(d);
    return complete ? 0 : -1;
}
uint64_t desk_launch_wait(const struct desktop *d, uint64_t now)
{
    /* A stalled app must not make the desktop poll faster forever. */
    for (int i=0; i<DESK_CLIENTS; i++) {
        const struct desk_client *c=&d->clients[i];
        /* fx-8: a spawn still running on the launcher worker wakes the loop itself. */
        if (c->channel>=0 && c->awaiting_frame && !c->spawn_pending && now>=c->launched_ns &&
            now-c->launched_ns<5000000000ull) return 1000000;
    }
    return 10000000;
}
