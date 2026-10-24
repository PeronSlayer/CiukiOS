/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_APP_DESKTOP_H
#define CIUKI_APP_DESKTOP_H
#include "protocol.h"
#include "theme.h"
#include "start-orb.h"
#include "shell_link.h"
#include "startup.h"
#include "access.h"
#include <stddef.h>
#define DESK_CLIENTS 16
#define DESK_DAMAGE_MAX 32
struct desk_dirty_row { int32_t left, right; };   /* visible-span painter (compositor.c) */
#define DESK_TX_MAX 64
#define DESK_V3_OVER_MAX 64              /* f2-91: v3 transport overflow behind a full tx ring (lazily allocated) */
#define DESK_V3_SILENT_NS 5000000000ull  /* f2-91: a v3 client silent this long with 128 pending is dropped */
#define DESK_BORDER DESK_FRAME_BORDER   /* theme.h: one frame geometry for windows and dialogs */
#define DESK_TITLE DESK_FRAME_TITLE
#define DESK_RESIZE_MARGIN 7
#define DESK_EDGE_LEFT 1u
#define DESK_EDGE_RIGHT 2u
#define DESK_EDGE_TOP 4u
#define DESK_EDGE_BOTTOM 8u
/* The Ciuki (Start) button at the left end of the top bar; the Start menu
 * drops from it. Task buttons start DESK_TASK_X from the taskbar's left edge. */
#define DESK_TASK_X 4
#define DESK_BACKGROUND 0x375564u
#define DESK_PORTRAIT_SIZE 256
#define DESK_MENU_MAX 32
#define DESK_MENU_ROW 28
#define DESK_CLOSE_NS 2000000000ull
/* App scenarios (runtime-selected in the post-fault interaction stage): the
 * launched program gets --test=<its name> and reports ordered checkpoint
 * PINGs whose serials are base+n. */
#define DESK_APP_FILES 1
#define DESK_APP_NOTE 2
#define DESK_APP_CONTROL 3          /* /Programs/control: may send DESK_SETTINGS */
#define DESK_APP_TERMINAL 5         /* launcher-authorized terminal requests */
#define DESK_APP_IDE 4              /* /Programs/ciukide: project LAUNCH only */
#define DESK_FILES_CHECKPOINT 0xf2310000u
#define DESK_FILES_CHECKPOINTS 5
#define DESK_NOTE_CHECKPOINT 0xf2320000u
#define DESK_NOTE_CHECKPOINTS 3
#define DESK_APP_CHECKPOINT(kind) ((kind)==DESK_APP_NOTE ? DESK_NOTE_CHECKPOINT : DESK_FILES_CHECKPOINT)
#define DESK_APP_CHECKPOINTS(kind) ((kind)==DESK_APP_NOTE ? DESK_NOTE_CHECKPOINTS : DESK_FILES_CHECKPOINTS)
struct desk_box { int32_t x, y, w, h; };
#include "shell.h"
#include "sounds.h"
#define DESK_SOUND_PENDING 1
struct desk_damage { unsigned count; struct desk_box boxes[DESK_DAMAGE_MAX]; };
/* Per completed/attempted frame, plus totals for the 2 s hardware sample. */
struct desk_frame_stats {
    uint64_t frames, damage_px, scene_px, presents, lfb_bytes;
    uint64_t composite_ns, present_ns, input_ns, input_lag_ns;
    uint64_t completed, frame_ns, frame_p95_ns, frame_max_ns, overruns, dropped;
    /* f2-92: 2D engine requests that succeeded (fills, copies including
     * offscreen-cache copies, writes = cache uploads), engine requests that
     * fell back to software, and rows served from the offscreen caches. */
    uint64_t accel_fills, accel_copies, accel_writes, accel_fallback, cache_rows;
    uint64_t copy_bytes, compare_px; /* clipped RAM copies and pixel equality proofs */
};
/* f2-92: an unchanged surface kept in engine-only video memory below the
 * scanout (CIUKI_ACCEL_OFFSCREEN rows): the wallpaper/icon background, or a
 * window's content. Video memory equals `shadow` (or the shell background
 * at `serial`) once every row is uploaded; a scanout row is copied from it
 * only after an exact comparison with the composed RAM frame. */
