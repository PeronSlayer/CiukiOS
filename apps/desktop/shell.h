/* SPDX-License-Identifier: MIT
 * Desktop shell (the "Ciuki" Start menu, taskbar tray, desktop icons,
 * built-in windows, keyboard shortcuts). State lives in struct desktop
 * (desk_init zeroes it); shell.c never touches files or the kernel: it
 * raises requests that desktop.c serves (settings, wallpaper, power,
 * mixer, launches). Colours, faces and bevels come from the current theme
 * (theme.h) through the libdeskui toolkit; icons from the Tango store. */
#ifndef CIUKI_DESKTOP_SHELL_H
#define CIUKI_DESKTOP_SHELL_H
#include "settings.h"
#include "settings_catalog.h"
#include "folder.h"
#include "autoplay.h"
#include "wallpaper.h"
#include <stdint.h>
#include <stddef.h>
#include <ciuki/abi.h>
#define SH_LEVELS 3
#define SH_ITEMS 20
#define SH_ICONS (FM_ENTRIES + 4)
#define SH_TRAY_ICON_SIZE 16
#define SH_TRAY_GAP 4
#define SH_TRAY_MARGIN 6
/* Tray identifiers; geometry places Notifications next to the clock. */
enum sh_tray_icon { SH_TRAY_VOLUME, SH_TRAY_NETWORK, SH_TRAY_NOTIFICATION, SH_TRAY_MEDIA };
enum sh_notify_action { SH_NOTIFY_DETAILS, SH_NOTIFY_MEDIA, SH_NOTIFY_NETWORK, SH_NOTIFY_VOLUME };
struct sh_notification {
    char title[64], text[320];
    uint64_t started, expires;
    int active, icon, action, balloon, slide;
};
#define SH_START_ROW 30
#define SH_SUB_ROW 22
#define SH_BANNER 24
#define SH_SEPARATOR 8
#define SH_START_MARK 18        /* the portrait mark in the Ciuki button */
/* Built-in (internal) windows: desk_client.internal holds the kind. */
enum { DESK_INTERNAL_NONE, DESK_INTERNAL_ABOUT, DESK_INTERNAL_RUN, DESK_INTERNAL_SHUTDOWN,
       DESK_INTERNAL_MESSAGE, DESK_INTERNAL_POWER, DESK_INTERNAL_AUTOPLAY, DESK_INTERNAL_KINDS };
enum sh_menu_kind { SH_MENU_NONE, SH_MENU_START, SH_MENU_PROGRAMS, SH_MENU_DEVELOPER, SH_MENU_DOCUMENTS,
                    SH_MENU_SETTINGS, SH_MENU_HELP, SH_MENU_DESKTOP, SH_MENU_ICON, SH_MENU_TASK, SH_MENU_ACCESSORIES,
                    SH_MENU_GAMES, SH_MENU_NEW, SH_MENU_OPEN_WITH, SH_MENU_TRAY_SETTINGS,
                    SH_MENU_SETTINGS_CATEGORY, SH_MENU_SETTINGS_LAST = SH_MENU_SETTINGS_CATEGORY + CP_CATEGORIES - 1 };
enum sh_action { SH_ACT_NONE, SH_ACT_SUBMENU, SH_ACT_LAUNCH, SH_ACT_ABOUT, SH_ACT_RUN, SH_ACT_SHUTDOWN,
                 SH_ACT_DOCUMENT, SH_ACT_CONTROL, SH_ACT_KEYS, SH_ACT_ARRANGE, SH_ACT_ICON_OPEN,
                 SH_ACT_ICON_PROPERTIES, SH_ACT_REFRESH, SH_ACT_TASK_RESTORE, SH_ACT_TASK_MINIMIZE,
                 SH_ACT_TASK_CLOSE, SH_ACT_TASKMAN, SH_ACT_CLEAR_RECENT, SH_ACT_GAME, SH_ACT_EMPTY_RECYCLE, SH_ACT_FILE, SH_ACT_RENAME, SH_ACT_OPEN_WITH, SH_ACT_DOS_FULLSCREEN,
                 SH_ACT_TASK_MOVE, SH_ACT_TASK_SIZE, SH_ACT_TASK_MAXIMIZE };
#define SH_ITEM_SEPARATOR 1u
#define SH_ITEM_DISABLED 2u
#define SH_ITEM_DEFAULT 4u
struct sh_item { char label[44]; const char *icon; uint8_t action, flags; int16_t arg; };
struct sh_menu { int kind, count, hot, row_h, icon; struct desk_box box; struct sh_item items[SH_ITEMS]; };
/* Desktop shortcuts. action: SH_ACT_LAUNCH with a program name and an
 * optional argument (Files with a folder), or a built-in action. */
