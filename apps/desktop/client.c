/* SPDX-License-Identifier: MIT */
#include "desktop.h"
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <signal.h>
int desk_ignore_sigpipe(void)
{ return signal(SIGPIPE,SIG_IGN)==SIG_ERR ? -1 : 0; }
void desk_init(struct desktop *d, const struct desk_ops *ops, uint32_t w, uint32_t h)
{
    memset(d, 0, sizeof(*d));
    d->ops = *ops; d->width = w; d->height = h;
    d->display = d->input = d->output = d->focus = d->drag = -1;
    d->mouse_x = (int32_t)w / 2; d->mouse_y = (int32_t)h / 2;
    d->clock_second = UINT64_MAX;
    for (unsigned i = 0; i < DESK_CLIENTS; i++)
        d->clients[i].channel = d->clients[i].surface = -1;
    desk_damage_add(&d->damage, (struct desk_box){0, 0, (int32_t)w, (int32_t)h}, w, h);
}
int desk_add_client(struct desktop *d, int fd)
{
    for (unsigned i = 0; i < DESK_CLIENTS; i++) if (d->clients[i].channel == fd) return -1;
    for (unsigned i = 0; i < DESK_CLIENTS; i++) if (d->clients[i].channel < 0) {
        d->clients[i].channel = fd;
        return (int)i;
    }
    return -1;
}
void desk_drop_client(struct desktop *d, int i)
{
    struct desk_client *c = &d->clients[i];
    if (c->channel < 0) return;
    if (d->focus == i) desk_focus(d, -1);
    if (d->drag == i) d->drag = -1;
    if (c->window) desk_damage_add(&d->damage, c->frame, d->width, d->height);
    if (c->pixels) d->ops.unmap((void *)c->pixels, c->info.allocation_bytes);
    if (c->surface >= 0) d->ops.close(c->surface);
    d->ops.close(c->channel);
    memset(c, 0, sizeof(*c));
    c->channel = c->surface = -1;
    if (d->focus < 0) {
        int next = -1;
        for (int j = 0; j < DESK_CLIENTS; j++) if (d->clients[j].window &&
            (next < 0 || d->clients[j].order > d->clients[next].order)) next = j;
        desk_focus(d, next);
    }
}
void desk_queue(struct desktop *d, int i, const struct ciuki_message *m)
{
    if (i < 0 || i >= DESK_CLIENTS) return;
    struct desk_client *c = &d->clients[i];
    if (c->channel < 0) return;
    if (c->tx_count == DESK_TX_MAX) {
        /* A stalled peer cannot block the compositor. Repair its input state
         * once its bounded queue drains. Keep the window and backing alive. */
        c->resync_pending = 1;
        if (m->data[1]==DESK_CONFIGURE) c->configure_pending=1;
        if (m->data[1]==DESK_FOCUS) c->focus_pending=1;
        return;
    }
    c->tx[(c->tx_head + c->tx_count++) % DESK_TX_MAX] = *m;
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
        if (c->configure_pending) {
            desk_message(&m,DESK_CONFIGURE,12);
            desk_put32(m.data+4,c->info.width); desk_put32(m.data+8,c->info.height);
            c->configure_pending=0; desk_queue(d,i,&m);
        }
        if (c->focus_pending) {
            desk_message(&m,DESK_FOCUS,8); desk_put32(m.data+4,(uint32_t)(d->focus==i));
            c->focus_pending=0; desk_queue(d,i,&m);
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
void desk_move(struct desktop *d, int i, int32_t x, int32_t y)
{
    struct desk_client *c = &d->clients[i];
    int32_t max_x = (int32_t)d->width - c->frame.w;
    int32_t max_y = (int32_t)d->height - c->frame.h;
    if (max_x < 0) max_x = 0;
    if (max_y < DESK_BAR) max_y = DESK_BAR;
    if (x < 0) x = 0;
    if (x > max_x) x = max_x;
    if (y < DESK_BAR) y = DESK_BAR;
    if (y > max_y) y = max_y;
    if (c->frame.x == x && c->frame.y == y) return;
    desk_damage_add(&d->damage, c->frame, d->width, d->height);
    c->frame.x = x; c->frame.y = y;
    desk_damage_add(&d->damage, c->frame, d->width, d->height);
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
    int create = m->length >= 4 && m->data[1] == DESK_CREATE_WINDOW && m->fd_count == 1;
    int info_ok = !create || !d->ops.info(m->fds[0], &s);
    if (c->channel < 0 || !info_ok || !desk_parse(m, 1, &s, &p) ||
        !m->sender_pid || (c->pid && c->pid != m->sender_pid) ||
        (!c->hello && p.opcode != DESK_HELLO) ||
        (c->hello && p.opcode == DESK_HELLO) ||
        (p.opcode == DESK_CREATE_WINDOW && c->window) ||
        ((p.opcode == DESK_DAMAGE || p.opcode == DESK_MOVE || p.opcode == DESK_CLOSE) && !c->window)) {
        close_attachments(d, m, -1);
        struct ciuki_message closed;
        desk_message(&closed, DESK_CLOSED, 4);
        if (c->channel >= 0) d->ops.send(c->channel, &closed, DONTWAIT);
        desk_drop_client(d, i);
        return;
    }
    c->pid = m->sender_pid;
    d->received++;
    struct ciuki_message reply;
    switch (p.opcode) {
    case DESK_HELLO:
        c->hello = 1;
        c->next_ping_ns = d->now_ns + 1000000000ull;
        break;
    case DESK_CREATE_WINDOW: {
        void *pixels = d->ops.map(m->fds[0], PROT_READ);
        if (pixels == MAP_FAILED || !pixels) {
            close_attachments(d, m, -1); desk_drop_client(d, i); return;
        }
        c->surface = m->fds[0]; c->info = s; c->pixels = pixels; c->window = 1;
        memcpy(c->title, p.title, sizeof(c->title));
        c->frame = desk_frame(s.width, s.height, 0, DESK_BAR);
        desk_move(d, i, 20 + i * 18, DESK_BAR + 20 + i * 18);
        desk_damage_add(&d->damage, c->frame, d->width, d->height);
        desk_message(&reply, DESK_CONFIGURE, 12);
        desk_put32(reply.data + 4, s.width); desk_put32(reply.data + 8, s.height);
        desk_queue(d, i, &reply);
        desk_focus(d, i);
        break;
    }
    case DESK_DAMAGE: {
        struct desk_box b = desk_content(c);
        b.x += p.x; b.y += p.y; b.w = (int32_t)p.width; b.h = (int32_t)p.height;
        desk_damage_add(&d->damage, b, d->width, d->height);
        break;
    }
    case DESK_MOVE: desk_move(d, i, p.x, p.y); break;
    case DESK_CLOSE:
        desk_message(&reply, DESK_CLOSED, 4);
        d->ops.send(c->channel, &reply, DONTWAIT);
        desk_drop_client(d, i);
        break;
    case DESK_PING:
        desk_message(&reply, DESK_PONG, 8); desk_put32(reply.data + 4, p.serial);
        desk_queue(d, i, &reply);
        break;
    case DESK_PONG:
        if (c->ping_pending && p.serial == c->serial) {
            c->ping_pending = 0; c->next_ping_ns = d->now_ns + 1000000000ull;
            if (c->unresponsive) {
                c->unresponsive = 0;
                desk_damage_add(&d->damage, c->frame, d->width, d->height);
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
        desk_damage_add(&d->damage, (struct desk_box){(int32_t)d->width - 120, 0, 120, DESK_BAR}, d->width, d->height);
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
            if (c->window) desk_damage_add(&d->damage, c->frame, d->width, d->height);
        }
    }
}
