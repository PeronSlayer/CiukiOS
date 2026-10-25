#include "schedsvc.h"
#pragma weak desk_schedsvc_run
#pragma weak desk_schedsvc_completed
#pragma weak desk_schedsvc_exited
/* SPDX-License-Identifier: MIT */
#include "desktop.h"
#include "gate.h"
#include "help_request.h"
#include "browser_launch.h"
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
extern void desk_clip_receive(struct desktop *, int, const struct ciuki_message *, const struct desk_packet *) __attribute__((weak));
extern void desk_clip_drop(struct desktop *, int) __attribute__((weak));
extern int desk_recovery_win(struct desktop *) __attribute__((weak)); /* G14 */
static void power_target(struct desktop *, uint32_t, const char *, int);
static void v3_configure(struct desktop *, int);
static void session_answer(struct desktop *, int, uint32_t, uint32_t);
static void session_pong(struct desktop *, int, uint32_t);
static void session_ping_sent(struct desktop *, int);
static void queued_attachments_close(struct desktop *d,const struct ciuki_message *m)
{
    if (m->data[0]==DESK_V3_VERSION && m->data[1]==DESK_DROP_FILES)
        for (unsigned k=0;k<m->fd_count && k<4;k++) if (m->fds[k]>=0) d->ops.close(m->fds[k]);
}
static uint64_t client_clock(struct desktop *);
static int netsurf_wallpaper_request(const struct desk_client *c, const struct desk_packet *p)
{
    static const char prefix[] = "--set-wallpaper=";
    const char *path;
    size_t n;
    if (!c || !p || (p->opcode != DESK_LAUNCH && p->opcode != DESK_V3_LAUNCH) ||
        (strcmp(c->application,"netsurf") && strcmp(c->application,"paint") && strcmp(c->application,"viewer")) ||
        strcmp(p->title,"control") || strncmp(p->argument,prefix,sizeof(prefix)-1)) return 0;
    path=p->argument+sizeof(prefix)-1;
    size_t length=strlen(path);
    if (path[0]!='/' || strstr(path,"/../") || strstr(path,"/./") || !path[1] ||
        (length>=3 && !strcmp(path+length-3,"/..")) || (length>=2 && !strcmp(path+length-2,"/.")) || length>=128) return 0;
    n=length;
    return (n>=4 && !strcasecmp(path+n-4,".png")) ||
           (n>=4 && !strcasecmp(path+n-4,".bmp")) ||
           (n>=4 && !strcasecmp(path+n-4,".jpg")) ||
           (n>=5 && !strcasecmp(path+n-5,".jpeg"));
}
int desk_ignore_sigpipe(void)
{ return signal(SIGPIPE,SIG_IGN)==SIG_ERR ? -1 : 0; }
void desk_init(struct desktop *d, const struct desk_ops *ops, uint32_t w, uint32_t h)
{
    memset(d, 0, sizeof(*d));
    d->cursor_loaded = DESK_CURSOR_COUNT;
    d->ops = *ops; d->width = w; d->height = h;
    d->display = d->input = d->output = d->focus = d->drag = d->about = d->authority_fd = d->mixer_fd = -1;
    d->media_fd = d->network_fd = d->mixer_observer_fd = -1;
    d->action = -1;
    d->mouse_x = (int32_t)w / 2; d->mouse_y = (int32_t)h / 2;
    d->clock_second = UINT64_MAX;
    d->title_click_window = d->icon_click_window = -1;
    d->cursor_loaded=DESK_CURSOR_COUNT;
    for (unsigned i = 0; i < DESK_CLIENTS; i++)
        d->clients[i].channel = d->clients[i].surface = -1;
    desk_damage_add(&d->damage, (struct desk_box){0, 0, (int32_t)w, (int32_t)h}, w, h);
}
void desk_theme_changed(struct desktop *d)
{ desk_render_release(d); d->sh.background_valid=0; desk_damage_add(&d->damage,(struct desk_box){0,0,(int32_t)d->width,(int32_t)d->height},d->width,d->height); }
int desk_add_client(struct desktop *d, int fd)
{
    for (unsigned i = 0; i < DESK_CLIENTS; i++) if (d->clients[i].channel == fd) return -1;
    for (unsigned i = 0; i < DESK_CLIENTS; i++) if (d->clients[i].channel < 0 && !d->clients[i].internal) {
        d->clients[i].channel = fd;
        return (int)i;
    }
    return -1;
}
void desk_drop_client(struct desktop *d, int i)
{
    struct desk_client *c = &d->clients[i];
    if (c->channel < 0 && !c->internal) return;
    if (!strcmp(c->application,"help")) { d->help_starting=0; d->help_pending[0]=0; memset(d->help_users,0,sizeof(d->help_users)); }
    if (c->version==DESK_V3_VERSION && !c->v3_peer)
        for (unsigned k=0;k<DESK_CLIENTS;k++) if (d->help_users[k]==c->pid) d->help_users[k]=0;
    if (c->version==DESK_V3_VERSION && !c->v3_peer) {
        for (int j=0;j<DESK_CLIENTS;j++) if (d->clients[j].v3_peer==(unsigned)i+1) desk_drop_client(d,j);
    }
    if (d->v3_lease_window==c->window_id && c->window_id) desk_v3_revoke(d);
    if (d->v3_reactivate_pending==i+1)d->v3_reactivate_pending=0;
    if (d->v3_pointer_window==i+1) desk_v3_pointer_release(d);
    if (c->v3_peer && c->window_id) for (int j=0;j<DESK_CLIENTS;j++)
        if (d->clients[j].v3_peer==c->v3_peer && d->clients[j].owner_id==c->window_id) desk_drop_client(d,j);
    if (desk_clip_drop) desk_clip_drop(d,i);
    uint32_t pid=c->pid, owner=c->owner_id, id=c->window_id;
    if (id) desk_shell_program_remove(d,id);
    char application[sizeof(c->application)]; memcpy(application,c->application,sizeof(application));
    int was_focused=d->focus==i;
    if (d->focus == i) desk_focus(d, -1);
    if(d->drag==i && d->geometry_outline) desk_geometry_end(d,1);
    if (d->drag == i) { d->drag = -1; d->resize_edges=0; d->keyboard_geometry=0; }
    /* fx-19: a drag whose source window closes ends; a target that closes is left. */
    if (d->shell && d->sh.dnd.active) {
        if (d->sh.dnd.source==i) desk_shell_drag_cancel(d);
        else if (d->sh.dnd.target==i && (d->sh.dnd.kind==SH_DROP_FILES || d->sh.dnd.kind==SH_DROP_APP)) {
            d->sh.dnd.kind=SH_DROP_NONE; d->sh.dnd.target=-1; d->sh.dnd.status_target=-1; d->sh.dnd.effect=0; d->sh.dnd.allowed=0;
        }
    }
    if (c->window) desk_sound(d,SOUND_WINDOW_CLOSE);
    if (c->window) desk_damage_add(&d->damage, desk_outer(c), d->width, d->height);
    /* f2-95: a closing program (an installer) may have added shortcuts. */
    if (c->window && !c->internal) d->v3_links_force|=DESK_LINKS_SCAN;
    if (c->pixels && !c->internal) d->ops.unmap((void *)c->pixels, c->info.allocation_bytes);
    if (c->surface >= 0) d->ops.close(c->surface);
    for (unsigned k=0;k<c->tx_count;k++) queued_attachments_close(d,&c->tx[(c->tx_head+k)%DESK_TX_MAX]);
    for (unsigned k=0;k<c->v3_over_count;k++) queued_attachments_close(d,&c->v3_over[(c->v3_over_head+k)%DESK_V3_OVER_MAX]);
    free(c->v3_over);
    if (c->channel >= 0 && !c->v3_peer && !c->channel_invalid) d->ops.close(c->channel);
    if (d->about == i) d->about = -1;
    if (c->internal) desk_internal_closed(d, i);
    memset(c, 0, sizeof(*c));
    c->channel = c->surface = -1;
    for (int j=0;j<DESK_CLIENTS;j++) if (d->clients[j].owner_id==id) d->clients[j].owner_id=0;
    if (was_focused || d->focus < 0) {
        int next=desk_next_window(d,i,pid,application);
        for (int j=0;j<DESK_CLIENTS;j++) if (owner && d->clients[j].window_id==owner &&
            d->clients[j].window && !d->clients[j].minimized) next=j;
        desk_focus(d, next);
    }
    if (d->shell) desk_damage_add(&d->damage,desk_taskbar(d),d->width,d->height);
    desk_cursor_update(d);
}
/* f2-91: v3 pointer motion is merged exactly like older protocols when the client is behind (last
 * position wins, only inside a trailing motion run of the same window), keys/buttons/everything else
 * keep their order. A full ring never drops the window: the message waits in an ordered overflow,
 * the windows are marked busy ("not responding" path) and delivery resumes when the client drains.
 * Only a client that read nothing for DESK_V3_SILENT_NS with ring and overflow both full is dropped. */
static int v3_is_motion(const struct ciuki_message *m)
{
    return m->data[0]==DESK_V3_VERSION && m->data[1]==DESK_MOTION && m->length==56 &&
        desk_get32(m->data+28)==CIUKI_INPUT_MOTION;
}
static struct ciuki_message *v3_trailing(struct desk_client *p)
{
    if (p->v3_over_count) return &p->v3_over[(p->v3_over_head+p->v3_over_count-1)%DESK_V3_OVER_MAX];
    if (p->tx_count) return &p->tx[(p->tx_head+p->tx_count-1)%DESK_TX_MAX];
    return NULL;
}
static void v3_set_busy(struct desktop *d, int pi, int busy)
{
    struct desk_client *p=&d->clients[pi];
    if (p->v3_busy==busy) return;
    p->v3_busy=busy;
    for (int j=0;j<DESK_CLIENTS;j++) {
        struct desk_client *w=&d->clients[j];
        if (w->channel<0 || w->v3_peer!=(unsigned)pi+1 || !w->window) continue;
        if (busy && !w->unresponsive) { w->unresponsive=1; w->v3_busy_mark=1; }
        else if (!busy && w->v3_busy_mark) { w->unresponsive=0; w->v3_busy_mark=0; }
        else continue;
        desk_damage_add(&d->damage,desk_outer(w),d->width,d->height);
        /* fx-2: the task button shows the state too ("Not responding"). */
        if (d->shell) desk_damage_add(&d->damage,desk_task_button(d,j),d->width,d->height);
    }
}
static void v3_check_silent(struct desktop *d, int pi)
{
    struct desk_client *p=&d->clients[pi];
    if (p->v3_failed || p->tx_count<DESK_TX_MAX || p->v3_over_count<DESK_V3_OVER_MAX) return;
    uint64_t silent=d->now_ns>p->v3_progress_ns ? d->now_ns-p->v3_progress_ns : 0;
    if (silent<DESK_V3_SILENT_NS) return;
    p->v3_failed=1;
    fprintf(stderr,"[desktop] client_drop window=%u pending=%u silent_ms=%llu\n",
        p->window_id,p->tx_count+p->v3_over_count,(unsigned long long)(silent/1000000ull));
}
static void v3_hold(struct desktop *d, int pi, const struct ciuki_message *wire)
{
    struct desk_client *p=&d->clients[pi];
    v3_set_busy(d,pi,1);
    if (v3_is_motion(wire)) {
        struct ciuki_message *q=v3_trailing(p);
        if (q && v3_is_motion(q) && desk_get32(q->data+8)==desk_get32(wire->data+8) &&
            desk_get32(q->data+12)==desk_get32(wire->data+12)) { *q=*wire; return; }
    }
    if (!p->v3_over) p->v3_over=malloc(sizeof(*p->v3_over)*DESK_V3_OVER_MAX);
    if (!p->v3_over || p->v3_over_count==DESK_V3_OVER_MAX) {
        queued_attachments_close(d,wire);
        p->v3_over_lost++; /* bounded: only reachable inside the silence deadline, newest message discarded */
        v3_check_silent(d,pi); return;
    }
    p->v3_over[(p->v3_over_head+p->v3_over_count++)%DESK_V3_OVER_MAX]=*wire;
    v3_check_silent(d,pi);
}
static void v3_refill(struct desktop *d, int pi)
{
    struct desk_client *p=&d->clients[pi];
    while (p->v3_over_count && p->tx_count<DESK_TX_MAX) {
        p->tx[(p->tx_head+p->tx_count++)%DESK_TX_MAX]=p->v3_over[p->v3_over_head];
        p->v3_over_head=(p->v3_over_head+1)%DESK_V3_OVER_MAX; p->v3_over_count--;
    }
    if (!p->v3_over_count && p->v3_busy) v3_set_busy(d,pi,0);
}
void desk_queue(struct desktop *d, int i, const struct ciuki_message *m)
{
    if (i < 0 || i >= DESK_CLIENTS) { queued_attachments_close(d,m); return; }
    struct desk_client *c = &d->clients[i];
    if (c->channel < 0) { queued_attachments_close(d,m); return; }
    if (c->version==DESK_V3_VERSION) {
        struct ciuki_message wire=*m;
        if (m->data[0]!=DESK_V3_VERSION) {
            unsigned op=m->data[1], payload=m->length-4;
            if (op==DESK_FOCUS) { op=DESK_V3_ACTIVATE; payload=8; }
            desk_v3_message(&wire,op,payload,0,c->v3_id,c->v3_generation);
            memcpy(wire.data+16,m->data+4,m->length-4);
            if (op==DESK_V3_ACTIVATE) desk_put32(wire.data+20,desk_get32(m->data+4));
        }
        struct desk_client *peer=c->v3_peer ? &d->clients[c->v3_peer-1] : c;
        int pi=(int)(peer-d->clients);
        if (peer->tx_count==DESK_TX_MAX || peer->v3_over_count) { v3_hold(d,pi,&wire); return; }
        if (!peer->tx_count) peer->v3_progress_ns=d->now_ns;
        peer->tx[(peer->tx_head+peer->tx_count++)%DESK_TX_MAX]=wire;
        return;
    }
    /* Coalesce unsent size proposals; committed acknowledgements stay in
     * FIFO order and must reach the client before it releases a surface. */
    if (m->data[1] == DESK_CONFIGURE && m->length == 20 && !desk_get32(m->data + 16)) {
        for (unsigned k = 0; k < c->tx_count; k++) {
            struct ciuki_message *q = &c->tx[(c->tx_head + k) % DESK_TX_MAX];
            if (q->data[1] == DESK_CONFIGURE && q->length == 20 && !desk_get32(q->data + 16)) {
                *q = *m; q->data[0] = (uint8_t)c->version; q->data[2] = (uint8_t)c->minor;
                c->configure_pending = 0; return;
            }
        }
    }
    if (c->tx_count == DESK_TX_MAX) {
        /* A full client FIFO means this peer is behind. Keep the newest
         * absolute pointer position in a trailing motion run; while there is
         * room, every motion remains a distinct FIFO message. */
        if (m->data[1] == DESK_MOTION && m->length==44 &&
            desk_get32(m->data+16)==CIUKI_INPUT_MOTION && c->tx_count) {
            unsigned last=(c->tx_head+c->tx_count-1)%DESK_TX_MAX;
            struct ciuki_message *q=&c->tx[last];
            if (q->data[1] == DESK_MOTION && q->length==44 &&
                desk_get32(q->data+16)==CIUKI_INPUT_MOTION) {
                *q=*m; q->data[0]=(uint8_t)c->version; q->data[2]=(uint8_t)c->minor;
                return;
            }
        }
        /* A stalled peer cannot block the compositor. Repair its input state
         * once its bounded queue drains. Keep the window and backing alive. */
        c->resync_pending = 1;
        if (m->data[1]==DESK_CONFIGURE) {
            if (c->minor && desk_get32(m->data + 16)) c->commit_pending=1;
            else c->configure_pending=1;
        }
        if (m->data[1]==DESK_FOCUS) c->focus_pending=1;
        if (m->data[1]==DESK_CLOSE_REQUEST || m->data[1]==DESK_CLOSED) c->close_pending=1;
        return;
    }
    unsigned slot = (c->tx_head + c->tx_count++) % DESK_TX_MAX;
    if (m->data[1] == DESK_CONFIGURE) {
        if (c->minor && desk_get32(m->data + 16)) c->commit_pending = 0;
        else c->configure_pending = 0;
    }
    c->tx[slot] = *m;
    if (c->version == DESK_SHELL_VERSION) c->tx[slot].data[0] = DESK_SHELL_VERSION;
    c->tx[slot].data[2] = (uint8_t)c->minor;
}
static void configure(struct desktop *d, int i, int committed)
{
    struct desk_client *c = &d->clients[i]; struct ciuki_message m;
    if (c->version==DESK_V3_VERSION) { v3_configure(d,i); return; }
    desk_message(&m, DESK_CONFIGURE, c->minor ? 20 : 12);
    desk_put32(m.data + 4, committed ? c->info.width : (uint32_t)(c->frame.w - 2 * DESK_BORDER));
    desk_put32(m.data + 8, committed ? c->info.height :
        (uint32_t)((c->collapsed ? c->shade_height : c->frame.h) - DESK_TITLE - DESK_BORDER));
    if (c->minor) {
        desk_put32(m.data + 12, committed ? c->applied_serial : c->resize_serial);
        desk_put32(m.data + 16, committed ? DESK_CONFIGURE_COMMITTED : 0);
    }
    desk_queue(d, i, &m);
}
void desk_flush(struct desktop *d, int i)
{
    struct desk_client *c = &d->clients[i];
    if (c->v3_peer) return;
    if (c->v3_failed) { desk_drop_client(d,i); return; }
    unsigned budget = DESK_TX_MAX;
    if (c->version==DESK_V3_VERSION) v3_refill(d,i);
    while (c->channel >= 0 && c->tx_count && budget--) {
        struct ciuki_message *message = &c->tx[c->tx_head];
        unsigned flags = DONTWAIT;
        if (message->data[1] == DESK_FOCUS && message->length == 8)
            flags |= desk_get32(message->data+4) ? CIUKI_CHANNEL_FOCUS : CIUKI_CHANNEL_UNFOCUS;
        if (i == d->focus && message->length == 44 &&
            (message->data[1] == DESK_KEY || message->data[1] == DESK_BUTTON || message->data[1] == DESK_MOTION) &&
            desk_get32(message->data+16) != CIUKI_INPUT_RESYNC)
            flags |= CIUKI_CHANNEL_INPUT;
        if (message->data[0]==DESK_V3_VERSION) {
            if (message->data[1]==DESK_V3_ACTIVATE)
                flags|=desk_get32(message->data+16) ? CIUKI_CHANNEL_FOCUS : CIUKI_CHANNEL_UNFOCUS;
            if (message->data[1]==DESK_KEY || message->data[1]==DESK_BUTTON || message->data[1]==DESK_MOTION ||
                message->data[1]==DESK_V3_RELATIVE_EVENT) {
                unsigned id=desk_get32(message->data+8), generation=desk_get32(message->data+12);
                if (d->focus>=0 && d->clients[d->focus].v3_id==id && d->clients[d->focus].v3_generation==generation &&
                    d->clients[d->focus].channel==c->channel && desk_get32(message->data+28)!=CIUKI_INPUT_RESYNC)
                    flags|=CIUKI_CHANNEL_INPUT;
            }
        }
        int r = d->ops.send(c->channel, message, flags);
        if (r < 0) {
            if (errno == EPIPE || errno == EBADF) desk_drop_client(d, i);
            return;
        }
        /* A shell-owned wait or transport backpressure before this send is
         * not client time. v3 PINGs share their transport: stamp the addressed
         * window, rather than the transport anchor. */
        if (message->data[1]==DESK_PING) {
            int target=i;
            if (message->data[0]==DESK_V3_VERSION) {
                uint32_t id=desk_get32(message->data+8), gen=desk_get32(message->data+12);
                for (int k=0;k<DESK_CLIENTS;k++) if (d->clients[k].v3_id==id && d->clients[k].v3_generation==gen &&
                    d->clients[k].channel==c->channel) { target=k; break; }
            }
            struct desk_client *ping=&d->clients[target];
            if (ping->ping_pending && ping->ping_queued &&
                desk_get32(message->data+(message->data[0]==DESK_V3_VERSION ? 16 : 4))==ping->serial) {
                ping->ping_queued=0; ping->ping_sent_ns=client_clock(d);
            }
        }
        d->replied++;
        queued_attachments_close(d,message);
        c->tx_head = (c->tx_head + 1) % DESK_TX_MAX; c->tx_count--;
        c->v3_progress_ns = d->now_ns;
        if (c->version==DESK_V3_VERSION) v3_refill(d,i);
    }
    if (c->channel>=0 && c->gfx_reply_pending && c->tx_count<DESK_TX_MAX)
        desk_gfx_complete(d,i,c->gfx_result,c->gfx_completed_ns,c->gfx_next_ns);
    if (c->channel >= 0 && !c->tx_count) {
        struct ciuki_message m;
        if (c->commit_pending) { c->commit_pending = 0; configure(d, i, 1); }
        if (c->configure_pending) {
            c->configure_pending=0; configure(d,i,!c->minor || !c->resize_serial);
        }
        if (c->focus_pending) {
            desk_message(&m,DESK_FOCUS,8); desk_put32(m.data+4,(uint32_t)(d->focus==i));
            c->focus_pending=0; desk_queue(d,i,&m);
        }
        if (c->close_pending) {
            desk_message(&m,c->version>=DESK_SHELL_VERSION ? DESK_CLOSE_REQUEST : DESK_CLOSED,4);
            c->close_pending=0; desk_queue(d,i,&m);
        }
    }
    if (c->channel >= 0 && c->tx_count<DESK_TX_MAX && c->resync_pending) {
        struct ciuki_input_event e = { .type = CIUKI_INPUT_RESYNC, .lost_count = 1 };
        struct ciuki_message m;
        desk_event_message(&m, DESK_KEY, &e);
        c->resync_pending = 0;
        desk_queue(d, i, &m);
    }
}
static int32_t size_limit(int32_t v, uint32_t min, uint32_t max, uint32_t step)
{
    if (v < (int32_t)min) v = (int32_t)min;
    if (v > (int32_t)max) v = (int32_t)max;
    return v - (v - (int32_t)min) % (int32_t)step;
}
void desk_resize(struct desktop *d, int i, struct desk_box f)
{
    if (i < 0 || i >= DESK_CLIENTS) return;
    struct desk_client *c = &d->clients[i];
    if (!c->window || !c->resizable || c->collapsed || c->internal) return;
    int left=d->drag==i && (d->resize_edges&DESK_EDGE_LEFT);
    int top=d->drag==i && (d->resize_edges&DESK_EDGE_TOP);
    int right=c->frame.x+c->frame.w, bottom=c->frame.y+c->frame.h;
    struct desk_box work=desk_work_area(d);
    int work_bottom=work.y+work.h;
    if (f.x<0 && !left) f.x=0;
    if (f.y<DESK_BAR && !top) f.y=DESK_BAR;
    int available_w=(left ? right-work.x : work.x+work.w-f.x)-2*DESK_BORDER;
    int available_h=(top ? bottom-DESK_BAR : work_bottom-f.y)-DESK_TITLE-DESK_BORDER;
    uint32_t max_w=c->max_width, max_h=c->max_height;
    /* A client minimum can exceed a newly selected display. Honour that
     * minimum and keep its title reachable rather than sending invalid sizes. */
    if (available_w>=(int)c->min_width && available_w<(int)max_w) max_w=(unsigned)available_w;
    if (available_h>=(int)c->min_height && available_h<(int)max_h) max_h=(unsigned)available_h;
    int32_t w = size_limit(f.w - 2 * DESK_BORDER, c->min_width, max_w, c->step_width);
    int32_t h = size_limit(f.h - DESK_TITLE - DESK_BORDER, c->min_height, max_h, c->step_height);
    /* The opposite edge stays put when tracking a left/top edge. */
    if (left) f.x=right-w-2*DESK_BORDER;
    if (top) f.y=bottom-h-DESK_TITLE-DESK_BORDER;
    f.w = w + 2 * DESK_BORDER; f.h = h + DESK_TITLE + DESK_BORDER;
    if (f.x < 0) f.x = 0;
    if (f.y < DESK_BAR) f.y = DESK_BAR;
    if (!memcmp(&f, &c->frame, sizeof(f))) return;
    desk_damage_add(&d->damage, desk_outer(c), d->width, d->height);
    c->frame = f;
    if (!++c->resize_serial) ++c->resize_serial;
    configure(d, i, 0);
    desk_damage_add(&d->damage, desk_outer(c), d->width, d->height);
}
static void animation_damage(struct desktop *d, struct desk_box b)
{
    struct desk_box edges[4]={{b.x,b.y,b.w,3},{b.x,b.y+b.h-3,b.w,3},
        {b.x,b.y+3,3,b.h-6},{b.x+b.w-3,b.y+3,3,b.h-6}};
    for (unsigned k=0;k<4;k++) desk_damage_add(&d->damage,edges[k],d->width,d->height);
}
void desk_animate(struct desktop *d, struct desk_box from, struct desk_box to)
{
    if (!d->shell || !d->now_ns || from.w<=0 || to.w<=0) return;
    if (d->animation.active) animation_damage(d,d->animation.box);
    d->animation.from=from; d->animation.to=to; d->animation.box=from;
    d->animation.started=d->now_ns; d->animation.step=0; d->animation.active=1;
    animation_damage(d,from);
}
void desk_animation_tick(struct desktop *d)
{
    if (!d->animation.active) return;
    uint64_t age=d->now_ns>=d->animation.started ? d->now_ns-d->animation.started : 0;
    unsigned step=age>=150000000ull ? 6 : (unsigned)(age/25000000ull);
    if (step==d->animation.step) return;
    animation_damage(d,d->animation.box);
    d->animation.step=step;
    if (step==6) { d->animation.active=0; return; }
    struct desk_box a=d->animation.from, b=d->animation.to;
    d->animation.box=(struct desk_box){a.x+(b.x-a.x)*(int)step/6,a.y+(b.y-a.y)*(int)step/6,
        a.w+(b.w-a.w)*(int)step/6,a.h+(b.h-a.h)*(int)step/6};
    animation_damage(d,d->animation.box);
}
/* Only a successfully presented frame in the requested/confirmed mode may
 * replace the saved preference. Control Panel previews retain that preference
 * until its confirmation save. Failed reallocations still have the old id. */
