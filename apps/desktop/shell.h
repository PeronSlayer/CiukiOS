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
#include "users.h"
#include "shell_link.h"
#include "settings_catalog.h"
#include "folder.h"
#include "autoplay.h"
#include "wallpaper.h"
#include "power_policy.h"
#include <stdint.h>
#include <stddef.h>
#include <ciuki/abi.h>
#define SH_LEVELS 10
#define SH_ITEMS 80
#define SH_PROGRAM_TRAY 8
struct sh_program_tray {
    uint32_t window, id, callback;
    uint32_t pixels[256];
    char tip[128];
    uint64_t click_ns;
};
struct sh_program_balloon {
    uint32_t window, id, timeout, icon;
    char title[64], text[160];
};
#define SH_ICONS (FM_ENTRIES + 4 + (int)DESK_LINKS)
#define SH_TRAY_ICON_SIZE 16
#define SH_TRAY_GAP 4
#define SH_TRAY_MARGIN 6
struct desktop;
int desk_shell_program_icon(struct desktop *, int, const struct desk_packet *, const struct ciuki_surface_info *, int);
int desk_shell_program_balloon(struct desktop *, int, const struct desk_packet *);
void desk_shell_program_remove(struct desktop *, uint32_t);
/* Tray identifiers; geometry places Notifications next to the clock. */
enum sh_tray_icon { SH_TRAY_VOLUME, SH_TRAY_NETWORK, SH_TRAY_NOTIFICATION, SH_TRAY_MEDIA, SH_TRAY_POWER, SH_TRAY_LANGUAGE };
enum sh_notify_action { SH_NOTIFY_DETAILS, SH_NOTIFY_MEDIA, SH_NOTIFY_NETWORK, SH_NOTIFY_VOLUME };
/* fx-21b: wording of the Windows notification (Microsoft KB 2654149, KB 314634). */
#define DESK_USB_UNRECOGNIZED_TITLE "USB Device Not Recognized"
#define DESK_USB_UNRECOGNIZED_TEXT "One of the USB devices attached to this computer has malfunctioned, " \
    "and CiukiOS does not recognize it. Unplug the device, wait a few seconds and plug it in again. " \
    "If the problem continues, try another USB port."
struct sh_notification {
    char title[64], text[320];
    uint64_t started, expires;
    int active, icon, action, balloon, slide;
};
#define SH_START_ROW 30
#define SH_SUB_ROW 22
#define SH_BANNER 24
#define SH_SEPARATOR 8
#include "start-orb.h"         /* shared orb / approved portrait geometry */
/* Built-in (internal) windows: desk_client.internal holds the kind. */
enum { DESK_INTERNAL_NONE, DESK_INTERNAL_ABOUT, DESK_INTERNAL_RUN, DESK_INTERNAL_SHUTDOWN,
       DESK_INTERNAL_MESSAGE, DESK_INTERNAL_POWER, DESK_INTERNAL_AUTOPLAY, DESK_INTERNAL_TRANSFER, DESK_INTERNAL_PROGRAMS, DESK_INTERNAL_GAME_DATA,
       DESK_INTERNAL_END_TASK /* G3: not responding or DOS warning (d->end_task) */, DESK_INTERNAL_SHORTCUT, DESK_INTERNAL_KINDS };
enum sh_menu_kind { SH_MENU_NONE, SH_MENU_START, SH_MENU_PROGRAMS, SH_MENU_DEVELOPER, SH_MENU_DOCUMENTS,
                    SH_MENU_SETTINGS, SH_MENU_HELP, SH_MENU_DESKTOP, SH_MENU_ICON, SH_MENU_TASK, SH_MENU_ACCESSORIES,
                    SH_MENU_GAMES, SH_MENU_NEW, SH_MENU_OPEN_WITH, SH_MENU_TRAY_SETTINGS,
                    SH_MENU_SYSTEM_TOOLS, SH_MENU_SETTINGS_CATEGORY, SH_MENU_SETTINGS_LAST = SH_MENU_SETTINGS_CATEGORY + CP_CATEGORIES - 1,
                    SH_MENU_DRAG, SH_MENU_TASKBAR, SH_MENU_FIND,
                    SH_MENU_POWER /* G11: tray scheme choice */, SH_MENU_POWER_METER /* G11: tray right click */ };

