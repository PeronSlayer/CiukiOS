/* SPDX-License-Identifier: MIT
 * /Programs/charmap: CiukiOS Character Map (desktop client, protocol v2).
 * Usage: charmap [--channel-fd=N] [--test=charmap] */
#include "charmap.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv)
{
    int channel, test; const char *path;
    if (acc_args(argc, argv, "charmap", &channel, &test, &path) || path) return 2;
    struct { struct du_window window; struct du_ui ui; struct charmap_app app; } *m = calloc(1, sizeof(*m));
    if (!m) return 1;
    if (du_window_open(&m->window, &m->ui, channel, CHARMAP_W, CHARMAP_H, "Character Map")) {
        fprintf(stderr, "charmap: desktop connection failed: %s\n", strerror(errno)); free(m); return 1;
    }
    charmap_attach(&m->app, &m->ui);
    if (test) du_window_move(&m->window, 24, 48);
    int r = du_run(&m->window, NULL, NULL);
    du_window_destroy(&m->window); free(m);
    return r < 0 ? 1 : 0;
}
