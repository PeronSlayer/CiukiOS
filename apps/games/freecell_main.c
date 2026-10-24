/* SPDX-License-Identifier: MIT
 * /Programs/freecell: CiukiOS FreeCell (desktop client, protocol v2).
 * Usage: freecell [--channel-fd=N] [--test=freecell] */
#include "freecell.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
struct fc_main { struct du_window window; struct du_ui ui; struct fc_app app; };
static void quit(void *host) { du_window_close(&((struct fc_main *)host)->window); }
int main(int argc, char **argv)
{
    int channel, test; const char *path;
    if (acc_args(argc, argv, "freecell", &channel, &test, &path) || path) return 2;
    struct fc_main *m = calloc(1, sizeof(*m));
    if (!m) return 1;
    if (du_window_open(&m->window, &m->ui, channel, FC_W, FC_H, "FreeCell")) {
        fprintf(stderr, "freecell: desktop connection failed: %s\n", strerror(errno)); free(m); return 1;
    }
    fc_deal(&m->app.game, test ? 1 : (unsigned)time(NULL) % FC_GAME_MAX + 1);
    m->app.quit = quit; m->app.host = m;
    fc_attach(&m->app, &m->ui);
    if (test) du_window_move(&m->window, 24, 48);
    int r = du_run(&m->window, NULL, NULL);
    du_window_destroy(&m->window); free(m);
    return r < 0 ? 1 : 0;
}
