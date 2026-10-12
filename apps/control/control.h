/* SPDX-License-Identifier: MIT
 * /Programs/control: the CiukiOS Control Panel on libdeskui. The panel edits
 * /system/settings/desktop.cfg (apps/desktop/settings.h) and asks the
 * desktop to apply it at once (DESK_SETTINGS); the master volume goes
 * through the desktop, which holds the writable mixer handle. */
#ifndef CIUKI_CONTROL_H
#define CIUKI_CONTROL_H
#include "../libdeskui/deskui.h"
#include "../desktop/settings.h"
#include "../desktop/wallpaper.h"
#include <stdint.h>
#define CP_WIDTH 500
#define CP_HEIGHT 420
#define CP_WALLPAPERS 40
enum { CP_HOME = -1, CP_DISPLAY, CP_MOUSE, CP_KEYBOARD, CP_SOUNDS, CP_DATETIME, CP_SYSTEM,
       CP_DEVICES, CP_DRIVERS, CP_NETWORK, CP_POWER, CP_PAGES };
#define CP_TREE_MAX (1 + CIUKI_DEVICE_CLASSES + CIUKI_DEVICES_MAX)
#define CP_BOOT_MAX 128
struct cp_host {
    int (*load)(void *, char *text, size_t size, size_t *length);      /* desktop.cfg; -1 when absent */
    int (*save)(void *, const char *text, size_t length);
    int (*wallpapers)(void *, char names[][CFG_PATH_MAX], unsigned capacity);   /* absolute paths */
    int (*decode)(void *, const char *path, struct desk_image *out);
    int (*mixer)(void *, int *volume, int *muted);                    /* read-only master; -1 absent */
    int (*apply)(void *, unsigned request, int volume, int muted);      /* DESK_SETTINGS */
    uint64_t (*monotonic)(void *);                                    /* nanoseconds */
    uint64_t (*now)(void *);                                          /* UTC seconds */
    int (*devices)(void *, struct ciuki_devices_info *);
    int (*boot_load)(void *, char *, size_t, size_t *);
    int (*boot_save)(void *, const char *, size_t);
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
    int page, volume, muted, mixer_ok;
    char wall_names[CP_WALLPAPERS][CFG_PATH_MAX]; unsigned wall_count;
    struct du_item theme_items[CFG_THEMES], wall_items[CP_WALLPAPERS + 8], key_items[CFG_KEYS_COUNT], tab_items[CP_PAGES];
    char wall_labels[CP_WALLPAPERS + 8][48];
    uint32_t preview[192 * 144];
    int preview_valid;
    struct desk_image image;
    int preview_pending;            /* an image wallpaper decode is scheduled after the first paint */
    char image_path[128];           /* path of the wallpaper decoded into image (preview_build cache) */
    char status[96], clock_line[64], date_line[64];
    struct ciuki_devices_info devices;
    int devices_ok, system_tab, properties_device, detail_list;
    unsigned detail_index;
    uint64_t snapshot_time;
    unsigned collapsed;
    uint32_t selected_device_id;
    struct du_item applet_items[CP_PAGES], system_tabs[3];
    struct du_item tree_items[CP_TREE_MAX], driver_items[CIUKI_DRIVERS_MAX], lease_items[CIUKI_LEASES_MAX];
    int tree_device[CP_TREE_MAX]; unsigned tree_count;
    char device_names[CIUKI_DEVICES_MAX][96];
    char driver_bound[CIUKI_DRIVERS_MAX][16], lease_range[CIUKI_LEASES_MAX][48], lease_gen[CIUKI_LEASES_MAX][16];
    char boot_text[CP_BOOT_MAX]; size_t boot_length;
    int boot_ok, serial_next;
    int quit;
};
int cp_attach(struct cp_app *, struct du_ui *, int page);
void cp_detach(struct cp_app *);
int cp_page_index(const char *name);
void cp_tick(struct cp_app *);
void cp_open(struct cp_app *, int page);
int cp_snapshot_valid(const struct ciuki_devices_info *);
void cp_tree_build(struct cp_app *);
const char *cp_device_class(unsigned);
const char *cp_device_state(unsigned);
/* Preserve every other BOOT.CFG token, including test selectors. */
int cp_boot_serial(const char *text, size_t length, int serial, char *out, size_t capacity);
#endif
