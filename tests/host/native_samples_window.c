/* SPDX-License-Identifier: MIT
 * Window boundary for the host-ABI sample. Exercise the sample's actual
 * callback; transport/surfaces are separately covered by libdeskui harnesses. */
#include <deskui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int du_window_open(struct du_window *w,struct du_ui *u,int channel,int width,int height,const char *title)
{
    if (channel!=3 || width!=360 || height!=150 || strcmp(title,"Hello Ciuki")) return -1;
    w->ui=u; u->count=0; return 0;
}
struct du_widget *du_add(struct du_ui *u,enum du_kind kind,int id,struct du_rect box,const char *text)
{
    struct du_widget *w=&u->widgets[u->count++]; w->kind=kind; w->id=id; w->box=box;
    snprintf(w->text,sizeof(w->text),"%s",text); return w;
}
struct du_widget *du_get(struct du_ui *u,int id)
{ for (unsigned i=0;i<u->count;++i) if (u->widgets[i].id==id) return &u->widgets[i]; return NULL; }
void du_set_text(struct du_widget *w,const char *s) { snprintf(w->text,sizeof(w->text),"%s",s); }
int du_run(struct du_window *w,void (*idle)(void *),void *ctx)
{
    (void)idle; (void)ctx; struct du_ui *u=w->ui;
    if (!u->event || u->count!=2 || strcmp(du_get(u,1)->text,"Hello from CiukiOS!")) return -1;
    struct du_event event={.type=DU_EV_ACTIVATE,.id=2}; u->event(u,&event,u->user);
    if (!u->dirty || strcmp(du_get(u,1)->text,"Classic computing, one woof at a time.")) return -1;
    puts("Hello window sample callback: PASS"); return 0;
}
void du_window_destroy(struct du_window *w) { (void)w; }
