/* SPDX-License-Identifier: MIT
 * /Programs/logview: CiukiOS Log Viewer (desktop client, protocol v2).
 * Usage: logview [--channel-fd=N] [--test=logview] */
#include "logview.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct lv_main { struct du_window window; struct du_ui ui; struct lv_app app; };
static void quit(void *host) { du_window_close(&((struct lv_main *)host)->window); }
int main(int argc, char **argv)
{
    int channel, test; const char *path;
    if (acc_args(argc, argv, "logview", &channel, &test, &path) || path) return 2;
    struct lv_main *m = calloc(1, sizeof(*m));
    if (!m) return 1;
    int w, h;
    acc_window_size(LV_W, LV_H, &w, &h);
    if (test) { w = LV_W; h = LV_H; }
    if (du_window_open_resizable(&m->window, &m->ui, channel, w, h, "Log Viewer", 440, 280)) {
        fprintf(stderr, "logview: desktop connection failed: %s\n", strerror(errno)); free(m); return 1;
    }
    if (lv_attach(&m->app, &m->ui)) { du_window_destroy(&m->window); free(m); return 1; }
    m->app.quit = quit; m->app.host = m;
    if (test) du_window_move(&m->window, 24, 48);
    int r = du_run(&m->window, NULL, NULL);
    lv_detach(&m->app); du_window_destroy(&m->window); free(m);
    return r < 0 ? 1 : 0;
}
