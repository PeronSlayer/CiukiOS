/* SPDX-License-Identifier: MIT */
#include "desktop.h"
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <signal.h>
#include <stdio.h>
int desk_ignore_sigpipe(void)
{ return signal(SIGPIPE,SIG_IGN)==SIG_ERR ? -1 : 0; }
void desk_init(struct desktop *d, const struct desk_ops *ops, uint32_t w, uint32_t h)
{
    memset(d, 0, sizeof(*d));
    d->ops = *ops; d->width = w; d->height = h;
    d->display = d->input = d->output = d->focus = d->drag = d->about = -1;
    d->action = -1;
    d->mouse_x = (int32_t)w / 2; d->mouse_y = (int32_t)h / 2;
    d->clock_second = UINT64_MAX;
    for (unsigned i = 0; i < DESK_CLIENTS; i++)
        d->clients[i].channel = d->clients[i].surface = -1;
    desk_damage_add(&d->damage, (struct desk_box){0, 0, (int32_t)w, (int32_t)h}, w, h);
}
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
    if (c->window) desk_damage_add(&d->damage, c->frame, d->width, d->height);
    if (c->pixels && !c->internal) d->ops.unmap((void *)c->pixels, c->info.allocation_bytes);
    if (c->surface >= 0) d->ops.close(c->surface);
    if (c->channel >= 0) d->ops.close(c->channel);
    if (d->about == i) d->about = -1;
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
    if (c->tx_count == DESK_TX_MAX) {
        /* A stalled peer cannot block the compositor. Repair its input state
         * once its bounded queue drains. Keep the window and backing alive. */
        c->resync_pending = 1;
        if (m->data[1]==DESK_CONFIGURE) c->configure_pending=1;
        if (m->data[1]==DESK_FOCUS) c->focus_pending=1;
        if (m->data[1]==DESK_CLOSE_REQUEST || m->data[1]==DESK_CLOSED) c->close_pending=1;
        return;
    }
    unsigned slot = (c->tx_head + c->tx_count++) % DESK_TX_MAX;
    c->tx[slot] = *m;
    if (c->version == DESK_SHELL_VERSION) c->tx[slot].data[0] = DESK_SHELL_VERSION;
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
        (c->hello && c->version != m->data[0]) ||
        (!c->hello && p.opcode != DESK_HELLO) ||
        (c->hello && p.opcode == DESK_HELLO) ||
        (p.opcode == DESK_CREATE_WINDOW && c->window) ||
        ((p.opcode == DESK_DAMAGE || p.opcode == DESK_MOVE || p.opcode == DESK_CLOSE) && !c->window)) {
        close_attachments(d, m, -1);
        struct ciuki_message closed;
        desk_message(&closed, DESK_CLOSED, 4);
        if (c->version==DESK_SHELL_VERSION) closed.data[0]=DESK_SHELL_VERSION;
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
        c->version = m->data[0];
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
        if (d->shell && d->menu_open) {
            c->order=++d->order; d->menu_focus=i;
            desk_message(&reply,DESK_FOCUS,8); desk_queue(d,i,&reply);
        } else desk_focus(d, i);
        if (d->shell) desk_damage_add(&d->damage,desk_taskbar(d),d->width,d->height);
        if (d->shell_test && c->pid == d->shell_pid && d->shell_stage == 2) d->shell_stage = 3;
        break;
    }
    case DESK_DAMAGE: {
        struct desk_box b = desk_content(c);
        b.x += p.x; b.y += p.y; b.w = (int32_t)p.width; b.h = (int32_t)p.height;
        if (!c->minimized) desk_damage_add(&d->damage, b, d->width, d->height);
        break;
    }
    case DESK_MOVE: desk_move(d, i, p.x, p.y); break;
    case DESK_CLOSE:
        desk_message(&reply, DESK_CLOSED, 4);
        if (c->version==DESK_SHELL_VERSION) reply.data[0]=DESK_SHELL_VERSION;
        d->ops.send(c->channel, &reply, DONTWAIT);
        desk_drop_client(d, i);
        break;
    case DESK_PING:
        desk_message(&reply, DESK_PONG, 8); desk_put32(reply.data + 4, p.serial);
        desk_queue(d, i, &reply);
        /* Files test milestones arrive in order after their frame's DAMAGE. */
        if (d->files_test && d->files_pid && c->pid == d->files_pid && d->files_checkpoint < DESK_FILES_CHECKPOINTS &&
            p.serial == DESK_FILES_CHECKPOINT + (uint32_t)d->files_checkpoint + 1) d->files_checkpoint++;
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
        desk_damage_add(&d->damage, (struct desk_box){(int32_t)d->width - 120,
            d->shell ? (int32_t)d->height-DESK_BAR : 0, 120, DESK_BAR}, d->width, d->height);
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
    if (d->shell) desk_close_timeouts(d);
}

