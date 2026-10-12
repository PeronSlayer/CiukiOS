/* SPDX-License-Identifier: MIT
 * /Programs/viewer: the CiukiOS Image Viewer (desktop client, protocol v2).
 * Usage: viewer [--channel-fd=N] [--test=viewer] [path] */
#include "viewer.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct viewer_main { struct du_window window; struct du_ui ui; struct viewer_app app; };
static void quit(void *host) { du_window_close(&((struct viewer_main *)host)->window); }
int main(int argc, char **argv)
{
    int channel, test; const char *path;
    if (acc_args(argc, argv, "viewer", &channel, &test, &path)) return 2;
    struct viewer_main *m = calloc(1, sizeof(*m));
    if (!m) return 1;
    char title[64]; int w, h;
    acc_title(title, sizeof(title), "Image Viewer", path);
    acc_window_size(VIEWER_W, VIEWER_H, &w, &h);
    if (test) { w = VIEWER_W; h = VIEWER_H; }
    if (du_window_open_resizable(&m->window, &m->ui, channel, w, h, title, 320, 240)) {
        fprintf(stderr, "viewer: desktop connection failed: %s\n", strerror(errno)); free(m); return 1;
    }
    viewer_attach(&m->app, &m->ui, viewer_native_fs());
    m->app.quit = quit; m->app.host = m;
    if (path) viewer_open(&m->app, path);
    if (test) du_window_move(&m->window, 24, 48);
    int r = du_run(&m->window, NULL, NULL);
    viewer_detach(&m->app); du_window_destroy(&m->window); free(m);
    return r < 0 ? 1 : 0;
}