enum { DESK_VRAM_NONE, DESK_VRAM_BACKGROUND, DESK_VRAM_WINDOW };
#define DESK_VRAM_ENTRIES 3
struct desk_vram_entry {
    int kind, client, valid;
    uint32_t row, width, height, uploaded, serial;
    uint32_t *shadow;
    uint64_t dirty_ns, order;
};
#define DESK_LATENCY_BUCKETS 256
#define DESK_LINKS_SCAN 1u
#define DESK_LINKS_GAMES 2u
#define DESK_FRAME_NS 16666667ull /* VBE does not expose measured refresh; conservative 60 Hz */
#define DESK_SMALL_FRAME_NS 33333333ull  /* f2-95: 30 fps for continuous small damage */
#define DESK_INPUT_ACTIVE_NS 250000000ull /* f2-95: input this recent keeps the display rate */
#define DESK_POINTER_RETRY_NS 1000000000ull /* f2-96: software pointer offered back to the engine */
struct desk_gfx_stats {
    uint32_t serial,app_serial,render_us,submit_us,wait_us;
    uint32_t width,height,source_w,source_h,view_w,view_h;
    uint64_t convert_ns,copy_ns,slot_ns;
};
struct desk_client {
    int channel, surface, hello, window, unresponsive, ping_pending, resync_pending;
    int configure_pending, focus_pending, close_pending;
    int commit_pending, resizable, zoomed;
    unsigned cursor;
    /* V3 child slots share the transport's fd but never receive/close it. */
    unsigned v3_peer, v3_id, v3_generation, v3_style, v3_scale;
    uint32_t gl_present_request; /* result waits for completed compositor frame */
    uint32_t v3_win32_hwnd; /* f5-07B: copied identity for session z-order */
    unsigned v3_failed, v3_last[DESK_CLIENTS], v3_pointer;
    /* f2-91 (transport roots): ordered overflow behind the full tx ring, busy marks, silence clock. */
    struct ciuki_message *v3_over; unsigned v3_over_head, v3_over_count, v3_over_lost;
    int v3_busy, v3_busy_mark;
    uint64_t v3_progress_ns;
    struct desk_box v3_clip;
    int32_t v3_visibility;
    unsigned v3_cursor, v3_cursor_valid;
    uint32_t v3_lease_w, v3_lease_h, v3_lease_bpp, v3_lease_wanted, v3_lease_mode;
    uint32_t v3_cursor_pixels[4][1024];
    uint8_t v3_hot_x[4], v3_hot_y[4];
    uint32_t owner_id;           /* shell modal owner, stable across slot reuse */
    uint32_t v3_limits[4];       /* G4: WM_GETMINMAXINFO track sizes (min w/h, max w/h), 0 default */
    int minimized, internal, closing, program;   /* internal: DESK_INTERNAL_* kind; program: 1 Files, 2 CiukNote, 3 Control Panel */
    int collapsed;                /* retained shade state, independent of maximize */
    int32_t shade_height;
    int taskman;                  /* f2-33: launched /Programs/tasks; may send task requests */
    int sysapp;                   /* G30: launched sysinfo/sysmon/schedtask/resmeter; window list and argument-free catalogue launches */
    int gfxdemo;                  /* launch-bound graphics diagnostics authority */
    int dosbox;                   /* launch-bound DOS full-screen menu */
    int mixer;                    /* launched Volume Control or Music Player; may send MIXER_SET */
    int awaiting_frame;           /* launched graphical program that has not sent its first DAMAGE yet */
    int launch_frame_pending;    /* first DAMAGE accepted, awaiting successful desktop present */
    uint64_t launch_input_ns;     /* original click, retained through first-frame presentation */
    uint64_t launched_ns;         /* its launch time (desktop.c: the gate scenarios' input hold) */
    uint64_t window_ns, damage_ns; /* fx-15: its first CREATE_WINDOW and first frame (first-frame record) */
    int spawn_pending;            /* fx-8: slot reserved while the launcher worker creates its process */
    int accept_files;             /* fx-19: DragAcceptFiles (DESK_ACCEPT_FILES) */
    int flash;                    /* G18: visual inversion, never activation */
    int initial_show;             /* shortcut Run state, consumed on CREATE */
    /* G3: session_aware: the window registered for QUERY_END_SESSION;
     * session_end: 1 send END_SESSION(1) at the end of the session, 2 the
     * Win32 broker delivered WM_ENDSESSION (no close message); close_sent_ns:
     * when the pending close request was sent (hung-program detection). */
    int session_aware, session_end;
    int power_aware, power_pending, power_granted;
    uint32_t power_serial;
    uint64_t close_sent_ns;
    uint32_t pid, serial, version, window_id;
    uint32_t minor, resize_serial, applied_serial;
    uint32_t min_width, min_height, max_width, max_height, step_width, step_height;
    uint64_t ping_sent_ns, next_ping_ns, order;
    struct ciuki_surface_info info;
    const uint32_t *pixels;
    struct desk_box frame;
    struct desk_box restore;
    struct desk_box gfx_box; /* scene source shares this origin in the client surface */
    uint32_t gfx_width, gfx_height, gfx_serial;
    int gfx_pending;
    int gfx_reply_pending, gfx_result;
    uint64_t gfx_completed_ns, gfx_next_ns;
    int gfx_timing;
    struct desk_gfx_stats gfx_stats;
    char title[DESK_TITLE_MAX + 1];
    char application[64];        /* launch-bound program identity, never supplied by a client */
    struct ciuki_message tx[DESK_TX_MAX];
    unsigned tx_head, tx_count;
};
static inline int desk_task_visible(const struct desk_client *c)
{
    return c->window && (c->version!=DESK_V3_VERSION ||
        ((c->v3_style&3)!=DESK_V3_TOOL && !c->owner_id && c->minimized!=2));
}
struct desk_ops {
    int (*send)(int, const struct ciuki_message *, uint32_t);
    int (*info)(int, struct ciuki_surface_info *);
    void *(*map)(int, int);
    int (*unmap)(void *, size_t);
    int (*close)(int);
    int (*present)(int, int, const struct ciuki_rect *);
};
struct desk_program { char name[64], path[DESK_ARG_MAX + 1]; };
/* fx-8: who asked for a launch, so that a launch completing on the launcher
 * worker reports its result the way the synchronous call did (Windows 9x
 * shell: ShellExecute returns at once, a failure is reported later). */
enum { DESK_LAUNCH_SHELL, DESK_LAUNCH_RUN, DESK_LAUNCH_GAME, DESK_LAUNCH_AUTOPLAY, DESK_LAUNCH_CLIENT };
struct desk_launch_request {
    unsigned origin;
    char label[48];
    /* DESK_LAUNCH_CLIENT: the TASK_RESULT owed to this requester, which is
     * matched again by slot, channel and PID before the late reply. */
    int client, channel, recent;
    uint32_t pid, serial, opcode;
    char argument[DESK_ARG_MAX + 1];
    char run_command[DESK_RUN_MAX]; /* internal async Run completion, never on the wire */
};
struct desk_child { uint32_t pid; uint64_t close_ns; int term_sent, close_deferred, pending; };
#define DESK_POWER_CLOSE_NS 3000000000ull
#define DESK_POWER_RETRY_NS 250000000ull
#define DESK_POWER_BUSY_NS 5000000000ull
enum { DESK_POWER_IDLE, DESK_POWER_CLOSING, DESK_POWER_RETRY,
       DESK_POWER_BUSY, DESK_POWER_FORCE, DESK_POWER_DONE };
