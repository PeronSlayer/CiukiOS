/* SPDX-License-Identifier: MIT
 * /Programs/sysmon [--channel-fd=N]: System Monitor (Windows 98 model). */
#include "sysmon_view.h"
#include "../tasks/model.h"
#include "../accessories/acc.h"
#include <ciuki/net.h>
#include <ciuki/proc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct app {
    struct du_window window; struct du_ui ui; struct sysmon_app view;
    struct sm_source source;
    int log_fd;
    uint8_t list[TM_LIST_BYTES];
};
/* Call 70 (system record, every process record for the thread total) and the network frame counters. */
static int sample(void *ctx, struct sm_raw *raw)
{
    struct app *a = ctx;
    int n = ciuki_proc_list(a->list, sizeof(a->list));
    if (n < 0) return -errno;
    struct ciuki_proc_system sys; memcpy(&sys, a->list, sizeof(sys));
    if (sys.version != CIUKI_PROC_VERSION) return -EIO;
    unsigned threads = 0;
    for (int i = 0; i < n && sizeof(sys) + (size_t)(i + 1) * sizeof(struct ciuki_proc_record) <= sizeof(a->list); i++) {
        struct ciuki_proc_record r;
        memcpy(&r, a->list + sizeof(sys) + (size_t)i * sizeof(r), sizeof(r));
        threads += r.threads;
    }
    struct ciuki_net_state net; memset(&net, 0, sizeof(net));
    int have_net = ciuki_net_status(&net) == 0 && net.present;
    sm_raw_from_proc(raw, &a->source, &sys, threads, have_net, net.rx_frames, net.tx_frames);
    return 0;
}
static int log_open(void *ctx, const char *path)
{
    struct app *a = ctx;
    if (a->log_fd >= 0) close(a->log_fd);
    a->log_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    return a->log_fd < 0 ? -errno : 0;
}
static int log_write(void *ctx, const char *text, size_t length)
{
    struct app *a = ctx;
    if (a->log_fd < 0) return -EBADF;
    while (length) {
        ssize_t w = write(a->log_fd, text, length);
        if (w < 0) { if (errno == EINTR) continue; return -errno; }
        text += w; length -= (size_t)w;
    }
    return 0;
}
static void log_close(void *ctx)
{ struct app *a = ctx; if (a->log_fd >= 0) { close(a->log_fd); a->log_fd = -1; } }
static void quit(void *ctx) { du_window_close(&((struct app *)ctx)->window); }

int main(int argc, char **argv)
{
    int channel, test; const char *path;
    if (acc_args(argc, argv, "sysmon", &channel, &test, &path) || path || test) return 2;
    struct app *a = calloc(1, sizeof(*a));
    if (!a) return 1;
    a->log_fd = -1;
    if (du_window_open_resizable(&a->window, &a->ui, channel, SMV_WIDTH, SMV_HEIGHT, "System Monitor", SMV_MIN_WIDTH, SMV_MIN_HEIGHT)) {
        free(a); return 1;
    }
    a->window.user = a;
    struct sysmon_host host = { a, sample, log_open, log_write, log_close, quit };
    if (smv_init(&a->view, &a->ui, &host)) { du_window_destroy(&a->window); free(a); return 1; }
    int r = du_run(&a->window, NULL, NULL);
    smv_stop_logging(&a->view);
    du_window_destroy(&a->window);
    free(a);
    return r < 0 ? 1 : 0;
}
