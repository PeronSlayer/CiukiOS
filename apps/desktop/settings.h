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
#define CFG_DESKTOP_COLOR 0x375564u
#define CFG_TEXT_MAX 32768
#define CFG_PATH_MAX 128
#define CFG_RECENT_MAX 8
#define CFG_EXTRA_MAX 16384
/* Wallpaper placement (wallpaper_mode). The values are the order of
 * cfg_mode_names; new modes are appended so existing files keep parsing.
 * Fill: uniform scale that covers the area, centred crop of the overflow. */
enum { CFG_WALL_CENTER, CFG_WALL_STRETCH, CFG_WALL_TILE, CFG_WALL_FIT, CFG_WALL_FILL, CFG_WALL_MODES };
/* Keyboard layouts (keyboard). */
enum { CFG_KEYS_US, CFG_KEYS_IT, CFG_KEYS_COUNT };
#define CFG_THEMES 2
#define CFG_ICON_SPACING_MIN 60
#define CFG_ICON_SPACING_MAX 256
#define CFG_MOUSE_MIN 1
#define CFG_MOUSE_MAX 10
#define CFG_MOUSE_NORMAL 5
struct ciuki_settings {
    char theme[32];                 /* "Platinum", "Ciuki Blue" */
    char wallpaper[CFG_PATH_MAX];   /* "ciuki", "none", "builtin:<name>" or an absolute file path */
    int wallpaper_pattern;          /* DESK_WALL_PATTERNS index; 0 = none */
    int wallpaper_mode;             /* CFG_WALL_* */
    uint32_t desktop_color;         /* 0xRRGGBB behind and around the wallpaper */
    int clock_24h, clock_seconds, desktop_icons;
    int icon_auto_arrange, icon_sort; /* G19: persisted desktop view state */
    int icon_spacing_x, icon_spacing_y; /* icon-cell width and height, pixels */
    int icon_font;                  /* DESK_FONT_* caption font */
    int mouse_speed;                /* 1..10, 5 = one screen pixel per mouse count */
    int double_click_ms;            /* 200..900 */
    uint16_t display_mode;         /* confirmed VBE mode id; 0 keeps loader default */
    int saver_enabled, saver_seconds, saver_kind, monitor_seconds; /* G10: 0..INT32_MAX seconds; kinds 0..2 */
    int keyboard;                   /* CFG_KEYS_* */
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
int cfg_icon_position(const struct ciuki_settings *, const char *, int *, int *);
int cfg_icon_move(struct ciuki_settings *, const char *, int, int);
void cfg_icon_forget(struct ciuki_settings *, const char *);
/* Atomic rename of all saved resolution profiles, or 0 at capacity. */
int cfg_icon_rename(struct ciuki_settings *, const char *old_name, const char *new_name);
void cfg_icons_arrange(struct ciuki_settings *);
/* Resolution profiles preserve each layout independently; old unqualified
 * positions are a fallback until that resolution is explicitly arranged. */
int cfg_icon_position_at(const struct ciuki_settings *, const char *, unsigned width, unsigned height, int *, int *);
int cfg_icon_move_at(struct ciuki_settings *, const char *, unsigned width, unsigned height, int, int);
int cfg_icons_arrange_at(struct ciuki_settings *, unsigned width, unsigned height);
int cfg_icon_label(const struct ciuki_settings *, const char *, char *, size_t);
int cfg_icon_label_set(struct ciuki_settings *, const char *, const char *);
void cfg_icon_label_forget(struct ciuki_settings *, const char *);
int cfg_theme_index(const char *name);   /* -1 when unknown */
/* Mouse speed as a rational multiplier num/8 applied to relative motion. */
int cfg_mouse_numerator(int speed);
#endif