struct desk_power_target {
    uint32_t pid;
    int owned, terminated, gone;
    char name[DESK_TITLE_MAX + 1];
};
struct desk_power_sequence {
    int state, action, window, previous_focus, prepared;
    unsigned count;
    uint64_t close_deadline, busy_deadline, next_try;
    struct desk_power_target targets[DESK_CLIENTS * 2];
    char text[320];
};
/* G4: the taskbar's window arrangement with its Undo (client.c). Windows 98
 * keeps the previous positions of the windows an arrangement moved, so that
 * the menu offers Undo Cascade, Undo Tile or Undo Minimize All until the
 * next arrangement. Windows are identified by window_id (stable across
 * slot reuse); windows that closed meanwhile are skipped by the undo. */
enum { DESK_ARRANGE_NONE, DESK_ARRANGE_CASCADE, DESK_ARRANGE_TILE_H, DESK_ARRANGE_TILE_V, DESK_ARRANGE_MINIMIZE_ALL };
struct desk_arrange_saved {
    uint32_t window_id;
    struct desk_box frame, restore;
    int32_t shade_height;
    uint8_t minimized, zoomed, collapsed;
};
struct desk_arrange_undo {
    int kind;                       /* DESK_ARRANGE_*: the operation Undo reverses, or none */
    unsigned count;
    uint32_t focus_id;              /* window_id that was active before the operation */
    struct desk_arrange_saved windows[DESK_CLIENTS];
};
/* G3 end of session (docs/design/desktop-shell.md, "End of session"):
 * Windows 9x WM_QUERYENDSESSION / WM_ENDSESSION. Shut Down, Restart and Log
 * Off ask every program in turn, topmost window first; a program may show
 * its own prompt and the session waits for the user with no deadline while
 * the program answers PINGs. A program unresponsive for DESK_HUNG_NS (the
 * Windows 9x HungAppTimeout, 5000 ms) gets "This program is not responding"
 * with End Task and Cancel; a running DOS program gets the Windows 98
 * "cannot shut down this program automatically" warning. Any veto, No or
 * Cancel ends the session with every program still running; only when all
 * agreed does desk_power_begin run the fx-5 power sequence. */
#define DESK_HUNG_NS 5000000000ull
#define DESK_SESSION_POLL_NS 20000000ull  /* Win32 broker status cadence */
enum { DESK_SESSION_IDLE, DESK_SESSION_QUERY };
enum { DESK_PART_WINDOW, DESK_PART_LEGACY, DESK_PART_WIN32 };
enum { DESK_PART_WAITING, DESK_PART_QUERIED, DESK_PART_ASKING, DESK_PART_AGREED, DESK_PART_GONE };
struct desk_session_part {
    int kind, state, answered;
    uint32_t window_id, pid, serial, ping; /* ping: first PING serial sent after the query */
    uint64_t sent_ns;
};
/* Win32 broker seam (w32srv W32R_ENDSESSION; the values are W32R_SESSION_*). */
#define DESK_W32_SESSION_BEGIN 1u
#define DESK_W32_SESSION_STATUS 4u
#define DESK_W32_SESSION_RESET 5u
#define DESK_W32_SESSION_CANCEL 6u
#define DESK_W32_SESSION_COMMIT 7u
#define DESK_W32_SESSION_HOLD 0x100u   /* BEGIN flag: wait for COMMIT/CANCEL after the queries */
#define DESK_EWX_LOGOFF 0u
#define DESK_EWX_SHUTDOWN 1u
#define DESK_EWX_REBOOT 2u
#define DESK_EWX_POWEROFF 8u
enum { DESK_W32_PENDING, DESK_W32_ENDED, DESK_W32_VETOED, DESK_W32_HELD };
struct desk_w32_status { uint32_t state, pid, hwnd, delivered; };
/* G11 (docs/design/power-policy.md): WM_POWERBROADCAST through the same
 * broker (W32R_ENDSESSION indices W32R_POWER_*). STATUS state: 0 running,
 * 1 delivered, 2 denied (BROADCAST_QUERY_DENY); pid = windows that granted. */
#define DESK_W32_POWER_BEGIN 8u
#define DESK_W32_POWER_STATUS 9u
#define DESK_W32_POWER_RESET 10u
#define DESK_POWER_QUERY_NS 20000000000ull   /* PBT_APMQUERYSUSPEND: ~20 s, then agreement */
#define DESK_PBT_QUEUE 4
enum { DESK_STANDBY_IDLE, DESK_STANDBY_QUERY, DESK_STANDBY_SUSPEND };
struct desk_pbt { uint32_t event, lparam; };
struct desk_power_broadcast {
    struct desk_pbt queue[DESK_PBT_QUEUE];
    unsigned count;
    uint32_t serial;
    int w32_running;
    uint32_t sequence_done;
    int sequence_denied;
    int running, denied;            /* denied: the last finished query */
    struct desk_pbt current, done;  /* done: the last finished broadcast (event + 1, 0 none) */
    uint64_t started_ns;
    int standby, standby_lparam;    /* DESK_STANDBY_* */
    unsigned broadcasts, standbys;
    int standby_error;
    int resume_pending;
    uint64_t resume_input_ns;
};
struct desk_session {
    int state, action;
    unsigned count, at;
    uint32_t flags, serial;
    struct desk_session_part parts[DESK_CLIENTS + 1];
    uint32_t w32_pid, w32_hwnd, w32_delivered;
    uint64_t w32_since, w32_next;
    int force;   /* G11 alarm "Force": a program that stops responding is ended */
};
/* The "This program is not responding" (End Task, Cancel) and DOS warning
 * (Yes, No) dialogs: one at a time, for a window close, End Task or the session. */
