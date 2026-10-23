/* SPDX-License-Identifier: MIT
 * Files entry point: desktop channel, native VFS, program launching. */
#include "app.h"
#include "../desktop/autoplay.h"
#include <ciuki/media.h>
#include <fcntl.h>
#include "../desktop/catalog.h"
#include <ciuki/spawn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECKPOINT_SERIAL 0xf2310000u

struct files_main { struct files_app app; struct du_ui ui; struct du_window window; int checkpoint, sent; uint32_t launch_serial; char title[DESK_TITLE_MAX + 1]; };

/* CiukNote is a window: only the desktop can give it a channel and the display grants, so Files
 * asks it (protocol v2 LAUNCH with the path as the single argument; the desktop honours this for
 * /Programs/ciuknote from a Files it launched). The answer, TASK_RESULT, arrives later: 0 here means
 * the request was queued. Everything else is spawned directly, as before. */
static int launch_window(struct files_main *m, const char *name, const char *path)
{
    size_t length = strlen(name), n = strlen(path);
    if (!n || n > DESK_ARG_MAX) return -ENAMETOOLONG;
    for (size_t i = 0; i < n; i++) if ((unsigned char)path[i] < 32 || path[i] == 127) return -EINVAL;   /* the wire refuses control characters */
    struct ciuki_message msg;
    desk_message(&msg, DESK_LAUNCH, (unsigned)(12 + length + 4 + n));
    if (!++m->launch_serial) ++m->launch_serial;
    desk_put32(msg.data + 4, m->launch_serial); desk_put32(msg.data + 8, (uint32_t)length);
    memcpy(msg.data + 12, name, length);
    desk_put32(msg.data + 12 + length, (uint32_t)n); memcpy(msg.data + 16 + length, path, n);
    return du_window_queue(&m->window, &msg) ? -errno : 0;
}
static void on_message(struct du_window *w, const struct desk_packet *p, void *user)
{
    struct files_app *a = user; struct files_main *m = a->user; (void)w;
    if (p->opcode == DESK_FILE_DROP) files_drop(a,p->x,p->y,(int)p->flags);
    if (p->opcode == DESK_DRAG_OVER) files_drag_over(a,p->serial,(int)p->flags,p->x,p->y,p->index);
    if (p->opcode == DESK_DRAG_DROP) files_drag_drop(a,p->serial,p->x,p->y,p->flags);
    if (p->opcode == DESK_DRAG_END) files_drag_end(a,p->flags);
    if (p->opcode == DESK_TASK_RESULT && p->request == DESK_LAUNCH && p->serial == m->launch_serial && p->result < 0)
        files_launch_failed(a, -p->result);
}