int desk_display_frame_commit(struct desktop *d, int redraw)
{
    if (redraw || !d->display_pending || d->display_pending != d->display_mode || d->damage.count) return 0;
    uint16_t mode=d->display_pending;
    d->display_pending = 0;
    if (d->display_reply_mode && d->display_reply_preview) return DESK_DISPLAY_PREVIEW_FRAME;
    d->sh.cfg.display_mode=mode;
    return DESK_DISPLAY_PERSIST_FRAME;
}
void desk_display_reply(struct desktop *d, int result)
{
    if (!d->display_reply_mode) return;
    int i=d->display_reply_client;
    if (i>=0 && i<DESK_CLIENTS && d->clients[i].channel>=0 && d->clients[i].pid==d->display_reply_pid) {
        struct ciuki_message m; desk_message(&m,DESK_TASK_RESULT,16);
        desk_put32(m.data+4,d->display_reply_serial); desk_put32(m.data+8,DESK_SETTINGS);
        desk_put32(m.data+12,(uint32_t)result); desk_queue(d,i,&m);
    }
    d->display_reply_mode=0; d->display_reply_preview=0;
}

/* Called only after allocating the new screen surface. Client surfaces stay
 * attached until their CONFIGURE/DAMAGE handshake replaces them. */
void desk_display_relayout(struct desktop *d, uint32_t width, uint32_t height)
{
    desk_render_release(d);
    d->width = width; d->height = height;
    d->drag = -1; d->resize_edges = 0; d->geometry_outline=0;
    d->title_hot = d->title_down = 0;
    if (d->mouse_x >= (int32_t)width) d->mouse_x = (int32_t)width - 1;
    if (d->mouse_y >= (int32_t)height) d->mouse_y = (int32_t)height - 1;
    struct desk_box area=desk_work_area(d);
    int32_t work=area.h;
    for (int i = 0; i < DESK_CLIENTS; i++) {
        struct desk_client *c = &d->clients[i];
        if (!c->window) continue;
        if (c->collapsed && c->resizable) desk_collapse(d, i);
        if (c->resizable && !c->internal) {
            struct desk_box f = c->frame;
            /* Size against the new work area, not the old window position:
             * desk_resize clamps sizes using the requested origin. */
            if (c->zoomed) { f.x=area.x;f.y=area.y; }
            if (c->zoomed || f.w>area.w) f.w=area.w;
            if (c->zoomed || f.h > work) f.h = work;
            uint32_t serial = c->resize_serial;
            desk_resize(d, i, f);
            /* The display geometry changed even if this client's size did
             * not. Issue a fresh serial, allowing the client to re-layout. */
            if (serial == c->resize_serial) {
                if (!++c->resize_serial) ++c->resize_serial;
                configure(d, i, 0);
            }
        }
        desk_move(d, i, c->zoomed ? area.x : c->frame.x, c->zoomed ? area.y : c->frame.y);
        int32_t max_x = (int32_t)width - c->restore.w, max_y = (int32_t)height - c->restore.h - (d->shell ? DESK_BAR : 0);
        if (max_x < 0) max_x = 0;
        if (max_y < DESK_BAR) max_y = DESK_BAR;
        if (c->restore.x > max_x) c->restore.x = max_x;
        if (c->restore.y > max_y) c->restore.y = max_y;
    }
    d->sh.wall_valid = 0;
    if (d->shell) desk_shell_apply(d);
    d->damage.count = 0;
    desk_damage_add(&d->damage, (struct desk_box){0, 0, (int32_t)width, (int32_t)height}, width, height);
}

void desk_zoom(struct desktop *d, int i)
{
    if (i < 0 || i >= DESK_CLIENTS || !d->clients[i].window) return;
    struct desk_client *c = &d->clients[i];
    if (!c->resizable || c->internal || desk_modal_target(d,i)!=i) return;
    if (c->collapsed) desk_collapse(d, i);
    struct desk_box f;
    if (c->zoomed) f = c->restore;
    else {
        c->restore = c->frame;
        f = desk_work_area(d);
    }
    struct desk_box old=c->frame;
    c->zoomed = !c->zoomed; desk_resize(d, i, f);
    desk_animate(d,old,c->frame);
}
void desk_move(struct desktop *d, int i, int32_t x, int32_t y)
{
    struct desk_client *c = &d->clients[i];
    int32_t max_x = (int32_t)d->width - c->frame.w;
    struct desk_box work=desk_work_area(d);
    int32_t max_y=work.y+work.h-c->frame.h;
    if (max_x < 0) max_x = 0;
    if (max_y < DESK_BAR) max_y = DESK_BAR;
    if (x < 0) x = 0;
    if (x > max_x) x = max_x;
    if (y < DESK_BAR) y = DESK_BAR;
    if (y > max_y) y = max_y;
    if (c->frame.x == x && c->frame.y == y) return;
    if (d->shell_test && d->shell_stage == 3 && c->pid == d->shell_pid && d->drag == i)
        d->shell_stage = 4;
    struct desk_box old = desk_outer(c), frame = c->frame;
    int continuing=d->copy_pending && d->copy_client==i &&
        d->copy_rect.width==(uint32_t)frame.w && d->copy_rect.height==(uint32_t)(frame.h-desk_theme->radius);
    /* fx-2: damage recorded at the intermediate position (a client update,
     * a caption) changed what the planned copy would carry from the
     * original source, and the redraw can only exclude it at the source or
     * the final destination. Recompose this move instead of copying. */
    /* fx-26: only damage inside the carried rectangle at the intermediate
     * position can change what the copy carries (the compositor clips the
     * copy to the window's opaque visible region). Damage of a window below
     * that merely touches the moving window's shadow ring, a DOS window
     * playing under the dragged one, cancelled the copy on every motion
     * and recomposed both outer boxes in software (T23 model: 34-71 ms
     * frames for an 800x560 drag). */
    struct desk_box carried={frame.x,frame.y+desk_theme->radius,frame.w,frame.h-desk_theme->radius};
    for (unsigned k=0;continuing && k<d->damage.count;k++) {
        struct desk_box touched;
        if (desk_intersect(d->damage.boxes[k],carried,&touched)) continuing=0;
    }
    /* A new copy is useful only when its source is clean: damage inside
     * the window's own carried area (its update, a cancelled move's boxes)
     * is recomposed there anyway. */
    int clean=1;
    for (unsigned k=0;!continuing && clean && k<d->damage.count;k++) {
        struct desk_box touched;
        if (desk_intersect(d->damage.boxes[k],carried,&touched)) clean=0;
    }
    /* fx-26: a new move copies even while other damage is pending (a clock,
     * a playing DOS window, a notification sliding in), as long as none is
     * inside its own source: the compositor cuts every damage box, and its
     * image at the source, out of the copy
     * (copy_region/region_cut in desk_redraw), as Windows 98 copies the
     * window bits with BitBlt and repaints only the invalid parts
     * (SetWindowPos without SWP_NOCOPYBITS). Requiring no damage at all made
     * every drag frame a software recomposition of both outer boxes while
     * anything else on screen changed (T23 f441c5bd). */
    int copy = d->frame_valid && !d->present_damage.count && (continuing || (!d->copy_pending && clean)) && !d->power.state && !d->geometry_outline && !d->animation.active &&
        !c->internal && !c->collapsed && !c->minimized && c->order>0;

    if (frame.x < 0 || frame.y < 0 || frame.x+frame.w > (int32_t)d->width ||
        frame.y+frame.h > (int32_t)d->height) copy=0;
    /* The compositor clips the copy against both the old and new visible
     * regions. A higher window no longer disables the entire BitBlt. */
    if (d->copy_pending && !copy) {
        for (unsigned k=0;k<d->move_damage.count;k++)
            desk_damage_add(&d->damage,d->move_damage.boxes[k],d->width,d->height);
        d->move_damage.count=0; d->copy_pending=0;
        desk_damage_add(&d->damage,frame,d->width,d->height);
    }
    c->frame.x = x; c->frame.y = y;
    if (c->version==DESK_V3_VERSION) v3_configure(d,i);
    if (copy) {
        if (continuing) { d->copy_rect.dst_x=x; d->copy_rect.dst_y=y+desk_theme->radius; }
        else d->copy_rect=(struct ciuki_rect){frame.x,frame.y+desk_theme->radius,x,y+desk_theme->radius,(uint32_t)frame.w,(uint32_t)(frame.h-desk_theme->radius)};
        d->copy_pending=1; d->copy_client=i;
    }
    struct desk_damage *damage=copy ? &d->move_damage : &d->damage;
    /* Drop intermediate move footprints: RAM/scanout still hold the original. */
    if (copy && continuing) {
        /* Reconstruct the original outer box from the retained source. */
        old=desk_outer(c);
        old.x+=d->copy_rect.src_x-c->frame.x;
        old.y+=d->copy_rect.src_y-desk_theme->radius-c->frame.y;
        damage->count=0;
    }
    desk_damage_add(damage,old,d->width,d->height);
    desk_damage_add(damage,desk_outer(c),d->width,d->height);
}
static void close_attachments(struct desktop *d, const struct ciuki_message *m, int keep)
{
    for (unsigned k = 0; k < m->fd_count && k < 4; k++)
        if (m->fds[k] >= 0 && m->fds[k] != keep) d->ops.close(m->fds[k]);
}
static int replace_client_surface(struct desktop *d,int i,const struct ciuki_message *m,
    const struct ciuki_surface_info *info,uint32_t serial)
{
    struct desk_client *c=&d->clients[i];
    void *pixels=d->ops.map(m->fds[0],PROT_READ);
    if (!pixels || pixels==MAP_FAILED) { close_attachments(d,m,-1);desk_drop_client(d,i);return -1; }
    desk_damage_add(&d->damage,desk_outer(c),d->width,d->height);
    d->ops.unmap((void *)c->pixels,c->info.allocation_bytes);d->ops.close(c->surface);
    c->pixels=pixels;c->surface=m->fds[0];c->info=*info;c->applied_serial=serial;
    c->gfx_box=(struct desk_box){0};c->gfx_width=c->gfx_height=0;
    configure(d,i,1);return 0;
}
/* fx-15: surface coordinates to screen coordinates. A pending resize
 * stretches the retained backing across the whole well (compositor.c
 * client_rows, nearest sampling): damage is mapped through that ratio and
 * rounded outwards, so every texel's stretched copy is repainted. */