enum { DESK_HANG_NONE, DESK_HANG_NOT_RESPONDING, DESK_HANG_DOS };
struct desk_end_task {
    int kind, window, session;
    uint32_t pid, target;          /* target: the program's window id, 0 for a Win32 broker target */
    char name[DESK_TITLE_MAX + 1];
};
/* G25 accessibility glue state (access_desktop.c); the engine is struct access (access.h). */
#define DESK_ACCESS_SOURCE 0xACC0u  /* input event source of MouseKeys pointer events: not scaled by the mouse speed */
struct desk_access {
    struct access eng;
    int started;
    unsigned shown_bits;            /* tray indicator state last painted */
    int shown_visible;
    int flash_active; struct desk_box flash; uint64_t flash_until;
    /* Magnifier (docs/design/accessibility.md): the panel and what it shows */
    int mag_shown, mag_pending;
    struct access_mag_box mag_panel, mag_source;
    int mag_mx, mag_my, mag_hw; unsigned mag_shape, mag_level, mag_invert;
    uint64_t mouse_stamp, focus_stamp; int focus_cx, focus_cy;
    int mag_side;
};
struct desk_render_cache;
struct desk_clipboard;
struct desktop {
    struct desk_startup_gate startup;
    struct desk_clipboard *clipboard;
    void (*wake)(void *);
    void *wake_ctx;
    uint64_t drain_ns;  /* f2-88: when the loop last drained every client channel (0: not tracked, host tests) */
    unsigned games_removed;
    int (*install_program)(struct desktop *,const char *);
    int (*open_path)(struct desktop *,const char *); /* G8, shared with Files and G9 */
    struct desk_ops ops;
    int display, input, output, focus, drag;
    int v3_pointer_window; /* slot + 1, zero means no capture/clip */
    unsigned v3_cursor_id, v3_cursor_slot, v3_cursor_hidden;
    uint32_t v3_lease_window, v3_lease_generation, v3_lease_state, v3_lease_pid;
    uint32_t v3_lease_w, v3_lease_h, v3_lease_bpp;
    struct desk_lease_tuple v3_lease_tuple;
    uint32_t v3_lease_preferred_mode;
    int v3_lease_error, v3_reactivate_pending;
    uint64_t v3_lease_started, v3_restore_started, v3_restore_presents;
    unsigned v3_restoring, v3_restore_reported;
    uint32_t v3_restore_watchdog_ms, v3_restore_watchdog_pending, v3_restore_watchdog_reported;
    void (*v3_lease_poll)(struct desktop *);
    /* f5-08 adapter: choose/set mode + delegate atomically; revoke synchronously
     * and restore/repaint desktop before returning. No grant/fb mapping to peer. */
    int (*v3_lease)(struct desktop *, uint32_t pid, int acquire,
                    uint32_t w, uint32_t h, uint32_t bpp, uint32_t generation);
    int (*v3_restart)(struct desktop *, uint32_t pid, uint32_t flags);
    uint32_t v3_dialog_pid, v3_restart_flags;
    int v3_dialog_kind; /* 1 crash, 2 restart confirmation */
    int v3_details_open;
    char v3_details[640];
    int (*v3_fault_close)(uint32_t);
    uint32_t width, height, *pixels;
    const uint32_t *portrait;
    uint64_t saver_idle, saver_frame, saver_poll;
    unsigned saver_active, saver_kind, saver_sequence, saver_reported;
    int monitor_off, monitor_failed, saver_external_running, saver_display_inhibited;
    int (*screen_control)(struct ciuki_screen_control *);
    int (*monitor_power)(int);

