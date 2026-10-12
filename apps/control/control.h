/* SPDX-License-Identifier: MIT
 * /Programs/control: the CiukiOS Control Panel on libdeskui. The panel edits
 * /system/settings/desktop.cfg (apps/desktop/settings.h) and asks the
 * desktop to apply it at once (DESK_SETTINGS); the master volume goes
 * through the desktop, which holds the writable mixer handle. */
#ifndef CIUKI_CONTROL_H
#define CIUKI_CONTROL_H
#include "../libdeskui/deskui.h"
#include "../desktop/settings.h"
#include "../desktop/sounds.h"
#include "../desktop/wallpaper.h"
#include <stdint.h>
#define CP_WIDTH 500
#define CP_HEIGHT 420
#define CP_WALLPAPERS 40
enum { CP_DISPLAY, CP_MOUSE, CP_KEYBOARD, CP_SOUNDS, CP_DATETIME, CP_SYSTEM, CP_PAGES };
struct cp_host {
    int (*load)(void *, char *text, size_t size, size_t *length);      /* desktop.cfg; -1 when absent */
    int (*save)(void *, const char *text, size_t length);
    int (*wallpapers)(void *, char names[][CFG_PATH_MAX], unsigned capacity);   /* absolute paths */
    int (*decode)(void *, const char *path, struct desk_image *out);
    int (*mixer)(void *, int *volume, int *muted);                    /* read-only master; -1 absent */
    int (*apply)(void *, unsigned request, int volume, int muted);      /* DESK_SETTINGS */
    uint64_t (*now)(void *);                                          /* UTC seconds */
    int (*load_sounds)(void *, char *, size_t, size_t *);
    int (*save_sounds)(void *, const char *, size_t);
    void *ctx;
    const uint32_t *portrait;                                         /* 256x256 or NULL */
    uint32_t screen_w, screen_h;
    char version[65], build[65], machine[65];
    uint64_t mem_total_k, mem_free_k; unsigned processes;
};
struct cp_app {
    struct du_ui *ui;
    struct cp_host host;
    struct ciuki_settings cfg;
    int page, volume, muted, mixer_ok, audio_testing;
    char sound_text[SOUND_TEXT_MAX+1];
    struct sound_scheme sounds; unsigned sound_selected;
    struct du_item sound_items[SOUND_EVENTS];
    char wall_names[CP_WALLPAPERS][CFG_PATH_MAX]; unsigned wall_count;
    struct du_item theme_items[CFG_THEMES], wall_items[CP_WALLPAPERS + 8], key_items[CFG_KEYS_COUNT], tab_items[CP_PAGES];
    char wall_labels[CP_WALLPAPERS + 8][48];
    uint32_t preview[192 * 144];
    int preview_valid;
    struct desk_image image;
    int preview_pending;            /* an image wallpaper decode is scheduled after the first paint */
    char image_path[128];           /* path of the wallpaper decoded into image (preview_build cache) */
    char status[96], clock_line[64], date_line[64];
    int quit;
};
int cp_attach(struct cp_app *, struct du_ui *, int page);
void cp_detach(struct cp_app *);
int cp_page_index(const char *name);
void cp_tick(struct cp_app *);
void cp_audio_result(struct cp_app *, int result);
#endif