enum sh_action { SH_ACT_NONE, SH_ACT_SUBMENU, SH_ACT_LAUNCH, SH_ACT_ABOUT, SH_ACT_RUN, SH_ACT_SHUTDOWN,
                 SH_ACT_DOCUMENT, SH_ACT_CONTROL, SH_ACT_KEYS, SH_ACT_ARRANGE, SH_ACT_ICON_OPEN,
                 SH_ACT_ICON_PROPERTIES, SH_ACT_REFRESH, SH_ACT_TASK_RESTORE, SH_ACT_TASK_MINIMIZE,
                 SH_ACT_TASK_CLOSE, SH_ACT_TASKMAN, SH_ACT_CLEAR_RECENT, SH_ACT_GAME, SH_ACT_EMPTY_RECYCLE, SH_ACT_FILE, SH_ACT_RENAME, SH_ACT_OPEN_WITH, SH_ACT_DOS_FULLSCREEN, SH_ACT_DOS_MARK, SH_ACT_DOS_COPY, SH_ACT_DOS_PASTE, SH_ACT_DOS_QUICKEDIT,
                 SH_ACT_TASK_MOVE, SH_ACT_TASK_SIZE, SH_ACT_TASK_MAXIMIZE, SH_ACT_WIN32_LINK, SH_ACT_PROGRAMS_FOLDER, SH_ACT_WIN32_PROGRAM,
                 SH_ACT_LOGOFF, SH_ACT_LOCK,
                 SH_ACT_DRAG /* fx-19 right-drag menu: arg is the DROPEFFECT, 0 Cancel */,
                 /* G4 taskbar menu: retain main's action values. */
                 SH_ACT_CASCADE, SH_ACT_TILE_H, SH_ACT_TILE_V, SH_ACT_MINIMIZE_ALL, SH_ACT_ARRANGE_UNDO,
                 SH_ACT_TASKBAR_PROPERTIES, SH_ACT_FIND,
                 SH_ACT_FILES_ACTION, /* G8: appended after G7 Find. */
                 SH_ACT_HELP /* G15: Start > Help */,
                 SH_ACT_POWER_SCHEME /* G11: arg is the scheme */, SH_ACT_POWER_METER };

#define SH_ITEM_SEPARATOR 1u
#define SH_ITEM_DISABLED 2u
#define SH_ITEM_DEFAULT 4u
/* mark: 1 + the byte of label that is the underlined access key (0 none). */
struct sh_item { char label[44]; const char *icon; uint8_t action, flags, mark; int16_t arg; };
struct sh_menu { int kind, count, hot, row_h, icon; struct desk_box box; struct sh_item items[SH_ITEMS]; };
/* Desktop shortcuts. action: SH_ACT_LAUNCH with a program name and an
 * optional argument (Files with a folder), or a built-in action. */
struct sh_icon { char label[FM_NAME], program[24], argument[FM_PATH]; const char *icon; int action, ciuki, selected; struct desk_box box; };
/* fx-19 shell drag session (DoDragDrop): the desktop owns the pointer from
 * the drag threshold until the drop, Esc or a right-drag menu choice. */