    /* Shell icons (desktop.c: libdeskui's Tango store, icon_map.c keys such as
     * "program:files"); NULL draws none. */
    const uint32_t *(*icon)(const char *object, unsigned size);
    int32_t mouse_x, mouse_y, drag_x, drag_y;
    unsigned resize_edges;
    struct desk_box resize_start;
    int keyboard_geometry;       /* 1 move, 2 size; drag identifies the window */
    int geometry_zoomed;
    struct desk_box geometry_restore;
    unsigned cascade;
    unsigned usb_wheel_records;
    struct {
        struct desk_box from, to, box;
        uint64_t started;
        unsigned step;
        int active;
    } animation;
    uint8_t keys[513];
    uint32_t buttons, routed_buttons;
    uint64_t order, now_ns, clock_second, presents, inputs, received, replied;
    void (*input_pending)(void); /* independent desktop stall observer */
    struct desk_damage damage;
    struct desk_damage present_damage;
    struct desk_render_cache *render_cache;
    int render_cache_prepared;
    uint64_t chrome_rebuilds;
    int frame_valid, scanout_valid;
    struct desk_damage move_damage;
    /* fx-2: the stacking the composed frame shows (window id per slot, 0
     * when hidden, and its order). desk_stack_damage reconciles it: a
     * changed relative order invalidates the pair's overlap. */
    uint32_t stack_window[DESK_CLIENTS]; uint64_t stack_order[DESK_CLIENTS];
    /* fx-15: what the last composed frame showed of every window, task
     * button and shell popup (desk_scene_reconcile): a change of any of
     * them, through whatever path, invalidates the old and the new pixels
     * before the next frame (Windows 9x: ShowWindow/SetWindowPos/
     * SetWindowText/activation invalidate what they change). */
    struct desk_shown {
        uint32_t window_id; int shown; struct desk_box outer;
        const uint32_t *icon; unsigned buttons, hot, pressed; int active, alert, collapsed, maximized;
        char title[DESK_TITLE_MAX + 1];
    } shown[DESK_CLIENTS];
    struct desk_task_shown { struct desk_box box; const char *icon; unsigned flags; char title[DESK_TITLE_MAX + 1]; } task_shown[DESK_CLIENTS];
    struct desk_box popup_shown[SH_LEVELS + 6];
    struct desk_box tip_shown; uint32_t tip_shown_hash;
    struct desk_box outline_shown; int outline_icon, about_windows;
    struct desk_box band_shown;  /* fx-15b: the rubber band as last composed (zero: none) */
    struct { int x, y; uint64_t until; } pointer_trail[7];
    unsigned pointer_trail_count;
    unsigned layout_alt, layout_shift, layout_chord, text_dead;
    unsigned text_scan, text_caps, text_routed;
    uint32_t input_saved[DESK_INPUT_VALUES], input_volatile_mask;
    unsigned input_change_key,input_change_flags;
    unsigned shell_caret_hidden;uint64_t shell_caret_next;
    uint32_t cursor_under[64*64]; struct desk_box cursor_box;
    int cursor_valid, cursor_dirty;
    unsigned cursor_shape, cursor_loaded;
    unsigned lfb_bytes_per_pixel;
    uint64_t next_frame_ns;
    uint64_t last_frame_ns, last_input_ns; /* f2-95: last completed frame's start, last input */
    struct desk_gfx_stats gfx_sample; /* most recently completed demo frame */
    uint64_t pending_input_ns, pending_input_lag_ns, frame_input_ns;
    uint64_t latency_buckets[DESK_LATENCY_BUCKETS];
    uint64_t (*time_ns)(void);
    struct desk_frame_stats frame_stats, stats;
    struct desk_frame_stats sample; uint64_t sample_ns, sample_begin_ns;
    struct desk_client clients[DESK_CLIENTS];
    int shell, menu_open, menu_focus, about, action, shell_test, shell_stage;
    uint32_t shell_pid, shell_keys;
    /* f2-31/f2-32 app scenarios (app_test is DESK_APP_FILES or DESK_APP_NOTE):
     * the program's checkpoints are PINGs with serial DESK_APP_CHECKPOINT+n. */
    int app_test, app_checkpoint, app_reported, app_done;
    /* G14 (recovery.c): --safe and --shell=terminal from the supervisor. */
    int safe_mode, terminal_shell;
    uint32_t app_pid;
    const char *launch_arg;      /* single argument for the next launch (LAUNCH from Files) */
    int (*launch)(struct desktop *, const struct desk_program *);
    uint64_t launch_click_ns;
    /* fx-8: launch_async: spawns run on the launcher worker (never in gate
     * scenarios); launch_request: the next launch's origin, reset after it;
     * launch_deferred: the last launch returned 0 and completes later. */
    int launch_async, launch_deferred;
    int help_starting;
    char help_pending[DESK_ARG_MAX+1];
    uint32_t help_users[DESK_CLIENTS];
    struct desk_launch_request launch_request;
    int (*launch_mark)(uint32_t event, ciuki_pid_t pid, uint64_t click_us);
    /* fx-15: a launched program's first frame reached the screen (quiet
     * boots too): click, process, window, first frame and present times. */
    void (*first_frame)(struct desktop *, const struct desk_client *, uint64_t presented_ns);
    int (*terminate)(uint32_t, int);
    uint64_t realtime_second;
    int realtime_source;
    /* G1: the system time zone (syscall 97): local = realtime_second +
     * local_offset; tz_env is the TZ handed to launched programs. */
    int32_t local_offset;
    uint32_t time_flags, time_generation;
    char tz_env[8 + CIUKI_TIME_RULE_MAX];
    char build_identity[65], version[65], notice[64];
    struct desk_link_entry *v3_links;
    unsigned v3_link_count;
    int launch_show;              /* scoped by the shortcut launcher */
    int (*drop_snapshot)(struct desktop *); /* private read-only drag surface */
    uint64_t v3_links_next;
    /* f2-95: DESK_LINKS_SCAN asks the shortcut worker for a scan at the next
     * snapshot, DESK_LINKS_GAMES also for the bundled-games query. Without a
     * request the worker rescans every 10 s, never on every 2 s snapshot. */
    unsigned v3_links_force;
    /* Enumerate a fresh bounded snapshot; read source again before launch. */
    int (*v3_links_list)(struct desktop *, struct desk_link_entry *, unsigned);
    int (*v3_link_read)(struct desktop *, const char *, struct desk_launch_record *);
    int (*v3_link_save)(struct desktop *, const char *, const struct desk_launch_record *);
    const uint32_t *(*v3_link_icon)(struct desktop *, const char *, unsigned);
    int (*v3_launch)(struct desktop *, const struct desk_launch_record *);
    struct desk_program programs[DESK_MENU_MAX];
    unsigned program_count;
    int pictures_exists;        /* runtime stat of the owner's /Documents/Pictures */
    struct desk_child children[DESK_CLIENTS];
    /* f2-33: task-manager extension and its runtime-selected gate scenario. */
    uint32_t next_window_id, self_pid;
    int authority_fd, tasks_test, tasks_stage, tasks_checkpoint;
    uint32_t tasks_pid, tasks_demo_pid, tasks_demo_window;
    /* Desktop shell (shell.h): settings, menus, icons, tray, built-in windows. */
    int mixer_fd;                /* delegated /dev/mixer control handle (CIUKI_MIXER_FD) or -1 */
    int media_fd, network_fd, mixer_observer_fd; /* lifetime O_CLOEXEC observers, or -1 */
    const char *launch_argv[6];  /* Run dialog: extra arguments for the next launch */
    const struct fm_vfs *autoplay_fs;
    int (*autoplay_preference)(struct desktop *, const char *id, const char *label, int action);
    void (*media_sound)(struct desktop *, int connected);
    struct desk_shell sh;
    uint64_t resources_next_ns;     /* G30: next Resource Meter sample (resources.c) */
    struct desk_schedsvc *schedsvc; /* G30: scheduled tasks service (schedsvc.c), or NULL */
    struct desk_access acc;
    struct desk_power_sequence power;
    struct desk_session session;   /* G3: the end-of-session queries before power */
    struct desk_end_task end_task;
    /* G3: the Win32 end-session broker; NULL or an error: no Win32 programs to ask. */
    int (*w32_session)(struct desktop *, unsigned action, uint32_t flags, struct desk_w32_status *);
    /* G11: WM_POWERBROADCAST (action DESK_W32_POWER_*, event = PBT_*), and
     * call 73 STANDBY (0 or -1 with errno). NULL: none. */
    int (*w32_power)(struct desktop *, unsigned action, uint32_t event, uint32_t lparam, struct desk_w32_status *);
    int (*power_standby)(void);
    int (*power_cancel_standby)(void);
    struct desk_power_broadcast pbt;
    /* Shutdown syscall seams; elapsed time is d->now_ns. alive: 0 gone,
     * 1 still present, -1 unknown (never infer exit from an IPC disconnect). */
    void (*sound_event)(struct desktop *, unsigned event);
    /* sound_request: 0/-errno, or DESK_SOUND_PENDING for a deferred reply. */
    int (*sound_request)(struct desktop *, int client, unsigned request, unsigned event, uint32_t serial);
    int (*sound_shutdown)(struct desktop *); /* 1 complete, 0 still within the 2 s allowance */
    void (*sound_cancel)(struct desktop *);
    void (*sound_tone)(struct desktop *, unsigned tone);   /* G25: a synthesised accessibility tone (access.h ACC_TONE_*) */
    int (*power_request)(int action);
    int (*power_state)(void); /* query capabilities and irreversible progress */
    void (*power_prepare)(struct desktop *);
    void (*logoff)(struct desktop *); /* G32: called after G3 has closed the session */
    int (*power_alive)(struct desktop *, uint32_t pid);
    /* Volume Control requests (DESK_MIXER_SET): applied with the delegated mixer
     * control handle; NULL on hosts without one. 0 or -errno. */
    int (*joystick_control)(struct desktop *, struct ciuki_joystick_control *);
    int (*mixer_set)(struct desktop *, uint32_t left, uint32_t right, uint32_t mute);
    int (*mixer_line)(struct desktop *, const struct ciuki_mixer_line *);
    /* Title-bar double-click (maximize and restore). */
    uint64_t title_click_ns; int title_click_window; int32_t title_click_x, title_click_y;
    /* G4: the caption icon's double click closes (WM-11); the taskbar's Undo record. */
    uint64_t icon_click_ns; int icon_click_window;
    struct desk_arrange_undo arrange;
    /* Title buttons (desk_title_code): under the pointer, and pressed with the left button. */
    int title_hot, title_down;
    uint32_t display_counter;
    uint16_t display_mode, display_pending;
    int (*display_set)(struct desktop *, uint16_t);
    int display_reply_client; uint32_t display_reply_pid, display_reply_serial;
    uint16_t display_reply_mode; int display_reply_preview;
    /* Optional privileged presenter; NULL retains ordinary surface presents. */
    int (*accel)(int, const struct ciuki_accel_request *);
    int (*accel_info)(int, uint32_t, struct ciuki_accel_info *);
    int (*engine_info)(int, uint32_t, struct ciuki_display_engine_info *);
    int (*commands)(int, uint32_t, struct ciuki_command_info *);
    int (*batch)(int, const void *, uint32_t, struct ciuki_batch_reply *);
    int (*fence)(int, uint64_t, uint16_t, uint32_t, struct ciuki_batch_reply *);
    uint32_t command_caps, colour_cursor_width, colour_cursor_height;
    uint64_t recovering_fence;
    int hw_colour_cursor;
    uint32_t command_native, command_video_row;
    int command_video_valid;
    struct ciuki_framebuffer_info command_format;
    struct desk_box command_old_box;
    uint32_t *command_old_pixels;
    int (*display_info)(int, struct ciuki_framebuffer_info *);
    int (*gfx_benchmark)(struct desktop *, unsigned, uint64_t result[3]);
    uint64_t gfx_next_bench_ns;
    uint32_t accel_generation, accel_caps;
    /* hw_cursor: the hardware cursor is loaded and owns the pointer.
     * f2-96: hw_cursor_shown: the hardware cursor may be visible (SHOW 1
     * succeeded and no CURSOR_SET, SHOW 0, engine loss or new generation
     * hid it since). The software pointer is drawn only when both are 0,
     * so the two are never on screen together. Busy/stale answers keep the
     * owner and retry next frame; other errors hide the hardware cursor and
     * fall back to software; a software pointer is offered back to the
     * engine at most once per DESK_POINTER_RETRY_NS. */
    int hw_cursor, hw_cursor_shown;
    uint64_t hw_cursor_retry_ns;
    unsigned hw_cursor_retries, hw_cursor_fallbacks;
    int32_t cursor_x, cursor_y;
    /* Clean moves copy pixels visible at both endpoints; other exposure is damage. */
    int copy_pending, copy_client;
    struct ciuki_rect copy_rect;
    /* f2-92: offscreen rows [vram_first, vram_end), their surface caches,
     * the next window stability check and an engine lost while in use. */
    uint32_t vram_first, vram_end, vram_queries, vram_failures;
    struct desk_vram_entry vram[DESK_VRAM_ENTRIES];
    uint64_t vram_check_ns, vram_query_ns;
    int accel_lost;
};
/* Worker publication only; never inspect live shell state from a worker. */
static inline void desk_wake(struct desktop *d)
{ if (d && d->wake) d->wake(d->wake_ctx); }

