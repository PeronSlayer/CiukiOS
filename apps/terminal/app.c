/* SPDX-License-Identifier: MIT */
#include "terminal.h"
#include "../libdeskui/internal.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { GRID=1, SCROLL=2, COPY=3, CLEAR=4, HELP=5 };
static char *duplicate(const char *s) { char *p=malloc(strlen(s)+1); if(p)strcpy(p,s); return p; }
static unsigned wrapped(const struct terminal *t,const char *s)
{ size_t n=strlen(s); return n ? (unsigned)((n+t->columns-1)/t->columns) : 1; }
unsigned term_visual_rows(const struct terminal *t)
{
    unsigned n=0; for(unsigned i=0;i<t->count;i++)n+=wrapped(t,t->lines[(t->first+i)%TERM_SCROLLBACK]);
    if(t->used)n+=wrapped(t,t->pending); return n;
}
void term_row(const struct terminal *t,unsigned index,char *out,size_t size)
{
    if(!size)return; out[0]=0;
    for(unsigned i=0;i<t->count+(t->used!=0);i++) {
        const char *s=i==t->count ? t->pending : t->lines[(t->first+i)%TERM_SCROLLBACK]; unsigned n=wrapped(t,s);
        if(index>=n) { index-=n; continue; }
        size_t start=(size_t)index*t->columns, length=strlen(s+start); if(length>t->columns)length=t->columns;
        if(length>=size)length=size-1; memcpy(out,s+start,length); out[length]=0; return;
    }
}
static void prompt(struct terminal *t,char *out,size_t size)
{
    char cwd[FM_PATH];
    if(fm_display_path(cwd,t->cwd))snprintf(cwd,sizeof(cwd),"%s",t->cwd);
    snprintf(out,size,"%s%s",t->lua_mode ? (t->chunk_length ? ">> " : "lua> ") : cwd,t->lua_mode ? t->input : "> ");
    if(!t->lua_mode)strncat(out,t->input,size-strlen(out)-1);
}
static unsigned total(struct terminal *t)
{ char *p=t->prompt_text; prompt(t,p,sizeof(t->prompt_text)); return term_visual_rows(t)+wrapped(t,p); }
static void update(struct terminal *t)
{
    unsigned n=total(t), max=n>t->rows ? n-t->rows : 0;
    if(t->follow || t->top>max)t->top=max;
    struct du_widget *s=du_get(t->ui,SCROLL); s->total=(int)n; s->page=(int)t->rows; s->value=(int)t->top;
    t->ui->dirty=1;
}
static int finish_line(struct terminal *t)
{
    char *s=duplicate(t->pending); if(!s)return -ENOMEM;
    if(t->count==TERM_SCROLLBACK) {
        unsigned removed=wrapped(t,t->lines[t->first]); free(t->lines[t->first]); t->first=(t->first+1)%TERM_SCROLLBACK; t->count--;
        t->top=t->top>removed ? t->top-removed : 0; t->selection_a=t->selection_b=0;
    }
    t->lines[(t->first+t->count++)%TERM_SCROLLBACK]=s; t->used=0; t->pending[0]=0; return 0;
}
void term_write(struct terminal *t,const char *s)
{
    if(t->capture) {
        size_t n=strlen(s); if(n>TERM_CAPTURE-t->captured) { t->capture_error=-EOVERFLOW; return; }
        memcpy(t->capture+t->captured,s,n+1); t->captured+=n; return;
    }
    while(*s) {
        unsigned c=(unsigned char)*s++;
        if(c=='\n') { if(finish_line(t))break; }
        else if(c=='\r')continue;
        else if(c=='\t') { unsigned n=8-t->used%8; while(n--)term_write(t," "); }
        else {
            if(t->used==TERM_LINE && finish_line(t))break;
            t->pending[t->used++]=(char)(c<32 || c>=127 ? '?' : c); t->pending[t->used]=0;
        }
    }
    if(t->ui)update(t);
}
void term_printf(struct terminal *t,const char *fmt,...)
{
    char s[2048]; va_list a; va_start(a,fmt); vsnprintf(s,sizeof(s),fmt,a); va_end(a); term_write(t,s);
}
static int write_all(struct terminal *t,int fd,const char *p,unsigned size)
{
    while(size) { int n=t->host.fs.write(t->host.fs.ctx,fd,p,size); if(n<=0)return n ? n : -EIO; p+=n; size-=(unsigned)n; } return 0;
}
int term_history_save(struct terminal *t)
{
    char temporary[80]; int fd=-EEXIST;
    for(unsigned i=0;i<1000 && fd==-EEXIST;i++) {
        snprintf(temporary,sizeof(temporary),"/Documents/.terminal_history.%u.new",i); fd=t->host.fs.open(t->host.fs.ctx,temporary,1);
    }
    if(fd<0)return fd;
    int r=0;
    for(unsigned i=0;!r && i<t->history_count;i++) {
        r=write_all(t,fd,t->history[i],(unsigned)strlen(t->history[i])); if(!r)r=write_all(t,fd,"\n",1);
    }
    if(!r)r=t->host.fs.sync(t->host.fs.ctx,fd);
    int c=t->host.fs.close(t->host.fs.ctx,fd); if(!r)r=c;
    if(!r)r=t->host.fs.rename(t->host.fs.ctx,temporary,"/Documents/.terminal_history");
    if(r)t->host.fs.unlink(t->host.fs.ctx,temporary); return r;
}
static void remember(struct terminal *t,const char *line)
{
    if(!*line || (t->history_count && !strcmp(t->history[t->history_count-1],line)))return;
    char *s=duplicate(line); if(!s)return;
    if(t->history_count==TERM_HISTORY) { free(t->history[0]); memmove(t->history,t->history+1,(TERM_HISTORY-1)*sizeof(*t->history)); t->history_count--; }
    t->history[t->history_count++]=s;
}
int term_history_load(struct terminal *t)
{
    int fd=t->host.fs.open(t->host.fs.ctx,"/Documents/.terminal_history",0); if(fd<0)return fd==-ENOENT ? 0 : fd;
    char line[DU_TEXT_MAX], buf[512]; unsigned used=0; int overflow=0,r;
    while((r=t->host.fs.read(t->host.fs.ctx,fd,buf,sizeof(buf)))>0)for(int i=0;i<r;i++) {
        unsigned c=(unsigned char)buf[i];
        if(c=='\n') { line[used]=0; if(!overflow)remember(t,line); used=0; overflow=0; }
        else if(c>=32 && c<127) { if(used+1<sizeof(line))line[used++]=(char)c; else overflow=1; }
        else if(c!='\r' && c!='\t')overflow=1;
    }
    if(used && !overflow) { line[used]=0; remember(t,line); }
    int c=t->host.fs.close(t->host.fs.ctx,fd); t->history_at=t->history_count; return r<0 ? r : c;
}
void term_submit(struct terminal *t)
{
    if(t->busy)return;
    char line[DU_TEXT_MAX], *p=t->prompt_text; strcpy(line,t->input); prompt(t,p,sizeof(t->prompt_text));
    if(t->used)term_write(t,"\n"); term_write(t,p); term_write(t,"\n");
    t->input[0]=0; t->cursor=t->anchor=0; t->follow=1; t->selection_a=t->selection_b=0; t->output_focus=0;
    remember(t,line); t->history_at=t->history_count; t->draft[0]=0;
    t->history_error=term_history_save(t);
    if(t->history_error)term_printf(t,"History: %s (error %d)\n",strerror(-t->history_error),-t->history_error);
    t->busy=1; t->interrupted=0;
    if(t->lua_mode) {
        int r=term_lua_line(t,line);
        if(r && r!=-EINVAL && r!=-EINTR)term_printf(t,"Lua: %s (error %d)\n",strerror(-r),-r);
    } else term_execute(t,line);
    t->busy=0; if(t->used)term_write(t,"\n"); update(t);
}
struct completion { struct terminal *t; const char *prefix; char common[FM_NAME]; unsigned count; int directory; };
static int completion_entry(const char *name,const struct fm_meta *meta,void *ctx)
{
    struct completion *c=ctx; size_t n=strlen(c->prefix);
    if(strlen(name)<n || !strcmp(name,".") || !strcmp(name,".."))return 0;
    for(size_t i=0;i<n;i++) { char a=c->prefix[i],b=name[i]; if(a>='A'&&a<='Z')a+=32; if(b>='A'&&b<='Z')b+=32; if(a!=b)return 0; }
    if(!c->count) { snprintf(c->common,sizeof(c->common),"%s",name); c->directory=meta->type==FM_DIR; }
    else { size_t i=0; while(c->common[i] && name[i]) { char a=c->common[i],b=name[i]; if(a>='A'&&a<='Z')a+=32; if(b>='A'&&b<='Z')b+=32; if(a!=b)break; i++; } c->common[i]=0; }
    c->count++; return 0;
}
void term_complete(struct terminal *t)
{
    if(t->busy || t->lua_mode)return;
    size_t start=0; int quote=0;
    for(size_t i=0;i<t->cursor;i++) { if(t->input[i]=='"')quote=!quote; else if(!quote && (t->input[i]==' ' || t->input[i]=='\t'))start=i+1; }
    char *token=t->scratch[0]; size_t used=0;
    for(size_t i=start;i<t->cursor;i++)if(t->input[i]!='"')token[used++]=t->input[i]; token[used]=0;
    char *slash=strrchr(token,'/'),*back=strrchr(token,'\\'); if(back && (!slash || back>slash))slash=back;
    size_t prefix=slash ? (size_t)(slash-token)+1 : 0;
    char *parent_arg=t->scratch[1], *path=t->scratch[2]; memcpy(parent_arg,token,prefix); parent_arg[prefix]=0;
    struct completion c={.t=t,.prefix=token+prefix};
    if(!prefix && !t->cwd[0]) { if(fm_volumes(&t->host.fs,completion_entry,&c))return; }
    else if(term_path(t,prefix ? parent_arg : ".",path) || !path[0] || t->host.fs.list(t->host.fs.ctx,path,FM_LIST_SNIFF,completion_entry,&c))return;
    if(!c.count)return;
    char *replacement=t->scratch[3]; size_t n=prefix+strlen(c.common)+(c.count==1 && c.directory);
    if(n+3>=DU_TEXT_MAX)return;
    memcpy(replacement,token,prefix); strcpy(replacement+prefix,c.common);
    if(c.count==1 && c.directory)strcat(replacement,back ? "\\" : "/");
    if(strchr(replacement,' ') || quote || t->input[start]=='"') { memmove(replacement+1,replacement,n+1); replacement[0]='"'; replacement[n+1]='"'; replacement[n+2]=0; n+=2; }
    size_t tail=strlen(t->input+t->cursor);
    /* Consume a closing quote already to the right of the caret. */
    if(t->input[t->cursor]=='"' && n && replacement[n-1]=='"') { t->cursor++; tail--; }
    if(start+n+tail>=sizeof(t->input))return;
    memmove(t->input+start+n,t->input+t->cursor,tail+1); memcpy(t->input+start,replacement,n); t->cursor=start+n;
    t->anchor=t->cursor; t->selection_a=t->selection_b=0; t->output_focus=0; t->follow=1; update(t);
}
static void view_row(struct terminal *t,unsigned row,char *out,size_t size)
{
    unsigned rows=term_visual_rows(t);
    if(row<rows) { term_row(t,row,out,size); return; }
    prompt(t,t->prompt_text,sizeof(t->prompt_text));
    size_t at=(size_t)(row-rows)*t->columns,n=strlen(t->prompt_text);
    if(at>=n) { out[0]=0; return; }
    n-=at; if(n>t->columns)n=t->columns; if(n>=size)n=size-1;
    memcpy(out,t->prompt_text+at,n); out[n]=0;
}
static size_t input_start(struct terminal *t)
{
    prompt(t,t->prompt_text,sizeof(t->prompt_text));
    return (size_t)term_visual_rows(t)*t->columns+strlen(t->prompt_text)-strlen(t->input);
}
static void copy_selection(struct terminal *t)
{
    if(t->cursor!=t->anchor) {
        size_t a=t->cursor<t->anchor?t->cursor:t->anchor,b=t->cursor>t->anchor?t->cursor:t->anchor;
        du_clipboard_set_n(t->input+a,b-a); return;
    }
    size_t a=t->selection_a,b=t->selection_b; if(a>b) { size_t x=a; a=b; b=x; }
    unsigned rows=total(t); if(a==b || a/t->columns>=rows)return;
    if(b>(size_t)rows*t->columns)b=(size_t)rows*t->columns;
    char *s=malloc(b-a+rows+1); if(!s)return; size_t used=0;
    for(unsigned row=(unsigned)(a/t->columns);row<rows && (size_t)row*t->columns<b;row++) {
        char text[TERM_LINE+1]; view_row(t,row,text,sizeof(text)); size_t n=strlen(text);
        size_t left=row==a/t->columns ? a%t->columns : 0, right=b/t->columns==row ? b%t->columns : t->columns;
        if(right>n)right=n;
        if(left<right) { memcpy(s+used,text+left,right-left); used+=right-left; }
        if((size_t)(row+1)*t->columns<=b && row+1<rows)s[used++]='\n';
    }
    s[used]=0; du_clipboard_set(s); free(s);
}
static void layout(struct du_ui *u,void *ctx)
{
    struct terminal *t=ctx; struct du_widget *g=du_get(u,GRID),*s=du_get(u,SCROLL);
    g->box=(struct du_rect){4,4,u->canvas.width-24,u->canvas.height-8};
    s->box=(struct du_rect){u->canvas.width-18,4,14,u->canvas.height-8};
    t->columns=(unsigned)(g->box.w/TERM_CELL_W); if(!t->columns)t->columns=1;
    t->rows=(unsigned)(g->box.h/TERM_CELL_H); if(!t->rows)t->rows=1;
    t->selection_a=t->selection_b=0; t->selecting=0; t->text_selection.ns=0; update(t);
}
static void paint(struct du_ui *u,void *ctx)
{
    struct terminal *t=ctx; update(t); struct du_rect box=du_get(u,GRID)->box; struct du_canvas c=u->canvas; c.clip=box;
    du_fill(&c,box,0x101820); char *p=t->prompt_text; prompt(t,p,sizeof(t->prompt_text)); unsigned output=term_visual_rows(t);
    size_t a=t->selection_a,b=t->selection_b; if(a>b) { size_t x=a; a=b; b=x; }
    for(unsigned y=0;y<t->rows;y++) {
        unsigned row=t->top+y; char text[TERM_LINE+1];
        if(row<output)term_row(t,row,text,sizeof(text));
        else { size_t start=(size_t)(row-output)*t->columns; size_t n=strlen(p); if(start>=n)text[0]=0;
            else { n-=start; if(n>t->columns)n=t->columns; if(n>TERM_LINE)n=TERM_LINE; memcpy(text,p+start,n); text[n]=0; } }
        for(unsigned x=0;text[x] && x<t->columns;x++) {
            size_t cell=(size_t)row*t->columns+x, start=(size_t)output*t->columns+strlen(p)-strlen(t->input);
            size_t lo=t->anchor<t->cursor?t->anchor:t->cursor,hi=t->anchor>t->cursor?t->anchor:t->cursor;
            int selected=(cell>=a && cell<b) || (cell>=start+lo && cell<start+hi);
            if(selected)du_fill(&c,(struct du_rect){box.x+(int)x*TERM_CELL_W,box.y+(int)y*TERM_CELL_H,TERM_CELL_W,TERM_CELL_H},DU_SELECT);
            char letter[]={text[x],0}; du_text_font(&c,DESK_FONT_MONO,box.x+(int)x*TERM_CELL_W,box.y+(int)y*TERM_CELL_H+2,letter,selected ? DU_SELECT_INK : 0xe8f0e8);
        }
    }
    if(!t->busy && t->follow && t->anchor==t->cursor && a==b) {
        size_t at=strlen(p)-strlen(t->input)+t->cursor; unsigned row=output+(unsigned)(at/t->columns);
        if(row>=t->top && row-t->top<t->rows)du_fill(&c,(struct du_rect){box.x+(int)(at%t->columns)*TERM_CELL_W,box.y+(int)(row-t->top)*TERM_CELL_H+13,TERM_CELL_W,2},0xe8f0e8);
    }
}
static void insert(struct terminal *t,unsigned ch)
{
    size_t n=strlen(t->input),a=t->anchor<t->cursor?t->anchor:t->cursor,b=t->anchor>t->cursor?t->anchor:t->cursor;
    if(ch<32 || ch>=127 || n-(b-a)+1>=sizeof(t->input))return;
    memmove(t->input+a+1,t->input+b,n-b+1); t->input[a]=(char)ch; t->anchor=t->cursor=a+1;
    t->selection_a=t->selection_b=0; t->output_focus=0;
}
void term_event(struct du_ui *u,const struct du_event *e,void *ctx)
{
    struct terminal *t=ctx;
    if(e->type==DU_EV_DIALOG && e->id==50) {
        unsigned action=t->pending_power; t->pending_power=0;
        if(!e->index && action) { int r=t->host.power ? t->host.power(t->host.ctx,action) : -ENOSYS;
            if(r)term_printf(t,"Power: %s (error %d)\n",strerror(-r),-r); else term_write(t,"Power request sent.\n"); }
        return;
    }
    if(e->type==DU_EV_CHANGE && e->id==SCROLL) { t->top=(unsigned)du_get(u,SCROLL)->value; t->follow=0; update(t); return; }
    if(e->type==DU_EV_CONTEXT && e->id==GRID) { du_popup(u,t->menu_items,3,e->x,e->y,GRID); return; }
    if(e->type==DU_EV_ACTIVATE) { if(e->id==COPY)copy_selection(t); else if(e->id==CLEAR)term_execute(t,"CLS"); else if(e->id==HELP)term_execute(t,"HELP"); return; }
    if(e->type==DU_EV_POINTER && e->id==GRID) {
        struct du_rect b=du_get(u,GRID)->box; int x=(e->x-b.x)/TERM_CELL_W,y=(e->y-b.y)/TERM_CELL_H;
        if(x<0)x=0; if(y<0)y=0; if((unsigned)x>t->columns)x=(int)t->columns; if((unsigned)y>=t->rows)y=(int)t->rows-1;
        size_t at=(size_t)(t->top+(unsigned)y)*t->columns+(unsigned)x;
        size_t start=input_start(t),limit=start+strlen(t->input);
        if(e->index==DU_POINTER_DOWN && e->key==1) {
            t->selecting=at>=start && !t->busy?2:1;
            t->output_focus=t->selecting==1;
            if(t->selecting==2) {
                t->selection_a=t->selection_b=0;
                du_select_click(&t->text_selection,t->input,at>limit?strlen(t->input):at-start,
                                (e->modifiers&DU_SHIFT)!=0,u->now,e->x,e->y,&t->anchor,&t->cursor);
            } else {
                t->anchor=t->cursor;
                char row[TERM_LINE+1]; unsigned index=(unsigned)(at/t->columns); view_row(t,index,row,sizeof(row));
                size_t lo=0,hi=0;
                du_select_click(&t->text_selection,row,at%t->columns,0,u->now,e->x,e->y,&lo,&hi);
                size_t base=(size_t)index*t->columns;
                if(t->text_selection.clicks==3)hi=t->columns;
                t->text_selection.lo=base+lo; t->text_selection.hi=base+hi;
                if(!(e->modifiers&DU_SHIFT))t->selection_a=base+lo;
                t->selection_b=base+hi;
            }
        } else if((e->index==DU_POINTER_MOVE || e->index==DU_POINTER_UP) && t->selecting) {
            if(t->selecting==2)du_select_drag(&t->text_selection,t->input,at<start?0:at>limit?strlen(t->input):at-start,&t->anchor,&t->cursor);
            else {
                char row[TERM_LINE+1]; unsigned index=(unsigned)(at/t->columns); view_row(t,index,row,sizeof(row));
                size_t lo,hi; du_text_unit(row,at%t->columns,t->text_selection.clicks,&lo,&hi);
                size_t base=(size_t)index*t->columns;
                if(t->text_selection.clicks==3)hi=t->columns;
                if(t->text_selection.clicks<=1)t->selection_b=base+lo;
                else if(at<t->text_selection.lo) { t->selection_a=t->text_selection.hi; t->selection_b=base+lo; }
                else { t->selection_a=t->text_selection.lo; t->selection_b=base+hi; }
            }
        }
        if(e->index==DU_POINTER_UP || e->index==DU_POINTER_CANCEL)t->selecting=0;
        u->dirty=1; return;
    }
    if(e->type!=DU_EV_KEY)return;
    t->text_selection.ns=0;
    if(t->output_focus && (e->modifiers&DU_SHIFT) &&
       (e->key==DU_KEY_LEFT || e->key==DU_KEY_RIGHT || e->key==DU_KEY_UP || e->key==DU_KEY_DOWN || e->key==DU_KEY_HOME || e->key==DU_KEY_END)) {
        size_t at=t->selection_b,last=(size_t)total(t)*t->columns;
        if(e->key==DU_KEY_LEFT) { if(at)at--; }
        else if(e->key==DU_KEY_RIGHT) { if(at<last)at++; }
        else if(e->key==DU_KEY_UP)at=at>t->columns?at-t->columns:0;
        else if(e->key==DU_KEY_DOWN)at=at+t->columns<last?at+t->columns:last;
        else if(e->key==DU_KEY_HOME)at-=at%t->columns;
        else { char row[TERM_LINE+1]; view_row(t,(unsigned)(at/t->columns),row,sizeof(row)); at=at/t->columns*t->columns+strlen(row); }
        t->selection_b=at; unsigned row=(unsigned)(at/t->columns);
        if(row<t->top)t->top=row; else if(row>=t->top+t->rows)t->top=row-t->rows+1;
        t->follow=0; update(t); return;
    }
    if(e->modifiers & DU_CTRL) {
        if(e->key==0x2e) { if(t->busy)t->interrupted=1; else copy_selection(t); }
        else if(t->output_focus && e->key==0x1e) { t->selection_a=0; t->selection_b=(size_t)total(t)*t->columns; u->dirty=1; }
        else if(t->output_focus && e->key==0x2d)copy_selection(t);
        else if(!t->busy && (e->key==0x1e || e->key==0x2d || e->key==0x2f)) {
            struct du_widget edit={.kind=DU_EDIT,.id=GRID,.cursor=t->cursor,.anchor=t->anchor};
            strcpy(edit.text,t->input); du_edit_key(u,&edit,e->key);
            strcpy(t->input,edit.text); t->cursor=edit.cursor; t->anchor=edit.anchor;
            t->selection_a=t->selection_b=0; t->output_focus=0; t->follow=1; update(t);
        }
        return;
    }
    if(t->busy)return;
    if(t->output_focus && (e->key==DU_KEY_DELETE || e->key==DU_KEY_BACKSPACE))return;
    if(e->key==DU_KEY_ENTER) { term_submit(t); return; }
    if(e->key==DU_KEY_TAB) { term_complete(t); return; }
    if(e->key==DU_KEY_PGUP || e->key==DU_KEY_PGDN) {
        if(e->key==DU_KEY_PGUP)t->top=t->top>t->rows ? t->top-t->rows : 0; else t->top+=t->rows;
        t->follow=0; update(t); return;
    }
    if((e->key==DU_KEY_UP || e->key==DU_KEY_DOWN) && !(e->modifiers&DU_SHIFT)) {
        if(t->history_at==t->history_count)strcpy(t->draft,t->input);
        if(e->key==DU_KEY_UP && t->history_at)t->history_at--; else if(e->key==DU_KEY_DOWN && t->history_at<t->history_count)t->history_at++;
        strcpy(t->input,t->history_at<t->history_count ? t->history[t->history_at] : t->draft); t->anchor=t->cursor=strlen(t->input);
    }
    else if(e->key==DU_KEY_ESC) { t->input[0]=0; t->anchor=t->cursor=0; }
    else if(e->ch)insert(t,e->ch);
    else {
        struct du_widget edit={.kind=DU_EDIT,.id=GRID,.cursor=t->cursor,.anchor=t->anchor};
        strcpy(edit.text,t->input); du_edit_key(u,&edit,e->key);
        strcpy(t->input,edit.text); t->cursor=edit.cursor; t->anchor=edit.anchor;
    }
    t->selection_a=t->selection_b=0; t->output_focus=0;
    t->follow=1; update(t);
}
int term_attach(struct terminal *t,struct du_ui *u,const struct term_host *h)
{
    memset(t,0,sizeof(*t)); t->host=*h; t->ui=u; strcpy(t->cwd,"/Documents"); t->columns=80; t->rows=25; t->follow=1;
    u->event=term_event; u->paint=paint; u->layout=layout; u->user=t;
    struct du_widget *g=du_add(u,DU_CANVAS,GRID,(struct du_rect){0},""); if(!g)return -ENOMEM; g->accepts_tab=1; g->pointer_cursor=DESK_CURSOR_TEXT;
    struct du_widget *s=du_add(u,DU_SCROLLBAR,SCROLL,(struct du_rect){0},""); if(!s)return -ENOMEM; s->vertical=1;
    /* Keep the grid at 80x25: actions are available from the context menu. */
    t->menu_items[0]=(struct du_menu_item){"&Copy",COPY,0,0,0};
    t->menu_items[1]=(struct du_menu_item){"C&lear",CLEAR,0,0,0}; t->menu_items[2]=(struct du_menu_item){"&Help",HELP,0,0,0};
    layout(u,t); du_focus(u,GRID); t->history_error=term_history_load(t);
    term_write(t,"CiukiOS Terminal. Type HELP for commands.\n");
    if(t->history_error)term_printf(t,"History: %s (error %d)\n",strerror(-t->history_error),-t->history_error);
    return 0;
}
void term_detach(struct terminal *t)
{
    term_lua_close(t); for(unsigned i=0;i<t->count;i++)free(t->lines[(t->first+i)%TERM_SCROLLBACK]);
    for(unsigned i=0;i<t->history_count;i++)free(t->history[i]); free(t->chunk); t->chunk=NULL;
}
