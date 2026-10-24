/* SPDX-License-Identifier: MIT
 * /Programs/wordpad: a WordPad-class rich text editor on libdeskui (G28).
 * Usage: wordpad [--channel-fd=N] [path] */
#include "fonts.h"
#include "../libdeskui/job.h"
#include "../libdeskui/recent.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct pad_main {
    struct wp_app app; struct du_ui ui; struct du_window window;
    struct du_job opening; const char *path; char *raw; size_t length; int open_error; char resolved[CN_PATH]; uint64_t open_revision;
};

static time_t host_now(void *p) { (void)p; return time(NULL); }
static void host_recent(const char *path) { (void)du_recent_add(path); }
static void host_quit(void *p) { du_window_close(&((struct pad_main *)p)->window); }
static int close_request(struct du_window *w, void *p) { (void)w; return wp_close_request(p); }
static void host_declined(void *p) { du_window_close_reply(&((struct pad_main *)p)->window, 0); }

/* The file named on the command line is read by a worker, so a large document never stalls the window. */
static void load_argument(void *arg)
{
    struct pad_main *m = arg;
    m->open_error = cn_resolve(m->resolved, "/", m->path);
    if (m->open_error) return;
    m->raw = malloc((size_t)WP_FILE_MAX + 1);
    m->open_error = m->raw ? m->app.fs->load(m->app.fs->ctx, m->resolved, m->raw, (size_t)WP_FILE_MAX + 1, &m->length) : -ENOMEM;
}
static void idle(void *arg)
{
    struct pad_main *m = arg;
    if (du_job_ready(&m->opening)) {
        du_job_finish(&m->opening);
        int r = m->open_error;
        if (!r && m->open_revision != m->app.doc.revision) r = -EBUSY;   /* the user typed meanwhile */
        if (!r && m->length > WP_FILE_MAX) r = -EFBIG;
        if (!r) r = wp_open_bytes(&m->app, m->resolved, m->raw, m->length);
        if (r) du_message(&m->ui, 999, "WordPad", r == -ENOTSUP ? "Word (.doc) and Write (.wri) documents cannot be opened yet." :
                          r == -EFBIG ? "The file is larger than 16 MiB." : strerror(-r), DU_DLG_ICON_ERROR, NULL, 0, 0, 0);
        free(m->raw); m->raw = NULL; m->path = NULL;
        wp_refresh(&m->app);
    } else if (m->path && !m->opening.started && m->window.configured && !m->ui.dirty && m->ui.frame_count) {
        m->open_revision = m->app.doc.revision;
        int r = du_job_start_ui(&m->opening, load_argument, m, &m->ui);
        if (r) { du_message(&m->ui, 999, "WordPad", strerror(-r), DU_DLG_ICON_ERROR, NULL, 0, 0, 0); m->path = NULL; }
    }
}

int main(int argc, char **argv)
{
    int channel = 3; const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--channel-fd=", 13)) {
            char *end; long n = strtol(argv[i] + 13, &end, 10);
            if (!argv[i][13] || *end || n < 3 || n >= 128) return 2;
            channel = (int)n;
        } else if (argv[i][0] == '-' || path) return 2;
        else path = argv[i];
    }
    struct pad_main *m = calloc(1, sizeof(*m));
    if (!m) return 1;
    char title[64] = "WordPad";
    if (path) {   /* the protocol has no title update: the file's name is part of the creation title */
        size_t n = (size_t)snprintf(title, sizeof(title), "WordPad - %s", cn_basename(path));
        for (size_t i = 0; i < n && i < sizeof(title) - 1; i++) if ((unsigned char)title[i] < 0x20 || (unsigned char)title[i] > 0x7e) title[i] = '?';
    }
    long dw = 640, dh = 480;
    const char *d = getenv("CIUKI_DISPLAY");
    if (d) { char *end; dw = strtol(d, &end, 10); if (*end == 'x') dh = strtol(end + 1, NULL, 10); }
    int width = dw >= 1000 && dh >= 700 ? 700 : dw >= 800 && dh >= 600 ? 640 : 600;
    int height = dw >= 1000 && dh >= 700 ? 520 : dw >= 800 && dh >= 600 ? 420 : 360;
    if (du_window_open_resizable(&m->window, &m->ui, channel, width, height, title, 480, 300)) {
        fprintf(stderr, "WordPad: desktop connection failed: %s\n", strerror(errno)); free(m); return 1;
    }
    struct wp_app *a = &m->app;
    m->window.close_request = close_request; m->window.user = a;
    a->fs = cn_native_fs(); a->draw = wp_fonts(); a->now = host_now; a->quit = host_quit; a->declined = host_declined;
    a->recent = host_recent; a->host = m;
    const char *tz = getenv("LANG");
    a->a4 = !tz || strncmp(tz, "en_US", 5) != 0;   /* A4 unless the locale says US (G24 Regional Settings later chooses the paper) */
    if (wp_attach(a, &m->ui)) { fprintf(stderr, "WordPad: out of memory\n"); wp_detach(a); du_window_destroy(&m->window); free(m); return 1; }
    m->path = path;
    du_window_accept_files(&m->window, 1);
    int r = du_run(&m->window, idle, m);
    du_job_finish(&m->opening); free(m->raw);
    wp_detach(a); du_window_destroy(&m->window); free(m);
    return r < 0 ? 1 : 0;
}