/* f2-33 tasks scenario stages: 1 Tasks launched from the menu, 2 its window,
 * 3 demo launched through LAUNCH, 4 demo window, 5 End Task requested,
 * 6 demo reaped with status 0, 7 Tasks reported the PID gone and the
 * desktop's own process list confirmed it (desktop.c). */
#define DESK_TASKS_DONE 7
int desk_intersect(struct desk_box a, struct desk_box b, struct desk_box *out);
struct desk_box desk_frame(uint32_t w, uint32_t h, int32_t x, int32_t y);
struct desk_box desk_content(const struct desk_client *c);
struct desk_box desk_close_box(const struct desk_client *c);
struct desk_box desk_caption_icon_box(const struct desktop *, const struct desk_client *c);   /* zero when the theme draws none */
struct desk_box desk_help_box(const struct desk_client *c);   /* G15: empty without DESK_V3_CONTEXTHELP */
void desk_context_help(struct desktop *, int index);          /* G15: HELP to a v3 client */
struct desk_box desk_min_box(const struct desk_client *c);
struct desk_box desk_shade_box(const struct desk_client *c);   /* zoom; zero box when too narrow */
struct desk_box desk_outer(const struct desk_client *c);       /* frame plus drop shadow */
void desk_collapse(struct desktop *, int index);                /* toggle windowshade */
void desk_theme_changed(struct desktop *);                       /* repaint after desk_theme_use */
struct desk_box desk_taskbar(const struct desktop *);
struct desk_box desk_start_button(const struct desktop *);       /* top bar, left end */
struct desk_box desk_start_bounds(const struct desktop *);       /* includes soft shadow */
int desk_start_contains(const struct desktop *, int32_t, int32_t);
int desk_shell_chrome_intersects(const struct desktop *, struct desk_box);
int desk_title_code(const struct desktop *, int32_t x, int32_t y);   /* title button under (x, y): index*4+k+1, or 0 */
struct desk_box desk_title_code_box(const struct desktop *, int code);
struct desk_box desk_task_button(const struct desktop *, int index);
struct desk_box desk_menu_box(const struct desktop *);
int32_t desk_menu_row(const struct desktop *);
int desk_contains(struct desk_box, int32_t, int32_t);
int desk_hit(const struct desktop *, int32_t, int32_t);
int desk_program_add(struct desktop *, const char *name, const uint8_t *elf, size_t);
int desk_program_graphical(const char *name);
int desk_ide_launch(struct desktop *, const char *path);
const char *desk_program_label(const char *name);
const char *desk_program_description(const char *name);   /* catalog.h application record, or NULL */
const char *desk_app_program(int kind);          /* "files" / "ciuknote" */
int desk_program_kind(const char *name);
int desk_catalog_takes_file(const char *name);   /* catalog.h accessories that open a file argument */         /* 1 Files, 2 CiukNote, else 0 */
void desk_shell_enable(struct desktop *);
void desk_minimize(struct desktop *, int index);
/* G4: Windows 98 taskbar arrangements (client.c). Each acts on the windows that
 * have a task button, are not minimised, owned, built-in dialogs or topmost
 * (cascade and tile), inside the work area between the two bars; the result
 * is the number of windows moved. desk_arrange_undo reverses the last one. */
