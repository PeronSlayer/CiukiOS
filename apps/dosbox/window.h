/* G17 DOS window controls. Uses the existing desktop theme and primitives.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_DOSBOX_WINDOW_H
#define CIUKI_DOSBOX_WINDOW_H
#include "program.h"
#include "../libdeskui/deskui.h"
#include <stddef.h>

#define DOSBOX_TOOLBAR 28
#define DOSBOX_VIEW_HEIGHT (480-DOSBOX_TOOLBAR)
#define DOSBOX_PROFILE_KEYS (DOSBOX_KEYS_MAX*4u)
#define DOSBOX_PASTE_KEYS 4096u
enum dosbox_action { DB_NONE, DB_MARK, DB_COPY, DB_PASTE, DB_FULLSCREEN, DB_APPLY, DB_CANCEL_MARK };
struct dosbox_window {
    struct dosbox_profile profile, draft;
    int properties, page, field, dirty, mouse_x, mouse_y, pressed, shift;
    int font_menu, font_choice, hover;
    int marking, selecting, selected, x0, y0, x1, y1;
    unsigned columns, rows, scroll_x, scroll_y;
    char notice[128];
};
struct dosbox_field { const char *name; unsigned page, offset, maximum; const char *const *choices; };
static const char *const db_zoom[]={"Fit","100%","125%"};
static const char *const db_memory[]={"Automatic (paged)","Locked (compatibility)"};
static const char *const db_screen[]={"Window","Full screen","Exclusive full screen"};
#define DB_FIELD(name,page,member,max,choices) {name,page,offsetof(struct dosbox_profile,member),max,choices}
static const struct dosbox_field db_fields[]={
    DB_FIELD("Command line",0,command,96,NULL),
    DB_FIELD("Working directory",0,directory,93,NULL),
    DB_FIELD("Startup batch file",0,batch,96,NULL),
    DB_FIELD("Close on exit",0,close_on_exit,1,NULL),
    DB_FIELD("Font size",1,zoom,2,db_zoom),
    DB_FIELD("XMS / EMS / DPMI pool",2,memory,1,db_memory),
    DB_FIELD("Start in",3,screen,2,db_screen),
    DB_FIELD("Continue in background",4,background,1,NULL),
    DB_FIELD("QuickEdit",4,quickedit,1,NULL),
    DB_FIELD("Allow fast pasting",4,fast_paste,1,NULL),
    DB_FIELD("Warn if still active",4,warn,1,NULL),
    DB_FIELD("Idle sensitivity (0-100)",4,idle,100,NULL),
};
#undef DB_FIELD
#define DB_FIELD_COUNT (sizeof(db_fields)/sizeof(*db_fields))
static const char *const db_pages[]={"Program","Font","Memory","Screen","Misc"};
static inline int db_text_field(unsigned i) { return i<3; }
/* Toolbar (Windows 98 MS-DOS Prompt order): the font size combo, then icon
 * buttons Mark, Copy, Paste | Full screen | Properties, Background, Font.
 * Ids keep the earlier eight-slot numbering; id 6 is the combo. */