static struct desk_box surface_damage(const struct desk_client *c, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    struct desk_box b=desk_content(c);
    if (!c->info.width || !c->info.height || b.w<=0 || b.h<=0) return (struct desk_box){0,0,0,0};
    uint64_t sw=c->info.width, sh=c->info.height, dw=(uint64_t)b.w, dh=(uint64_t)b.h;
    uint64_t left=(uint64_t)x*dw/sw, top=(uint64_t)y*dh/sh;
    uint64_t right=(((uint64_t)x+w)*dw+sw-1)/sw, bottom=(((uint64_t)y+h)*dh+sh-1)/sh;
    return (struct desk_box){b.x+(int32_t)left,b.y+(int32_t)top,(int32_t)(right-left),(int32_t)(bottom-top)};
}
static uint64_t client_clock(struct desktop *d) { return d->time_ns ? d->time_ns() : d->now_ns; }
static void client_frame_ready(struct desktop *d,struct desk_client *c)
{
    if (c->awaiting_frame) {
        c->damage_ns=client_clock(d);
        c->launch_frame_pending=1;
        if (d->launch_mark) d->launch_mark(CIUKI_LAUNCH_DAMAGE,c->pid,0);
    }
    c->awaiting_frame=0;
}
void desk_gfx_complete(struct desktop *d, int i, int result, uint64_t completed, uint64_t next)
{
    struct desk_client *c=&d->clients[i];
    if (!c->gfx_pending) return;
    if (c->gfx_reply_pending) { result=c->gfx_result;completed=c->gfx_completed_ns;next=c->gfx_next_ns; }
    if (c->tx_count==DESK_TX_MAX) {
        c->gfx_reply_pending=1;c->gfx_result=result;c->gfx_completed_ns=completed;c->gfx_next_ns=next;
        return;
    }
    struct ciuki_message reply; desk_message(&reply,DESK_GFX_PRESENTED,c->gfx_timing ? 64 : 32);
    desk_put32(reply.data+4,c->gfx_serial); desk_put32(reply.data+8,(uint32_t)result);
    desk_put32(reply.data+12,(uint32_t)completed); desk_put32(reply.data+16,(uint32_t)(completed>>32));
    if (next<completed) next=completed;
    desk_put32(reply.data+20,(uint32_t)next); desk_put32(reply.data+24,(uint32_t)(next>>32));
    if (c->gfx_timing) {
        uint64_t values[4]={c->gfx_stats.convert_ns,c->gfx_stats.copy_ns,c->gfx_stats.slot_ns,DESK_FRAME_NS};
        for (unsigned k=0;k<4;k++) {
            desk_put32(reply.data+32+k*8,(uint32_t)values[k]);
            desk_put32(reply.data+36+k*8,(uint32_t)(values[k]>>32));
        }
        if (!result) d->gfx_sample=c->gfx_stats;
    }
    desk_queue(d,i,&reply); c->gfx_pending=c->gfx_reply_pending=0;
}
static void input_state(struct desktop *, int, uint32_t);
static int input_setting(struct desktop *, const struct desk_packet *);
void desk_receive(struct desktop *d, int i, const struct ciuki_message *m)
{
    if (m->length && m->data[0]==DESK_V3_VERSION) { (void)desk_v3_receive(d,i,m); return; }
    struct desk_client *c = &d->clients[i];
    if (c->version==DESK_V3_VERSION) { close_attachments(d,m,-1); desk_drop_client(d,i); return; }
    struct desk_packet p;
    struct ciuki_surface_info s = c->info;
    int create = m->length >= 4 && (m->data[1] == DESK_CREATE_WINDOW || m->data[1] == DESK_DAMAGE || m->data[1]==DESK_GFX_FRAME) && m->fd_count == 1;
    int info_ok = !create || !d->ops.info(m->fds[0], &s);
    uint32_t swap= (m->length==68 || m->length==84) && m->data[1]==DESK_GFX_FRAME ? desk_get32(m->data+64) :
        m->length==24 && m->data[1]==DESK_DAMAGE ? desk_get32(m->data+20) : 0;
    if (c->channel < 0 || !info_ok || !desk_parse(m, 1, &s, &p) ||
        !m->sender_pid || (c->pid && c->pid != m->sender_pid) ||
        (c->hello && (c->version != m->data[0] || c->minor != m->data[2])) ||
        (!c->hello && p.opcode != DESK_HELLO) ||
        (c->hello && p.opcode == DESK_HELLO) ||
        (p.opcode == DESK_CREATE_WINDOW && c->window) ||
        ((p.opcode == DESK_DAMAGE || p.opcode == DESK_MOVE || p.opcode == DESK_CLOSE || p.opcode == DESK_SET_TITLE || p.opcode==DESK_SET_CURSOR) && !c->window) ||
        ((p.opcode == DESK_DAMAGE || p.opcode==DESK_GFX_FRAME) && swap && (!c->resizable || swap > c->resize_serial ||
            swap <= c->applied_serial || s.width < c->min_width || s.height < c->min_height ||
            s.width > c->max_width || s.height > c->max_height ||
            (swap == c->resize_serial &&
                (s.width != (uint32_t)(c->frame.w - 2 * DESK_BORDER) ||
                 s.height != (uint32_t)((c->collapsed ? c->shade_height : c->frame.h) - DESK_TITLE - DESK_BORDER))) ||
            (s.width - c->min_width) % c->step_width || (s.height - c->min_height) % c->step_height))) {
        close_attachments(d, m, -1);
        struct ciuki_message closed;
        desk_message(&closed, DESK_CLOSED, 4);
        if (c->version==DESK_SHELL_VERSION) closed.data[0]=DESK_SHELL_VERSION;
        closed.data[2]=(uint8_t)c->minor;
        if (c->channel >= 0) d->ops.send(c->channel, &closed, DONTWAIT);
        desk_drop_client(d, i);
        return;
    }
    c->pid = m->sender_pid;
    d->received++;
    struct ciuki_message reply;
    switch (p.opcode) {
    case DESK_CLIPBOARD:
        if (desk_clip_receive) desk_clip_receive(d,i,m,&p);
        else close_attachments(d,m,-1);
        break;
    case DESK_GFX_FRAME: {
        if (!c->gfxdemo || !c->window || c->closing || c->gfx_pending) {
            /* A rejected request never changes the persistent scene mapping. */
            desk_message(&reply,DESK_GFX_PRESENTED,32);
            desk_put32(reply.data+4,p.serial);
            desk_put32(reply.data+8,(uint32_t)(c->gfx_pending ? -EBUSY : -EPERM));
            desk_put32(reply.data+12,(uint32_t)d->now_ns); desk_put32(reply.data+16,(uint32_t)(d->now_ns>>32));
            uint64_t next=d->next_frame_ns>d->now_ns ? d->next_frame_ns : d->now_ns;
            desk_put32(reply.data+20,(uint32_t)next); desk_put32(reply.data+24,(uint32_t)(next>>32));
            close_attachments(d,m,-1);desk_queue(d,i,&reply);break;
        }
        if (p.resize_serial && replace_client_surface(d,i,m,&s,p.resize_serial)) return;
        client_frame_ready(d,c);
        struct desk_box content=desk_content(c), old=c->gfx_box;
        struct desk_box region={p.x,p.y,(int32_t)p.width,(int32_t)p.height};
        int changed=memcmp(&old,&region,sizeof(old)) || c->gfx_width!=p.source_w || c->gfx_height!=p.source_h;
        c->gfx_box=region; c->gfx_width=p.source_w; c->gfx_height=p.source_h;
        c->gfx_serial=p.serial; c->gfx_pending=1;
        c->gfx_timing=m->length==84;
        uint64_t received=c->gfx_timing && d->time_ns ? d->time_ns() : d->now_ns;
        c->gfx_stats=(struct desk_gfx_stats){.serial=p.serial,.app_serial=p.timing_serial,
            .render_us=p.render_us,.submit_us=p.submit_us,.wait_us=p.wait_us,
            .width=c->info.width,.height=c->info.height,.source_w=p.source_w,.source_h=p.source_h,
            .view_w=p.width,.view_h=p.height,
            .slot_ns=d->next_frame_ns>received ? d->next_frame_ns-received : 0};
        if (c->minimized || c->collapsed) { desk_gfx_complete(d,i,-EAGAIN,d->now_ns,d->now_ns+DESK_FRAME_NS); break; }
        (void)content;
        if (p.ui_w && p.ui_h) desk_client_damage(d,i,surface_damage(c,p.ui_x,p.ui_y,p.ui_w,p.ui_h));
        if (changed && old.w && old.h)
            desk_client_damage(d,i,surface_damage(c,(uint32_t)old.x,(uint32_t)old.y,(uint32_t)old.w,(uint32_t)old.h));
        struct desk_box dirty=region;
        if (!changed && p.source_w && p.damage_w && p.damage_h) {
            unsigned left=p.damage_x*p.width/p.source_w, top=p.damage_y*p.height/p.source_h;
            unsigned right=((p.damage_x+p.damage_w)*p.width+p.source_w-1)/p.source_w;
            unsigned bottom=((p.damage_y+p.damage_h)*p.height+p.source_h-1)/p.source_h;
            dirty=(struct desk_box){p.x+(int)left,p.y+(int)top,(int)(right-left),(int)(bottom-top)};
        }
        /* No changed texels still needs a completion, but no upload. */
        if (changed || (p.damage_w && p.damage_h))
            desk_client_damage(d,i,surface_damage(c,(uint32_t)dirty.x,(uint32_t)dirty.y,(uint32_t)dirty.w,(uint32_t)dirty.h));
        if (!d->damage.count) desk_gfx_complete(d,i,-EAGAIN,d->now_ns,d->now_ns+DESK_FRAME_NS);
        break;
    }
    case DESK_GFX_QUERY: case DESK_GFX_QUERY_ENGINE: case DESK_GFX_BENCH: {
        if ((!c->gfxdemo && !((c->program==DESK_APP_CONTROL || !strcmp(c->application,"hwreport")) && (p.opcode==DESK_GFX_QUERY || p.opcode==DESK_GFX_QUERY_ENGINE))) || !c->window || c->closing) {
            desk_message(&reply,DESK_TASK_RESULT,16); desk_put32(reply.data+4,p.serial);
            desk_put32(reply.data+8,p.opcode); desk_put32(reply.data+12,(uint32_t)-EPERM);
        } else if (p.opcode==DESK_GFX_QUERY_ENGINE) {
            struct ciuki_display_engine_info a={.version=CIUKI_DISPLAY_ENGINE_VERSION,.size=sizeof(a),
                .software_operations=CIUKI_ENGINE_SOFTWARE_OPERATIONS};
            int e=d->engine_info ? d->engine_info(d->display,d->accel_generation,&a) : -ENOSYS;
            desk_message(&reply,DESK_GFX_INFO_ENGINE,256);
            desk_put32(reply.data+4,p.serial); desk_put32(reply.data+8,(uint32_t)e);
            desk_put32(reply.data+12,d->width); desk_put32(reply.data+16,d->height);
            desk_put32(reply.data+20,d->lfb_bytes_per_pixel*8);
            memcpy(reply.data+24,&a,sizeof(a));
        } else if (p.opcode==DESK_GFX_QUERY) {
            struct ciuki_accel_info a={0};
            int e=d->accel_info ? d->accel_info(d->display,d->accel_generation,&a) : -ENOSYS;
            desk_message(&reply,DESK_GFX_INFO,64); desk_put32(reply.data+4,p.serial);
            desk_put32(reply.data+8,(uint32_t)e); desk_put32(reply.data+12,d->width);
            desk_put32(reply.data+16,d->height); desk_put32(reply.data+20,d->lfb_bytes_per_pixel*8);
            desk_put32(reply.data+24,a.driver); desk_put32(reply.data+28,a.capabilities);
            desk_put32(reply.data+32,a.quarantined); desk_put32(reply.data+36,a.generation);
            uint64_t values[3]={a.operations,a.idle_waits,a.fallbacks};
            for (unsigned k=0;k<3;k++) {
                desk_put32(reply.data+40+k*8,(uint32_t)values[k]); desk_put32(reply.data+44+k*8,(uint32_t)(values[k]>>32));
            }
        } else {
            uint64_t values[3]={0};
            int e=-EBUSY;
            if (d->now_ns>=d->gfx_next_bench_ns && !d->power.state && !c->minimized && d->focus==i) {
                d->gfx_next_bench_ns=d->now_ns+100000000ull;
                e=d->gfx_benchmark ? d->gfx_benchmark(d,p.index,values) : -ENOSYS;
            }
            desk_message(&reply,DESK_GFX_RESULT,40); desk_put32(reply.data+4,p.serial);
            desk_put32(reply.data+8,p.index); desk_put32(reply.data+12,(uint32_t)e);
            for (unsigned k=0;k<3;k++) {
                desk_put32(reply.data+16+k*8,(uint32_t)values[k]); desk_put32(reply.data+20+k*8,(uint32_t)(values[k]>>32));
            }
        }
        desk_queue(d,i,&reply); break;
    }
    case DESK_SYSTEM_QUERY: {
        if (!c->taskman && !c->gfxdemo) {
            desk_message(&reply,DESK_TASK_RESULT,16); desk_put32(reply.data+4,p.serial);
            desk_put32(reply.data+8,DESK_SYSTEM_QUERY); desk_put32(reply.data+12,(uint32_t)-EPERM);
        } else {
            const struct desk_frame_stats *s=&d->sample;
            uint64_t values[9]={s->frames,s->damage_px,s->presents,s->lfb_bytes,s->composite_ns,
                               s->present_ns,s->input_ns,s->input_lag_ns,d->sample_ns};
            desk_message(&reply,DESK_SYSTEM_INFO,80); desk_put32(reply.data+4,p.serial);
            for (unsigned k=0;k<9;k++) {
                desk_put32(reply.data+8+k*8,(uint32_t)values[k]);
                desk_put32(reply.data+12+k*8,(uint32_t)(values[k]>>32));
            }
        }
        desk_queue(d,i,&reply); break;
    }
    case DESK_HELLO:
        c->hello = 1;
        c->version = m->data[0];
        c->minor = m->data[2];
        c->next_ping_ns = d->now_ns + 1000000000ull;
        break;
    case DESK_CREATE_WINDOW: {
        void *pixels = d->ops.map(m->fds[0], PROT_READ);
        if (pixels == MAP_FAILED || !pixels) {
            close_attachments(d, m, -1); desk_drop_client(d, i); return;
        }
        c->surface = m->fds[0]; c->info = s; c->pixels = pixels; c->window = 1;
        desk_sound(d,SOUND_WINDOW_OPEN);
        c->resizable = (p.flags & DESK_RESIZABLE) != 0;
        c->min_width = p.min_width; c->min_height = p.min_height;
        c->max_width = p.max_width; c->max_height = p.max_height;
        c->step_width = p.step_width; c->step_height = p.step_height;
        if (!++d->next_window_id) ++d->next_window_id;
        c->window_id = d->next_window_id;
        memcpy(c->title, p.title, sizeof(c->title));
        c->frame = desk_frame(s.width, s.height, 0, DESK_BAR);
        unsigned slot=d->cascade++%8;
        desk_move(d, i, 20 + (int)slot * 18, DESK_BAR + 20 + (int)slot * 18);
        desk_damage_add(&d->damage, desk_outer(c), d->width, d->height);
        configure(d, i, 1);
        if (c->awaiting_frame) c->window_ns=client_clock(d);
        if (c->awaiting_frame && d->launch_mark) d->launch_mark(CIUKI_LAUNCH_CONFIGURE,c->pid,0);
        if (d->shell && d->menu_open) {
            c->order=++d->order; d->menu_focus=i;
            desk_message(&reply,DESK_FOCUS,8); desk_queue(d,i,&reply);
        } else desk_focus(d, i);
        if (c->initial_show==3) desk_zoom(d,i);
        else if (c->initial_show==7) desk_minimize(d,i);
        c->initial_show=0;
        if (d->shell) desk_damage_add(&d->damage,desk_taskbar(d),d->width,d->height);
        if (d->shell_test && c->pid == d->shell_pid && d->shell_stage == 2) d->shell_stage = 3;
        if (d->tasks_test && d->tasks_stage == 1 && c->pid == d->tasks_pid) d->tasks_stage = 2;
        if (d->tasks_test && d->tasks_stage == 3 && c->pid == d->tasks_demo_pid) {
            d->tasks_stage = 4; d->tasks_demo_window = c->window_id;
        }
        if (d->power.state) {
            power_target(d,c->pid,c->title,0);
            desk_request_close(d,i);
        }
        break;
    }
    case DESK_DAMAGE: {
        if (p.serial) {
            if (replace_client_surface(d,i,m,&s,p.serial)) return;
            /* A superseded request already in flight may commit; keep the
             * newest frame geometry and clip this surface until its swap. */
        } else {
            int visible=desk_client_damage(d,i,surface_damage(c,(uint32_t)p.x,(uint32_t)p.y,p.width,p.height));
            /* The DOS producer has already waited for its VM's real VGA
             * deadline. Do not quantize 35/70 Hz to the shell's 30/60 Hz. */
            if (visible && c->dosbox && d->next_frame_ns>d->now_ns) d->next_frame_ns=d->now_ns;
        }
        /* A valid replacement surface can also carry the first rendered frame. */
        client_frame_ready(d,c);
        break;
    }
    case DESK_FILE_DROP:
        if (d->shell && !d->sh.gate && !d->power.state && c->window && c->program==DESK_APP_FILES) {
            struct desk_box content=desk_content(c);
            int x=d->mouse_x,y=d->mouse_y;
            d->mouse_x=content.x+p.x; d->mouse_y=content.y+p.y;
            desk_folder_drop(d,i,(int)p.flags);
            d->mouse_x=x; d->mouse_y=y;
        }
        break;
    case DESK_DRAG_BEGIN:
        /* fx-19: only a desktop-launched Files window that holds the button
         * it reports (it received the press) can start a shell drag. */
        if (d->shell && !d->sh.gate && !d->power.state && c->window && c->program==DESK_APP_FILES && d->focus==i &&
            (d->routed_buttons & (p.flags==2 ? 2u : 1u)))
            (void)desk_shell_drag_begin(d,i,p.flags,p.count);
        break;
    case DESK_DRAG_STATUS:
        if (d->shell && c->program==DESK_APP_FILES) desk_shell_drag_status(d,i,p.serial,p.flags,p.values[0]);
        break;
    case DESK_ACCEPT_FILES:
        c->accept_files=(int)p.flags; break;
    case DESK_SET_CURSOR:
        c->cursor=p.flags; desk_cursor_update(d); break;
    case DESK_SET_TITLE:
        if (strcmp(c->title, p.title)) {
            memcpy(c->title, p.title, sizeof(c->title));
            desk_caption_damage(d,i);
        }
        break;
    case DESK_MOVE: desk_move(d, i, p.x, p.y); break;
    case DESK_CLOSE_ACK:
        /* G3 (Windows 9x WM_CLOSE): no close deadline. A declined close
         * (the user cancelled the program's own prompt) ends the request;
         * LATER keeps it while the program answers PINGs (the hung check in
         * desk_close_timeouts); a running DOS program gets the Windows 98
         * "cannot shut down this program automatically" warning. */
        if (c->window && c->closing && !d->power.state) {
            if (p.flags==DESK_CLOSE_DECLINED) {
                for (int k=0;k<DESK_CLIENTS;k++) if (d->clients[k].pid==c->pid && (k==i || c->pid) && d->clients[k].closing) {
                    d->clients[k].closing=0; d->clients[k].close_sent_ns=0; desk_caption_damage(d,k);
                    if (d->end_task.kind && !d->end_task.session && d->end_task.target==d->clients[k].window_id) desk_end_task_choice(d,-1);
                }
            } else if (p.flags==DESK_CLOSE_DOS_ACTIVE && !desk_session_busy(d))
                desk_end_task_open(d,DESK_HANG_DOS,0,c->pid,c->window_id,c->title);
        }
        break;
    case DESK_POWER_REPLY:
        if (!p.serial) c->power_aware=1;
        else desk_power_reply(d,i,p.serial,p.flags);
        break;
    case DESK_SESSION_ANSWER:
        if (!p.serial) c->session_aware=1;     /* registration: this window handles queries */
        else session_answer(d,i,p.serial,p.flags);
        break;
    case DESK_CLOSE:
        desk_message(&reply, DESK_CLOSED, 4);
        if (c->version==DESK_SHELL_VERSION) reply.data[0]=DESK_SHELL_VERSION;
        reply.data[2]=(uint8_t)c->minor;
        d->ops.send(c->channel, &reply, DONTWAIT);
        desk_drop_client(d, i);
        break;
    case DESK_PING:
        desk_message(&reply, DESK_PONG, 8); desk_put32(reply.data + 4, p.serial);
        desk_queue(d, i, &reply);
        /* App scenario milestones (Files, CiukNote) arrive in order after their frame's DAMAGE. */
        if (d->app_test && d->app_pid && c->pid == d->app_pid && d->app_checkpoint < DESK_APP_CHECKPOINTS(d->app_test) &&
            p.serial == DESK_APP_CHECKPOINT(d->app_test) + (uint32_t)d->app_checkpoint + 1) d->app_checkpoint++;
        /* Private f2-33 scenario checkpoint (apps/desktop/gate.h): Tasks saw
         * the ended PID leave the kernel process list. */
        if (d->tasks_test && c->pid == d->tasks_pid && c->taskman && p.serial == DESK_TASKS_CHECKPOINT &&
            d->tasks_stage >= 5) d->tasks_checkpoint = 1;
        break;
    case DESK_WINDOWS_QUERY:
        if (c->taskman || c->sysapp) desk_task_windows(d, i, p.serial);
        else {
            desk_message(&reply, DESK_TASK_RESULT, 16); desk_put32(reply.data + 4, p.serial);
            desk_put32(reply.data + 8, DESK_WINDOWS_QUERY); desk_put32(reply.data + 12, (uint32_t)-EPERM);
            desk_queue(d, i, &reply);
        }
        break;
    case DESK_END_TASK: case DESK_SWITCH_TO: case DESK_LAUNCH: {
        /* The Task Manager holds every request. Files, which the desktop itself launched, may only
         * LAUNCH /Programs/ciuknote with one absolute path argument (opening a text file). */
        int files_open = p.opcode == DESK_LAUNCH && c->program == DESK_APP_FILES && !c->taskman &&
                         (!strcmp(p.title, "ciuknote") || desk_catalog_takes_file(p.title)) && p.argument[0] == '/';
        int shell_open = p.opcode==DESK_LAUNCH && c->program==DESK_APP_FILES && !c->taskman &&
                         !strcmp(p.title,"shell-open") && p.argument[0]=='/';
        /* G28: CiukNote hands a document above its limit to WordPad (one absolute path argument). */
        int note_wordpad = p.opcode == DESK_LAUNCH && c->program == DESK_APP_NOTE && !c->taskman &&
                           !strcmp(p.title, "wordpad") && p.argument[0] == '/';
        int ide_run = p.opcode == DESK_LAUNCH && c->program == DESK_APP_IDE && !c->taskman &&
                      !strcmp(p.title,"ciukide-run");
        int ide_guide = p.opcode == DESK_LAUNCH && c->program == DESK_APP_IDE && !c->taskman &&
                        !strcmp(p.title,"ciuknote") && !strcmp(p.argument,"/Documents/Developer/Guide.txt");
        char file_types[40];snprintf(file_types,sizeof(file_types),"--file-action=%d:",FILES_TYPES);
        int panel_launch=p.opcode==DESK_LAUNCH && c->program==DESK_APP_CONTROL &&
                         ((!strcmp(p.title,"control") && (!strcmp(p.argument,"--panel=display") || !strcmp(p.argument,"--panel=power"))) ||
                          (!strcmp(p.title,"files") && !strcmp(p.argument,file_types)));
        int install=p.opcode==DESK_LAUNCH && c->program==DESK_APP_CONTROL && !strcmp(p.title,"install") && p.argument[0];
        /* G15: every program's F1 and Help menu opens CiukiOS Help (topic:<id> or no argument). */
        int help_open = p.opcode == DESK_LAUNCH && !c->taskman && !strcmp(p.title, "help") && desk_help_argument(p.argument);
        int browser_wallpaper=netsurf_wallpaper_request(c,&p);
        int terminal = c->program == DESK_APP_TERMINAL;
        int terminal_launch = terminal && p.opcode == DESK_LAUNCH && desk_program_graphical(p.title) &&
                              (!p.argument[0] || p.argument[0] == '/');
        int terminal_end = terminal && p.opcode == DESK_END_TASK && !p.window_id && p.pid;
        int scheduled_run = c->sysapp && !strcmp(c->application, "schedtask") && p.opcode == DESK_LAUNCH &&
            !strcmp(p.title, "schedtask") && !strncmp(p.argument, "run:", 4) && p.argument[4];
        int sysapp_launch = c->sysapp && p.opcode == DESK_LAUNCH && desk_program_graphical(p.title) && !p.argument[0];
        /* G14: WIN at a Command prompt only start opens the normal desktop. */
        int terminal_win = terminal && p.opcode == DESK_LAUNCH && !strcmp(p.title, "win") && !p.argument[0];
        /* fx-8: a queued launch owes this TASK_RESULT; it is sent when the
         * launcher worker completes, to this requester only. */
        d->launch_deferred = 0;
        if (p.opcode == DESK_LAUNCH) {
            desk_launch_origin(d, DESK_LAUNCH_CLIENT, NULL);
            struct desk_launch_request *r = &d->launch_request;
            r->client = i; r->channel = c->channel; r->pid = c->pid; r->serial = p.serial; r->opcode = p.opcode;
            r->recent = files_open; snprintf(r->argument, sizeof(r->argument), "%s", p.argument);
        }
        int32_t result = terminal_win ? (desk_recovery_win ? desk_recovery_win(d) : -ENOSYS) :
            !c->taskman && !files_open && !shell_open && !note_wordpad && !ide_run && !ide_guide && !terminal_launch && !terminal_end && !install && !panel_launch && !help_open && !sysapp_launch && !scheduled_run && !browser_wallpaper ? -EPERM :
            p.opcode == DESK_END_TASK ? (terminal_end && p.pid == 1 ? -EPERM : desk_task_end(d, p.window_id, p.pid)) :
            p.opcode == DESK_SWITCH_TO ? desk_task_switch(d, p.window_id) :
            shell_open ? (d->power.state ? -EBUSY : d->open_path ? d->open_path(d,p.argument) : -ENOSYS) :
            install ? (d->power.state ? -EBUSY : d->install_program ? d->install_program(d,p.argument) : -ENOSYS) :
            scheduled_run ? (desk_schedsvc_run ? desk_schedsvc_run(d, p.argument + 4) : -ENOSYS) :
            c->taskman && p.argument[0] ? -EPERM : ide_run ? desk_ide_launch(d,p.argument) : desk_task_launch(d, p.title, p.argument);
        int deferred = p.opcode == DESK_LAUNCH && !result && d->launch_deferred;
        memset(&d->launch_request, 0, sizeof(d->launch_request)); d->launch_deferred = 0;
        /* Outside the desktop's child group use its existing kernel authority.
         * Protect PID 1 and the desktop even if a provider is misconfigured. */
        if (terminal_end && result == -EPERM && p.pid != 1 && p.pid != d->self_pid && !d->power.state)
            result = !d->terminate ? -ENOSYS : d->terminate(p.pid, SIGKILL) ? -(errno ? errno : EIO) : 0;
        if (p.opcode == DESK_END_TASK && !result) desk_sound(d,SOUND_END_TASK);
        /* Documents menu: files opened from Files (never in the runtime gate scenarios). */
        if (files_open && result > 0 && d->shell && !d->sh.gate) { cfg_recent_add(&d->sh.cfg, p.argument); d->sh.want_save = 1; }
        if (d->tasks_test && c->taskman && c->pid == d->tasks_pid) {
            if (p.opcode == DESK_LAUNCH && result > 0 && d->tasks_stage == 2 && !strcmp(p.title, "demo")) {
                d->tasks_demo_pid = (uint32_t)result; d->tasks_stage = 3;
            }
            if (p.opcode == DESK_END_TASK && !result && d->tasks_stage == 4 &&
                p.window_id == d->tasks_demo_window) d->tasks_stage = 5;
        }
        if (deferred) break;
        /* The launcher may have reused this slot's neighbours; c stays valid. */
        desk_message(&reply, DESK_TASK_RESULT, 16); desk_put32(reply.data + 4, p.serial);
        desk_put32(reply.data + 8, p.opcode); desk_put32(reply.data + 12, (uint32_t)result);
        desk_queue(d, i, &reply);
        break;
    }
    case DESK_TERMINAL_POWER: {
        int result = c->program != DESK_APP_TERMINAL || !d->shell || d->sh.gate ? -EPERM : desk_session_busy(d) ? -EBUSY : 0;
        desk_message(&reply, DESK_TASK_RESULT, 16); desk_put32(reply.data + 4, p.serial);
        desk_put32(reply.data + 8, DESK_TERMINAL_POWER); desk_put32(reply.data + 12, (uint32_t)result);
        desk_queue(d, i, &reply);
        if (!result) desk_session_begin(d, (int)p.flags);   /* G3: programs are asked first */
        break;
    }
    case DESK_JOYSTICK: {
        int result=c->program!=DESK_APP_CONTROL || !d->shell || d->sh.gate ? -EPERM :
            d->power.state ? -EBUSY : d->joystick_control ? d->joystick_control(d,&p.joystick) : -ENODEV;
        desk_message(&reply,DESK_JOYSTICK_RESULT,112); reply.data[0]=DESK_SHELL_VERSION;
        desk_put32(reply.data+4,p.serial); memcpy(reply.data+8,&p.joystick,sizeof(p.joystick));
        desk_put32(reply.data+108,(uint32_t)result); desk_queue(d,i,&reply); break;
    }
    case DESK_INPUT_SETTINGS:
        if (!input_setting(d,&p)) input_state(d,i,0);
        break;
    case DESK_SETTINGS: {
        /* Control Panel settings use the real display callback and share
         * the shutdown guard with the other preference requests. */
        /* G3: Task Manager may open the Shut Down dialog (its File > Shut Down). */
        int taskman_power = c->taskman && p.request == DESK_SETTINGS_OPEN_POWER;
        int32_t result = (c->program != DESK_APP_CONTROL && !taskman_power) || !d->shell ? -EPERM : desk_session_busy(d) ? -EBUSY : 0;
        if (!result && p.request == DESK_SETTINGS_OPEN_POWER && desk_internal_open(d,DESK_INTERNAL_SHUTDOWN)<0) result=-ENOMEM;
        if (!result && p.request==DESK_SETTINGS_SAVER_PREVIEW) result=desk_saver_start(d,p.values[0]);
        if (!result && p.request == DESK_SETTINGS_RELOAD) d->sh.want_reload |= 1;
        if (!result && p.request == DESK_SETTINGS_PROGRAMS) {d->sh.want_reload |= 3;d->sh.want_programs=1;d->v3_links_next=0;d->v3_links_force|=DESK_LINKS_SCAN|DESK_LINKS_GAMES;}
        if (!result && (p.request == DESK_SETTINGS_DISPLAY || p.request == DESK_SETTINGS_DISPLAY_PREVIEW)) {
            if (d->display_reply_mode) result=-EBUSY;
            else result = d->display_set ? d->display_set(d, (uint16_t)p.values[0]) : -ENOSYS;
            if (!result && d->display_pending) {
                d->display_reply_client=i; d->display_reply_pid=c->pid;
                d->display_reply_serial=p.serial; d->display_reply_mode=(uint16_t)p.values[0];
                d->display_reply_preview=p.request==DESK_SETTINGS_DISPLAY_PREVIEW;
                break; /* reply only after the kernel accepts the complete new frame */
            }
        }
        if (!result && p.request == DESK_SETTINGS_MASTER) {
            if (d->mixer_fd < 0) result = -ENODEV;
            else {
                d->sh.volume = (int)((p.values[0] + p.values[1]) / 2); d->sh.muted = (int)p.values[2];
                d->sh.want_mixer = 1;
                desk_damage_add(&d->damage, desk_taskbar(d), d->width, d->height);
            }
        }
        if (!result && (p.request == DESK_SETTINGS_SOUND_RELOAD || p.request == DESK_SETTINGS_SOUND_TEST ||
                        p.request == DESK_SETTINGS_AUDIO_TEST))
            result = d->sound_request ? d->sound_request(d,i,p.request,p.values[0],p.serial) : -ENODEV;
        if (result == DESK_SOUND_PENDING) break; /* hardware completion replies asynchronously */
        desk_message(&reply, DESK_TASK_RESULT, 16); desk_put32(reply.data + 4, p.serial);
        desk_put32(reply.data + 8, DESK_SETTINGS); desk_put32(reply.data + 12, (uint32_t)result);
        desk_queue(d, i, &reply);
        break;
    }
    case DESK_SOUND:
        desk_sound(d,p.flags);
        break;
    case DESK_MIXER_SET: {
        /* Desktop-launched Volume Control, Music Player and Terminal use the master control handle. */
        int32_t result = !c->mixer ? -EPERM : d->power.state ? -EBUSY : p.values[0] ? 0 : d->mixer_set ? d->mixer_set(d, p.width, p.height, p.flags) : -ENODEV;
        if (p.values[0] && c->mixer && !d->power.state) {
            struct ciuki_mixer_line line={.version=1,.size=sizeof(line),.line=p.values[0],.recording=p.values[1],
                .left=p.width,.right=p.height,.mute=p.values[1] ? 0 : p.flags,.selected=p.values[1] ? p.flags : 0};
            result=d->mixer_line ? d->mixer_line(d,&line) : -ENODEV;
        }
        desk_message(&reply, DESK_TASK_RESULT, 16); desk_put32(reply.data + 4, p.serial);
        desk_put32(reply.data + 8, DESK_MIXER_SET); desk_put32(reply.data + 12, (uint32_t)result);
        desk_queue(d, i, &reply);
        break;
    }
    case DESK_PONG:
        session_pong(d,i,p.serial);
        if (c->ping_pending && p.serial == c->serial) {
            c->ping_pending = c->ping_queued = 0; c->next_ping_ns = d->now_ns + 1000000000ull;
            if (c->unresponsive) {
                c->unresponsive = 0;
                desk_damage_add(&d->damage, desk_outer(c), d->width, d->height);
                if (d->shell) desk_damage_add(&d->damage, desk_task_button(d, i), d->width, d->height);
            }
        }
        break;
    default: break;
    }
}
static void v3_restore_poll(struct desktop *d,uint64_t now)
{
    if (d->v3_restoring) {
        uint64_t elapsed=now-d->v3_restore_started;
        int complete=!d->v3_lease_state && d->presents>d->v3_restore_presents && !d->damage.count && !d->present_damage.count;
        if (complete || (!d->v3_restore_reported && elapsed>=2000000000ull)) {
            fprintf(stderr,"[win32] display restore elapsed_ms=%llu result=%d\n",
                (unsigned long long)(elapsed/1000000),complete && elapsed<=2000000000ull ? 0 : -ETIMEDOUT);
            d->v3_restore_reported=1;
        }
        if (complete) {d->v3_restoring=0;__atomic_store_n(&d->v3_restore_watchdog_pending,0,__ATOMIC_RELEASE);}
    }
}
/* G10: no input polling; heartbeat plus the explicit idle/frame deadline. */
static void saver_damage(struct desktop *d)
{
    d->copy_pending=0; d->scanout_valid=d->frame_valid=d->cursor_valid=0;
    d->cursor_dirty=1;
    desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,(int)d->height},d->width,d->height);
}
static int saver_suppressed(const struct desktop *d)
{
    if (!d->shell || d->sh.gate || d->saver_external_running || d->saver_display_inhibited || desk_session_busy(d) || d->v3_lease_state ||
        d->v3_restoring || d->drag>=0 || d->sh.dnd.active) return 1;
    if (d->focus>=0) {
        const struct desk_client *c=&d->clients[d->focus];
        if (c->window && !c->minimized && c->frame.x<=0 && c->frame.y<=0 &&
            c->frame.w>=(int)d->width && c->frame.h>=(int)d->height) return 1;
    }
    return 0;
}
void desk_saver_config(struct desktop *d)
{
    if (!d->screen_control) return;
    struct ciuki_screen_control q={.version=CIUKI_SCREEN_VERSION,.size=sizeof(q),
        .action=CIUKI_SCREEN_CONFIG,.enabled=(uint32_t)d->sh.cfg.saver_enabled,
        .timeout=(uint32_t)d->sh.cfg.saver_seconds,.saver=(uint32_t)d->sh.cfg.saver_kind,
        .monitor_seconds=(uint32_t)d->sh.cfg.monitor_seconds};
    if (!d->screen_control(&q)) d->saver_poll=0;
    d->saver_idle=d->now_ns; d->monitor_failed=0;
}
int desk_saver_start(struct desktop *d, unsigned kind)
{
    if (kind>2) return -EINVAL;
    if (d->screen_control) {
        struct ciuki_screen_control q={.version=CIUKI_SCREEN_VERSION,.size=sizeof(q)};
        int e=d->screen_control(&q);
        if (e) return e;
        d->saver_display_inhibited=q.inhibited!=0;
    }
    if (saver_suppressed(d) || d->monitor_off) return -EBUSY;
    d->saver_active=1; d->saver_poll=0; d->saver_kind=kind; d->saver_frame=d->now_ns;
    saver_damage(d); return 0;
}
static int saver_wake(struct desktop *d)
{
    if (d->monitor_off) {
        int e=d->monitor_power ? d->monitor_power(-1) : -ENOSYS;
        if (e) return e;
        d->monitor_off=0; saver_damage(d);
    }
    if (d->saver_active) { d->saver_active=0; saver_damage(d); }
    return 0;
}
int desk_saver_input(struct desktop *d, const struct ciuki_input_event *e)
{
    if (e->type==CIUKI_INPUT_RESYNC) return 0;
    if (e->type==CIUKI_INPUT_MOTION && !e->value && !e->value2) return 0;
    int consumed=d->saver_active || d->monitor_off;
    d->saver_idle=d->now_ns; d->monitor_failed=0;
    if (consumed) {
        (void)saver_wake(d);
        /* RESYNC cancels held keys/buttons, including keys pressed before
         * a forced saver start. The dismissing click never reaches an app. */
        struct ciuki_input_event reset={.type=CIUKI_INPUT_RESYNC};
        desk_input(d,&reset);
        d->saver_poll=0;
    }
    return consumed;
}
void desk_saver_tick(struct desktop *d)
{
    if (!d->shell) return;
    if (!d->saver_idle) d->saver_idle=d->now_ns;
    if (saver_suppressed(d)) {
        d->saver_idle=d->now_ns;
        (void)saver_wake(d);
    }
    if (d->screen_control && d->now_ns>=d->saver_poll) {
        struct ciuki_screen_control q={.version=CIUKI_SCREEN_VERSION,.size=sizeof(q)};
        d->saver_poll=d->now_ns+1000000000ull;
        if (!d->screen_control(&q)) {
            d->saver_display_inhibited=q.inhibited!=0;
            d->saver_external_running=q.running && !d->saver_active && !d->saver_reported;
            d->sh.cfg.saver_enabled=(int)q.enabled; d->sh.cfg.saver_seconds=(int)q.timeout;
            d->sh.cfg.saver_kind=(int)q.saver; d->sh.cfg.monitor_seconds=(int)q.monitor_seconds;
            if (q.sequence!=d->saver_sequence) {
                d->saver_sequence=q.sequence;
                if (q.flags&CIUKI_SCREEN_PERSIST) d->sh.want_save=1;
                if (q.command==0xf140) (void)desk_saver_start(d,q.argument<0 ? q.saver : (unsigned)q.argument);
                if (q.command==0xf170 && !saver_suppressed(d)) {
                    if (q.argument==-1) (void)saver_wake(d);
                    else if (d->monitor_power) {
                        int e=d->monitor_power(q.argument);
                        if (!e) d->monitor_off=1;
                    }
                }
            }
            q.action=CIUKI_SCREEN_STATUS;
            q.value=(int)(d->saver_active!=d->saver_reported ? d->saver_active : q.running);
            d->saver_reported=d->saver_active;
            (void)d->screen_control(&q);
        }
    }
    if (saver_suppressed(d)) return;
    uint64_t idle=d->now_ns>=d->saver_idle ? d->now_ns-d->saver_idle : 0;
    if (!d->saver_active && d->sh.cfg.saver_enabled && d->sh.cfg.saver_seconds>0 &&
        idle>=(uint64_t)d->sh.cfg.saver_seconds*1000000000ull)
        (void)desk_saver_start(d,(unsigned)d->sh.cfg.saver_kind);
    if (!d->monitor_off && !d->monitor_failed && d->sh.cfg.monitor_seconds>0 &&
        idle>=(uint64_t)d->sh.cfg.monitor_seconds*1000000000ull) {
        int e=d->monitor_power ? d->monitor_power(2) : -ENOSYS;
        if (!e) d->monitor_off=1;
        else { d->monitor_failed=1; snprintf(d->notice,sizeof(d->notice),"Monitor power is unavailable (%d).",-e); }
    }
    if (d->saver_active && !d->monitor_off && d->now_ns>=d->saver_frame) {
        saver_damage(d); d->saver_frame=d->saver_kind ? d->now_ns+50000000ull : UINT64_MAX;
    }
}

