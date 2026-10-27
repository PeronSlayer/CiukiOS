#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
/* SPDX-License-Identifier: MIT */
#include "clipboard.h"
#include "../libdeskui/clipstore.h"
#include <ciuki/surface.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
static pthread_once_t lock_once=PTHREAD_ONCE_INIT;
static pthread_mutex_t lock;
static int lock_error;
static void init_lock(void)
{
    pthread_mutexattr_t attr;
    lock_error=pthread_mutexattr_init(&attr); if (lock_error) return;
    lock_error=pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE);
    if (!lock_error) lock_error=pthread_mutex_init(&lock,&attr);
    pthread_mutexattr_destroy(&attr);
}
static int enter(void)
{ int e=pthread_once(&lock_once,init_lock); return e ? e : lock_error ? lock_error : pthread_mutex_lock(&lock); }
struct clip_pending {
    struct desk_clip_wire reply, get;
    void *data, *display_data;
    struct desk_clip_wire display;
    unsigned display_owner;
    uint32_t display_token, display_serial;
    uint64_t deadline;
    int ready, watch;
    unsigned events, render, get_render;
    uint32_t destroy_token;
};
struct desk_clipboard { struct clip_store store; struct clip_pending clients[DESK_CLIENTS]; uint32_t display_serial; };
static struct desk_clipboard *state(struct desktop *d)
{
    if (!d->clipboard) { d->clipboard=calloc(1,sizeof(*d->clipboard)); if (d->clipboard) clip_init(&d->clipboard->store); }
    return d->clipboard;
}
static void notify(struct desk_clipboard *s)
{
    if (!s->store.dirty) return;
    for (int i=0;i<DESK_CLIENTS;i++) if (s->clients[i].watch) s->clients[i].events|=1u<<DESK_CLIP_CHANGED;
    s->store.dirty=0;
}
static int request(struct desktop *d, uint32_t id, struct desk_clip_wire *w, const void *data, struct desk_clip_wire *r, void **out)
{
    struct desk_clipboard *s=state(d); if (!s) return -ENOMEM;
    unsigned old=s->store.owner; uint32_t token=s->store.token;
    int e=clip_request(&s->store,id,w,data,r,out);
    if (!e && w->op==DESK_CLIP_EMPTY && old && old<=DESK_CLIENTS) {
        struct clip_pending *p=&s->clients[old-1]; p->events|=1u<<DESK_CLIP_DESTROY; p->destroy_token=token;
    }
    if ((w->op==DESK_CLIP_CLOSE && !s->store.opener) || (!e && (w->flags&DESK_CLIP_RENDERED)) || w->op==DESK_CLIP_RELEASE) notify(s);
    return e;
}
int desk_clip_local(void *ctx, struct desk_clip_wire *w, const void *data, struct desk_clip_wire *r, void **out)
{
    int e=enter(); if (e) return -e;
    struct desktop *d=ctx;
    if (w->op==DESK_CLIP_PUT && (w->format==1 || w->format==7 || w->format==13)) {
        struct desk_clipboard *s=state(d);
        if (s) s->store.locale=d->sh.cfg.keyboard==CFG_KEYS_IT ? 0x410 : 0x409;
    }
    e=request(d,DESK_CLIENTS+1,w,data,r,out); pthread_mutex_unlock(&lock); return e;
}
static void clip_receive_locked(struct desktop *d, int i, const struct ciuki_message *m, const struct desk_packet *p)
{
    struct desk_clipboard *s=state(d);
    if (!s) { for (unsigned k=0;k<m->fd_count;k++) d->ops.close(m->fds[k]); desk_drop_client(d,i); return; }
    s->store.locale=d->sh.cfg.keyboard==CFG_KEYS_IT ? 0x410 : 0x409;
    struct clip_pending *pending=&s->clients[i];
    int render_call=pending->deadline && pending->get.op==DESK_CLIP_GET && s->store.owner==(unsigned)i+1 &&
        ((p->clip.op==DESK_CLIP_PUT && (p->clip.flags&DESK_CLIP_RENDERED)) || p->clip.op==DESK_CLIP_REGISTER ||
         p->clip.op==DESK_CLIP_NAME || p->clip.op==DESK_CLIP_QUERY);
    if ((pending->ready || pending->deadline) && !render_call) { for (unsigned k=0;k<m->fd_count;k++) d->ops.close(m->fds[k]); desk_drop_client(d,i); return; }
    void *data=NULL; int error=0;
    if (m->fd_count) {
        struct ciuki_surface_info info={0};
        if (d->ops.info(m->fds[0],&info) || !desk_surface_valid(&info) || p->clip.bytes>info.allocation_bytes) error=-EINVAL;
        else {
            void *map=d->ops.map(m->fds[0],PROT_READ);
            if (!map || map==MAP_FAILED) error=-ENOMEM;
            else { data=malloc(p->clip.bytes); if (!data) error=-ENOMEM; else memcpy(data,map,p->clip.bytes); d->ops.unmap(map,info.allocation_bytes); }
        }
        d->ops.close(m->fds[0]);
    }
    struct desk_clip_wire w=p->clip;
    if (render_call) {
        struct desk_clip_wire r={.serial=w.serial,.op=w.op,.kind=DESK_CLIP_REPLY,.result=error}; void *out=NULL;
        if (!error) (void)request(d,i+1,&w,data,&r,&out);
        struct ciuki_message reply;
        if (!desk_clip_message(&reply,d->clients[i].version,0,&r,-1)) desk_queue(d,i,&reply);
        free(out); free(data); return;
    }
    if (!error && w.op==DESK_CLIP_DISPLAY) {
        int available=0;
        for (unsigned k=0;k<s->store.count;k++) if (s->store.items[k].format==0x80) available=1;
        unsigned owner=s->store.owner;
        if (!available || !owner || owner>DESK_CLIENTS || owner==(unsigned)i+1 || (w.token && w.token!=s->store.token)) error=-ENOENT;
        else if (s->clients[owner-1].display.serial) error=-EBUSY;
        else {
            struct clip_pending *to=&s->clients[owner-1];
            if (!++s->display_serial) ++s->display_serial;
            pending->display_serial=s->display_serial;
            to->display=(struct desk_clip_wire){.kind=DESK_CLIP_DISPLAY_EVENT,.op=w.op,.serial=s->display_serial,
                .token=(unsigned)i+1,.format=w.format,.bytes=w.bytes,.sequence=s->store.sequence};
            to->display_data=data;
            pending->get=w; pending->deadline=d->now_ns+DESK_CLIP_RENDER_NS;
            pending->display_owner=owner; pending->display_token=s->store.token;
            return;
        }
    }
    if (!error && w.op==DESK_CLIP_DISPLAY_REPLY) {
        if (w.token>DESK_CLIENTS) error=-EINVAL;
        else {
            struct clip_pending *to=&s->clients[w.token-1];
            if (!to->deadline || to->get.op!=DESK_CLIP_DISPLAY || to->display_serial!=w.flags ||
                to->display_owner!=(unsigned)i+1 || s->store.owner!=(unsigned)i+1 ||
                to->display_token!=s->store.token || to->get.format!=w.format) error=-ESRCH;
            else {
                to->reply=(struct desk_clip_wire){.kind=DESK_CLIP_REPLY,.op=DESK_CLIP_DISPLAY,
                    .serial=to->get.serial,.format=w.format,.bytes=w.bytes,.sequence=s->store.sequence};
                to->data=data; data=NULL; to->deadline=0; to->ready=1;
            }
        }
        pending->reply=(struct desk_clip_wire){.kind=DESK_CLIP_REPLY,.op=w.op,.serial=w.serial,.result=error};
        pending->ready=1; free(data); return;
    }
    if (!error) error=request(d,i+1,&w,data,&pending->reply,&pending->data);
    else pending->reply=(struct desk_clip_wire){.serial=w.serial,.kind=DESK_CLIP_REPLY,.op=w.op,.result=error,.sequence=s->store.sequence};
    free(data);
    if (error==-EAGAIN && w.op==DESK_CLIP_GET && s->store.owner && s->store.owner<=DESK_CLIENTS) {
        pending->get=w; pending->deadline=d->now_ns+DESK_CLIP_RENDER_NS;
        struct clip_pending *owner=&s->clients[s->store.owner-1]; owner->render=pending->get_render=pending->reply.format;
    } else pending->ready=1;
    if (!error && w.op==DESK_CLIP_WATCH) { pending->watch=w.flags; pending->reply.bytes=!!w.flags; }
}
static int send_event(struct desktop *d, int i, struct desk_clip_wire *w, const void *data)
{
    struct desk_client *c=&d->clients[i];
    if (c->tx_count || c->v3_over_count) return 0;
    struct ciuki_message m; int fd=-1;
    if (w->bytes && (w->op==DESK_CLIP_GET || w->op==DESK_CLIP_DISPLAY || w->kind==DESK_CLIP_DISPLAY_EVENT) && !w->result) {
        uint32_t width,height; desk_clip_dims(w->bytes,&width,&height);
        fd=ciuki_surface_create(width,height,CIUKI_SURFACE_XRGB8888);
        if (fd<0) return 0;
        struct ciuki_surface_info info;
        if (d->ops.info(fd,&info)) { d->ops.close(fd); return 0; }
        void *map=d->ops.map(fd,PROT_READ|PROT_WRITE);
        if (!map || map==MAP_FAILED) { d->ops.close(fd); return 0; }
        memcpy(map,data,w->bytes); d->ops.unmap(map,info.allocation_bytes);
    }
    if (desk_clip_message(&m,c->version,0,w,fd)) { if (fd>=0) d->ops.close(fd); return 0; }
    if (c->version==DESK_SHELL_VERSION) m.data[2]=c->minor;
    int result=d->ops.send(c->channel,&m,DONTWAIT);
    if (fd>=0) d->ops.close(fd);
    if (result<0 && errno!=EAGAIN && errno!=EINTR) desk_drop_client(d,i);
    return result>=0;
}
static void clip_tick_locked(struct desktop *d)
{
    struct desk_clipboard *s=d->clipboard; if (!s) return;
    for (int i=0;i<DESK_CLIENTS;i++) {
        struct clip_pending *p=&s->clients[i];
        if (d->clients[i].channel<0 || d->clients[i].v3_peer) continue;
        if (p->deadline && p->get.op==DESK_CLIP_DISPLAY) {
            if (d->now_ns>=p->deadline || s->store.owner!=p->display_owner || s->store.token!=p->display_token) {
                p->reply=(struct desk_clip_wire){.kind=DESK_CLIP_REPLY,.op=DESK_CLIP_DISPLAY,.serial=p->get.serial,.result=-ETIMEDOUT};
                p->deadline=0; p->ready=1;
            }
        }
        if (p->deadline && p->get.op!=DESK_CLIP_DISPLAY) {
            int e=clip_request(&s->store,i+1,&p->get,NULL,&p->reply,&p->data);
            if (e==-EAGAIN && p->reply.format!=p->get_render && s->store.owner && s->store.owner<=DESK_CLIENTS) {
                p->get_render=p->reply.format; s->clients[s->store.owner-1].render=p->reply.format;
            }
            if (e!=-EAGAIN || d->now_ns>=p->deadline) { if (e==-EAGAIN) p->reply.result=-ETIMEDOUT; p->deadline=0; p->ready=1; }
        }
        if (p->events&(1u<<DESK_CLIP_DESTROY)) {
            struct desk_clip_wire event={.kind=DESK_CLIP_DESTROY,.token=p->destroy_token,.sequence=s->store.sequence};
            if (send_event(d,i,&event,NULL)) p->events&=~(1u<<DESK_CLIP_DESTROY);
            else continue;
        }
        if (p->ready && send_event(d,i,&p->reply,p->data)) { free(p->data); p->data=NULL; p->ready=0; }
        if (p->display.serial && send_event(d,i,&p->display,p->display_data)) {
            free(p->display_data); p->display_data=NULL; p->display=(struct desk_clip_wire){0};
        }
        if (p->render) {
            struct desk_clip_wire event={.kind=DESK_CLIP_RENDER,.format=p->render,.token=s->store.token,.sequence=s->store.sequence};
            if (send_event(d,i,&event,NULL)) p->render=0;
        }
        for (unsigned kind=DESK_CLIP_CHANGED;kind<=DESK_CLIP_CHANGED;kind++) if (p->events&(1u<<kind)) {
            struct desk_clip_wire event={.kind=kind,.token=s->store.token,.sequence=s->store.sequence};
            if (send_event(d,i,&event,NULL)) p->events&=~(1u<<kind);
        }
    }
}
static void clip_drop_locked(struct desktop *d, int i)
{
    if (!d->clipboard || d->clients[i].v3_peer) return;
    free(d->clipboard->clients[i].data); free(d->clipboard->clients[i].display_data); memset(&d->clipboard->clients[i],0,sizeof(d->clipboard->clients[i]));
    clip_disconnect(&d->clipboard->store,i+1); notify(d->clipboard);
}
static void clip_destroy_locked(struct desktop *d)
{
    if (!d->clipboard) return;
    for (int i=0;i<DESK_CLIENTS;i++) { free(d->clipboard->clients[i].data); free(d->clipboard->clients[i].display_data); }
    clip_destroy(&d->clipboard->store); free(d->clipboard); d->clipboard=NULL;
}
static int clip_capture_locked(struct desktop *d, int active)
{
    struct desk_box box={0,0,d->width,d->height};
    if (active) { if (d->focus<0) return -ENOENT; box=desk_outer(&d->clients[d->focus]); }
    if (box.x<0) { box.w+=box.x; box.x=0; } if (box.y<0) { box.h+=box.y; box.y=0; }
    if (box.x+box.w>(int)d->width) box.w=d->width-box.x;
    if (box.y+box.h>(int)d->height) box.h=d->height-box.y;
    if (box.w<=0 || box.h<=0 || !d->pixels) return -EINVAL;
    void *data=NULL; uint32_t n;
    int e=clip_dib_encode(d->pixels+(size_t)box.y*d->width+box.x,d->width,box.w,box.h,&data,&n); if (e) return e;
    struct desk_clip_wire w={.serial=1,.op=DESK_CLIP_OPEN},r; void *out=NULL;
    e=desk_clip_local(d,&w,NULL,&r,&out);
    if (!e) {
        w.op=DESK_CLIP_EMPTY; e=desk_clip_local(d,&w,NULL,&r,&out);
        if (!e) { w.op=DESK_CLIP_PUT; w.format=DESK_CF_DIB; w.bytes=n; e=desk_clip_local(d,&w,data,&r,&out); }
        w=(struct desk_clip_wire){.serial=1,.op=DESK_CLIP_CLOSE}; desk_clip_local(d,&w,NULL,&r,&out);
    }
    free(data); return e;
}

