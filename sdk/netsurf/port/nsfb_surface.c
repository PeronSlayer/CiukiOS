/* SPDX-License-Identifier: MIT
 * libnsfb surface glue; no PS/2, display grant, lease, or networking access. */
#include <errno.h>
#include <poll.h>
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
#include "apps/libdeskui/clipboard.h"

static bool clip_waiting;
static uint64_t clip_now(void)
{
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC,&ts))return 0;
    return (uint64_t)ts.tv_sec*1000000000u+ts.tv_nsec;
}

/* G5 v3 clipboard RPCs belong to the connection (window/generation zero).
 * Keep unrelated input for the next UI turn, without recursive callbacks. */
static int clipboard_rpc(void *ctx,struct desk_clip_wire *w,const void *data,
                         struct desk_clip_wire *reply,void **out)
{
    struct ciuki_browser_surface *s=ctx;
    if(clip_waiting)return -EBUSY;
    if(s->closed || s->connection.disconnected)return -EPIPE;
    *out=NULL;
    int fd=-1,error=0,done=0;
    if(w->bytes) {
        uint32_t width,height;
        desk_clip_dims(w->bytes,&width,&height);
        fd=s->ops.create(width,height,CIUKI_SURFACE_XRGB8888);
        if(fd<0)return -errno;
        struct ciuki_surface_info info;
        if(s->ops.transport.info(fd,&info) || !desk_surface_valid(&info) || w->bytes>info.allocation_bytes)error=-EIO;
        else {
            void *map=s->ops.map(fd,PROT_READ|PROT_WRITE);
            if(!map || map==MAP_FAILED)error=-ENOMEM;
            else { memcpy(map,data,w->bytes);s->ops.unmap(map,info.allocation_bytes); }
        }
    }
    struct ciuki_message m;
    if(!error)error=desk_clip_message(&m,DESK_V3_VERSION,1,w,fd);
    uint64_t now=clip_now(),deadline=now+3000000000ull;
    if(!now)error=-EIO;
    clip_waiting=true;
    while(!error && s->ops.transport.send(s->connection.channel,&m,DONTWAIT)) {
        if(errno!=EAGAIN && errno!=EINTR) { error=-errno;break; }
        now=clip_now();if(!now || now>=deadline) { error=-ETIMEDOUT;break; }
        struct pollfd p={s->connection.channel,POLLOUT,0};poll(&p,1,10);
    }
    if(fd>=0)s->ops.transport.close(fd);
    while(!error && !done && !s->closed) {
        now=clip_now();if(!now || now>=deadline) { error=-ETIMEDOUT;break; }
        struct desk_packet p;
        int got=du3_next(&s->connection,&p);
        if(got<0 && (errno==EAGAIN || errno==EINTR)) {
            struct pollfd wait={s->connection.channel,POLLIN,0};poll(&wait,1,10);continue;
        }
        if(got!=1) { ciuki_browser_mark_closed(s,"clipboard-channel",got?errno:0);error=-EIO;break; }
        if(p.opcode==DESK_V3_CLIPBOARD_EVENT) {
            du_clip_observe(p.clip.sequence);
            if(p.clip.kind==DESK_CLIP_REPLY && p.clip.serial==w->serial) {
                *reply=p.clip;done=1;error=reply->result;
                if(reply->op!=w->op)error=-EIO;
                if(!error && w->op==DESK_CLIP_GET && reply->bytes) {
                    struct ciuki_surface_info info;
                    if(p.clipboard_fd<0 || s->ops.transport.info(p.clipboard_fd,&info) ||
                       !desk_surface_valid(&info) || reply->bytes>info.allocation_bytes)error=-EIO;
                    else {
                        void *map=s->ops.map(p.clipboard_fd,PROT_READ);
                        if(!map || map==MAP_FAILED)error=-ENOMEM;
                        else {
                            *out=malloc(reply->bytes);
                            if(!*out)error=-ENOMEM;else memcpy(*out,map,reply->bytes);
                            s->ops.unmap(map,info.allocation_bytes);
                        }
                    }
                }
            }
            if(p.clipboard_fd>=0)s->ops.transport.close(p.clipboard_fd);
        } else if(p.window_id==s->window && p.generation==s->generation) {
            if(p.opcode==DESK_PING) {
                if(du3_ui_progress(&s->connection,s->window,s->generation,p.serial))error=-EIO;
            } else if(!ciuki_browser_hold(s,&p))error=-ENOMEM;
        }
    }
    if(!error && !done)error=-EIO;
    clip_waiting=false;
    return error;
}