static void drop(struct files_app *a, int x, int y, int copy)
{
    struct files_main *m=a->user; struct ciuki_message msg;
    desk_message(&msg,DESK_FILE_DROP,16); desk_put32(msg.data+4,copy ? 1u : 0u);
    desk_put32(msg.data+8,(uint32_t)x); desk_put32(msg.data+12,(uint32_t)y);
    if (du_window_queue(&m->window,&msg)) files_launch_failed(a,errno);
}
static int drag_begin(struct files_app *a, int button, unsigned count)
{
    struct files_main *m=a->user; struct ciuki_message msg;
    desk_message(&msg,DESK_DRAG_BEGIN,12); desk_put32(msg.data+4,(uint32_t)button); desk_put32(msg.data+8,count);
    return du_window_queue(&m->window,&msg) ? -errno : 0;
}
static void drag_status(struct files_app *a, uint32_t serial, unsigned effect, unsigned allowed)
{
    struct files_main *m=a->user; struct ciuki_message msg;
    desk_message(&msg,DESK_DRAG_STATUS,16); desk_put32(msg.data+4,serial);
    desk_put32(msg.data+8,effect); desk_put32(msg.data+12,allowed);
    (void)du_window_queue(&m->window,&msg);
}
static int launch(struct files_app *a, const char *program, char *const argv[])
{
    /* File-taking desktop clients (catalog takes_file) and Control Panel need the desktop's channel and grants. */
    const char *name = !strncmp(program, "/Programs/", sizeof("/Programs/") - 1) ? program + sizeof("/Programs/") - 1 : NULL;
    const struct desk_catalog_entry *entry = name ? desk_catalog_find(name) : NULL;
    if ((!strcmp(program, "/Programs/ciuknote") || !strcmp(program, "/Programs/control") || (entry && entry->takes_file)) && argv[0] && argv[1] && !argv[2])
        return launch_window(a->user, program + sizeof("/Programs/") - 1, argv[1]);
    char *env[] = {"LC_ALL=C", "TZ=UTC0", "HOME=/Documents", "TMPDIR=/tmp", "PATH=/Programs", NULL};
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

static void close_declined(struct files_app *a)
{ du_window_close_reply(&((struct files_main *)a->user)->window,0); }
static int close_request(struct du_window *w, void *user) { (void)w; return files_close_request(user); }

static void idle(void *arg)
{
    struct files_main *m = ((struct files_app *)arg)->user;
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) { }
    char display[FM_PATH], title[DESK_TITLE_MAX + 1];
    if (!fm_display_path(display, m->app.model.path)) {
        snprintf(title, sizeof(title), "Files - %.*s", (int)sizeof(title) - 9, display);
        for (unsigned i = 0; title[i]; i++) if ((unsigned char)title[i] < 32 || (unsigned char)title[i] > 126) title[i] = '?';
        if (strcmp(title, m->title) && !du_window_set_title(&m->window, title)) strcpy(m->title, title);
    }
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

static int media_label(unsigned drive,char out[33])
{
    out[0]=0;
    struct ciuki_media_volume v={.version=CIUKI_MEDIA_VERSION,.size=sizeof(v),.drive=drive};
    int fd=open("/dev/media",O_RDONLY|O_CLOEXEC); if (fd<0) return -errno;
    int result=ciuki_devctl(fd,MEDIA_GET_VOLUME,&v,sizeof(v)); close(fd);
    if (result || !v.flags) return -1;
    char label[33];
    if (!ap_drive_label(fm_native_vfs(),drive,label) && label[0]) memcpy(out,label,33);
    else memcpy(out,v.label,33);
    return 0;
}
/* No root listing/content read on unchanged media polls. Native lookup is
 * case insensitive on the supported FAT/optical volumes, as is ap_drive_label. */
static int media_label_key(unsigned drive,struct files_label_key *key)
{
    *key=(struct files_label_key){0};
    struct ciuki_media_volume v={.version=CIUKI_MEDIA_VERSION,.size=sizeof(v),.drive=drive};
    int fd=open("/dev/media",O_RDONLY|O_CLOEXEC); if (fd<0) return -errno;
    int result=ciuki_devctl(fd,MEDIA_GET_VOLUME,&v,sizeof(v)); close(fd);
    if (result || !v.flags) return -1;
    memcpy(key->volume,v.label,33); key->volume[32]=0;
    const struct fm_vfs *fs=fm_native_vfs();
    char path[32]; snprintf(path,sizeof(path),"/mnt/%c/autorun.inf",'a'+drive);
    struct fm_meta meta={0};
    key->inf_status=fs->stat_path(fs->ctx,path,&meta);
    if (key->inf_status && key->inf_status!=-ENOENT) return key->inf_status;
    if (!key->inf_status) { key->size=meta.size; key->ino=meta.ino; key->modified=meta.modified; }
    return 0;
}
static int media_read(struct ciuki_media_state *s)
{
    *s=(struct ciuki_media_state){.version=CIUKI_MEDIA_VERSION,.size=sizeof(*s)};
    int fd=open("/dev/media",O_RDONLY|O_CLOEXEC); if (fd<0) return -errno;
    int e=ciuki_devctl(fd,MEDIA_GET_STATE,s,sizeof(*s)) ? -errno : 0; close(fd); return e;
}
static int media_remove(unsigned d,uint32_t generation)
{
    int fd=open("/dev/media",O_RDONLY|O_CLOEXEC); if (fd<0) return -errno;
    struct ciuki_media_remove r={CIUKI_MEDIA_VERSION,sizeof(r),d,generation};
    int e=ciuki_devctl(fd,MEDIA_SAFE_REMOVE,&r,sizeof(r)) ? -errno : 0; close(fd); return e;
}
static int media_load(unsigned d)
{
    int fd=open("/dev/media",O_RDONLY|O_CLOEXEC); if (fd<0) return -errno;
    struct ciuki_media_remove r={CIUKI_MEDIA_VERSION,sizeof(r),d,0};
    int e=ciuki_devctl(fd,MEDIA_FLOPPY_LOAD,&r,sizeof(r)) ? -errno : 0; close(fd); return e;
}
static int media_space(unsigned d,struct ciuki_media_space *sp)
{
    *sp=(struct ciuki_media_space){.version=CIUKI_MEDIA_VERSION,.size=sizeof(*sp),.drive=d};
    int fd=open("/dev/media",O_RDONLY|O_CLOEXEC); if (fd<0) return -errno;
    int e=ciuki_devctl(fd,MEDIA_GET_SPACE,sp,sizeof(*sp)) ? -errno : 0; close(fd); return e;
}
static int media_blockers(unsigned drive,uint32_t generation,struct ciuki_media_blockers *b)
{
    *b=(struct ciuki_media_blockers){.version=CIUKI_MEDIA_VERSION,.size=sizeof(*b),.drive=drive,.generation=generation};
    int fd=open("/dev/media",O_RDONLY|O_CLOEXEC); if (fd<0) return -errno;
    int e=ciuki_devctl(fd,MEDIA_GET_BLOCKERS,b,sizeof(*b)) ? -errno : 0; close(fd); return e;
}
int main(int argc, char **argv)
{
    int channel = 3, test = 0;
    const char *path = FM_COMPUTER;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--channel-fd=", 13)) {
            char *end; long n = strtol(argv[i] + 13, &end, 10);
            if (!argv[i][13] || *end || n < 3 || n >= 128) return 2;
            channel = (int)n;
        } else if (!strcmp(argv[i], "--test=files")) test = 1;
        else if (argv[i][0] != '-') path = argv[i];
        else return 2;
    }
    struct files_main *m = calloc(1, sizeof(*m));
    if (!m) return 1;
    int width, height;
    window_size(&width, &height);
    if (test) { width = 760; height = 520; }
    char display[FM_PATH], title[DESK_TITLE_MAX + 1];
    if (fm_display_path(display, path)) { free(m); return 2; }
    snprintf(title, sizeof(title), "Files - %.*s", (int)sizeof(title) - 9, display);
    for (unsigned i = 0; title[i]; i++) if ((unsigned char)title[i] < 32 || (unsigned char)title[i] > 126) title[i] = '?';
    strcpy(m->title, title);
    if (du_window_open_resizable(&m->window, &m->ui, channel, width, height, title, 600, 360)) {
        fprintf(stderr, "Files: desktop connection failed: %s\n", strerror(errno));
        free(m);
        return 1;
    }
    struct files_app *a = &m->app;
    a->user = m; a->test = test; a->spawn = launch; a->drop=drop; a->drag_begin=drag_begin; a->drag_status=drag_status; a->close = close_window; a->close_declined=close_declined; a->portrait = portrait;
    a->poll_clipboard=1; a->media_label_key=media_label_key; a->media_label=media_label; a->media_read=media_read; a->media_remove=media_remove; a->media_blockers=media_blockers;
    a->media_load=media_load; a->media_space=media_space;
    if (test) a->checkpoint = checkpoint;
    m->window.close_request = close_request; m->window.message = on_message; m->window.user = a;
    if (files_setup(a, &m->ui, fm_native_vfs(), path)) { du_window_destroy(&m->window); free(m); return 1; }
    /* Fixed placement keeps the scripted desktop case deterministic. */
    if (test) du_window_move(&m->window, 4, 36);
    int r = du_run(&m->window, idle, a);
    files_destroy(a);
    du_window_destroy(&m->window);
    free(m);
    return r < 0 ? 1 : 0;
}
