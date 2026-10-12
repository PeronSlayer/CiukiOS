/* SPDX-License-Identifier: MIT
 * /Programs/clock: CiukiOS Clock and Calendar (desktop client, protocol v2).
 * Usage: clock [--channel-fd=N] [--test=clock] */
#include "clock.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define TEST_TIME 1791711660   /* 2026-10-11 09:41:00 UTC, as CiukNote's test clock */
static int test_mode;
static int64_t now_utc(void *host) { (void)host; return test_mode ? TEST_TIME : (int64_t)time(NULL); }
int main(int argc, char **argv)
{
    int channel; const char *path;
    if (acc_args(argc, argv, "clock", &channel, &test_mode, &path) || path) return 2;
    struct { struct du_window window; struct du_ui ui; struct clock_app app; } *m = calloc(1, sizeof(*m));
    if (!m) return 1;
    if (du_window_open(&m->window, &m->ui, channel, CLOCK_W, CLOCK_H, "Clock and Calendar")) {
        fprintf(stderr, "clock: desktop connection failed: %s\n", strerror(errno)); free(m); return 1;
    }
    clock_attach(&m->app, &m->ui, now_utc, NULL);
    du_timer_start(&m->ui, 1, 250000000ull, 1);   /* four checks a second keep the seconds hand on time */
    if (test_mode) du_window_move(&m->window, 24, 48);
    int r = du_run(&m->window, NULL, NULL);
    du_window_destroy(&m->window); free(m);
    return r < 0 ? 1 : 0;
}
