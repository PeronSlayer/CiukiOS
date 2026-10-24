/* SPDX-License-Identifier: MIT
 * /Programs/ciuknote: a Notepad-class text editor on libdeskui (f2-32).
 * Usage: ciuknote [--channel-fd=N] [--test=ciuknote] [path] */
#include "app.h"
#include "../libdeskui/job.h"
#include "../libdeskui/recent.h"
#include "../libprint/dialog.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TEST_TIME 1791711660   /* 2026-10-11 09:41:00 UTC: the fixed clock of --test runs */
#define CHECKPOINT_SERIAL 0xf2320000u

struct note_main { struct cn_app app; struct du_ui ui; struct du_window window; struct cp_ui printing; int checkpoint, sent; struct du_job opening; const char *path; char *raw; size_t length; int open_error; char resolved[CN_PATH]; uint64_t open_revision; uint32_t launch_serial; };
static void print_request(void *arg,int setup)
{
    struct note_main *m=arg;
    if (setup) cp_ui_setup(&m->printing,&m->ui);
    else cp_ui_text(&m->printing,&m->ui,m->app.named?cn_basename(m->app.path):"Untitled - CiukNote",m->app.ta.text,m->app.ta.len);
}
static void print_event(struct du_ui *u,const struct du_event *e,void *arg)
{
    struct cn_app *a=arg;struct note_main *m=a->host;
    if (!cp_ui_event(&m->printing,u,e)) cn_event(u,e,a);
}

static time_t host_now(void *p) { struct note_main *m = p; return m->app.test ? (time_t)TEST_TIME : time(NULL); }
static void host_recent(const char *path) { (void)du_recent_add(path); }
static void host_quit(void *p) { du_window_close(&((struct note_main *)p)->window); }
static int close_request(struct du_window *w, void *p) { (void)w; return cn_close_request(p); }
/* G28: a document above CiukNote's limit opens in WordPad. Only the desktop can give a window its channel, so
 * the request is a protocol v2 LAUNCH of /Programs/wordpad with the absolute path as its single argument
 * (the desktop honours it for CiukNote, as for Files). The answer arrives later as TASK_RESULT. */
static int host_wordpad(void *p, const char *path)
{
    struct note_main *m = p; static const char name[] = "wordpad";
    size_t length = sizeof(name) - 1, n = strlen(path);
    if (!n || n > DESK_ARG_MAX) return -ENAMETOOLONG;
    for (size_t i = 0; i < n; i++) if ((unsigned char)path[i] < 32 || path[i] == 127) return -EINVAL;
    struct ciuki_message msg;
    desk_message(&msg, DESK_LAUNCH, (unsigned)(12 + length + 4 + n));
    if (!++m->launch_serial) ++m->launch_serial;
    desk_put32(msg.data + 4, m->launch_serial); desk_put32(msg.data + 8, (uint32_t)length);
    memcpy(msg.data + 12, name, length);
    desk_put32(msg.data + 12 + length, (uint32_t)n); memcpy(msg.data + 16 + length, path, n);
    return du_window_queue(&m->window, &msg) ? -errno : 0;
}
static void on_message(struct du_window *w, const struct desk_packet *pk, void *p)
{
    struct note_main *m = ((struct cn_app *)p)->host; (void)w;
    if (pk->opcode == DESK_TASK_RESULT && pk->request == DESK_LAUNCH && pk->serial == m->launch_serial && pk->result < 0)
        du_message(&m->ui, 998, "CiukNote", "WordPad could not be started.", DU_DLG_ICON_ERROR, NULL, 0, 0, 0);
}
static void host_declined(void *p) { du_window_close_reply(&((struct note_main *)p)->window, 0); }

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
    cp_ui_poll(&m->printing,&m->ui);
    if (du_job_ready(&m->opening)) {
        du_job_finish(&m->opening);
        int r=m->open_error;
        if ((r==0 || r==-ENOENT) && m->open_revision!=m->app.ta.revision) r=-EBUSY;
        if (!r) r=cn_open_bytes(&m->app,m->resolved,m->raw,m->length);
        else if (r==-ENOENT) cn_open_argument(&m->app,m->resolved);
        if (r==-EFBIG) cn_offer_wordpad(&m->app,m->resolved);
        else if (r && r!=-ENOENT) du_message(&m->ui,999,"CiukNote",strerror(-r),DU_DLG_ICON_ERROR,NULL,0,0,0);
        free(m->raw); m->raw=NULL; m->path=NULL;
    } else if (m->path && !m->opening.started && m->window.configured && !m->ui.dirty && m->ui.frame_count) {
        m->open_revision=m->app.ta.revision;
        int r=du_job_start_ui(&m->opening,load_argument,m,&m->ui);
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
    du_help_topic(&m->ui, "ciuknote");   /* G15: F1 opens this program's Help topic */
    struct cn_app *a = &m->app;
    m->window.close_request = close_request; m->window.user = a;
    a->fs = cn_native_fs(); a->now = host_now; a->quit = host_quit; a->declined = host_declined; a->wordpad = test ? NULL : host_wordpad; m->window.message = on_message; a->portrait = portrait;
    if (!test) a->recent = host_recent;   /* scripted runs keep the disk unchanged */
    a->host = m; a->test = test;
    if (test) a->checkpoint = on_checkpoint;
    if (cn_attach(a, &m->ui)) { fprintf(stderr, "CiukNote: out of memory\n"); du_window_destroy(&m->window); free(m); return 1; }
    a->printing=print_request;m->ui.event=print_event;
    m->path=path;
    du_window_accept_files(&m->window, 1);   /* fx-19: DragAcceptFiles */
    if (test) du_window_move(&m->window, 24, 48); /* fixed placement for the scripted QEMU case */
    int r = du_run(&m->window, idle, m);
    du_job_finish(&m->opening); free(m->raw);
    cp_ui_destroy(&m->printing);
    cn_detach(a); du_window_destroy(&m->window); free(m);
    return r < 0 ? 1 : 0;
}
