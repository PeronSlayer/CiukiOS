/* SPDX-License-Identifier: MIT */
#include "desktop.h"
#include "gate.h"
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <signal.h>
#include <stdio.h>
static void power_target(struct desktop *, uint32_t, const char *, int);
int desk_ignore_sigpipe(void)
{ return signal(SIGPIPE,SIG_IGN)==SIG_ERR ? -1 : 0; }
void desk_init(struct desktop *d, const struct desk_ops *ops, uint32_t w, uint32_t h)
{
    memset(d, 0, sizeof(*d));
    d->ops = *ops; d->width = w; d->height = h;
    d->display = d->input = d->output = d->focus = d->drag = d->about = d->authority_fd = d->mixer_fd = -1;
    d->action = -1;
    d->mouse_x = (int32_t)w / 2; d->mouse_y = (int32_t)h / 2;
    d->clock_second = UINT64_MAX;
    d->title_click_window = -1;
    for (unsigned i = 0; i < DESK_CLIENTS; i++)
        d->clients[i].channel = d->clients[i].surface = -1;
    desk_damage_add(&d->damage, (struct desk_box){0, 0, (int32_t)w, (int32_t)h}, w, h);
}
void desk_theme_changed(struct desktop *d)
{ d->sh.background_valid=0; desk_damage_add(&d->damage,(struct desk_box){0,0,(int32_t)d->width,(int32_t)d->height},d->width,d->height); }
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
    if (d->focus == i) desk_focus(d, -1);
    if (d->drag == i) d->drag = -1;
    if (c->window) desk_sound(d,SOUND_WINDOW_CLOSE);
    if (c->window) desk_damage_add(&d->damage, desk_outer(c), d->width, d->height);
    if (c->pixels && !c->internal) d->ops.unmap((void *)c->pixels, c->info.allocation_bytes);
    if (c->surface >= 0) d->ops.close(c->surface);
    if (c->channel >= 0) d->ops.close(c->channel);
    if (d->about == i) d->about = -1;
    if (c->internal) desk_internal_closed(d, i);
    memset(c, 0, sizeof(*c));
    c->channel = c->surface = -1;
    if (d->focus < 0) {
        int next = -1;
        for (int j = 0; j < DESK_CLIENTS; j++) if (d->clients[j].window && !d->clients[j].minimized &&
            (next < 0 || d->clients[j].order > d->clients[next].order)) next = j;
        desk_focus(d, next);
    }
    if (d->shell) desk_damage_add(&d->damage,desk_taskbar(d),d->width,d->height);
}
void desk_queue(struct desktop *d, int i, const struct ciuki_message *m)
{
    if (i < 0 || i >= DESK_CLIENTS) return;
    struct desk_client *c = &d->clients[i];
    if (c->channel < 0) return;
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
    unsigned budget = DESK_TX_MAX;
    while (c->channel >= 0 && c->tx_count && budget--) {
        int r = d->ops.send(c->channel, &c->tx[c->tx_head], DONTWAIT);
        if (r < 0) {
            if (errno == EPIPE || errno == EBADF) desk_drop_client(d, i);
            return;
        }
        d->replied++;
        c->tx_head = (c->tx_head + 1) % DESK_TX_MAX; c->tx_count--;
    }
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
            desk_message(&m,c->version==DESK_SHELL_VERSION ? DESK_CLOSE_REQUEST : DESK_CLOSED,4);
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
    int32_t w = size_limit(f.w - 2 * DESK_BORDER, c->min_width, c->max_width, c->step_width);
    int32_t h = size_limit(f.h - DESK_TITLE - DESK_BORDER, c->min_height, c->max_height, c->step_height);
    /* The opposite edge stays put when tracking a left/top edge. */
    if (d->drag == i && (d->resize_edges & DESK_EDGE_LEFT)) f.x += f.w - w - 2 * DESK_BORDER;
    if (d->drag == i && (d->resize_edges & DESK_EDGE_TOP)) f.y += f.h - h - DESK_TITLE - DESK_BORDER;
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
/* Only a successfully presented frame in the requested/confirmed mode may
 * replace the saved preference. Failed reallocations still have the old id. */
int desk_display_frame_commit(struct desktop *d, int redraw)
{
    if (redraw || !d->display_pending || d->display_pending != d->display_mode || d->damage.count) return 0;
    d->sh.cfg.display_mode = d->display_pending;
    d->display_pending = 0;
    return 1;
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
    d->display_reply_mode=0;
}

/* Called only after allocating the new screen surface. Client surfaces stay
 * attached until their CONFIGURE/DAMAGE handshake replaces them. */
void desk_display_relayout(struct desktop *d, uint32_t width, uint32_t height)
{
    desk_render_release(d);
    d->width = width; d->height = height;
    d->drag = -1; d->resize_edges = 0;
    d->title_hot = d->title_down = 0;
    if (d->mouse_x >= (int32_t)width) d->mouse_x = (int32_t)width - 1;
    if (d->mouse_y >= (int32_t)height) d->mouse_y = (int32_t)height - 1;
    int32_t work = (int32_t)height - DESK_BAR - (d->shell ? DESK_BAR : 0);
    for (int i = 0; i < DESK_CLIENTS; i++) {
        struct desk_client *c = &d->clients[i];
        if (!c->window) continue;
        if (c->collapsed && c->resizable) desk_collapse(d, i);
        if (c->resizable && !c->internal) {
            struct desk_box f = c->frame;
            if (c->zoomed || f.w > (int32_t)width) f.w = (int32_t)width;
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
        desk_move(d, i, c->zoomed ? 0 : c->frame.x, c->zoomed ? DESK_BAR : c->frame.y);
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
    if (!c->resizable || c->internal) return;
    if (c->collapsed) desk_collapse(d, i);
    struct desk_box f;
    if (c->zoomed) f = c->restore;
    else {
        c->restore = c->frame;
        f = (struct desk_box){0, DESK_BAR, (int32_t)d->width, (int32_t)d->height - DESK_BAR - (d->shell ? DESK_BAR : 0)};
    }
    c->zoomed = !c->zoomed; desk_resize(d, i, f);
}
void desk_move(struct desktop *d, int i, int32_t x, int32_t y)
{
    struct desk_client *c = &d->clients[i];
    int32_t max_x = (int32_t)d->width - c->frame.w;
    int32_t max_y = (int32_t)d->height - c->frame.h - (d->shell ? DESK_BAR : 0);
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
        d->copy_rect.width==(uint32_t)frame.w && d->copy_rect.height==(uint32_t)frame.h;
    int copy = d->accel && (d->accel_caps & 1u) && d->hw_cursor &&
        d->accel_front_valid && !d->present_damage.count && (continuing || (!d->copy_pending && !d->damage.count)) && !d->menu_open && !d->sh.levels &&
        !d->sh.volume_open && !d->sh.switcher && !d->sh.tooltip && !d->power.state &&
        !c->internal && !c->collapsed && !c->minimized;
    struct desk_box next = {x,y,frame.w,frame.h}, ignored;
    if (frame.x < 0 || frame.y < 0 || frame.x+frame.w > (int32_t)d->width ||
        frame.y+frame.h > (int32_t)d->height) copy=0;
    for (int j=0; j<DESK_CLIENTS && copy; j++) {
        struct desk_client *other=&d->clients[j];
        if (j!=i && other->window && !other->minimized && other->order>c->order &&
            (desk_intersect(desk_outer(other),frame,&ignored) || desk_intersect(desk_outer(other),next,&ignored))) copy=0;
    }
    if (d->copy_pending && !copy) { d->copy_pending=0; desk_damage_add(&d->damage,frame,d->width,d->height); }
    c->frame.x = x; c->frame.y = y;
    if (copy) {
        if (continuing) { d->copy_rect.dst_x=x; d->copy_rect.dst_y=y; }
        else d->copy_rect=(struct ciuki_rect){frame.x,frame.y,x,y,(uint32_t)frame.w,(uint32_t)frame.h};
        d->copy_pending=1; d->copy_client=i;
    }
    desk_damage_add(&d->damage,old,d->width,d->height);
    desk_damage_add(&d->damage,desk_outer(c),d->width,d->height);
}
static void close_attachments(struct desktop *d, const struct ciuki_message *m, int keep)
{
    for (unsigned k = 0; k < m->fd_count && k < 4; k++)
        if (m->fds[k] >= 0 && m->fds[k] != keep) d->ops.close(m->fds[k]);
}
void desk_receive(struct desktop *d, int i, const struct ciuki_message *m)
{
    struct desk_client *c = &d->clients[i];
    struct desk_packet p;
    struct ciuki_surface_info s = c->info;
    int create = m->length >= 4 && (m->data[1] == DESK_CREATE_WINDOW || m->data[1] == DESK_DAMAGE) && m->fd_count == 1;
    int info_ok = !create || !d->ops.info(m->fds[0], &s);
    if (c->channel < 0 || !info_ok || !desk_parse(m, 1, &s, &p) ||
        !m->sender_pid || (c->pid && c->pid != m->sender_pid) ||
        (c->hello && (c->version != m->data[0] || c->minor != m->data[2])) ||
        (!c->hello && p.opcode != DESK_HELLO) ||
        (c->hello && p.opcode == DESK_HELLO) ||
        (p.opcode == DESK_CREATE_WINDOW && c->window) ||
        ((p.opcode == DESK_DAMAGE || p.opcode == DESK_MOVE || p.opcode == DESK_CLOSE || p.opcode == DESK_SET_TITLE) && !c->window) ||
        (p.opcode == DESK_DAMAGE && p.serial && (!c->resizable || p.serial > c->resize_serial ||
            p.serial <= c->applied_serial || s.width < c->min_width || s.height < c->min_height ||
            s.width > c->max_width || s.height > c->max_height ||
            (p.serial == c->resize_serial &&
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
    case DESK_SYSTEM_QUERY: {
        if (!c->taskman) {
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
        desk_move(d, i, 20 + i * 18, DESK_BAR + 20 + i * 18);
        desk_damage_add(&d->damage, desk_outer(c), d->width, d->height);
        configure(d, i, 1);
        if (c->awaiting_frame && d->launch_mark) d->launch_mark(CIUKI_LAUNCH_CONFIGURE,c->pid,0);
        if (d->shell && d->menu_open) {
            c->order=++d->order; d->menu_focus=i;
            desk_message(&reply,DESK_FOCUS,8); desk_queue(d,i,&reply);
        } else desk_focus(d, i);
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
            void *pixels = d->ops.map(m->fds[0], PROT_READ);
            if (!pixels || pixels == MAP_FAILED) { close_attachments(d, m, -1); desk_drop_client(d, i); return; }
            desk_damage_add(&d->damage, desk_outer(c), d->width, d->height);
            d->ops.unmap((void *)c->pixels, c->info.allocation_bytes); d->ops.close(c->surface);
            c->pixels = pixels; c->surface = m->fds[0]; c->info = s; c->applied_serial = p.serial;
            configure(d, i, 1);
            /* A superseded request already in flight may commit; keep the
             * newest frame geometry and clip this surface until its swap. */
        } else {
            struct desk_box b = desk_content(c);
            b.x += p.x; b.y += p.y; b.w = (int32_t)p.width; b.h = (int32_t)p.height;
            if (!c->minimized && !c->collapsed) desk_damage_add(&d->damage, b, d->width, d->height);
        }
        /* A valid replacement surface can also carry the first rendered frame. */
        if (c->awaiting_frame) {
            c->launch_frame_pending = 1;
            if (d->launch_mark) d->launch_mark(CIUKI_LAUNCH_DAMAGE,c->pid,0);
        }
        c->awaiting_frame = 0;
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
    case DESK_SET_TITLE:
        if (strcmp(c->title, p.title)) {
            memcpy(c->title, p.title, sizeof(c->title));
            if (!c->minimized) desk_damage_add(&d->damage, desk_outer(c), d->width, d->height);
            if (d->shell) desk_damage_add(&d->damage, desk_taskbar(d), d->width, d->height);
        }
        break;
    case DESK_MOVE: desk_move(d, i, p.x, p.y); break;
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
        if (c->taskman) desk_task_windows(d, i, p.serial);
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
        int ide_run = p.opcode == DESK_LAUNCH && c->program == DESK_APP_IDE && !c->taskman &&
                      !strcmp(p.title,"ciukide-run");
        int ide_guide = p.opcode == DESK_LAUNCH && c->program == DESK_APP_IDE && !c->taskman &&
                        !strcmp(p.title,"ciuknote") && !strcmp(p.argument,"/Documents/Developer/Guide.txt");
        int terminal = c->program == DESK_APP_TERMINAL;
        int terminal_launch = terminal && p.opcode == DESK_LAUNCH && desk_program_graphical(p.title) &&
                              (!p.argument[0] || p.argument[0] == '/');
        int terminal_end = terminal && p.opcode == DESK_END_TASK && !p.window_id && p.pid;
        int32_t result = !c->taskman && !files_open && !ide_run && !ide_guide && !terminal_launch && !terminal_end ? -EPERM :
            p.opcode == DESK_END_TASK ? (terminal_end && p.pid == 1 ? -EPERM : desk_task_end(d, p.window_id, p.pid)) :
            p.opcode == DESK_SWITCH_TO ? desk_task_switch(d, p.window_id) :
            c->taskman && p.argument[0] ? -EPERM : ide_run ? desk_ide_launch(d,p.argument) : desk_task_launch(d, p.title, p.argument);
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
        /* The launcher may have reused this slot's neighbours; c stays valid. */
        desk_message(&reply, DESK_TASK_RESULT, 16); desk_put32(reply.data + 4, p.serial);
        desk_put32(reply.data + 8, p.opcode); desk_put32(reply.data + 12, (uint32_t)result);
        desk_queue(d, i, &reply);
        break;
    }
    case DESK_TERMINAL_POWER: {
        int result = c->program != DESK_APP_TERMINAL || !d->shell || d->sh.gate ? -EPERM : d->power.state ? -EBUSY : 0;
        desk_message(&reply, DESK_TASK_RESULT, 16); desk_put32(reply.data + 4, p.serial);
        desk_put32(reply.data + 8, DESK_TERMINAL_POWER); desk_put32(reply.data + 12, (uint32_t)result);
        desk_queue(d, i, &reply);
        if (!result) desk_power_begin(d, (int)p.flags);
        break;
    }
    case DESK_SETTINGS: {
        /* Control Panel settings use the real display callback and share
         * the shutdown guard with the other preference requests. */
        int32_t result = c->program != DESK_APP_CONTROL || !d->shell ? -EPERM : d->power.state ? -EBUSY : 0;
        if (!result && p.request == DESK_SETTINGS_RELOAD) d->sh.want_reload |= 1;
        if (!result && p.request == DESK_SETTINGS_DISPLAY) {
            if (d->display_reply_mode) result=-EBUSY;
            else result = d->display_set ? d->display_set(d, (uint16_t)p.values[0]) : -ENOSYS;
            if (!result && d->display_pending) {
                d->display_reply_client=i; d->display_reply_pid=c->pid;
                d->display_reply_serial=p.serial; d->display_reply_mode=(uint16_t)p.values[0];
                break; /* reply only after the kernel accepts the complete new frame */
            }
        }
        if (!result && p.request == DESK_SETTINGS_MASTER) {
            if (d->mixer_fd < 0) result = -ENODEV;
            else {
                d->sh.volume = (int)((p.values[0] + p.values[1]) / 2); d->sh.muted = (int)p.values[2];
                d->sh.want_mixer = 1;
                d->sh.want_reload |= 8; /* volume feedback after applying gain */
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
        int32_t result = !c->mixer ? -EPERM : d->power.state ? -EBUSY : d->mixer_set ? d->mixer_set(d, p.width, p.height, p.flags) : -ENODEV;
        desk_message(&reply, DESK_TASK_RESULT, 16); desk_put32(reply.data + 4, p.serial);
        desk_put32(reply.data + 8, DESK_MIXER_SET); desk_put32(reply.data + 12, (uint32_t)result);
        desk_queue(d, i, &reply);
        break;
    }
    case DESK_PONG:
        if (c->ping_pending && p.serial == c->serial) {
            c->ping_pending = 0; c->next_ping_ns = d->now_ns + 1000000000ull;
            if (c->unresponsive) {
                c->unresponsive = 0;
                desk_damage_add(&d->damage, desk_outer(c), d->width, d->height);
            }
        }
        break;
    default: break;
    }
}
void desk_heartbeat(struct desktop *d, uint64_t now)
{
    d->now_ns = now;
    if (now / 1000000000ull != d->clock_second) {
        d->clock_second = now / 1000000000ull;
        if (!d->shell) desk_damage_add(&d->damage, (struct desk_box){(int32_t)d->width - 120,
            0, 120, DESK_BAR}, d->width, d->height);
    }
    for (int i = 0; i < DESK_CLIENTS; i++) {
        struct desk_client *c = &d->clients[i];
        if (c->channel < 0 || !c->hello) continue;
        if (!c->ping_pending && now >= c->next_ping_ns && c->tx_count < DESK_TX_MAX) {
            struct ciuki_message m;
            desk_message(&m, DESK_PING, 8); desk_put32(m.data + 4, ++c->serial);
            desk_queue(d, i, &m); c->ping_pending = 1; c->ping_sent_ns = now;
        }
        if (c->ping_pending && now - c->ping_sent_ns >= 5000000000ull && !c->unresponsive) {
            c->unresponsive = 1;
            if (c->window) desk_damage_add(&d->damage, desk_outer(c), d->width, d->height);
        }
    }
    if (d->shell) desk_close_timeouts(d);
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
    desk_sound(d,c->minimized ? SOUND_RESTORE : SOUND_MINIMISE);
    if (c->minimized) { c->minimized=0; desk_focus(d,i); }
    else {
        c->minimized=1;
        if (d->drag==i) d->drag=-1;
        if (d->focus==i) {
            int next=-1;
            for (int j=0; j<DESK_CLIENTS; j++) if (d->clients[j].window && !d->clients[j].minimized &&
                (next<0 || d->clients[j].order>d->clients[next].order)) next=j;
            desk_focus(d,next);
        }
    }
    desk_damage_add(&d->damage,desk_outer(c),d->width,d->height);
    desk_damage_add(&d->damage,desk_taskbar(d),d->width,d->height);
}
void desk_request_close(struct desktop *d, int i)
{
    if (i<0 || i>=DESK_CLIENTS || !d->clients[i].window) return;
    struct desk_client *c=&d->clients[i];
    if (c->internal) { desk_drop_client(d,i); return; }
    if (c->closing) return;
    c->closing=1;
    struct ciuki_message m;
    desk_message(&m,c->version==DESK_SHELL_VERSION ? DESK_CLOSE_REQUEST : DESK_CLOSED,4);
    desk_queue(d,i,&m);
    for (int j=0; j<DESK_CLIENTS; j++) if (d->children[j].pid==c->pid && c->pid)
        d->children[j].close_ns=d->now_ns+DESK_CLOSE_NS;
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
}
void desk_child_exited(struct desktop *d, uint32_t pid, int status)
{
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
        unsigned n=0; while (n<DESK_TITLE_MAX && c->title[n]) n++;
        uint32_t flags=(c->unresponsive ? DESK_WINDOW_UNRESPONSIVE : 0) | (c->minimized ? DESK_WINDOW_MINIMIZED : 0) |
            (d->focus==i ? DESK_WINDOW_FOCUSED : 0) | (c->internal ? DESK_WINDOW_INTERNAL : 0) |
            (c->closing ? DESK_WINDOW_CLOSING : 0);
        desk_message(&m,DESK_WINDOW_INFO,32+n);
        desk_put32(m.data+4,serial); desk_put32(m.data+8,at++); desk_put32(m.data+12,count);
        desk_put32(m.data+16,c->window_id); desk_put32(m.data+20,c->internal ? d->self_pid : c->pid);
        desk_put32(m.data+24,flags); desk_put32(m.data+28,n); memcpy(m.data+32,c->title,n);
        desk_queue(d,index,&m);
    }
}
int desk_task_end(struct desktop *d, uint32_t window_id, uint32_t pid)
{
    if (d->power.state) return -EBUSY;
    if (window_id) {
        int i=window_slot(d,window_id);
        if (i<0 || (pid && d->clients[i].pid!=pid && !d->clients[i].internal)) return -ESRCH;
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
int desk_task_switch(struct desktop *d, uint32_t window_id)
{
    if (d->power.state) return -EBUSY;
    int i=window_slot(d,window_id);
    if (i<0) return -ESRCH;
    if (d->clients[i].minimized) desk_minimize(d,i); else desk_focus(d,i);
    return 0;
}
int desk_task_launch_args(struct desktop *d, const char *name, const char *const *args, unsigned count)
{
    if (d->power.state) return -EBUSY;
    if (!count || count > sizeof(d->launch_argv) / sizeof(d->launch_argv[0])) return -EINVAL;
    if (!strncmp(name,"/Programs/",sizeof("/Programs/")-1)) name+=sizeof("/Programs/")-1;
    for (unsigned k=0; k<d->program_count; k++) if (!strcmp(d->programs[k].name,name)) {
        if (!d->launch) return -ENOSYS;
        for (unsigned a=0; a<count; a++) d->launch_argv[a]=args[a];
        int pid=d->launch(d,&d->programs[k]);
        int error=errno;
        for (unsigned a=0; a<count; a++) d->launch_argv[a]=NULL;
        return pid<0 ? -(error ? error : EIO) : pid;
    }
    return -ENOENT;
}
int desk_task_launch(struct desktop *d, const char *name, const char *argument)
{
    if (d->power.state) return -EBUSY;
    if (!strncmp(name,"/Programs/",sizeof("/Programs/")-1)) name+=sizeof("/Programs/")-1;
    for (unsigned k=0; k<d->program_count; k++) if (!strcmp(d->programs[k].name,name)) {
        if (!d->launch) return -ENOSYS;
        d->launch_arg=argument && argument[0] ? argument : NULL;
        int pid=d->launch(d,&d->programs[k]);
        int error=errno;
        d->launch_arg=NULL;
        return pid<0 ? -(error ? error : EIO) : pid;
    }
    return -ENOENT;
}

int desk_ide_launch(struct desktop *d,const char *path)
{
    if (d->power.state) return -EBUSY;
    size_t n=strlen(path);
    if (n<=11 || n>DESK_ARG_MAX || strncmp(path,"/Documents/",11)) return -EPERM;
    const char *start=path+11;
    for (const char *p=start;;++p) {
        unsigned char ch=(unsigned char)*p;
        if (!ch || ch=='/') {
            size_t len=(size_t)(p-start);
            if (!len || (len==1 && *start=='.') || (len==2 && start[0]=='.' && start[1]=='.')) return -EINVAL;
            if (!ch) break;
            start=p+1;
        } else if (ch<32 || ch>126 || ch=='\\' || ch==':') return -EINVAL;
    }
    if (!d->launch) return -ENOSYS;
    /* The ordinary launcher owns channel creation, child tracking and reap.
       Kernel ELF validation still admits/rejects the actual file. A generic
       project app gets no Task Manager, mixer, Files or IDE privileges. */
    struct desk_program program={0};
    strcpy(program.name,"ciukide-app"); memcpy(program.path,path,n+1);
    int pid=d->launch(d,&program), error=errno;
    return pid<0 ? -(error ? error : EIO) : pid;
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
    if (!d->shell || d->sh.gate || d->power.state || (action!=1 && action!=2)) return;
    memset(&d->power,0,sizeof(d->power));
    d->power.state=DESK_POWER_CLOSING; d->power.action=action;
    d->power.previous_focus=d->focus;
    d->power.close_deadline=d->now_ns+DESK_POWER_CLOSE_NS;
    desk_menu_close(d);
    d->sh.volume_open=d->sh.volume_drag=d->sh.switcher=d->sh.tooltip=0;
    d->sh.want_run[0]=0; d->action=-1;
    d->drag=-1; d->resize_edges=0; d->title_down=d->title_hot=0;
    for (int i=0; i<DESK_CLIENTS; i++) {
        struct desk_client *c=&d->clients[i];
        if (!c->internal && c->channel>=0) power_target(d,c->pid,c->title,0);
    }
    for (int i=0; i<DESK_CLIENTS; i++) power_target(d,d->children[i].pid,NULL,1);
    for (int i=0; i<DESK_CLIENTS; i++) if (d->clients[i].window) desk_request_close(d,i);
    for (int i=0; i<DESK_CLIENTS; i++) if (d->children[i].pid)
        d->children[i].close_ns=d->power.close_deadline;
    strcpy(d->power.text,"Shutting down...\nClosing programs and virtual machines.");
    d->power.window=desk_internal_open(d,DESK_INTERNAL_POWER);
    desk_theme_changed(d);
}
void desk_power_cancel(struct desktop *d)
{
    if (d->sound_cancel) d->sound_cancel(d);
    if (!d->power.state || d->power.state==DESK_POWER_DONE) return;
    int window=d->power.window, focus=d->power.previous_focus;
    d->power.state=DESK_POWER_IDLE;
    if (window>=0 && d->clients[window].internal==DESK_INTERNAL_POWER) desk_drop_client(d,window);
    for (int i=0; i<DESK_CLIENTS; i++) {
        for (unsigned j=0; j<d->power.count; j++) if (d->children[i].pid==d->power.targets[j].pid) {
            d->children[i].close_ns=0; d->children[i].term_sent=0;
        }
        d->clients[i].closing=0;
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
