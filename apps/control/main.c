/* SPDX-License-Identifier: MIT
 * /Programs/control: CiukiOS Control Panel. Usage: control [--channel-fd=N] [--panel=NAME]
 * NAME: home, display, mouse, keyboard, sounds, datetime, system, devices, drivers, network, power, adapter. */
#include "control.h"
#include "../desktop/settings_io.h"
#include <ciuki/audio.h>
#include <ciuki/proc.h>
#include <ciuki/devices.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

struct cp_main { struct cp_app app; struct du_ui ui; struct du_window window; struct cfg_writer writer;
    int writer_started, reload_pending; uint32_t portrait[256 * 256]; uint32_t serial, display_serial, audio_serial; uint64_t last_tick; };

static int load(void *ctx, char *text, size_t size, size_t *length)
{
    (void)ctx;
    FILE *f = fopen(CFG_FILE, "rb");
    if (!f) return -1;
    *length = fread(text, 1, size, f);
    int ok = !ferror(f) && *length < size;
    fclose(f);
    return ok ? 0 : -1;
}
static int save(void *ctx, const char *text, size_t length)
{
    struct cp_main *m=ctx;
    if (!m->writer_started) { errno=EIO; return -1; }
    return cfg_writer_submit(&m->writer,text,length);
}
/* BOOT.CFG is a pre-existing bounded boot file. In-place replacement fits
 * its existing cluster and works with the native VFS's rename semantics. */
