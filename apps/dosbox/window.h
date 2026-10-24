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
    int font_menu, font_choice;
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
        if (w->selecting) db_select(w);
        return w->properties || w->font_menu || w->marking || w->mouse_y<DOSBOX_TOOLBAR;
    }
    if (e->type==CIUKI_INPUT_KEY && (e->code==CIUKI_KEY_LEFTSHIFT || e->code==CIUKI_KEY_RIGHTSHIFT)) w->shift=e->value!=0;
    if (e->type==CIUKI_INPUT_RESYNC) { w->shift=0; w->selecting=0; w->pressed=0; return w->properties || w->font_menu || w->marking; }
    if (w->font_menu) {
        int choose=0;
        if (e->type==CIUKI_INPUT_BUTTON && e->code==CIUKI_BUTTON_LEFT && e->value) {
            if (w->mouse_x>=480 && w->mouse_x<640 && w->mouse_y>=28 && w->mouse_y<106) {
                w->font_choice=(w->mouse_y-28)/26; choose=1;
            }
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
            if (y>=56 && y<82 && x>=32 && x<607) db_page(w,(x-32)/115);
            else if (y>=384 && y<414 && x>=336 && x<606) {
                int button=(x-336)/90;
                if (button==1) { w->properties=0; w->dirty=1; }
                else *action=db_apply(w,button==0);
            } else {
                int row=0;
                for (unsigned i=0;i<DB_FIELD_COUNT;i++) if ((int)db_fields[i].page==w->page) {
                    if (y>=100+row*43 && y<135+row*43 && x>=32 && x<608) {
                        w->field=(int)i;
                        if (!db_text_field(i)) { uint32_t *n=db_value(&w->draft,i); *n=(*n+1)%(db_fields[i].maximum+1); }
                        w->dirty=1; break;
                    }
                    row++;
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
                int index=w->mouse_x/80;
                if (e->value) { w->pressed=index+1; w->dirty=1; }
                else if (w->pressed==index+1) {
                    w->pressed=0; w->dirty=1;
                    if (index==0) *action=DB_MARK;
                    if (index==1) *action=DB_COPY;
                    if (index==2) *action=DB_PASTE;
                    if (index==3) *action=DB_FULLSCREEN;
                    if (index==4) db_properties(w);
                    if (index==5) { w->profile.background=!w->profile.background; w->draft=w->profile; *action=DB_APPLY; }
                    if (index==6) { w->font_menu=1; w->font_choice=(int)w->profile.zoom; }
                    if (index==7) { db_properties(w); db_page(w,1); }
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
static inline void db_box(struct du_canvas *c, struct du_rect b, const char *label, int active)
{
    du_fill(c,b,active ? DU_SELECT : DU_FACE); du_edge(c,b,DU_RAISED);
    du_text_fit(c,b.x+5,b.y+7,b.w-10,label,active ? DU_SELECT_INK : DU_INK);
}
static inline void db_paint_rows(struct dosbox_window *w, struct du_canvas *c, const uint32_t *guest,
                                 const uint32_t *source_rows, uint32_t *damage_rows)
{
    for (int y=DOSBOX_TOOLBAR;y<480;y++) {
        int sy=db_source_y(w,y);
        if (source_rows && (w->properties || w->font_menu || (w->notice[0] && y>=458) ||
            !(source_rows[sy/32]&(1u<<(sy%32))))) continue;
        if (damage_rows) damage_rows[y/32]|=1u<<(y%32);
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
    const char *labels[]={w->marking ? "Mark *" : "Mark","Copy","Paste","Full screen","Properties",w->profile.background ? "Background" : "Paused bg",db_zoom[w->profile.zoom],"Font"};
    for (int i=0;i<8;i++) db_box(c,(struct du_rect){i*80,0,80,28},labels[i],w->pressed==i+1);
    if (w->font_menu) for (int i=0;i<3;i++) db_box(c,(struct du_rect){480,28+i*26,160,26},db_zoom[i],w->font_choice==i);
    if (!w->properties) {
        if (w->notice[0]) { du_fill(c,(struct du_rect){0,458,640,22},DU_FACE); du_text_fit(c,5,464,630,w->notice,DU_INK); }
        w->dirty=0; return;
    }
    du_fill(c,(struct du_rect){24,32,592,398},DU_FACE); du_edge(c,(struct du_rect){24,32,592,398},DU_RAISED);
    du_text_fit(c,32,38,576,"DOS Program Properties",DU_INK);
    for (int i=0;i<5;i++) db_box(c,(struct du_rect){32+i*115,56,115,26},db_pages[i],w->page==i);
    int row=0;
    for (unsigned i=0;i<DB_FIELD_COUNT;i++) if ((int)db_fields[i].page==w->page) {
        int y=100+row++*43; char value[112];
        du_text_fit(c,36,y,260,db_fields[i].name,DU_INK);
        if (db_text_field(i)) snprintf(value,sizeof(value),"%s",db_text(&w->draft,i));
        else if (db_fields[i].choices) snprintf(value,sizeof(value),"%s",db_fields[i].choices[*db_value(&w->draft,i)]);
        else if (db_fields[i].maximum==1) snprintf(value,sizeof(value),"[%c]",*db_value(&w->draft,i) ? 'X' : ' ');
        else snprintf(value,sizeof(value),"%u",*db_value(&w->draft,i));
        db_box(c,(struct du_rect){300,y-3,300,28},value,w->field==(int)i);
    }
    const char *help[]={"Command, folder and batch take effect next launch. F6 changes page.",
        "VGA raster font; guest glyphs and colours are preserved. Wheel over toolbar scrolls.",
        "Conventional, EMS, XMS and DPMI amounts: automatic. Protected memory: VMM managed.",
        "Exclusive uses the program profile and startup batch. Alt+Enter returns to the desktop.",
        "Higher idle sensitivity detects empty keyboard polling sooner. Tab selects a field."};
    du_text_wrap(c,(struct du_rect){36,320,564,40},help[w->page],DU_INK);
    if (w->notice[0]) du_text_fit(c,36,363,564,w->notice,DU_INK);
    db_box(c,(struct du_rect){336,384,84,30},"OK",0);
    db_box(c,(struct du_rect){426,384,84,30},"Cancel",0);
    db_box(c,(struct du_rect){516,384,84,30},"Apply",0);
    w->dirty=0;
}
static inline void db_paint(struct dosbox_window *w, struct du_canvas *c, const uint32_t *guest)
{ db_paint_rows(w,c,guest,NULL,NULL); }
#endif
