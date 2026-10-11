/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_APP_DESKTOP_H
#define CIUKI_APP_DESKTOP_H
#include "protocol.h"
#include <stddef.h>
#define DESK_CLIENTS 16
#define DESK_DAMAGE_MAX 32
#define DESK_TX_MAX 64
#define DESK_BAR 32
#define DESK_BORDER 3
#define DESK_TITLE 24
#define DESK_BACKGROUND 0x375564u
#define DESK_PORTRAIT_SIZE 256
#define DESK_MENU_MAX 16
#define DESK_MENU_ROW 28
#define DESK_CLOSE_NS 2000000000ull
/* App scenarios (runtime-selected in the post-fault interaction stage): the
 * launched program gets --test=<its name> and reports ordered checkpoint
 * PINGs whose serials are base+n. */
#define DESK_APP_FILES 1
#define DESK_APP_NOTE 2
#define DESK_APP_CONTROL 3          /* /bin/control: may send DESK_SETTINGS */
#define DESK_FILES_CHECKPOINT 0xf2310000u
#define DESK_FILES_CHECKPOINTS 5
#define DESK_NOTE_CHECKPOINT 0xf2320000u
#define DESK_NOTE_CHECKPOINTS 3
#define DESK_APP_CHECKPOINT(kind) ((kind)==DESK_APP_NOTE ? DESK_NOTE_CHECKPOINT : DESK_FILES_CHECKPOINT)
#define DESK_APP_CHECKPOINTS(kind) ((kind)==DESK_APP_NOTE ? DESK_NOTE_CHECKPOINTS : DESK_FILES_CHECKPOINTS)
struct desk_box { int32_t x, y, w, h; };
#include "shell.h"
struct desk_damage { unsigned count; struct desk_box boxes[DESK_DAMAGE_MAX]; };
struct desk_client {
    int channel, surface, hello, window, unresponsive, ping_pending, resync_pending;
    int configure_pending, focus_pending, close_pending;
    int minimized, internal, closing, program;   /* internal: DESK_INTERNAL_* kind; program: 1 Files, 2 CiukNote, 3 Control Panel */
    int taskman;                  /* f2-33: launched /bin/tasks; may send task requests */
    uint32_t pid, serial, version, window_id;
    uint64_t ping_sent_ns, next_ping_ns, order;
    struct ciuki_surface_info info;
    const uint32_t *pixels;
    struct desk_box frame;
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
struct desk_program { char name[64], path[70]; };
struct desk_child { uint32_t pid; uint64_t close_ns; int term_sent; };
struct desktop {
    struct desk_ops ops;
    int display, input, output, focus, drag;
    uint32_t width, height, *pixels;
    const uint32_t *portrait;
    /* Shell icons (desktop.c: libdeskui's Tango store, icon_map.c keys such as
     * "program:files"); NULL draws none. */
    const uint32_t *(*icon)(const char *object, unsigned size);
    int32_t mouse_x, mouse_y, drag_x, drag_y;
    uint8_t keys[513];
    uint32_t buttons, routed_buttons;
    uint64_t order, now_ns, clock_second, presents, inputs, received, replied;
    struct desk_damage damage;
    struct desk_client clients[DESK_CLIENTS];
    int shell, menu_open, menu_focus, about, action, shell_test, shell_stage;
    uint32_t shell_pid, shell_keys;
    /* f2-31/f2-32 app scenarios (app_test is DESK_APP_FILES or DESK_APP_NOTE):
     * the program's checkpoints are PINGs with serial DESK_APP_CHECKPOINT+n. */
    int app_test, app_checkpoint, app_reported, app_done;
    uint32_t app_pid;
    const char *launch_arg;      /* single argument for the next launch (LAUNCH from Files) */
    int (*launch)(struct desktop *, const struct desk_program *);
    int (*terminate)(uint32_t, int);
    uint64_t realtime_second;
    int realtime_source;
    char build_identity[65], version[65], notice[64];
    struct desk_program programs[DESK_MENU_MAX];
    unsigned program_count;
    struct desk_child children[DESK_CLIENTS];
    /* f2-33: task-manager extension and its runtime-selected gate scenario. */
    uint32_t next_window_id, self_pid;
    int authority_fd, tasks_test, tasks_stage, tasks_checkpoint;
    uint32_t tasks_pid, tasks_demo_pid, tasks_demo_window;
    /* Desktop shell (shell.h): settings, menus, icons, tray, built-in windows. */
    int mixer_fd;                /* delegated /dev/mixer control handle (CIUKI_MIXER_FD) or -1 */
    const char *launch_argv[6];  /* Run dialog: extra arguments for the next launch */
    struct desk_shell sh;
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
struct desk_box desk_taskbar(const struct desktop *);
struct desk_box desk_task_button(const struct desktop *, int index);
struct desk_box desk_menu_box(const struct desktop *);
int32_t desk_menu_row(const struct desktop *);
int desk_contains(struct desk_box, int32_t, int32_t);
int desk_hit(const struct desktop *, int32_t, int32_t);
int desk_program_add(struct desktop *, const char *name, const uint8_t *elf, size_t);
int desk_program_graphical(const char *name);
const char *desk_program_label(const char *name);
const char *desk_app_program(int kind);          /* "files" / "ciuknote" */
int desk_program_kind(const char *name);         /* 1 Files, 2 CiukNote, else 0 */
void desk_shell_enable(struct desktop *);
void desk_minimize(struct desktop *, int index);
void desk_request_close(struct desktop *, int index);
void desk_close_timeouts(struct desktop *);
void desk_child_exited(struct desktop *, uint32_t pid, int status);
void desk_about(struct desktop *);
void desk_task_windows(struct desktop *, int index, uint32_t serial);
int desk_task_end(struct desktop *, uint32_t window_id, uint32_t pid);
int desk_task_switch(struct desktop *, uint32_t window_id);
int desk_task_launch(struct desktop *, const char *name, const char *argument);
int desk_shell_click(struct desktop *);
void desk_route_key(struct desktop *, int code, int value);
void desk_shell_draw(struct desktop *, struct desk_box);
void desk_damage_add(struct desk_damage *, struct desk_box, uint32_t, uint32_t);
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
void desk_input(struct desktop *, const struct ciuki_input_event *);
void desk_composite(struct desktop *, struct desk_box);
void desk_text(struct desktop *, struct desk_box, int32_t, int32_t,
               const char *, int, uint32_t);
uint8_t desk_glyph_row(unsigned, unsigned);
int desk_redraw(struct desktop *);
int desk_load_portrait(uint32_t *pixels);
#endif
