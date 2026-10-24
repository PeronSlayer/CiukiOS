/* SPDX-License-Identifier: MIT
 * /Programs/help: CiukiOS Help and text viewer (desktop client, protocol v2).
 * Usage: help [--channel-fd=N] [--test=help] [topic:<id> | text file]
 * A program's F1 and Help menu start it as "help topic:<program>". */
#include "help.h"
#include "../libdeskui/job.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct help_main { struct du_window window; struct du_ui ui; struct help_app app; struct du_job opening; const char *path; char *text; size_t length; int error; uint64_t open_revision; };
static void quit(void *host) { du_window_close(&((struct help_main *)host)->window); }
static void read_help(void *arg)
{
    struct help_main *m=arg; FILE *f=fopen(m->path,"rb"); m->error=f ? 0 : -errno;
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
        int r=m->error ? m->error : m->open_revision!=m->app.ta.revision ? 0 : help_open_bytes(&m->app,m->path,m->text,m->length);
        if (r) du_message(&m->ui,H_D_INFO,"CiukiOS Help",strerror(-r),DU_DLG_ICON_WARNING,NULL,0,0,0);
        free(m->text); m->text=NULL; m->path=NULL;
    } else if (m->path && !m->opening.started && m->window.configured && !m->ui.dirty && m->ui.frame_count) {
        m->open_revision=m->app.ta.revision;
        int r=du_job_start_ui(&m->opening,read_help,m,&m->ui);
        if (r) { m->path=NULL; du_message(&m->ui,H_D_INFO,"CiukiOS Help",strerror(-r),DU_DLG_ICON_WARNING,NULL,0,0,0); }
    }
}
int main(int argc, char **argv)
{
    int channel, test; const char *path;
    if (acc_args(argc, argv, "help", &channel, &test, &path)) return 2;
    struct help_main *m = calloc(1, sizeof(*m));
    if (!m) return 1;
    char title[64]; int w, h;
    acc_title(title, sizeof(title), "CiukiOS Help", path && strncmp(path, "topic:", 6) ? path : NULL);
    acc_window_size(HELP_W, HELP_H, &w, &h);
    if (test) { w = HELP_W; h = HELP_H; }
    if (du_window_open_resizable(&m->window, &m->ui, channel, w, h, title, 440, 280)) {
        fprintf(stderr, "help: desktop connection failed: %s\n", strerror(errno)); free(m); return 1;
    }
    if (help_attach(&m->app, &m->ui)) { du_window_destroy(&m->window); free(m); return 1; }
    m->app.quit = quit; m->app.host = m;
    if (path && !strncmp(path, "topic:", 6)) { (void)help_open_argument(&m->app, path); path = NULL; }
    m->path=path;
    if (test) du_window_move(&m->window, 24, 48);
    int r = du_run(&m->window,idle,m);
    du_job_finish(&m->opening); free(m->text);
    help_detach(&m->app); du_window_destroy(&m->window); free(m);
    return r < 0 ? 1 : 0;
}