void desk_heartbeat(struct desktop *d, uint64_t now)
{
    d->now_ns = now;
    desk_saver_tick(d);
    if (d->v3_lease_poll) d->v3_lease_poll(d);
    if (d->v3_lease_state==2) desk_v3_revoke(d);
    v3_restore_poll(d,now);
    if(d->v3_reactivate_pending && !d->v3_restoring) {
        int i=d->v3_reactivate_pending-1;
        if(d->focus==i)desk_v3_reactivate(d,i);else d->v3_reactivate_pending=0;
    }
    if (now / 1000000000ull != d->clock_second) {
        d->clock_second = now / 1000000000ull;
        if (!d->shell) desk_damage_add(&d->damage, (struct desk_box){(int32_t)d->width - 120,
            0, 120, DESK_BAR}, d->width, d->height);
    }
    for (int i = 0; i < DESK_CLIENTS; i++) {
        struct desk_client *c = &d->clients[i];
        if (c->channel >= 0 && c->version==DESK_V3_VERSION && !c->v3_peer) v3_check_silent(d,i);
    }
    for (int i = 0; i < DESK_CLIENTS; i++) {
        struct desk_client *c = &d->clients[i];
        if (c->channel < 0 || !c->hello || (c->version==DESK_V3_VERSION && !c->window)) continue;
        if (!c->ping_pending && now >= c->next_ping_ns && c->tx_count < DESK_TX_MAX) {
            struct ciuki_message m;
            desk_message(&m, DESK_PING, 8); desk_put32(m.data + 4, ++c->serial);
            desk_queue(d, i, &m); c->ping_pending = 1; c->ping_queued = 1; c->ping_sent_ns = now;
            session_ping_sent(d,i);
        }
        /* One rule for every application: a PING unanswered for 5 s marks the window, but only when the desktop
         * itself drained the channels after that deadline. A desktop-owned wait (a synchronous VFS lookup in a
         * launch, a present, a worker mailbox) leaves the PONG unread in the channel and must not be charged
         * to the application (f2-88). */
        if (c->ping_pending && !c->ping_queued && now >= c->ping_sent_ns && now - c->ping_sent_ns >= 5000000000ull && !c->unresponsive &&
            (!d->drain_ns || d->drain_ns >= c->ping_sent_ns + 5000000000ull)) {
            c->unresponsive = 1;
            if (c->window) desk_damage_add(&d->damage, desk_outer(c), d->width, d->height);
            if (c->window && d->shell) desk_damage_add(&d->damage, desk_task_button(d, i), d->width, d->height);
        }
    }
    if (d->shell) desk_close_timeouts(d);
    if (d->session.state) desk_session_tick(d);
    desk_animation_tick(d); desk_cursor_update(d);
}

/* A bounded /Programs entry parser, independent of directory enumeration order.
 * This is an admission filter, never a replacement for kernel ELF validation. */
int desk_program_add(struct desktop *d, const char *name, const uint8_t *elf, size_t n)
{
    size_t len = 0;
    if (!name || !elf) return 0;
    while (name[len] && len < 64) {
        unsigned char c = (unsigned char)name[len++];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) return 0;
    }
    if (!len || len == 64 || name[0] == '.' ||
        n < 52 || memcmp(elf,"\177ELF\1\1\1",7) ||
        elf[16] != 2 || elf[17] || elf[18] != 3 || elf[19] ||
        desk_get32(elf+20) != 1 || elf[40] != 52 || elf[41]) return 0;
    char lower[64];
    for (size_t k=0; k<=len; k++) lower[k] = name[k]>='A' && name[k]<='Z' ? name[k]+32 : name[k];
    if (!strcmp(lower,"desktop")) return 0;
    unsigned at = 0;
    while (at < d->program_count && strcmp(d->programs[at].name,name) < 0) at++;
    if (at < d->program_count && !strcmp(d->programs[at].name,name)) return 0;
    if (at >= DESK_MENU_MAX) return 0;
    unsigned count = d->program_count < DESK_MENU_MAX ? d->program_count+1 : DESK_MENU_MAX;
    memmove(d->programs+at+1,d->programs+at,(count-at-1)*sizeof(d->programs[0]));
    struct desk_program *p = &d->programs[at];
    memcpy(p->name,name,len+1);
    snprintf(p->path,sizeof(p->path),"/Programs/%s",name);
    d->program_count = count;
    return 1;
}
void desk_shell_enable(struct desktop *d)
{
    if (d->shell) return;
    d->shell = 1;
    d->sh.autoplay.workspace=ap_workspace_create();
    /* desktop.c loads desktop.cfg first; otherwise the built-in defaults. */
    if (!d->sh.cfg.theme[0]) desk_shell_reset(d);
    d->sh.icon_selected = d->sh.click_icon = -1;
    for (int i=0; i<DESK_CLIENTS; i++) if (d->clients[i].window)
        desk_move(d,i,d->clients[i].frame.x,d->clients[i].frame.y);
    desk_shell_apply(d);
    desk_damage_add(&d->damage,(struct desk_box){0,0,(int32_t)d->width,(int32_t)d->height},d->width,d->height);
}
void desk_minimize(struct desktop *d, int i)
{
    if (!d->shell || i<0 || i>=DESK_CLIENTS || !d->clients[i].window) return;
    struct desk_client *c = &d->clients[i];
    int modal=desk_modal_target(d,i);
    if (modal!=i) { desk_focus(d,modal); return; }
    if (c->owner_id) return;
    if(d->drag==i && d->geometry_outline) desk_geometry_end(d,1);
    struct desk_box from=c->minimized ? desk_task_button(d,i) : c->frame;
    desk_sound(d,c->minimized ? SOUND_RESTORE : SOUND_MINIMISE);
    if (c->minimized) { c->minimized=0; desk_focus(d,i); }
    else {
        c->minimized=1;
        if (d->drag==i) d->drag=-1;
        if (d->focus==i) {
            int next=desk_next_window(d,i,c->pid,c->application);
            desk_focus(d,next);
        }
    }
    desk_damage_add(&d->damage,desk_outer(c),d->width,d->height);
    desk_damage_add(&d->damage,desk_taskbar(d),d->width,d->height);
    if (c->version==DESK_V3_VERSION) v3_configure(d,i);
    desk_animate(d,from,c->minimized ? desk_task_button(d,i) : c->frame);
    desk_cursor_update(d);
}
/* ---- G4: taskbar window arrangement (Windows 98 CascadeWindows, TileWindows,
 * Minimize All Windows, Show Desktop and their Undo) ---------------------- */
static struct desk_box arrange_work(const struct desktop *d)
{
    /* The area between the top bar and the taskbar: windows never cover a bar. */
    return desk_work_area(d);
}
/* Which windows an arrangement moves. Cascade and tile leave alone what
 * Windows leaves alone: minimised windows, owned windows (dialogs and
 * Win32 popups stay with their owner), windows with an owned window open,
 * built-in dialogs, Win32 popup and tool windows, topmost windows. */
int desk_arrangeable(const struct desktop *d, int i)
{
    if (!d->shell || i<0 || i>=DESK_CLIENTS) return 0;
    const struct desk_client *c=&d->clients[i];
    if (!c->window || c->minimized || c->internal || c->owner_id || !desk_task_visible(c) || c->closing) return 0;
    if (c->version==DESK_V3_VERSION && ((c->v3_style&3)!=DESK_V3_OVERLAPPED || (c->v3_style&DESK_V3_TOPMOST))) return 0;
    for (int k=0;k<DESK_CLIENTS;k++)
        if (k!=i && d->clients[k].window && d->clients[k].owner_id==c->window_id) return 0;
    return 1;
}
static void arrange_forget(struct desktop *d) { memset(&d->arrange,0,sizeof(d->arrange)); }
static void arrange_save(struct desktop *d, int kind)
{
    arrange_forget(d);
    d->arrange.kind=kind;
    if (d->focus>=0 && d->clients[d->focus].window) d->arrange.focus_id=d->clients[d->focus].window_id;
}
static void arrange_remember(struct desktop *d, int i)
{
    const struct desk_client *c=&d->clients[i];
    struct desk_arrange_saved *s=&d->arrange.windows[d->arrange.count++];
    s->window_id=c->window_id; s->frame=c->frame; s->restore=c->restore; s->shade_height=c->shade_height;
    s->minimized=(uint8_t)c->minimized; s->zoomed=(uint8_t)c->zoomed; s->collapsed=(uint8_t)c->collapsed;
}
static int window_by_id(const struct desktop *d, uint32_t id)
{
    for (int i=0;id && i<DESK_CLIENTS;i++) if (d->clients[i].window && d->clients[i].window_id==id) return i;
    return -1;
}
/* Puts one window at `frame` inside the work area. A maximised or shaded
 * window is first restored (Windows restores before it arranges); a window
 * that cannot be resized keeps its size and takes the cell's corner. */
static void arrange_place(struct desktop *d, int i, struct desk_box frame)
{
    struct desk_client *c=&d->clients[i];
    if (c->collapsed) desk_collapse(d,i);
    if (c->zoomed) {
        c->zoomed=0;
        desk_damage_add(&d->damage,desk_outer(c),d->width,d->height);
    }
    if (c->resizable) desk_resize(d,i,frame);   /* limits, steps and the work area apply */
    desk_move(d,i,frame.x,frame.y);                /* and keeps a window that cannot shrink inside it */
}
static void arrange_settle(struct desktop *d)
{
    /* The pending single-window copy of desk_move describes one move; an
     * arrangement moves many windows, so every box is repainted. */
    if (d->copy_pending) {
        for (unsigned k=0;k<d->move_damage.count;k++)
            desk_damage_add(&d->damage,d->move_damage.boxes[k],d->width,d->height);
        d->move_damage.count=0; d->copy_pending=0;
    }
    desk_damage_add(&d->damage,desk_taskbar(d),d->width,d->height);
    desk_cursor_update(d);
}
static int arrange_prepare(struct desktop *d)
{
    if (!d->shell || d->power.state) return 0;
    if (d->keyboard_geometry || d->geometry_outline) desk_geometry_end(d,1);
    d->drag=-1; d->resize_edges=0;
    if (d->copy_pending) {
        for (unsigned k=0;k<d->move_damage.count;k++)
            desk_damage_add(&d->damage,d->move_damage.boxes[k],d->width,d->height);
        d->move_damage.count=0; d->copy_pending=0;
    }
    return 1;
}
/* Windows are ordered by the stacking: bottom to top. */
static int arrange_list(const struct desktop *d, int *list, int top_first)
{
    int n=0;
    for (int i=0;i<DESK_CLIENTS;i++) if (desk_arrangeable(d,i)) list[n++]=i;
    for (int a=1;a<n;a++) {
        int key=list[a], b=a;
        while (b && (top_first ? d->clients[list[b-1]].order<d->clients[key].order :
                                 d->clients[list[b-1]].order>d->clients[key].order)) { list[b]=list[b-1]; b--; }
        list[b]=key;
    }
    return n;
}
/* CascadeWindows: every window at three quarters of the work area (a window
 * limited by its own minimum or maximum keeps that size), each offset by one
 * caption height down and to the right of the one beneath it; the sequence
 * starts over at the top left when the next offset would leave the work area.
 * The stacking is not changed, so the window that was in front stays in
 * front, at the end of the cascade. */
static int arrange_cascade(struct desktop *d)
{
    int list[DESK_CLIENTS], n=arrange_list(d,list,0);
    if (!n) return 0;
    struct desk_box w=arrange_work(d);
    int32_t step=DESK_TITLE, cw=w.w*3/4, ch=w.h*3/4;
    arrange_save(d,DESK_ARRANGE_CASCADE);
    for (int a=0;a<n;a++) arrange_remember(d,list[a]);
    int32_t wrap=0;   /* positions that fit before the cascade starts over */
    {
        int32_t fx=(w.w-cw)/step+1, fy=(w.h-ch)/step+1;
        wrap=fx<fy ? fx : fy;
        if (wrap<1) wrap=1;
    }
    for (int a=0;a<n;a++) {
        int32_t k=a%wrap;
        arrange_place(d,list[a],(struct desk_box){w.x+k*step,w.y+k*step,cw,ch});
    }
    return n;
}
/* TileWindows: up to three windows share one strip (MDITILE_HORIZONTAL
 * stacks them, MDITILE_VERTICAL puts them side by side); more windows fill a
 * grid of about ceil(sqrt(n)) by ceil(n/ceil(sqrt(n))) cells, the shorter
 * line on the side of the tiling direction, and the windows of a short last
 * line share its width. The window in front takes the first cell. */
static int arrange_tile(struct desktop *d, int vertical)
{
    int list[DESK_CLIENTS], n=arrange_list(d,list,1);
    if (!n) return 0;
    struct desk_box w=arrange_work(d);
    int cols, rows;
    if (n<=3) { cols=vertical ? n : 1; }
    else {
        int a=1; while (a*a<n) a++;
        int b=(n+a-1)/a;
        cols=vertical ? a : b;
    }
    rows=(n+cols-1)/cols;
    arrange_save(d,vertical ? DESK_ARRANGE_TILE_V : DESK_ARRANGE_TILE_H);
    for (int a=0;a<n;a++) arrange_remember(d,list[a]);
    int index=0;
    for (int r=0;r<rows;r++) {
        int count=r<rows-1 ? cols : n-(rows-1)*cols;
        int32_t y0=w.y+w.h*r/rows, y1=w.y+w.h*(r+1)/rows;
        for (int cidx=0;cidx<count;cidx++,index++) {
            int32_t x0=w.x+w.w*cidx/count, x1=w.x+w.w*(cidx+1)/count;
            arrange_place(d,list[index],(struct desk_box){x0,y0,x1-x0,y1-y0});
        }
    }
    return n;
}
int desk_arrange(struct desktop *d, int kind)
{
    if (!arrange_prepare(d)) return 0;
    int n=0;
    switch (kind) {
    case DESK_ARRANGE_CASCADE: n=arrange_cascade(d); break;
    case DESK_ARRANGE_TILE_H: n=arrange_tile(d,0); break;
    case DESK_ARRANGE_TILE_V: n=arrange_tile(d,1); break;
    default: return 0;
    }
    if (n) arrange_settle(d);
    return n;
}
/* A window that Minimize All Windows takes to the taskbar. */
int desk_minimizable(const struct desktop *d, int i)
{
    const struct desk_client *c=&d->clients[i];
    if (!c->window || c->minimized || c->owner_id || !desk_task_visible(c)) return 0;
    return c->version!=DESK_V3_VERSION || (c->v3_style&3)!=DESK_V3_POPUP;
}
int desk_minimize_all(struct desktop *d)
{
    if (!arrange_prepare(d)) return 0;
    int mark[DESK_CLIENTS], n=0;
    for (int i=0;i<DESK_CLIENTS;i++) { mark[i]=desk_minimizable(d,i); n+=mark[i]; }
    if (!n) return 0;
    /* Native dialogs owned by a window that goes to the taskbar follow it. */
    for (int i=0;i<DESK_CLIENTS;i++) {
        const struct desk_client *c=&d->clients[i];
        if (c->window && !c->minimized && c->owner_id && c->internal && c->version!=DESK_V3_VERSION) {
            int owner=window_by_id(d,c->owner_id);
            if (owner>=0 && mark[owner]==1) mark[i]=2;
        }
    }
    arrange_save(d,DESK_ARRANGE_MINIMIZE_ALL);
    for (int i=0;i<DESK_CLIENTS;i++) if (mark[i]) arrange_remember(d,i);
    for (int i=0;i<DESK_CLIENTS;i++) if (mark[i]) {
        struct desk_client *c=&d->clients[i];
        c->minimized=1;
        desk_damage_add(&d->damage,desk_outer(c),d->width,d->height);
        if (c->version==DESK_V3_VERSION) v3_configure(d,i);
    }
    if (d->focus>=0 && d->clients[d->focus].minimized) desk_focus(d,-1);
    desk_sound(d,SOUND_MINIMISE);
    arrange_settle(d);
    return n;
}
int desk_arrange_undo_kind(const struct desktop *d)
{
    if (!d->arrange.kind) return 0;
    for (unsigned k=0;k<d->arrange.count;k++) if (window_by_id(d,d->arrange.windows[k].window_id)>=0) return d->arrange.kind;
    return 0;
}
int desk_arrange_undo(struct desktop *d)
{
    if (!desk_arrange_undo_kind(d) || !arrange_prepare(d)) return 0;
    struct desk_arrange_undo undo=d->arrange;
    arrange_forget(d);
    int n=0;
    for (unsigned k=0;k<undo.count;k++) {
        const struct desk_arrange_saved *s=&undo.windows[k];
        int i=window_by_id(d,s->window_id);
        if (i<0) continue;
        struct desk_client *c=&d->clients[i];
        n++;
        if (undo.kind==DESK_ARRANGE_MINIMIZE_ALL) {
            if (c->minimized && !s->minimized) {
                c->minimized=0;
                desk_damage_add(&d->damage,desk_outer(c),d->width,d->height);
                if (c->version==DESK_V3_VERSION) v3_configure(d,i);
            }
            continue;
        }
        /* Cascade and tile: the previous size, place and state. */
        if (c->collapsed!=s->collapsed) desk_collapse(d,i);
        c->zoomed=s->zoomed; c->restore=s->restore;
        if (c->collapsed) c->shade_height=s->shade_height;
        else if (c->resizable && !c->internal) desk_resize(d,i,s->frame);
        desk_move(d,i,s->frame.x,s->frame.y);
    }
    if (undo.kind==DESK_ARRANGE_MINIMIZE_ALL) {
        int f=window_by_id(d,undo.focus_id);
        if (f>=0 && !d->clients[f].minimized) desk_focus(d,f);
        desk_sound(d,SOUND_RESTORE);
    }
    arrange_settle(d);
    return n;
}
/* Win+D and the Show Desktop button: minimise everything; when nothing is
 * left to minimise and the last arrangement was Minimize All, bring the
 * windows back. */
