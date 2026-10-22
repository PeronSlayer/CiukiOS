/* SPDX-License-Identifier: MIT
 * libnsfb surface glue; no PS/2, display grant, lease, or networking access. */
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <ciuki/channel.h>
#include <ciuki/surface.h>
#include "libnsfb.h"
#include "libnsfb_plot.h"
#include "libnsfb_event.h"
#include "nsfb.h"
#include "surface.h"
#include "plot.h"
#include "ciuki_port/surface.h"

struct ciuki_browser_surface *ciuki_browser_active;
int ciuki_browser_channel=-1;
static int defaults(nsfb_t *fb)
{ fb->width=800;fb->height=542;fb->format=NSFB_FMT_XRGB8888;select_plotters(fb);return 0; }
static void bind(nsfb_t *fb,struct ciuki_browser_surface *s)
{
    fb->width=s->current.info.width;
    fb->height=s->current.info.height-CIUKI_BROWSER_TOP-CIUKI_BROWSER_STATUS;
    fb->ptr=(uint8_t *)s->current.back+CIUKI_BROWSER_TOP*s->current.info.stride;
    fb->linelen=s->current.info.stride; fb->format=NSFB_FMT_XRGB8888;
    select_plotters(fb); fb->clip=(nsfb_bbox_t){0,0,fb->width,fb->height};
}
static int initialise(nsfb_t *fb)
{
    if(ciuki_browser_active)return -1;
    /* f6-19: a launch without --channel-fd (Programs menu, desktop icon, Run,
     * ShellExecute before the program record granted it) still owns the
     * desktop channel the launcher mapped at fd 3; a non-channel fd fails HELLO. */
    int channel=ciuki_browser_channel>=0 ? ciuki_browser_channel : CIUKI_BROWSER_CHANNEL_FD;
    struct ciuki_browser_surface *s=calloc(1,sizeof(*s));
    if(!s)return -1;
    const struct ciuki_browser_ops ops={
        .transport={ciuki_channel_send,ciuki_channel_recv,ciuki_surface_info,close},
        .create=ciuki_surface_create,.map=ciuki_surface_map,.unmap=munmap};
    if(ciuki_browser_open(s,&ops,channel,fb->width,fb->height+CIUKI_BROWSER_TOP+CIUKI_BROWSER_STATUS)) { free(s);return -1; }
    fb->surface_priv=ciuki_browser_active=s; bind(fb,s); ciuki_chrome_init(s);return 0;
}
static int finalise(nsfb_t *fb)
{
    struct ciuki_browser_surface *s=fb->surface_priv;
    if(s) { ciuki_chrome_release();ciuki_browser_close(s);free(s); }
    ciuki_browser_active=NULL;fb->ptr=NULL;fb->surface_priv=NULL;return 0;
}
static int geometry(nsfb_t *fb,int w,int h,enum nsfb_format_e format)
{
    (void)format;
    if(fb->surface_priv) { bind(fb,fb->surface_priv);return 0; }
    if(w>0)fb->width=w; if(h>0)fb->height=h;
    if(fb->width<300 || fb->width>1024 || fb->height<200 || fb->height>710)return -1;
    fb->format=NSFB_FMT_XRGB8888;select_plotters(fb);fb->linelen=fb->width*4;return 0;
}
static bool input(nsfb_t *fb,nsfb_event_t *event,int timeout)
{
    struct ciuki_browser_surface *s=fb->surface_priv;
    if(!s)return false;
    if(ciuki_browser_event(s,event))return true;
    for(unsigned turns=0;turns<64;turns++) {
        struct desk_packet p;
        int r=du3_next(&s->connection,&p);
        if(r<=0) { if(r==0 || (errno!=EAGAIN && errno!=EINTR))s->closed=true;break; }
        if(p.window_id!=s->window || p.generation!=s->generation)continue;
        ciuki_browser_packet(s,&p);
        if(p.opcode==DESK_KEY || p.opcode==DESK_MOTION || p.opcode==DESK_BUTTON) {
            bool content_drag=s->buttons && (p.event.type==CIUKI_INPUT_MOTION || p.event.type==CIUKI_INPUT_BUTTON);
            if(content_drag || !ciuki_chrome_input(&p.event))ciuki_browser_input(s,&p.event);
        } else if((p.opcode==DESK_V3_ACTIVATE && !p.focused) || p.opcode==DESK_V3_POINTER_STATE)ciuki_chrome_release();
        if(s->count)break;
    }
    if(s->resized) {
        bind(fb,s);s->resized=false;ciuki_chrome_resize();
        event->type=NSFB_EVENT_RESIZE;event->value.resize.w=fb->width;event->value.resize.h=fb->height;return true;
    }
    ciuki_chrome_render();ciuki_browser_present(s);
    /* Only this UI turn can acknowledge liveness. */
    if(s->ping && !du3_ui_progress(&s->connection,s->window,s->generation,s->ping))s->ping=0;
    if(s->closed) { event->type=NSFB_EVENT_CONTROL;event->value.controlcode=NSFB_CONTROL_QUIT;return true; }
    if(ciuki_browser_event(s,event))return true;
    if(timeout) { struct timespec delay={0,(timeout<0 || timeout>10?10:timeout)*1000000L};nanosleep(&delay,NULL); }
    return false;
}
static int claim(nsfb_t *fb,nsfb_bbox_t *box) { (void)fb;(void)box;return 0; }
static int update(nsfb_t *fb,nsfb_bbox_t *box)
{
    struct ciuki_browser_surface *s=fb->surface_priv;if(!s)return -1;
    nsfb_bbox_t b=box?*box:(nsfb_bbox_t){0,0,fb->width,fb->height};
    b.y0+=CIUKI_BROWSER_TOP;b.y1+=CIUKI_BROWSER_TOP;
    ciuki_browser_damage(s,b);return ciuki_browser_present(s);
}
static int cursor(nsfb_t *fb,struct nsfb_cursor_s *value) { (void)fb;(void)value;return 0; }
static const nsfb_surface_rtns_t routines={.defaults=defaults,.initialise=initialise,.finalise=finalise,
    .geometry=geometry,.input=input,.claim=claim,.update=update,.cursor=cursor};
NSFB_SURFACE_DEF(ciuki,NSFB_SURFACE_CIUKI,&routines)