int desk_arrangeable(const struct desktop *, int index);
int desk_minimizable(const struct desktop *, int index);   /* has a task button, is shown and not owned */
int desk_arrange(struct desktop *, int kind);   /* DESK_ARRANGE_CASCADE, _TILE_H, _TILE_V */
int desk_minimize_all(struct desktop *);        /* Win+M, Minimize All Windows */
int desk_show_desktop(struct desktop *);        /* Win+D: minimise all, again restores */
int desk_arrange_undo_kind(const struct desktop *);   /* DESK_ARRANGE_* the Undo item reverses, 0 none */
int desk_arrange_undo(struct desktop *);        /* Shift+Win+M, Undo Cascade/Tile/Minimize All */
void desk_request_close(struct desktop *, int index);
void desk_close_timeouts(struct desktop *);
void desk_child_exited(struct desktop *, uint32_t pid, int status);
void desk_session_begin(struct desktop *, int action);   /* 1 restart, 2 shut down, 3 log off */
/* G11: queue a WM_POWERBROADCAST to every Win32 window (coalesces status
 * changes); desk_power_policy_tick drives the queue, the Stand by sequence
 * and the idle standby timer (shell.c, from desk_shell_tick). */
void desk_power_reply(struct desktop *, int client, uint32_t serial, uint32_t answer);
void desk_power_broadcast(struct desktop *, uint32_t event, uint32_t lparam);
void desk_power_policy_tick(struct desktop *);
void desk_session_tick(struct desktop *);
void desk_session_cancel(struct desktop *);
uint64_t desk_session_deadline(const struct desktop *);  /* next session/hung-close turn, 0 none */
int desk_session_busy(const struct desktop *);             /* a session query or the power sequence runs */
void desk_end_task_open(struct desktop *, int kind, int session, uint32_t pid, uint32_t target, const char *name);
void desk_end_task_choice(struct desktop *, int end);       /* 1 End Task / Yes, 0 Cancel / No */
void desk_power_begin(struct desktop *, int action);
void desk_power_tick(struct desktop *);
void desk_power_cancel(struct desktop *);
void desk_power_force(struct desktop *);
int desk_power_input(struct desktop *, const struct ciuki_input_event *);
void desk_about(struct desktop *);
void desk_task_windows(struct desktop *, int index, uint32_t serial);
int desk_task_end(struct desktop *, uint32_t window_id, uint32_t pid);
int desk_task_switch(struct desktop *, uint32_t window_id);
int desk_help_argument(const char *argument);   /* G15: LAUNCH help from any client */
/* Launch results: a PID, -errno, or 0 when d->launch_async queued the spawn
 * (fx-8); desk_launch_completed reports the late result to its origin. */
