/* SPDX-License-Identifier: MIT
 * /Programs/hwreport [--channel-fd=N] [--rescue]
 * /Programs/hwreport --rescue --text-fd=N   (headless: no window, no file writes) */
#include "native.h"
#include "textframe.h"
#include <ciuki/channel.h>
#include <signal.h>
#include "../accessories/acc.h"
#include "../libdeskui/textarea.h"
#include "../libdeskui/job.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { PREVIEW=1, DRIVE, SAVE, STATUS, CONFIRM, LOGS, SWITCH };
struct app {
    struct du_window window;
    struct du_ui ui;
    struct du_textarea preview;
    struct du_job job;
    struct hr_native native;
    char drive_names[26][32];
    unsigned letters[26],count;
    char *text; size_t length;
    int result,loaded,saving,started,asked,show_logs;
    uint64_t query_deadline;
};
static void status(struct app *a,const char *text)
{ du_set_text(du_get(&a->ui,STATUS),text); du_invalidate_widget(&a->ui,STATUS); }
static void preview_page(struct app *a)
{
    if (!a->show_logs) { (void)du_ta_set_text(&a->preview,a->text,a->length); status(a,"Report preview: CIUKIREP.TXT."); }
    else {
        size_t offset=(size_t)(a->show_logs-1)*HR_TEXT_MAX;
        size_t length=a->native.full_length-offset;
        if (length>HR_TEXT_MAX) length=HR_TEXT_MAX;
        (void)du_ta_set_text(&a->preview,a->native.full_logs+offset,length);
        char text[160]; snprintf(text,sizeof(text),"Full log preview %u/%u. Next preview cycles through all parts and the report.",
            (unsigned)a->show_logs,(unsigned)((a->native.full_length+HR_TEXT_MAX-1)/HR_TEXT_MAX));
        status(a,text);
    }
    du_invalidate_widget(&a->ui,PREVIEW);
}
static void collect(void *ctx)
{
    struct app *a=ctx;
    hr_native_collect(&a->native);
    a->result=hr_format(&a->native.source,a->text,HR_TEXT_MAX,&a->length);
}
static void save(void *ctx)
{ struct app *a=ctx; a->result=hr_native_save(&a->native,a->text,a->length); }
static void prepare(void *ctx)
{ struct app *a=ctx; a->result=hr_native_prepare_logs(&a->native); }
static void begin_collect(struct app *a)
{
    if (a->started) return;
    a->started=1;
    int e=du_job_start(&a->job,collect,a);
    if (e) { a->result=e; status(a,"Unable to start collection worker."); }
}
static void message(struct du_window *w,const struct desk_packet *p,void *ctx)
{
    (void)w; struct app *a=ctx;
    if (a->started || p->serial!=1) return;
    if (p->opcode==DESK_GFX_INFO) {
        if (!p->result) { a->native.engine=p->accelerator; a->native.source.engine=&a->native.engine; }
        begin_collect(a);
    } else if (p->opcode==DESK_TASK_RESULT && p->request==DESK_GFX_QUERY) begin_collect(a);
}
static void idle(void *ctx)
{
    struct app *a=ctx;
    if (!a->asked && a->window.configured) {
        struct ciuki_message q; desk_message(&q,DESK_GFX_QUERY,8); desk_put32(q.data+4,1);
        if (!du_window_queue(&a->window,&q)) { a->asked=1; a->query_deadline=du_now()+2000000000ull; }
    }
    if (!a->started && a->asked && du_now()>=a->query_deadline) begin_collect(a);
    if (!du_job_ready(&a->job)) return;
    du_job_finish(&a->job);
    if (a->saving==2) {
        a->saving=0;
        if (a->result) status(a,"Full logs unavailable (I/O, size limit or memory). Report can still be saved.");
        else {
            a->show_logs=1;
            preview_page(a);
            du_set_text(du_get(&a->ui,LOGS),"Exclude optional full logs");
            du_get(&a->ui,SWITCH)->disabled=0;
        }
    } else if (a->saving) {
        a->saving=0;
        char text[192];
        if (a->result) snprintf(text,sizeof(text),"Save failed: %s (%d). No success confirmed. Retry on another drive.",strerror(-a->result),-a->result);
        else if (a->native.archive_result) snprintf(text,sizeof(text),"CIUKIREP.TXT saved; CIUKILOG.GZ failed: %s (%d).",strerror(-a->native.archive_result),-a->native.archive_result);
        else snprintf(text,sizeof(text),"Saved %c:\\CIUKIREP.TXT%s.",'A'+a->native.selected,a->native.archive ? " and CIUKILOG.GZ" : "");
        status(a,text);
    } else {
        if (a->result || du_ta_set_text(&a->preview,a->text,a->length)) {
            status(a,"Collection failed; no report saved."); return;
        }
        a->loaded=1;
        for (unsigned i=0;i<26;i++) if (a->native.writable&(1u<<i)) {
            unsigned row=a->count++; a->letters[row]=i;
            snprintf(a->drive_names[row],sizeof(a->drive_names[row]),"%c: %s",'A'+i,
                a->native.usb&(1u<<i) ? "USB FAT" : i<2 ? "Floppy" : "Disk");
            if (i==a->native.selected) du_get(&a->ui,DRIVE)->active=row;
        }
        if (a->count) du_set_text(du_get(&a->ui,DRIVE),a->drive_names[du_get(&a->ui,DRIVE)->active]);
        status(a,a->count ? "Review the report. Click Drive to change the destination, then Save." :
            a->native.rescue ? "No USB FAT drive found. Insert one and reopen Hardware Report." : "No writable drive found. Reopen after inserting a medium.");
    }
    du_get(&a->ui,SAVE)->disabled=!a->loaded || !a->count;
    du_get(&a->ui,DRIVE)->disabled=!a->count;
    du_get(&a->ui,LOGS)->disabled=!a->loaded;
    du_invalidate(&a->ui,(struct du_rect){0,0,640,480});
}
static void event(struct du_ui *ui,const struct du_event *e,void *ctx)
{
    struct app *a=ctx;
    if (e->type==DU_EV_ACTIVATE && e->id==SWITCH && a->native.archive && !a->job.started) {
        a->show_logs++;
        if ((size_t)a->show_logs>(a->native.full_length+HR_TEXT_MAX-1)/HR_TEXT_MAX) a->show_logs=0;
        preview_page(a);
    }
    if (e->type==DU_EV_ACTIVATE && e->id==LOGS && a->loaded && !a->job.started) {
        if (a->native.archive) {
            free(a->native.archive); free(a->native.full_logs); a->native.archive=NULL; a->native.full_logs=NULL;
            a->show_logs=0; (void)du_ta_set_text(&a->preview,a->text,a->length);
            du_set_text(du_get(ui,LOGS),"Include optional full logs"); du_get(ui,SWITCH)->disabled=1;
            status(a,"Only CIUKIREP.TXT will be saved."); du_invalidate(ui,(struct du_rect){0,0,640,480});
        } else {
            a->saving=2; int error=du_job_start(&a->job,prepare,a);
            if (error) { a->saving=0; status(a,"Unable to start log worker."); return; }
            du_get(ui,LOGS)->disabled=1; du_get(ui,SAVE)->disabled=1; du_get(ui,DRIVE)->disabled=1;
            status(a,"Preparing full BOOT.LOG, BOOT1.LOG and BOOT2.LOG with the same privacy filter...");
        }
    }
    if (e->type==DU_EV_ACTIVATE && e->id==DRIVE && a->count && !a->job.started) {
        struct du_widget *w=du_get(ui,DRIVE); w->active=(w->active+1)%a->count;
        du_set_text(w,a->drive_names[w->active]); du_invalidate_widget(ui,DRIVE);
    }
    if (e->type==DU_EV_ACTIVATE && e->id==SAVE && a->loaded && a->count && !a->job.started) {
        static const char *const buttons[]={"&Save","&Cancel"};
        du_message(ui,CONFIRM,"Save Hardware Report",
            a->native.archive ? "Save CIUKIREP.TXT and the reviewed full logs as CIUKILOG.GZ?\nExisting exports will be replaced after each new file is written successfully." :
            "Save the report preview as CIUKIREP.TXT on the selected drive?\nAn existing report will be replaced after the new file is written successfully.",
            DU_DLG_ICON_INFO,buttons,2,1,1);
    }
    if (e->type==DU_EV_DIALOG && e->id==CONFIRM && e->index==0 && !a->job.started) {
        unsigned row=du_get(ui,DRIVE)->active;
        if (row>=a->count) return;
        a->native.selected=a->letters[row]; a->saving=1;
        int error=du_job_start(&a->job,save,a);
        if (error) { a->saving=0; status(a,"Unable to start save worker."); return; }
        du_get(ui,SAVE)->disabled=1; du_get(ui,DRIVE)->disabled=1;
        du_get(ui,LOGS)->disabled=1;
        status(a,"Writing report. Keep the medium connected until saving finishes."); du_invalidate(ui,(struct du_rect){0,0,640,480});
    }
}
static void labels(struct du_ui *ui,int id,int y,const char *text)
{
    while (*text) {
        const char *end=strchr(text,'\n'); size_t n=end ? (size_t)(end-text) : strlen(text);
        char line[128]; if (n>=sizeof(line)) n=sizeof(line)-1;
        memcpy(line,text,n); line[n]=0;
        du_add(ui,DU_LABEL,id++,(struct du_rect){12,y,616,16},line); y+=16;
        if (!end) break;
        text=end+1;
    }
}
/* Headless mode for Setup's rescue page: the report text goes to the parent
 * over the channel on descriptor fd, framed (textframe.h), and nothing else
 * happens: no window, no drive selection, no file written. */
