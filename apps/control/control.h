/* SPDX-License-Identifier: MIT
 * /Programs/control: the CiukiOS Control Panel on libdeskui. The panel edits
 * /system/settings/desktop.cfg (apps/desktop/settings.h) and asks the
 * desktop to apply it at once (DESK_SETTINGS); the master volume goes
 * through the desktop, which holds the writable mixer handle. */
#ifndef CIUKI_CONTROL_H
#define CIUKI_CONTROL_H
#include "../libdeskui/deskui.h"
#include "../libdeskui/textarea.h"
#include "../desktop/settings.h"
#include "../desktop/shell_link.h"
#include "../desktop/sounds.h"
#include "../desktop/autoplay.h"
#include "../desktop/wallpaper.h"
#include "../desktop/settings_catalog.h"
#include <ciuki/display.h>
#include <ciuki/abi.h>
#include "../../src/kernel/include/ciuki/game_ports_settings.h"
#include <ciuki/firmware_abi.h>
#include <ciuki/tzdata.h>
#include <ciuki/font.h>
#include <stdint.h>
#include "driver_pack.h"
#include "../desktop/users.h"
#define CP_PACK_INSTALL 1450
#define CP_PACK_REMOVE 1451
#define CP_PACK_LOCATION 1452
#define CP_PACK_PREVIEW 1453
#define CP_PACK_CONFIRM_REMOVE 1454
#define CP_PACK_FILES 1455
#define CP_PROGRAM_MAX 96u
#define CP_WIDTH 560
#define CP_HEIGHT 420
#define CP_WALLPAPERS 40
enum { CP_BACKGROUND, CP_APPEARANCE, CP_SCREEN_SETTINGS, CP_DISPLAY_TABS };
enum { CP_DISPLAY_TABS_ID = 960, CP_OK, CP_CANCEL, CP_CATEGORY, CP_POWER_ACTION, CP_OPEN_LOG, CP_LOG_PREV, CP_LOG_NEXT, CP_LOG_VIEW, CP_LOG_STATUS };
#define CP_LOG_BYTES 8192u
#define CP_LOG_FILE "/system/logs/BOOT.LOG"
#define CP_FONTS_MAX 32
#define CP_FONT_LIST 900
struct cp_font_entry {
    char family[CIUKI_FONT_NAME_MAX], style[CIUKI_FONT_NAME_MAX], file[128];
};
#define CP_TREE_MAX (1 + CIUKI_DEVICE_CLASSES + CIUKI_DEVICES_MAX)
#define CP_BOOT_MAX 128
enum { CP_AREA_LABEL = 800, CP_AREA, CP_DEPTH_LABEL, CP_DEPTH, CP_APPLY, CP_MODE_NOTE,
       CP_REFRESH, CP_SUMMARY, CP_CONFIRM, CP_MODE_TIMER };
