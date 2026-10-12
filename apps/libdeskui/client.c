/* SPDX-License-Identifier: MIT
 * Desktop protocol v2.1 client glue: handshake, surface, back buffer,
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
    m->data[2] = DESK_RESIZE_MINOR;
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

static int window_open(struct du_window *w, struct du_ui *u, int channel, int width, int height,
                       const char *title, int min_width, int min_height)
{
    memset(w, 0, sizeof(*w)); w->surface = w->old_surface = -1; w->channel = channel; w->ui = u;
    w->resizable = min_width > 0 && min_height > 0;
    size_t len = strlen(title);
    if (!len || len > DESK_TITLE_MAX || width <= 0 || height <= 0 ||
        min_width < 0 || min_height < 0 || (min_width == 0) != (min_height == 0) ||
        min_width > width || min_height > height) { errno = EINVAL; return -1; }
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
    desk_message(&m, DESK_CREATE_WINDOW, 44 + (unsigned)len);
    desk_put32(m.data + 4, (uint32_t)width); desk_put32(m.data + 8, (uint32_t)height); desk_put32(m.data + 12, (uint32_t)len);
    memcpy(m.data + 16, title, len); m.fd_count = 1; m.fds[0] = w->surface;
    if (w->resizable) {
        unsigned n = 16 + (unsigned)len;
        desk_put32(m.data + n, DESK_RESIZABLE);
        desk_put32(m.data + n + 4, (uint32_t)min_width); desk_put32(m.data + n + 8, (uint32_t)min_height);
        desk_put32(m.data + n + 12, CIUKI_SURFACE_DIMENSION_MAX); desk_put32(m.data + n + 16, CIUKI_SURFACE_DIMENSION_MAX);
        desk_put32(m.data + n + 20, 1); desk_put32(m.data + n + 24, 1);
    }
    queue(w, &m);
    return flush(w);
fail: { int error = errno ? errno : EINVAL; du_window_destroy(w); errno = error; return -1; }
}
int du_window_open(struct du_window *w, struct du_ui *u, int ch, int width, int height, const char *title)
{ return window_open(w,u,ch,width,height,title,0,0); }
int du_window_open_resizable(struct du_window *w, struct du_ui *u, int ch, int width, int height,
                             const char *title, int min_width, int min_height)
{ return window_open(w,u,ch,width,height,title,min_width,min_height); }

static void release_old(struct du_window *w)
{
    if (w->old_pixels) munmap(w->old_pixels, w->old_info.allocation_bytes);
    if (w->old_surface >= 0) close(w->old_surface);
    free(w->old_back);
    w->old_pixels = w->old_back = NULL; w->old_surface = -1;
    w->replacement = w->swap_sent = 0;
}
static void discard_replacement(struct du_window *w)
{
    munmap(w->pixels,w->info.allocation_bytes); close(w->surface); free(w->back);
    w->surface = w->old_surface; w->info = w->old_info; w->pixels = w->old_pixels; w->back = w->old_back;
    w->old_surface = -1; w->old_pixels = w->old_back = NULL; w->replacement = w->swap_sent = 0;
    du_resize(w->ui,w->back,(int)w->info.width,(int)w->info.height);
}
static int replacement(struct du_window *w)
{
    if (!w->request_serial || w->swap_sent) return 0;
    if (w->replacement) discard_replacement(w);
    /* Even a same-size proposal must acknowledge its serial with a surface. */
    int fd = ciuki_surface_create(w->request_width,w->request_height,w->info.format);
    if (fd < 0) return -1;
    struct ciuki_surface_info info; uint32_t *pixels = NULL, *back = NULL;
    if (ciuki_surface_info(fd,&info) || !desk_surface_valid(&info)) goto fail;
    pixels = ciuki_surface_map(fd,PROT_READ|PROT_WRITE);
    if (pixels == MAP_FAILED) { pixels = NULL; goto fail; }
    back = malloc((size_t)info.width*info.height*4);
    if (!back) { errno = ENOMEM; goto fail; }
    w->old_surface = w->surface; w->old_info = w->info; w->old_pixels = w->pixels; w->old_back = w->back;
    w->surface = fd; w->info = info; w->pixels = pixels; w->back = back;
    w->replacement = 1; w->configure_serial = w->request_serial; w->request_serial = 0; w->damaged = 0;
    du_resize(w->ui,back,(int)info.width,(int)info.height);
    return 0;
