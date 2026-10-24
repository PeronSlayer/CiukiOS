/* SPDX-License-Identifier: MIT
 * /Programs/paint [--channel-fd=N] [--test=paint] [BMP/PNG path]. */
#include "paint.h"
#include "../accessories/acc.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct paint_main { struct du_window window; struct du_ui ui; const char *path; };
static void quit(void *arg) { du_window_close(&((struct paint_main *)arg)->window); }
static void title(void *arg, const char *text) { du_window_set_title(&((struct paint_main *)arg)->window,text); }
static int wallpaper(void *arg,const char *path) { return acc_set_wallpaper(&((struct paint_main *)arg)->window,path); }
/* libdeskui: nonzero closes now. paint_request_close returns 1 when it
 * showed the save prompt (G3: before, the window closed under the prompt
 * and a clean picture never closed until the desktop's deadline). */
static int closing(struct du_window *w, void *arg) { (void)w; (void)arg; return !paint_request_close(); }
static void declined(void *arg) { du_window_close_reply(&((struct paint_main *)arg)->window,0); }
static void idle(void *arg)
{
    struct paint_main *m=arg;
    if (!m->window.configured || m->ui.dirty || !m->ui.frame_count) return;
    int r=paint_open_poll();
    if (m->path) { r=paint_open_begin(m->path); m->path=NULL; }
    if (r<0) du_message(&m->ui,999,"CiukPaint",strerror(-r),DU_DLG_ICON_ERROR,NULL,0,0,0);
}
int main(int argc, char **argv)
{
    int channel,test; const char *path;
    if (acc_args(argc,argv,"paint",&channel,&test,&path)) return 2;
    struct paint_main *m=calloc(1,sizeof(*m)); if (!m) return 1;
    int width,height; acc_window_size(PAINT_W,PAINT_H,&width,&height);
    if (width<360) width=360; if (height<384) height=384;
    if (test) { width=PAINT_W; height=PAINT_H; }
    if (du_window_open_resizable(&m->window,&m->ui,channel,width,height,"Untitled - CiukPaint",360,384)) {
        fprintf(stderr,"paint: desktop connection failed: %s\n",strerror(errno)); free(m); return 1;
    }
    if (paint_attach(&m->ui)) { du_window_destroy(&m->window); free(m); return 1; }
    paint_callbacks(quit,title,m); paint_wallpaper_callback(wallpaper); paint_decline_callback(declined); m->window.close_request=closing;
    m->path=path;
    du_window_accept_files(&m->window,1);   /* fx-19: DragAcceptFiles */
    if (test) du_window_move(&m->window,24,48);
    fprintf(stderr,"[PAINT] ready\n");
    int r=du_run(&m->window,idle,m);
    paint_detach(); du_window_destroy(&m->window); free(m); return r<0?1:0;
}
