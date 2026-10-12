/* SPDX-License-Identifier: MIT
 * Integer-only CPU compositor. All drawing is clipped before pixel access. */
#include "desktop.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "shell_icons.h"
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
{ return (struct desk_box){c->frame.x+DESK_BORDER,c->frame.y+DESK_TITLE,(int32_t)c->info.width,c->collapsed ? 0 : (int32_t)c->info.height}; }
/* The frame plus its theme's drop shadow: the area a frame change repaints. */
struct desk_box desk_outer(const struct desk_client *c)
{
    int32_t s=desk_theme->shadow_size;
    if (!s) return c->frame;
    return (struct desk_box){c->frame.x-s+s/3,c->frame.y-s+s/2,c->frame.w+2*s,c->frame.h+2*s};
}
struct desk_box desk_close_box(const struct desk_client *c)
{
    int32_t w = c->frame.w - DESK_BORDER*2;
    if (w > 18) w = 18;
    return (struct desk_box){c->frame.x+c->frame.w-DESK_BORDER-w,c->frame.y+3,w,18};
}
struct desk_box desk_min_box(const struct desk_client *c)
{
    struct desk_box close=desk_close_box(c);
    int32_t available=close.x-c->frame.x-DESK_BORDER-2;
    if (available<=0) return (struct desk_box){0,0,0,0};
    int32_t w=available<18 ? available : 18;
    return (struct desk_box){close.x-w-2,close.y,w,close.h};
}
/* Windowshade (collapse to the title bar), left of the minimise box. */
struct desk_box desk_shade_box(const struct desk_client *c)
{
    struct desk_box min=desk_min_box(c);
    int32_t available=min.w>0 ? min.x-c->frame.x-DESK_BORDER-40 : 0;
    if (available<18) return (struct desk_box){0,0,0,0};
    return (struct desk_box){min.x-20,min.y,18,min.h};
}
void desk_collapse(struct desktop *d, int i)
{
    if (i<0 || i>=DESK_CLIENTS || !d->clients[i].window) return;
    struct desk_client *c=&d->clients[i];
    desk_damage_add(&d->damage,desk_outer(c),d->width,d->height);
    c->collapsed=!c->collapsed;
    c->frame.h=c->collapsed ? DESK_TITLE : (int32_t)c->info.height+DESK_TITLE+DESK_BORDER;
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
    int32_t available=(int32_t)d->width-164;
    if (available<=0 || !count) return (struct desk_box){0,0,0,0};
    int32_t w=available/(int32_t)count;
    if (w>160) w=160;
    int32_t left=84+(int32_t)position*w;
    return (struct desk_box){left,(int32_t)d->height-DESK_BAR+3,w,26};
}
int32_t desk_menu_row(const struct desktop *d)
{
    int32_t h=((int32_t)d->height-2*DESK_BAR)/(int32_t)(d->program_count+2);
    if (h>DESK_MENU_ROW) h=DESK_MENU_ROW;
    return h>0 ? h : 1;
}
struct desk_box desk_menu_box(const struct desktop *d)
{
    int32_t h=desk_menu_row(d)*(int32_t)(d->program_count+2);
    int32_t w=d->width>244 ? 240 : (int32_t)d->width-8;
    return (struct desk_box){4,(int32_t)d->height-DESK_BAR-h,w,h};
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
        struct desk_box a = d->boxes[i];
        if (b.x <= a.x+a.w && a.x <= b.x+b.w && b.y <= a.y+a.h && a.y <= b.y+b.h) {
            b = unite(a,b); d->boxes[i] = d->boxes[--d->count]; i = 0;
        } else i++;
    }
    if (d->count == DESK_DAMAGE_MAX) {
        for (unsigned i = 0; i < d->count; i++) b = unite(b,d->boxes[i]);
        d->count = 0;
    }
    d->boxes[d->count++] = b;
}
static void fill(struct desktop *d, struct desk_box b, struct desk_box clip, uint32_t colour)
{
    if (!desk_intersect(b,clip,&b)) return;
    for (int32_t y = b.y; y < b.y+b.h; y++)
        for (int32_t x = b.x; x < b.x+b.w; x++) d->pixels[(uint32_t)y*d->width+(uint32_t)x] = colour;
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
    if (desk_intersect((struct desk_box){x0,y0,DESK_PORTRAIT_SIZE,DESK_PORTRAIT_SIZE},clip,&b))
        for (int32_t y=b.y; y<b.y+b.h; y++) for (int32_t x=b.x; x<b.x+b.w; x++) {
            uint32_t v=d->portrait[(y-y0)*DESK_PORTRAIT_SIZE+x-x0];
            d->pixels[(uint32_t)y*d->width+(uint32_t)x] = v==DESK_BACKGROUND ? desk_theme->desktop : v;
        }
    struct desk_paint p=paint_of(d,clip);
    int font=desk_theme->font_large, w=desk_text_measure(font,"CiukiOS");
    desk_paint_text(&p,font,((int32_t)d->width-w)/2,y0+DESK_PORTRAIT_SIZE+28,"CiukiOS",desk_theme->desktop_label);
}
static void portrait_at(struct desktop *d, struct desk_box clip, int32_t x, int32_t y, int32_t size)
{
    struct desk_box b;
    if (!d->portrait || size<=0 || !desk_intersect((struct desk_box){x,y,size,size},clip,&b)) return;
    for (int32_t yy=b.y; yy<b.y+b.h; yy++) for (int32_t xx=b.x; xx<b.x+b.w; xx++)
        d->pixels[(uint32_t)yy*d->width+(uint32_t)xx]=
            d->portrait[((yy-y)*DESK_PORTRAIT_SIZE/size)*DESK_PORTRAIT_SIZE+(xx-x)*DESK_PORTRAIT_SIZE/size];
}
static void icon(struct desktop *d, struct desk_box clip, int32_t x, int32_t y, const uint32_t *pixels)
{
    struct desk_box b;
    if (!desk_intersect((struct desk_box){x,y,16,16},clip,&b)) return;
    for (int32_t yy=b.y; yy<b.y+b.h; yy++) for (int32_t xx=b.x; xx<b.x+b.w; xx++) {
        uint32_t src=pixels[(yy-y)*16+xx-x], *dst=&d->pixels[(uint32_t)yy*d->width+(uint32_t)xx];
        *dst=desk_mix(src,*dst,src>>24);
    }
}
static void about_content(struct desktop *d, struct desk_box content, struct desk_box clip)
{
    const struct desk_theme *t=desk_theme;
    struct desk_paint p=paint_of(d,clip);
    fill(d,content,clip,t->face);
    desk_paint_vgradient(&p,content.x,content.y,content.w,124,desk_mix(t->accent,t->face,60),t->face);
    portrait_at(d,clip,content.x+10,content.y+10,112);
    desk_paint_text(&p,t->font_large,content.x+136,content.y+40,"CiukiOS",t->ink);
    desk_paint_text(&p,t->font_ui,content.x+136,content.y+62,"A modern Retro OS",t->ink);
    desk_paint_text(&p,t->font_bold,content.x+136,content.y+86,"Dedicated to Ciuki",t->ink);
    if (d->version[0]) {            /* uname version: config/version.json */
        char line[80];
        snprintf(line,sizeof(line),"Version %.64s",d->version);
        desk_paint_text(&p,t->font_ui,content.x+136,content.y+106,line,t->ink);
    }
    desk_paint_text(&p,t->font_bold,content.x+10,content.y+141,"Kernel build:",t->ink);
    desk_paint_text(&p,t->font_ui,content.x+10,content.y+155,d->build_identity,t->ink);
    desk_paint_text(&p,t->font_ui,content.x+10,content.y+171,"Tango 0.8.90 icons: Public Domain",t->ink);
    desk_paint_text(&p,t->font_ui,content.x+10,content.y+187,"Ciuki portrait: separate project asset",t->ink);
    desk_paint_text(&p,t->font_ui,content.x+10,content.y+205,d->realtime_source ? "Clock: RTC (UTC)" : "Clock: build seed (UTC)",t->ink);
}
static const uint32_t *program_icon(int kind);
/* Rounded top corners: pixels a row of the frame skips at either side. */
static int corner_inset(int radius, int row)
{
    if (row<0 || row>=radius) return 0;
    int d=2*radius-2*row-1, v=4*radius*radius-d*d, r=0;
    while ((r+1)*(r+1)<=v) r++;
    return radius-(r+1)/2;
}
struct frame_painter { struct desk_paint p; struct desk_box f; int radius; };
static void ffill(struct frame_painter *fp, int x, int y, int w, int h, uint32_t colour)
{
    for (int yy=y; yy<y+h; yy++) {
        int in=corner_inset(fp->radius,yy-fp->f.y), x0=x, x1=x+w;
        if (x0<fp->f.x+in) x0=fp->f.x+in;
        if (x1>fp->f.x+fp->f.w-in) x1=fp->f.x+fp->f.w-in;
        if (x1>x0) desk_paint_fill(&fp->p,x0,yy,x1-x0,1,colour);
    }
}
static void fgradient(struct frame_painter *fp, int x, int y, int w, int h, uint32_t top, uint32_t bottom, int horizontal)
{
    if (horizontal) {
        struct desk_paint keep=fp->p;
        for (int yy=y; yy<y+h; yy++) {
            int in=corner_inset(fp->radius,yy-fp->f.y);
            fp->p.cx=keep.cx; fp->p.cw=keep.cw;
            struct desk_box row={fp->f.x+in,yy,fp->f.w-2*in,1}, a={keep.cx,keep.cy,keep.cw,keep.ch}, out;
            if (!desk_intersect(row,a,&out)) continue;
            fp->p.cx=out.x; fp->p.cw=out.w;
            desk_paint_hgradient(&fp->p,x,yy,w,1,top,bottom);
        }
        fp->p=keep;
        return;
    }
    for (int r=0; r<h; r++) ffill(fp,x,y+r,w,1,desk_mix(bottom,top,h>1 ? (unsigned)(r*255/(h-1)) : 0));
}
static void frame_outline(struct frame_painter *fp, uint32_t c)
{
    struct desk_box f=fp->f;
    int r=fp->radius;
    for (int i=0; i<r && i<f.h; i++) {
        int in=corner_inset(r,i), prev=i ? corner_inset(r,i-1) : f.w;
        int run=i ? (prev-1>in ? prev-1-in : 1) : f.w-2*in;
        if (!i) { desk_paint_fill(&fp->p,f.x+in,f.y,f.w-2*in,1,c); continue; }
        desk_paint_fill(&fp->p,f.x+in,f.y+i,run,1,c);
        desk_paint_fill(&fp->p,f.x+f.w-in-run,f.y+i,run,1,c);
    }
    if (!r) desk_paint_fill(&fp->p,f.x,f.y,f.w,1,c);
    int top=r ? r : 1;
    desk_paint_fill(&fp->p,f.x,f.y+top,1,f.h-top,c);
    desk_paint_fill(&fp->p,f.x+f.w-1,f.y+top,1,f.h-top,c);
    desk_paint_fill(&fp->p,f.x,f.y+f.h-1,f.w,1,c);
}
static void window(struct desktop *d, int i, struct desk_box clip)
{
    struct desk_client *c = &d->clients[i];
    const struct desk_theme *t = desk_theme;
    struct desk_box outer=desk_outer(c), area;
    if (!desk_intersect(outer,clip,&area)) return;
    struct frame_painter fp={paint_of(d,area),c->frame,t->radius};
    struct desk_box f=c->frame;
    int active=i==d->focus && !c->unresponsive, platinum=t->title_style==DESK_TITLE_PINSTRIPE;
    if (t->shadow_size) desk_paint_shadow(&fp.p,f.x,f.y,f.w,f.h,t->shadow_size,t->drop_shadow);
    if (!desk_intersect(f,clip,&clip)) return;
    fp.p=paint_of(d,clip);
    uint32_t edge=active ? t->frame : t->frame_inactive;
    ffill(&fp,f.x,f.y,f.w,f.h,t->face);
    struct desk_box close = desk_close_box(c), min=desk_min_box(c), shade=desk_shade_box(c);
    int buttons_x=d->shell && shade.w>0 ? shade.x : d->shell && min.w>0 ? min.x : close.x;
    const char *title=c->closing ? "Closing..." : c->unresponsive ? "Not responding" : c->title;
    int font=active ? t->font_title : t->font_title;
    uint32_t ink=active ? t->title_text : t->title_text_inactive;
    if (platinum) {
        /* Platinum: rounded outline, light bevel, title gradient with
         * pinstripes either side of the centred title when active. */
        fgradient(&fp,f.x+1,f.y+1,f.w-2,DESK_TITLE-2,active ? t->title[0] : t->title_inactive[0],
                  active ? t->title[1] : t->title_inactive[1],0);
        ffill(&fp,f.x+1,f.y+1,f.w-2,1,t->hilite);
        desk_paint_fill(&fp.p,f.x+1,f.y+2,1,f.h-3,t->hilite);
        desk_paint_fill(&fp.p,f.x+f.w-2,f.y+2,1,f.h-3,active ? t->shadow : t->light);
        desk_paint_fill(&fp.p,f.x+1,f.y+f.h-2,f.w-2,1,active ? t->shadow : t->light);
        int tw=desk_text_measure(font,title), left=f.x+8, right=buttons_x-6;
        int tx=f.x+(f.w-tw)/2;
        if (tx+tw+8>right) tx=right-8-tw;
        if (tx<left+8) tx=left+8;
        if (active) {
            int sy=f.y+6, sh=12;
            if (tx-8>left) desk_paint_pinstripes(&fp.p,left,sy,tx-8-left,sh,t->stripe_light,t->stripe_dark);
            if (right>tx+tw+8) desk_paint_pinstripes(&fp.p,tx+tw+8,sy,right-tx-tw-8,sh,t->stripe_light,t->stripe_dark);
        }
        struct desk_paint tp=fp.p; struct desk_box tb;
        if (desk_intersect((struct desk_box){left,f.y,right-left,DESK_TITLE},clip,&tb)) {
            tp=paint_of(d,tb);
            if (active) desk_paint_text(&tp,font,tx,f.y+17,title,t->hilite==0xffffffu ? 0xf8f8f8u : t->hilite);
            desk_paint_text(&tp,font,tx,f.y+16,title,ink);
        }
    } else {
        /* Gradient captions: raised frame, caption band, icon and title. */
        uint32_t a0=active ? t->title[0] : t->title_inactive[0], a1=active ? t->title[1] : t->title_inactive[1];
        if (c->unresponsive) { a0=desk_mix(0x8a3a2au,a0,140); a1=desk_mix(0x8a3a2au,a1,140); }
        if (t->control_style==DESK_CONTROLS_CLASSIC) {
            desk_paint_fill(&fp.p,f.x,f.y,f.w-1,1,t->light); desk_paint_fill(&fp.p,f.x,f.y,1,f.h-1,t->light);
            desk_paint_fill(&fp.p,f.x+1,f.y+1,f.w-3,1,t->hilite); desk_paint_fill(&fp.p,f.x+1,f.y+1,1,f.h-3,t->hilite);
            desk_paint_fill(&fp.p,f.x,f.y+f.h-1,f.w,1,t->dark); desk_paint_fill(&fp.p,f.x+f.w-1,f.y,1,f.h,t->dark);
            desk_paint_fill(&fp.p,f.x+1,f.y+f.h-2,f.w-2,1,t->shadow); desk_paint_fill(&fp.p,f.x+f.w-2,f.y+1,1,f.h-2,t->shadow);
            desk_paint_hgradient(&fp.p,f.x+3,f.y+3,f.w-6,DESK_TITLE-5,a0,a1);
        } else {
            fgradient(&fp,f.x+1,f.y+1,f.w-2,DESK_TITLE-1,a0,a1,1);
            ffill(&fp,f.x+1,f.y+1,f.w-2,1,desk_mix(0xffffffu,a0,90));
            desk_paint_fill(&fp.p,f.x+1,f.y+DESK_TITLE-1,f.w-2,1,desk_mix(t->dark,a1,120));
            desk_paint_fill(&fp.p,f.x+1,f.y+DESK_TITLE,1,f.h-DESK_TITLE-2,t->hilite);
            desk_paint_fill(&fp.p,f.x+f.w-2,f.y+DESK_TITLE,1,f.h-DESK_TITLE-1,t->shadow);
            desk_paint_fill(&fp.p,f.x+1,f.y+f.h-2,f.w-2,1,t->shadow);
        }
        int tx=f.x+7;
        struct desk_box tb;
        if (desk_intersect((struct desk_box){f.x+4,f.y,buttons_x-f.x-6,DESK_TITLE},clip,&tb)) {
            struct desk_paint tp=paint_of(d,tb);
            if (c->internal || c->program) {
                struct desk_box ib; if (desk_intersect((struct desk_box){f.x+5,f.y+4,16,16},tb,&ib)) {
                    if (c->internal) portrait_at(d,ib,f.x+5,f.y+4,16); else icon(d,ib,f.x+5,f.y+4,program_icon(c->program));
                }
                tx=f.x+25;
            }
            desk_paint_text(&tp,font,tx,f.y+17,title,ink);
        }
    }
    struct desk_paint bp=fp.p; struct desk_box bb;
    if (desk_intersect((struct desk_box){buttons_x,f.y,f.x+f.w-buttons_x,DESK_TITLE},clip,&bb)) {
        bp=paint_of(d,bb);
        int s=t->button, by=platinum ? f.y+(DESK_TITLE-s)/2 : f.y+4;
        struct desk_box boxes[3]={close,min,shade}; int kinds[3]={DESK_GLYPH_CLOSE,DESK_GLYPH_MINIMIZE,DESK_GLYPH_COLLAPSE};
        for (int k=0; k<3; k++) {
            if (k && (!d->shell || boxes[k].w<=0)) continue;
            int bx=platinum ? boxes[k].x+(boxes[k].w-s)/2 : boxes[k].x+boxes[k].w-s-1;
            desk_paint_title_button(&bp,bx,by,s,kinds[k],active || !platinum,0,0);
        }
    }
    if (platinum) {
        /* Content well: a dark line under the title and round the content. */
        uint32_t well=active ? t->dark : t->shadow;
        desk_paint_fill(&fp.p,f.x+2,f.y+DESK_TITLE-1,f.w-4,1,well);
        if (!c->collapsed) {
            desk_paint_fill(&fp.p,f.x+2,f.y+DESK_TITLE,1,f.h-DESK_TITLE-2,well);
            desk_paint_fill(&fp.p,f.x+f.w-3,f.y+DESK_TITLE,1,f.h-DESK_TITLE-2,well);
            desk_paint_fill(&fp.p,f.x+2,f.y+f.h-3,f.w-4,1,well);
        }
    }
    frame_outline(&fp,platinum || t->control_style!=DESK_CONTROLS_CLASSIC ? edge : t->dark);
    if (t->control_style==DESK_CONTROLS_CLASSIC && !platinum) {
        /* Classic frames are bevels only: restore the raised outer edge. */
        desk_paint_fill(&fp.p,f.x,f.y,f.w-1,1,t->light); desk_paint_fill(&fp.p,f.x,f.y,1,f.h-1,t->light);
    }
    struct desk_box content = desk_content(c), b;
    if (c->collapsed) return;
    if (c->internal) {
        if (desk_intersect(content,clip,&b)) about_content(d,content,b);
        return;
    }
    if (desk_intersect(content,clip,&b))
        for (int32_t y=b.y; y<b.y+b.h; y++) for (int32_t x=b.x; x<b.x+b.w; x++)
            d->pixels[(uint32_t)y*d->width+(uint32_t)x] =
                c->pixels[(uint32_t)(y-content.y)*c->info.width+(uint32_t)(x-content.x)] & 0xffffffu;
}
/* Programs that open a desktop window receive their channel at fd 3. */
int desk_program_graphical(const char *name)
{ return !strcmp(name,"demo") || !strcmp(name,"dosbox") || !strcmp(name,"files") || !strcmp(name,"tasks") || !strcmp(name,"ciuknote"); }
const char *desk_program_label(const char *name)
{
    return !strcmp(name,"demo") ? "Ciuki Demo" : !strcmp(name,"files") ? "Files" : !strcmp(name,"tasks") ? "Task Manager" :
           !strcmp(name,"ciuknote") ? "CiukNote" : name;
}
const char *desk_app_program(int kind) { return kind==DESK_APP_NOTE ? "ciuknote" : "files"; }
int desk_program_kind(const char *name)
{ return !strcmp(name,"files") ? DESK_APP_FILES : !strcmp(name,"ciuknote") ? DESK_APP_NOTE : 0; }
static const uint32_t *program_icon(int kind)
{ return kind==DESK_APP_FILES ? desk_icon_files : kind==DESK_APP_NOTE ? desk_icon_text : desk_icon_run; }
static void bevel(struct desk_paint *p, struct desk_box b, int sunken)
{
    const struct desk_theme *t=desk_theme;
    uint32_t tl=sunken ? t->shadow : t->hilite, br=sunken ? t->hilite : t->dark;
    desk_paint_fill(p,b.x,b.y,b.w-1,1,tl); desk_paint_fill(p,b.x,b.y,1,b.h-1,tl);
    desk_paint_fill(p,b.x,b.y+b.h-1,b.w,1,br); desk_paint_fill(p,b.x+b.w-1,b.y,1,b.h,br);
    if (!sunken) {
        desk_paint_fill(p,b.x+1,b.y+b.h-2,b.w-2,1,t->shadow); desk_paint_fill(p,b.x+b.w-2,b.y+1,1,b.h-2,t->shadow);
    } else {
        desk_paint_fill(p,b.x+1,b.y+1,b.w-2,1,t->dark==0 ? t->dark : desk_mix(t->dark,t->shadow,90));
    }
}
void desk_shell_draw(struct desktop *d, struct desk_box clip)
{
    const struct desk_theme *t=desk_theme;
    struct desk_box bar=desk_taskbar(d), b;
    struct desk_paint p=paint_of(d,clip);
    /* Taskbar: flat face, a light top edge, the Ciuki start button, raised
     * task buttons (the focused one pressed) and a sunken tray clock. */
    fill(d,bar,clip,t->taskbar);
    desk_paint_fill(&p,bar.x,bar.y,bar.w,1,t->light);
    desk_paint_fill(&p,bar.x,bar.y+1,bar.w,1,t->hilite);
    struct desk_box start={4,bar.y+3,76,26};
    fill(d,start,clip,d->menu_open ? t->start_open : t->start);
    bevel(&p,start,d->menu_open);
    portrait_at(d,clip,8,bar.y+6,20);
    desk_paint_text(&p,t->font_bold,34+d->menu_open,bar.y+20+d->menu_open,"Ciuki",t->start_text);
    for (int i=0; i<DESK_CLIENTS; i++) if (desk_intersect(desk_task_button(d,i),clip,&b)) {
        struct desk_box button=desk_task_button(d,i);
        struct desk_paint tp=paint_of(d,b);
        int on=d->focus==i;
        struct desk_box face={button.x+1,button.y,button.w-2,button.h};
        if (on) { desk_paint_fill(&tp,face.x,face.y,face.w,face.h,t->task_active); bevel(&tp,face,1); }
        else desk_paint_raised(&tp,face.x,face.y,face.w,face.h,0,0);
        if (button.w>24) {
            int dx=on;
            if (d->clients[i].internal) portrait_at(d,b,button.x+5+dx,button.y+5+dx,16);
            else icon(d,b,button.x+5+dx,button.y+5+dx,program_icon(d->clients[i].program));
            struct desk_box label;
            if (desk_intersect((struct desk_box){button.x+24,button.y,button.w-28,button.h},b,&label)) {
                struct desk_paint lp=paint_of(d,label);
                desk_paint_text(&lp,on ? t->font_bold : t->font_ui,button.x+25+dx,button.y+17+dx,d->clients[i].title,
                                on ? t->task_active_text : t->taskbar_text);
            }
        }
    }
    char clock[24]; uint64_t s=d->app_test ? 12*3600 : d->realtime_second; /* fixed in the app scenarios */
    snprintf(clock,sizeof(clock),"%02u:%02u:%02u",(unsigned)(s/3600%24),(unsigned)(s/60%60),(unsigned)(s%60));
    struct desk_box tray={(int32_t)d->width-76,bar.y+4,72,24};
    bevel(&p,tray,1);
    desk_paint_text(&p,t->font_ui,tray.x+(tray.w-desk_text_measure(t->font_ui,clock))/2,tray.y+16,clock,t->taskbar_text);
    if (!d->menu_open) return;
    struct desk_box menu=desk_menu_box(d);
    if (!desk_intersect(menu,clip,&b)) return;
    /* Start menu: raised panel with a Ciuki banner down its left side. */
    struct desk_paint mp=paint_of(d,b);
    desk_paint_fill(&mp,menu.x,menu.y,menu.w,menu.h,t->face);
    desk_paint_vgradient(&mp,menu.x+2,menu.y+2,22,menu.h-4,t->select,desk_mix(t->select,0x000000u,150));
    portrait_at(d,b,menu.x+5,menu.y+menu.h-22,16);
    bevel(&mp,menu,0);
    int32_t row=desk_menu_row(d);
    for (unsigned i=0; i<d->program_count+2; i++) {
        struct desk_box item={menu.x+24,menu.y+(int32_t)i*row,menu.w-26,row}, part;
        if (!desk_intersect(item,b,&part)) continue;
        struct desk_paint ip=paint_of(d,part);
        if (i==d->program_count) {
            desk_paint_fill(&ip,item.x+4,item.y,item.w-8,1,t->shadow);
            desk_paint_fill(&ip,item.x+4,item.y+1,item.w-8,1,t->hilite);
        }
        const char *name=i<d->program_count ? desk_program_label(d->programs[i].name) :
            i==d->program_count ? "About CiukiOS" : "Restart (unavailable)";
        if (row>=18) {
            if (i==d->program_count) portrait_at(d,part,item.x+6,item.y+(row-16)/2,16);
            else icon(d,part,item.x+6,item.y+(row-16)/2,i<d->program_count ?
                      program_icon(desk_program_kind(d->programs[i].name)) : desk_icon_restart);
        }
        desk_paint_text(&ip,t->font_ui,item.x+30,item.y+(row-desk_font_cap(t->font_ui))/2+desk_font_cap(t->font_ui),name,
                        i==d->program_count+1 ? t->gray : t->ink);
    }
}
/* Arrow pointer, 11x17: '#' outline, 'o' fill. */
static const char *const arrow[17] = {
    "#..........", "##.........", "#o#........", "#oo#.......", "#ooo#......", "#oooo#.....",
    "#ooooo#....", "#oooooo#...", "#ooooooo#..", "#oooooooo#.", "#ooooo#####", "#oo#oo#....",
    "#o#.#oo#...", "##..#oo#...", "#....#oo#..", ".....#oo#..", "......##..."};
