/* SPDX-License-Identifier: MIT
 * Files entry point: desktop channel, native VFS, program launching. */
#include "app.h"
#include <ciuki/spawn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECKPOINT_SERIAL 0xf2310000u

struct files_main { struct files_app app; struct du_ui ui; struct du_window window; int checkpoint, sent; };

static int launch(struct files_app *a, const char *program, char *const argv[])
{
    (void)a;
    char *env[] = {"LC_ALL=C", "TZ=UTC0", "HOME=/home", "TMPDIR=/tmp", "PATH=/bin", NULL};
    ciuki_pid_t pid = ciuki_spawn(program, argv, env, NULL, 0, 0);
    return pid < 0 ? -errno : (int)pid;
}

static void close_window(struct files_app *a) { du_window_close(&((struct files_main *)a->user)->window); }

/* Test-only milestones: a PING whose serial names the completed stage. It
 * is queued after the turn that rendered the stage and queued its DAMAGE,
 * so the desktop presents that frame before it reports the checkpoint. */
static void checkpoint(struct files_app *a, int stage) { ((struct files_main *)a->user)->checkpoint = stage; }

static int portrait(uint32_t *pixels)
{
    FILE *f = fopen("/system/assets/ciuki-portrait.xrgb", "rb");
    if (!f) return -1;
    size_t n = fread(pixels, 4, 256 * 256, f);
    fclose(f);
    return n == 256 * 256 ? 0 : -1;
}

static int close_request(struct du_window *w, void *user) { (void)w; return files_close_request(user); }

static void idle(void *arg)
{
    struct files_main *m = ((struct files_app *)arg)->user;
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) { }
    if (m->checkpoint > m->sent && !m->window.damaged && !m->ui.dirty &&
        !du_window_ping(&m->window, CHECKPOINT_SERIAL + (unsigned)m->sent + 1)) m->sent++;
}

/* The desktop passes CIUKI_DISPLAY=<width>x<height>; size the window to fit. */
static void window_size(int *w, int *h)
{
    const char *d = getenv("CIUKI_DISPLAY");
    long dw = 640, dh = 480;
    if (d) { char *end; dw = strtol(d, &end, 10); if (*end == 'x') dh = strtol(end + 1, NULL, 10); }
    if (dw >= 1000 && dh >= 700) { *w = 760; *h = 520; }
    else if (dw >= 800 && dh >= 600) { *w = 680; *h = 440; }
    else { *w = 620; *h = 380; }
}

int main(int argc, char **argv)
{
    int channel = 3, test = 0;
    const char *path = "/";
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--channel-fd=", 13)) {
            char *end; long n = strtol(argv[i] + 13, &end, 10);
            if (!argv[i][13] || *end || n < 3 || n >= 128) return 2;
            channel = (int)n;
        } else if (!strcmp(argv[i], "--test=files")) test = 1;
        else if (argv[i][0] == '/') path = argv[i];
        else return 2;
    }
    struct files_main *m = calloc(1, sizeof(*m));
    if (!m) return 1;
    int width, height;
    window_size(&width, &height);
    if (test) { width = 760; height = 520; }
    if (du_window_open(&m->window, &m->ui, channel, width, height, "Files")) {
        fprintf(stderr, "Files: desktop connection failed: %s\n", strerror(errno));
        free(m);
        return 1;
    }
    struct files_app *a = &m->app;
    a->user = m; a->test = test; a->spawn = launch; a->close = close_window; a->portrait = portrait;
    if (test) a->checkpoint = checkpoint;
    m->window.close_request = close_request; m->window.user = a;
    if (files_setup(a, &m->ui, fm_native_vfs(), path)) { du_window_destroy(&m->window); free(m); return 1; }
    /* Fixed placement keeps the scripted desktop case deterministic. */
    if (test) du_window_move(&m->window, 4, 36);
    int r = du_run(&m->window, idle, a);
    files_destroy(a);
    du_window_destroy(&m->window);
    free(m);
    return r < 0 ? 1 : 0;
}
