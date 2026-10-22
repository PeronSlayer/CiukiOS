/* SPDX-License-Identifier: MIT
 * /Programs/ciuknote: a Notepad-class text editor on libdeskui (f2-32).
 * Usage: ciuknote [--channel-fd=N] [--test=ciuknote] [path] */
#include "app.h"
#include "../libdeskui/job.h"
#include "../libdeskui/recent.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TEST_TIME 1791711660   /* 2026-10-11 09:41:00 UTC: the fixed clock of --test runs */
#define CHECKPOINT_SERIAL 0xf2320000u

struct note_main { struct cn_app app; struct du_ui ui; struct du_window window; int checkpoint, sent; struct du_job opening; const char *path; char *raw; size_t length; int open_error; char resolved[CN_PATH]; uint64_t open_revision; };

static time_t host_now(void *p) { struct note_main *m = p; return m->app.test ? (time_t)TEST_TIME : time(NULL); }
static void host_recent(const char *path) { (void)du_recent_add(path); }
static void host_quit(void *p) { du_window_close(&((struct note_main *)p)->window); }
static int close_request(struct du_window *w, void *p) { (void)w; return cn_close_request(p); }

/* Test-only milestones, as Files: a PING whose serial names the completed stage is queued after
 * the turn that rendered it and queued its DAMAGE, so the desktop presents that frame before it
 * reports the checkpoint. The serial log carries the surface digest of the same frame. */
static void on_checkpoint(struct cn_app *a, int stage) { ((struct note_main *)a->host)->checkpoint = stage; }

static void load_argument(void *arg)
{
    struct note_main *m=arg;
    m->open_error=cn_resolve(m->resolved,"/",m->path); if (m->open_error) return;
    m->raw=malloc(2*(size_t)CN_LIMIT+1);
    m->open_error=m->raw ? m->app.fs->load(m->app.fs->ctx,m->resolved,m->raw,2*(size_t)CN_LIMIT+1,&m->length) : -ENOMEM;
}
static void idle(void *arg)
{
    struct note_main *m = arg;
    if (du_job_ready(&m->opening)) {
        du_job_finish(&m->opening);
        int r=m->open_error;
        if ((r==0 || r==-ENOENT) && m->open_revision!=m->app.ta.revision) r=-EBUSY;
        if (!r) r=cn_open_bytes(&m->app,m->resolved,m->raw,m->length);
        else if (r==-ENOENT) cn_open_argument(&m->app,m->resolved);
        if (r && r!=-ENOENT) du_message(&m->ui,999,"CiukNote",strerror(-r),DU_DLG_ICON_ERROR,NULL,0,0,0);
        free(m->raw); m->raw=NULL; m->path=NULL;
    } else if (m->path && !m->opening.started && m->window.configured && !m->ui.dirty && m->ui.frame_count) {
        m->open_revision=m->app.ta.revision;
        int r=du_job_start(&m->opening,load_argument,m);
        if (r) { du_message(&m->ui,999,"CiukNote",strerror(-r),DU_DLG_ICON_ERROR,NULL,0,0,0); m->path=NULL; }
    }
    if (m->checkpoint > m->sent && !m->window.damaged && !m->ui.dirty &&
        !du_window_ping(&m->window, CHECKPOINT_SERIAL + (unsigned)m->sent + 1)) {
        char line[256];
        cn_describe(&m->app, m->sent + 1, line, sizeof(line));
        puts(line); fflush(stdout);
        m->sent++;
    }
}

static int portrait(uint32_t *pixels)
{
    FILE *f = fopen("/system/assets/ciuki-portrait.xrgb", "rb");
    if (!f) return -1;
    size_t n = fread(pixels, 4, 256 * 256, f);
    fclose(f);
    return n == 256 * 256 ? 0 : -1;
}

/* The desktop passes CIUKI_DISPLAY=<width>x<height>; size the window to fit. */
static void window_size(int *w, int *h)
{
    const char *d = getenv("CIUKI_DISPLAY");
    long dw = 640, dh = 480;
    if (d) { char *end; dw = strtol(d, &end, 10); if (*end == 'x') dh = strtol(end + 1, NULL, 10); }
    if (dw >= 1000 && dh >= 700) { *w = 640; *h = 440; }
    else if (dw >= 800 && dh >= 600) { *w = 600; *h = 380; }
    else { *w = 560; *h = 320; }
}

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
    struct note_main *m = calloc(1, sizeof(*m));
    if (!m) return 1;
    char title[64] = "CiukNote";
    if (path) { /* the protocol has no title update: the file's name is part of the creation title */
        size_t n = (size_t)snprintf(title, sizeof(title), "CiukNote - %s", cn_basename(path));
        for (size_t i = 0; i < n && i < sizeof(title) - 1; i++) if ((unsigned char)title[i] < 0x20 || (unsigned char)title[i] > 0x7e) title[i] = '?';
    }
    int width, height;
    window_size(&width, &height);
    if (test) { width = 640; height = 440; }
    if (du_window_open_resizable(&m->window, &m->ui, channel, width, height, title, 440, 280)) {
        fprintf(stderr, "CiukNote: desktop connection failed: %s\n", strerror(errno)); free(m); return 1;
    }
    struct cn_app *a = &m->app;
    m->window.close_request = close_request; m->window.user = a;
    a->fs = cn_native_fs(); a->now = host_now; a->quit = host_quit; a->portrait = portrait;
    if (!test) a->recent = host_recent;   /* scripted runs keep the disk unchanged */
    a->host = m; a->test = test;
    if (test) a->checkpoint = on_checkpoint;
    if (cn_attach(a, &m->ui)) { fprintf(stderr, "CiukNote: out of memory\n"); du_window_destroy(&m->window); free(m); return 1; }
    m->path=path;
    if (test) du_window_move(&m->window, 24, 48); /* fixed placement for the scripted QEMU case */
    int r = du_run(&m->window, idle, m);
    du_job_finish(&m->opening); free(m->raw);
    cn_detach(a); du_window_destroy(&m->window); free(m);
    return r < 0 ? 1 : 0;
}