static int link_send(void *ctx,const void *data,size_t bytes)
{
    int fd=*(int *)ctx; struct ciuki_message m;
    if (bytes>sizeof(m.data)) return -EMSGSIZE;
    memset(&m,0,sizeof(m)); m.length=(uint32_t)bytes; memcpy(m.data,data,bytes);
    while (ciuki_channel_send(fd,&m,0)) { if (errno!=EINTR) return -errno; }
    return 0;
}
static int text_mode(int fd)
{
    if (signal(SIGPIPE,SIG_IGN)==SIG_ERR) return 1;
    struct hr_native *n=calloc(1,sizeof(*n)); char *text=malloc(HR_TEXT_MAX);
    if (!n || !text) { free(n); free(text); return 1; }
    n->rescue=1;
    hr_native_collect(n);
    struct hr_link_io io={&fd,link_send}; int status=0;
    int r=hr_link_report(&n->source,text,HR_TEXT_MAX,&io,&status);
    hr_native_free(n); free(n); free(text);
    return r ? 1 : 0;
}
int main(int argc,char **argv)
{
    int rescue=0,text_fd=-1;
    for (int i=1;i<argc;i++) if (!strncmp(argv[i],"--text-fd=",10)) {
        char *end; long v=strtol(argv[i]+10,&end,10);
        if (!argv[i][10] || *end || v<3 || v>=64 || text_fd>=0) return 2;
        text_fd=(int)v; memmove(argv+i,argv+i+1,(size_t)(argc-i)*sizeof(*argv)); argc--; i--;
    }
    for (int i=1;i<argc;i++) if (!strcmp(argv[i],"--rescue")) {
        rescue=1; memmove(argv+i,argv+i+1,(size_t)(argc-i)*sizeof(*argv)); argc--; i--;
    }
    /* Text mode is only for the rescue page and takes no other argument. */
    if (text_fd>=0) return rescue && argc==1 ? text_mode(text_fd) : 2;
    int channel,test; const char *path;
    if (acc_args(argc,argv,"hwreport",&channel,&test,&path) || path || test) return 2;
    struct app *a=calloc(1,sizeof(*a)); if (!a) return 1;
    a->native.rescue=rescue; a->text=malloc(HR_TEXT_MAX);
    if (!a->text || du_ta_init(&a->preview,HR_TEXT_MAX)) { free(a->text); free(a); return 1; }
    if (du_window_open(&a->window,&a->ui,channel,640,480,"Hardware Report")) {
        du_ta_free(&a->preview); free(a->text); free(a); return 1;
    }
    a->window.user=a; a->window.message=message;
    a->ui.user=a; a->ui.event=event;
    labels(&a->ui,20,8,hr_summary);
    labels(&a->ui,30,60,hr_privacy);
    struct du_widget *preview=du_add(&a->ui,DU_TEXTAREA,PREVIEW,(struct du_rect){12,126,616,246},"");
    preview->data=&a->preview; a->preview.readonly=1;
    du_add(&a->ui,DU_BUTTON,LOGS,(struct du_rect){12,378,300,24},"Include optional full logs")->disabled=1;
    du_add(&a->ui,DU_BUTTON,SWITCH,(struct du_rect){468,378,160,24},"Next preview")->disabled=1;
    du_add(&a->ui,DU_LABEL,12,(struct du_rect){12,413,62,20},"Drive:");
    du_add(&a->ui,DU_BUTTON,DRIVE,(struct du_rect){76,408,270,26},"")->disabled=1;
    du_add(&a->ui,DU_BUTTON,SAVE,(struct du_rect){468,408,160,26},"&Save CIUKIREP.TXT")->disabled=1;
    du_add(&a->ui,DU_STATUS,STATUS,(struct du_rect){0,452,640,28},"Collecting hardware inventory...");
    int r=du_run(&a->window,idle,a);
    du_job_finish(&a->job); hr_native_free(&a->native); du_ta_free(&a->preview);
    du_window_destroy(&a->window); free(a->text); free(a); return r<0 ? 1 : 0;
}
