/* SPDX-License-Identifier: MIT
 * /bin/control: CiukiOS Control Panel. Usage: control [--channel-fd=N] [--panel=NAME]
 * NAME: display, mouse, keyboard, sounds, datetime, system. */
#include "control.h"
#include <ciuki/audio.h>
#include <ciuki/proc.h>
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

struct cp_main { struct cp_app app; struct du_ui ui; struct du_window window; uint32_t portrait[256 * 256]; uint32_t serial; uint64_t last_tick; };

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
    (void)ctx;
    if (mkdir(CFG_DIR, 0755) && errno != EEXIST) return -1;
    static const char temp[] = CFG_DIR "/desktop.cpl";
    FILE *f = fopen(temp, "wb");
    if (!f) return -1;
    int ok = fwrite(text, 1, length, f) == length;
    if (fflush(f) || fsync(fileno(f))) ok = 0;
    if (fclose(f)) ok = 0;
    if (ok && rename(temp, CFG_FILE)) ok = !unlink(CFG_FILE) && !rename(temp, CFG_FILE);
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
    struct ciuki_message msg;
    desk_message(&msg, DESK_SETTINGS, 24); msg.data[0] = DESK_SHELL_VERSION;
    desk_put32(msg.data + 4, ++m->serial); desk_put32(msg.data + 8, request);
    if (request == DESK_SETTINGS_MASTER) {
        desk_put32(msg.data + 12, (uint32_t)volume); desk_put32(msg.data + 16, (uint32_t)volume); desk_put32(msg.data + 20, (uint32_t)(muted != 0));
    }
    return du_window_queue(&m->window, &msg);
}
static void on_message(struct du_window *w, const struct desk_packet *p, void *user)
{
    (void)w;
    struct cp_main *m = user;
    if (p->opcode == DESK_TASK_RESULT && p->request == DESK_SETTINGS && p->result < 0) {
        struct du_widget *s = du_get(&m->ui, 3);   /* the status line */
        if (s) { du_set_text(s, p->result == -ENODEV ? "No writable mixer: the volume is read-only." : "The desktop refused the change."); m->ui.dirty = 1; }
    }
}
static uint64_t now(void *ctx) { (void)ctx; struct timespec t; return clock_gettime(CLOCK_REALTIME, &t) || t.tv_sec < 0 ? 0 : (uint64_t)t.tv_sec; }
static void idle(void *arg)
{
    struct cp_main *m = arg;
    if (m->app.quit) { du_window_close(&m->window); m->app.quit = 0; }
    uint64_t t = now(m);
    if (t != m->last_tick) { m->last_tick = t; cp_tick(&m->app); if (m->app.page == CP_DATETIME) m->ui.dirty = 1; }
}
int main(int argc, char **argv)
{
    int channel = 3, page = CP_DISPLAY;
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
    struct cp_host *h = &m->app.host;
    h->load = load; h->save = save; h->wallpapers = wallpapers; h->decode = decode; h->mixer = mixer; h->apply = apply; h->now = now; h->ctx = m;
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
        fprintf(stderr, "Control Panel: desktop connection failed: %s\n", strerror(errno)); free(m); return 1;
    }
    m->window.message = on_message; m->window.user = m;
    if (cp_attach(&m->app, &m->ui, page)) { du_window_destroy(&m->window); free(m); return 1; }
    int r = du_run(&m->window, idle, m);
    cp_detach(&m->app); du_window_destroy(&m->window); free(m);
    return r < 0 ? 1 : 0;
}