enum { CP_MODE_IDLE, CP_MODE_WAIT, CP_MODE_CONFIRM, CP_MODE_REVERT, CP_MODE_SAVE };
/* f2-88b: one preview job. image is moved to the worker (NULL: decode path first). */
struct cp_preview_req {
    char path[128]; int mode; uint32_t colour, bar; int font; uint32_t label; int aa;
    const uint32_t *portrait; struct desk_image image;
};
struct cp_preview_done {
    struct cp_preview_req req;      /* the snapshot the preview was rendered from */
    struct desk_image image;        /* the image returned to the UI (pixels NULL: decode failed) */
    int rendered;                   /* preview[] holds the rendering of req */
    int error;                      /* negative errno; ECANCELED results are discarded */
    unsigned decode_ms, render_ms;
    uint32_t *preview;              /* caller-supplied 192*144 buffer, filled when rendered */
};
#define CP_NET_TEST 4u /* UI worker command; never passed to syscall 90. */
struct cp_network_result {
    struct ciuki_net_control adapters[2];
    unsigned count;
    int error;
    char message[96];
};
struct cp_game_result {
    struct game_ports_settings ports;
    unsigned game;
    int saved, error, rejected;
};
struct cp_host {
    int (*user_begin)(void *,const struct users_request *);
    int (*user_poll)(void *,struct users_view *,int *);
    int (*pack_scan)(void *, const char *, char names[][192], unsigned);
    int (*pack_preview)(void *, struct dp_plan *, const char *, const char *, const struct ndp_device *, char reason[160]);
    int (*pack_install)(void *, const struct dp_plan *, const char *, char reason[160]);
    int (*pack_remove)(void *, const struct ndp_device *, char reason[160]);
    int (*load)(void *, char *text, size_t size, size_t *length);      /* desktop.cfg; -1 when absent */
    int (*save)(void *, const char *text, size_t length);
    int save_async; /* save queued; main calls cp_settings_saved after durable completion */
    int (*wallpapers)(void *, char names[][CFG_PATH_MAX], unsigned capacity);   /* absolute paths */
    int (*decode)(void *, const char *path, struct desk_image *out);
    /* f2-88/f2-88b: the UI thread never reads, decodes or resamples a wallpaper. decode_begin starts a worker
     * job (0, or -errno). decode_poll takes a result once, after endpoint readiness. Both return at once
     * and never wait on the worker. The request moves the UI's decoded image (if any) to the worker, which
     * returns it in done->image; the worker also renders the 192x144 preview from a snapshot of the request. */
    int (*decode_begin)(void *, const struct cp_preview_req *);
    int (*decode_poll)(void *, struct cp_preview_done *);
    void (*decode_cancel)(void *);
    void (*log)(void *, const char *line);                            /* serial record, optional */
    int (*mixer)(void *, int *volume, int *muted);                    /* read-only master; -1 absent */
    int (*apply)(void *, unsigned request, int value, int flag);      /* DESK_SETTINGS */
    uint64_t (*monotonic)(void *);                                    /* nanoseconds */
    uint64_t (*now)(void *);                                          /* UTC seconds */
    int (*game_begin)(void *, unsigned, const struct game_ports_settings *); /* NULL=load */
    int (*game_poll)(void *, struct cp_game_result *);
    int (*joystick)(void *, const struct ciuki_joystick_control *);
    int (*joystick_save_game)(void *, unsigned, const struct ciuki_joystick_control *);
    int (*devices)(void *, struct ciuki_devices_info *);
    int (*firmware)(void *, struct ciuki_firmware_page *);
    int (*hwid_name)(void *, unsigned vendor, unsigned device, char *out, size_t size);
    int (*usb_hwid_name)(void *, unsigned vendor, unsigned device, char *out, size_t size);
    int (*boot_load)(void *, char *, size_t, size_t *);
    int (*boot_save)(void *, const char *, size_t);
    int (*load_sounds)(void *, char *, size_t, size_t *);
    int (*save_sounds)(void *, const char *, size_t);
    int (*display)(void *, struct ciuki_display_info *);              /* ciuki_display_query: 0 or -errno */
    int (*engine)(void *, struct ciuki_accel_info *);
    int (*engine_info)(void *, struct ciuki_display_engine_info *);
    int (*accel_retry)(void *); /* re-arm existing marker; restart required */
    int (*network)(void *, struct ciuki_net_state *);
    int (*network_begin)(void *, unsigned, const struct ciuki_net_control *, const char *);
    int (*network_poll)(void *, struct cp_network_result *);
    int (*log_read)(void *, uint32_t offset, char *, size_t cap, size_t *length, uint32_t *total);
    int (*power)(void *); /* opens the shell's safe shutdown dialog */
    int (*power_status)(void *, struct ciuki_power_status *);
    int (*audio_diagnostics)(void *, struct ciuki_audio_diagnostics *);
    int (*autoplay_list)(void *, struct ap_preference *, unsigned);
    int (*autoplay_forget)(void *, const char *);
    /* f5-07A RPC adapter; enumerate HKLM and HKCU Uninstall, never open hives.
     * Run re-resolves the selected key and command at the service. */
    int (*uninstall_list)(void *, struct desk_uninstall_entry *, unsigned);
    int (*uninstall_run)(void *, const char *key);
    int (*program_remove)(void *,const struct desk_uninstall_entry *);
    int (*program_install)(void *,const char *);
    void (*programs_changed)(void *);
    /* G1: the system time-zone record (syscall 97): GET, SET_LOCAL,
     * SET_ZONE; 0 or -errno. NULL: Date/Time shows the host clock read-only. */
    int (*time_zone)(void *, struct ciuki_time_zone *, uint32_t operation);
    void *ctx;
    const uint32_t *portrait;                                         /* 256x256 or NULL */
    uint32_t screen_w, screen_h;
    char version[65], build[65], machine[65];
    uint64_t mem_total_k, mem_free_k; unsigned processes;
    int fonts_async;
    void (*job_notify)(void *); /* completion notification; uses host.ctx */
    const char *fonts_path;                                          /* trusted installed store; NULL = /System/fonts */
};
struct cp_app {
    struct users_view users;
    struct users_request user_request;
    struct du_item user_items[USERS_MAX];
    int user_busy, user_loaded, user_operation;
    char user_note[160];
    struct game_ports_settings game_ports;
    struct ciuki_joystick_control game_joystick;
    int game_open, game_busy, game_loaded, game_joy_ready, game_joy_dirty, game_save;
    char game_note[128];
    struct ciuki_joystick_control joystick, joystick_dos;
    int joystick_busy, joystick_calibrating, joystick_capture_pending;
    int joystick_save_pending;
    unsigned joystick_axis, joystick_step, joystick_game;
    int32_t joystick_points[4][3];
    char joystick_line[128];