int desk_show_desktop(struct desktop *d)
{
    if (!d->shell) return 0;
    int n=0;
    for (int i=0;i<DESK_CLIENTS;i++) n+=desk_minimizable(d,i);
    if (n) return desk_minimize_all(d);
    if (desk_arrange_undo_kind(d)==DESK_ARRANGE_MINIMIZE_ALL) return desk_arrange_undo(d);
    return 0;
}

void desk_request_close(struct desktop *d, int i)
{
    if (i<0 || i>=DESK_CLIENTS || !d->clients[i].window) return;
    i=desk_modal_target(d,i);
    struct desk_client *c=&d->clients[i];
    if (c->internal==DESK_INTERNAL_TRANSFER) { desk_folder_cancel(d); return; }
    if (c->internal) { desk_drop_client(d,i); return; }
    if (c->closing) return;
    c->closing=1; c->close_sent_ns=d->now_ns ? d->now_ns : 1;
    struct ciuki_message m;
    desk_message(&m,c->version>=DESK_SHELL_VERSION ? DESK_CLOSE_REQUEST : DESK_CLOSED,4);
    desk_queue(d,i,&m);
    /* G3: only a v1 client (told CLOSED, it cannot prompt) keeps the
     * DESK_CLOSE_NS termination; v2 and v3 programs may ask the user and
     * are watched for hangs instead (desk_close_timeouts). */
    for (int j=0; j<DESK_CLIENTS; j++) if (c->version==DESK_VERSION &&
        d->children[j].pid==c->pid && c->pid && !d->children[j].close_ns) {
        d->children[j].close_ns=d->now_ns+DESK_CLOSE_NS; d->children[j].close_deferred=0;
    }
    if (d->shell_test && d->shell_stage==4 && c->pid==d->shell_pid) d->shell_stage=5;
    desk_damage_add(&d->damage,desk_outer(c),d->width,d->height);
}
void desk_close_timeouts(struct desktop *d)
{
    if (d->power.state) return; /* shutdown owns its three-second deadline */
    for (int i=0; i<DESK_CLIENTS; i++) {
        struct desk_child *c=&d->children[i];
        if (c->pid && c->close_ns && !c->term_sent && d->now_ns>=c->close_ns && d->terminate) {
            c->term_sent=1;
            if (d->terminate(c->pid,SIGTERM) && errno!=ESRCH)
                snprintf(d->notice,sizeof(d->notice),"Close failed (errno %d)",errno);
        }
    }
    /* G3: a window asked to close that stops answering PINGs for the
     * Windows 9x HungAppTimeout gets "This program is not responding" (End
     * Task, Cancel). A responsive program, even one showing its own save
     * prompt, is never terminated. */
    if (d->session.state || d->end_task.kind) return;
    for (int i=0; i<DESK_CLIENTS; i++) {
        struct desk_client *c=&d->clients[i];
        if (!c->window || c->internal || !c->closing || !c->close_sent_ns || c->version==DESK_VERSION || !c->pid ||
            !c->unresponsive || d->now_ns<c->close_sent_ns+DESK_HUNG_NS) continue;
        c->close_sent_ns=0; /* one dialog per close request */
        desk_end_task_open(d,DESK_HANG_NOT_RESPONDING,0,c->pid,c->window_id,c->title);
        return;
    }
}
/* poll() reported POLLNVAL for this slot's channel: the descriptor is not
 * open in this process (closed under the desktop, q3crash 2026-10-25). The
 * client is gone; drop it without close(), since the number may already be
 * another file of ours. Never an exit of the desktop. */
void desk_client_invalid(struct desktop *d, int i)
{
    struct desk_client *c=&d->clients[i];
    if (i<0 || i>=DESK_CLIENTS || c->channel<0) return;
    fprintf(stderr,"desktop: client channel invalid slot=%d fd=%d pid=%u; dropped\n",i,c->channel,c->pid);
    int anchor=c->v3_peer ? (int)c->v3_peer-1 : i;
    if (anchor!=i && d->clients[anchor].channel==c->channel) { d->clients[anchor].channel_invalid=1; desk_drop_client(d,anchor); return; }
    c->channel_invalid=1;
    desk_drop_client(d,i);
}
void desk_child_exited(struct desktop *d, uint32_t pid, int status)
{
    if (desk_schedsvc_exited) desk_schedsvc_exited(d, pid, status);
    if (pid && d->end_task.kind && d->end_task.pid==pid && !d->end_task.session) desk_end_task_choice(d,-1);
    for (unsigned i=0; i<d->power.count; i++)
        if (d->power.targets[i].pid==pid) d->power.targets[i].gone=1;
    int owned=0;
    for (int i=0; i<DESK_CLIENTS; i++) {
        if (d->clients[i].pid==pid && pid) desk_drop_client(d,i);
        if (d->children[i].pid==pid && pid) { owned=1; memset(&d->children[i],0,sizeof(d->children[i])); }
    }
    if (owned && d->shell_test && d->shell_stage==5 && pid==d->shell_pid && !status) d->shell_stage=6;
    if (owned && d->tasks_test && d->tasks_stage==5 && pid==d->tasks_demo_pid && !status) d->tasks_stage=6;
    if (owned && d->app_test && pid==d->app_pid && !status && d->app_checkpoint==DESK_APP_CHECKPOINTS(d->app_test)) d->app_done=1;
}

void desk_about(struct desktop *d)
{ desk_internal_open(d, DESK_INTERNAL_ABOUT); }
/* Kept for the f2-30 tests: a left press at the pointer through the shell. */
int desk_shell_click(struct desktop *d)
{ return desk_shell_button(d, CIUKI_BUTTON_LEFT, 1); }

/* f2-33 task-manager requests. Only a client the desktop itself launched as
 * /Programs/tasks (taskman) reaches these; every result is a TASK_RESULT. */
static int window_slot(const struct desktop *d, uint32_t id)
{
    for (int i=0; i<DESK_CLIENTS; i++) if (id && d->clients[i].window && d->clients[i].window_id==id) return i;
    return -1;
}
void desk_task_windows(struct desktop *d, int index, uint32_t serial)
{
    struct desk_client *requester=&d->clients[index];
    unsigned count=0;
    for (int i=0; i<DESK_CLIENTS; i++) count+=d->clients[i].window!=0;
    struct ciuki_message m;
    if (DESK_TX_MAX-requester->tx_count < (count ? count : 1)) {
        /* Never deliver a partial list: the client retries on its next refresh. */
        desk_message(&m,DESK_TASK_RESULT,16); desk_put32(m.data+4,serial);
        desk_put32(m.data+8,DESK_WINDOWS_QUERY); desk_put32(m.data+12,(uint32_t)-EAGAIN);
        desk_queue(d,index,&m); return;
    }
    if (!count) {
        desk_message(&m,DESK_WINDOW_INFO,32); desk_put32(m.data+4,serial); desk_queue(d,index,&m); return;
    }
    unsigned at=0;
    for (int i=0; i<DESK_CLIENTS; i++) {
        const struct desk_client *c=&d->clients[i];
        if (!c->window) continue;
        char title[DESK_TITLE_MAX+1]; unsigned n=0;
        for (unsigned k=0;k<DESK_TITLE_MAX && c->title[k];k++) {
            unsigned char ch=(unsigned char)c->title[k];
            if (ch<128) title[n++]=(char)ch;
            else if ((ch&0xc0)!=0x80) title[n++]='?';
        }
        if (!n) { memcpy(title,"Windows program",15); n=15; }
        uint32_t flags=(c->unresponsive ? DESK_WINDOW_UNRESPONSIVE : 0) | (c->minimized ? DESK_WINDOW_MINIMIZED : 0) |
            (d->focus==i ? DESK_WINDOW_FOCUSED : 0) | (c->internal ? DESK_WINDOW_INTERNAL : 0) |
            (c->closing ? DESK_WINDOW_CLOSING : 0);
        desk_message(&m,DESK_WINDOW_INFO,32+n);
        desk_put32(m.data+4,serial); desk_put32(m.data+8,at++); desk_put32(m.data+12,count);
        desk_put32(m.data+16,c->window_id); desk_put32(m.data+20,c->internal ? d->self_pid : c->pid);
        desk_put32(m.data+24,flags); desk_put32(m.data+28,n); memcpy(m.data+32,title,n);
        desk_queue(d,index,&m);
    }
}
int desk_task_end(struct desktop *d, uint32_t window_id, uint32_t pid)
{
    if (d->power.state) return -EBUSY;
    if (window_id) {
        int i=window_slot(d,window_id);
        if (i<0 || (pid && d->clients[i].pid!=pid && !d->clients[i].internal)) return -ESRCH;
        struct desk_client *c=&d->clients[i];
        /* G3, Windows 98 Close Program: End Task asks the program to close
         * (WM_CLOSE, with its own save prompt). A program already asked, or
         * one that does not respond, gets "This program is not responding"
         * whose End Task terminates the authenticated host process. */
        if (!c->internal && (c->closing || c->unresponsive)) {
            if (!d->terminate || !c->pid || c->pid==d->self_pid) return -EPERM;
            desk_end_task_open(d,DESK_HANG_NOT_RESPONDING,0,c->pid,c->window_id,c->title);
            return 0;
        }
        desk_request_close(d,i);
        return 0;
    }
    if (!pid || pid==d->self_pid) return -EPERM;
    int owned=0;
    for (int j=0; j<DESK_CLIENTS; j++) owned|=d->children[j].pid==pid;
    if (!owned) return -EPERM;   /* outside the desktop's close path: kernel authority */
    int windows=0;
    for (int i=0; i<DESK_CLIENTS; i++) if (d->clients[i].window && d->clients[i].pid==pid) { desk_request_close(d,i); windows++; }
    if (!windows) {
        for (int j=0; j<DESK_CLIENTS; j++) if (d->children[j].pid==pid) d->children[j].term_sent=1;
        if (!d->terminate || (d->terminate(pid,SIGTERM) && errno!=ESRCH)) return -EPERM;
    }
    return 0;
}
/* G15: the argument of a client's request for CiukiOS Help: none, or
 * "topic:" and a lower-case topic id (apps/accessories/help_topics.c). */
/* G15: the "?" caption button of a DESK_V3_CONTEXTHELP window was pressed:
 * the client starts What's This? (SC_CONTEXTHELP). Only v3 clients ask for the style. */
