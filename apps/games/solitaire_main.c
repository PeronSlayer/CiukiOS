/* SPDX-License-Identifier: MIT
 * /Programs/solitaire: CiukiOS Solitaire (desktop client, protocol v2).
 * Usage: solitaire [--channel-fd=N] [--test=solitaire] */
#include "solitaire.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
struct sol_main { struct du_window window; struct du_ui ui; struct sol_app app; };
static void quit(void *host) { du_window_close(&((struct sol_main *)host)->window); }
int main(int argc, char **argv)
{
    int channel, test; const char *path;
    if (acc_args(argc, argv, "solitaire", &channel, &test, &path) || path) return 2;
    struct sol_main *m = calloc(1, sizeof(*m));
    if (!m) return 1;
    if (du_window_open(&m->window, &m->ui, channel, SOL_W, SOL_H, "Solitaire")) {
        fprintf(stderr, "solitaire: desktop connection failed: %s\n", strerror(errno)); free(m); return 1;
    }
    sol_init(&m->app.game, test ? 1 : (uint32_t)time(NULL) ^ 0x50111717u);
    m->app.quit = quit; m->app.host = m;
    sol_attach(&m->app, &m->ui);
    if (test) du_window_move(&m->window, 24, 48);
    int r = du_run(&m->window, NULL, NULL);
    du_window_destroy(&m->window); free(m);
    return r < 0 ? 1 : 0;
}
