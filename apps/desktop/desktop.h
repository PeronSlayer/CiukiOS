/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_APP_DESKTOP_H
#define CIUKI_APP_DESKTOP_H
#include "protocol.h"
#include "theme.h"
#include <stddef.h>
#define DESK_CLIENTS 16
#define DESK_DAMAGE_MAX 32
#define DESK_TX_MAX 64
#define DESK_BAR 32
#define DESK_BORDER DESK_FRAME_BORDER   /* theme.h: one frame geometry for windows and dialogs */
#define DESK_TITLE DESK_FRAME_TITLE
#define DESK_RESIZE_MARGIN 7
#define DESK_EDGE_LEFT 1u
#define DESK_EDGE_RIGHT 2u
#define DESK_EDGE_TOP 4u
#define DESK_EDGE_BOTTOM 8u
/* The Ciuki (Start) button at the left end of the top bar; the Start menu
 * drops from it. Task buttons start DESK_TASK_X from the taskbar's left edge. */
#define DESK_START_X 4
#define DESK_START_Y 4
#define DESK_START_W 96
#define DESK_START_H 24
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
};
#define DESK_FRAME_NS 16666667ull /* VBE does not expose measured refresh; conservative 60 Hz */
struct desk_client {
    int channel, surface, hello, window, unresponsive, ping_pending, resync_pending;
    int configure_pending, focus_pending, close_pending;
    int commit_pending, resizable, zoomed;
    int minimized, internal, closing, program;   /* internal: DESK_INTERNAL_* kind; program: 1 Files, 2 CiukNote, 3 Control Panel */
    int collapsed;                /* windowshade: only the title bar shows (double-click) */
    int32_t shade_height;
    int taskman;                  /* f2-33: launched /Programs/tasks; may send task requests */
    int mixer;                    /* launched Volume Control or Music Player; may send MIXER_SET */
    int awaiting_frame;           /* launched graphical program that has not sent its first DAMAGE yet */
    int launch_frame_pending;    /* first DAMAGE accepted, awaiting successful desktop present */
    uint64_t launched_ns;         /* its launch time (desktop.c: the gate scenarios' input hold) */
    uint32_t pid, serial, version, window_id;
    uint32_t minor, resize_serial, applied_serial;
    uint32_t min_width, min_height, max_width, max_height, step_width, step_height;
    uint64_t ping_sent_ns, next_ping_ns, order;
    struct ciuki_surface_info info;
    const uint32_t *pixels;
    struct desk_box frame;
    struct desk_box restore;
    char title[DESK_TITLE_MAX + 1];
    struct ciuki_message tx[DESK_TX_MAX];
    unsigned tx_head, tx_count;
};
struct desk_ops {
    int (*send)(int, const struct ciuki_message *, uint32_t);
    int (*info)(int, struct ciuki_surface_info *);
    void *(*map)(int, int);
    int (*unmap)(void *, size_t);
    int (*close)(int);
    int (*present)(int, int, const struct ciuki_rect *);
};
struct desk_program { char name[64], path[DESK_ARG_MAX + 1]; };
struct desk_child { uint32_t pid; uint64_t close_ns; int term_sent; };
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
struct desktop {
    struct desk_ops ops;
    int display, input, output, focus, drag;
    uint32_t width, height, *pixels;
    const uint32_t *portrait;
    /* Shell icons (desktop.c: libdeskui's Tango store, icon_map.c keys such as
     * "program:files"); NULL draws none. */
    const uint32_t *(*icon)(const char *object, unsigned size);
    int32_t mouse_x, mouse_y, drag_x, drag_y;
    unsigned resize_edges;
    struct desk_box resize_start;
    uint8_t keys[513];
    uint32_t buttons, routed_buttons;
    uint64_t order, now_ns, clock_second, presents, inputs, received, replied;
    struct desk_damage damage;
    struct desk_damage present_damage;
    uint32_t *scene; size_t scene_bytes;
    uint32_t cursor_under[11*17]; struct desk_box cursor_box;
    int cursor_valid, cursor_dirty;
    unsigned lfb_bytes_per_pixel;
    uint64_t next_frame_ns;
    uint64_t pending_input_ns, pending_input_lag_ns;
    uint64_t (*time_ns)(void);
    struct desk_frame_stats frame_stats, stats;
    struct desk_frame_stats sample; uint64_t sample_ns, sample_begin_ns;
    struct desk_client clients[DESK_CLIENTS];
    int shell, menu_open, menu_focus, about, action, shell_test, shell_stage;
    uint32_t shell_pid, shell_keys;
    /* f2-31/f2-32 app scenarios (app_test is DESK_APP_FILES or DESK_APP_NOTE):
     * the program's checkpoints are PINGs with serial DESK_APP_CHECKPOINT+n. */
    int app_test, app_checkpoint, app_reported, app_done;
    uint32_t app_pid;
    const char *launch_arg;      /* single argument for the next launch (LAUNCH from Files) */
    int (*launch)(struct desktop *, const struct desk_program *);
    uint64_t launch_click_ns;
    int (*launch_mark)(uint32_t event, ciuki_pid_t pid, uint64_t click_us);
    int (*terminate)(uint32_t, int);
    uint64_t realtime_second;
    int realtime_source;
    char build_identity[65], version[65], notice[64];
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
    const char *launch_argv[6];  /* Run dialog: extra arguments for the next launch */
    const struct fm_vfs *autoplay_fs;
    int (*autoplay_preference)(struct desktop *, const char *id, const char *label, int action);
    void (*media_sound)(struct desktop *, int connected);
    struct desk_shell sh;
    struct desk_power_sequence power;
    /* Shutdown syscall seams; elapsed time is d->now_ns. alive: 0 gone,
     * 1 still present, -1 unknown (never infer exit from an IPC disconnect). */
    void (*sound_event)(struct desktop *, unsigned event);
    /* sound_request: 0/-errno, or DESK_SOUND_PENDING for a deferred reply. */
    int (*sound_request)(struct desktop *, int client, unsigned request, unsigned event, uint32_t serial);
    int (*sound_shutdown)(struct desktop *); /* 1 complete, 0 still within the 2 s allowance */
    void (*sound_cancel)(struct desktop *);
    int (*power_request)(int action);
    void (*power_prepare)(struct desktop *);
    int (*power_alive)(struct desktop *, uint32_t pid);
    /* Volume Control requests (DESK_MIXER_SET): applied with the delegated mixer
     * control handle; NULL on hosts without one. 0 or -errno. */
    int (*mixer_set)(struct desktop *, uint32_t left, uint32_t right, uint32_t mute);
    /* Title-bar double-click (windowshade). */
    uint64_t title_click_ns; int title_click_window; int32_t title_click_x, title_click_y;
    /* Title buttons (desk_title_code): under the pointer, and pressed with the left button. */
    int title_hot, title_down;
    uint32_t display_counter;
    uint16_t display_mode, display_pending;
    int (*display_set)(struct desktop *, uint16_t);
    int display_reply_client; uint32_t display_reply_pid, display_reply_serial;
    uint16_t display_reply_mode;
    /* Optional privileged presenter; NULL retains ordinary surface presents. */
    int (*accel)(int, const struct ciuki_accel_request *);
    uint32_t accel_generation, accel_caps;
    int hw_cursor;
    int32_t cursor_x, cursor_y;
    uint32_t *accel_front; /* last completed RAM frame; allocated only with rectangles */
    int accel_front_valid;
    /* A move can copy only a clean, unobscured window. Other exposure is damage. */
    int copy_pending, copy_client;
    struct ciuki_rect copy_rect;
};
/* f2-33 tasks scenario stages: 1 Tasks launched from the menu, 2 its window,
 * 3 demo launched through LAUNCH, 4 demo window, 5 End Task requested,
 * 6 demo reaped with status 0, 7 Tasks reported the PID gone and the
 * desktop's own process list confirmed it (desktop.c). */