int desk_task_launch(struct desktop *, const char *name, const char *argument);
/* G14 startup recovery (recovery.c; weak where host tests link without it). */
void desk_recovery_init(struct desktop *, int safe, int terminal);
void desk_recovery_start(struct desktop *);
void desk_recovery_tick(struct desktop *);
void desk_recovery_paint(struct desktop *, struct desk_box clip);
int desk_recovery_win(struct desktop *);
int desk_recovery_startup_allowed(const struct desktop *);
void desk_launch_origin(struct desktop *, unsigned origin, const char *label);
void desk_launch_completed(struct desktop *, const struct desk_launch_request *, const char *name, int result);
/* Launch with several arguments after the desktop's own (Programs > Games: /Programs/dosbox). */
int desk_task_launch_args(struct desktop *, const char *name, const char *const *args, unsigned count);
/* Programs > Games entry index; kernel NULL keeps the game's saved setting. */
int desk_launch_game(struct desktop *, int index, const char *kernel);
void desk_game_properties(struct desktop *,unsigned game);
void desk_game_data_result(struct desktop *,unsigned game,unsigned op,int error,const struct ciuki_dosvm_command *);
int desk_shell_click(struct desktop *);
void desk_route_key(struct desktop *, int code, int value);
void desk_shell_draw(struct desktop *, struct desk_box);
void desk_damage_add(struct desk_damage *, struct desk_box, uint32_t, uint32_t);
int desk_client_damage(struct desktop *, int, struct desk_box);
void desk_stack_damage(struct desktop *);
void desk_scene_reconcile(struct desktop *); /* fx-15: before every frame decision and in desk_redraw */
void desk_caption_damage(struct desktop *, int);
unsigned desk_shell_occluders(const struct desktop *, struct desk_box *); /* SH_LEVELS+6 slots */
/* G25: SoundSentry and ShowSounds follow every desktop sound (access_desktop.c; weak: tests may omit it). */
/* G25 accessibility glue (access_desktop.c). Hosts that omit it get NULL from the weak references. */
void desk_access_tick(struct desktop *);                   /* every service turn: SlowKeys, hot-key holds, MouseKeys, time-out, flash */
uint64_t desk_access_wake(const struct desktop *, uint64_t now_ns);   /* next deadline, 0 none */
void desk_access_settings_changed(struct desktop *);       /* desktop.cfg was reloaded or edited */
void desk_access_sound_cue(struct desktop *, unsigned event) __attribute__((weak));
static inline void desk_sound(struct desktop *d, unsigned e)
{ if (d->sound_event) d->sound_event(d,e); if (desk_access_sound_cue) desk_access_sound_cue(d,e); }
void desk_init(struct desktop *, const struct desk_ops *, uint32_t, uint32_t);
int desk_v3_receive(struct desktop *, int, const struct ciuki_message *);
void desk_v3_revoke(struct desktop *);
void desk_v3_pointer_release(struct desktop *);
void desk_v3_restack(struct desktop *);
void desk_v3_reactivate(struct desktop *, int);
void desk_v3_lease_failed(struct desktop *, int error);
int desk_v3_restore_watchdog(struct desktop *,uint32_t now_ms,char *,size_t);
int desk_v3_mode(const struct ciuki_display_info *, unsigned w, unsigned h, unsigned bpp);
int desk_v3_dialog_accept(struct desktop *);
struct ciuki_w32_fault;
void desk_w32_fault_dialog(struct desktop *, const struct ciuki_w32_fault *);
void desk_links_refresh(struct desktop *);
int desk_link_launch(struct desktop *, unsigned);
int desk_ignore_sigpipe(void);
int desk_add_client(struct desktop *, int channel);
void desk_drop_client(struct desktop *, int index);
void desk_receive(struct desktop *, int index, const struct ciuki_message *);
void desk_queue(struct desktop *, int index, const struct ciuki_message *);
void desk_flush(struct desktop *, int index);
void desk_heartbeat(struct desktop *, uint64_t now_ns);
void desk_focus(struct desktop *, int index);
int desk_modal_target(const struct desktop *, int index);
int desk_next_window(const struct desktop *, int excluded, uint32_t pid, const char *application);
void desk_preferences_changed(struct desktop *);
void desk_preferences_persistent(const struct desktop *, struct ciuki_settings *);
void desk_cursor_update(struct desktop *);
void desk_geometry_begin(struct desktop *, int index, int size);
void desk_geometry_end(struct desktop *, int cancel);
void desk_animate(struct desktop *, struct desk_box from, struct desk_box to);
void desk_animation_tick(struct desktop *);
void desk_move(struct desktop *, int index, int32_t x, int32_t y);
void desk_resize(struct desktop *, int index, struct desk_box frame);
enum { DESK_DISPLAY_NO_FRAME, DESK_DISPLAY_PERSIST_FRAME, DESK_DISPLAY_PREVIEW_FRAME };
int desk_display_frame_commit(struct desktop *, int redraw_result);
void desk_display_relayout(struct desktop *, uint32_t width, uint32_t height);
void desk_zoom(struct desktop *, int index);
unsigned desk_resize_hit(const struct desk_client *, int32_t x, int32_t y);
void desk_saver_config(struct desktop *);
void desk_saver_tick(struct desktop *);
int desk_saver_start(struct desktop *, unsigned kind);
int desk_saver_input(struct desktop *, const struct ciuki_input_event *);
void desk_input(struct desktop *, const struct ciuki_input_event *);
void desk_input_unfiltered(struct desktop *, const struct ciuki_input_event *);   /* G25: after the accessibility filters */
int desk_drain_input(struct desktop *, int (*read_events)(int, struct ciuki_input_event *, uint32_t));
void desk_composite(struct desktop *, struct desk_box);
void desk_accel_reset(struct desktop *, uint32_t generation);
/* f2-92: one bounded offscreen-cache maintenance step (upload or check);
 * 1 when work remains. Call between frames. */
int desk_accel_cache_step(struct desktop *, uint64_t now);
void desk_cursor_motion(struct desktop *);
void desk_text(struct desktop *, struct desk_box, int32_t, int32_t,
               const char *, int, uint32_t);
int desk_redraw(struct desktop *);
void desk_render_release(struct desktop *);
int desk_frame_due(const struct desktop *, uint64_t now);
int desk_small_damage(const struct desktop *, uint64_t now);
uint64_t desk_frame_time(const struct desktop *, uint64_t now);
void desk_gfx_complete(struct desktop *, int index, int result, uint64_t completed, uint64_t next);
void desk_stats_sample(struct desktop *);
void desk_display_reply(struct desktop *, int result);
uint64_t desk_launch_wait(const struct desktop *, uint64_t now); /* ns; short startup polling only */
int desk_load_portrait(uint32_t *pixels);
#endif
