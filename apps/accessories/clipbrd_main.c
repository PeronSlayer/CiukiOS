/* SPDX-License-Identifier: MIT. Clipboard Viewer, native shared clipboard. */
#include "acc.h"
#include "../desktop/profile.h" /* sec-user-04: My Documents is per user */
#include "../libdeskui/clipboard.h"
#include "../libdeskui/clipstore.h"
#include "../libdeskui/clipfile.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __linux__
#undef _SC_ARG_MAX
#undef _SC_OPEN_MAX
#undef _SC_PAGESIZE
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
#endif
#include <unistd.h>
enum { OPEN=1,SAVE,DELETE,EXIT,FORMAT=100,OPEN_DIALOG=200,SAVE_DIALOG };
struct viewer {
    struct du_window window; struct du_ui ui;
    struct du_menu menus[3]; struct du_menu_item file[3],edit[1],display[DESK_CLIP_LIST_MAX];
    char labels[DESK_CLIP_LIST_MAX][160];
    struct desk_clip_wire formats;
    unsigned selected,width,height;
    uint32_t *image; char *text;
    uint64_t poll;
};
static const char *format_name(unsigned f)
{
    switch (f) { case 1:return "ANSI text"; case 2:return "Bitmap"; case 7:return "OEM text"; case 8:return "DIB";
    case 0x80:return "Owner display"; case 3:return "Metafile picture"; case 9:return "Palette"; case 14:return "Enhanced metafile";
    case 13:return "Unicode text"; case 15:return "Files (CF_HDROP)"; case 16:return "Locale"; case 17:return "DIB V5"; default:return "Private format"; }
}
static void preview(struct viewer *v)
{
    free(v->text); v->text=NULL; free(v->image); v->image=NULL; v->width=v->height=0;
    void *data=NULL; uint32_t n=0;
    unsigned f=v->selected;
    if (f==0x80) {
        uint8_t request[24]={0}; uint32_t bytes=0; void *reply=NULL;
        unsigned width=v->ui.canvas.width>20 ? v->ui.canvas.width-20 : 1;
        unsigned height=v->ui.canvas.height>70 ? v->ui.canvas.height-70 : 1;
        desk_put32(request+16,width); desk_put32(request+20,height);
        (void)du_clip_display(0x30b,request,24,&reply,&bytes); free(reply); reply=NULL;
        if (!du_clip_display(0x309,request,24,&reply,&bytes) && bytes>4)
            (void)clip_dib_decode((unsigned char *)reply+4,bytes-4,&v->image,&v->width,&v->height);
        free(reply); v->ui.dirty=1; return;
    }
    if (!f || du_clip_open()) return;
    int e=du_clip_get(f==2 ? 8 : f,&data,&n);
    if (!e && (f==8 || f==2 || f==17)) (void)clip_dib_decode(data,n,&v->image,&v->width,&v->height);
    if (!e && (f==1 || f==7 || f==13)) {
        free(data); data=NULL;
        if (!du_clip_get(13,&data,&n)) (void)clip_text_decode(data,n,&v->text);
    }
    if (!e && f==15 && n>=22) {
        unsigned char *b=data; unsigned offset=desk_get32(b),wide=desk_get32(b+16);
        if (offset>=20 && offset<n && (!wide || !((offset|n)&1))) {
            if (wide) {
                for (unsigned k=offset;k+3<n;k+=2) if (!b[k] && !b[k+1] && (b[k+2] || b[k+3])) b[k]='\n';
                (void)clip_text_decode(b+offset,(n-offset)&~1u,&v->text);
            } else {
                v->text=malloc(n-offset+1);
                if (v->text) { for (unsigned k=offset;k<n;k++) v->text[k-offset]=b[k] ? (char)b[k] : '\n'; v->text[n-offset]=0; }
            }
        }
    }
    free(data); du_clip_close(); v->ui.dirty=1;
}
static void refresh(struct viewer *v)
{
    struct desk_clip_wire q;
    if (du_clip_query(&q) || (q.sequence==v->formats.sequence && v->poll)) return;
    v->formats=q; unsigned present=0;
    int opened=!du_clip_open();
    for (unsigned i=0;i<q.count;i++) {
        unsigned f=q.formats[i]; char name[128]; snprintf(name,sizeof(name),"%s",format_name(f));
        if (f==0x80) {
            unsigned char request[8]={0}; desk_put32(request,sizeof(name)); void *reply=NULL; uint32_t bytes=0;
            if (!du_clip_display(0x30c,request,8,&reply,&bytes) && bytes>4) {
                unsigned used=bytes-4; if (used>=sizeof(name)) used=sizeof(name)-1;
                memcpy(name,(unsigned char *)reply+4,used); name[used]=0;
            }
            free(reply);
        }
        if (f>=0xc000) {
            struct desk_clip_wire w={.op=DESK_CLIP_NAME,.format=f},r; void *unused=NULL;
            if (!du_clip_call(&w,NULL,&r,&unused)) snprintf(name,sizeof(name),"%s",r.name);
            free(unused);
        }
        uint32_t bytes=0; void *data=NULL;
        int loaded=opened ? !du_clip_get(f,&data,&bytes) : 0; free(data);
        if (loaded) snprintf(v->labels[i],sizeof(v->labels[i]),"%u: %s (%u bytes)",f,name,bytes);
        else snprintf(v->labels[i],sizeof(v->labels[i]),"%u: %s (unavailable)",f,name);
        v->display[i]=(struct du_menu_item){v->labels[i],FORMAT+(int)i,0,0,0};
        if (f==v->selected) present=1;
    }
    if (opened) du_clip_close();
    if (!present) v->selected=q.count ? q.formats[0] : 0;
    v->menus[2].count=q.count;
    preview(v); v->ui.dirty=1;
}
static void paint(struct du_ui *ui, void *ctx)
{
    struct viewer *v=ctx; struct du_canvas c=ui->canvas;
    du_fill(&c,(struct du_rect){0,20,c.width,c.height-20},0xffffff);
    char title[160]; snprintf(title,sizeof(title),"%s — %u formats",v->selected ? format_name(v->selected) : "Clipboard is empty",v->formats.count);
    du_text_fit(&c,10,32,c.width-20,title,0);
    if (v->text) du_text_wrap(&c,(struct du_rect){10,58,c.width-20,c.height-68},v->text,0);
    if (v->image && v->width && v->height) {
        unsigned w=v->width,h=v->height,maxw=c.width>20 ? c.width-20 : 1,maxh=c.height>70 ? c.height-70 : 1;
        if (w>maxw) { h=(uint64_t)h*maxw/w; w=maxw; }
        if (h>maxh) { w=(uint64_t)w*maxh/h; h=maxh; }
        for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++)
            acc_pixel(&c,x+10,y+60,v->image[(uint64_t)y*v->height/h*v->width+(uint64_t)x*v->width/w]);
    }
}
static int list(void *ctx, const char *path, struct du_file_entry *entries, unsigned cap, unsigned *count)
{
    (void)ctx; DIR *dir=opendir(path); if (!dir) return -errno;
    unsigned n=0; struct dirent *entry;
    while (n<cap && (entry=readdir(dir))) {
        if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        char full[1024]; struct stat s;
        int used=snprintf(full,sizeof(full),"%s/%s",path,entry->d_name);
        if (used<0 || used>=(int)sizeof(full) || stat(full,&s)) continue;
        memset(&entries[n],0,sizeof(entries[n]));
        snprintf(entries[n].name,sizeof(entries[n].name),"%s",entry->d_name);
        entries[n].directory=S_ISDIR(s.st_mode); n++;
    }
    closedir(dir); *count=n; return 0;
}
static int read_clip(const char *path)
{
    FILE *f=fopen(path,"rb"); if (!f) return -errno;
    int e=0; long n;
    if (fseek(f,0,SEEK_END) || (n=ftell(f))<4 || (unsigned long)n>DESK_CLIP_BYTES_MAX+4096 || fseek(f,0,SEEK_SET)) { fclose(f); return -EINVAL; }
    void *data=malloc(n); if (!data) { fclose(f); return -ENOMEM; }
    if (fread(data,1,n,f)!=(size_t)n) e=-EIO;
    if (fclose(f) && !e) e=-EIO;
    if (!e) e=du_clip_import(data,n);
    free(data); return e;
}
static int save_clip(const char *path)
{
    void *data=NULL; uint32_t n; int e=du_clip_export(&data,&n); if (e) return e;
    char temp[1024]; int fd=-1;
    if (snprintf(temp,sizeof(temp),"%s.clipbrd-%lu.tmp",path,(unsigned long)getpid())>=(int)sizeof(temp)) e=-EINVAL;
    if (!e) { fd=open(temp,O_WRONLY|O_CREAT|O_EXCL,0600); if (fd<0) e=-errno; }
    if (fd>=0) {
        unsigned used=0;
        while (used<n) { ssize_t written=write(fd,(char *)data+used,n-used); if (written<0 && errno==EINTR) continue; if (written<=0) { e=-EIO; break; } used+=written; }
        if (!e && fsync(fd)) e=-errno;
        if (close(fd) && !e) e=-errno;
        if (!e && rename(temp,path)) e=-errno;
        if (e) unlink(temp);
    }
    free(data); return e;
}
static void event(struct du_ui *ui, const struct du_event *event, void *ctx)
{
    struct viewer *v=ctx; int e=0;
    if (event->type==DU_EV_ACTIVATE) {
        if (event->id==EXIT) du_window_close(&v->window);
        else if (event->id==OPEN || event->id==SAVE) du_file_dialog(ui,event->id==OPEN ? OPEN_DIALOG : SAVE_DIALOG,event->id==SAVE,event->id==OPEN ? "Open Clipboard" : "Save Clipboard",profile_home(),"clipboard.clp",list,NULL);
        else if (event->id==DELETE) { e=du_clip_open(); if (!e) { e=du_clip_empty(); du_clip_close(); } }
        else if (event->id>=FORMAT && (unsigned)(event->id-FORMAT)<v->formats.count) { v->selected=v->formats.formats[event->id-FORMAT]; preview(v); }
    } else if (event->type==DU_EV_DIALOG && !event->index && event->text) {
        if (event->id==OPEN_DIALOG) e=read_clip(event->text);
        else if (event->id==SAVE_DIALOG) e=save_clip(event->text);
    }
    if (e) du_message(ui,999,"Clipboard Viewer","The clipboard operation failed. The format may be unsupported, too large, or the file unavailable.",DU_DLG_ICON_WARNING,NULL,0,0,0);
}
static void idle(void *ctx)
{
    struct viewer *v=ctx; uint64_t now=du_now();
    if (du_clip_observed()!=v->formats.sequence || now-v->poll>=500000000ull) { refresh(v); v->poll=now; }
}
int main(int argc, char **argv)
{
    int channel,test; const char *path;
    if (acc_args(argc,argv,"clipbrd",&channel,&test,&path)) return 2;
    struct viewer *v=calloc(1,sizeof(*v)); if (!v) return 1;
    if (du_window_open_resizable(&v->window,&v->ui,channel,640,440,"Clipboard Viewer",320,240)) { free(v); return 1; }
    v->file[0]=(struct du_menu_item){"&Open...",OPEN,0,0,0}; v->file[1]=(struct du_menu_item){"&Save As...",SAVE,0,0,0}; v->file[2]=(struct du_menu_item){"E&xit",EXIT,0,0,0};
    v->edit[0]=(struct du_menu_item){"&Delete",DELETE,0,0,0};
    v->menus[0]=(struct du_menu){"&File",v->file,3}; v->menus[1]=(struct du_menu){"&Edit",v->edit,1}; v->menus[2]=(struct du_menu){"&Display",v->display,0};
    v->ui.menus=v->menus; v->ui.menu_count=3; v->ui.paint=paint; v->ui.event=event; v->ui.user=v;
    struct desk_clip_wire watch={.op=DESK_CLIP_WATCH,.flags=1},reply; void *unused=NULL;
    (void)du_clip_call(&watch,NULL,&reply,&unused); free(unused);
    du_timer_start(&v->ui,1,500000000ull,1);
    if (path) (void)read_clip(path);
    int r=du_run(&v->window,idle,v);
    if (v->selected==0x80) {
        unsigned char request[24]={0}; void *reply=NULL; uint32_t bytes=0;
        (void)du_clip_display(0x30b,request,24,&reply,&bytes); free(reply);
    }
    free(v->image); free(v->text); du_window_destroy(&v->window); free(v); return r<0;
}
