/* SPDX-License-Identifier: MIT
 * Integer-only CPU compositor. All drawing is clipped before pixel access. */
#include "desktop.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    if (t->shadow_size) desk_paint_shadow(&p,f.x,f.y,f.w,f.h,t->shadow_size,t->drop_shadow);
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
    if (c->internal || c->collapsed) desk_paint_frame(&p,f.x,f.y,f.w,f.h,&s);
    else desk_paint_frame_border(&p,f.x,f.y,f.w,f.h,&s);
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
    if (desk_intersect(content,old,&b) && desk_intersect(b,clip,&b))
        for (int32_t y=b.y; y<b.y+b.h; y++) for (int32_t x=b.x; x<b.x+b.w; x++)
            d->pixels[(uint32_t)y*d->width+(uint32_t)x] =
                c->pixels[(uint32_t)(y-content.y)*c->info.width+(uint32_t)(x-content.x)] & 0xffffffu;
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
static void scene(struct desktop *d, struct desk_box clip)
{
    if (!desk_intersect(clip,(struct desk_box){0,0,(int32_t)d->width,(int32_t)d->height},&clip)) return;
    const struct desk_theme *t=desk_theme;
    struct desk_paint p=paint_of(d,clip);
    if (d->shell && desk_shell_background) desk_shell_background(d,clip);   /* wallpaper and desktop icons (shell.c) */
    else { fill(d,clip,clip,t->desktop); portrait(d,clip); }
    /* Top bar: a Platinum-style menu bar with the Ciuki mark. */
    fill(d,(struct desk_box){0,0,(int32_t)d->width,DESK_BAR},clip,t->topbar);
    desk_paint_fill(&p,0,DESK_BAR-1,(int)d->width,1,t->topbar_edge);
    desk_paint_fill(&p,0,0,(int)d->width,1,desk_mix(0xffffffu,t->topbar,160));
    if (d->shell && desk_shell_topbar) desk_shell_topbar(d,clip);
    else {
        portrait_at(d,clip,8,6,20);
        desk_paint_text(&p,t->font_bold,34,20,"CiukiOS",t->topbar_text);   /* no tagline in the top bar (owner, 2026-10-12) */
    }
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
}
static void cursor(struct desktop *d, struct desk_box clip)
{
    for (int y = 0; y < 17; y++) for (int i = 0; i < 11; i++) {
        char k=arrow[y][i];
        if (k!='.') fill(d,(struct desk_box){d->mouse_x+i,d->mouse_y+y,1,1},clip,k=='#' ? 0x101010u : 0xffffffu);
    }
}
/* Reference/capture renderer: the runtime keeps this arrow out of the scene. */
void desk_composite(struct desktop *d, struct desk_box clip)
{ scene(d,clip); cursor(d,clip); }
void desk_render_release(struct desktop *d)
{
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
    if (desk_intersect((struct desk_box){d->mouse_x,d->mouse_y,11,17},
                       (struct desk_box){0,0,(int)d->width,(int)d->height},&b)) {
        for (int y=0;y<b.h;y++)
            memcpy(d->cursor_under+y*11,output+(size_t)(b.y+y)*d->width+b.x,(size_t)b.w*4);
        d->cursor_box=b; d->cursor_valid=1; cursor(d,b);
        if (d->cursor_dirty) desk_damage_add(&d->present_damage,b,d->width,d->height);
    } else d->cursor_valid=0;
    d->cursor_dirty=0;
    s->composite_ns=time_now(d)-begin;
    for (unsigned i=0;i<d->present_damage.count;i++)
        s->damage_px+=(uint64_t)d->present_damage.boxes[i].w*d->present_damage.boxes[i].h;
    begin=time_now(d);
    unsigned done = 0;
    for (; done < d->present_damage.count; done++) {
        struct desk_box b = d->present_damage.boxes[done];
        struct ciuki_rect r = {b.x,b.y,b.x,b.y,(uint32_t)b.w,(uint32_t)b.h};
        if (d->ops.present(d->display,d->output,&r)) break;
        d->presents++;
        s->presents++; s->lfb_bytes+=(uint64_t)b.w*b.h*(d->lfb_bytes_per_pixel ? d->lfb_bytes_per_pixel : 4);
    }
    s->present_ns=time_now(d)-begin;
    s->input_ns=d->pending_input_ns; s->input_lag_ns=d->pending_input_lag_ns;
    d->pending_input_ns=d->pending_input_lag_ns=0;
    d->stats.frames++; d->stats.damage_px+=s->damage_px; d->stats.scene_px+=s->scene_px;
    d->stats.presents+=s->presents; d->stats.lfb_bytes+=s->lfb_bytes;
    d->stats.composite_ns+=s->composite_ns; d->stats.present_ns+=s->present_ns;
    int complete=done==d->present_damage.count;
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
