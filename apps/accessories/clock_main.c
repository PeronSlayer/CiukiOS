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
/* G1: the wall clock of the system zone (libc localtime follows it), as
 * seconds of the same calendar since 1970 for the model's arithmetic. */
static int64_t now_local(void *host)
{
    (void)host;
    if (test_mode) return TEST_TIME;
    time_t t = time(NULL);
    struct tm l;
    if (!localtime_r(&t, &l)) return (int64_t)t;
    int64_t y = (int64_t)l.tm_year + 1900 - (l.tm_mon < 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    int64_t doy = (153 * (l.tm_mon < 2 ? l.tm_mon + 10 : l.tm_mon - 2) + 2) / 5 + l.tm_mday - 1;
    int64_t days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    return days * 86400 + l.tm_hour * 3600 + l.tm_min * 60 + l.tm_sec;
}
int main(int argc, char **argv)
{
    int channel; const char *path;
    if (acc_args(argc, argv, "clock", &channel, &test_mode, &path) || path) return 2;
    struct { struct du_window window; struct du_ui ui; struct clock_app app; } *m = calloc(1, sizeof(*m));
    if (!m) return 1;
    if (du_window_open(&m->window, &m->ui, channel, CLOCK_W, CLOCK_H, "Clock and Calendar")) {
        fprintf(stderr, "clock: desktop connection failed: %s\n", strerror(errno)); free(m); return 1;
    }
    du_help_topic(&m->ui, "clock");   /* G15: F1 opens this program's Help topic */
    clock_attach(&m->app, &m->ui, now_local, NULL);
    du_timer_start(&m->ui, 1, 250000000ull, 1);   /* four checks a second keep the seconds hand on time */
    if (test_mode) du_window_move(&m->window, 24, 48);
    int r = du_run(&m->window, NULL, NULL);
    du_window_destroy(&m->window); free(m);
    return r < 0 ? 1 : 0;
}
