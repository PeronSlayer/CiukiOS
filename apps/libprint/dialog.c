/* SPDX-License-Identifier: MIT
 * Native Print and Page Setup: points, inclusive page range, copy count and
 * an explicit output path. Rendering and disk writes run off the UI thread. */
#include "dialog.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { PRINT_DIALOG=31000, PAGE_DIALOG, PRINT_NOTICE };
static void notice(struct du_ui *u, const char *text)
{ du_message(u,PRINT_NOTICE,"Printing",text,DU_DLG_ICON_INFO,NULL,0,0,0); }
static int init(struct cp_ui *p)
{
    int first=!p->initialized;
    if (first) { cp_page_default(&p->page);p->printer=CP_PRINTERS; }
    if (!p->config) p->config=malloc(sizeof(*p->config));
    if (!p->config) { errno=ENOMEM;return -1; }
    int e=cp_config_load(p->root,p->config);
    if (!e && first) { if (!p->preset) p->page=p->config->printers[p->config->default_printer].page;p->initialized=1; }
    return e;
}
static struct du_widget *field(struct du_ui *u,unsigned n,enum du_kind kind,const char *text,int y)
{
    struct du_dialog *d=&u->dialog;struct du_widget *w=&d->items[n];
    memset(w,0,sizeof(*w));w->kind=kind;w->id=-32000-(int)n;
    w->box=(struct du_rect){d->box.x+12,d->box.y+y,d->box.w-24,20};
    du_set_text(w,text);if (d->count<=n) d->count=n+1;return w;
}
static void box(struct du_ui *u,int id,const char *title)
{
    static const char *const buttons[]={"OK","Cancel"};
    du_message(u,id,title,"",0,buttons,2,0,1);
    struct du_dialog *d=&u->dialog;
    int w=u->canvas.width-8,h=u->canvas.height-8;
    if (w>480) w=480;if (h>308) h=308;
    d->box=(struct du_rect){(u->canvas.width-w)/2,(u->canvas.height-h)/2,w,h};
    d->items[0].box=(struct du_rect){d->box.x+w-174,d->box.y+h-30,75,23};
    d->items[1].box=(struct du_rect){d->box.x+w-93,d->box.y+h-30,75,23};
    d->focus=2;u->dirty=1;
}
static int integers(const char *text,unsigned *a,unsigned *b,unsigned *c,unsigned *d,unsigned count)
{
    unsigned *out[]={a,b,c,d};const char *s=text;
    for (unsigned i=0;i<count;i++) {
        while (*s==' ') s++;
        if (*s<'0'||*s>'9') return 0;
        char *end;unsigned long v=strtoul(s,&end,10);
        if (v>10000) return 0;*out[i]=(unsigned)v;s=end;
    }
    while (*s==' ') s++;
    return !*s;
}
static int accept(struct du_ui *u,int button,void *arg)
{
    struct cp_ui *p=arg;struct du_dialog *d=&u->dialog;
    if (button==d->cancel) return 0;
    struct cp_page page=p->page;unsigned a=0,b=0,c=0,e=0;
    if (d->id==PAGE_DIALOG) {
        if (!integers(d->items[3].text,&a,&b,&c,&e,2)) goto invalid;
        page.width=a;page.height=b;page.landscape=!!d->items[4].value;
        if (!integers(d->items[6].text,&a,&b,&c,&e,4)) goto invalid;
        page.left=a;page.top=b;page.right=c;page.bottom=e;
    } else {
        if (d->items[2].active>=d->items[2].count) goto invalid;
        unsigned printer=p->printer_ids[d->items[2].active];
        if (p->image && p->config->printers[printer].driver==CP_TEXT) {
            du_set_text(&d->items[9],"Pictures require a PostScript or PCL printer.");u->dirty=1;return 1;
        }
        if (!integers(d->items[5].text,&a,&b,&c,&e,3)) goto invalid;
        page.copies=a;page.first=b;page.last=c;
        if (p->config->printers[printer].driver==CP_PCL &&
            !((page.width==612 && (page.height==792 || page.height==1008)) || (page.width==595 && page.height==842))) {
            du_set_text(&d->items[9],"PCL supports Letter, Legal or A4 paper.");u->dirty=1;return 1;
        }
        if (d->items[6].value && (d->items[7].text[0]!='/' || strlen(d->items[7].text)>=sizeof(p->target))) goto invalid;
        p->printer=printer;
        snprintf(p->target,sizeof(p->target),"%s",d->items[6].value?d->items[7].text:"");
    }
    if (!cp_page_valid(&page)) goto invalid;
    p->page=page;return 0;
invalid:
    du_set_text(&d->items[9],"Check the values and try again.");u->dirty=1;return 1;
}
void cp_ui_setup(struct cp_ui *p,struct du_ui *u)
{
    if (p->active || p->job.started) return;
    if (init(p)) { notice(u,strerror(errno));return; }
    box(u,PAGE_DIALOG,"Page Setup");char text[96];
    field(u,2,DU_LABEL,"Paper width and height in points (72 = 1 inch):",34);
    snprintf(text,sizeof(text),"%u %u",p->page.width,p->page.height);field(u,3,DU_EDIT,text,57);
    field(u,4,DU_CHECKBOX,"&Landscape",84)->value=p->page.landscape;
    field(u,5,DU_LABEL,"Margins: left top right bottom (points):",112);
    snprintf(text,sizeof(text),"%u %u %u %u",p->page.left,p->page.top,p->page.right,p->page.bottom);
    field(u,6,DU_EDIT,text,135);
    field(u,7,DU_LABEL,"A4: 595 842. Letter: 612 792. Legal: 612 1008.",163);
    field(u,9,DU_LABEL,"",190);u->dialog.accept=accept;u->dialog.accept_ctx=p;p->active=2;
}
static void print_dialog(struct cp_ui *p,struct du_ui *u)
{
    if (init(p)) { free(p->snapshot);p->snapshot=NULL;notice(u,strerror(errno));return; }
    unsigned selected=p->printer<CP_PRINTERS?p->printer:p->config->default_printer;
    box(u,PRINT_DIALOG,"Print");du_set_text(&u->dialog.items[0],"&Print");
    struct du_widget *list=field(u,2,DU_LISTVIEW,"",34);list->box.h=60;list->view=DU_LIST;
    for (unsigned i=0;i<CP_PRINTERS;i++) if (p->config->printers[i].present) {
        unsigned k=list->count++;p->printer_ids[k]=i;
        p->printer_rows[k].text[0]=p->config->printers[i].name;
        if(i==selected)list->active=k;
    }
    list->items=p->printer_rows;
    char text[80];
    field(u,4,DU_LABEL,"Copies, first page, last page (0 0 = all):",100);
    snprintf(text,sizeof(text),"%u %u %u",p->page.copies,p->page.first,p->page.last);field(u,5,DU_EDIT,text,122);
    field(u,6,DU_CHECKBOX,"Print to &file (output path below):",148);
    field(u,7,DU_EDIT,"/Documents/print-output.prn",172);
    field(u,9,DU_LABEL,"",200);u->dialog.focus=2;u->dialog.accept=accept;u->dialog.accept_ctx=p;p->active=1;
}
void cp_ui_text(struct cp_ui *p,struct du_ui *u,const char *title,const char *text,size_t bytes)
{
    if (p->active || p->job.started) return;
    if (bytes>CP_MAX_BYTES || (!text&&bytes)) { notice(u,"The document is too large to print.");return; }
    p->snapshot=malloc(bytes?bytes:1);
    if (!p->snapshot) { notice(u,"Not enough memory to print.");return; }
    if (bytes) memcpy(p->snapshot,text,bytes);
    p->document=0;p->preset=0;p->bytes=bytes;p->image=0;snprintf(p->title,sizeof(p->title),"%s",title?title:"Document");print_dialog(p,u);
}
void cp_ui_image(struct cp_ui *p,struct du_ui *u,const char *title,const uint32_t *pixels,unsigned width,unsigned height)
{
    if (p->active || p->job.started) return;
    if (!pixels || !width || !height || width>CP_MAX_BYTES/4/height) { notice(u,"The picture is too large to print.");return; }
    p->bytes=(size_t)width*height*4;p->snapshot=malloc(p->bytes);
    if (!p->snapshot) { notice(u,"Not enough memory to print.");return; }
    memcpy(p->snapshot,pixels,p->bytes);p->width=width;p->height=height;p->image=1;p->document=0;p->preset=0;
    snprintf(p->title,sizeof(p->title),"%s",title?title:"Picture");print_dialog(p,u);
}
void cp_ui_doc(struct cp_ui *p,struct du_ui *u,const char *title,const struct cp_page *initial,cp_prepare_fn prepare,void *ctx)
{
    if (p->active || p->job.started || !prepare) return;
    p->document=1;p->image=0;p->prepare=prepare;p->prepare_ctx=ctx;
    if (initial && cp_page_valid(initial)) { p->page=*initial;p->preset=1;p->printer=CP_PRINTERS; }
    else p->preset=0;
    snprintf(p->title,sizeof(p->title),"%s",title?title:"Document");print_dialog(p,u);
}
static void submit(void *arg)
{
    struct cp_ui *p=arg;
    if (p->document) p->result=cp_submit_doc(p->root,p->printer,p->title,&p->doc,&p->page,p->target[0]?p->target:NULL);
    else if (p->image) p->result=cp_submit_image(p->root,p->printer,p->title,p->snapshot,p->width,p->height,&p->page,p->target[0]?p->target:NULL);
    else p->result=cp_submit_text(p->root,p->printer,p->title,p->snapshot,p->bytes,&p->page,p->target[0]?p->target:NULL);
    p->error=errno;
}
int cp_ui_event(struct cp_ui *p,struct du_ui *u,const struct du_event *e)
{
    if (e->type!=DU_EV_DIALOG || (e->id!=PRINT_DIALOG && e->id!=PAGE_DIALOG && e->id!=PRINT_NOTICE)) return 0;
    if (e->id==PRINT_NOTICE) return 1;
    p->active=0;
    if (e->id==PAGE_DIALOG) return 1;
    if (e->index!=0) { free(p->snapshot);p->snapshot=NULL;p->document=0;return 1; }
    if (p->document) {   /* lay the document out for the chosen printer on this thread; the worker only writes it */
        cp_doc_free(&p->doc);
        int pr=p->prepare(p->prepare_ctx,&p->page,p->config->printers[p->printer].driver,&p->doc);
        if (pr) { cp_doc_free(&p->doc);p->document=0;notice(u,pr==-ENOMEM?"Not enough memory to print.":"The document could not be prepared for printing.");return 1; }
    }
    int r=du_job_start_ui(&p->job,submit,p,u);
    if (r) { free(p->snapshot);p->snapshot=NULL;cp_doc_free(&p->doc);p->document=0;notice(u,strerror(-r)); }
    return 1;
}
void cp_ui_poll(struct cp_ui *p,struct du_ui *u)
{
    if (!du_job_ready(&p->job) || u->dialog.kind) return;
    du_job_finish(&p->job);free(p->snapshot);p->snapshot=NULL;cp_doc_free(&p->doc);p->document=0;
    char text[128];
    if (p->result<0) snprintf(text,sizeof(text),"The document could not be queued: %s",strerror(p->error));
    else snprintf(text,sizeof(text),"Document queued as job %d. Open Printers to view or cancel it.",p->result);
    notice(u,text);
}
void cp_ui_destroy(struct cp_ui *p)
{ du_job_finish(&p->job);free(p->snapshot);free(p->config);cp_doc_free(&p->doc);p->snapshot=NULL;p->config=NULL; }
