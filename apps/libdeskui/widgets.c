/* SPDX-License-Identifier: MIT */
#include "deskui.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
static char clipboard[DU_TEXT_MAX];
void du_clipboard_set(const char *s) { snprintf(clipboard,sizeof(clipboard),"%s",s?s:""); }
const char *du_clipboard_get(void) { return clipboard; }
static void emit(struct du_ui *u,enum du_event_kind type,int id,int index)
{
    struct du_event e={type,id,index,u->mouse_x,u->mouse_y,u->modifiers};
    u->dirty=1; if (u->event) u->event(u,&e,u->user);
}
void du_init(struct du_ui *u,uint32_t *pixels,unsigned w,unsigned h)
{
    memset(u,0,sizeof(*u)); u->canvas=(struct du_canvas){pixels,w,h,{0,0,(int)w,(int)h}};
    u->focus=u->capture=u->menu=-1; u->dirty=1;
}
struct du_widget *du_add(struct du_ui *u,enum du_kind kind,int id,struct du_rect r,const char *s)
{
    if (u->count==DU_WIDGETS || du_get(u,id)) return NULL;
    struct du_widget *w=&u->widgets[u->count++]; memset(w,0,sizeof(*w));
    w->id=id; w->kind=kind; w->box=r;
    snprintf(w->text,sizeof(w->text),"%s",s?s:""); w->cursor=w->anchor=strlen(w->text);
    return w;
}
struct du_widget *du_get(struct du_ui *u,int id)
{ for (unsigned i=0;i<u->count;i++) if (u->widgets[i].id==id) return &u->widgets[i]; return NULL; }
int du_hit(const struct du_ui *u,int x,int y)
{
    for (unsigned i=u->count;i>0;i--) { const struct du_widget *w=&u->widgets[i-1];
        if (!w->hidden && !w->disabled && w->kind!=DU_GROUP && du_contains(w->box,x,y)) return (int)i-1; }
    return -1;
}
static unsigned columns(const struct du_widget *w)
{ unsigned n=(unsigned)(w->box.w>0?w->box.w:1)/96; return n?n:1; }
static unsigned visible(const struct du_widget *w)
{ unsigned n=(unsigned)(w->box.h>24?w->box.h-24:1)/(w->view==DU_ICONS?64:20); return (n?n:1)*(w->view==DU_ICONS?columns(w):1); }
int du_list_hit(const struct du_widget *w,int x,int y)
{
    if (!du_contains(w->box,x,y)) return -1;
    int row=y-w->box.y-(w->view==DU_DETAILS?22:0);
    if (row<0) return -1;
    unsigned n=w->first+(unsigned)row/(w->view==DU_ICONS?64:20);
    if (w->view==DU_ICONS) n=w->first+(unsigned)row/64*columns(w)+(unsigned)(x-w->box.x)/96;
    return n<w->count?(int)n:-1;
}
void du_focus(struct du_ui *u,int id)
{ for (unsigned i=0;i<u->count;i++) if (u->widgets[i].id==id) { u->focus=(int)i; u->dirty=1; return; } }
void du_release(struct du_ui *u)
{ memset(u->keys,0,sizeof(u->keys)); u->buttons=u->modifiers=0; u->capture=-1; u->dirty=1; }
static struct du_rect dialog_box(const struct du_ui *u)
{ int w=(int)u->canvas.width-32; if (w>460) w=460; return (struct du_rect){((int)u->canvas.width-w)/2,((int)u->canvas.height-176)/2,w,176}; }
static struct du_rect dialog_button(const struct du_ui *u,unsigned i)
{
    struct du_rect r=dialog_box(u); int w=(r.w-24)/(int)u->dialog.count;
    return (struct du_rect){r.x+12+(int)i*w,r.y+140,w-4,24};
}
void du_dialog_show(struct du_ui *u,enum du_dialog_kind kind,int id,const char *title,const char *message,const char *initial)
{
    struct du_dialog *d=&u->dialog; memset(d,0,sizeof(*d));
    d->visible=1; d->kind=kind; d->id=id; d->count=2; d->buttons[0]=kind==DU_JOB?"Cancel":"OK";
    d->buttons[1]="Cancel"; if (kind==DU_JOB) d->count=1;
    snprintf(d->title,sizeof(d->title),"%s",title); snprintf(d->message,sizeof(d->message),"%s",message);
    d->edit.kind=DU_EDIT; snprintf(d->edit.text,sizeof(d->edit.text),"%s",initial?initial:"");
    d->edit.cursor=strlen(d->edit.text); d->edit.anchor=0; u->dirty=1;
}
void du_dialog_buttons(struct du_ui *u,const char *const *buttons,unsigned n)
{ if (n>4 || !n) return; u->dialog.count=n; for (unsigned i=0;i<n;i++) u->dialog.buttons[i]=buttons[i]; u->dirty=1; }
void du_context(struct du_ui *u,const struct du_menu_item *items,unsigned n,int x,int y)
{
    u->context=items; u->context_count=n; u->context_x=x; u->context_y=y;
    if (x+224>(int)u->canvas.width) u->context_x=(int)u->canvas.width-224;
    if (y+(int)n*22>(int)u->canvas.height) u->context_y=(int)u->canvas.height-(int)n*22;
    if (u->context_x<0) u->context_x=0;
    if (u->context_y<0) u->context_y=0;
    u->menu_item=0; u->dirty=1;
}
static int menu_x(const struct du_ui *u,unsigned index)
{ int x=0; for (unsigned i=0;i<index;i++) x+=(int)strlen(u->menus[i].label)*8+24; return x; }
static void draw_edit(struct du_ui *u,struct du_widget *w,int focused)
{
    struct du_canvas *c=&u->canvas; du_bevel(c,w->box,1);
    struct du_rect clip=c->clip; c->clip=du_intersection(clip,(struct du_rect){w->box.x+3,w->box.y+3,w->box.w-6,w->box.h-6});
    du_fill(c,c->clip,DU_WHITE);
    size_t cells=(size_t)(w->box.w>16?w->box.w-8:8)/8;
    if (w->cursor<w->offset) w->offset=w->cursor;
    if (w->cursor>w->offset+cells-1) w->offset=w->cursor-cells+1;
    size_t a=w->cursor<w->anchor?w->cursor:w->anchor,b=w->cursor>w->anchor?w->cursor:w->anchor;
    if (focused && b>a) du_fill(c,(struct du_rect){w->box.x+4+(int)(a>w->offset?a-w->offset:0)*8,w->box.y+4,(int)(b-(a>w->offset?a:w->offset))*8,16},0xb8d0dcu);
    du_text(c,w->box.x+4,w->box.y+4,w->text+w->offset,DU_INK);
    if (focused) du_fill(c,(struct du_rect){w->box.x+4+(int)(w->cursor-w->offset)*8,w->box.y+4,1,16},DU_INK);
    c->clip=clip;
}
/* f2-33: caller-defined Details columns; the final column takes the rest. */
static int column_left(const struct du_widget *w,unsigned col)
{
    unsigned percent=0; for (unsigned i=0;i<col && i<w->column_count;i++) percent+=w->columns[i].percent;
    return col>=w->column_count?w->box.w:(int)((int64_t)w->box.w*percent/100);
}
static int column_at(const struct du_widget *w,int x)
{
    for (unsigned i=0;i+1<w->column_count;i++) if (x-w->box.x<column_left(w,i+1)) return (int)i;
    return w->column_count?(int)w->column_count-1:0;
}
static void draw_columns(struct du_ui *u,struct du_widget *w)
{
    struct du_canvas *c=&u->canvas; struct du_rect all=c->clip;
    for (unsigned i=0;i<w->column_count;i++) {
        int left=column_left(w,i),right=column_left(w,i+1);
        struct du_rect cell={w->box.x+left,w->box.y,right-left,22};
        du_bevel(c,cell,0); c->clip=du_intersection(all,(struct du_rect){cell.x+2,cell.y,cell.w-4,22});
        char label[48]; snprintf(label,sizeof(label),"%s%s",w->columns[i].label,
            w->sort_column==(int)i?(w->sort_descending?" v":" ^"):"");
        int width=(int)strlen(label)*8;
        du_text(c,w->columns[i].right && width<cell.w-8?cell.x+cell.w-4-width:cell.x+4,cell.y+3,label,DU_INK);
        c->clip=all;
    }
}
static void draw_cells(struct du_ui *u,struct du_widget *w,unsigned index,int y,uint32_t ink)
{
    struct du_canvas *c=&u->canvas; struct du_rect all=c->clip; const struct du_row *row=&w->rows[index];
    for (unsigned i=0;i<w->column_count;i++) {
        int left=column_left(w,i),right=column_left(w,i+1);
        c->clip=du_intersection(all,(struct du_rect){w->box.x+left+2,y,right-left-4,20});
        const char *cell=w->cells?w->cells[(size_t)index*w->column_count+i]:NULL;
        const char *text=cell?cell:!i&&row->name?row->name:"";
        int x=w->box.x+left+4;
        if (!i && row->icon) { du_icon(c,x-2,y+2,row->icon,16); x+=18; }
        int width=(int)strlen(text)*8;
        if (w->columns[i].right && width<right-left-8) x=w->box.x+right-4-width;
        du_text(c,x,y+2,text,ink);
    }
    c->clip=all;
}
int du_tabs_hit(const struct du_widget *w,int x,int y)
{
    if (!du_contains(w->box,x,y)) return -1;
    int at=w->box.x+2;
    for (unsigned i=0;i<w->label_count;i++) { int width=(int)strlen(w->labels[i])*8+20;
        if (x>=at && x<at+width) return (int)i; at+=width; }
    return -1;
}
static void draw_tabs(struct du_ui *u,struct du_widget *w,int focused)
{
    struct du_canvas *c=&u->canvas; int at=w->box.x+2,bottom=w->box.y+w->box.h-1;
    du_fill(c,(struct du_rect){w->box.x,bottom,w->box.w,1},DU_WHITE);
    for (unsigned i=0;i<w->label_count;i++) {
        int width=(int)strlen(w->labels[i])*8+20,selected=w->value==i;
        struct du_rect r={at,w->box.y+(selected?0:3),width,w->box.h-(selected?0:3)};
        du_fill(c,r,DU_BG); du_fill(c,(struct du_rect){r.x,r.y,r.w,1},DU_WHITE);
        du_fill(c,(struct du_rect){r.x,r.y,1,r.h},DU_WHITE); du_fill(c,(struct du_rect){r.x+r.w-1,r.y,1,r.h},0x879399u);
        if (selected) du_fill(c,(struct du_rect){r.x+1,bottom,r.w-2,1},DU_BG);
        du_text(c,r.x+10,r.y+(selected?4:2),w->labels[i],DU_INK);
        if (selected && focused) du_fill(c,(struct du_rect){r.x+8,r.y+r.h-5,r.w-16,1},DU_INK);
        at+=width;
    }
}
static int graph_y(const struct du_widget *w,unsigned value)
{ if (value>100) value=100; return w->box.y+w->box.h-2-(int)((int64_t)(w->box.h-4)*value/100); }
static void draw_graph(struct du_ui *u,struct du_widget *w)
{
    struct du_canvas *c=&u->canvas; du_fill(c,w->box,0x000000u);
    for (int y=w->box.y+w->box.h-1;y>=w->box.y;y-=12) du_fill(c,(struct du_rect){w->box.x,y,w->box.w,1},0x00573fu);
    for (int x=w->box.x+w->box.w-1;x>=w->box.x;x-=12) du_fill(c,(struct du_rect){x,w->box.y,1,w->box.h},0x00573fu);
    unsigned capacity=w->total>1?w->total:2,n=w->sample_count<capacity?w->sample_count:capacity;
    if (!w->samples || !n) return;
    /* Newest sample at the right edge; integer Bresenham segments. */
    for (unsigned i=0;i<n;i++) {
        unsigned slot=capacity-n+i;
        int x1=w->box.x+(int)((int64_t)(w->box.w-1)*slot/(capacity-1)),y1=graph_y(w,w->samples[w->sample_count-n+i]);
        if (!i) { du_fill(c,(struct du_rect){x1,y1,1,1},0x00ff00u); continue; }
        int x0=w->box.x+(int)((int64_t)(w->box.w-1)*(slot-1)/(capacity-1)),y0=graph_y(w,w->samples[w->sample_count-n+i-1]);
        int dx=x1-x0,dy=y1>y0?y1-y0:y0-y1,sy=y0<y1?1:-1,err=dx-dy;
        for (;;) { du_fill(c,(struct du_rect){x0,y0,1,1},0x00ff00u); if (x0==x1 && y0==y1) break;
            int e2=2*err; if (e2>-dy) { err-=dy; x0++; } if (e2<dx) { err+=dx; y0+=sy; } }
    }
}
static void draw_group(struct du_ui *u,struct du_widget *w)
{
    struct du_canvas *c=&u->canvas; struct du_rect b=w->box;
    du_fill(c,(struct du_rect){b.x,b.y+7,b.w,1},0x879399u); du_fill(c,(struct du_rect){b.x,b.y+b.h-1,b.w,1},0x879399u);
    du_fill(c,(struct du_rect){b.x,b.y+7,1,b.h-7},0x879399u); du_fill(c,(struct du_rect){b.x+b.w-1,b.y+7,1,b.h-7},0x879399u);
    if (w->text[0]) { int width=(int)strlen(w->text)*8+8; du_fill(c,(struct du_rect){b.x+6,b.y,width,16},DU_BG); du_text(c,b.x+10,b.y,w->text,DU_INK); }
}
static void draw_list(struct du_ui *u,struct du_widget *w)
{
    struct du_canvas *c=&u->canvas; du_fill(c,w->box,DU_WHITE);
    int header=w->view==DU_DETAILS?22:0;
    if (header && w->column_count) draw_columns(u,w);
    else if (header) {
        const char *labels[]={"Name","Size","Type","Modified"};
        int widths[]={w->box.w*43/100,w->box.w*17/100,w->box.w*18/100,w->box.w*22/100};
        int x=w->box.x;
        for (unsigned i=0;i<4;i++) { du_bevel(c,(struct du_rect){x,w->box.y,widths[i],22},0); du_text(c,x+4,w->box.y+3,labels[i],DU_INK); x+=widths[i]; }
    }
    unsigned end=w->first+visible(w)+columns(w); if (end>w->count) end=w->count;
    for (unsigned i=w->first;i<end;i++) {
        struct du_row *row=&w->rows[i]; unsigned n=i-w->first;
        int x=w->box.x,y=w->box.y+header+(int)n*20,width=w->box.w,height=20;
        if (w->view==DU_ICONS) { x+=(int)(n%columns(w))*96; y=w->box.y+(int)(n/columns(w))*64; width=96; height=64; }
        struct du_rect old=c->clip; c->clip=du_intersection(old,(struct du_rect){x,y,width,height});
        uint32_t ink=row->selected?DU_WHITE:DU_INK;
        if (row->selected) du_fill(c,c->clip,DU_SELECTED);
        if (w->view==DU_ICONS) { du_icon(c,x+32,y+2,row->icon,32); du_text(c,x+4,y+40,row->name,ink); }
        else if (header && w->column_count) draw_cells(u,w,i,y,ink);
        else {
            int indent=w->kind==DU_TREE?(int)row->depth*12:0;
            if (w->kind==DU_TREE) du_text(c,x+indent,y+2,row->expanded?"-":"+",ink);
            du_icon(c,x+2+indent+(w->kind==DU_TREE?12:0),y+2,row->icon,16);
            struct du_rect all=c->clip;
            if (header) c->clip=du_intersection(all,(struct du_rect){x,y,w->box.w*43/100,20});
            du_text(c,x+22+indent+(w->kind==DU_TREE?12:0),y+2,row->name,ink); c->clip=all;
            if (header) {
                int offsets[]={43,60,78,100}; const char *values[]={row->size,row->type,row->modified};
                for (unsigned col=0;col<3;col++) {
                    int left=w->box.w*offsets[col]/100,right=w->box.w*offsets[col+1]/100;
                    c->clip=du_intersection(all,(struct du_rect){x+left,y,right-left,20});
                    du_text(c,x+left+4,y+2,values[col]?values[col]:"",ink);
                }
            }
        }
        c->clip=old;
    }
}
static void draw_menu(struct du_ui *u,const struct du_menu_item *items,unsigned n,int x,int y)
{
    struct du_canvas *c=&u->canvas; du_bevel(c,(struct du_rect){x,y,224,(int)n*22+4},0);
    for (unsigned i=0;i<n;i++) {
        struct du_rect r={x+2,y+2+(int)i*22,220,22};
        if ((int)i==u->menu_item) du_fill(c,r,DU_SELECTED);
        du_text(c,r.x+6,r.y+3,items[i].label,items[i].disabled?0x879399u:(int)i==u->menu_item?DU_WHITE:DU_INK);
    }
}
void du_render(struct du_ui *u)
{
    struct du_canvas *c=&u->canvas; c->clip=(struct du_rect){0,0,(int)c->width,(int)c->height}; du_fill(c,c->clip,DU_BG);
    for (unsigned i=0;i<u->count;i++) {
        struct du_widget *w=&u->widgets[i]; if (w->hidden) continue;
        struct du_rect clip=c->clip; c->clip=du_intersection(clip,w->box);
        switch (w->kind) {
        case DU_BUTTON:
            du_bevel(c,w->box,u->capture==(int)i); du_icon(c,w->box.x+5,w->box.y+5,w->icon,16);
            du_text(c,w->box.x+(w->icon?26:5),w->box.y+5,w->text,w->disabled?0x879399u:DU_INK); break;
        case DU_EDIT: draw_edit(u,w,u->focus==(int)i); break;
        case DU_LIST: case DU_TREE: draw_list(u,w); break;
        case DU_SCROLLBAR: {
            du_bevel(c,w->box,0); unsigned total=w->total?w->total:1;
            int size=(int)((uint64_t)(unsigned)w->box.h*w->page/total); if (size<16) size=16; if (size>w->box.h) size=w->box.h;
            int at=total>w->page?(int)((uint64_t)(unsigned)(w->box.h-size)*w->value/(total-w->page)):0;
            du_bevel(c,(struct du_rect){w->box.x,w->box.y+at,w->box.w,size},1); break;
        }
        case DU_SPLITTER: du_fill(c,w->box,0xa9b4b8u); break;
        case DU_PROGRESS: du_bevel(c,w->box,1); du_fill(c,(struct du_rect){w->box.x+2,w->box.y+2,w->total?(int)((uint64_t)(unsigned)(w->box.w-4)*w->value/w->total):0,w->box.h-4},DU_SELECTED); break;
        case DU_STATUS: du_bevel(c,w->box,1); du_text(c,w->box.x+4,w->box.y+4,w->text,DU_INK); break;
        case DU_LABEL: du_text(c,w->box.x,w->box.y,w->text,DU_INK); break;
        case DU_TABS: draw_tabs(u,w,u->focus==(int)i); break;
        case DU_GRAPH: draw_graph(u,w); break;
        case DU_GROUP: draw_group(u,w); break;
        }
        c->clip=clip;
    }
    for (unsigned i=0;i<u->menu_count;i++) { int x=menu_x(u,i); struct du_rect r={x,0,(int)strlen(u->menus[i].label)*8+24,24};
        if (u->menu==(int)i) du_fill(c,r,DU_SELECTED);
        du_text(c,x+12,4,u->menus[i].label,u->menu==(int)i?DU_WHITE:DU_INK); }
    if (u->menu>=0 && (unsigned)u->menu<u->menu_count) draw_menu(u,u->menus[u->menu].items,u->menus[u->menu].count,menu_x(u,(unsigned)u->menu),24);
    if (u->context) draw_menu(u,u->context,u->context_count,u->context_x,u->context_y);
    if (u->dialog.visible) {
        struct du_dialog *d=&u->dialog; struct du_rect r=dialog_box(u); du_bevel(c,r,0);
        du_fill(c,(struct du_rect){r.x+3,r.y+3,r.w-6,24},DU_SELECTED); du_text(c,r.x+10,r.y+7,d->title,DU_WHITE);
        c->clip=du_intersection(c->clip,(struct du_rect){r.x+8,r.y+32,r.w-16,100});
        const char *line=d->message; int y=r.y+36;
        for (unsigned n=0;n<4 && *line;n++,y+=18) {
            char text[128]; const char *end=strchr(line,'\n'); size_t len=end?(size_t)(end-line):strlen(line);
            if (len>=sizeof(text)) len=sizeof(text)-1;
            memcpy(text,line,len); text[len]=0; du_text(c,r.x+12,y,text,DU_INK); if (!end) break; line=end+1;
        }
        if (d->kind==DU_INPUT || d->kind==DU_OPEN || d->kind==DU_SAVE) { d->edit.box=(struct du_rect){r.x+12,r.y+100,r.w-24,28}; draw_edit(u,&d->edit,!d->focus); }
        if (d->kind==DU_JOB) { du_bevel(c,(struct du_rect){r.x+12,r.y+108,r.w-24,18},1);
            unsigned v=d->progress>d->total?d->total:d->progress;
            du_fill(c,(struct du_rect){r.x+14,r.y+110,d->total?(int)((uint64_t)(unsigned)(r.w-28)*v/d->total):0,14},DU_SELECTED); }
        c->clip=(struct du_rect){0,0,(int)c->width,(int)c->height};
        for (unsigned i=0;i<d->count;i++) { struct du_rect b=dialog_button(u,i); du_bevel(c,b,d->focus==(int)i+1); du_text(c,b.x+5,b.y+4,d->buttons[i],DU_INK); }
    }
    u->dirty=0;
}
/* Set-1 make codes for ASCII letters (public keyboard table). */
static unsigned letter_code(char ch)
{
    static const unsigned char codes[26]={0x1e,0x30,0x2e,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
                                          0x31,0x18,0x19,0x10,0x13,0x1f,0x14,0x16,0x2f,0x11,0x2d,0x15,0x2c};
    if (ch>='A' && ch<='Z') ch=(char)(ch-'A'+'a');
    return ch>='a' && ch<='z'?codes[ch-'a']:0;
}
static size_t previous(const char *s,size_t n) { if (!n) return 0; do { n--; } while (n && ((unsigned char)s[n]&0xc0)==0x80); return n; }
static size_t next(const char *s,size_t n) { if (!s[n]) return n; n++; while (((unsigned char)s[n]&0xc0)==0x80) n++; return n; }
static void edit_insert(struct du_widget *w,const char *text)
{
    size_t a=w->cursor<w->anchor?w->cursor:w->anchor,b=w->cursor>w->anchor?w->cursor:w->anchor,len=strlen(w->text),n=strlen(text);
    if (len-(b-a)+n>=sizeof(w->text)) return;
    memmove(w->text+a+n,w->text+b,len-b+1); memcpy(w->text+a,text,n); w->cursor=w->anchor=a+n;
}
static void edit_key(struct du_ui *u,struct du_widget *w,unsigned code)
{
    size_t len=strlen(w->text); int shift=(u->modifiers&DU_SHIFT)!=0;
    if ((u->modifiers&DU_CTRL) && code==0x1e) { w->anchor=0; w->cursor=len; }
    else if ((u->modifiers&DU_CTRL) && (code==0x2e || code==0x2d)) {
        size_t a=w->cursor<w->anchor?w->cursor:w->anchor,b=w->cursor>w->anchor?w->cursor:w->anchor;
        memcpy(clipboard,w->text+a,b-a); clipboard[b-a]=0; if (code==0x2d) edit_insert(w,"");
    } else if ((u->modifiers&DU_CTRL) && code==0x2f) edit_insert(w,clipboard);
    else if (code==0x14b) { w->cursor=previous(w->text,w->cursor); if (!shift) w->anchor=w->cursor; }
    else if (code==0x14d) { w->cursor=next(w->text,w->cursor); if (!shift) w->anchor=w->cursor; }
    else if (code==0x147 || code==0x14f) { w->cursor=code==0x147?0:len; if (!shift) w->anchor=w->cursor; }
    else if (code==0x0e || code==0x153) {
        if (w->cursor==w->anchor) { if (code==0x0e) w->anchor=previous(w->text,w->cursor); else w->anchor=next(w->text,w->cursor); }
        edit_insert(w,"");
    }
    u->dirty=1;
}
static void dialog_result(struct du_ui *u,int index)
{ int id=u->dialog.id; u->dialog.result=index; u->dialog.visible=0; emit(u,DU_ACTIVATE,id,index); }
void du_input(struct du_ui *u,const struct ciuki_input_event *e)
{
    if (e->type==CIUKI_INPUT_RESYNC) { du_release(u); return; }
    if (e->type==CIUKI_INPUT_MOTION) {
        u->mouse_x=e->value; u->mouse_y=e->value2;
        if (u->capture>=0) { struct du_widget *w=&u->widgets[u->capture];
            if (w->kind==DU_SPLITTER) { w->value=(unsigned)(u->mouse_x>32?u->mouse_x:32); emit(u,DU_CHANGE,w->id,(int)w->value); }
            if (w->kind==DU_SCROLLBAR && w->box.h>0) {
                int y=u->mouse_y-w->box.y; if (y<0) y=0; if (y>w->box.h) y=w->box.h;
                w->value=w->total>w->page?(unsigned)((uint64_t)(unsigned)y*(w->total-w->page)/(unsigned)w->box.h):0; emit(u,DU_CHANGE,w->id,(int)w->value);
            }
        } return;
    }
    if (e->type==CIUKI_INPUT_BUTTON) {
        if (e->code<1 || e->code>3) return;
        unsigned bit=1u<<(e->code-1); if (e->value) u->buttons|=bit; else u->buttons&=~bit;
        if (u->dialog.visible) {
            if (e->value && e->code==1) for (unsigned i=0;i<u->dialog.count;i++) if (du_contains(dialog_button(u,i),u->mouse_x,u->mouse_y)) { dialog_result(u,(int)i); break; }
            return;
        }
        if (e->value && e->code==1 && (u->context || u->menu>=0)) {
            const struct du_menu_item *items=u->context?u->context:u->menus[u->menu].items;
            unsigned count=u->context?u->context_count:u->menus[u->menu].count;
            int x=u->context?u->context_x:menu_x(u,(unsigned)u->menu),y=u->context?u->context_y:24;
            int n=(u->mouse_y-y-2)/22;
            if (u->mouse_y>=y+2 && du_contains((struct du_rect){x,y,224,(int)count*22+4},u->mouse_x,u->mouse_y) && n>=0 && (unsigned)n<count) {
                int id=items[n].id,disabled=items[n].disabled; u->context=NULL; u->menu=-1; if (!disabled) emit(u,DU_ACTIVATE,id,0); return;
            }
            u->context=NULL; u->menu=-1; u->dirty=1;
        }
        if (e->value && e->code==1 && u->mouse_y<24) {
            for (unsigned i=0;i<u->menu_count;i++) if (u->mouse_x>=menu_x(u,i) && u->mouse_x<menu_x(u,i+1)) { u->menu=(int)i; u->menu_item=0; u->dirty=1; return; }
        }
        int hit=du_hit(u,u->mouse_x,u->mouse_y);
        if (e->value) {
            if (hit<0) { if (e->code==3) emit(u,DU_CONTEXT,-1,-1); return; }
            struct du_widget *w=&u->widgets[hit]; u->focus=hit;
            if (e->code==1) u->capture=hit;
            if (w->kind==DU_TABS) {
                int n=du_tabs_hit(w,u->mouse_x,u->mouse_y);
                if (n>=0 && e->code==1) { w->value=(unsigned)n; emit(u,DU_SELECT,w->id,n); }
            } else if (w->kind==DU_LIST || w->kind==DU_TREE) {
                int n=du_list_hit(w,u->mouse_x,u->mouse_y);
                if (w->view==DU_DETAILS && u->mouse_y<w->box.y+22 && e->code==1 && w->column_count) emit(u,DU_SORT,w->id,column_at(w,u->mouse_x));
                else if (w->view==DU_DETAILS && u->mouse_y<w->box.y+22 && e->code==1) {
                    int x=(u->mouse_x-w->box.x)*100/(w->box.w?w->box.w:1); emit(u,DU_SORT,w->id,x<43?0:x<60?1:x<78?2:3);
                } else {
                    if (n>=0) w->active=(unsigned)n;
                    emit(u,e->code==3?DU_CONTEXT:DU_SELECT,w->id,n);
                }
            } else if (w->kind==DU_EDIT) { w->cursor=strlen(w->text); w->anchor=w->cursor; u->dirty=1; }
        } else if (e->code==1) {
            int captured=u->capture; u->capture=-1;
            if (captured>=0 && captured==hit && u->widgets[hit].kind==DU_BUTTON) emit(u,DU_ACTIVATE,u->widgets[hit].id,0);
            u->dirty=1;
        } return;
    }
    if (e->type==CIUKI_INPUT_KEY) {
        if (e->code<0 || e->code>512) return;
        unsigned code=(unsigned)e->code; u->keys[code]=(uint8_t)(e->value!=0);
        u->modifiers=(u->keys[0x2a]||u->keys[0x36]?DU_SHIFT:0)|(u->keys[0x1d]||u->keys[0x11d]?DU_CTRL:0)|(u->keys[0x38]||u->keys[0x138]?DU_ALT:0);
        if (!e->value) return;
        if (u->dialog.visible) {
            if (code==1) dialog_result(u,(int)u->dialog.count-1);
            else if (code==0x0f) { u->dialog.focus=(u->dialog.focus+1)%(int)(u->dialog.count+1); u->dirty=1; }
            else if (code==0x1c) dialog_result(u,u->dialog.focus?u->dialog.focus-1:0);
            else if (!u->dialog.focus) edit_key(u,&u->dialog.edit,code);
            return;
        }
        if (code==1) { u->menu=-1; u->context=NULL; u->dirty=1; return; }
        if (code==0x44) { u->menu=u->menu>=0?-1:0; u->menu_item=0; u->dirty=1; return; }
        if (u->menu>=0 || u->context) {
            const struct du_menu_item *items=u->context?u->context:u->menus[u->menu].items;
            unsigned count=u->context?u->context_count:u->menus[u->menu].count;
            if (count && (code==0x148 || code==0x150)) u->menu_item=(u->menu_item+(code==0x148?(int)count-1:1))%(int)count;
            else if (!u->context && (code==0x14b || code==0x14d) && u->menu_count) { u->menu=(u->menu+(code==0x14b?(int)u->menu_count-1:1))%(int)u->menu_count; u->menu_item=0; }
            else if (code==0x1c && count) { int id=items[u->menu_item].id,disabled=items[u->menu_item].disabled; u->menu=-1; u->context=NULL; if (!disabled) emit(u,DU_ACTIVATE,id,0); }
            u->dirty=1; return;
        }
        if (u->focus>=0 && u->widgets[u->focus].kind==DU_EDIT) {
            if (code==0x1c) emit(u,DU_ACTIVATE,u->widgets[u->focus].id,0); else edit_key(u,&u->widgets[u->focus],code); return;
        }
        for (unsigned m=0;m<u->menu_count;m++) for (unsigned i=0;i<u->menus[m].count;i++) {
            const struct du_menu_item *item=&u->menus[m].items[i];
            if (!item->disabled && item->key==code && item->modifiers==u->modifiers) { emit(u,DU_ACTIVATE,item->id,0); return; }
        }
        for (unsigned i=0;i<u->count;i++) { const struct du_widget *w=&u->widgets[i];
            if (w->kind==DU_BUTTON && w->key && !w->hidden && !w->disabled && w->key==code && w->key_modifiers==u->modifiers) { emit(u,DU_ACTIVATE,w->id,0); return; } }
        if ((u->modifiers&DU_ALT) && code<128) {
            /* Each menu's mnemonic is the first letter of its label (set-1 code). */
            for (unsigned i=0;i<u->menu_count;i++) if (code==letter_code(u->menus[i].label[0])) { u->menu=(int)i; u->menu_item=0; u->dirty=1; return; }
        }
        if ((code==0x0f || code==0x149 || code==0x151) && (u->modifiers&DU_CTRL)) {
            for (unsigned i=0;i<u->count;i++) { struct du_widget *w=&u->widgets[i];
                if (w->kind!=DU_TABS || w->hidden || !w->label_count) continue;
                int back=code==0x149 || (code==0x0f && (u->modifiers&DU_SHIFT));
                w->value=(w->value+(back?w->label_count-1:1))%w->label_count; emit(u,DU_SELECT,w->id,(int)w->value); return; }
        }
        if (code==0x0f && u->count) {
            for (unsigned n=0;n<u->count;n++) { u->focus=(u->focus+1)%(int)u->count; struct du_widget *w=&u->widgets[u->focus];
                if (!w->hidden && !w->disabled && w->kind!=DU_LABEL && w->kind!=DU_STATUS && w->kind!=DU_GROUP && w->kind!=DU_GRAPH) break; }
            u->dirty=1; return;
        }
        if (u->focus>=0) {
            struct du_widget *w=&u->widgets[u->focus];
            if ((w->kind==DU_LIST || w->kind==DU_TREE) && w->count) {
                int delta=code==0x148?-1:code==0x150?1:code==0x14b?-1:code==0x14d?1:code==0x149?-(int)visible(w):code==0x151?(int)visible(w):0;
                if (w->view==DU_ICONS && (code==0x148 || code==0x150)) delta*= (int)columns(w);
                int n=(int)w->active+delta;
                if (code==0x147) n=0; if (code==0x14f) n=(int)w->count-1;
                if (n<0) n=0; if ((unsigned)n>=w->count) n=(int)w->count-1;
                if (delta || code==0x147 || code==0x14f) {
                    w->active=(unsigned)n; if (w->active<w->first) w->first=w->active;
                    if (w->active>=w->first+visible(w)) w->first=w->active-visible(w)+1;
                    emit(u,DU_SELECT,w->id,n); return;
                }
                if (code==0x1c) { emit(u,DU_ACTIVATE,w->id,(int)w->active); return; }
                if (code==0x39 && (u->modifiers&DU_CTRL)) { emit(u,DU_SELECT,w->id,(int)w->active); return; }
                if (code==0x15d || (code==0x44 && (u->modifiers&DU_SHIFT))) { emit(u,DU_CONTEXT,w->id,(int)w->active); return; }
            }
            if (w->kind==DU_BUTTON && (code==0x1c || code==0x39)) emit(u,DU_ACTIVATE,w->id,0);
            if (w->kind==DU_TABS && w->label_count && (code==0x14b || code==0x14d)) {
                w->value=(w->value+(code==0x14b?w->label_count-1:1))%w->label_count; emit(u,DU_SELECT,w->id,(int)w->value); }
        }
        return;
    }
    if (e->type==CIUKI_INPUT_TEXT && !(u->modifiers&(DU_CTRL|DU_ALT)) && e->code>=32 && e->code!=127) {
        char s[5]={0}; unsigned ch=(unsigned)e->code;
        if (ch<128) s[0]=(char)ch;
        else if (ch<2048) { s[0]=(char)(0xc0|(ch>>6)); s[1]=(char)(0x80|(ch&63)); }
        else if (ch<65536) { s[0]=(char)(0xe0|(ch>>12)); s[1]=(char)(0x80|((ch>>6)&63)); s[2]=(char)(0x80|(ch&63)); }
        else return; /* FAT UCS-2 names */
        if (u->dialog.visible) { if (u->dialog.kind!=DU_MESSAGE && u->dialog.kind!=DU_JOB && !u->dialog.focus) edit_insert(&u->dialog.edit,s); }
        else if (u->focus>=0 && u->widgets[u->focus].kind==DU_EDIT) { edit_insert(&u->widgets[u->focus],s); emit(u,DU_CHANGE,u->widgets[u->focus].id,0); }
        else emit(u,DU_CHANGE,-1,(int)ch);
        u->dirty=1;
    }
}
int du_timer_start(struct du_ui *u,int id,uint64_t now,uint64_t interval)
{
    if (!interval || UINT64_MAX-now<interval) return -1;
    for (unsigned i=0;i<DU_TIMERS;i++) if (!u->timers[i].active || u->timers[i].id==id) { u->timers[i]=(struct du_timer){now+interval,interval,id,1}; return 0; }
    return -1;
}
void du_timer_stop(struct du_ui *u,int id) { for (unsigned i=0;i<DU_TIMERS;i++) if (u->timers[i].id==id) u->timers[i].active=0; }
void du_tick(struct du_ui *u,uint64_t now)
{ for (unsigned i=0;i<DU_TIMERS;i++) if (u->timers[i].active && now>=u->timers[i].deadline) { int id=u->timers[i].id; u->timers[i].deadline=UINT64_MAX-now<u->timers[i].interval?UINT64_MAX:now+u->timers[i].interval; emit(u,DU_TICK,id,0); } }
