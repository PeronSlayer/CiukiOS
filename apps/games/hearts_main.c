/* SPDX-License-Identifier: MIT
 * /Programs/hearts: CiukiOS Hearts (desktop client, protocol v2).
 * Usage: hearts [--channel-fd=N] [--test=hearts] */
#include "hearts.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
struct he_main { struct du_window window; struct du_ui ui; struct hearts_app app; };
static void quit(void *host) { du_window_close(&((struct he_main *)host)->window); }
int main(int argc, char **argv)
{
    int channel, test; const char *path;
    if (acc_args(argc, argv, "hearts", &channel, &test, &path) || path) return 2;
    struct he_main *m = calloc(1, sizeof(*m));
    if (!m) return 1;
    if (du_window_open(&m->window, &m->ui, channel, HE_W, HE_H, "Hearts")) {
        fprintf(stderr, "hearts: desktop connection failed: %s\n", strerror(errno)); free(m); return 1;
    }
    hearts_new_game(&m->app.game, test ? 1 : (uint32_t)time(NULL) ^ 0x4ea47500u);
    m->app.quit = quit; m->app.host = m;
    hearts_attach(&m->app, &m->ui);
    du_timer_start(&m->ui, HV_TIMER, 600000000ull, 1);   /* the computer players move six tenths of a second apart */
    if (test) du_window_move(&m->window, 24, 48);
    int r = du_run(&m->window, NULL, NULL);
    du_window_destroy(&m->window); free(m);
    return r < 0 ? 1 : 0;
}