void desk_context_help(struct desktop *d, int index)
{
    if (index<0 || index>=DESK_CLIENTS || !d->clients[index].window) return;
    struct desk_client *c=&d->clients[index]; struct ciuki_message m;
    if (c->version!=DESK_V3_VERSION || !(c->v3_style&DESK_V3_CONTEXTHELP)) return;
    desk_v3_message(&m,DESK_V3_HELP,0,0,c->v3_id,c->v3_generation);
    desk_queue(d,index,&m);
}
int desk_help_argument(const char *argument)
{
    if (!argument[0]) return 1;
    struct desk_help_request request;
    if (desk_help_decode(argument,&request)) return 1;
    if (strncmp(argument, "topic:", 6)) return 0;
    size_t n = strlen(argument + 6);
    if (!n || n > 40) return 0;
    for (size_t i = 0; i < n; i++) {
        char ch = argument[6 + i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-')) return 0;
    }
    return 1;
}
int desk_task_switch(struct desktop *d, uint32_t window_id)
{
    if (d->power.state) return -EBUSY;
    int i=window_slot(d,window_id);
    if (i<0) return -ESRCH;
    if (d->clients[i].minimized) desk_minimize(d,i); else desk_focus(d,i);
    return 0;
}
static void launch_clear(struct desktop *d)
{
    d->launch_arg=NULL;
    memset(d->launch_argv,0,sizeof(d->launch_argv));
}
/* fx-8: the origin applies to the one launch that follows; every return of
 * the public launchers forgets it, including refusals before d->launch. */
static int launch_done(struct desktop *d, int result)
{
    memset(&d->launch_request,0,sizeof(d->launch_request));
    return result;
}
void desk_launch_origin(struct desktop *d, unsigned origin, const char *label)
{
    memset(&d->launch_request,0,sizeof(d->launch_request));
    d->launch_request.origin=origin;
    if (label) snprintf(d->launch_request.label,sizeof(d->launch_request.label),"%s",label);
}
/* All launch routes replace the entire one-shot payload, including on failure.
 * A previous single-file/page argument and a vector must never be combined. */
static int task_launch(struct desktop *d,const struct desk_program *program,
                       const char *argument,const char *const *args,unsigned count)
{
    launch_clear(d);
    d->launch_deferred=0;
    if (d->power.state) return -EBUSY;
    if (!d->launch) return -ENOSYS;
    d->launch_arg=argument && argument[0] ? argument : NULL;
    for (unsigned a=0;a<count;a++) d->launch_argv[a]=args[a];
    errno=0;
    int pid=d->launch(d,program), error=errno;
    launch_clear(d);
    if (!pid && !d->launch_deferred) return -EIO;   /* a launcher must not return 0 unless it queued */
    return pid<0 ? -(error ? error : EIO) : pid;
}
int desk_task_launch_args(struct desktop *d, const char *name, const char *const *args, unsigned count)
{
    if (!args || !count || count > sizeof(d->launch_argv) / sizeof(d->launch_argv[0])) {
        launch_clear(d); return launch_done(d,-EINVAL);
    }
    const char *copy[6];
    for (unsigned a=0;a<count;a++) copy[a]=args[a];
    launch_clear(d);
    if (desk_session_busy(d)) return launch_done(d,-EBUSY);
    if (!strncmp(name,"/Programs/",sizeof("/Programs/")-1)) name+=sizeof("/Programs/")-1;
    for (unsigned k=0; k<d->program_count; k++) if (!strcmp(d->programs[k].name,name))
        return launch_done(d,task_launch(d,&d->programs[k],NULL,copy,count));
    return launch_done(d,-ENOENT);
}
int desk_task_launch(struct desktop *d, const char *name, const char *argument)
{
    launch_clear(d);
    if (desk_session_busy(d)) return launch_done(d,-EBUSY);
    if (!strncmp(name,"/Programs/",sizeof("/Programs/")-1)) name+=sizeof("/Programs/")-1;
    if (!strcmp(name,"help")) {
        const char *arg=argument && *argument ? argument : "topic:welcome";
        size_t n=strlen(arg); if (n>DESK_ARG_MAX) return launch_done(d,-EINVAL);
        for (unsigned k=0;k<DESK_CLIENTS;k++) {
            struct desk_client *c=&d->clients[k];
            if (c->channel<0 || c->closing || strcmp(c->application,"help")) continue;
            struct ciuki_message msg; desk_message(&msg,DESK_LAUNCH,20+(unsigned)n);
            desk_put32(msg.data+4,1); desk_put32(msg.data+8,4); memcpy(msg.data+12,"help",4);
            desk_put32(msg.data+16,(uint32_t)n); memcpy(msg.data+20,arg,n); desk_queue(d,(int)k,&msg);
            if (c->window) { if (c->collapsed) desk_collapse(d,(int)k); (void)desk_task_switch(d,c->window_id); }
            d->help_starting=0; return launch_done(d,(int)c->pid);
        }
        if (d->help_starting) { snprintf(d->help_pending,sizeof(d->help_pending),"%s",arg); return launch_done(d,0); }
    }
    for (unsigned k=0; k<d->program_count; k++) if (!strcmp(d->programs[k].name,name))
    {
        int result=task_launch(d,&d->programs[k],argument,NULL,0);
        if (!strcmp(name,"help") && !result && d->launch_deferred) d->help_starting=1;
        return launch_done(d,result);
    }
    return launch_done(d,-ENOENT);
}

int desk_ide_launch(struct desktop *d,const char *path)
{
    launch_clear(d);
    if (desk_session_busy(d)) return launch_done(d,-EBUSY);
    size_t n=strlen(path);
    if (n<=11 || n>DESK_ARG_MAX || strncmp(path,"/Documents/",11)) return launch_done(d,-EPERM);
    const char *start=path+11;
    for (const char *p=start;;++p) {
        unsigned char ch=(unsigned char)*p;
        if (!ch || ch=='/') {
            size_t len=(size_t)(p-start);
            if (!len || (len==1 && *start=='.') || (len==2 && start[0]=='.' && start[1]=='.')) return launch_done(d,-EINVAL);
            if (!ch) break;
            start=p+1;
        } else if (ch<32 || ch>126 || ch=='\\' || ch==':') return launch_done(d,-EINVAL);
    }
    if (!d->launch) return launch_done(d,-ENOSYS);
    /* The ordinary launcher owns channel creation, child tracking and reap.
       Kernel ELF validation still admits/rejects the actual file. A generic
       project app gets no Task Manager, mixer, Files or IDE privileges. */
    struct desk_program program={0};
    strcpy(program.name,"ciukide-app"); memcpy(program.path,path,n+1);
    return launch_done(d,task_launch(d,&program,NULL,NULL,0));
}
/* fx-8: a launch that completed on the launcher worker. The synchronous
 * routes reported their own failures; this reports the late result the
 * same way: the client's TASK_RESULT (sent only to the same requester),
 * the Run and Games messages, AutoPlay's message, else a notification. */
void desk_launch_completed(struct desktop *d, const struct desk_launch_request *r, const char *name, int result)
{
    if (!strcmp(name,"help")) {
        d->help_starting=0;
        if (result>0 && d->help_pending[0]) {
            char arg[DESK_ARG_MAX+1]; memcpy(arg,d->help_pending,sizeof(arg)); d->help_pending[0]=0;
            (void)desk_task_launch(d,"help",arg);
        } else d->help_pending[0]=0;
    }
    char text[200];
    switch (r->origin) {
    case DESK_LAUNCH_CLIENT: {
        if (r->client<0 || r->client>=DESK_CLIENTS) return;
        struct desk_client *c=&d->clients[r->client];
        if (c->channel!=r->channel || c->channel<0 || c->pid!=r->pid || c->spawn_pending) return;
        if (r->recent && result>0 && d->shell && !d->sh.gate) { cfg_recent_add(&d->sh.cfg, r->argument); d->sh.want_save = 1; }
        struct ciuki_message reply;
        desk_message(&reply, DESK_TASK_RESULT, 16); desk_put32(reply.data + 4, r->serial);
        desk_put32(reply.data + 8, r->opcode); desk_put32(reply.data + 12, (uint32_t)result);
        desk_queue(d, r->client, &reply);
        return;
    }
    case DESK_LAUNCH_RUN:
        if (r->run_command[0]) {
            if (result>=0) desk_run_remember(d,r->run_command);
            desk_run_result(d,r->run_command,result<0 ? result : 0);
            return;
        }
        if (result<0) {
            snprintf(text,sizeof(text),"'%s' could not be started.\n%s (%d)",name,strerror(-result),-result);
            desk_shell_message(d,"Run",text,1);
        }
        return;
    case DESK_LAUNCH_GAME:
        if (result<0) {
            snprintf(text,sizeof(text),"'%s' could not be started (error %d).",r->label[0] ? r->label : name,-result);
            desk_shell_message(d,"Games",text,1);
        }
        return;
    case DESK_LAUNCH_SCHEDULE:
        if (desk_schedsvc_completed) desk_schedsvc_completed(d, r->serial, result);
        if (result < 0) desk_shell_notify_error(d, "Scheduled task did not start", -result);
        return;
    case DESK_LAUNCH_AUTOPLAY:
        if (result<0) desk_shell_message(d,"AutoPlay","The selected program could not be started.",1);
        return;
    default:
        if (result<0) desk_shell_notify_error(d,"Program did not start",-result);
        return;
    }
}

/* Shutdown is driven by the ordinary IPC/reap/paint loop, never by sleeps.
 * A channel CLOSE/disconnect is not proof that its process (or VM) exited. */
static void power_target(struct desktop *d, uint32_t pid, const char *name, int owned)
{
    if (!pid || pid==d->self_pid) return;
    for (unsigned i=0; i<d->power.count; i++) if (d->power.targets[i].pid==pid) {
        d->power.targets[i].owned|=owned;
        if (name && name[0]) snprintf(d->power.targets[i].name,sizeof(d->power.targets[i].name),"%s",name);
        return;
    }
    if (d->power.count>=DESK_CLIENTS*2) return;
    struct desk_power_target *t=&d->power.targets[d->power.count++];
    t->pid=pid; t->owned=owned;
    if (name && name[0]) snprintf(t->name,sizeof(t->name),"%s",name);
    else snprintf(t->name,sizeof(t->name),"PID %u",pid);
}
static int power_live(struct desktop *d, struct desk_power_target *t)
{
    if (t->gone) return 0;
    if (t->owned) {
        for (int i=0; i<DESK_CLIENTS; i++) if (d->children[i].pid==t->pid) return 1;
        t->gone=1; return 0;
    }
    if (d->power_alive && d->power_alive(d,t->pid)==0) { t->gone=1; return 0; }
    return 1; /* Unknown is still pending, including after IPC disconnect. */
}
static void power_terminate(struct desktop *d, struct desk_power_target *t)
{
    if (!power_live(d,t)) return;
    t->terminated=1;
    if (d->terminate && d->terminate(t->pid,SIGTERM)<0 && errno==ESRCH) t->gone=1;
    for (int i=0; i<DESK_CLIENTS; i++) if (d->children[i].pid==t->pid) d->children[i].term_sent=1;
}
void desk_power_begin(struct desktop *d, int action)
{
    if (!d->shell || d->sh.gate || d->power.state || action<1 || action>3) return;
    memset(&d->power,0,sizeof(d->power));
    d->power.state=DESK_POWER_CLOSING; d->power.action=action;
    d->power.previous_focus=d->focus;
    d->power.close_deadline=d->now_ns+DESK_POWER_CLOSE_NS;
    desk_menu_close(d);
    d->sh.volume_open=d->sh.volume_drag=d->sh.switcher=d->sh.tooltip=0;
    d->sh.want_run[0]=0; d->action=-1;
    d->drag=-1; d->resize_edges=0; d->geometry_outline=0; d->title_down=d->title_hot=0;
    for (int i=0; i<DESK_CLIENTS; i++) {
        struct desk_client *c=&d->clients[i];
        if (!c->internal && c->channel>=0) power_target(d,c->pid,c->title,0);
    }
    for (int i=0; i<DESK_CLIENTS; i++) power_target(d,d->children[i].pid,NULL,1);
    /* G3: after every program agreed, a registered window gets END_SESSION(1)
     * (exit without prompting), a Win32 window already got WM_ENDSESSION from
     * the broker; any other window gets its ordinary close message. */
    for (int i=0; i<DESK_CLIENTS; i++) {
        struct desk_client *c=&d->clients[i];
        if (!c->window) continue;
        if (c->session_end==1 && c->channel>=0 && !c->closing) {
            struct ciuki_message m;
            desk_message(&m,DESK_END_SESSION,16); desk_put32(m.data+4,d->session.serial ? d->session.serial : 1);
            desk_put32(m.data+8,1); desk_put32(m.data+12,d->session.flags);
            desk_queue(d,i,&m); c->closing=1; desk_caption_damage(d,i);
        } else if (c->session_end==2) { c->closing=1; desk_caption_damage(d,i); }
        else desk_request_close(d,i);
    }
    for (int i=0; i<DESK_CLIENTS; i++) if (d->children[i].pid)
        d->children[i].close_ns=d->power.close_deadline;
    strcpy(d->power.text,action==3 ? "Logging off...\nClosing programs and virtual machines." :
                         "Shutting down...\nClosing programs and virtual machines.");
    d->power.window=desk_internal_open(d,DESK_INTERNAL_POWER);
    desk_theme_changed(d);
}
void desk_power_cancel(struct desktop *d)
{
    if (d->power.state==DESK_POWER_DONE) return; /* flushed/stopped session cannot resume */
    if (d->sound_cancel) d->sound_cancel(d);
    if (!d->power.state || d->power.state==DESK_POWER_DONE) return;
    int window=d->power.window, focus=d->power.previous_focus;
    d->power.state=DESK_POWER_IDLE;
    if (window>=0 && d->clients[window].internal==DESK_INTERNAL_POWER) desk_drop_client(d,window);
    for (int i=0; i<DESK_CLIENTS; i++) {
        for (unsigned j=0; j<d->power.count; j++) if (d->children[i].pid==d->power.targets[j].pid) {
            d->children[i].close_ns=0; d->children[i].term_sent=0;
        }
        d->clients[i].closing=0; d->clients[i].close_sent_ns=0; d->clients[i].session_end=0;
    }
    d->sh.modifiers=0; d->sh.logo_down=d->sh.alt_routed=d->sh.alt_used=d->sh.alt_code=0;
    memset(d->keys,0,sizeof(d->keys)); d->buttons=d->routed_buttons=0;
    d->sh.want_save=d->sh.want_mixer=d->sh.want_reload=d->sh.want_power=0;
    desk_focus(d,focus>=0 && d->clients[focus].window ? focus : -1);
    desk_theme_changed(d);
}
static void power_busy(struct desktop *d)
{
    d->power.state=DESK_POWER_BUSY;
    strcpy(d->power.text,"Some programs are still running:");
    unsigned count=0;
    for (unsigned i=0; i<d->power.count; i++) if (power_live(d,&d->power.targets[i])) {
        size_t n=strlen(d->power.text);
        if (sizeof(d->power.text)-n<54) { strcat(d->power.text,"\n..."); break; }
        snprintf(d->power.text+n,sizeof(d->power.text)-n,"\n%.48s",d->power.targets[i].name);
        count++;
    }
    if (!count) strcat(d->power.text,"\nUnknown storage holder (no client remains).");
    if (d->power.window>=0) desk_drop_client(d,d->power.window);
    d->power.window=desk_internal_open(d,DESK_INTERNAL_POWER);
    desk_theme_changed(d);
}
void desk_power_force(struct desktop *d)
{
    if (d->power.state!=DESK_POWER_BUSY) return;
    for (unsigned i=0; i<d->power.count; i++) power_terminate(d,&d->power.targets[i]);
    d->power.state=DESK_POWER_FORCE;
    d->power.next_try=d->now_ns+DESK_POWER_RETRY_NS;
    strcpy(d->power.text,"Shutting down...\nWaiting for terminated programs.");
    desk_theme_changed(d);
}
void desk_power_tick(struct desktop *d)
{
    struct desk_power_sequence *p=&d->power;
    if (!p->state || d->sh.gate || p->state==DESK_POWER_BUSY || p->state==DESK_POWER_DONE) return;
    if (p->state==DESK_POWER_CLOSING) {
        int pending=0;
        for (unsigned i=0; i<p->count; i++) if (power_live(d,&p->targets[i])) {
            pending=1;
            if (d->now_ns>=p->close_deadline && !p->targets[i].terminated) power_terminate(d,&p->targets[i]);
        }
        if (pending && d->now_ns<p->close_deadline) return;
        if (p->action==3) {
            /* G32: retain G3 veto/close semantics, then return to logon. */
            if (pending) {
                for (unsigned i=0; i<p->count; i++) if (power_live(d,&p->targets[i]) && !p->targets[i].terminated) power_terminate(d,&p->targets[i]);
                if (d->now_ns<p->close_deadline+DESK_POWER_BUSY_NS) return;
            }
            desk_power_cancel(d);
            if (d->logoff) d->logoff(d);
            return;
        }
        p->state=DESK_POWER_RETRY; p->next_try=d->now_ns;
        p->busy_deadline=d->now_ns+DESK_POWER_BUSY_NS;
    }
    if (d->now_ns<p->next_try) return;
    if (d->sound_shutdown && !d->sound_shutdown(d)) return;
    if (!p->prepared) { p->prepared=1; if (d->power_prepare) d->power_prepare(d); }
    int r;
    if (d->power_request) r=d->power_request(p->action);
    else { errno=ENOSYS; r=-1; }
    if (!r) { p->state=DESK_POWER_DONE; return; } /* real success never returns */
    int error=errno;
    int progress=d->power_state ? d->power_state() : 0;
    if (progress>=0 && (progress&(CIUKI_POWER_STOPPED|CIUKI_POWER_UNCERTAIN))) {
        p->state=DESK_POWER_DONE;
        if (progress&CIUKI_POWER_UNCERTAIN)
            snprintf(p->text,sizeof(p->text),"Shutdown could not complete (error %d).\nA device or disk could not be stopped safely.\nIts resources remain reserved.\nUse the computer's reset button to restart CiukiOS.",error);
        else
            snprintf(p->text,sizeof(p->text),"Programs are closed and devices have stopped.\nIt is now safe to turn off your computer.\nPower-off returned error %d.",error);
        desk_theme_changed(d); return;
    }
    if (error==EBUSY) {
        if (p->state==DESK_POWER_FORCE || d->now_ns>=p->busy_deadline) power_busy(d);
        else p->next_try=d->now_ns+DESK_POWER_RETRY_NS;
        return;
    }
    char text[300];
    int action=p->action;
    desk_power_cancel(d);
    if (error==ENOSYS) snprintf(text,sizeof(text),"%s is not available yet: this kernel has no\npower interface (call 73). Turn the computer off\nwith its power button once the disk is idle.\n%s (error %d).",
                               action==1 ? "Restart" : "Shut down",strerror(error),error);
    else snprintf(text,sizeof(text),"%s failed (error %d): %s.",action==1 ? "Restart" : "Shut down",error,strerror(error));
    desk_shell_message(d,"Shut Down CiukiOS",text,1);
}

/* ---- G3 end of session ------------------------------------------------
 * Windows 9x model (MS-end; docs/design/desktop-shell.md "End of session"):
 * every program is asked in turn, topmost window first; the Win32 programs
 * are asked by the w32srv broker as one turn at the position of their
 * topmost window. A program that shows its own prompt (ASKING) is waited for
 * without a deadline while it answers PINGs. Only after every program
 * agreed does desk_power_begin run (END_SESSION, then the fx-5 sequence). */
int desk_session_busy(const struct desktop *d)
{ return d->power.state || d->session.state; }
static int session_slot(const struct desktop *d, uint32_t id)
{
    if (!id) return -1;
    for (int i=0; i<DESK_CLIENTS; i++)
        if (d->clients[i].window && !d->clients[i].internal && d->clients[i].window_id==id && d->clients[i].channel>=0) return i;
    return -1;
}
static struct desk_session_part *session_current(struct desktop *d)
{
    struct desk_session *s=&d->session;
    return s->state==DESK_SESSION_QUERY && s->at<s->count ? &s->parts[s->at] : NULL;
}
static void session_send_end(struct desktop *d, int i, int ending)
{
    struct ciuki_message m;
    desk_message(&m,DESK_END_SESSION,16); desk_put32(m.data+4,d->session.serial ? d->session.serial : 1);
    desk_put32(m.data+8,(uint32_t)ending); desk_put32(m.data+12,d->session.flags);
    desk_queue(d,i,&m);
}
static void session_close_dialog(struct desktop *d)
{ if (d->end_task.kind && d->end_task.session) desk_end_task_choice(d,-1); }
static int session_w32(struct desktop *d, unsigned action, uint32_t flags, struct desk_w32_status *st)
{
    struct desk_w32_status ignored;
    if (!d->w32_session) return -ENODEV;
    return d->w32_session(d,action,flags,st ? st : &ignored);
}
/* Any veto, No or Cancel: the queried programs that agreed get
 * END_SESSION(0) (WM_ENDSESSION FALSE) and every program keeps running. */
void desk_session_cancel(struct desktop *d)
{
    struct desk_session *s=&d->session;
    if (s->state!=DESK_SESSION_QUERY) return;
    for (unsigned k=0; k<s->count; k++) {
        struct desk_session_part *p=&s->parts[k];
        if (p->kind==DESK_PART_WIN32) {
            if (p->state==DESK_PART_QUERIED || p->state==DESK_PART_AGREED) (void)session_w32(d,DESK_W32_SESSION_CANCEL,0,NULL);
            continue;
        }
        int i=session_slot(d,p->window_id);
        if (i>=0 && d->clients[i].session_aware && p->answered && p->state!=DESK_PART_WAITING) session_send_end(d,i,0);
    }
    session_close_dialog(d);
    fprintf(stderr,"[desktop] end-session cancelled action=%d at=%u of=%u\n",s->action,s->at,s->count);
    memset(s,0,sizeof(*s));
    for (int i=0; i<DESK_CLIENTS; i++) d->clients[i].session_end=0;
    desk_cursor_update(d);
}
static void session_finish(struct desktop *d)
{
    struct desk_session *s=&d->session;
    int action=s->action, w32=0;
    for (unsigned k=0; k<s->count; k++) {
        struct desk_session_part *p=&s->parts[k];
        if (p->kind==DESK_PART_WIN32) { w32=p->state==DESK_PART_AGREED && p->answered; continue; }
        int i=session_slot(d,p->window_id);
        if (i>=0 && p->kind==DESK_PART_WINDOW && d->clients[i].session_aware && p->answered) d->clients[i].session_end=1;
    }
    if (w32 && !session_w32(d,DESK_W32_SESSION_COMMIT,0,NULL))
        for (int i=0; i<DESK_CLIENTS; i++) if (d->clients[i].window && d->clients[i].v3_win32_hwnd) d->clients[i].session_end=2;
    fprintf(stderr,"[desktop] end-session agreed action=%d programs=%u\n",action,s->count);
    s->state=DESK_SESSION_IDLE; s->at=s->count=0; /* serial and flags remain for END_SESSION */
    desk_power_begin(d,action);
    for (int i=0; i<DESK_CLIENTS; i++) d->clients[i].session_end=0;
}
static void session_step(struct desktop *d);
static void session_agree(struct desktop *d, struct desk_session_part *p, int state)
{
    p->state=state;
    if (d->end_task.kind && d->end_task.session) session_close_dialog(d);
    d->session.at++;
    session_step(d);
}
/* The program in turn is hung: unresponsive (no PONG for DESK_HUNG_NS, the
 * HungAppTimeout) since at least DESK_HUNG_NS after its query; a Win32 USER
 * thread that has not even fetched its WM_QUERYENDSESSION is hung too. */
static int session_hung(struct desktop *d, struct desk_session_part *p, const char **name, uint32_t *pid, uint32_t *target)
{
    if (p->kind==DESK_PART_WINDOW) {
        int i=session_slot(d,p->window_id);
        if (i<0 || !d->clients[i].unresponsive || d->now_ns<p->sent_ns+DESK_HUNG_NS) return 0;
        *name=d->clients[i].title; *pid=d->clients[i].pid; *target=d->clients[i].window_id; return 1;
    }
    struct desk_session *s=&d->session;
    if (p->kind!=DESK_PART_WIN32 || !s->w32_pid || d->now_ns<s->w32_since+DESK_HUNG_NS) return 0;
    int hung=!s->w32_delivered, shown=-1;
    for (int i=0; i<DESK_CLIENTS; i++) if (d->clients[i].window && d->clients[i].pid==s->w32_pid && d->clients[i].version==DESK_V3_VERSION) {
        if (shown<0 || d->clients[i].v3_win32_hwnd==s->w32_hwnd) shown=i;
        if (d->clients[i].unresponsive) hung=1;
    }
    if (!hung) return 0;
    *name=shown>=0 ? d->clients[shown].title : "Windows program"; *pid=s->w32_pid; *target=0; return 1;
}
static void session_step(struct desktop *d)
{
    struct desk_session *s=&d->session;
    while (s->state==DESK_SESSION_QUERY && s->at<s->count) {
        struct desk_session_part *p=&s->parts[s->at];
        if (p->state==DESK_PART_AGREED || p->state==DESK_PART_GONE) { s->at++; continue; }
        if (p->kind==DESK_PART_LEGACY) { p->state=DESK_PART_AGREED; s->at++; continue; }
        if (p->kind==DESK_PART_WINDOW) {
            int i=session_slot(d,p->window_id);
            if (i<0) { p->state=DESK_PART_GONE; s->at++; continue; }  /* closed or exited: nothing to lose */
            struct desk_client *c=&d->clients[i];
            if (!c->session_aware) { p->state=DESK_PART_AGREED; s->at++; continue; } /* DefWindowProc: TRUE */
            if (p->state==DESK_PART_WAITING) {
                struct ciuki_message m;
                if (!++s->serial) ++s->serial;
                p->serial=s->serial; p->sent_ns=d->now_ns; p->state=DESK_PART_QUERIED; p->ping=0;
                desk_message(&m,DESK_QUERY_END_SESSION,12); desk_put32(m.data+4,p->serial); desk_put32(m.data+8,s->flags);
                desk_queue(d,i,&m);
                if (!c->ping_pending && c->tx_count<DESK_TX_MAX) {
                    desk_message(&m,DESK_PING,8); desk_put32(m.data+4,++c->serial);
                    desk_queue(d,i,&m); c->ping_pending=1; c->ping_queued=1; c->ping_sent_ns=d->now_ns; p->ping=c->serial;
                }
                if (d->focus!=i && !c->minimized) desk_focus(d,i);   /* its prompt comes to the front */
                return;
            }
            return; /* QUERIED or ASKING: wait for its answer */
        }
        /* DESK_PART_WIN32 */
        struct desk_w32_status st={0};
        if (p->state==DESK_PART_WAITING) {
            uint32_t ewx=s->action==1 ? DESK_EWX_REBOOT : s->action==2 ? DESK_EWX_SHUTDOWN|DESK_EWX_POWEROFF : DESK_EWX_LOGOFF;
            int r=session_w32(d,DESK_W32_SESSION_BEGIN,ewx|DESK_W32_SESSION_HOLD,&st);
            if (r) {
                fprintf(stderr,"[desktop] end-session win32 broker unavailable result=%d\n",r);
                p->state=DESK_PART_AGREED; s->at++; continue;
            }
            p->state=DESK_PART_QUERIED; p->answered=1; p->sent_ns=d->now_ns; s->w32_next=0; s->w32_pid=0;
        }
        if (d->now_ns<s->w32_next) return;
        s->w32_next=d->now_ns+DESK_SESSION_POLL_NS;
        if (session_w32(d,DESK_W32_SESSION_STATUS,0,&st)) { p->state=DESK_PART_AGREED; s->at++; continue; }
        if (st.state==DESK_W32_VETOED) { desk_session_cancel(d); return; }
        if (st.state==DESK_W32_HELD || st.state==DESK_W32_ENDED) { session_agree(d,p,DESK_PART_AGREED); return; }
        if (st.pid!=s->w32_pid || st.hwnd!=s->w32_hwnd) { s->w32_pid=st.pid; s->w32_hwnd=st.hwnd; s->w32_since=d->now_ns; }
        if (st.delivered && !s->w32_delivered) s->w32_since=d->now_ns;  /* fetched: its handler runs now */
        s->w32_delivered=st.delivered;
        return;
    }
    if (s->state==DESK_SESSION_QUERY && s->at>=s->count) session_finish(d);
}
void desk_session_begin(struct desktop *d, int action)
{
    if (!d->shell || d->sh.gate || desk_session_busy(d) || action<1 || action>3) return;
    struct desk_session *s=&d->session;
    uint32_t serial=s->serial;
    memset(s,0,sizeof(*s));
    s->serial=serial; s->action=action; s->flags=action==3 ? DESK_ENDSESSION_LOGOFF : 0;
    if (d->end_task.kind) desk_end_task_choice(d,-1);
    desk_menu_close(d);
    /* Topmost first (desk_client.order is the stacking order). */
    int order[DESK_CLIENTS]; unsigned n=0;
    for (int i=0; i<DESK_CLIENTS; i++) {
        const struct desk_client *c=&d->clients[i];
        if (!c->window || c->internal || c->channel<0) continue;
        unsigned at=n++;
        while (at && d->clients[order[at-1]].order<c->order) { order[at]=order[at-1]; at--; }
        order[at]=i;
    }
    int w32=0;
    for (unsigned k=0; k<n; k++) {
        const struct desk_client *c=&d->clients[order[k]];
        struct desk_session_part *p=&s->parts[s->count];
        if (c->version==DESK_V3_VERSION) {
            /* Win32 windows: one broker turn; another v3 program (NetSurf)
             * has no query handler and agrees, as DefWindowProc does. */
            if (!c->v3_win32_hwnd || w32) continue;
            w32=1; p->kind=DESK_PART_WIN32;
        } else {
            p->kind=c->version==DESK_VERSION ? DESK_PART_LEGACY : DESK_PART_WINDOW;
            p->window_id=c->window_id; p->pid=c->pid;
        }
        s->count++;
    }
    /* Win32 programs with hidden windows only are asked last. */
    if (!w32 && d->w32_session) s->parts[s->count++].kind=DESK_PART_WIN32;
    s->state=DESK_SESSION_QUERY;
    fprintf(stderr,"[desktop] end-session begin action=%d programs=%u\n",action,s->count);
    session_step(d);
}
void desk_session_tick(struct desktop *d)
{
    struct desk_session_part *p=session_current(d);
    if (!p) { if (d->session.state) session_step(d); return; }
    if (p->kind==DESK_PART_WINDOW && session_slot(d,p->window_id)<0) { session_agree(d,p,DESK_PART_GONE); return; }
    if (p->kind==DESK_PART_WIN32) {
        session_step(d);
        if (!(p=session_current(d)) || p->kind!=DESK_PART_WIN32) return;
    }
    const char *name=NULL; uint32_t pid=0,target=0;
    int hung=session_hung(d,p,&name,&pid,&target);
    if (d->end_task.kind && d->end_task.session) {
        if (d->end_task.kind==DESK_HANG_NOT_RESPONDING && !hung) session_close_dialog(d); /* it answers again */
        return;
    }
    if (hung) desk_end_task_open(d,DESK_HANG_NOT_RESPONDING,1,pid,target,name);
    /* G11: a battery alarm with "Force standby or shutdown even if a program
     * stops responding" ends it, as End Task would. */
    if (hung && d->session.force && d->end_task.kind==DESK_HANG_NOT_RESPONDING && d->end_task.session) desk_end_task_choice(d,1);
}
/* Next instant the session or a window close needs a turn (0: none). */
uint64_t desk_session_deadline(const struct desktop *d)
{
    uint64_t next=0;
    const struct desk_session *s=&d->session;
    if (s->state==DESK_SESSION_QUERY && s->at<s->count) {
        const struct desk_session_part *p=&s->parts[s->at];
        if (p->kind==DESK_PART_WIN32) next=s->w32_next ? s->w32_next : d->now_ns;
        else if (p->kind==DESK_PART_WINDOW && p->state==DESK_PART_QUERIED) next=p->sent_ns+DESK_HUNG_NS;
    }
    if (!s->state && !d->end_task.kind && !d->power.state) for (int i=0; i<DESK_CLIENTS; i++) {
        const struct desk_client *c=&d->clients[i];
        if (c->window && c->closing && c->close_sent_ns && c->unresponsive && c->version!=DESK_VERSION) {
            uint64_t t=c->close_sent_ns+DESK_HUNG_NS;
            if (!next || t<next) next=t;
        }
    }
    return next;
}
static void session_answer(struct desktop *d, int i, uint32_t serial, uint32_t answer)
{
    struct desk_session_part *p=session_current(d);
    struct desk_client *c=&d->clients[i];
    if (!p || p->kind!=DESK_PART_WINDOW || p->window_id!=c->window_id || p->serial!=serial ||
        (p->state!=DESK_PART_QUERIED && p->state!=DESK_PART_ASKING)) return;
    switch (answer) {
    case DESK_SESSION_VETO:
        fprintf(stderr,"[desktop] end-session veto pid=%u\n",c->pid);
        p->answered=0; desk_session_cancel(d); return;
    case DESK_SESSION_AGREE: p->answered=1; session_agree(d,p,DESK_PART_AGREED); return;
    case DESK_SESSION_ASKING: p->answered=1; p->state=DESK_PART_ASKING; return;
    case DESK_SESSION_DOS_ACTIVE:
        p->answered=1; p->state=DESK_PART_ASKING;
        desk_end_task_open(d,DESK_HANG_DOS,1,c->pid,c->window_id,c->title);
        return;
    default: return;
    }
}
/* A registered window that answers a PING sent after its query without
 * answering the query agrees (DefWindowProc returns TRUE). */
static void session_pong(struct desktop *d, int i, uint32_t serial)
{
    struct desk_session_part *p=session_current(d);
    if (!p || p->kind!=DESK_PART_WINDOW || p->state!=DESK_PART_QUERIED || !p->ping ||
        d->clients[i].window_id!=p->window_id || (int32_t)(serial-p->ping)<0) return;
    p->answered=1; session_agree(d,p,DESK_PART_AGREED);
}
static void session_ping_sent(struct desktop *d, int i)
{
    struct desk_session_part *p=session_current(d);
    if (p && p->kind==DESK_PART_WINDOW && p->state==DESK_PART_QUERIED && !p->ping && d->clients[i].window_id==p->window_id)
        p->ping=d->clients[i].serial;
}
void desk_end_task_open(struct desktop *d, int kind, int session, uint32_t pid, uint32_t target, const char *name)
{
    if (kind!=DESK_HANG_NOT_RESPONDING && kind!=DESK_HANG_DOS) return;
    if (d->end_task.kind) {
        if (!session || d->end_task.session) return;   /* one at a time; the session's takes over */
        desk_end_task_choice(d,-1);
    }
    d->end_task=(struct desk_end_task){.kind=kind,.session=session,.pid=pid,.target=target,.window=-1};
    snprintf(d->end_task.name,sizeof(d->end_task.name),"%s",name && name[0] ? name : "Program");
    int w=desk_internal_open(d,DESK_INTERNAL_END_TASK);
    if (w<0) {
        memset(&d->end_task,0,sizeof(d->end_task)); d->end_task.window=-1;
        if (session) desk_session_cancel(d);   /* no way to ask the user: keep everything running */
        return;
    }
    d->end_task.window=w;
    snprintf(d->clients[w].title,sizeof(d->clients[w].title),"%s",d->end_task.name);
    desk_caption_damage(d,w);
    fprintf(stderr,"[desktop] end task dialog kind=%s pid=%u session=%d\n",kind==DESK_HANG_DOS ? "dos" : "not-responding",pid,session);
}
/* end: 1 End Task (Yes), 0 Cancel (No), -1 dismissed (the program answered,
 * closed or exited, or the session ended). */
void desk_end_task_choice(struct desktop *d, int end)
{
    struct desk_end_task e=d->end_task;
    if (!e.kind) return;
    memset(&d->end_task,0,sizeof(d->end_task)); d->end_task.window=-1;
    if (e.window>=0 && e.window<DESK_CLIENTS && d->clients[e.window].internal==DESK_INTERNAL_END_TASK) desk_drop_client(d,e.window);
    if (end<0) return;
    struct desk_session_part *p=e.session ? session_current(d) : NULL;
    if (e.session && !p) return;
    if (!end) {
        if (e.session) { desk_session_cancel(d); return; }
        if (e.kind==DESK_HANG_DOS)   /* No: the window and its program stay */
            for (int i=0; i<DESK_CLIENTS; i++) if (d->clients[i].window && d->clients[i].pid==e.pid && d->clients[i].closing) {
                d->clients[i].closing=0; d->clients[i].close_sent_ns=0; desk_caption_damage(d,i);
            }
        return;   /* Cancel: the close request stays queued for when the program recovers */
    }
    if (e.session && e.kind==DESK_HANG_DOS) { p->answered=1; session_agree(d,p,DESK_PART_AGREED); return; }
    /* End Task / Yes: terminate the program's process (TerminateProcess). */
    int r=-1, error=EPERM;
    if (e.pid && e.pid!=d->self_pid && d->terminate) { errno=0; r=d->terminate(e.pid,SIGKILL); error=errno; }
    fprintf(stderr,"[desktop] end task pid=%u result=%d\n",e.pid,r ? -(error ? error : EIO) : 0);
    if (!r) desk_sound(d,SOUND_END_TASK);
    if (e.session) {
        if (p->kind==DESK_PART_WINDOW) session_agree(d,p,DESK_PART_GONE);
        else d->session.w32_since=d->now_ns;  /* the broker skips the ended process's windows */
    }
}

/* V3 transports retain one anchor slot; child windows share only its fd.
 * All parsing is byte based; channel sender_pid remains the sole PID authority. */
static void v3_reply(struct desktop *d, int peer, const struct desk_packet *p, int error)
{
    struct ciuki_message m;
    desk_v3_message(&m,DESK_V3_RESULT,8,p->request,p->window_id,p->generation);
    desk_put32(m.data+16,p->opcode); desk_put32(m.data+20,(uint32_t)error);
    desk_queue(d,peer,&m);
}
static int v3_find(const struct desktop *d, int peer, unsigned id, unsigned generation)
{
    for (int k=0;k<DESK_CLIENTS;k++) {
        const struct desk_client *c=&d->clients[k];
        if (c->v3_peer==(unsigned)peer+1 && c->window && c->v3_id==id && c->v3_generation==generation) return k;
    }
    return -1;
}
void desk_v3_restack(struct desktop *d)
{
    /* Stable partition followed by owner-before-owned topological sorting.
     * Existing native order stays byte-for-byte untouched without v3 windows. */
    int list[DESK_CLIENTS], n=0, any=0;
    for (int k=0;k<DESK_CLIENTS;k++) if (d->clients[k].window) {
        list[n++]=k; if (d->clients[k].version==DESK_V3_VERSION) any=1;
    }
    if (!any) return;
    for (int a=1;a<n;a++) {
        int key=list[a], b=a;
        while (b && d->clients[list[b-1]].order>d->clients[key].order) { list[b]=list[b-1]; b--; }
        list[b]=key;
    }
    for (int a=0;a<n;a++) for (int b=0;b+1<n;b++) {
        struct desk_client *lo=&d->clients[list[b]], *hi=&d->clients[list[b+1]];
        unsigned lp=lo->internal ? 2u : (lo->v3_style&DESK_V3_TOPMOST) ? 1u : 0u;
        unsigned hp=hi->internal ? 2u : (hi->v3_style&DESK_V3_TOPMOST) ? 1u : 0u;
        if (lp>hp) {
            int t=list[b]; list[b]=list[b+1]; list[b+1]=t;
        }
    }
    for (int pass=0;pass<n;pass++) for (int a=0;a<n;a++) {
        struct desk_client *c=&d->clients[list[a]];
        for (int b=a+1;b<n;b++) if (c->owner_id==d->clients[list[b]].window_id) {
            int t=list[a]; memmove(list+a,list+a+1,(size_t)(b-a)*sizeof(*list)); list[b]=t; break;
        }
    }
    for (int a=0;a<n;a++) d->clients[list[a]].order=++d->order;
}
void desk_v3_pointer_release(struct desktop *d)
{
    if (d->v3_pointer_window>0) {
        int i=d->v3_pointer_window-1; struct desk_client *c=&d->clients[i]; struct ciuki_message m;
        desk_v3_message(&m,DESK_V3_POINTER_STATE,4,0,c->v3_id,c->v3_generation); desk_queue(d,i,&m);
        c->v3_pointer=0;
    }
    d->v3_pointer_window=0; d->v3_cursor_hidden=0; d->cursor_dirty=1;
}
static void v3_lease_event(struct desktop *d, int i, unsigned state)
{
    struct ciuki_message m; struct desk_client *c=&d->clients[i];
    desk_v3_message(&m,DESK_V3_LEASE_EVENT,72,0,c->v3_id,c->v3_generation);
    desk_put32(m.data+16,state); desk_put32(m.data+20,d->v3_lease_generation);
    desk_put32(m.data+24,d->v3_lease_w); desk_put32(m.data+28,d->v3_lease_h); desk_put32(m.data+32,d->v3_lease_bpp);
    const struct desk_lease_tuple *t=&d->v3_lease_tuple;
    const uint32_t words[]={t->token,t->generation,t->mode,t->scale,t->surface.width,t->surface.height,
        t->surface.pitch,t->surface.size,t->surface.format,t->native_width,t->native_height,t->native_bpp,
        (uint32_t)d->v3_lease_error};
    for (unsigned k=0;k<sizeof(words)/sizeof(*words);k++) desk_put32(m.data+36+4*k,words[k]);
    desk_queue(d,i,&m);
}
void desk_v3_revoke(struct desktop *d)
{
    if (!d->v3_lease_state) return;
    int i=-1;
    for (int k=0;k<DESK_CLIENTS;k++) if (d->clients[k].window_id==d->v3_lease_window) i=k;
    if (!d->v3_restoring) {
        d->v3_restoring=1; d->v3_restore_reported=0;
        d->v3_restore_started=d->now_ns; d->v3_restore_presents=d->presents;
        __atomic_store_n(&d->v3_restore_watchdog_ms,(uint32_t)(d->now_ns/1000000ull),__ATOMIC_RELEASE);
        __atomic_store_n(&d->v3_restore_watchdog_reported,0,__ATOMIC_RELEASE);
        __atomic_store_n(&d->v3_restore_watchdog_pending,1,__ATOMIC_RELEASE);
    }
    d->v3_lease_state=2;
    if (d->v3_lease && d->v3_lease(d,d->v3_lease_pid,0,
        d->v3_lease_w,d->v3_lease_h,d->v3_lease_bpp,d->v3_lease_generation)) return;
    if (i>=0) v3_lease_event(d,i,3);
    d->v3_lease_state=0; d->v3_lease_window=0;
    desk_v3_pointer_release(d); d->cursor_dirty=1; d->scanout_valid=0;
    desk_accel_reset(d,0); d->cursor_loaded=DESK_CURSOR_COUNT; d->hw_cursor=0;
    desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,(int)d->height},d->width,d->height);
}
/* The desktop's independent 100 ms watchdog can observe a blocked revoke or
 * repaint without touching the lease state from another thread. It reports
 * the 2 s contract violation; it never enables composition over a live lease. */
