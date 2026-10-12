/* SPDX-License-Identifier: MIT
 * /Programs/paint [--channel-fd=N] [--test=paint] [BMP/PNG path]. */
#include "paint.h"
#include "../accessories/acc.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct paint_main { struct du_window window; struct du_ui ui; };
static void quit(void *arg) { du_window_close(&((struct paint_main *)arg)->window); }
static void title(void *arg, const char *text) { du_window_set_title(&((struct paint_main *)arg)->window,text); }
static int closing(struct du_window *w, void *arg) { (void)w; (void)arg; return paint_request_close(); }
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
    paint_callbacks(quit,title,m); m->window.close_request=closing;
    if (path) {
        int r=paint_open(path);
        if (r) du_message(&m->ui,999,"CiukPaint",strerror(-r),DU_DLG_ICON_ERROR,NULL,0,0,0);
    }
    if (test) du_window_move(&m->window,24,48);
    fprintf(stderr,"[PAINT] ready\n");
    int r=du_run(&m->window,NULL,NULL);
    paint_detach(); du_window_destroy(&m->window); free(m); return r<0?1:0;
}