enum { SH_DROP_NONE, SH_DROP_DESKTOP, SH_DROP_ICON, SH_DROP_TRASH, SH_DROP_FILES, SH_DROP_APP, SH_DROP_TASK };
#define SH_DRAG_IMAGE_ITEMS 8
struct sh_dnd {
    int active, menu;                 /* menu: right-drag menu waits for a choice */
    int source;                       /* Files client index, or -1 for desktop icons */
    uint32_t source_pid, serial;
    unsigned button;                  /* CIUKI_BUTTON_LEFT or CIUKI_BUTTON_RIGHT */
    char **paths; char *types; unsigned count;
    int source_volume; unsigned source_allowed;
    int kind, target;                 /* SH_DROP_*, icon or client index */
    unsigned effect, allowed, keys;
    int status_target; uint32_t status_serial; /* DRAG_STATUS of the current Files target */
    int32_t start_x, start_y, menu_x, menu_y;
    char destination[FM_PATH];
    uint64_t hover_ns, repeat_ns;
    int hover_task, drop_index;
    int icons[SH_DRAG_IMAGE_ITEMS]; unsigned icon_count;   /* desktop source: dragged icons drawn */
    struct desk_box image;            /* drag image bounds as last painted */
};
struct desk_shell {
    struct sh_program_tray program_tray[SH_PROGRAM_TRAY];
    struct sh_program_balloon program_balloons[SH_PROGRAM_TRAY];
    unsigned program_balloon_count;
    uint32_t balloon_window, balloon_id;
    struct desk_launch_record shortcut_record;
    char shortcut_source[260], shortcut_number[24], shortcut_status[160];
    unsigned shortcut_field, shortcut_repair;
    int shortcut_index;
    struct ciuki_settings cfg;
    struct user_profile profile;
    struct users_view users;
    struct users_request user_request;
    int user_gate, user_focus, user_pending, user_want, user_choose, user_previous_focus;
    char user_status[96];
    int programs_selected, programs_scroll, programs_click;
    unsigned save_game, save_op, save_bytes, save_quota, save_confirm, save_focus, save_busy;
    char save_status[160];
    uint64_t programs_click_ns;
    uint64_t clock_click_ns;        /* G1: first click of a tray-clock double-click */
    uint64_t volume_click_ns;       /* G22: popup on click, Volume Control on double-click */
    int gate;                       /* runtime gate scenarios: defaults only, no settings I/O */
    struct sh_menu menus[SH_LEVELS];
    int levels, menu_owner;         /* menu_owner: client for SH_MENU_TASK, icon for SH_MENU_ICON */
    struct sh_icon *icons; int icon_capacity;
    struct desk_folder *folder;
    int icon_drag, icon_dragged, drag_start_x, drag_start_y;
    int icon_count, icon_selected;
    /* fx-19 multi-selection: icons[k].selected, with icon_selected the focus
     * (selected unless focus_unselected); anchor for Shift ranges; the
     * rubber band in screen coordinates and the selection it started from. */
    int focus_unselected, anchor, icon_button, icon_pending;
    int band, band_x0, band_y0, band_x1, band_y1; unsigned band_modifiers;
    uint8_t *band_original; int band_capacity;
    struct sh_dnd dnd;
    uint64_t click_ns; int click_icon;
    int volume_open, volume, muted, mixer_state;   /* mixer_state: 0 unknown, 1 read-only, 2 controllable, -1 absent */
    int volume_drag;
    struct ciuki_net_state network;
    struct ciuki_power_status power_status;
    uint64_t power_poll_ns;
    uint32_t power_seen;
    /* G11 power policy (docs/design/power-policy.md): desktop.cfg's power_*
     * lines, the meter summary, alarm and PBT_APMPOWERSTATUSCHANGE state,
     * the call-73 EVENTS seen, QUERY's CAN_STANDBY, the monitor timeout
     * of the current power source (consumed by G10's DPMS) and the drive
     * timer last sent to the kernel. */
    struct pp_config power_cfg;
    struct pp_summary power_summary;
    struct pp_alarm_state power_alarm;
    struct pp_broadcast_state power_broadcast;
    struct ciuki_power_events power_events;
    int power_events_valid, power_can_standby;
    int power_meter_hidden;   /* !show_meter; zero (shown) in a zeroed desktop */
    uint32_t power_monitor_seconds, power_disk_sent;
    uint64_t power_sleep_notice_ns;
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
    int about_tab, about_scroll, shutdown_choice, message_kind;
    int confirm;                    /* G2: SH_CONFIRM_*, the message window asks Yes or No */
    char run_text[160]; unsigned run_length;
    char message_title[48], message_text[320];
    unsigned modifiers;             /* DU_SHIFT | DU_CTRL | DU_ALT from the raw key stream */
    int logo_down, alt_routed, alt_used, alt_code;
    /* G4 (KB-12, KB-13): Windows keys held (bit 0 left, bit 1 right) and the keys
     * a Win chord consumed, whose releases are swallowed too; the keyboard
     * focus of the taskbar (Win+Tab, Ctrl+Esc then Esc): task_pick is a client
     * slot or DESK_CLIENTS for the Start button, task_return the window id that
     * had the focus before; start_keyboard: the Start menu was opened by key. */
    unsigned logo_keys; uint16_t chord[4];
    int taskbar_key, task_pick, start_keyboard, start_focus_shown; uint32_t task_return;
    int32_t motion_rx, motion_ry;   /* sub-pixel remainders of the mouse speed */
    /* Requests served by desktop.c after each input batch. */
    int want_save, want_wallpaper, want_mixer, want_power, want_reload, want_programs;
    /* G11: want_power_force ends programs that stop responding in that
     * session (alarm "Force"); want_standby 1 from the user, 2 automatic. */
    int want_power_force, want_standby;
    char want_run[192];
    /* Wallpaper cache: full screen, rebuilt when settings or size change. */
    uint32_t *wall; uint32_t wall_w, wall_h; int wall_valid;
    uint32_t *background; int background_valid, background_selected, background_outline;
    uint64_t background_rebuilds;
    uint32_t background_serial; /* f2-92: changes with every write to background */
    struct desk_image image;        /* decoded file, owned by desktop.c */
    int image_failed;
    int wallpaper_pending;        /* owned frame is prepared by the wallpaper worker */
    char status[96];
    uint64_t mem_total_k, mem_free_k; uint32_t cpu_mhz, processes, uptime_s; int system_valid;
    char cpu_name[48];
};
struct desktop;
struct desk_client;
/* mode 1: logon (Cancel selects default); mode 2: password lock. */
void desk_logon_open(struct desktop *, int mode);
void desk_logon_result(struct desktop *, const struct users_view *, int error);
void desk_logon_done(struct desktop *);
int desk_logon_input(struct desktop *, const struct ciuki_input_event *);
void desk_session_lock(struct desktop *);
void desk_shortcut_properties(struct desktop *, unsigned link, int repair);
void desk_shell_reset(struct desktop *);
void desk_shell_apply(struct desktop *);
void desk_shell_background(struct desktop *, struct desk_box clip);
void desk_shell_topbar(struct desktop *, struct desk_box clip);
void desk_shell_overlay(struct desktop *, struct desk_box clip);
int desk_shell_button(struct desktop *, int button, int down);
void desk_shell_motion(struct desktop *);
unsigned desk_shell_cursor(const struct desktop *);
int desk_shell_key(struct desktop *, int code, int value);
int desk_shell_consumer(struct desktop *, unsigned action, int value);
int desk_shell_wheel(struct desktop *, int detents);   /* wheel over the focused built-in window; 1 when consumed */
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
int desk_shell_power_update(struct desktop *, const struct ciuki_power_status *);
/* G11: the power meter icon is shown (a valid record and "Show power meter
 * on taskbar"). */