int desk_v3_restore_watchdog(struct desktop *d,uint32_t now,char *out,size_t cap)
{
    if(!d || !__atomic_load_n(&d->v3_restore_watchdog_pending,__ATOMIC_ACQUIRE))return 0;
    uint32_t start=__atomic_load_n(&d->v3_restore_watchdog_ms,__ATOMIC_ACQUIRE),expected=0;
    uint32_t elapsed=now-start;
    if((int32_t)elapsed<2000 || !__atomic_compare_exchange_n(&d->v3_restore_watchdog_reported,&expected,1,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return 0;
    int n=snprintf(out,cap,"[win32] display restore watchdog_ms=%u result=%d\n",elapsed,-ETIMEDOUT);
    return n>0 && (size_t)n<cap ? n : 0;
}
void desk_v3_lease_failed(struct desktop *d, int error)
{
    d->v3_lease_error=error;
    for (int k=0;k<DESK_CLIENTS;k++)
        if (d->clients[k].window_id==d->v3_lease_window) d->clients[k].v3_lease_wanted=0;
    desk_v3_revoke(d);
}
static void v3_configure(struct desktop *d, int i)
{
    struct desk_client *c=&d->clients[i]; struct desk_box b=desk_content(c);
    struct ciuki_message m;
    desk_v3_message(&m,DESK_CONFIGURE,32,0,c->v3_id,c->v3_generation);
    desk_put32(m.data+16,(unsigned)b.w/c->v3_scale); desk_put32(m.data+20,(unsigned)b.h/c->v3_scale);
    desk_put32(m.data+24,c->v3_scale); desk_put32(m.data+28,c->resize_serial);
    desk_put32(m.data+32,(uint32_t)b.x); desk_put32(m.data+36,(uint32_t)b.y);
    desk_put32(m.data+40,c->minimized ? (uint32_t)c->minimized : c->zoomed ? 3u : 0u); desk_put32(m.data+44,c->v3_style);
    desk_queue(d,i,&m);
}
/* The track limits a window asked for (WM_GETMINMAXINFO) on top of the scale. */
static void v3_apply_limits(struct desk_client *c)
{
    unsigned s=c->v3_scale ? c->v3_scale : 1;
    c->min_width=(c->v3_limits[0] ? c->v3_limits[0] : 1)*s; c->min_height=(c->v3_limits[1] ? c->v3_limits[1] : 1)*s;
    c->max_width=(c->v3_limits[2] ? c->v3_limits[2] : 2048)*s; c->max_height=(c->v3_limits[3] ? c->v3_limits[3] : 2048)*s;
    c->step_width=c->step_height=s;
}
static void v3_place(struct desktop *d, int i, const struct desk_packet *p)
{
    struct desk_client *c=&d->clients[i];
    desk_damage_add(&d->damage,desk_outer(c),d->width,d->height);
    c->v3_style=p->style; c->v3_scale=p->scale;
    c->resizable=(p->style&3)==DESK_V3_OVERLAPPED;
    v3_apply_limits(c);
    unsigned w=p->width*p->scale, h=p->height*p->scale;
    struct desk_box frame=(p->style&3)==DESK_V3_POPUP ? (struct desk_box){p->x,p->y,(int)w,(int)h} : desk_frame(w,h,p->x,p->y);
    /* Windows keeps the maximised state through minimising and hiding; only
     * the normal state (0) restores the window to its previous place. */
    if (c->zoomed && p->flags==0) { c->zoomed=0; frame=c->restore; }
    else if (c->zoomed) frame=c->frame;
    c->frame=frame;
    c->minimized=p->flags==3 ? 0 : (int)p->flags; c->resize_serial++; if (!c->resize_serial) c->resize_serial++;
    if (p->flags==3 && !c->zoomed && c->resizable && !c->collapsed && !c->internal && desk_modal_target(d,i)==i) {
        c->restore=c->frame; c->zoomed=1;
        desk_resize(d,i,desk_work_area(d));
    }
    desk_damage_add(&d->damage,desk_outer(c),d->width,d->height);
    v3_configure(d,i); desk_v3_restack(d);
}
int desk_v3_dialog_accept(struct desktop *d)
{
    int result=0;
    if (d->v3_dialog_kind==1) result=d->v3_fault_close ? d->v3_fault_close(d->v3_dialog_pid) : d->terminate ? d->terminate(d->v3_dialog_pid,SIGKILL) : -ENOSYS;
    else if (d->v3_dialog_kind==2) result=d->v3_restart ? d->v3_restart(d,d->v3_dialog_pid,d->v3_restart_flags) : -ENOSYS;
    d->v3_dialog_kind=0; d->v3_dialog_pid=0;
    return result;
}
#include <ciuki/w32_diagnostics.h>
#include "fault_text.h"
void desk_w32_fault_dialog(struct desktop *d,const struct ciuki_w32_fault *f)
{
    char text[320];
    d->v3_details_open=0;d->v3_details[0]=0;d->v3_dialog_pid=f->pid;
    if(f->kind==W32_REPORT_CRASH){
        d->v3_dialog_kind=1;
        snprintf(text,sizeof(text),"%.63s has performed an illegal operation and will be shut down.",f->exe);
        int n=snprintf(d->v3_details,sizeof(d->v3_details),
            "Exception: %08x\nModule: %.63s + %08x\nEIP: %08x\nEAX: %08x  EBX: %08x  ECX: %08x\nEDX: %08x  ESI: %08x  EDI: %08x\nEBP: %08x  ESP: %08x  EFLAGS: %08x\nStack:",
            f->code,f->module,f->offset,f->eip,f->registers[0],f->registers[1],f->registers[2],f->registers[3],f->registers[4],f->registers[5],f->registers[6],f->registers[7],f->registers[8]);
        for(unsigned i=0;i<8&&n>0&&(unsigned)n<sizeof(d->v3_details);i++){
            int k=f->valid&(1u<<i)?snprintf(d->v3_details+n,sizeof(d->v3_details)-(unsigned)n,"%s%08x",i%4?" ":"\n",f->stack[i]):
                snprintf(d->v3_details+n,sizeof(d->v3_details)-(unsigned)n,"%s????????",i%4?" ":"\n");n+=k;
        }
        desk_shell_message(d,"Windows program error",text,1);return;
    }
    if(f->kind==W32_REPORT_NATIVE_FAULT){
        /* G13: Windows 9x wording for a native program's fault; the program is already shut down. */
        d->v3_dialog_kind=1;
        char title[64];snprintf(title,sizeof(title),"%.40s",f->exe);
        if(desk_native_fault_text(f,d->v3_details,sizeof(d->v3_details))<0)snprintf(d->v3_details,sizeof(d->v3_details),"Details are not available.");
        desk_shell_message(d,title,"This program has performed an illegal operation and will be shut down.\n\nIf the problem persists, contact the program vendor.",1);return;
    }
    d->v3_dialog_kind=3;
    switch(f->code){
    case W32_REFUSE_MSI:snprintf(text,sizeof(text),"This program uses Windows Installer, which CiukiOS does not support");break;
    case W32_REFUSE_DRIVER:snprintf(text,sizeof(text),"This program requires a Windows device driver, which CiukiOS does not support.");break;
    case W32_REFUSE_RUNTIME:snprintf(text,sizeof(text),"This program requires a runtime which CiukiOS does not support.");break;
    case W32_REFUSE_MISSING_DLL:snprintf(text,sizeof(text),"The dynamic link library %.63s could not be found in the specified path.",f->module);break;
    case W32_REFUSE_EXPORT:snprintf(text,sizeof(text),"The %.63s file is linked to missing export %.63s.",f->exe,f->module);break;
    case W32_REFUSE_SERVER:snprintf(text,sizeof(text),"The Windows program server is not available. Please try again.");break;
    default:snprintf(text,sizeof(text),"%.63s is not a valid Win32 application.",f->exe);break;
    }
    desk_shell_message(d,"Error Starting Program",text,1);
}
static int v3_order(struct desktop *d, int peer, int i, const struct desk_packet *p)
{
    struct desk_client *c=&d->clients[i];
    int after=-1, owner=-1;
    if ((p->flags&DESK_V3_ORDER_Z) && p->index && p->index<DESK_V3_ORDER_NOTOPMOST) {
        after=v3_find(d,peer,p->index,p->serial);
        if (after<0) return -ENOENT;
        if (after==i) return -EINVAL;
    }
    if ((p->flags&DESK_V3_ORDER_OWNER) && p->owner) {
        owner=v3_find(d,peer,p->owner,p->owner_generation);
        if (owner<0) return -ENOENT;
        for (int k=owner, n=0;k>=0 && n<DESK_CLIENTS;n++) {
            if (k==i) return -EINVAL;
            unsigned id=d->clients[k].owner_id; k=-1;
            for (int j=0;id && j<DESK_CLIENTS;j++) if (d->clients[j].window_id==id) { k=j; break; }
        }
    }
    if ((p->flags&DESK_V3_ORDER_OWNER) && owner<0 && (c->v3_style&DESK_V3_MODAL)) return -EINVAL;
    if (p->flags&DESK_V3_ORDER_OWNER) c->owner_id=owner<0 ? 0 : d->clients[owner].window_id;
    if (p->flags&DESK_V3_ORDER_Z) {
        if (p->index==DESK_V3_ORDER_TOPMOST) c->v3_style|=DESK_V3_TOPMOST;
        else if (p->index==DESK_V3_ORDER_BOTTOM || p->index==DESK_V3_ORDER_NOTOPMOST ||
                 (after>=0 && !(d->clients[after].v3_style&DESK_V3_TOPMOST))) {
            c->v3_style&=~DESK_V3_TOPMOST;
            /* SetWindowPos: a window made non-topmost takes its owners and its
             * owned windows out of the topmost band (a topmost owner cannot
             * own a non-topmost window). */
            unsigned id=c->owner_id;
            for (int n=0;id && n<DESK_CLIENTS;n++) {
                int k=-1;for (int j=0;j<DESK_CLIENTS;j++) if (d->clients[j].window_id==id) { k=j;break; }
                if (k<0) break;
                d->clients[k].v3_style&=~DESK_V3_TOPMOST;id=d->clients[k].owner_id;
            }
            for (int k=0;k<DESK_CLIENTS;k++) {
                unsigned up=d->clients[k].window ? d->clients[k].owner_id : 0;
                for (int n=0;up && n<DESK_CLIENTS;n++) {
                    if (up==c->window_id) { d->clients[k].v3_style&=~DESK_V3_TOPMOST; break; }
                    int j=-1;for (int q=0;q<DESK_CLIENTS;q++) if (d->clients[q].window_id==up) { j=q;break; }
                    up=j<0 ? 0 : d->clients[j].owner_id;
                }
            }
        } else if (after>=0) c->v3_style|=d->clients[after].v3_style&DESK_V3_TOPMOST;
        int list[DESK_CLIENTS], n=0;
        for (int k=0;k<DESK_CLIENTS;k++) if (k!=i && d->clients[k].window) {
            int at=n++;
            while (at && d->clients[list[at-1]].order>d->clients[k].order) { list[at]=list[at-1];at--; }
            list[at]=k;
        }
        int at=n;
        if (p->index==DESK_V3_ORDER_BOTTOM) at=0;
        else if (after>=0) { for (at=0;at<n && list[at]!=after;at++); }
        memmove(list+at+1,list+at,(size_t)(n-at)*sizeof(*list));list[at]=i;n++;
        for (int k=0;k<n;k++) d->clients[list[k]].order=++d->order;
    }
    /* Propagate the owner's band through every owned descendant. */
    for (int pass=0;pass<DESK_CLIENTS;pass++) for (int k=0;k<DESK_CLIENTS;k++) {
        struct desk_client *w=&d->clients[k];
        for (int j=0;w->window && w->owner_id && j<DESK_CLIENTS;j++)
            if (d->clients[j].window_id==w->owner_id && (d->clients[j].v3_style&DESK_V3_TOPMOST)) w->v3_style|=DESK_V3_TOPMOST;
    }
    desk_v3_restack(d);
    desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,(int)d->height},d->width,d->height);
    return 0;
}
static void input_state(struct desktop *d, int i, uint32_t request)
{
    struct desk_client *c=&d->clients[i];
    struct ciuki_message m;
    unsigned offset=c->version==DESK_V3_VERSION ? 16 : 4;
    desk_message(&m,DESK_INPUT_STATE,offset+DESK_INPUT_VALUES*4+8);
    if(offset==16) {
        m.data[0]=DESK_V3_VERSION;desk_put32(m.data+4,request);
        desk_put32(m.data+8,c->v3_id);desk_put32(m.data+12,c->v3_generation);
    }
    for(unsigned k=1;k<=DESK_INPUT_VALUES;k++)desk_put32(m.data+offset+(k-1)*4,(unsigned)*cfg_input_field(&d->sh.cfg,k));
    desk_put32(m.data+offset+DESK_INPUT_VALUES*4,d->input_change_key);
    desk_put32(m.data+offset+DESK_INPUT_VALUES*4+4,d->input_change_flags);
    desk_queue(d,i,&m);
}
void desk_preferences_persistent(const struct desktop *d,struct ciuki_settings *out)
{
    *out=d->sh.cfg;
    for(unsigned k=1;k<=DESK_INPUT_VALUES;k++)if(d->input_volatile_mask&(1u<<(k-1)))
        *cfg_input_field(out,k)=(int)d->input_saved[k-1];
}
void desk_preferences_changed(struct desktop *d)
{
    d->shell_caret_hidden=0;d->shell_caret_next=0;
    d->cursor_dirty=1;
    desk_damage_add(&d->damage,(struct desk_box){0,0,(int)d->width,(int)d->height},d->width,d->height);
    for(int i=0;i<DESK_CLIENTS;i++)if(d->clients[i].hello && d->clients[i].window) input_state(d,i,0);
}
static int input_setting(struct desktop *d, const struct desk_packet *p)
{
    if(!p->index)return 0;
    unsigned old=(unsigned)*cfg_input_field(&d->sh.cfg,p->index);
    if(cfg_input_set(&d->sh.cfg,p->index,p->values[0]))return -EINVAL;
    unsigned bit=1u<<(p->index-1);
    if(p->flags&1)d->input_volatile_mask&=~bit;
    else if(!(d->input_volatile_mask&bit)) {d->input_saved[p->index-1]=old;d->input_volatile_mask|=bit;}
    if(p->flags&1)d->sh.want_save=1;
    d->input_change_key=p->index;d->input_change_flags=p->flags;
    desk_preferences_changed(d);
    d->input_change_key=d->input_change_flags=0;
    return 0;
}

