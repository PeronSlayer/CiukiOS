/* SPDX-License-Identifier: MIT
 * /Programs/calc: the CiukiOS Calculator (desktop client, protocol v2).
 * Usage: calc [--channel-fd=N] [--test=calc] */
#include "calc.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    int channel, test; const char *path;
    if (acc_args(argc, argv, "calc", &channel, &test, &path) || path) return 2;
    struct { struct du_window window; struct du_ui ui; struct calc_app app; } *m = calloc(1, sizeof(*m));
    if (!m) return 1;
    if (du_window_open(&m->window, &m->ui, channel, CALC_W, CALC_H, "Calculator")) {
        fprintf(stderr, "calc: desktop connection failed: %s\n", strerror(errno)); free(m); return 1;
    }
    du_help_topic(&m->ui, "calc");   /* G15: F1 opens this program's Help topic */
    calc_attach(&m->app, &m->ui);
    if (test) du_window_move(&m->window, 24, 48);
    int r = du_run(&m->window, NULL, NULL);
    du_window_destroy(&m->window); free(m);
    return r < 0 ? 1 : 0;
}
