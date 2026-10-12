/* SPDX-License-Identifier: MIT
 * Desktop protocol v2 client glue: handshake, surface, back buffer,
 * coalesced damage, close requests, focus releases and the event loop. */
#include "deskui.h"
#include <ciuki/channel.h>
#include <ciuki/surface.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int queue(struct du_window *w, struct ciuki_message *m)
{
    if (w->count == 16) { errno = EAGAIN; return -1; }
    m->data[0] = DESK_SHELL_VERSION;
    w->tx[(w->head + w->count) % 16] = *m; w->count++;
    return 0;
}

static int flush(struct du_window *w)
{
    while (w->count) {
        if (ciuki_channel_send(w->channel, &w->tx[w->head], DONTWAIT)) {
            if (errno == EAGAIN || errno == EINTR) return 0;
            w->error = errno; w->closed = 1; return -1;
        }
        w->head = (w->head + 1) % 16; w->count--;
    }
    return 0;
}

uint64_t du_now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000000u + (unsigned)t.tv_nsec;
}

int du_window_open(struct du_window *w, struct du_ui *u, int channel, int width, int height, const char *title)
{
    memset(w, 0, sizeof(*w)); w->surface = -1; w->channel = channel; w->ui = u;
    size_t len = strlen(title);
    if (!len || len > DESK_TITLE_MAX || width <= 0 || height <= 0) { errno = EINVAL; return -1; }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) return -1;
    w->surface = ciuki_surface_create((uint32_t)width, (uint32_t)height, CIUKI_SURFACE_XRGB8888);
    if (w->surface < 0) return -1;
    if (ciuki_surface_info(w->surface, &w->info) || !desk_surface_valid(&w->info)) goto fail;
    w->pixels = ciuki_surface_map(w->surface, PROT_READ | PROT_WRITE);
    if (w->pixels == MAP_FAILED) { w->pixels = NULL; goto fail; }
    w->back = malloc((size_t)width * (size_t)height * 4);
    if (!w->back) { errno = ENOMEM; goto fail; }
    du_init(u, w->back, width, height);
    u->now = du_now();
    struct ciuki_message m;
    desk_message(&m, DESK_HELLO, 4); queue(w, &m);
    desk_message(&m, DESK_CREATE_WINDOW, 16 + (unsigned)len);
    desk_put32(m.data + 4, (uint32_t)width); desk_put32(m.data + 8, (uint32_t)height); desk_put32(m.data + 12, (uint32_t)len);
    memcpy(m.data + 16, title, len); m.fd_count = 1; m.fds[0] = w->surface; queue(w, &m);
    return flush(w);
fail: { int error = errno ? errno : EINVAL; du_window_destroy(w); errno = error; return -1; }
}

static void add_damage(struct du_window *w, struct du_rect r)
{
    if (!w->damaged) { w->damage = r; w->damaged = 1; return; }
    int x = w->damage.x < r.x ? w->damage.x : r.x, y = w->damage.y < r.y ? w->damage.y : r.y;
    int right = w->damage.x + w->damage.w > r.x + r.w ? w->damage.x + w->damage.w : r.x + r.w;
    int bottom = w->damage.y + w->damage.h > r.y + r.h ? w->damage.y + w->damage.h : r.y + r.h;
    w->damage = (struct du_rect){x, y, right - x, bottom - y};
}

/* Copy only changed pixels from the back buffer and damage their bounds:
 * the compositor never sees a half-rendered frame and repaints little. */
void du_window_present(struct du_window *w)
{
    int width = (int)w->info.width, height = (int)w->info.height;
    int x0 = width, x1 = -1, y0 = height, y1 = -1;
    for (int y = 0; y < height; y++) {
        uint32_t *src = w->back + (size_t)y * (size_t)width, *dst = w->pixels + (size_t)y * (size_t)width;
        if (!memcmp(src, dst, (size_t)width * 4)) continue;
        int a = 0, b = width - 1;
        while (src[a] == dst[a]) a++;
        while (src[b] == dst[b]) b--;
        memcpy(dst + a, src + a, (size_t)(b - a + 1) * 4);
        if (a < x0) x0 = a;
        if (b > x1) x1 = b;
        if (y < y0) y0 = y;
        y1 = y;
    }
    if (x1 >= 0) add_damage(w, (struct du_rect){x0, y0, x1 - x0 + 1, y1 - y0 + 1});
}

int du_window_queue(struct du_window *w, struct ciuki_message *m) { return queue(w, m); }

int du_window_ping(struct du_window *w, uint32_t serial)
{
    struct ciuki_message m;
    desk_message(&m, DESK_PING, 8); desk_put32(m.data + 4, serial);
    return queue(w, &m);
}