int desk_v3_receive(struct desktop *d, int peer, const struct ciuki_message *m)
{
    struct desk_client *anchor=&d->clients[peer];
    if (anchor->v3_peer) peer=(int)anchor->v3_peer-1;
    anchor=&d->clients[peer];
    unsigned id=desk_get32(m->data+8), generation=desk_get32(m->data+12);
    int i=v3_find(d,peer,id,generation);
    struct ciuki_surface_info info={0};
    if (i>=0) info=d->clients[i].info;
    struct desk_packet p;
    /* q3exec2/q3exec3: a per-window message is checked against the window
     * it names. One that names a window this desktop no longer has (closed,
     * dropped with its owner, or window/generation 0 after the program
     * cleared its id) or a frame update whose rectangle no longer fits the
     * window's surface is stale and is refused alone. Dropping the
     * connection for it removed every window of the program: the Quake III
     * Arena demo installer lost its background and every wizard page, whose
     * DialogBoxParam then failed and was taken as Next (a DESK_PONG with
     * window 0 in the QEMU run of main a347ca76b). A packet that no live
     * window and surface could make valid, one from another sender, one
     * carrying descriptors or one out of the HELLO order still ends the
     * connection. */
    if (anchor->channel>=0 && anchor->hello && anchor->version==DESK_V3_VERSION &&
        m->sender_pid && (!anchor->pid || anchor->pid==m->sender_pid) && !m->fd_count &&
        m->length>=16 && m->data[0]==DESK_V3_VERSION && m->data[1]!=DESK_HELLO &&
        !desk_parse(m,1,&info,&p)) {
        const struct ciuki_surface_info any={sizeof(any),CIUKI_SURFACE_DIMENSION_MAX,CIUKI_SURFACE_DIMENSION_MAX,
            CIUKI_SURFACE_DIMENSION_MAX*4,CIUKI_SURFACE_XRGB8888,CIUKI_SURFACE_MAX};
        struct ciuki_message named=*m;
        if (!id || !generation) { desk_put32(named.data+8,id ? id : 1); desk_put32(named.data+12,generation ? generation : 1); }
        if (desk_parse(&named,1,&any,&p)) {
            int error=i<0 ? -ENOENT : -EINVAL;
            p.window_id=id; p.generation=generation;
            if (d->v3_stale_updates++<8)
                fprintf(stderr,"[desktop] v3 stale update pid=%u opcode=%u window=%u generation=%u error=%d\n",
                    anchor->pid,p.opcode,id,generation,error);
            /* A RESULT must name a window (protocol.c): a request naming
             * none is refused without one and the program's wait ends on
             * its own timeout. */
            if (p.request && id && generation) v3_reply(d,peer,&p,error);
            return error;
        }
    }
    if (anchor->channel<0 || (anchor->hello && anchor->version!=DESK_V3_VERSION) ||
        !m->sender_pid || (anchor->pid && anchor->pid!=m->sender_pid) ||
        (m->fd_count==1 && d->ops.info(m->fds[0],&info)) || !desk_parse(m,1,&info,&p) ||
        (!anchor->hello && p.opcode!=DESK_HELLO) || (anchor->hello && p.opcode==DESK_HELLO)) {
        /* Every window of the program goes with the connection: say so. */
        if (anchor->channel>=0)
            fprintf(stderr,"[desktop] v3 drop pid=%u sender=%u opcode=%u length=%u fds=%u window=%u generation=%u\n",
                anchor->pid,m->sender_pid,m->length>1 ? m->data[1] : 0,m->length,m->fd_count,id,generation);
        close_attachments(d,m,-1); desk_drop_client(d,peer); return -EINVAL;
    }
    if (p.opcode==DESK_HELLO) {
        anchor->pid=m->sender_pid; anchor->hello=1; anchor->version=DESK_V3_VERSION; return 0;
    }
    if(p.opcode==DESK_INPUT_SETTINGS && !id && !generation) {
        int error=input_setting(d,&p);
        if(!error)input_state(d,peer,p.request);
        if(p.request)v3_reply(d,peer,&p,error);
        return error;
    }
    if (p.opcode==DESK_V3_CLIPBOARD && desk_clip_receive) { desk_clip_receive(d,peer,m,&p); return 0; }
    int error=0;
    if (!p.request && p.opcode!=DESK_PONG) error=-EINVAL;
    if (p.opcode==DESK_CREATE_WINDOW && !error) {
        int owner=p.owner ? v3_find(d,peer,p.owner,p.owner_generation) : -1;
        if (!id || id>DESK_CLIENTS || generation<=anchor->v3_last[id-1]) error=-ENOENT;
        else if (p.owner && owner<0) error=-ENOENT;
        else {
            for (int k=0;k<DESK_CLIENTS;k++) if (d->clients[k].v3_peer==(unsigned)peer+1 && d->clients[k].v3_id==id) error=-EEXIST;
            int slot=-1;
            for (int k=0;k<DESK_CLIENTS;k++) if (d->clients[k].channel<0 && !d->clients[k].internal) { slot=k; break; }
            if (!error && slot<0) error=-ENOSPC;
            if (!error) {
                void *pixels=d->ops.map(m->fds[0],PROT_READ);
                if (!pixels || pixels==MAP_FAILED) error=-ENOMEM;
                else {
                    i=slot; struct desk_client *c=&d->clients[i]; memset(c,0,sizeof(*c));
                    c->channel=anchor->channel; c->surface=m->fds[0]; c->pixels=pixels; c->info=info;
                    c->hello=1; c->version=DESK_V3_VERSION; c->window=1; c->pid=anchor->pid;
                    c->v3_peer=(unsigned)peer+1; c->v3_id=id; c->v3_generation=generation; c->v3_scale=p.scale;
                    c->v3_style=p.style; c->next_ping_ns=d->now_ns+1000000000ull;
                    if (!++d->next_window_id) ++d->next_window_id;
                    c->window_id=d->next_window_id; c->order=++d->order;
                    c->owner_id=owner>=0 ? d->clients[owner].window_id : 0;
                    if (owner>=0 && (d->clients[owner].v3_style&DESK_V3_TOPMOST)) p.style|=DESK_V3_TOPMOST;
                    memcpy(c->title,p.title,sizeof(c->title)); anchor->v3_last[id-1]=generation;
                    if (anchor->awaiting_frame && !anchor->window_ns) anchor->window_ns=client_clock(d);
                    v3_place(d,i,&p); v3_reply(d,peer,&p,0); desk_focus(d,i);
                    /* fx-2: its task button, as for a native window. */
                    if (d->shell) desk_damage_add(&d->damage,desk_taskbar(d),d->width,d->height);
                    return 0;
                }
            }
        }
    } else if (i<0 && !error) error=-ENOENT;
    if (error) { close_attachments(d,m,-1); if (p.request) v3_reply(d,peer,&p,error); return error; }
    struct desk_client *c=&d->clients[i];
    switch (p.opcode) {
    case DESK_LAUNCH: {
        if (!strcmp(p.title,"netsurf")) {
            /* G26: URL-only launch from NetSurf (a link opened in a new browser window). */
            if (strcmp(anchor->application,"netsurf")) { error=-EPERM; break; }
            const char *args[]={p.argument};
            desk_launch_origin(d,DESK_LAUNCH_SHELL,NULL);
            error=desk_task_launch_args(d,"netsurf",args,1);
            if (error>0) error=0;
            break;
        }
        if (!desk_help_argument(p.argument)) { error=-EPERM; break; }
        struct desk_help_request request;
        int encoded=desk_help_decode(p.argument,&request);
        int quit=encoded && ((request.format==1 && request.command==2) || (request.format==2 && request.command==18));
        if (quit) {
            unsigned users=0;
            for (unsigned k=0;k<DESK_CLIENTS;k++) { if (d->help_users[k]==c->pid) d->help_users[k]=0; users+=d->help_users[k]!=0; }
            if (users) break;
            int found=d->help_starting;
            for (unsigned k=0;k<DESK_CLIENTS;k++) if (d->clients[k].channel>=0 && !strcmp(d->clients[k].application,"help")) found=1;
            if (!found) break;
        } else {
            unsigned free_slot=DESK_CLIENTS;
            for (unsigned k=0;k<DESK_CLIENTS;k++) {
                if (d->help_users[k]==c->pid) { free_slot=DESK_CLIENTS; break; }
                if (!d->help_users[k]) free_slot=k;
            }
            if (free_slot<DESK_CLIENTS) d->help_users[free_slot]=c->pid;
        }
        int result=desk_task_launch(d,"help",p.argument); if (result<0) error=result;
        break;
    }
    case DESK_V3_TRAY_ICON:
        error=desk_shell_program_icon(d,i,&p,&info,m->fd_count ? m->fds[0] : -1);
        if (!error && m->fd_count) d->ops.close(m->fds[0]);
        break;
    case DESK_V3_TRAY_BALLOON:
        error=desk_shell_program_balloon(d,i,&p); break;
    case DESK_ACCEPT_FILES:
        c->accept_files=(int)p.flags; break;
    case DESK_V3_FLASH:
        c->flash=p.flags ? !c->flash : 0;
        desk_damage_add(&d->damage,desk_outer(c),d->width,d->height);
        desk_damage_add(&d->damage,desk_task_button(d,i),d->width,d->height); break;
    case DESK_POWER_REPLY:
        if (!p.serial) c->power_aware=1;
        else desk_power_reply(d,i,p.serial,p.flags);
        break;
    case DESK_V3_LAUNCH:
        error=(!netsurf_wallpaper_request(c,&p) &&
               !( !strcmp(anchor->application,"netsurf") && !strcmp(p.title,"files") && desk_browser_file_argument(p.argument))) || !d->shell
              ? -EPERM : desk_task_launch(d,p.title,p.argument);
        if (error>0) error=0; /* A positive PID means the Control Panel was started. */
        d->launch_deferred=0; /* V3 replies report accepted queueing, not a later v2 TASK_RESULT. */
        break;
    case DESK_INPUT_SETTINGS:
        error=input_setting(d,&p);
        if(!error)input_state(d,i,p.request);
        break;
    case DESK_DAMAGE: case DESK_V3_PRESENT: {
        if (p.opcode==DESK_V3_PRESENT && (!p.request || c->gl_present_request || c->minimized || c->collapsed || !p.width || !p.height)) { error=-EAGAIN;break; }
        if (!p.width || !p.height) break;
        /* fx-15: a v3 program (NetSurf, a Win32 program) was launched on its
         * transport slot; its first window's first frame ends AppStarting
         * and completes the launch there (the STARTUPINFO feedback rule). */
        if (anchor->awaiting_frame && c->window) {
            c->awaiting_frame=1; c->launch_input_ns=anchor->launch_input_ns; c->launched_ns=anchor->launched_ns;
            c->window_ns=anchor->window_ns; anchor->awaiting_frame=0; client_frame_ready(d,c);
        }
        /* A pending resize stretches the old backing (surface_damage). */
        int visible=desk_client_damage(d,i,surface_damage(c,(uint32_t)p.x,(uint32_t)p.y,p.width,p.height));
        if(p.opcode==DESK_V3_PRESENT && visible){c->gl_present_request=p.request;return 0;}
        break;
    }
    case DESK_V3_SOUND:
        desk_sound(d,p.flags); return 0;      /* G16 MessageBeep: no reply (the event is checked by the sound runtime) */
    case DESK_V3_WIN32_HWND:
        if(c->v3_win32_hwnd && c->v3_win32_hwnd!=p.flags)error=-EINVAL;
        else c->v3_win32_hwnd=p.flags;
        break;
    case DESK_V3_WINDOW_ORDER:
        error=v3_order(d,peer,i,&p); break;
    case DESK_V3_LIMITS:
        /* WM_GETMINMAXINFO: remember the track sizes and hold the window to them. */
        c->v3_limits[0]=p.min_width; c->v3_limits[1]=p.min_height; c->v3_limits[2]=p.max_width; c->v3_limits[3]=p.max_height;
        v3_apply_limits(c);
        if (c->resizable && !c->collapsed && !c->minimized) desk_resize(d,i,c->frame);
        break;
    case DESK_V3_PLACE:
        if ((p.style&DESK_V3_MODAL) && !c->owner_id) { error=-EINVAL; break; }
        if (c->owner_id) for (int k=0;k<DESK_CLIENTS;k++) if (d->clients[k].window_id==c->owner_id &&
            (d->clients[k].v3_style&DESK_V3_TOPMOST)) p.style|=DESK_V3_TOPMOST;
        v3_place(d,i,&p); if (c->minimized && d->focus==i) desk_focus(d,desk_next_window(d,i,0,NULL));
        break;
    case DESK_V3_RESIZE: {
        struct desk_box b=desk_content(c);
        if (p.serial!=c->resize_serial || p.serial<=c->applied_serial || p.width!=(unsigned)b.w/c->v3_scale || p.height!=(unsigned)b.h/c->v3_scale) error=-ENOENT;
        else {
            void *pixels=d->ops.map(m->fds[0],PROT_READ);
            if (!pixels || pixels==MAP_FAILED) error=-ENOMEM;
            else {
                const void *old=c->pixels; unsigned bytes=c->info.allocation_bytes; int oldfd=c->surface;
                c->pixels=pixels; c->info=info; c->surface=m->fds[0]; c->applied_serial=p.serial;
                v3_reply(d,peer,&p,0); d->ops.unmap((void *)old,bytes); d->ops.close(oldfd);
                desk_damage_add(&d->damage,b,d->width,d->height); return 0;
            }
        }
        break;
    }
    case DESK_CLOSE_ACK:
        if (c->closing && !p.flags) { c->closing=0; desk_damage_add(&d->damage,desk_outer(c),d->width,d->height); }
        break;
    case DESK_V3_ACTIVATE:
        if (p.focused) { c->minimized=0; desk_focus(d,i); v3_configure(d,i); }
        else if (d->focus==i) desk_focus(d,desk_next_window(d,i,0,NULL));
        break;
    case DESK_SET_TITLE:
        if(strcmp(c->title,p.title)) { memcpy(c->title,p.title,sizeof(c->title));desk_caption_damage(d,i); }
        break;
    case DESK_SET_CURSOR:
        if (p.index && !(c->v3_cursor_valid&(1u<<(p.index-1)))) error=-ENOENT;
        else { c->cursor=p.flags; c->v3_cursor=p.index; c->v3_visibility=p.result; d->cursor_loaded=DESK_CURSOR_COUNT; desk_cursor_update(d); }
        break;
    case DESK_V3_CURSOR_BITMAP: {
        void *pixels=d->ops.map(m->fds[0],PROT_READ);
        if (!pixels || pixels==MAP_FAILED) error=-ENOMEM;
        else {
            memcpy(c->v3_cursor_pixels[p.index-1],pixels,4096); c->v3_cursor_valid|=1u<<(p.index-1);
            c->v3_hot_x[p.index-1]=(uint8_t)p.x; c->v3_hot_y[p.index-1]=(uint8_t)p.y;
            d->ops.unmap(pixels,info.allocation_bytes); d->cursor_loaded=DESK_CURSOR_COUNT; d->cursor_dirty=1;
        }
        if (!error) close_attachments(d,m,-1);
        break;
    }
    case DESK_V3_POINTER: case DESK_V3_WARP:
        if (d->focus<0 || d->clients[d->focus].pid!=c->pid || c->minimized || d->power.state || d->sh.user_gate || d->sh.levels) { error=-EPERM; break; }
        if (p.opcode==DESK_V3_POINTER) {
            struct desk_box b=desk_content(c), clip={b.x+p.x*(int)c->v3_scale,b.y+p.y*(int)c->v3_scale,
                (int)p.width*(int)c->v3_scale,(int)p.height*(int)c->v3_scale}, screen={0,0,(int)d->width,(int)d->height};
            if ((p.flags&DESK_V3_CLIP) && !desk_intersect(clip,screen,&clip)) { error=-EINVAL; break; }
            desk_v3_pointer_release(d); c->v3_pointer=p.flags; c->v3_clip=clip;
            d->v3_pointer_window=p.flags ? i+1 : 0;
            if (p.flags&DESK_V3_CLIP) {
                if (d->mouse_x<clip.x) d->mouse_x=clip.x;
                if (d->mouse_x>=clip.x+clip.w) d->mouse_x=clip.x+clip.w-1;
                if (d->mouse_y<clip.y) d->mouse_y=clip.y;
                if (d->mouse_y>=clip.y+clip.h) d->mouse_y=clip.y+clip.h-1;
                desk_cursor_motion(d);
            }
            struct ciuki_message state; desk_v3_message(&state,DESK_V3_POINTER_STATE,4,0,c->v3_id,c->v3_generation);
            desk_put32(state.data+16,p.flags); desk_queue(d,i,&state);
        } else {
            struct desk_box b=desk_content(c); int64_t x=(int64_t)b.x+(int64_t)p.x*c->v3_scale, y=(int64_t)b.y+(int64_t)p.y*c->v3_scale;
            struct desk_box clip={0,0,(int)d->width,(int)d->height};
            if (d->v3_pointer_window && (d->clients[d->v3_pointer_window-1].v3_pointer&DESK_V3_CLIP)) clip=d->clients[d->v3_pointer_window-1].v3_clip;
            if (x<clip.x) x=clip.x;
            if (x>=clip.x+clip.w) x=clip.x+clip.w-1;
            if (y<clip.y) y=clip.y;
            if (y>=clip.y+clip.h) y=clip.y+clip.h-1;
            d->mouse_x=(int32_t)x; d->mouse_y=(int32_t)y; desk_cursor_motion(d);
        }
        desk_cursor_update(d); break;
    case DESK_V3_LEASE:
        if (!p.flags) {
            c->v3_lease_wanted=0;
            if(d->v3_reactivate_pending==i+1)d->v3_reactivate_pending=0;
            if (d->v3_lease_state && d->v3_lease_window!=c->window_id) error=-EPERM;
            else if (d->v3_lease_state) { desk_v3_revoke(d); if (d->v3_lease_state) error=-EBUSY; }
        }
        else if (d->focus!=i || d->power.state || d->sh.user_gate || d->sh.levels || d->v3_lease_state) error=-EBUSY;
        else if (!d->v3_lease) error=-ENOSYS;
        else {
            if (!++d->v3_lease_generation) ++d->v3_lease_generation;
            d->v3_lease_error=0;
            error=d->v3_lease(d,c->pid,1,p.width,p.height,p.bpp,d->v3_lease_generation);
            if (!error) { c->v3_lease_wanted=1; c->v3_lease_w=p.width; c->v3_lease_h=p.height; c->v3_lease_bpp=p.bpp;
                d->v3_lease_started=d->now_ns; d->v3_lease_state=1; d->v3_lease_window=c->window_id; d->v3_lease_pid=c->pid;
                d->v3_lease_w=p.width; d->v3_lease_h=p.height; d->v3_lease_bpp=p.bpp; c->v3_lease_mode=d->v3_lease_tuple.mode; v3_lease_event(d,i,1); }
        }
        break;
    case DESK_V3_CRASH: case DESK_V3_RESTART: {
        if (d->v3_dialog_kind) { error=-EBUSY; break; }
        if (p.opcode==DESK_V3_RESTART && !d->v3_restart) { error=-ENOSYS; break; }
        desk_v3_revoke(d); desk_v3_pointer_release(d);
        d->v3_dialog_kind=p.opcode==DESK_V3_CRASH ? 1 : 2; d->v3_dialog_pid=c->pid; d->v3_restart_flags=p.flags;
        char text[320];
        if (p.opcode==DESK_V3_CRASH) {
            struct ciuki_w32_fault fault={.kind=W32_REPORT_CRASH,.pid=c->pid,.code=p.flags,.eip=p.values[0]};
            snprintf(fault.exe,sizeof(fault.exe),"%s",p.title);snprintf(fault.module,sizeof(fault.module),"%s",p.title);
            desk_w32_fault_dialog(d,&fault);break;
        }
        else snprintf(text,sizeof(text),"Setup wants to restart Windows. CiukiOS will close all Windows programs and finish the installation.");
        desk_shell_message(d,p.opcode==DESK_V3_CRASH ? "Windows program error" : "Restart CiukiOS",text,p.opcode==DESK_V3_CRASH);
        break;
    }
    case DESK_CLOSE:
        v3_reply(d,peer,&p,0);
        { struct ciuki_message closed; desk_v3_message(&closed,DESK_CLOSED,0,0,id,generation); desk_queue(d,i,&closed); }
        for (int k=0;k<DESK_CLIENTS;k++) if (d->clients[k].owner_id==c->window_id) desk_drop_client(d,k);
        desk_drop_client(d,i); return 0;
    case DESK_PING: {
        struct ciuki_message pong; desk_v3_message(&pong,DESK_PONG,4,0,id,generation); desk_put32(pong.data+16,p.serial); desk_queue(d,i,&pong); break;
    }
    case DESK_PONG:
        if (c->ping_pending && p.serial==c->serial) {
            if (c->unresponsive) {
                desk_damage_add(&d->damage,desk_outer(c),d->width,d->height);
                if (d->shell) desk_damage_add(&d->damage,desk_task_button(d,i),d->width,d->height);
            }
            c->ping_pending=c->ping_queued=0; c->unresponsive=0; c->next_ping_ns=d->now_ns+1000000000ull;
        }
        return 0;
    default: error=-EINVAL; break;
    }
    if (error) close_attachments(d,m,-1);
    v3_reply(d,peer,&p,error); return error;
}

int desk_v3_mode(const struct ciuki_display_info *info, unsigned w, unsigned h, unsigned bpp)
{
    if (!info || !w || !h || w>2048 || h>2048 || (bpp!=8 && bpp!=16 && bpp!=32) || info->mode_count>32) return -EINVAL;
    int best=-1; unsigned best_integer=0, best_area=0;
    for (unsigned k=0;k<info->mode_count;k++) {
        const struct ciuki_display_mode *m=&info->modes[k];
        if (!m->linear || m->width<w || m->height<h ||
            (m->bpp!=8 && m->bpp!=16 && m->bpp!=32) ||
            m->pitch<(unsigned)m->width*((m->bpp+7)/8)) continue;
        if (m->width==w && m->height==h && m->bpp==bpp) return m->id;
        if (m->bpp!=16 && m->bpp!=32) continue;
        unsigned scale=m->width/w, sy=m->height/h;
        unsigned integer=scale==sy && scale>=1 && scale<=3 && !(m->width%w) && !(m->height%h);
        unsigned area=(unsigned)m->width*m->height;
        if (best<0 || integer>best_integer || (integer==best_integer && (area<best_area ||
            (area==best_area && m->bpp==bpp)))) { best=(int)k; best_integer=integer; best_area=area; }
    }
    return best>=0 ? info->modes[best].id : -ENODEV;
}
void desk_v3_reactivate(struct desktop *d, int i)
{
    if (i<0 || d->v3_lease_state || d->power.state || d->sh.user_gate || d->sh.levels || d->sh.switcher || !d->v3_lease) return;
    struct desk_client *c=&d->clients[i];
    if (!c->v3_lease_wanted) {d->v3_reactivate_pending=0;return;}
    /* Finish the restored desktop frame before suspending it again. Otherwise
     * a rapid activation can strand the restoration watchdog behind a healthy
     * new exclusive lease. Failed repaint defers regrant to heartbeat recovery. */
    if(d->v3_restoring) {
        d->v3_reactivate_pending=i+1;
        if(desk_redraw(d))return;
        v3_restore_poll(d,d->time_ns ? d->time_ns() : d->now_ns);
        if(d->v3_restoring)return;
    }
    d->v3_reactivate_pending=0;
    d->v3_lease_w=c->v3_lease_w;d->v3_lease_h=c->v3_lease_h;d->v3_lease_bpp=c->v3_lease_bpp;
    if (!++d->v3_lease_generation) ++d->v3_lease_generation;
    d->v3_lease_error=0;
    d->v3_lease_preferred_mode=c->v3_lease_mode;
    int error=d->v3_lease(d,c->pid,1,c->v3_lease_w,c->v3_lease_h,c->v3_lease_bpp,d->v3_lease_generation);
    d->v3_lease_preferred_mode=0;
    if (error) {d->v3_lease_error=error;v3_lease_event(d,i,0);return;}
    d->v3_lease_started=d->now_ns;
    d->v3_lease_state=1; d->v3_lease_window=c->window_id; d->v3_lease_pid=c->pid;
    d->v3_lease_w=c->v3_lease_w; d->v3_lease_h=c->v3_lease_h; d->v3_lease_bpp=c->v3_lease_bpp;
    v3_lease_event(d,i,1);
}