enum { DB_TOOL_MARK, DB_TOOL_COPY, DB_TOOL_PASTE, DB_TOOL_FULL, DB_TOOL_PROPS, DB_TOOL_BACKGROUND, DB_TOOL_ZOOM, DB_TOOL_FONT, DB_TOOLS };
struct db_tool { int x, w; const char *icon, *glyph, *tip; };
static const struct db_tool db_tools[DB_TOOLS]={
    {112,24,"edit-select-all","M","Mark text"},
    {136,24,"edit-copy","C","Copy"},
    {160,24,"edit-paste","P","Paste"},
    {192,24,"view-fullscreen","F","Full screen"},
    {224,24,"document-properties","Pr","Properties"},
    {248,24,"preferences-desktop-wallpaper","B","Background"},
    {4,100,NULL,NULL,"Font size"},
    {272,24,"preferences-desktop-font","A","Font settings"},
};
#define DB_COMBO_ITEM 22
static inline int db_tool_at(int x, int y)
{
    if (y<0 || y>=DOSBOX_TOOLBAR) return -1;
    for (int i=0;i<DB_TOOLS;i++) if (x>=db_tools[i].x && x<db_tools[i].x+db_tools[i].w) return i;
    return -1;
}
static inline int db_font_menu_item(int x, int y)
{
    if (x<db_tools[DB_TOOL_ZOOM].x || x>=db_tools[DB_TOOL_ZOOM].x+db_tools[DB_TOOL_ZOOM].w ||
        y<DOSBOX_TOOLBAR || y>=DOSBOX_TOOLBAR+3*DB_COMBO_ITEM) return -1;
    return (y-DOSBOX_TOOLBAR)/DB_COMBO_ITEM;
}
/* Properties sheet geometry shared by painting and hit-testing. */
#define DB_SX 40
#define DB_SY 36
#define DB_SW 560
#define DB_SH 404
#define DB_TAB_X (DB_SX+10)
#define DB_TAB_Y (DB_SY+26)
#define DB_TAB_W 80
#define DB_TAB_H 20
#define DB_PAGE_X (DB_SX+8)
#define DB_PAGE_Y (DB_TAB_Y+DB_TAB_H-1)
#define DB_PAGE_W (DB_SW-16)
#define DB_PAGE_H (DB_SY+DB_SH-44-DB_PAGE_Y)
#define DB_BUTTON_Y (DB_SY+DB_SH-34)
#define DB_BUTTON_W 75
#define DB_BUTTON_H 23
enum { DB_KIND_TEXT, DB_KIND_COMBO, DB_KIND_CHECK, DB_KIND_SLIDER };
static inline int db_text_field(unsigned i);
static inline int db_kind(unsigned i)
{ return i<3 ? DB_KIND_TEXT : db_fields[i].choices ? DB_KIND_COMBO : db_fields[i].maximum==1 ? DB_KIND_CHECK : DB_KIND_SLIDER; }
static inline struct du_rect db_button_rect(int n)
{ return (struct du_rect){DB_SX+DB_SW-10-(3-n)*DB_BUTTON_W-(2-n)*6,DB_BUTTON_Y,DB_BUTTON_W,DB_BUTTON_H}; }
static inline struct du_rect db_row(unsigned field)
{
    int y=DB_PAGE_Y+14;
    for (unsigned i=0;i<field;i++) if (db_fields[i].page==db_fields[field].page) y+=db_kind(i)==DB_KIND_TEXT ? 52 : 30;
    return (struct du_rect){DB_PAGE_X+8,y,DB_PAGE_W-16,db_kind(field)==DB_KIND_TEXT ? 40 : 22};
}
static inline struct du_rect db_control(unsigned field)
{
    struct du_rect r=db_row(field);
    switch (db_kind(field)) {
    case DB_KIND_TEXT: return (struct du_rect){r.x+8,r.y+16,r.w-16,22};
    case DB_KIND_COMBO: return (struct du_rect){r.x+208,r.y,240,22};
    case DB_KIND_SLIDER: return (struct du_rect){r.x+208,r.y,200,22};
    default: return (struct du_rect){r.x+8,r.y+4,13,13};
    }
}
static inline int db_in(struct du_rect r, int x, int y)
{ return x>=r.x && x<r.x+r.w && y>=r.y && y<r.y+r.h; }

static inline uint32_t *db_value(struct dosbox_profile *p, unsigned i)
{ return (uint32_t *)((unsigned char *)p+db_fields[i].offset); }
static inline char *db_text(struct dosbox_profile *p, unsigned i)
{ return (char *)p+db_fields[i].offset; }
static inline void db_notice(struct dosbox_window *w, const char *text)
{ snprintf(w->notice,sizeof(w->notice),"%s",text); w->dirty=1; }
static inline void db_properties(struct dosbox_window *w)
{
    w->draft=w->profile; w->properties=1; w->page=0; w->field=0; w->dirty=1;
    w->notice[0]=0;
}
static inline void db_page(struct dosbox_window *w, int page)
{
    w->page=(page+5)%5;
    for (unsigned i=0;i<DB_FIELD_COUNT;i++) if ((int)db_fields[i].page==w->page) { w->field=(int)i; break; }
    w->dirty=1;
}
static inline int db_apply(struct dosbox_window *w, int close_sheet)
{
    if (dosbox_profile_save(&w->draft)) { db_notice(w,"Cannot save properties. Check command, paths and available disk space."); return DB_NONE; }
    w->profile=w->draft; w->properties=!close_sheet; w->dirty=1;
    db_notice(w,"Saved. Program, memory and startup screen changes apply on the next launch.");
    return DB_APPLY;
}
static inline int db_source_x(const struct dosbox_window *w, int x)
{
    unsigned scale=w->profile.zoom==2 ? 125u : 100u;
    int at=x<0 ? 0 : x>639 ? 639 : x;
    unsigned value=(unsigned)at*100u/scale+w->scroll_x;
    return (int)(value>639 ? 639 : value);
}
static inline int db_source_y(const struct dosbox_window *w, int y)
{
    int at=y-DOSBOX_TOOLBAR;
    if (at<0) at=0;
    if (at>=DOSBOX_VIEW_HEIGHT) at=DOSBOX_VIEW_HEIGHT-1;
    unsigned value=w->profile.zoom ? (unsigned)at*100u/(w->profile.zoom==2 ? 125u : 100u)+w->scroll_y :
        (unsigned)at*480u/DOSBOX_VIEW_HEIGHT;
    return (int)(value>479 ? 479 : value);
}
static inline void db_select(struct dosbox_window *w)
{
    w->x1=db_source_x(w,w->mouse_x)*(int)w->columns/640;
    w->y1=db_source_y(w,w->mouse_y)*(int)w->rows/480;
    w->selected=1; w->dirty=1;
}
/* Nonzero means consumed, with action returned separately. All modal input
 * stays out of DOS. The shell still owns Alt+Enter and close/session requests. */