#define DESK_TASKS_DONE 7
int desk_intersect(struct desk_box a, struct desk_box b, struct desk_box *out);
struct desk_box desk_frame(uint32_t w, uint32_t h, int32_t x, int32_t y);
struct desk_box desk_content(const struct desk_client *c);
struct desk_box desk_close_box(const struct desk_client *c);
struct desk_box desk_min_box(const struct desk_client *c);
struct desk_box desk_shade_box(const struct desk_client *c);   /* zoom; zero box when too narrow */
struct desk_box desk_outer(const struct desk_client *c);       /* frame plus drop shadow */
void desk_collapse(struct desktop *, int index);                /* toggle windowshade */
void desk_theme_changed(struct desktop *);                       /* repaint after desk_theme_use */
struct desk_box desk_taskbar(const struct desktop *);
struct desk_box desk_start_button(const struct desktop *);       /* top bar, left end */
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
const char *desk_app_program(int kind);          /* "files" / "ciuknote" */
int desk_program_kind(const char *name);
int desk_catalog_takes_file(const char *name);   /* catalog.h accessories that open a file argument */         /* 1 Files, 2 CiukNote, else 0 */
void desk_shell_enable(struct desktop *);
void desk_minimize(struct desktop *, int index);
void desk_request_close(struct desktop *, int index);
void desk_close_timeouts(struct desktop *);
void desk_child_exited(struct desktop *, uint32_t pid, int status);
void desk_power_begin(struct desktop *, int action);
void desk_power_tick(struct desktop *);
void desk_power_cancel(struct desktop *);
void desk_power_force(struct desktop *);
int desk_power_input(struct desktop *, const struct ciuki_input_event *);
void desk_about(struct desktop *);
void desk_task_windows(struct desktop *, int index, uint32_t serial);
int desk_task_end(struct desktop *, uint32_t window_id, uint32_t pid);
int desk_task_switch(struct desktop *, uint32_t window_id);
int desk_task_launch(struct desktop *, const char *name, const char *argument);
/* Launch with several arguments after the desktop's own (Programs > Games: /Programs/dosbox). */
int desk_task_launch_args(struct desktop *, const char *name, const char *const *args, unsigned count);
int desk_shell_click(struct desktop *);
void desk_route_key(struct desktop *, int code, int value);
void desk_shell_draw(struct desktop *, struct desk_box);
void desk_damage_add(struct desk_damage *, struct desk_box, uint32_t, uint32_t);
static inline void desk_sound(struct desktop *d, unsigned e) { if (d->sound_event) d->sound_event(d,e); }
void desk_init(struct desktop *, const struct desk_ops *, uint32_t, uint32_t);
int desk_ignore_sigpipe(void);
int desk_add_client(struct desktop *, int channel);
void desk_drop_client(struct desktop *, int index);
void desk_receive(struct desktop *, int index, const struct ciuki_message *);
void desk_queue(struct desktop *, int index, const struct ciuki_message *);
void desk_flush(struct desktop *, int index);
void desk_heartbeat(struct desktop *, uint64_t now_ns);
void desk_focus(struct desktop *, int index);
void desk_move(struct desktop *, int index, int32_t x, int32_t y);
void desk_resize(struct desktop *, int index, struct desk_box frame);
int desk_display_frame_commit(struct desktop *, int redraw_result);
void desk_display_relayout(struct desktop *, uint32_t width, uint32_t height);
void desk_zoom(struct desktop *, int index);
unsigned desk_resize_hit(const struct desk_client *, int32_t x, int32_t y);
void desk_input(struct desktop *, const struct ciuki_input_event *);
int desk_drain_input(struct desktop *, int (*read_events)(int, struct ciuki_input_event *, uint32_t));
void desk_composite(struct desktop *, struct desk_box);
void desk_accel_reset(struct desktop *, uint32_t generation);
void desk_cursor_motion(struct desktop *);
void desk_text(struct desktop *, struct desk_box, int32_t, int32_t,
               const char *, int, uint32_t);
uint8_t desk_glyph_row(unsigned, unsigned);
int desk_redraw(struct desktop *);
void desk_render_release(struct desktop *);
int desk_frame_due(const struct desktop *, uint64_t now);
void desk_display_reply(struct desktop *, int result);
uint64_t desk_launch_wait(const struct desktop *, uint64_t now); /* ns; short startup polling only */
int desk_load_portrait(uint32_t *pixels);
#endif