int du_window_move(struct du_window *w, int x, int y)
{
    struct ciuki_message m;
    desk_message(&m, DESK_MOVE, 12); desk_put32(m.data + 4, (uint32_t)x); desk_put32(m.data + 8, (uint32_t)y);
    return queue(w, &m);
}

void du_window_close(struct du_window *w) { if (!w->closing) w->closing = 1; du_release(w->ui); }

int du_window_turn(struct du_window *w, uint64_t now)
{
    if (w->closed) return 0;
    if (flush(w)) return -1;
    for (unsigned n = 0; n < 32 && w->count < 14; n++) {
        struct ciuki_message m;
        int r = ciuki_channel_recv(w->channel, &m, DONTWAIT);
        if (r < 0) { if (errno == EAGAIN || errno == EINTR) break; w->error = errno; w->closed = 1; return -1; }
        if (!r) { w->closed = 1; du_release(w->ui); return 0; }
        for (unsigned i = 0; i < m.fd_count && i < 4; i++) close(m.fds[i]);
        struct desk_packet p;
        if (!m.sender_pid || (w->server_pid && m.sender_pid != w->server_pid) ||
            m.data[0] != DESK_SHELL_VERSION || !desk_parse(&m, 0, NULL, &p)) {
            w->error = EIO; w->closed = 1; return -1;
        }
        w->server_pid = m.sender_pid;
        if (now > w->ui->now) w->ui->now = now;
        switch (p.opcode) {
        case DESK_CONFIGURE:
            if (w->configured || p.width != w->info.width || p.height != w->info.height) { w->error = EIO; w->closed = 1; return -1; }
            w->configured = 1; w->ui->dirty = 1; break;
        case DESK_PING:
            desk_message(&m, DESK_PONG, 8); desk_put32(m.data + 4, p.serial); queue(w, &m); break;
        case DESK_PONG: break;
        case DESK_CLOSED: w->closed = 1; du_release(w->ui); return 0;
        case DESK_CLOSE_REQUEST:
            if (!w->close_request || w->close_request(w, w->user)) du_window_close(w);
            break;
        case DESK_FOCUS: du_window_focus(w->ui, p.focused != 0); break;
        case DESK_KEY: case DESK_MOTION: case DESK_BUTTON:
            /* Repair remembered input on the desktop's backlog RESYNC too. */
            du_input(w->ui, &p.event); break;
        default: if (w->message) w->message(w, &p, w->user); break;
        }
    }
    if (w->closing) {
        if (w->closing == 1 && w->count < 16) {
            struct ciuki_message m; desk_message(&m, DESK_CLOSE, 4);
            if (!queue(w, &m)) w->closing = 2;
        }
        if (flush(w)) return -1;
        if (w->closing == 2 && !w->count) w->closed = 1;
        return w->closed ? 0 : 1;
    }
    du_tick(w->ui, now);
    /* ui-look: follow the shell's theme setting (read at once, then a stat every 2 s). */
    if (desk_theme_poll(DESK_SETTINGS_PATH, now)) w->ui->dirty = 1;
    if (w->configured && w->ui->dirty) { du_render(w->ui); du_window_present(w); }
    if (w->configured && w->damaged && w->count < 16) {
        struct ciuki_message m; desk_message(&m, DESK_DAMAGE, 20);
        desk_put32(m.data + 4, (uint32_t)w->damage.x); desk_put32(m.data + 8, (uint32_t)w->damage.y);
        desk_put32(m.data + 12, (uint32_t)w->damage.w); desk_put32(m.data + 16, (uint32_t)w->damage.h);
        if (!queue(w, &m)) w->damaged = 0;
    }
    return flush(w) ? -1 : 1;
}

int du_run(struct du_window *w, void (*idle)(void *), void *arg)
{
    int r;
    while ((r = du_window_turn(w, du_now())) > 0) {
        if (idle) idle(arg);
        /* No poll/select in the F2 ABI: nonblocking turns plus a short sleep. */
        uint64_t now = du_now(), next = du_next_deadline(w->ui), wait = 10000000u;
        if (next > now && next - now < wait) wait = next - now;
        if (next <= now || w->ui->dirty) wait = 1000000u;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = (long)wait};
        nanosleep(&pause, NULL);
    }
    return r;
}

void du_window_destroy(struct du_window *w)
{
    if (w->pixels) { munmap(w->pixels, w->info.allocation_bytes); w->pixels = NULL; }
    free(w->back); w->back = NULL;
    if (w->surface >= 0) { close(w->surface); w->surface = -1; }
    if (w->channel >= 0) { close(w->channel); w->channel = -1; }
}
