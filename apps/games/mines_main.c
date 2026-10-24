/* SPDX-License-Identifier: MIT
 * /Programs/mines: CiukiOS Minesweeper (desktop client, protocol v2).
 * Usage: mines [--channel-fd=N] [--test=mines]. Best times: ~/.minesweeper (HOME, else /System/settings). */
#include "mines.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
struct mines_main { struct du_window window; struct du_ui ui; struct mines_app app; char path[256]; };
static int reopen_w, reopen_h;
static void quit(void *host) { du_window_close(&((struct mines_main *)host)->window); }
static void relayout(void *host, int w, int h) { reopen_w = w; reopen_h = h; du_window_close(&((struct mines_main *)host)->window); }
int main(int argc, char **argv)
{
    int channel, test; const char *path;
    if (acc_args(argc, argv, "mines", &channel, &test, &path) || path) return 2;
    struct mines_main *m = calloc(1, sizeof(*m));
    if (!m) return 1;
    int w, h;
    const char *home = getenv("HOME");
    snprintf(m->path, sizeof(m->path), "%s/minesweeper.cfg", home && *home ? home : "/System/settings");
    if (!test) m->app.best_path = m->path;
    mines_init(&m->app.game, test ? 1 : (uint32_t)time(NULL) ^ 0x5eed1234u);
    if (test) acc_window_size(MINES_W, MINES_H, &w, &h);   /* the harness keeps the full-size window */
    else mines_window_size(&m->app.game, &w, &h);          /* otherwise the window wraps its board, like Windows */
    m->app.quit = quit; m->app.host = m;
    if (!test) m->app.relayout = relayout;
    int r = 0;
    for (;;) {
        reopen_w = 0;
        if (du_window_open(&m->window, &m->ui, channel, w, h, "Minesweeper")) {
            fprintf(stderr, "mines: desktop connection failed: %s\n", strerror(errno)); free(m); return 1;
        }
        mines_attach(&m->app, &m->ui);
        if (test) du_window_move(&m->window, 24, 48);
        r = du_run(&m->window, NULL, NULL);
        du_window_destroy(&m->window);
        if (!reopen_w || r < 0) break;
        w = reopen_w; h = reopen_h;   /* a level change: the same game in a window that wraps the new board */
        memset(&m->ui, 0, sizeof(m->ui));
    }
    free(m);
    return r < 0 ? 1 : 0;
}
