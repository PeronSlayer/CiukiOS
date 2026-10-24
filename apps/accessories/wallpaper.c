/* SPDX-License-Identifier: MIT
 * Set As Wallpaper: settings file update and desktop reload request. */
#include "wallpaper.h"
#include "../desktop/protocol.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int acc_wallpaper_supported(const char *path)
{
    const char *dot = path ? strrchr(path, '.') : NULL;
    return dot && (!strcasecmp(dot, ".bmp") || !strcasecmp(dot, ".png") || !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".jpe"));
}
int acc_wallpaper_write(const char *cfg_path, const char *image_path, int mode)
{
    if (!image_path || image_path[0] != '/' || strlen(image_path) >= CFG_PATH_MAX) return -ENAMETOOLONG;
    if (mode != CFG_WALL_CENTER && mode != CFG_WALL_TILE && mode != CFG_WALL_STRETCH) return -EINVAL;
    if (!acc_wallpaper_supported(image_path)) return -EINVAL;
    struct ciuki_settings *s = malloc(sizeof(*s));
    char *text = malloc(CFG_TEXT_MAX + 1), *out = malloc(CFG_TEXT_MAX), *temp = malloc(strlen(cfg_path) + 8);
    int r = 0;
    if (!s || !text || !out || !temp) { r = -ENOMEM; goto done; }
    cfg_defaults(s);
    FILE *f = fopen(cfg_path, "rb");
    if (f) {
        size_t n = fread(text, 1, CFG_TEXT_MAX, f);
        int bad = ferror(f); fclose(f);
        if (bad) { r = -EIO; goto done; }
        cfg_parse(s, text, n);
    }
    snprintf(s->wallpaper, sizeof(s->wallpaper), "%s", image_path);
    s->wallpaper_mode = mode;
    size_t length = cfg_format(s, out, CFG_TEXT_MAX);
    if (!length) { r = -EFBIG; goto done; }
    snprintf(temp, strlen(cfg_path) + 8, "%s.tmp", cfg_path);
    f = fopen(temp, "wb");
    if (!f) { r = -errno; goto done; }
    if (fwrite(out, 1, length, f) != length) r = -EIO;
    if (fclose(f)) r = -EIO;
    if (!r && rename(temp, cfg_path)) r = -errno;
    if (r) remove(temp);
done:
    free(s); free(text); free(out); free(temp);
    return r;
}
int acc_wallpaper_apply(struct du_window *w, const char *image_path, int mode)
{
    int r = acc_wallpaper_write(CFG_FILE, image_path, mode);
    if (r) return r;
    static uint32_t serial;
    struct ciuki_message m;
    desk_message(&m, DESK_SETTINGS, 24);
    m.data[0] = DESK_SHELL_VERSION;
    desk_put32(m.data + 4, ++serial); desk_put32(m.data + 8, DESK_SETTINGS_RELOAD);
    return du_window_queue(w, &m) ? -errno : 0;
}