static inline int db_input(struct dosbox_window *w, struct ciuki_input_event *e, int *action)
{
    *action=DB_NONE;
    if (e->type==CIUKI_INPUT_MOTION) {
        w->mouse_x=e->value; w->mouse_y=e->value2;
        int hover=db_tool_at(w->mouse_x,w->mouse_y)+1;
        if (hover!=w->hover) { w->hover=hover; w->dirty=1; }
        if (w->selecting) db_select(w);
        return w->properties || w->font_menu || w->marking || w->mouse_y<DOSBOX_TOOLBAR;
    }
    if (e->type==CIUKI_INPUT_KEY && (e->code==CIUKI_KEY_LEFTSHIFT || e->code==CIUKI_KEY_RIGHTSHIFT)) w->shift=e->value!=0;
    if (e->type==CIUKI_INPUT_RESYNC) { w->shift=0; w->selecting=0; w->pressed=0; if (w->hover) { w->hover=0; w->dirty=1; } return w->properties || w->font_menu || w->marking; }
    if (w->font_menu) {
        int choose=0;
        if (e->type==CIUKI_INPUT_BUTTON && e->code==CIUKI_BUTTON_LEFT && e->value) {
            int item=db_font_menu_item(w->mouse_x,w->mouse_y);
            if (item>=0) { w->font_choice=item; choose=1; }
            w->font_menu=0; w->dirty=1;
        } else if (e->type==CIUKI_INPUT_KEY && e->value) {
            if (e->code==CIUKI_KEY_ESCAPE) w->font_menu=0;
            if (e->code==CIUKI_KEY_UP) w->font_choice=(w->font_choice+2)%3;
            if (e->code==CIUKI_KEY_DOWN) w->font_choice=(w->font_choice+1)%3;
            if (e->code==CIUKI_KEY_ENTER) { choose=1; w->font_menu=0; }
            w->dirty=1;
        }
        if (choose) {
            w->profile.zoom=(unsigned)w->font_choice; w->scroll_x=w->scroll_y=0;
            w->draft=w->profile; *action=DB_APPLY;
        }
        return 1;
    }
    if (w->properties) {
        if (e->type==CIUKI_INPUT_BUTTON && e->code==CIUKI_BUTTON_LEFT && e->value) {
            int x=w->mouse_x, y=w->mouse_y;
            if (y>=DB_TAB_Y && y<DB_TAB_Y+DB_TAB_H && x>=DB_TAB_X && x<DB_TAB_X+5*DB_TAB_W) db_page(w,(x-DB_TAB_X)/DB_TAB_W);
            else if (y>=DB_BUTTON_Y && y<DB_BUTTON_Y+DB_BUTTON_H && x>=db_button_rect(0).x && x<db_button_rect(2).x+DB_BUTTON_W) {
                for (int button=0;button<3;button++) if (db_in(db_button_rect(button),x,y)) {
                    if (button==1) { w->properties=0; w->dirty=1; }
                    else *action=db_apply(w,button==0);
                }
            } else {
                for (unsigned i=0;i<DB_FIELD_COUNT;i++) if ((int)db_fields[i].page==w->page) {
                    struct du_rect row=db_row(i); row.y-=4; row.h+=8;
                    if (!db_in(row,x,y)) continue;
                    w->field=(int)i; w->dirty=1;
                    uint32_t *n=db_value(&w->draft,i);
                    if (db_kind(i)==DB_KIND_SLIDER) {
                        struct du_rect t=db_control(i); int v=(x-t.x-6)*(int)db_fields[i].maximum/(t.w-12);
                        *n=(uint32_t)(v<0 ? 0 : v>(int)db_fields[i].maximum ? (int)db_fields[i].maximum : v);
                    } else if (db_kind(i)!=DB_KIND_TEXT) *n=(*n+1)%(db_fields[i].maximum+1);
                    break;
                }
            }
        } else if (e->type==CIUKI_INPUT_TEXT) {
            unsigned i=(unsigned)w->field; char *s=db_text(&w->draft,i); int shift;
            if (db_text_field(i) && e->code>=32 && e->code<=126 && dosbox_key_for((char)e->code,&shift)) {
                size_t n=strlen(s); if (n<db_fields[i].maximum) { s[n]=(char)e->code; s[n+1]=0; w->dirty=1; }
            }
        } else if (e->type==CIUKI_INPUT_KEY && e->value) {
            unsigned i=(unsigned)w->field;
            if (e->code==CIUKI_KEY_ESCAPE) { w->properties=0; w->dirty=1; }
            else if (e->code==CIUKI_KEY_ENTER) *action=db_apply(w,1);
            else if (e->code==CIUKI_KEY_F6) db_page(w,w->page+(w->shift ? -1 : 1));
            else if (e->code==CIUKI_KEY_TAB) {
                do { i=(i+(w->shift ? DB_FIELD_COUNT-1 : 1))%DB_FIELD_COUNT; } while ((int)db_fields[i].page!=w->page);
                w->field=(int)i; w->dirty=1;
            } else if (e->code==CIUKI_KEY_BACKSPACE && db_text_field(i)) {
                char *s=db_text(&w->draft,i); size_t n=strlen(s); if (n) s[n-1]=0; w->dirty=1;
            } else if (!db_text_field(i) && (e->code==CIUKI_KEY_SPACE || e->code==CIUKI_KEY_RIGHT || e->code==CIUKI_KEY_LEFT)) {
                uint32_t *n=db_value(&w->draft,i), max=db_fields[i].maximum;
                *n=e->code==CIUKI_KEY_LEFT ? (*n ? *n-1 : max) : (*n+1)%(max+1); w->dirty=1;
            }
        }
        return 1;
    }
    if (w->marking && e->type==CIUKI_INPUT_KEY) {
        if (e->value) {
            if (e->code==CIUKI_KEY_ESCAPE) *action=DB_CANCEL_MARK;
            else if (e->code==CIUKI_KEY_ENTER) *action=DB_COPY;
            else {
                if (e->code==CIUKI_KEY_LEFT && w->x1>0) w->x1--;
                if (e->code==CIUKI_KEY_RIGHT && w->x1+1<(int)w->columns) w->x1++;
                if (e->code==CIUKI_KEY_UP && w->y1>0) w->y1--;
                if (e->code==CIUKI_KEY_DOWN && w->y1+1<(int)w->rows) w->y1++;
                if (!w->shift) { w->x0=w->x1; w->y0=w->y1; }
                w->selected=1; w->dirty=1;
            }
        }
        return 1;
    }
    if (e->type==CIUKI_INPUT_WHEEL && w->mouse_y<DOSBOX_TOOLBAR) {
        int64_t scroll=(int64_t)(w->shift ? w->scroll_x : w->scroll_y)-(int64_t)e->value*16;
        unsigned max=w->shift ? (w->profile.zoom==2 ? 128u : 0u) :
            w->profile.zoom==2 ? 119u : w->profile.zoom ? 28u : 0u;
        unsigned value=scroll<0 ? 0u : scroll>max ? max : (unsigned)scroll;
        if (w->shift) w->scroll_x=value; else w->scroll_y=value;
        w->dirty=1; return 1;
    }
    if (e->type==CIUKI_INPUT_BUTTON) {
        if (w->pressed && e->code==CIUKI_BUTTON_LEFT && !e->value && w->mouse_y>=DOSBOX_TOOLBAR) {
            w->pressed=0; w->dirty=1; return 1;
        }
        if (w->mouse_y<DOSBOX_TOOLBAR) {
            if (e->code==CIUKI_BUTTON_LEFT) {
                int index=db_tool_at(w->mouse_x,w->mouse_y);
                if (e->value) { w->pressed=index+1; w->dirty=1; }
                else if (index>=0 && w->pressed==index+1) {
                    w->pressed=0; w->dirty=1;
                    if (index==0) *action=DB_MARK;
                    if (index==1) *action=DB_COPY;
                    if (index==2) *action=DB_PASTE;
                    if (index==3) *action=DB_FULLSCREEN;
                    if (index==4) db_properties(w);
                    if (index==5) { w->profile.background=!w->profile.background; w->draft=w->profile; *action=DB_APPLY; }
                    if (index==DB_TOOL_ZOOM) { w->font_menu=1; w->font_choice=(int)w->profile.zoom; }
                    if (index==DB_TOOL_FONT) { db_properties(w); db_page(w,1); }
                }
            }
            return 1;
        }
        if (!w->marking && w->profile.quickedit && e->code==CIUKI_BUTTON_LEFT && e->value) { *action=DB_MARK; return 1; }
        if (w->marking) {
            if (e->code==CIUKI_BUTTON_LEFT) {
                w->selecting=e->value!=0; db_select(w);
                if (e->value) { w->x0=w->x1; w->y0=w->y1; }
            }
            if (e->code==CIUKI_BUTTON_RIGHT && e->value) *action=w->selected ? DB_COPY : DB_PASTE;
            return 1;
        }
    }
    return w->marking;
}
static inline void db_rect(struct du_canvas *c, int x, int y, int w, int h, uint32_t colour)
{ du_fill(c,(struct du_rect){x,y,w,h},colour); }
static inline void db_outline(struct du_canvas *c, struct du_rect r, uint32_t colour)
{
    db_rect(c,r.x,r.y,r.w,1,colour); db_rect(c,r.x,r.y+r.h-1,r.w,1,colour);
    db_rect(c,r.x,r.y,1,r.h,colour); db_rect(c,r.x+r.w-1,r.y,1,r.h,colour);
}
static inline void db_dots(struct du_canvas *c, struct du_rect r)
{
    for (int x=r.x;x<r.x+r.w;x+=2) { db_rect(c,x,r.y,1,1,DU_INK); db_rect(c,x,r.y+r.h-1,1,1,DU_INK); }
    for (int y=r.y;y<r.y+r.h;y+=2) { db_rect(c,r.x,y,1,1,DU_INK); db_rect(c,r.x+r.w-1,y,1,1,DU_INK); }
}
static inline void db_arrow(struct du_canvas *c, int cx, int cy, uint32_t colour)
{ for (int i=0;i<4;i++) db_rect(c,cx-3+i,cy-1+i,7-2*i,1,colour); }
static inline void db_button(struct du_canvas *c, struct du_rect b, const char *label, int def, int pressed)
{
    du_fill(c,b,DU_FACE); du_edge(c,b,pressed ? DU_PRESSED : DU_BUTTON_EDGE);
    if (def) db_outline(c,(struct du_rect){b.x-1,b.y-1,b.w+2,b.h+2},DU_INK);
    du_text_fit(c,b.x+(b.w-du_text_width(label))/2+pressed,b.y+7+pressed,b.w-4,label,DU_INK);
}
static inline void db_edit(struct du_canvas *c, struct du_rect b, const char *text, int focused)
{
    du_fill(c,b,DU_WINDOW); du_edge(c,b,DU_SUNKEN);
    int room=b.w-10; const char *s=text;
    while (*s && du_text_width(s)>room) s++;
    du_text_fit(c,b.x+4,b.y+7,room,s,DU_INK);
    if (focused) { int x=b.x+4+du_text_width(s); if (x>b.x+b.w-4) x=b.x+b.w-4; db_rect(c,x,b.y+4,1,14,DU_INK); }
}
static inline void db_combo(struct du_canvas *c, struct du_rect b, const char *text, int focused, int open)
{
    du_fill(c,b,DU_WINDOW); du_edge(c,b,DU_SUNKEN);
    struct du_rect field={b.x+2,b.y+2,b.w-20,b.h-4};
    if (focused || open) db_rect(c,field.x,field.y,field.w,field.h,DU_SELECT);
    du_text_fit(c,b.x+5,b.y+7,field.w-4,text,focused || open ? DU_SELECT_INK : DU_INK);
    struct du_rect arrow={b.x+b.w-18,b.y+2,16,b.h-4};
    du_fill(c,arrow,DU_FACE); du_edge(c,arrow,open ? DU_PRESSED : DU_BUTTON_EDGE);
    db_arrow(c,arrow.x+8+(open ? 1 : 0),arrow.y+arrow.h/2-1+(open ? 1 : 0),DU_INK);
}
static inline void db_checkbox(struct du_canvas *c, struct du_rect b, int value)
{
    du_fill(c,b,DU_WINDOW); du_edge(c,b,DU_SUNKEN);
    if (value) {
        static const int8_t tick[][2]={{2,5},{3,6},{4,7},{5,6},{6,5},{7,4},{8,3},{9,2}};
        for (unsigned k=0;k<sizeof(tick)/sizeof(*tick);k++) db_rect(c,b.x+tick[k][0]+1,b.y+tick[k][1]+0,1,3,DU_INK);
    }
}
static inline void db_slider(struct du_canvas *c, struct du_rect b, unsigned value, unsigned maximum, int focused)
{
    struct du_rect groove={b.x+6,b.y+9,b.w-12,4};
    du_edge(c,groove,DU_SUNKEN_THIN);
    int travel=b.w-12-11; int tx=b.x+6+(int)((unsigned)travel*value/(maximum ? maximum : 1));
    struct du_rect thumb={tx,b.y+1,11,20};
    du_fill(c,thumb,DU_FACE); du_edge(c,thumb,DU_RAISED);
    if (focused) db_dots(c,(struct du_rect){b.x,b.y,b.w,b.h});
}
static inline int db_tip_visible(const struct dosbox_window *w)
{ return w->hover && !w->pressed && !w->properties && !w->font_menu; }
#define DB_TIP_BOTTOM (DOSBOX_TOOLBAR+22)
static inline void db_paint_toolbar(struct dosbox_window *w, struct du_canvas *c)
{
    du_fill(c,(struct du_rect){0,0,640,DOSBOX_TOOLBAR},DU_FACE);
    db_rect(c,0,DOSBOX_TOOLBAR-1,640,1,DU_SHADOW);
    for (int x=187;x<=219;x+=32) { db_rect(c,x,4,1,20,DU_SHADOW); db_rect(c,x+1,4,1,20,DU_HILITE); }
    for (int i=0;i<DB_TOOLS;i++) {
        const struct db_tool *t=&db_tools[i];
        int hot=w->hover==i+1, down=w->pressed==i+1 && hot;
        if (i==DB_TOOL_ZOOM) {
            db_combo(c,(struct du_rect){t->x,3,t->w,22},db_zoom[w->profile.zoom],0,w->font_menu);
            continue;
        }
        int on=down || (i==DB_TOOL_MARK && w->marking) || (i==DB_TOOL_BACKGROUND && w->profile.background);
        struct du_rect b={t->x,3,t->w,22};
        if (on) { du_fill(c,b,DU_FACE); du_edge(c,b,DU_SUNKEN_THIN); }
        else if (hot) du_edge(c,b,DU_RAISED_THIN);
        const struct du_icon *icon=du_icon(t->icon,16);
        int off=on ? 1 : 0;
        if (icon) du_icon_draw(c,b.x+4+off,b.y+3+off,icon,DU_ICON_NORMAL);
        else du_text_fit(c,b.x+6+off,b.y+7+off,16,t->glyph,DU_INK);
    }
}
static inline void db_paint_tip(struct dosbox_window *w, struct du_canvas *c)
{
    if (!db_tip_visible(w)) return;
    const struct db_tool *t=&db_tools[w->hover-1];
    const char *text=t->tip;
    if (w->hover-1==DB_TOOL_BACKGROUND) text=w->profile.background ? "Background: keeps running" : "Background: paused";
    if (w->hover-1==DB_TOOL_MARK && w->marking) text="Mark text (active)";
    int width=du_text_width(text)+10, x=t->x+t->w/2-width/2;
    if (x<0) x=0;
    if (x+width>640) x=640-width;
    struct du_rect r={x,DOSBOX_TOOLBAR+2,width,18};
    du_fill(c,r,0xffffe1u); db_outline(c,r,0);
    du_text_fit(c,r.x+5,r.y+5,width-8,text,0);
}
static inline void db_paint_sheet(struct dosbox_window *w, struct du_canvas *c)
{
    struct du_rect sheet={DB_SX,DB_SY,DB_SW,DB_SH};
    du_fill(c,sheet,DU_FACE); du_edge(c,sheet,DU_RAISED);
    db_rect(c,DB_SX+3,DB_SY+3,DB_SW-6,18,DU_SELECT);
    du_text_fit(c,DB_SX+8,DB_SY+8,DB_SW-20,"DOS Program Properties",DU_SELECT_INK);
    /* Page panel, then tabs (the active one joins the page). */
    struct du_rect page={DB_PAGE_X,DB_PAGE_Y,DB_PAGE_W,DB_PAGE_H};
    db_rect(c,page.x,page.y,page.w,1,DU_HILITE); db_rect(c,page.x,page.y,1,page.h,DU_HILITE);
    db_rect(c,page.x+page.w-1,page.y,1,page.h,DU_DARK); db_rect(c,page.x+page.w-2,page.y+1,1,page.h-2,DU_SHADOW);
    db_rect(c,page.x,page.y+page.h-1,page.w,1,DU_DARK); db_rect(c,page.x+1,page.y+page.h-2,page.w-2,1,DU_SHADOW);
    for (int i=0;i<5;i++) {
        int sel=w->page==i, x=DB_TAB_X+i*DB_TAB_W;
        struct du_rect r={x-(sel ? 2 : 0),DB_TAB_Y+(sel ? 0 : 2),DB_TAB_W+(sel ? 4 : 0),DB_TAB_H-(sel ? 0 : 2)};
        du_fill(c,r,DU_FACE);
        db_rect(c,r.x+2,r.y,r.w-4,1,DU_HILITE); db_rect(c,r.x,r.y+2,1,r.h-2,DU_HILITE);
        db_rect(c,r.x+1,r.y+1,1,1,DU_HILITE);
        db_rect(c,r.x+r.w-1,r.y+2,1,r.h-2,DU_DARK); db_rect(c,r.x+r.w-2,r.y+1,1,r.h-1,DU_SHADOW);
        if (sel) db_rect(c,r.x+1,DB_PAGE_Y,r.w-3,1,DU_FACE);
        int tx=x+(DB_TAB_W-du_text_width(db_pages[i]))/2, ty=DB_TAB_Y+6+(sel ? 0 : 1);
        du_text_fit(c,tx,ty,DB_TAB_W-4,db_pages[i],DU_INK);
        if (sel) db_dots(c,(struct du_rect){tx-3,ty-3,du_text_width(db_pages[i])+6,15});
    }
    for (unsigned i=0;i<DB_FIELD_COUNT;i++) if ((int)db_fields[i].page==w->page) {
        struct du_rect row=db_row(i), ctl=db_control(i); int focus=w->field==(int)i;
        int kind=db_kind(i);
        if (kind==DB_KIND_CHECK) {
            db_checkbox(c,ctl,(int)*db_value(&w->draft,i));
            int lx=ctl.x+ctl.w+7, lw=du_text_width(db_fields[i].name);
            du_text_fit(c,lx,ctl.y+2,row.w-30,db_fields[i].name,DU_INK);
            if (focus) db_dots(c,(struct du_rect){lx-3,ctl.y-2,lw+6,17});
            continue;
        }
        du_text_fit(c,row.x+8,kind==DB_KIND_TEXT ? row.y+1 : row.y+7,190,db_fields[i].name,DU_INK);
        if (kind==DB_KIND_TEXT) db_edit(c,ctl,db_text(&w->draft,i),focus);
        else if (kind==DB_KIND_COMBO) db_combo(c,ctl,db_fields[i].choices[*db_value(&w->draft,i)],focus,0);
        else {
            char value[16]; snprintf(value,sizeof(value),"%u",*db_value(&w->draft,i));
            db_slider(c,ctl,*db_value(&w->draft,i),db_fields[i].maximum,focus);
            du_text_fit(c,ctl.x+ctl.w+10,ctl.y+7,40,value,DU_INK);
        }
    }
    static const char *const help[]={"Command, folder and batch take effect next launch. F6 changes page.",
        "VGA raster font; guest glyphs and colours are preserved. Wheel over toolbar scrolls.",
        "Conventional, EMS, XMS and DPMI amounts: automatic. Protected memory: VMM managed.",
        "Exclusive uses the program profile and startup batch. Alt+Enter returns to the desktop.",
        "Higher idle sensitivity detects empty keyboard polling sooner. Tab selects a field."};
    db_rect(c,page.x+10,page.y+page.h-72,page.w-20,1,DU_SHADOW); db_rect(c,page.x+10,page.y+page.h-71,page.w-20,1,DU_HILITE);
    du_text_wrap(c,(struct du_rect){page.x+12,page.y+page.h-64,page.w-24,32},help[w->page],DU_INK);
    if (w->notice[0]) du_text_fit(c,page.x+12,page.y+page.h-24,page.w-24,w->notice,DU_INK);
    db_button(c,db_button_rect(0),"OK",1,0);
    db_button(c,db_button_rect(1),"Cancel",0,0);
    db_button(c,db_button_rect(2),"Apply",0,0);
}
static inline void db_paint_rows(struct dosbox_window *w, struct du_canvas *c, const uint32_t *guest,
                                 const uint32_t *source_rows, uint32_t *damage_rows)
{
    /* Fit/100% preserve horizontal pixels: copy each selected scanline.
     * 125% resolves horizontal coordinates once per paint, not per pixel.
     * Selection retains the scalar path and its guest-cell coordinates. */
    int plain=!(w->marking && w->selected);
    unsigned sx0=(unsigned)db_source_x(w,0);
    uint16_t columns[640];
    if (plain && w->profile.zoom==2)
        for (int x=0;x<640;x++) columns[x]=(uint16_t)db_source_x(w,x);
    for (int y=DOSBOX_TOOLBAR;y<480;y++) {
        int sy=db_source_y(w,y);
        if (source_rows && (w->properties || w->font_menu || (w->notice[0] && y>=458) || (db_tip_visible(w) && y<DB_TIP_BOTTOM) ||
            !(source_rows[sy/32]&(1u<<(sy%32))))) continue;
        if (damage_rows) damage_rows[y/32]|=1u<<(y%32);
        if (plain) {
            uint32_t *dest=c->pixels+y*640;
            const uint32_t *src=guest+sy*640;
            if (w->profile.zoom==2) for (int x=0;x<640;x++) dest[x]=src[columns[x]];
            else {
                unsigned count=640-sx0;
                memcpy(dest,src+sx0,count*sizeof(*dest));
                for (unsigned x=count;x<640;x++) dest[x]=src[639];
            }
            continue;
        }
        for (int x=0;x<640;x++) {
            int sx=db_source_x(w,x); uint32_t pixel=guest[sy*640+sx];
            if (w->marking && w->selected) {
                int col=sx*(int)w->columns/640, row=sy*(int)w->rows/480;
                int left=w->x0<w->x1 ? w->x0 : w->x1, right=w->x0>w->x1 ? w->x0 : w->x1;
                int top=w->y0<w->y1 ? w->y0 : w->y1, bottom=w->y0>w->y1 ? w->y0 : w->y1;
                if (col>=left && col<=right && row>=top && row<=bottom) pixel^=0xFFFFFF;
            }
            c->pixels[y*640+x]=pixel;
        }
    }
    if (source_rows) return;
    db_paint_toolbar(w,c);
    if (w->font_menu) {
        const struct db_tool *t=&db_tools[DB_TOOL_ZOOM];
        struct du_rect list={t->x,DOSBOX_TOOLBAR,t->w,3*DB_COMBO_ITEM+2};
        du_fill(c,list,DU_WINDOW); db_outline(c,list,DU_DARK);
        for (int i=0;i<3;i++) {
            struct du_rect row={list.x+1,list.y+1+i*DB_COMBO_ITEM,list.w-2,DB_COMBO_ITEM};
            if (w->font_choice==i) du_fill(c,row,DU_SELECT);
            du_text_fit(c,row.x+5,row.y+7,row.w-8,db_zoom[i],w->font_choice==i ? DU_SELECT_INK : DU_INK);
        }
    }
    if (!w->properties) {
        if (w->notice[0]) { du_fill(c,(struct du_rect){0,458,640,22},DU_FACE); du_text_fit(c,5,464,630,w->notice,DU_INK); }
        db_paint_tip(w,c);
        w->dirty=0; return;
    }
    db_paint_sheet(w,c);
    w->dirty=0;
}
static inline void db_paint(struct dosbox_window *w, struct du_canvas *c, const uint32_t *guest)
{ db_paint_rows(w,c,guest,NULL,NULL); }
#endif