void desk_composite(struct desktop *d, struct desk_box clip)
{
    if (!desk_intersect(clip,(struct desk_box){0,0,(int32_t)d->width,(int32_t)d->height},&clip)) return;
    const struct desk_theme *t=desk_theme;
    fill(d,clip,clip,t->desktop);
    portrait(d,clip);
    /* Top bar: a Platinum-style menu bar with the Ciuki mark. */
    struct desk_paint p=paint_of(d,clip);
    fill(d,(struct desk_box){0,0,(int32_t)d->width,DESK_BAR},clip,t->topbar);
    desk_paint_fill(&p,0,DESK_BAR-1,(int)d->width,1,t->topbar_edge);
    desk_paint_fill(&p,0,0,(int)d->width,1,desk_mix(0xffffffu,t->topbar,160));
    portrait_at(d,clip,8,6,20);
    int x=34+desk_paint_text(&p,t->font_bold,34,20,"CiukiOS",t->topbar_text);
    desk_paint_text(&p,t->font_ui,x+14,20,d->shell && d->notice[0] ? d->notice : "A modern Retro OS",
                    d->shell && d->notice[0] ? t->topbar_accent : desk_mix(t->topbar_text,t->topbar,170));
    char clock[24];
    uint64_t s = d->now_ns / 1000000000ull;
    snprintf(clock,sizeof(clock),"%02u:%02u:%02u",(unsigned)(s/3600%100),(unsigned)(s/60%60),(unsigned)(s%60));
    if (!d->shell) desk_paint_text(&p,t->font_ui,(int)d->width-12-desk_text_measure(t->font_ui,clock),20,clock,t->topbar_text);
    uint64_t last = 0;
    for (unsigned n = 0; n < DESK_CLIENTS; n++) {
        int next = -1;
        for (int i = 0; i < DESK_CLIENTS; i++) if (d->clients[i].window && !d->clients[i].minimized && d->clients[i].order > last &&
            (next < 0 || d->clients[i].order < d->clients[next].order)) next = i;
        if (next < 0) break;
        last = d->clients[next].order; window(d,next,clip);
    }
    if (d->shell) desk_shell_draw(d,clip);
    for (int y = 0; y < 17; y++) for (int i = 0; i < 11; i++) {
        char k=arrow[y][i];
        if (k!='.') fill(d,(struct desk_box){d->mouse_x+i,d->mouse_y+y,1,1},clip,k=='#' ? 0x101010u : 0xffffffu);
    }
}
int desk_redraw(struct desktop *d)
{
    unsigned done = 0;
    for (; done < d->damage.count; done++) {
        struct desk_box b = d->damage.boxes[done];
        desk_composite(d,b);
        struct ciuki_rect r = {b.x,b.y,b.x,b.y,(uint32_t)b.w,(uint32_t)b.h};
        if (d->ops.present(d->display,d->output,&r)) break;
        d->presents++;
    }
    if (done) {
        d->damage.count -= done;
        memmove(d->damage.boxes,d->damage.boxes+done,d->damage.count*sizeof(d->damage.boxes[0]));
    }
    return d->damage.count ? -1 : 0;
}