struct ciuki_browser_surface *ciuki_browser_active;
int ciuki_browser_channel=-1;
/* The native fetcher supplies its sockets, DNS completion and real expiry. */
int (*ciuki_browser_wait)(int,int);
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
        if(p->opcode==DESK_V3_CLIPBOARD_EVENT) {
            du_clip_observe(p->clip.sequence);
            if(p->clipboard_fd>=0)s->ops.transport.close(p->clipboard_fd);
            continue;
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
    /* fx-11: drain the whole channel; with room, a hold never refuses. */
    while(ciuki_browser_held_room(s) && receive(s,&p))
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
    fb->surface_priv=ciuki_browser_active=s; bind(fb,s);
    du_clip_bind(clipboard_rpc,s);
    ciuki_chrome_init(s);ciuki_ui_pump=pump;return 0;
}
static int finalise(nsfb_t *fb)
{
    struct ciuki_browser_surface *s=fb->surface_priv;
    ciuki_ui_pump=NULL;
    du_clip_bind(NULL,NULL);
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
            ciuki_chrome_dispatch(s,&p.event);
        } else if((p.opcode==DESK_V3_ACTIVATE && !p.focused) || p.opcode==DESK_V3_POINTER_STATE)ciuki_chrome_release();
    }
    s->in_input=false;
    /* fx-11: input dropped by a full hold ring during a long layout may have
     * contained key or button releases: resynchronise like a focus loss. */
    if(s->held_resync && !s->held_count) { s->held_resync=false; ciuki_browser_release(s); }
    if(s->resized) {
        bind(fb,s);s->resized=false;ciuki_chrome_resize();
        event->type=NSFB_EVENT_RESIZE;event->value.resize.w=fb->width;event->value.resize.h=fb->height;return true;
    }
    ciuki_chrome_render();ciuki_browser_present(s);
    /* Only this UI turn can acknowledge liveness. */
    if(s->ping && !du3_ui_progress(&s->connection,s->window,s->generation,s->ping))s->ping=0;
    if(s->closed) { event->type=NSFB_EVENT_CONTROL;event->value.controlcode=NSFB_CONTROL_QUIT;return true; }
    if(ciuki_browser_event(s,event))return true;
    if(timeout) {
        int r;
        if(ciuki_browser_wait)r=ciuki_browser_wait(s->connection.channel,timeout);
        else { struct pollfd fd={s->connection.channel,POLLIN,0};r=poll(&fd,1,timeout); }
        if(r<0 && errno!=EINTR)ciuki_browser_mark_closed(s,"event-wait",errno);
    }
    return false;
}
static int claim(nsfb_t *fb,nsfb_bbox_t *box) { (void)fb;(void)box;return 0; }
static int update(nsfb_t *fb,nsfb_bbox_t *box)
{
    struct ciuki_browser_surface *s=fb->surface_priv;if(!s)return -1;
    nsfb_bbox_t b=box?*box:(nsfb_bbox_t){0,0,fb->width,fb->height};
    b.y0+=CIUKI_BROWSER_TOP;b.y1+=CIUKI_BROWSER_TOP;
    return ciuki_browser_update(s,b);
}
static int cursor(nsfb_t *fb,struct nsfb_cursor_s *value) { (void)fb;(void)value;return 0; }
static const nsfb_surface_rtns_t routines={.defaults=defaults,.initialise=initialise,.finalise=finalise,
    .geometry=geometry,.input=input,.claim=claim,.update=update,.cursor=cursor};
NSFB_SURFACE_DEF(ciuki,NSFB_SURFACE_CIUKI,&routines)
