/* SPDX-License-Identifier: MIT
 * /Programs/paint [--channel-fd=N] [--test=paint] [BMP/PNG path]. */
#include "paint.h"
#include "../accessories/acc.h"
#include "../libprint/dialog.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct paint_main { struct du_window window; struct du_ui ui; struct cp_ui printing; void (*event)(struct du_ui *,const struct du_event *,void *); const char *path; };
static void print_request(void *arg,int setup)
{
    struct paint_main *m=arg;
    if (setup) { cp_ui_setup(&m->printing,&m->ui);return; }
    struct paint_state s;paint_snapshot(&s);
    size_t count=(size_t)s.width*s.height;
    uint32_t *pixels=malloc(count*sizeof(*pixels));
    if (!pixels) { du_message(&m->ui,999,"Print","Not enough memory to print.",DU_DLG_ICON_ERROR,NULL,0,0,0);return; }
    for (int y=0;y<s.height;y++) for (int x=0;x<s.width;x++) pixels[(size_t)y*s.width+x]=paint_rgb(paint_pixel(x,y));
    cp_ui_image(&m->printing,&m->ui,paint_path()[0]?acc_basename(paint_path()):"Untitled - CiukPaint",pixels,(unsigned)s.width,(unsigned)s.height);
    free(pixels);
}
static void print_event(struct du_ui *u,const struct du_event *e,void *arg)
{
    struct paint_main *m=arg;
    if (!cp_ui_event(&m->printing,u,e)) m->event(u,e,NULL);
}
static void quit(void *arg) { du_window_close(&((struct paint_main *)arg)->window); }
static void title(void *arg, const char *text) { du_window_set_title(&((struct paint_main *)arg)->window,text); }
/* libdeskui: nonzero closes now. paint_request_close returns 1 when it
 * showed the save prompt (G3: before, the window closed under the prompt
 * and a clean picture never closed until the desktop's deadline). */
static int closing(struct du_window *w, void *arg) { (void)w; (void)arg; return !paint_request_close(); }
static void declined(void *arg) { du_window_close_reply(&((struct paint_main *)arg)->window,0); }
static void idle(void *arg)
{
    struct paint_main *m=arg;
    cp_ui_poll(&m->printing,&m->ui);
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
    du_help_topic(&m->ui, "paint");   /* G15: F1 opens this program's Help topic */
    if (paint_attach(&m->ui)) { du_window_destroy(&m->window); free(m); return 1; }
    paint_callbacks(quit,title,m); paint_decline_callback(declined); m->window.close_request=closing;
    paint_print_callback(print_request);m->event=m->ui.event;m->ui.event=print_event;m->ui.user=m;
    m->path=path;
    du_window_accept_files(&m->window,1);   /* fx-19: DragAcceptFiles */
    if (test) du_window_move(&m->window,24,48);
    fprintf(stderr,"[PAINT] ready\n");
    int r=du_run(&m->window,idle,m);
    cp_ui_destroy(&m->printing);paint_detach(); du_window_destroy(&m->window); free(m); return r<0?1:0;
}
