/* SPDX-License-Identifier: MIT
 * Desktop settings: /system/settings/desktop.cfg, owned by the desktop shell
 * and edited by the Control Panel. Plain "key=value" lines; '#' starts a
 * comment line; unknown keys are kept verbatim so that other components
 * (for example the libdeskui theme engine) can add their own. Pure functions
 * over memory: the callers do the file I/O. See docs/desktop.md. */
#ifndef CIUKI_DESKTOP_SETTINGS_H
#define CIUKI_DESKTOP_SETTINGS_H
#include <stddef.h>
#include <stdint.h>
#define CFG_DIR "/system/settings"
#define CFG_FILE "/system/settings/desktop.cfg"
#define CFG_WALLPAPER_DIR "/system/wallpapers"
#define CFG_VERSION 1
#define CFG_TEXT_MAX 4096
#define CFG_PATH_MAX 128
#define CFG_RECENT_MAX 8
#define CFG_EXTRA_MAX 1024
/* Wallpaper placement (wallpaper_mode). The values are the order of
 * cfg_mode_names; new modes are appended so existing files keep parsing.
 * Fill: uniform scale that covers the area, centred crop of the overflow. */
enum { CFG_WALL_CENTER, CFG_WALL_STRETCH, CFG_WALL_TILE, CFG_WALL_FIT, CFG_WALL_FILL, CFG_WALL_MODES };
/* Keyboard layouts (keyboard). */
enum { CFG_KEYS_US, CFG_KEYS_IT, CFG_KEYS_COUNT };
#define CFG_THEMES 3
#define CFG_MOUSE_MIN 1
#define CFG_MOUSE_MAX 10
#define CFG_MOUSE_NORMAL 5
struct ciuki_settings {
    char theme[32];                 /* "Platinum", "Ciuki Blue", "Classic 98" */
    char wallpaper[CFG_PATH_MAX];   /* "ciuki", "none", "builtin:<name>" or an absolute file path */
    int wallpaper_mode;             /* CFG_WALL_* */
    uint32_t desktop_color;         /* 0xRRGGBB behind and around the wallpaper */
    int clock_24h, clock_seconds, desktop_icons;
    int mouse_speed;                /* 1..10, 5 = one screen pixel per mouse count */
    int double_click_ms;            /* 200..900 */
    int keyboard;                   /* CFG_KEYS_* */
    uint16_t display_mode;           /* confirmed VBE mode id; 0 keeps the boot mode */
    char recent[CFG_RECENT_MAX][CFG_PATH_MAX];
    unsigned recent_count;          /* newest first */
    char extra[CFG_EXTRA_MAX];      /* unknown "key=value" lines, newline separated */
};
extern const char *const cfg_theme_names[CFG_THEMES];
extern const char *const cfg_mode_names[CFG_WALL_MODES];
extern const char *const cfg_keyboard_names[CFG_KEYS_COUNT];     /* "us", "it" */
extern const char *const cfg_keyboard_labels[CFG_KEYS_COUNT];    /* "English (US)", "Italian" */
void cfg_defaults(struct ciuki_settings *);
/* Parses text over the defaults already in *s; returns the number of lines
 * that were rejected (bad value or overlong); those keep their defaults. */
int cfg_parse(struct ciuki_settings *, const char *text, size_t length);
/* Writes the complete file; returns its length, or 0 if it does not fit. */
size_t cfg_format(const struct ciuki_settings *, char *out, size_t size);
void cfg_recent_add(struct ciuki_settings *, const char *path);
int cfg_theme_index(const char *name);   /* -1 when unknown */
/* Mouse speed as a rational multiplier num/8 applied to relative motion. */
int cfg_mouse_numerator(int speed);
#endif
