/* SPDX-License-Identifier: MIT
 * /bin/help: CiukiOS Help and text viewer (desktop client, protocol v2).
 * Usage: help [--channel-fd=N] [--test=help] [text file] */
#include "help.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct help_main { struct du_window window; struct du_ui ui; struct help_app app; };
static void quit(void *host) { du_window_close(&((struct help_main *)host)->window); }
int main(int argc, char **argv)
{
    int channel, test; const char *path;
    if (acc_args(argc, argv, "help", &channel, &test, &path)) return 2;
    struct help_main *m = calloc(1, sizeof(*m));
    if (!m) return 1;
    char title[64]; int w, h;
    acc_title(title, sizeof(title), "CiukiOS Help", path);
    acc_window_size(HELP_W, HELP_H, &w, &h);
    if (test) { w = HELP_W; h = HELP_H; }
    if (du_window_open(&m->window, &m->ui, channel, w, h, title)) {
        fprintf(stderr, "help: desktop connection failed: %s\n", strerror(errno)); free(m); return 1;
    }
    if (help_attach(&m->app, &m->ui)) { du_window_destroy(&m->window); free(m); return 1; }
    m->app.quit = quit; m->app.host = m;
    if (path) help_open_file(&m->app, path);
    if (test) du_window_move(&m->window, 24, 48);
    int r = du_run(&m->window, NULL, NULL);
    help_detach(&m->app); du_window_destroy(&m->window); free(m);
    return r < 0 ? 1 : 0;
}
