/* SPDX-License-Identifier: MIT */
#include "regedit.h"
#include "../accessories/acc.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void idle(void *arg) { re_startup_poll(arg); }
int main(int argc, char **argv)
{
    int channel, test; const char *path;
    if (acc_args(argc, argv, "regedit", &channel, &test, &path) || path) return 2;
    struct { struct du_window window; struct du_ui ui; struct re_app app; } *m = calloc(1, sizeof(*m));
    if (!m) return 1;
    int w, h; acc_window_size(RE_WIDTH, RE_HEIGHT, &w, &h);
    if (du_window_open_resizable(&m->window, &m->ui, channel, w, h, "Registry Editor", 400, 240)) {
        fprintf(stderr, "regedit: %s\n", strerror(errno)); free(m); return 1;
    }
    du_help_topic(&m->ui, "regedit");   /* G15: F1 opens this program's Help topic */
    int result = re_attach_lazy(&m->app, &m->ui, REG_DIRECTORY);
    if (!result) { if (test) du_window_move(&m->window, 24, 48); result = du_run(&m->window,idle,&m->app); }
    re_destroy(&m->app); du_window_destroy(&m->window); free(m); return result < 0 ? 1 : 0;
}
