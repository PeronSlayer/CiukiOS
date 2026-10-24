/* SPDX-License-Identifier: MIT
 * /Programs/msconfig: System Configuration Utility (desktop client).
 * Usage: msconfig [--channel-fd=N] [--test=msconfig] */
#include "msconfig.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct msconfig_main { struct du_window window; struct du_ui ui; struct msconfig_app app; };
static void quit(void *host) { du_window_close(&((struct msconfig_main *)host)->window); }
int main(int argc, char **argv)
{
    int channel, test; const char *path;
    if (acc_args(argc, argv, "msconfig", &channel, &test, &path) || path) return 2;
    struct msconfig_main *m = calloc(1, sizeof(*m));
    if (!m) return 1;
    if (du_window_open(&m->window, &m->ui, channel, MC_W, MC_H, "System Configuration Utility")) {
        fprintf(stderr, "msconfig: desktop connection failed: %s\n", strerror(errno)); free(m); return 1;
    }
    m->app.quit = quit; m->app.host = m;
    msconfig_attach(&m->app, &m->ui);
    if (test) du_window_move(&m->window, 24, 48);
    int r = du_run(&m->window, NULL, NULL);
    sfc_free(&m->app.sfc); free(m->app.sfc_items); free(m->app.sfc_text);
    du_window_destroy(&m->window); free(m);
    return r < 0 ? 1 : 0;
}