int desk_shell_power_shown(const struct desktop *);
/* G11: after each status poll: EVENTS (NULL when unavailable) and QUERY's
 * bits (negative when unavailable): alarms, BIOS sleep requests,
 * PBT_APMPOWERSTATUSCHANGE, the monitor timeout. */
void desk_shell_power_events(struct desktop *, const struct ciuki_power_events *, int query);
/* G11: desktop.cfg was (re)loaded into sh.cfg: parse its power_* lines. */
void desk_shell_power_config(struct desktop *);
void desk_shell_power_scheme(struct desktop *, int scheme);   /* tray menu: make active and save */
void desk_shell_volume_damage(struct desktop *);
void desk_internal_paint(struct desktop *, int index, struct desk_box content, struct desk_box clip);
void desk_internal_closed(struct desktop *, int index);
int desk_internal_open(struct desktop *, int kind);
void desk_shell_message(struct desktop *, const char *title, const char *text, int error);
/* G2: a Yes/No question in the message window; Yes runs desk_folder_confirmed(action), No or closing it desk_folder_cancelled. */
enum { SH_CONFIRM_NONE, SH_CONFIRM_DELETE, SH_CONFIRM_EMPTY };
void desk_shell_confirm(struct desktop *, const char *title, const char *text, int icon, int action);
void desk_shell_bin_state(struct desktop *, int full);   /* the Recycle Bin icon's full or empty picture */
void desk_shell_message_class(struct desktop *, const char *title, const char *text, int icon);
void desk_shell_notify(struct desktop *, const char *title, const char *text, int icon, int action);
struct desk_box desk_shell_notification_bounds(const struct desktop *);
int desk_notification_intersects(const struct desktop *, struct desk_box);
void desk_shell_notify_error(struct desktop *, const char *title, int error);
void desk_shell_media_labels(struct desktop *, const struct ciuki_media_volume volumes[26]);
void desk_menu_open(struct desktop *, int kind, int32_t x, int32_t y);
void desk_menu_close(struct desktop *);
void desk_shell_find(struct desktop *, int computer);   /* G7 Files Find; computer selects the separate network-search placeholder. */
void desk_shell_taskbar_leave(struct desktop *);        /* the taskbar loses the keyboard focus (a window took it) */
int desk_shell_caption_icon(struct desktop *, int client, uint64_t stamp, int menu_was_open);   /* click on the caption icon: window menu, second click closes */
int desk_task_count(const struct desktop *);
const char *desk_program_icon_name(const char *program);
const char *desk_program_category(const char *program);
const uint32_t *desk_shell_icon(const char *tango_name, int size);
const uint32_t *desk_shell_window_icon(struct desktop *, int index, int size);   /* title-bar icon (compositor) */
int desk_shell_wallpaper_update(struct desktop *);
/* fx-19 drag session hooks (input.c, client.c). */
int desk_shell_drag_begin(struct desktop *, int client, unsigned button, unsigned count);
void desk_shell_drag_status(struct desktop *, int client, uint32_t serial, unsigned effect, unsigned allowed);
int desk_shell_drag_motion(struct desktop *);
int desk_shell_drag_button(struct desktop *, int button, int down);
void desk_shell_drag_cancel(struct desktop *);
int desk_shell_icon_marked(const struct desktop *, int index);
#endif