    struct cp_network_result network;
    struct ciuki_net_config network_edit;
    struct du_item network_items[2], network_tabs[3];
    char network_names[2][64], network_message[96], network_test_name[128];
    unsigned network_selected, network_tab;
    int network_busy, network_loaded, building;
    struct dp_plan *pack;
    char pack_root[DP_PATH], pack_names[32][192], pack_reason[160];
    int pack_count, pack_index, pack_valid;
    unsigned pack_file_page;
    struct ndp_device pack_device;
    struct desk_uninstall_entry uninstall[CP_PROGRAM_MAX];
    struct du_item uninstall_items[CP_PROGRAM_MAX];
    unsigned uninstall_count, uninstall_selected;
    char uninstall_sizes[CP_PROGRAM_MAX][32];
    unsigned remove_pending;
    struct du_ui *ui;
    struct cp_host host;
    char power_lines[20][128];
    struct ciuki_settings cfg;
    struct ciuki_settings display_original;
    int display_tab, display_session, display_dirty, display_preview, display_write;
    int display_accept, display_leaving, close_pending;
    int navigation_pending, next_page;
    uint16_t display_original_mode;
    int category, applet_pages[CP_PAGES];
    struct du_item category_items[CP_CATEGORIES], display_tabs[CP_DISPLAY_TABS];
    char engine_line[160];
    struct du_textarea log; uint32_t log_offset, log_total; char log_status[96];
    struct ap_preference autoplay[AP_PREFS];
    struct du_item autoplay_items[AP_PREFS];
    unsigned autoplay_count, autoplay_selected;
    int page, volume, muted, mixer_ok, audio_testing;
    struct ciuki_audio_diagnostics audio_before;
    int audio_before_ok;
    char sound_text[SOUND_TEXT_MAX+1];
    struct sound_scheme sounds; unsigned sound_selected;
    struct du_item sound_items[SOUND_EVENTS], scheme_items[3];
    char sound_volume_label[24];
    char wall_names[CP_WALLPAPERS][CFG_PATH_MAX]; unsigned wall_count;
    struct du_item theme_items[CFG_THEMES], wall_items[CP_WALLPAPERS + 8], key_items[CFG_KEYS_COUNT], tab_items[CP_PAGES];
    char wall_labels[CP_WALLPAPERS + 8][48];
    uint32_t preview[192 * 144];
    int preview_valid;
    struct desk_image image;
    int preview_pending;            /* an image wallpaper decode is scheduled after the first paint */
    int decode_wait;                /* an asynchronous decode is running for decode_path */
    int decode_cancelled;
    unsigned decode_ms, render_ms, decode_done;
    struct cp_preview_req wait_req; /* snapshot of the running job (image field unused) */
    char decode_path[128], decode_failed[128];
    char image_path[128];           /* path of the wallpaper decoded into image (preview_build cache) */
    char status[96], clock_line[64], date_line[64];
    struct ciuki_devices_info devices;
    struct ciuki_firmware_page firmware;
    uint32_t firmware_index;
    int devices_ok, system_tab, properties_device, detail_list;
    unsigned detail_index;
    uint64_t snapshot_time;
    unsigned collapsed;
    uint32_t selected_device_id;
    struct du_item applet_items[CP_PAGES], system_tabs[4];
    struct du_item tree_items[CP_TREE_MAX], driver_items[CIUKI_DRIVERS_MAX], lease_items[CIUKI_LEASES_MAX];
    int tree_device[CP_TREE_MAX]; unsigned tree_count;
    char device_names[CIUKI_DEVICES_MAX][96];
    char driver_bound[CIUKI_DRIVERS_MAX][16], lease_range[CIUKI_LEASES_MAX][48], lease_gen[CIUKI_LEASES_MAX][16];
    char boot_text[CP_BOOT_MAX]; size_t boot_length;
    int boot_ok, serial_next;
    int quit;
    struct ciuki_display_info display;
    struct du_item area_items[32], depth_items[3];
    char area_labels[32][32];
    uint16_t area_w[32], area_h[32], depths[3];
    unsigned area_count, depth_count, area, depth;
    int display_ok, mode_state;
    uint16_t previous_mode, pending_mode;
    uint64_t mode_deadline;
    uint64_t mode_retry_ns; int mode_retry;
    char adapter_lines[10][160];
    struct cp_font_entry fonts[CP_FONTS_MAX];
    struct du_item font_items[CP_FONTS_MAX];
    unsigned font_count, font_selected;
    int fonts_loaded;
    struct cp_font_job *font_job;
    struct ciuki_font_face *font_face;
    char font_status[96];
    /* G1 Date/Time Properties (datetime.inc). */
#define CP_TZ_MAX 64u
    struct ciuki_time_zone time;
    int time_ok, dt_tab, dt_zone, dt_auto, dt_zone_dirty, dt_date_dirty, dt_time_dirty;
    int dt_year, dt_month, dt_day;
    struct du_item dt_tabs[2], dt_zone_items[CP_TZ_MAX];
    char dt_time_text[16], dt_zone_line[160], dt_dst_line[192];
};
void cp_game_open(struct cp_app *, unsigned);
void cp_joystick_result(struct cp_app *, const struct ciuki_joystick_control *, int);
void cp_assets_ready(struct cp_app *);
int cp_attach(struct cp_app *, struct du_ui *, int page);
void cp_detach(struct cp_app *);
int cp_page_index(const char *name);
void cp_tick(struct cp_app *);
void cp_open(struct cp_app *, int page);
int cp_snapshot_valid(const struct ciuki_devices_info *);
int cp_hwid_lookup_json(const char *json, unsigned vendor, unsigned device, char *out, size_t size);
void cp_tree_build(struct cp_app *);
const char *cp_device_class(unsigned);
const char *cp_device_state(unsigned);
/* Preserve every other BOOT.CFG token, including test selectors. */
int cp_boot_serial(const char *text, size_t length, int serial, char *out, size_t capacity);
void cp_audio_result(struct cp_app *, int result);
void cp_engine_refresh(struct cp_app *);
void cp_preview_request(struct cp_app *, struct cp_preview_req *, int);
int cp_system_thumbnail(const char *, char *, size_t);
void cp_preview_ready(struct cp_app *);
void cp_preview_cancel(struct cp_app *);
int cp_display_wait_ms(uint64_t now, uint64_t deadline);
int cp_display_refresh(struct cp_app *);
void cp_display_sync(struct cp_app *);
int cp_display_event(struct cp_app *, const struct du_event *);
void cp_display_result(struct cp_app *, int result);
void cp_display_cancel(struct cp_app *);
void cp_display_saved(struct cp_app *, int error);
void cp_settings_saved(struct cp_app *, int error);
int cp_display_store(struct cp_app *);
void cp_display_accept(struct cp_app *);
int cp_display_leave(struct cp_app *);
int cp_close(struct cp_app *);
void cp_adapter_paint(struct cp_app *, struct du_canvas *);
const char *cp_adapter_name(uint16_t vendor, uint16_t device, char *out, size_t size);
void cp_settings_message(struct ciuki_message *, uint32_t serial, unsigned request, int value, int flag);
void cp_fonts_build(struct cp_app *);
void cp_fonts_paint(struct cp_app *, struct du_canvas *);
int cp_fonts_event(struct cp_app *, const struct du_event *);
/* Service after first paint/input/completion; job_notify supplies native wakeups. */
void cp_fonts_poll(struct cp_app *);
void cp_fonts_close(struct cp_app *);
int cp_pack_event(struct cp_app *, const struct du_event *);
void cp_pack_buttons(struct cp_app *);
#endif
