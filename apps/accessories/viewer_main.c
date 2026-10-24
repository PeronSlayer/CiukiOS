/* SPDX-License-Identifier: MIT
 * /Programs/viewer: the CiukiOS Image Viewer (desktop client, protocol v2).
 * Usage: viewer [--channel-fd=N] [--test=viewer] [path] */
#include "viewer.h"
#include "../libprint/dialog.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct viewer_main { struct du_window window; struct du_ui ui; struct viewer_app app; struct cp_ui printing; const char *path; };
static void print_request(void *arg,int setup)
{
    struct viewer_main *m=arg;
    if (setup) cp_ui_setup(&m->printing,&m->ui);
    else cp_ui_image(&m->printing,&m->ui,acc_basename(m->app.path),m->app.image.pixels,(unsigned)m->app.image.width,(unsigned)m->app.image.height);
}
static void print_event(struct du_ui *u,const struct du_event *e,void *arg)
{
    struct viewer_app *a=arg;struct viewer_main *m=a->host;
    if (!cp_ui_event(&m->printing,u,e)) viewer_event(u,e,a);
}
static void quit(void *host) { du_window_close(&((struct viewer_main *)host)->window); }
static void idle(void *arg)
{
    struct viewer_main *m=arg;
    cp_ui_poll(&m->printing,&m->ui);
    viewer_poll(&m->app);
    if (m->path && m->window.configured && !m->ui.dirty && m->ui.frame_count) {
        viewer_open(&m->app,m->path); m->path=NULL;
    }
}
int main(int argc, char **argv)
{
    int channel, test; const char *path;
    if (acc_args(argc, argv, "viewer", &channel, &test, &path)) return 2;
    struct viewer_main *m = calloc(1, sizeof(*m));
    if (!m) return 1;
    char title[64]; int w, h;
    acc_title(title, sizeof(title), "Image Viewer", path);
    acc_window_size(VIEWER_W, VIEWER_H, &w, &h);
    if (test) { w = VIEWER_W; h = VIEWER_H; }
    if (du_window_open_resizable(&m->window, &m->ui, channel, w, h, title, 320, 240)) {
        fprintf(stderr, "viewer: desktop connection failed: %s\n", strerror(errno)); free(m); return 1;
    }
    du_help_topic(&m->ui, "viewer");   /* G15: F1 opens this program's Help topic */
    viewer_attach(&m->app, &m->ui, viewer_native_fs());
    m->app.quit = quit; m->app.host = m;
    m->app.printing=print_request;m->ui.event=print_event;
    m->app.asynchronous=1; m->path=path;
    du_window_accept_files(&m->window, 1);   /* fx-19: DragAcceptFiles */
    if (test) du_window_move(&m->window, 24, 48);
    int r = du_run(&m->window, idle, m);
    cp_ui_destroy(&m->printing);viewer_detach(&m->app); du_window_destroy(&m->window); free(m);
    return r < 0 ? 1 : 0;
}
