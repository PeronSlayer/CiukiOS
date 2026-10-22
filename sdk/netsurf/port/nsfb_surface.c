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
#include "ciuki_port/budget.h"

struct ciuki_browser_surface *ciuki_browser_active;
int ciuki_browser_channel=-1;
/* The next packet for this window from the desktop channel; channel loss is
 * recorded as the close reason. EAGAIN/EINTR: nothing to read now. */
static bool receive(struct ciuki_browser_surface *s,struct desk_packet *p)
{
    for(unsigned foreign=0;foreign<CIUKI_BROWSER_DRAIN;foreign++) {
        int r=du3_next(&s->connection,p);
        if(r<=0) {
            if(r==0)ciuki_browser_mark_closed(s,"channel-eof",0);
            else if(errno!=EAGAIN && errno!=EINTR)ciuki_browser_mark_closed(s,"channel-error",errno);
            return false;
        }
        if(p->window_id==s->window && p->generation==s->generation)return true;
    }
    return false;
}
/* f6-20 keepalive: called (rate-limited) from long layouts and paints on the
 * UI thread. Read the channel so the desktop's queue for this client never
 * fills, but process nothing: PONG stays a UI-turn acknowledgement. */
static void pump(void)
{
    struct ciuki_browser_surface *s=ciuki_browser_active;
    if(!s || s->in_input || s->closed)return;
    struct desk_packet p;
    s->pumps++;
    while(s->held_count<CIUKI_BROWSER_HELD && receive(s,&p))
        if(!ciuki_browser_hold(s,&p))break;
}
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
    fb->surface_priv=ciuki_browser_active=s; bind(fb,s); ciuki_chrome_init(s);ciuki_ui_pump=pump;return 0;
}
static int finalise(nsfb_t *fb)
{
    struct ciuki_browser_surface *s=fb->surface_priv;
    ciuki_ui_pump=NULL;
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
    /* f6-20: drain the channel, held packets first, instead of one input
     * packet per UI turn: a slow turn (the T23) with the pointer moving over
     * the window otherwise fell behind until the desktop's 64-message queue
     * for this v3 client filled and the desktop dropped the client (the
     * browser then quit on channel EOF). Motion is coalesced in the queue. */
    s->in_input=true;
    for(unsigned turns=0;turns<CIUKI_BROWSER_DRAIN;turns++) {
        struct desk_packet p;
        if(!ciuki_browser_unhold(s,&p) && !receive(s,&p))break;
        ciuki_browser_packet(s,&p);
        if(p.opcode==DESK_KEY || p.opcode==DESK_MOTION || p.opcode==DESK_BUTTON) {
            bool content_drag=s->buttons && (p.event.type==CIUKI_INPUT_MOTION || p.event.type==CIUKI_INPUT_BUTTON);
            if(content_drag || !ciuki_chrome_input(&p.event))ciuki_browser_input(s,&p.event);
        } else if((p.opcode==DESK_V3_ACTIVATE && !p.focused) || p.opcode==DESK_V3_POINTER_STATE)ciuki_chrome_release();
    }
    s->in_input=false;
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
