/* SPDX-License-Identifier: MIT
 * Integer-only CPU compositor. All drawing is clipped before pixel access. */
#include "desktop.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
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
{ return (struct desk_box){c->frame.x+DESK_BORDER,c->frame.y+DESK_TITLE,(int32_t)c->info.width,(int32_t)c->info.height}; }
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
void desk_text(struct desktop *d, struct desk_box clip, int32_t x, int32_t y,
                 const char *s, int scale, uint32_t colour)
{
    while (*s && x < clip.x+clip.w) {
        unsigned c = (unsigned char)*s++;
        if (c >= 128) c = '?';
        for (int row = 0; row < 7; row++) for (int col = 0; col < 5; col++)
            if (font[c][row] & (1u << (4-col)))
                fill(d,(struct desk_box){x+col*scale,y+row*scale,scale,scale},clip,colour);
        x += 6*scale;
    }
}
static void portrait(struct desktop *d, struct desk_box clip)
{
    if (!d->portrait) return;
    int32_t x0 = ((int32_t)d->width-DESK_PORTRAIT_SIZE)/2;
    int32_t y0 = DESK_BAR + ((int32_t)d->height-DESK_BAR-DESK_PORTRAIT_SIZE-36)/2;
    if (y0 < DESK_BAR) y0 = DESK_BAR;
    struct desk_box b;
    if (desk_intersect((struct desk_box){x0,y0,DESK_PORTRAIT_SIZE,DESK_PORTRAIT_SIZE},clip,&b))
        for (int32_t y=b.y; y<b.y+b.h; y++) for (int32_t x=b.x; x<b.x+b.w; x++)
            d->pixels[(uint32_t)y*d->width+(uint32_t)x] = d->portrait[(y-y0)*DESK_PORTRAIT_SIZE+x-x0];
    desk_text(d,clip,((int32_t)d->width-84)/2,y0+DESK_PORTRAIT_SIZE+10,"CiukiOS",2,0xf6dfb6u);
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
    if (!pixels || !desk_intersect((struct desk_box){x,y,16,16},clip,&b)) return;
    for (int32_t yy=b.y; yy<b.y+b.h; yy++) for (int32_t xx=b.x; xx<b.x+b.w; xx++) {
        uint32_t src=pixels[(yy-y)*16+xx-x], *dst=&d->pixels[(uint32_t)yy*d->width+(uint32_t)xx];
        unsigned a=src>>24;
        uint32_t out=0;
        for (unsigned shift=0; shift<24; shift+=8)
            out|=((((src>>shift)&255)*a+((*dst>>shift)&255)*(255-a)+127)/255)<<shift;
        *dst=out;
    }
}
static void about_content(struct desktop *d, struct desk_box content, struct desk_box clip)
{
    fill(d,content,clip,0xe8d7b9u);
    portrait_at(d,clip,content.x+10,content.y+10,112);
    desk_text(d,clip,content.x+136,content.y+18,"CiukiOS",2,0x213c4cu);
    desk_text(d,clip,content.x+136,content.y+46,"A modern Retro OS",1,0x213c4cu);
    desk_text(d,clip,content.x+136,content.y+72,"Dedicated to Ciuki",1,0x213c4cu);
    if (d->version[0]) {            /* uname version: config/version.json */
        char line[80];
        snprintf(line,sizeof(line),"Version %.64s",d->version);
        desk_text(d,clip,content.x+136,content.y+98,line,1,0x213c4cu);
    }
    desk_text(d,clip,content.x+10,content.y+132,"Kernel build:",1,0x213c4cu);
    desk_text(d,clip,content.x+10,content.y+146,d->build_identity,1,0x213c4cu);
    desk_text(d,clip,content.x+10,content.y+162,"Tango 0.8.90 icons: Public Domain",1,0x213c4cu);
    desk_text(d,clip,content.x+10,content.y+178,"Ciuki portrait: separate project asset",1,0x213c4cu);
    desk_text(d,clip,content.x+10,content.y+196,d->realtime_source ? "Clock: RTC (UTC)" : "Clock: build seed (UTC)",1,0x213c4cu);
}
static void window(struct desktop *d, int i, struct desk_box clip)
{
    struct desk_client *c = &d->clients[i];
    if (!desk_intersect(c->frame,clip,&clip)) return;
    fill(d,c->frame,clip,0xb8c9ccu);
    fill(d,(struct desk_box){c->frame.x+DESK_BORDER,c->frame.y+3,c->frame.w-6,18},clip,
         c->unresponsive ? 0x725d51u : i == d->focus ? 0x213c4cu : 0x607a85u);
    struct desk_box close = desk_close_box(c), titleclip, min=desk_min_box(c);
    int32_t end=d->shell && min.w>0 ? min.x : close.x;
    if (desk_intersect((struct desk_box){c->frame.x+6,c->frame.y,end-c->frame.x-8,DESK_TITLE},clip,&titleclip))
        desk_text(d,titleclip,c->frame.x+7,c->frame.y+8,c->closing ? "Closing..." : c->unresponsive ? "Not responding" : c->title,1,0xffffffu);
    fill(d,close,clip,0xe8d7b9u);
    desk_text(d,clip,close.x+(close.w-5)/2,close.y+5,"X",1,0x213c4cu);
    if (d->shell && min.w>0) {
        fill(d,min,clip,0xe8d7b9u);
        desk_text(d,clip,min.x+(min.w-5)/2,min.y+5,"_",1,0x213c4cu);
    }
    struct desk_box content = desk_content(c), b;
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
/* 16-pixel Tango icon of an icon_map.c object ("program:<name>", "menu:restart"). */
static const uint32_t *shell_icon(const struct desktop *d, const char *object)
{ return d->icon ? d->icon(object,16) : NULL; }
static const uint32_t *program_icon(const struct desktop *d, const char *name)
{
    char object[80];
    snprintf(object,sizeof(object),"program:%.64s",name);
    return shell_icon(d,object);
}
static const char *client_program(const struct desk_client *c)
{ return c->program==DESK_APP_FILES ? "files" : c->program==DESK_APP_NOTE ? "ciuknote" : c->taskman ? "tasks" : "*"; }
void desk_shell_draw(struct desktop *d, struct desk_box clip)
{
    struct desk_box bar=desk_taskbar(d), b;
    fill(d,bar,clip,0xb8c9ccu);
    fill(d,(struct desk_box){4,bar.y+3,76,26},clip,d->menu_open ? 0x607a85u : 0xe8d7b9u);
    portrait_at(d,clip,8,bar.y+6,20);
    desk_text(d,clip,34,bar.y+12,"Ciuki",1,0x213c4cu);
    for (int i=0; i<DESK_CLIENTS; i++) if (desk_intersect(desk_task_button(d,i),clip,&b)) {
        struct desk_box button=desk_task_button(d,i);
        fill(d,(struct desk_box){button.x+1,button.y,button.w-2,button.h},b,
            d->focus==i ? 0x213c4cu : 0xe8d7b9u);
        if (button.w>24) {
            if (d->clients[i].internal) portrait_at(d,b,button.x+4,button.y+5,16);
            else icon(d,b,button.x+4,button.y+5,program_icon(d,client_program(&d->clients[i])));
            desk_text(d,b,button.x+24,button.y+9,d->clients[i].title,1,d->focus==i ? 0xffffffu : 0x213c4cu);
        }
    }
    char clock[24]; uint64_t s=d->app_test ? 12*3600 : d->realtime_second; /* fixed in the app scenarios */
    snprintf(clock,sizeof(clock),"%02u:%02u:%02u",(unsigned)(s/3600%24),(unsigned)(s/60%60),(unsigned)(s%60));
    desk_text(d,clip,(int32_t)d->width-64,bar.y+12,clock,1,0x213c4cu);
    if (!d->menu_open) return;
    struct desk_box menu=desk_menu_box(d);
    if (!desk_intersect(menu,clip,&b)) return;
    fill(d,menu,b,0xe8d7b9u);
    int32_t row=desk_menu_row(d);
    for (unsigned i=0; i<d->program_count+2; i++) {
        struct desk_box item={menu.x,menu.y+(int32_t)i*row,menu.w,row}, part;
        if (!desk_intersect(item,b,&part)) continue;
        fill(d,(struct desk_box){item.x,item.y,item.w,1},part,0xb8c9ccu);
        const char *name=i<d->program_count ? desk_program_label(d->programs[i].name) :
            i==d->program_count ? "About CiukiOS" : "Restart (unavailable)";
        if (row>=18) {
            if (i==d->program_count) portrait_at(d,part,item.x+6,item.y+(row-16)/2,16);
            else icon(d,part,item.x+6,item.y+(row-16)/2,i<d->program_count ?
                      program_icon(d,d->programs[i].name) : shell_icon(d,"menu:restart"));
        }
        desk_text(d,part,item.x+28,item.y+(row-7)/2,name,1,i==d->program_count+1 ? 0x607a85u : 0x213c4cu);
    }
}
void desk_composite(struct desktop *d, struct desk_box clip)
{
    if (!desk_intersect(clip,(struct desk_box){0,0,(int32_t)d->width,(int32_t)d->height},&clip)) return;
    fill(d,clip,clip,DESK_BACKGROUND);
    portrait(d,clip);
    fill(d,(struct desk_box){0,0,(int32_t)d->width,DESK_BAR},clip,0x213c4cu);
    desk_text(d,clip,12,12,"CiukiOS",1,0xf6dfb6u);
    desk_text(d,clip,72,12,d->shell && d->notice[0] ? d->notice : "A modern Retro OS",1,0xffffffu);
    char clock[24];
    uint64_t s = d->now_ns / 1000000000ull;
    snprintf(clock,sizeof(clock),"%02u:%02u:%02u",(unsigned)(s/3600%100),(unsigned)(s/60%60),(unsigned)(s%60));
    if (!d->shell) desk_text(d,clip,(int32_t)d->width-60,12,clock,1,0xffffffu);
    uint64_t last = 0;
    for (unsigned n = 0; n < DESK_CLIENTS; n++) {
        int next = -1;
        for (int i = 0; i < DESK_CLIENTS; i++) if (d->clients[i].window && !d->clients[i].minimized && d->clients[i].order > last &&
            (next < 0 || d->clients[i].order < d->clients[next].order)) next = i;
        if (next < 0) break;
        last = d->clients[next].order; window(d,next,clip);
    }
    if (d->shell) desk_shell_draw(d,clip);
    for (int y = 0; y < 16; y++) {
        int w = y < 11 ? y/2+1 : 3;
        fill(d,(struct desk_box){d->mouse_x,d->mouse_y+y,w+2,1},clip,0x132936u);
        if (w > 1) fill(d,(struct desk_box){d->mouse_x+1,d->mouse_y+y,w,1},clip,0xffffffu);
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
