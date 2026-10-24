/* SPDX-License-Identifier: MIT
 * /Programs/mixer: CiukiOS Volume Control (desktop client, protocol v2).
 * Usage: mixer [--channel-fd=N] [--test=mixer] */
#include "mixer.h"
#include <ciuki/audio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct mixer_main { struct du_window window; struct du_ui ui; struct mixer_app app; int fd; uint32_t serial, sent; int line_request; };
static int get(void *ctx, struct ciuki_mixer_master *m)
{
    struct mixer_main *x = ctx;
    if (x->fd < 0) x->fd = open("/dev/mixer", O_RDONLY | O_CLOEXEC);
    if (x->fd < 0) return -errno;
    return ciuki_devctl(x->fd, MIXER_GET_MASTER, m, sizeof(*m)) ? -errno : 0;
}
static int set(void *ctx, uint32_t left, uint32_t right, uint32_t mute)
{
    struct mixer_main *x = ctx;
    struct ciuki_message m;
    desk_message(&m, DESK_MIXER_SET, 20);
    if (!++x->serial) ++x->serial;
    desk_put32(m.data + 4, x->serial); desk_put32(m.data + 8, left); desk_put32(m.data + 12, right); desk_put32(m.data + 16, mute);
    if (du_window_queue(&x->window, &m)) return -errno;
    x->sent = x->serial; x->line_request=0;
    return 0;
}
static int line_get(void *ctx,struct ciuki_mixer_line *line)
{
    struct mixer_main *x=ctx;
    if (x->fd<0) x->fd=open("/dev/mixer",O_RDONLY|O_CLOEXEC);
    return x->fd<0 || ciuki_devctl(x->fd,MIXER_GET_LINE,line,sizeof(*line)) ? -errno : 0;
}
static int line_set(void *ctx,const struct ciuki_mixer_line *line)
{
    struct mixer_main *x=ctx; struct ciuki_message m;
    desk_message(&m,DESK_MIXER_SET,28);
    if (!++x->serial) ++x->serial;
    desk_put32(m.data+4,x->serial); desk_put32(m.data+8,line->left);
    desk_put32(m.data+12,line->right); desk_put32(m.data+16,line->recording ? line->selected : line->mute);
    desk_put32(m.data+20,line->line); desk_put32(m.data+24,line->recording);
    if (du_window_queue(&x->window,&m)) return -errno;
    x->sent=x->serial; x->line_request=1; return 0;
}
static void on_message(struct du_window *w, const struct desk_packet *p, void *user)
{
    (void)w; struct mixer_main *x = user;
    if (p->opcode == DESK_TASK_RESULT && p->request == DESK_MIXER_SET && p->serial == x->sent && (!x->line_request || p->result)) mixer_result(&x->app, p->result);
}
int main(int argc, char **argv)
{
    int channel, test; const char *path;
    if (acc_args(argc, argv, "mixer", &channel, &test, &path) || path) return 2;
    struct mixer_main *x = calloc(1, sizeof(*x));
    if (!x) return 1;
    x->fd = -1;
    static struct mixer_backend backend;
    backend = (struct mixer_backend){ get, set, x, line_get, line_set };
    if (du_window_open(&x->window, &x->ui, channel, MIXER_W, MIXER_H, "Volume Control")) {
        fprintf(stderr, "mixer: desktop connection failed: %s\n", strerror(errno)); free(x); return 1;
    }
    x->window.message = on_message; x->window.user = x;
    mixer_attach(&x->app, &x->ui, &backend);
    du_timer_start(&x->ui, MX_T_POLL, 1000000000ull, 1);   /* follow changes made elsewhere (tray, reboot) */
    if (test) du_window_move(&x->window, 24, 48);
    int r = du_run(&x->window, NULL, NULL);
    if (x->fd >= 0) close(x->fd);
    du_window_destroy(&x->window); free(x);
    return r < 0 ? 1 : 0;
}
