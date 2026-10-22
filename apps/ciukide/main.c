/* SPDX-License-Identifier: MIT */
#include "app.h"
#include "../libdeskui/job.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
struct ide_main { struct ide_app app; struct du_ui ui; struct du_window window; uint32_t serial; int closing, pending; struct du_job opening; const char *folder; struct ide_app *loaded; struct du_ui *load_ui; int load_error; uint64_t open_revision; };
static int launch(void *ctx,const char *path)
{
    struct ide_main *m=ctx;
    if (m->pending) return -EBUSY;
    const char *name="ciukide-run";
    if (!strcmp(path,"/Documents/Developer/Guide.txt")) name="ciuknote";
    size_t n=strlen(name),len=strlen(path);
    if (!len || len>DESK_ARG_MAX) return -ENAMETOOLONG;
    struct ciuki_message msg; desk_message(&msg,DESK_LAUNCH,(unsigned)(16+n+len));
    if (!++m->serial) ++m->serial;
    desk_put32(msg.data+4,m->serial); desk_put32(msg.data+8,(uint32_t)n); memcpy(msg.data+12,name,n);
    desk_put32(msg.data+12+n,(uint32_t)len); memcpy(msg.data+16+n,path,len);
    if (du_window_queue(&m->window,&msg)) return -errno;
    m->pending=1; return 0;
}
static void message(struct du_window *w,const struct desk_packet *p,void *ctx)
{
    (void)w; struct ide_main *m=ctx;
    if (p->opcode==DESK_TASK_RESULT && p->request==DESK_LAUNCH && p->serial==m->serial) {
        m->pending=0;
        ide_run_result(&m->app.model,p->result);
        if (p->result<0) { char text[128]; snprintf(text,sizeof(text),"Launch failed: %s",strerror(-p->result));
            du_message(&m->ui,IDE_MESSAGE,"CiukIDE",text,DU_DLG_ICON_ERROR,NULL,0,0,0); }
        ide_refresh(&m->app);
    }
}
static int close_request(struct du_window *w,void *ctx)
{
    (void)w; struct ide_main *m=ctx;
    int r=ide_save(&m->app);
    if (r) { char text[128]; snprintf(text,sizeof(text),"Save failed: %s",strerror(-r));
        du_message(&m->ui,IDE_MESSAGE,"CiukIDE",text,DU_DLG_ICON_ERROR,NULL,0,0,0); return 0; }
    if (m->app.job.pid) { kill(m->app.job.pid,SIGTERM); m->closing=1; return 0; }
    return 1;
}
static void read_project(void *arg)
{
    struct ide_main *m=arg;
    m->loaded=calloc(1,sizeof(*m->loaded)); m->load_ui=calloc(1,sizeof(*m->load_ui));
    if (!m->loaded || !m->load_ui) { m->load_error=-ENOMEM; return; }
    du_init(m->load_ui,NULL,560,330); m->loaded->fs=cn_native_fs();
    m->load_error=ide_attach(m->loaded,m->load_ui);
    if (!m->load_error) m->load_error=ide_open_project(m->loaded,m->folder);
}
static void free_project(struct ide_main *m)
{
    if (m->loaded) { ide_detach(m->loaded); free(m->loaded); m->loaded=NULL; }
    free(m->load_ui); m->load_ui=NULL;
}
static void idle(void *ctx)
{
    struct ide_main *m=ctx; ide_idle(&m->app);
    if (du_job_ready(&m->opening)) {
        du_job_finish(&m->opening);
        if (!m->load_error && m->open_revision==m->app.editor.revision && !m->app.editor.modified) ide_adopt_project(&m->app,m->loaded);
        else du_message(&m->ui,IDE_MESSAGE,"CiukIDE",m->load_error ? strerror(-m->load_error) : "Project loaded; save the current edits before opening it again.",DU_DLG_ICON_ERROR,NULL,0,0,0);
        free_project(m); m->folder=NULL;
    } else if (m->folder && !m->opening.started && m->window.configured && !m->ui.dirty && m->ui.frame_count) {
        m->open_revision=m->app.editor.revision;
        int r=du_job_start(&m->opening,read_project,m);
        if (r) { m->folder=NULL; du_message(&m->ui,IDE_MESSAGE,"CiukIDE",strerror(-r),DU_DLG_ICON_ERROR,NULL,0,0,0); }
    }
    if (m->closing && !m->app.job.pid) du_window_close(&m->window);
}
int main(int argc,char **argv)
{
    int channel=3; const char *folder=NULL;
    for (int i=1;i<argc;++i) {
        if (!strncmp(argv[i],"--channel-fd=",13)) { char *end; long n=strtol(argv[i]+13,&end,10);
            if (!argv[i][13] || *end || n<3 || n>=128) return 2; channel=(int)n; }
        else if (argv[i][0]=='-' || folder) return 2;
        else folder=argv[i];
    }
    struct ide_main *m=calloc(1,sizeof(*m)); if (!m) return 1;
    int width=560,height=330; const char *display=getenv("CIUKI_DISPLAY");
    if (display && atoi(display)>=800) { width=760; height=500; }
    if (du_window_open_resizable(&m->window,&m->ui,channel,width,height,"CiukIDE",500,300)) { free(m); return 1; }
    m->app.fs=cn_native_fs(); m->app.process=ide_native_process(); m->app.launch=launch; m->app.launch_ctx=m;
    m->window.user=m; m->window.message=message; m->window.close_request=close_request;
    if (ide_attach(&m->app,&m->ui)) { du_window_destroy(&m->window); free(m); return 1; }
    m->folder=folder;
    int r=du_run(&m->window,idle,m);
    if (m->app.job.pid) {
        kill(m->app.job.pid,SIGKILL);
        for (unsigned i=0;i<2;++i) if (m->app.job.fd[i]>=0) close(m->app.job.fd[i]);
        int status; while (waitpid(m->app.job.pid,&status,0)<0 && errno==EINTR) {}
    }
    du_job_finish(&m->opening); free_project(m);
    ide_detach(&m->app); du_window_destroy(&m->window); free(m); return r<0 ? 1 : 0;
}