/* A bounded /bin entry parser, independent of directory enumeration order.
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
    while (at < d->program_count && strcmp(d->programs[at].path+5,name) < 0) at++;
    if (at < d->program_count && !strcmp(d->programs[at].path+5,name)) return 0;
    if (at >= DESK_MENU_MAX) return 0;
    unsigned count = d->program_count < DESK_MENU_MAX ? d->program_count+1 : DESK_MENU_MAX;
    memmove(d->programs+at+1,d->programs+at,(count-at-1)*sizeof(d->programs[0]));
    struct desk_program *p = &d->programs[at];
    memcpy(p->name,name,len+1);
    snprintf(p->path,sizeof(p->path),"/bin/%s",name);
    d->program_count = count;
    return 1;
}
void desk_shell_enable(struct desktop *d)
{
    if (d->shell) return;
    d->shell = 1;
    for (int i=0; i<DESK_CLIENTS; i++) if (d->clients[i].window)
        desk_move(d,i,d->clients[i].frame.x,d->clients[i].frame.y);
    desk_damage_add(&d->damage,(struct desk_box){0,0,(int32_t)d->width,(int32_t)d->height},d->width,d->height);
}
void desk_minimize(struct desktop *d, int i)
{
    if (!d->shell || i<0 || i>=DESK_CLIENTS || !d->clients[i].window) return;
    struct desk_client *c = &d->clients[i];
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
    desk_damage_add(&d->damage,c->frame,d->width,d->height);
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
    desk_damage_add(&d->damage,c->frame,d->width,d->height);
}
void desk_close_timeouts(struct desktop *d)
{
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
    int owned=0;
    for (int i=0; i<DESK_CLIENTS; i++) {
        if (d->clients[i].pid==pid && pid) desk_drop_client(d,i);
        if (d->children[i].pid==pid && pid) { owned=1; memset(&d->children[i],0,sizeof(d->children[i])); }
    }
    if (owned && d->shell_test && d->shell_stage==5 && pid==d->shell_pid && !status) d->shell_stage=6;
    if (owned && d->files_test && pid==d->files_pid && !status && d->files_checkpoint==DESK_FILES_CHECKPOINTS) d->files_done=1;
}

void desk_about(struct desktop *d)
{
    if (d->about>=0) {
        d->clients[d->about].minimized=0; desk_focus(d,d->about); return;
    }
    int i;
    for (i=0; i<DESK_CLIENTS; i++) if (d->clients[i].channel<0 && !d->clients[i].internal) break;
    if (i==DESK_CLIENTS) return;
    struct desk_client *c=&d->clients[i];
    uint32_t w=d->width>420 ? 400 : d->width>20 ? d->width-20 : 1;
    uint32_t h=d->height>316 ? 220 : d->height>96 ? d->height-96 : 1;
    c->internal=c->window=1;
    c->info.width=w; c->info.height=h;
    strcpy(c->title,"About CiukiOS");
    c->frame=desk_frame(w,h,0,DESK_BAR);
    d->about=i;
    desk_move(d,i,((int32_t)d->width-c->frame.w)/2,DESK_BAR+20);
    desk_focus(d,i);
    desk_damage_add(&d->damage,c->frame,d->width,d->height);
    desk_damage_add(&d->damage,desk_taskbar(d),d->width,d->height);
}
int desk_shell_click(struct desktop *d)
{
    if (!d->shell) return 0;
    struct desk_box bar=desk_taskbar(d);
    if (desk_contains((struct desk_box){4,bar.y+3,76,26},d->mouse_x,d->mouse_y)) {
        if (!d->menu_open) { d->menu_focus=d->focus; desk_focus(d,-1); }
        d->menu_open=!d->menu_open;
        if (!d->menu_open) desk_focus(d,d->menu_focus);
        if (d->menu_open && d->shell_test && d->shell_stage==0) d->shell_stage=1;
        desk_damage_add(&d->damage,desk_menu_box(d),d->width,d->height);
        desk_damage_add(&d->damage,bar,d->width,d->height);
        return 1;
    }
    if (d->menu_open) {
        struct desk_box box=desk_menu_box(d);
        int row=(d->mouse_y-box.y)/desk_menu_row(d);
        int inside=desk_contains(box,d->mouse_x,d->mouse_y);
        d->menu_open=0;
        desk_damage_add(&d->damage,box,d->width,d->height);
        desk_damage_add(&d->damage,bar,d->width,d->height);
        if (inside) {
            if (row>=0 && (unsigned)row<d->program_count) d->action=row;
            else if ((unsigned)row==d->program_count) desk_about(d);
            else if ((unsigned)row==d->program_count+1) strcpy(d->notice,"Restart is unavailable");
            return 1;
        }
    }
    if (desk_contains(bar,d->mouse_x,d->mouse_y)) {
        for (int i=0; i<DESK_CLIENTS; i++) if (desk_contains(desk_task_button(d,i),d->mouse_x,d->mouse_y)) {
            if (d->clients[i].minimized || d->focus==i) desk_minimize(d,i);
            else desk_focus(d,i);
            break;
        }
        return 1;
    }
    return 0;
}
