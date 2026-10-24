/* SPDX-License-Identifier: MIT
 * /Programs/scandisk: ScanDisk (desktop client).
 * Usage: scandisk [--channel-fd=N] [--test=scandisk] [drive-root-path] */
#include "scandisk.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct scandisk_main { struct du_window window; struct du_ui ui; struct scandisk_app app; };
static void quit(void *host) { du_window_close(&((struct scandisk_main *)host)->window); }
int main(int argc, char **argv)
{
    int channel, test; const char *path;
    if (acc_args(argc, argv, "scandisk", &channel, &test, &path)) return 2;
    int drive = path ? scandisk_drive_of(path) : -1;
    if (path && drive < 0) return 2;
    struct scandisk_main *m = calloc(1, sizeof(*m));
    if (!m) return 1;
    if (du_window_open(&m->window, &m->ui, channel, SD_W, SD_H, "ScanDisk")) {
        fprintf(stderr, "scandisk: desktop connection failed: %s\n", strerror(errno)); free(m); return 1;
    }
    m->app.test = test; m->app.quit = quit; m->app.host = m;
    scandisk_attach(&m->app, &m->ui, drive);
    if (test) du_window_move(&m->window, 24, 48);
    int r = du_run(&m->window, NULL, NULL);
    du_window_destroy(&m->window); free(m);
    return r < 0 ? 1 : 0;
}
