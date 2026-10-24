/* SPDX-License-Identifier: MIT
 * /Programs/help: CiukiOS Help and text viewer (desktop client, protocol v2).
 * Usage: help [--channel-fd=N] [--test=help] [topic:<id> | text file]
 * A program's F1 and Help menu start it as "help topic:<program>". */
#include "help.h"
#include "../libdeskui/job.h"
#include "../desktop/help_request.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
struct help_main {
    struct du_window window; struct du_ui ui; struct help_app app; struct du_job opening;
    char pending[161], working[161], path[161]; char *text; size_t length;
    struct hf_document *document; unsigned page;
    int error; uint64_t open_revision,request_revision,opening_revision;
};
static void quit(void *host) { du_window_close(&((struct help_main *)host)->window); }
static void read_help(void *arg)
{
    struct help_main *m=arg;
    m->error=help_prepare_document(m->working,&m->document,&m->page,m->path);
    if (!m->error || !strncmp(m->working,"hlp:",4) || !strncmp(m->working,"chm:",4)) return;
    const char *ext=strrchr(m->working,'.');
    if (ext && (!strcasecmp(ext,".hlp") || !strcasecmp(ext,".chm"))) return;
    snprintf(m->path,sizeof(m->path),"%s",m->working);
    FILE *f=fopen(m->path,"rb"); m->error=f ? 0 : -errno;
    if (f) {
        m->text=malloc(HELP_FILE_MAX+2);
        if (!m->text) m->error=-ENOMEM;
        else { m->length=fread(m->text,1,HELP_FILE_MAX+1,f); if (ferror(f)) m->error=-EIO; }
        fclose(f);
    }
}
static void idle(void *arg)
{
    struct help_main *m=arg;
    if (du_job_ready(&m->opening)) {
        du_job_finish(&m->opening);
        int current=m->opening_revision==m->request_revision && m->open_revision==m->app.ta.revision;
        int r=current ? m->error : 0;
        if (!r && current) {
            if (m->document) { r=help_document_adopt(&m->app,m->document,m->page,m->path); if (!r) m->document=NULL; }
            else r=help_open_bytes(&m->app,m->path,m->text,m->length);
            if (!r) help_request_ui(&m->app,m->working);
        }
        if (r) du_message(&m->ui,H_D_INFO,"CiukiOS Help",r==-ENOSYS ? "This help command is not supported." : r==-ENOENT ? "The requested help file or topic was not found." : r==-EINVAL ? "This help file is invalid or damaged." : r==-EFBIG ? "This help file is too large to display." : strerror(-r),DU_DLG_ICON_WARNING,NULL,0,0,0);
        hf_close(m->document); m->document=NULL; free(m->text); m->text=NULL;
    } else if (m->pending[0] && !m->opening.started && m->window.configured && !m->ui.dirty && m->ui.frame_count) {
        memcpy(m->working,m->pending,sizeof(m->working)); m->pending[0]=0;
        m->open_revision=m->app.ta.revision;
        m->opening_revision=m->request_revision;
        int r=du_job_start_ui(&m->opening,read_help,m,&m->ui);
        if (r) du_message(&m->ui,H_D_INFO,"CiukiOS Help",strerror(-r),DU_DLG_ICON_WARNING,NULL,0,0,0);
    }
}
static void request(struct help_main *m,const char *argument)
{
    struct desk_help_request r;
    m->request_revision++;
    if (desk_help_decode(argument,&r) && ((r.format==1 && r.command==2) || (r.format==2 && r.command==18))) { du_window_close(&m->window); return; }
    if (!strncmp(argument,"topic:",6)) {
        int e=help_open_argument(&m->app,argument); m->pending[0]=0;
        if (e) du_message(&m->ui,H_D_INFO,"CiukiOS Help","The requested help topic was not found.",DU_DLG_ICON_WARNING,NULL,0,0,0);
        return;
    }
    int e=help_request_current(&m->app,argument);
    if (e!=-EAGAIN) {
        m->pending[0]=0;
        if (e) du_message(&m->ui,H_D_INFO,"CiukiOS Help",e==-ENOSYS ? "This help command is not supported." : "The requested help topic was not found.",DU_DLG_ICON_WARNING,NULL,0,0,0);
        return;
    }
    snprintf(m->pending,sizeof(m->pending),"%s",argument);
}
static void message(struct du_window *w,const struct desk_packet *p,void *user)
{
    (void)w; struct help_main *m=user;
    if (p->opcode==DESK_LAUNCH && !strcmp(p->title,"help")) request(m,p->argument);
}
int main(int argc, char **argv)
{
    int channel, test; const char *path;
    if (acc_args(argc, argv, "help", &channel, &test, &path)) return 2;
    struct help_main *m = calloc(1, sizeof(*m));
    if (!m) return 1;
    char title[64]; int w, h;
    acc_title(title, sizeof(title), "CiukiOS Help", path && strncmp(path,"topic:",6) && strncmp(path,"hlp:",4) && strncmp(path,"chm:",4) ? path : NULL);
    acc_window_size(HELP_W, HELP_H, &w, &h);
    if (test) { w = HELP_W; h = HELP_H; }
    if (du_window_open_resizable(&m->window, &m->ui, channel, w, h, title, 440, 280)) {
        fprintf(stderr, "help: desktop connection failed: %s\n", strerror(errno)); free(m); return 1;
    }
    if (help_attach(&m->app, &m->ui)) { du_window_destroy(&m->window); free(m); return 1; }
    m->app.quit = quit; m->app.host = m;
    m->window.message=message; m->window.user=m;
    if (path) request(m,path);
    if (test) du_window_move(&m->window, 24, 48);
    int r = du_run(&m->window,idle,m);
    du_job_finish(&m->opening); free(m->text); hf_close(m->document);
    help_detach(&m->app); du_window_destroy(&m->window); free(m);
    return r < 0 ? 1 : 0;
}