fail: {
    int error = errno ? errno : EINVAL;
    if (pixels) munmap(pixels,info.allocation_bytes);
    free(back); close(fd); errno = error; return -1;
} }

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
    if (w->closed || w->swap_sent) return;
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
    if (w->replacement) { w->damage = (struct du_rect){0,0,width,height}; w->damaged = 1; }
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

int du_window_set_title(struct du_window *w, const char *title)
{
    size_t n = title ? strlen(title) : 0;
    if (!n || n > DESK_TITLE_MAX) { errno = EINVAL; return -1; }
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)title[i] < 32 || (unsigned char)title[i] > 126) { errno = EINVAL; return -1; }
    struct ciuki_message msg;
    desk_message(&msg, DESK_SET_TITLE, 8 + (unsigned)n);
    desk_put32(msg.data + 4, (uint32_t)n); memcpy(msg.data + 8, title, n);
    return queue(w, &msg);
}

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
            m.data[0] != DESK_SHELL_VERSION || m.data[2] != DESK_RESIZE_MINOR || !desk_parse(&m, 0, NULL, &p)) {
            w->error = EIO; w->closed = 1; return -1;
        }
        w->server_pid = m.sender_pid;
        if (now > w->ui->now) w->ui->now = now;
        switch (p.opcode) {
        case DESK_CONFIGURE:
            if (p.flags & DESK_CONFIGURE_COMMITTED) {
                if ((!w->configured ? p.serial != 0 : !w->swap_sent || p.serial != w->configure_serial) ||
                    p.width != w->info.width || p.height != w->info.height) goto protocol_error;
                if (w->configured) release_old(w);
                w->configured = 1; w->ui->dirty = 1;
            } else {
                if (!w->configured || !w->resizable || p.serial <= w->last_serial) goto protocol_error;
                w->last_serial = w->request_serial = p.serial;
                w->request_width = p.width; w->request_height = p.height;
            }
            break;
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
    if (!w->closing && replacement(w)) { w->error = errno; w->closed = 1; return -1; }
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
    if (w->configured && !w->swap_sent && w->ui->dirty) { du_render(w->ui); du_window_present(w); }
    if (w->configured && w->damaged && w->count < 16) {
        struct ciuki_message m; desk_message(&m, DESK_DAMAGE, 24);
        desk_put32(m.data + 4, (uint32_t)w->damage.x); desk_put32(m.data + 8, (uint32_t)w->damage.y);
        desk_put32(m.data + 12, (uint32_t)w->damage.w); desk_put32(m.data + 16, (uint32_t)w->damage.h);
        if (w->replacement) {
            desk_put32(m.data + 20,w->configure_serial); m.fd_count = 1; m.fds[0] = w->surface;
            /* Send swaps directly after the FIFO drains: an EAGAIN surface
             * remains unsent and can be superseded by the next CONFIGURE. */
            if (flush(w)) return -1;
            if (!w->count) {
                m.data[0] = DESK_SHELL_VERSION; m.data[2] = DESK_RESIZE_MINOR;
                if (!ciuki_channel_send(w->channel,&m,DONTWAIT)) { w->swap_sent = 1; w->damaged = 0; }
                else if (errno != EAGAIN && errno != EINTR) { w->error = errno; w->closed = 1; return -1; }
            }
        } else if (!queue(w, &m)) w->damaged = 0;
    }
    return flush(w) ? -1 : 1;
protocol_error:
    w->error = EIO; w->closed = 1; return -1;
}

int du_run(struct du_window *w, void (*idle)(void *), void *arg)
{
    int r;
    uint64_t started = du_now();
    while ((r = du_window_turn(w, du_now())) > 0) {
        if (idle) idle(arg);
        /* No poll/select in the F2 ABI: nonblocking turns plus a short sleep. */
        uint64_t now = du_now(), next = du_next_deadline(w->ui), wait = 10000000u;
        if (!w->configured && now >= started && now - started < 5000000000ull) wait = 1000000u;
        if (next > now && next - now < wait) wait = next - now;
        if (next <= now || w->ui->dirty) wait = 1000000u;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = (long)wait};
        nanosleep(&pause, NULL);
    }
    return r;
}

void du_window_destroy(struct du_window *w)
{
    release_old(w);
    if (w->pixels) { munmap(w->pixels, w->info.allocation_bytes); w->pixels = NULL; }
    free(w->back); w->back = NULL;
    if (w->surface >= 0) { close(w->surface); w->surface = -1; }
    if (w->channel >= 0) { close(w->channel); w->channel = -1; }
}
