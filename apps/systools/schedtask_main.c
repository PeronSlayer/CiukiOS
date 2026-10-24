/* SPDX-License-Identifier: MIT
 * /Programs/schedtask: Scheduled Tasks (desktop client, protocol v2).
 * Usage: schedtask [--channel-fd=N] [--test=schedtask]
 * The task file is /system/settings/schedule.cfg, replaced atomically; the clock is the system zone's local time. */
#define _POSIX_C_SOURCE 200809L
#include "schedtask_ui.h"
#include "sysreq.h"
#include "../accessories/acc.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define TEST_MINUTES 29861861   /* 2026-10-11 09:41 local, the other accessories' test clock */
struct app {
    struct du_window window; struct du_ui ui; struct stui stui; struct stui_host host;
    int test; uint32_t serial;
};
static int64_t now_minutes(void *ctx)
{
    struct app *a = ctx;
    if (a->test) return TEST_MINUTES;
    time_t t = time(NULL); struct tm l;
    if (!localtime_r(&t, &l)) return (int64_t)t / 60;
    int64_t y = (int64_t)l.tm_year + 1900 - (l.tm_mon < 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    int64_t doy = (153 * (l.tm_mon < 2 ? l.tm_mon + 10 : l.tm_mon - 2) + 2) / 5 + l.tm_mday - 1;
    int64_t days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    return days * 1440 + l.tm_hour * 60 + l.tm_min;
}
static int load(void *ctx, char *text, size_t cap, size_t *length)
{
    (void)ctx;
    int fd = open(SS_PATH, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    size_t done = 0; int e = 0;
    while (done < cap) {
        ssize_t n = read(fd, text + done, cap - done);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { e = -errno; break; }
        if (!n) break;
        done += (size_t)n;
    }
    close(fd);
    if (e) return e;
    *length = done; return 0;
}
/* Temp file in the same directory, fsync, rename over the old file. */
static int save(void *ctx, const char *text, size_t length)
{
    (void)ctx;
    static const char temp[] = SS_PATH ".tmp";
    int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) return -errno;
    int e = 0; size_t done = 0;
    while (done < length) {
        ssize_t n = write(fd, text + done, length - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { e = n < 0 ? -errno : -EIO; break; }
        done += (size_t)n;
    }
    if (!e && fsync(fd)) e = -errno;
    if (close(fd) && !e) e = -errno;
    if (!e && rename(temp, SS_PATH)) {
        e = -errno;
        if (e == -EEXIST && !unlink(SS_PATH) && !rename(temp, SS_PATH)) e = 0;   /* a backend that does not replace */
    }
    if (e) unlink(temp);
    return e;
}
static int programs(void *ctx, char names[][STUI_NAME], unsigned cap, unsigned *count)
{
    (void)ctx;
    DIR *d = opendir("/Programs");
    if (!d) return -errno;
    unsigned n = 0; struct dirent *e;
    while ((e = readdir(d)) && n < cap) {
        if (strlen(e->d_name) >= STUI_NAME || e->d_name[0] == '.') continue;
        snprintf(names[n++], STUI_NAME, "%s", e->d_name);
    }
    closedir(d);
    *count = n; return 0;
}
static int launch(void *ctx, const char *program)
{ struct app *a = ctx; return sysreq_launch(&a->window, &a->serial, program); }
static void quit(void *ctx) { struct app *a = ctx; du_window_close(&a->window); }
static void on_message(struct du_window *w, const struct desk_packet *p, void *ctx)
{
    (void)w; struct app *a = ctx;
    if (p->opcode == DESK_TASK_RESULT && p->request == DESK_LAUNCH) stui_launch_result(&a->stui, (int)p->serial, p->result);
}
int main(int argc, char **argv)
{
    int channel; const char *path; int test;
    if (acc_args(argc, argv, "schedtask", &channel, &test, &path) || path) return 2;
    struct app *a = calloc(1, sizeof(*a));
    if (!a) return 1;
    a->test = test;
    if (du_window_open(&a->window, &a->ui, channel, STUI_W, STUI_H, "Scheduled Tasks")) {
        fprintf(stderr, "schedtask: desktop connection failed: %s\n", strerror(errno)); free(a); return 1;
    }
    a->host = (struct stui_host){ .ctx = a, .now_minutes = now_minutes, .load = load, .save = save,
                                  .programs = programs, .launch = launch, .exit = quit };
    a->window.message = on_message; a->window.user = a;
    stui_init(&a->stui, &a->ui, &a->host);
    if (test) du_window_move(&a->window, 24, 40);
    int r = du_run(&a->window, NULL, NULL);
    du_window_destroy(&a->window); free(a);
    return r < 0 ? 1 : 0;
}