struct sh_icon { char label[FM_NAME], program[24], argument[FM_PATH]; const char *icon; int action, ciuki; struct desk_box box; };
struct desk_shell {
    struct ciuki_settings cfg;
    int gate;                       /* runtime gate scenarios: defaults only, no settings I/O */
    struct sh_menu menus[SH_LEVELS];
    int levels, menu_owner;         /* menu_owner: client for SH_MENU_TASK, icon for SH_MENU_ICON */
    struct sh_icon *icons; int icon_capacity;
    struct desk_folder *folder;
    int icon_drag, icon_dragged, drag_start_x, drag_start_y;
    int icon_count, icon_selected;
    uint64_t click_ns; int click_icon;
    int volume_open, volume, muted, mixer_state;   /* mixer_state: 0 unknown, 1 read-only, 2 controllable, -1 absent */
    int volume_drag;
    struct ciuki_net_state network;
    int network_open;
    struct ciuki_media_state media;
    char media_labels[26][33];
    struct desk_autoplay autoplay;
    int media_open, want_remove;
    unsigned media_scroll;
    uint32_t remove_generation;
    uint64_t network_poll_ns;
    int start_hot;                  /* the pointer is over the Ciuki button */
    int tooltip, hover_target; uint64_t hover_since; int32_t hover_x, hover_y;
    struct desk_box tooltip_last;
    uint32_t tooltip_hash;
    int tooltip_painted;
    struct sh_notification notification;
    int switcher, switch_pick, switch_count, switch_list[16];
    int about_tab, shutdown_choice, message_kind;
    char run_text[160]; unsigned run_length;
    char message_title[48], message_text[320];
    unsigned modifiers;             /* DU_SHIFT | DU_CTRL | DU_ALT from the raw key stream */
    int logo_down, alt_routed, alt_used, alt_code;
    int32_t motion_rx, motion_ry;   /* sub-pixel remainders of the mouse speed */
    /* Requests served by desktop.c after each input batch. */
    int want_save, want_wallpaper, want_mixer, want_power, want_reload;
    char want_run[192];
    /* Wallpaper cache: full screen, rebuilt when settings or size change. */
    uint32_t *wall; uint32_t wall_w, wall_h; int wall_valid;
    uint32_t *background; int background_valid, background_selected, background_outline;
    uint64_t background_rebuilds;
    struct desk_image image;        /* decoded file, owned by desktop.c */
    int image_failed;
    int wallpaper_pending;        /* owned frame is prepared by the wallpaper worker */
    char status[96];
    uint64_t mem_total_k, mem_free_k; uint32_t cpu_mhz, processes, uptime_s; int system_valid;
    char cpu_name[48];
};
struct desktop;
struct desk_client;
void desk_shell_reset(struct desktop *);
void desk_shell_apply(struct desktop *);
void desk_shell_background(struct desktop *, struct desk_box clip);
void desk_shell_topbar(struct desktop *, struct desk_box clip);
void desk_shell_overlay(struct desktop *, struct desk_box clip);
int desk_shell_button(struct desktop *, int button, int down);
void desk_shell_motion(struct desktop *);
unsigned desk_shell_cursor(const struct desktop *);
int desk_shell_key(struct desktop *, int code, int value);
int desk_internal_click(struct desktop *, int index, int32_t x, int32_t y);
void desk_shell_scale_motion(struct desktop *, int32_t *dx, int32_t *dy);
void desk_shell_tick(struct desktop *);
void desk_shell_media_update(struct desktop *, const struct ciuki_media_state *);
void desk_shell_network_update(struct desktop *, const struct ciuki_net_state *);
void desk_shell_clock_damage(struct desktop *, uint64_t previous_second);
struct desk_box desk_shell_tray_box(const struct desktop *);
struct desk_box desk_shell_tray_icon_box(const struct desktop *, enum sh_tray_icon);
struct desk_box desk_shell_clock_box(const struct desktop *);
void desk_shell_clock_text_at(const struct desktop *, uint64_t, char *, size_t);
int desk_shell_clock_width_at(const struct desktop *, uint64_t);
void desk_shell_tray_damage(struct desktop *, int previous_width);
void desk_shell_volume_damage(struct desktop *);
void desk_internal_paint(struct desktop *, int index, struct desk_box content, struct desk_box clip);
void desk_internal_closed(struct desktop *, int index);
int desk_internal_open(struct desktop *, int kind);
void desk_shell_message(struct desktop *, const char *title, const char *text, int error);
void desk_shell_message_class(struct desktop *, const char *title, const char *text, int icon);
void desk_shell_notify(struct desktop *, const char *title, const char *text, int icon, int action);
struct desk_box desk_shell_notification_bounds(const struct desktop *);
int desk_notification_intersects(const struct desktop *, struct desk_box);
void desk_shell_notify_error(struct desktop *, const char *title, int error);
void desk_shell_media_labels(struct desktop *, const struct ciuki_media_volume volumes[26]);
void desk_menu_open(struct desktop *, int kind, int32_t x, int32_t y);
void desk_menu_close(struct desktop *);
int desk_task_count(const struct desktop *);
const char *desk_program_icon_name(const char *program);
const char *desk_program_category(const char *program);
const uint32_t *desk_shell_icon(const char *tango_name, int size);
const uint32_t *desk_shell_window_icon(struct desktop *, int index, int size);   /* title-bar icon (compositor) */
int desk_shell_wallpaper_update(struct desktop *);
#endif