static int boot_load(void *ctx, char *text, size_t size, size_t *length)
{
    (void)ctx;
    int fd = open("/system/BOOT.CFG", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t n = 0; int ok = 1;
    while (n < size) {
        ssize_t got = read(fd, text + n, size - n);
        if (got < 0) { ok = 0; break; }
        if (!got) break;
        n += (size_t)got;
    }
    if (close(fd)) ok = 0;
    if (n >= size) ok = 0;
    if (ok) { text[n] = 0; *length = n; }
    return ok ? 0 : -1;
}
static int boot_write(int fd, const char *text, size_t length)
{
    size_t at = 0;
    while (at < length) {
        ssize_t n = pwrite(fd, text + at, length - at, (off_t)at);
        if (n <= 0) return -1;
        at += (size_t)n;
    }
    return ftruncate(fd, (off_t)length) || fsync(fd) ? -1 : 0;
}
static int boot_save(void *ctx, const char *text, size_t length)
{
    char previous[CP_BOOT_MAX]; size_t n = 0;
    if (length >= CP_BOOT_MAX || boot_load(ctx, previous, sizeof(previous), &n)) return -1;
    int fd = open("/system/BOOT.CFG", O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    int r = boot_write(fd, text, length);
    if (r) (void)boot_write(fd, previous, n); /* best effort after an I/O error */
    if (close(fd)) r = -1;
    return r;
}
static int devices(void *ctx, struct ciuki_devices_info *out) { (void)ctx; return ciuki_devices(out); }
static int load_sounds(void *ctx, char *text, size_t size, size_t *length)
{
    (void)ctx; FILE *f = fopen(SOUND_CFG,"rb"); if (!f) return -1;
    *length = fread(text,1,size,f); int ok = !ferror(f) && *length < size; fclose(f); return ok ? 0 : -1;
}
static int save_sounds(void *ctx, const char *text, size_t length)
{
    (void)ctx; if (mkdir(CFG_DIR,0755) && errno != EEXIST) return -1;
    const char *temp = CFG_DIR "/sounds.cpl"; FILE *f = fopen(temp,"wb"); if (!f) return -1;
    int ok = fwrite(text,1,length,f) == length;
    if (fflush(f) || fsync(fileno(f))) ok = 0;
    if (fclose(f)) ok = 0;
    if (ok && rename(temp,SOUND_CFG)) ok = !unlink(SOUND_CFG) && !rename(temp,SOUND_CFG);
    if (!ok) unlink(temp);
    return ok ? 0 : -1;
}
static int image_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || name[0] == '.') return 0;
    char ext[8]; size_t n = 0;
    for (dot++; *dot && n < sizeof(ext) - 1; dot++) ext[n++] = (char)(*dot >= 'A' && *dot <= 'Z' ? *dot + 32 : *dot);
    ext[n] = 0;
    return !strcmp(ext, "bmp") || !strcmp(ext, "png") || !strcmp(ext, "jpg") || !strcmp(ext, "jpeg");
}
static int wallpapers(void *ctx, char names[][CFG_PATH_MAX], unsigned capacity)
{
    (void)ctx;
    DIR *dir = opendir(CFG_WALLPAPER_DIR);
    if (!dir) return 0;
    unsigned n = 0;
    struct dirent *e;
    while ((e = readdir(dir)) && n < capacity) {
        if (!image_name(e->d_name) || strlen(e->d_name) + sizeof(CFG_WALLPAPER_DIR) + 1 >= CFG_PATH_MAX) continue;
        snprintf(names[n], CFG_PATH_MAX, "%s/%s", CFG_WALLPAPER_DIR, e->d_name);
        /* Sorted insertion: the directory order is not meaningful. */
        for (unsigned k = n; k > 0 && strcmp(names[k - 1], names[k]) > 0; k--) {
            char t[CFG_PATH_MAX]; memcpy(t, names[k], CFG_PATH_MAX); memcpy(names[k], names[k - 1], CFG_PATH_MAX); memcpy(names[k - 1], t, CFG_PATH_MAX);
        }
        n++;
    }
    closedir(dir);
    return (int)n;
}
static int decode(void *ctx, const char *path, struct desk_image *out)
{
    (void)ctx;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t cap = 1 << 16, n = 0; uint8_t *data = malloc(cap);
    while (data) {
        if (n == cap) { if (cap >= DESK_IMAGE_FILE_MAX) { free(data); data = NULL; break; } uint8_t *g = realloc(data, cap * 2); if (!g) { free(data); data = NULL; break; } data = g; cap *= 2; }
        size_t got = fread(data + n, 1, cap - n, f);
        if (!got) break;
        n += got;
    }
    fclose(f);
    int r = data ? desk_image_decode(data, n, out) : -1;
    free(data);
    return r;
}
static int mixer(void *ctx, int *volume, int *muted)
{
    (void)ctx;
    int fd = open("/dev/mixer", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct ciuki_mixer_master m = {.version = CIUKI_AUDIO_VERSION, .size = sizeof(m)};
    int r = ciuki_devctl(fd, MIXER_GET_MASTER, &m, sizeof(m));
    close(fd);
    if (r < 0) return -1;
    *volume = (int)((m.left + m.right) / 2); *muted = m.mute != 0;
    return 0;
}
static int apply(void *ctx, unsigned request, int volume, int muted)
{
    struct cp_main *m = ctx;
    if (request==DESK_SETTINGS_RELOAD && m->writer_started && cfg_writer_busy(&m->writer)) {
        m->reload_pending=1; return 0;
    }
    struct ciuki_message msg;
    if (!++m->serial) ++m->serial;
    cp_settings_message(&msg, m->serial, request, volume, muted);
    int r = du_window_queue(&m->window, &msg);
    if (!r && request == DESK_SETTINGS_DISPLAY) m->display_serial = m->serial;
    if (!r && request == DESK_SETTINGS_AUDIO_TEST) m->audio_serial = m->serial;
    return r;
}
static int display_query(void *ctx, struct ciuki_display_info *info)
{ (void)ctx; return ciuki_display_query(info); }
static void on_message(struct du_window *w, const struct desk_packet *p, void *user)
{
    (void)w;
    struct cp_main *m = user;
    if (p->opcode == DESK_TASK_RESULT && p->request == DESK_SETTINGS && m->audio_serial && p->serial == m->audio_serial) {
        m->audio_serial = 0; cp_audio_result(&m->app, p->result); return;
    }
    if (p->opcode == DESK_TASK_RESULT && p->request == DESK_SETTINGS && m->display_serial && p->serial == m->display_serial) {
        m->display_serial = 0; cp_display_result(&m->app, p->result); return;
    }
    if (p->opcode == DESK_TASK_RESULT && p->request == DESK_SETTINGS && p->result < 0) {
        struct du_widget *s = du_get(&m->ui, 3);   /* the status line */
        if (s) { du_set_text(s, p->result == -ENODEV ? "No writable mixer: the volume is read-only." : "The desktop refused the change."); m->ui.dirty = 1; }
    }
}
static uint64_t monotonic(void *ctx) { (void)ctx; return du_now(); }
static uint64_t now(void *ctx) { (void)ctx; struct timespec t; return clock_gettime(CLOCK_REALTIME, &t) || t.tv_sec < 0 ? 0 : (uint64_t)t.tv_sec; }
static int close_request(struct du_window *w, void *user)
{ (void)w; struct cp_main *m = user;
  if (m->writer_started && cfg_writer_busy(&m->writer)) {
      du_set_text(du_get(&m->ui,3),"Waiting for settings to finish saving..."); m->ui.dirty=1; return 0;
  }
  cp_display_cancel(&m->app); return 1; }
static void idle(void *arg)
{
    struct cp_main *m = arg;
    int error;
    if (m->writer_started && cfg_writer_result(&m->writer,&error)) {
        if (m->reload_pending) { m->reload_pending=0; if (!error) apply(m,DESK_SETTINGS_RELOAD,0,0); }
        cp_settings_saved(&m->app,error);
    }
    if (m->writer_started && cfg_writer_busy(&m->writer) && m->app.quit) return;
    if (m->app.quit) { du_window_close(&m->window); m->app.quit = 0; }
    uint64_t t = now(m);
    if (t != m->last_tick) { m->last_tick = t; cp_tick(&m->app); if (m->app.page == CP_DATETIME) m->ui.dirty = 1; }
}
int main(int argc, char **argv)
{
    int channel = 3, page = CP_HOME;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--channel-fd=", 13)) {
            char *end; long n = strtol(argv[i] + 13, &end, 10);
            if (!argv[i][13] || *end || n < 3 || n >= 128) return 2;
            channel = (int)n;
        } else if (!strncmp(argv[i], "--panel=", 8)) page = cp_page_index(argv[i] + 8);
        else return 2;
    }
    struct cp_main *m = calloc(1, sizeof(*m));
    if (!m) return 1;
    m->writer_started=!cfg_writer_start(&m->writer,CFG_FILE,CFG_DIR "/desktop.cpl");
    struct cp_host *h = &m->app.host;
    h->load_sounds = load_sounds; h->save_sounds = save_sounds; h->load = load; h->save = save; h->wallpapers = wallpapers; h->decode = decode; h->mixer = mixer; h->apply = apply; h->now = now; h->ctx = m;
    h->display = display_query;
    h->save_async=1;
    h->devices = devices; h->monotonic = monotonic; h->boot_load = boot_load; h->boot_save = boot_save;
    FILE *f = fopen("/system/assets/ciuki-portrait.xrgb", "rb");
    if (f) { if (fread(m->portrait, 4, 256 * 256, f) == 256 * 256) h->portrait = m->portrait; fclose(f); }
    const char *display = getenv("CIUKI_DISPLAY");
    h->screen_w = 1024; h->screen_h = 768;
    if (display) { char *end; long w = strtol(display, &end, 10); if (*end == 'x' && w >= 320 && w <= 2048) { h->screen_w = (uint32_t)w; h->screen_h = (uint32_t)strtol(end + 1, NULL, 10); } }
    struct utsname u;
    if (!uname(&u)) { snprintf(h->version, sizeof(h->version), "%s", u.version); snprintf(h->build, sizeof(h->build), "%s", u.release); snprintf(h->machine, sizeof(h->machine), "%s", u.machine); }
    static uint8_t buffer[CIUKI_PROC_LIST_MIN + CIUKI_PROCESS_MAX * sizeof(struct ciuki_proc_record)];
    if (ciuki_proc_list(buffer, sizeof(buffer)) > 0) {
        struct ciuki_proc_system sys; memcpy(&sys, buffer, sizeof(sys));
        if (sys.version == CIUKI_PROC_VERSION) { h->mem_total_k = (uint64_t)sys.usable_pages * 4u; h->mem_free_k = (uint64_t)sys.free_pages * 4u; h->processes = sys.processes; }
    }
    if (du_window_open_resizable(&m->window, &m->ui, channel, CP_WIDTH, CP_HEIGHT, "Control Panel", CP_WIDTH, CP_HEIGHT)) {
        fprintf(stderr, "Control Panel: desktop connection failed: %s\n", strerror(errno));
        if (m->writer_started) cfg_writer_stop(&m->writer);
        free(m); return 1;
    }
    m->window.message = on_message; m->window.close_request = close_request; m->window.user = m;
    if (cp_attach(&m->app, &m->ui, page)) {
        du_window_destroy(&m->window); if (m->writer_started) cfg_writer_stop(&m->writer); free(m); return 1;
    }
    int r = du_run(&m->window, idle, m);
    if (m->writer_started) cfg_writer_stop(&m->writer);
    cp_detach(&m->app); du_window_destroy(&m->window); free(m);
    return r < 0 ? 1 : 0;
}
