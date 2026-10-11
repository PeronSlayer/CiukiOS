/* SPDX-License-Identifier: MIT
 * /bin/ciuknote: a Notepad-class text editor on libdeskui (f2-32).
 * Usage: ciuknote [--channel-fd=N] [--test=ciuknote] [path] */
#include "app.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TEST_TIME 1791711660 /* 2026-10-11 09:41:00 UTC: the fixed clock of --test runs */

struct host { struct du_window window; struct du_ui ui; struct cn_app app; };

static time_t host_now(void *p)
{ struct host *h = p; return h->app.test ? (time_t)TEST_TIME : time(NULL); }
static uint64_t host_mono(void *p) { (void)p; return du_now(); }
static void host_quit(void *p) { struct host *h = p; du_window_close(&h->window); }
static int close_request(struct du_window *w, void *p)
{ (void)w; return cn_close_request(&((struct host *)p)->app); }

int main(int argc, char **argv)
{
    int channel = 3, test = 0; const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--channel-fd=", 13)) {
            char *end; long n = strtol(argv[i] + 13, &end, 10);
            if (!argv[i][13] || *end || n < 3 || n >= 128) return 2;
            channel = (int)n;
        } else if (!strcmp(argv[i], "--test=ciuknote")) test = 1;
        else if (argv[i][0] == '-' || path) return 2;
        else path = argv[i];
    }
    struct host *h = calloc(1, sizeof(*h));
    if (!h) return 1;
    char title[64] = "CiukNote";
    if (path) { /* the protocol has no title update: the file's name is part of the creation title */
        size_t n = (size_t)snprintf(title, sizeof(title), "CiukNote - %s", cn_basename(path));
        for (size_t i = 0; i < n && i < sizeof(title) - 1; i++) if ((unsigned char)title[i] < 0x20 || (unsigned char)title[i] > 0x7e) title[i] = '?';
    }
    if (du_window_open(&h->window, &h->ui, channel, CN_WIDTH, CN_HEIGHT, title)) {
        fprintf(stderr, "CiukNote: desktop connection failed: %s\n", strerror(errno)); free(h); return 1;
    }
    h->window.close_request = close_request; h->window.user = h;
    h->app.fs = cn_native_fs(); h->app.now = host_now; h->app.mono = host_mono; h->app.quit = host_quit;
    h->app.host = h; h->app.test = test;
    if (cn_attach(&h->app, &h->ui)) { fprintf(stderr, "CiukNote: out of memory\n"); du_window_destroy(&h->window); free(h); return 1; }
    if (path) cn_open_argument(&h->app, path);
    if (test) du_window_move(&h->window, 24, 48); /* fixed placement for the scripted QEMU case */
    int r;
    while ((r = du_window_turn(&h->window, du_now())) > 0) {
        char line[256];
        if (test && cn_checkpoint(&h->app, line, sizeof(line))) { puts(line); fflush(stdout); }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000};
        nanosleep(&pause, NULL);
    }
    cn_detach(&h->app); du_window_destroy(&h->window); free(h);
    return r < 0 ? 1 : 0;
}