void desk_clip_receive(struct desktop *d,int i,const struct ciuki_message *m,const struct desk_packet *p)
{ if (enter()) { for (unsigned k=0;k<m->fd_count;k++) d->ops.close(m->fds[k]); return; } clip_receive_locked(d,i,m,p); pthread_mutex_unlock(&lock); }
void desk_clip_tick(struct desktop *d)
{ if (!enter()) { clip_tick_locked(d); pthread_mutex_unlock(&lock); } }
void desk_clip_drop(struct desktop *d,int i)
{ if (!enter()) { clip_drop_locked(d,i); pthread_mutex_unlock(&lock); } }
void desk_clip_destroy(struct desktop *d)
{ if (!enter()) { clip_destroy_locked(d); pthread_mutex_unlock(&lock); } }
int desk_clip_capture(struct desktop *d,int active)
{
    /* fx-doswin3 round 3: a DOS window presented straight from its surface
     * left older pixels in the RAM frame; one composed frame first (outside
     * the clipboard lock: it is the ordinary redraw). */
    if (desk_direct_settle) desk_direct_settle(d);
    int e=enter(); if (e) return -e; e=clip_capture_locked(d,active); pthread_mutex_unlock(&lock); return e;
}

int desk_clip_key(struct desktop *d,int code,int down,int alt)
{
    if (code!=CIUKI_KEY_PRINTSCREEN && code!=0x54) return 0;
    if (!down) return 1;
    int e=desk_clip_capture(d,alt); return e ? e : 1;
}
