/* f5-15C runtime-only driver of the QEMU `installers` tier: runs one
 * original installer from the run's CD fixture and executes the lead's
 * operator program (src/probes/win32_installers/title_program.h) through
 * the desktop's own input path. Windows are matched in w32srv's window
 * directory (the USER observer: HWND, owner PID, parent, style, caption and
 * class); keys and clicks go through desk_input exactly as a person's would.
 * The kernel observer (installer_title_probe.inc) owns every published fact
 * it can verify; this driver only reports where it is and what it saw.
 * Nothing here is reachable outside the selected probe's gate command.
 * SPDX-License-Identifier: MIT */
#include "desktop.h"
#include "win32_adapter.h"
#include "installer_title.h"
#include "../../src/probes/win32_installers/title_program.h"
#include "../../src/probes/win32_installers/title_controller.h"
#include <ciuki/proc.h>
#include <ciuki/spawn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "tz_env.h"
#include "profile.h" /* sec-user-05 */

extern void desk_v3_fixture_report(const char *);
#ifndef TITLE_MNT
#define TITLE_MNT "/mnt" /* the native namespace's drive mounts; a host test redirects it */
#endif
#define WINDOWS 256u
#define TRACKED 64u
enum { S_IDLE, S_INIT, S_SPAWN, S_RUN, S_UNINSTALL, S_DONE };
static struct {
    unsigned active, state, segment, serial, step, repeat_left, failure, failed_step;
    char run_drive, cd_drive;
    uint32_t self, root, root_reported, root_reaped, restart_pending, defaults;
    uint32_t tracked[TRACKED], tracked_count, baseline[TRACKED], baseline_count;
    uint64_t deadline, step_deadline, matched_at, apply_at, next_poll, quiet_since, segment_deadline;
    uint32_t pending_hwnd, pending_child, pending_kind, pending_default, last_signature;
    char keys_before[DESK_UNINSTALL_MAX][260];
    unsigned keys_before_count;
    char uninstall_key[260];
} title;
static struct tp_program program;
static struct tp_window windows[WINDOWS];
static unsigned window_count;
static struct title_result *result;
static uint8_t processes[CIUKI_PROC_LIST_MIN + CIUKI_PROCESS_MAX * sizeof(struct ciuki_proc_record)];

int desk_title_active(void) { return (int)title.active; }

static void report(unsigned phase)
{
    result->phase = phase;
    result->serial = ++title.serial;
    result->segment = title.segment;
    char line[96];
    snprintf(line, sizeof(line), "case=w32-title phase=%u serial=%u", phase, title.serial);
    desk_v3_fixture_report(line);
}
static void finish(unsigned failure)
{
    if (!title.active) return;
    if (failure && !title.failure) { title.failure = failure; title.failed_step = title.step + 1; }
    result->failure = title.failure;
    result->failed_step = title.failed_step;
    title.state = S_DONE;
    report(TITLE_DONE);
    title.active = 0;
}
int desk_title_begin(struct desktop *d, const char *argument)
{
    /* argument: <parameter-disk letter><CD letter>, lower case. */
    if (title.active || !argument || strlen(argument) != 2) return -EINVAL;
    for (unsigned i = 0; i < 2; i++)
        if (argument[i] < 'c' || argument[i] > 'z') return -EINVAL;
    memset(&title, 0, sizeof(title));
    /* The selected kernel probe mapped this private range before issuing the
     * gate command (TITLE_RESULT_VA; native mmap's arena starts at 0x20000000). */
    result = (struct title_result *)(uintptr_t)TITLE_RESULT_VA;
    memset(result, 0, sizeof(*result));
    result->magic = TITLE_MAGIC;
    result->version = 1;
    title.run_drive = argument[0];
    title.cd_drive = argument[1];
    title.self = (uint32_t)getpid();
    title.state = S_INIT;
    title.active = 1;
    title.deadline = d->now_ns + 60000000000ull; /* w32srv readiness */
    return 0;
}
/* ---- w32srv requests (read-only directory and registry) ---------------- */
static uint32_t directory(unsigned op, uint32_t key, uint32_t index, uint32_t flags, uint8_t *out, unsigned cap, uint32_t *bytes)
{
    static uint8_t reply[sizeof(struct w32r_reply) + 1100];
    uint8_t in[sizeof(struct w32r_request) + 2] = {0};
    struct w32r_request a = {.version = W32R_VERSION, .op = op, .key = key, .index = index, .flags = flags,
                             .path_bytes = 1, .name_bytes = 1};
    memcpy(in, &a, sizeof(a));
    uint32_t used = 0, e = desk_w32_exchange(in, sizeof(in), reply, sizeof(reply), &used);
    if (e) return e;
    struct w32r_reply r;
    if (used < sizeof(r)) return 13;
    memcpy(&r, reply, sizeof(r));
    if (r.version != W32R_VERSION) return 13;
    if (r.error) return r.error;
    if (r.name_bytes || used != sizeof(r) + r.data_bytes || r.data_bytes > cap) return 13;
    memcpy(out, reply + sizeof(r), r.data_bytes);
    *bytes = r.data_bytes;
    return 0;
}
static int ready(void) { return desk_w32_ready() == 0; }
static int tracked(uint32_t pid) { return tp_tracked(title.tracked, title.tracked_count, pid); }
static void snapshot_windows(void)
{
    window_count = 0;
    for (uint32_t i = 0; i < 4096 && window_count < WINDOWS; i++) {
        static uint8_t data[1100];
        uint32_t n = 0, e = directory(W32R_WINDOW_ENUM, 0, i, 1, data, sizeof(data), &n);
        if (e) break; /* 259: end of directory */
        struct w32r_window v;
        if (n < sizeof(v) + 1 || data[n - 1]) continue;
        memcpy(&v, data, sizeof(v));
        if (!tracked(v.pid)) continue;
        struct tp_window *w = &windows[window_count++];
        memset(w, 0, sizeof(*w));
        w->hwnd = v.hwnd; w->pid = v.pid; w->parent = v.parent; w->style = v.flags;
        snprintf(w->title, sizeof(w->title), "%s", (const char *)data + sizeof(v));
        uint8_t cls[128];
        uint32_t c = 0;
        if (!directory(W32R_WINDOW_CLASS, v.hwnd, 0, 0, cls, sizeof(cls) - 1, &c) && c && !cls[c - 1])
            snprintf(w->cls, sizeof(w->cls), "%s", (const char *)cls);
    }
}
static uint32_t window_signature(void)
{
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < window_count; i++) {
        const struct tp_window *w = &windows[i];
        const uint32_t words[] = {w->hwnd, w->style & (TP_WS_VISIBLE | TP_WS_DISABLED)};
        for (unsigned k = 0; k < 2; k++) for (unsigned b = 0; b < 4; b++) h = (h ^ ((words[k] >> (b * 8)) & 255)) * 16777619u;
        for (const char *s = w->title; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    }
    return h;
}
/* ---- process tracking ------------------------------------------------- */
static int process_list(void)
{
    int count = ciuki_proc_list(processes, sizeof(processes));
    struct ciuki_proc_system system;
    if (count <= 0 || (unsigned)count > CIUKI_PROCESS_MAX) return 0;
    memcpy(&system, processes, sizeof(system));
    if (system.version != CIUKI_PROC_VERSION || system.size != sizeof(system) || system.records != (unsigned)count) return 0;
    return count;
}
static struct ciuki_proc_record record_at(int i)
{
    struct ciuki_proc_record r;
    memcpy(&r, processes + sizeof(struct ciuki_proc_system) + (unsigned)i * sizeof(r), sizeof(r));
    return r;
}
static void track(uint32_t pid)
{
    if (!pid || tracked(pid) || title.tracked_count == TRACKED) return;
    title.tracked[title.tracked_count++] = pid;
    result->processes = title.tracked_count;
}
static int baseline(uint32_t pid) { return tp_tracked(title.baseline, title.baseline_count, pid); }
/* Descendants of tracked processes (by parent or process group, so a setup
 * child orphaned by an exiting self-extractor stays tracked), and during a
 * virtual restart the RunOnce launches the desktop starts. Returns the
 * number of live tracked processes. */
static unsigned update_tracking(void)
{
    int count = process_list();
    unsigned live = 0;
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < count; i++) {
            struct ciuki_proc_record r = record_at(i);
            if (r.version != CIUKI_PROC_VERSION || r.size != sizeof(r) || tracked(r.pid)) continue;
            int child_of_tracked = tracked(r.ppid);
            int group_of_root = title.root && r.pgid == title.root && r.pid != title.self;
            int restart_launch = title.restart_pending && r.ppid == title.self && !baseline(r.pid) &&
                memcmp(r.name, "w32srv", 7);
            if (child_of_tracked || group_of_root || restart_launch) track(r.pid);
        }
    for (int i = 0; i < count; i++) {
        struct ciuki_proc_record r = record_at(i);
        if (tracked(r.pid) && r.state == CIUKI_PROC_LIVE) live++;
    }
    return live;
}
static int root_zombie(void)
{
    int count = process_list();
    for (int i = 0; i < count; i++) {
        struct ciuki_proc_record r = record_at(i);
        if (r.pid == title.root) return r.state == CIUKI_PROC_ZOMBIE;
    }
    return -1; /* already gone */
}
/* ---- input through the desktop ---------------------------------------- */
static int client_for(struct desktop *d, uint32_t hwnd, uint32_t pid)
{
    for (int i = 0; i < DESK_CLIENTS; i++)
        if (d->clients[i].window && d->clients[i].v3_win32_hwnd == hwnd && !d->clients[i].closing) return i;
    for (int i = 0; i < DESK_CLIENTS; i++)
        if (d->clients[i].window && d->clients[i].pid == pid && !d->clients[i].closing && !d->clients[i].minimized) return i;
    return -1;
}
static void key(struct desktop *d, unsigned code, int down)
{
    struct ciuki_input_event e = {.type = CIUKI_INPUT_KEY, .monotonic_ns = d->now_ns, .code = (int32_t)code, .value = down};
    desk_input(d, &e);
}
static void chord(struct desktop *d, unsigned k)
{
    if (k & TP_CTRL) key(d, 0x1d, 1);
    if (k & TP_ALT) key(d, 0x38, 1);
    if (k & TP_SHIFT) key(d, 0x2a, 1);
    key(d, TP_CODE(k), 1);
    key(d, TP_CODE(k), 0);
    if (k & TP_SHIFT) key(d, 0x2a, 0);
    if (k & TP_ALT) key(d, 0x38, 0);
    if (k & TP_CTRL) key(d, 0x1d, 0);
}
static int type_text(struct desktop *d, const char *text)
{
    for (; *text; text++) {
        unsigned k = tp_char_key((unsigned char)*text);
        if (!k) return -EINVAL;
        chord(d, k);
    }
    return 0;
}
static void click(struct desktop *d, int client, int x, int y)
{
    struct desk_client *c = &d->clients[client];
    struct desk_box box = desk_content(c);
    unsigned scale = c->v3_scale ? c->v3_scale : 1;
    d->mouse_x = box.x + x * (int)scale;
    d->mouse_y = box.y + y * (int)scale;
    struct ciuki_input_event e = {.type = CIUKI_INPUT_MOTION, .monotonic_ns = d->now_ns};
    desk_input(d, &e);
    e.type = CIUKI_INPUT_BUTTON; e.code = CIUKI_BUTTON_LEFT; e.value = 1;
    desk_input(d, &e);
    e.value = 0;
    desk_input(d, &e);
}
static int press_button(struct desktop *d, const struct tp_window *button)
{
    unsigned m = tp_mnemonic(button->title);
    if (m) {
        unsigned k = tp_chord((char[]){(char)m, 0});
        if (!k) return -EINVAL;
        chord(d, k | TP_ALT);
    } else chord(d, 0x1c); /* the dialog's default push button */
    return 0;
}
/* ---- operator steps --------------------------------------------------- */
static const struct tp_step *current(void)
{
    unsigned first = title.segment ? program.install_steps : 0;
    unsigned count = title.segment ? program.uninstall_steps : program.install_steps;
    return title.step < count ? &program.steps[first + title.step] : NULL;
}
static void ui_report(unsigned kind, const struct tp_window *top, const struct tp_window *child)
{
    result->ui_step = kind == TITLE_A_STEP ? title.step + 1 : 0;
    result->ui_kind = kind;
    result->ui_owner = top ? top->pid : 0;
    result->ui_hwnd = top ? top->hwnd : 0;
    result->ui_shot = 1;
    result->ui_detail = child ? child->hwnd : 0;
    snprintf(result->ui_title, sizeof(result->ui_title), "%s", top ? top->title : "");
    snprintf(result->ui_class, sizeof(result->ui_class), "%s", top ? top->cls : "");
    report(TITLE_UI);
}
static int apply(struct desktop *d, const struct tp_step *s, int top, int child)
{
    const struct tp_window *w = top >= 0 ? &windows[top] : NULL;
    int client = w ? client_for(d, w->hwnd, w->pid) : -1;
    if (w && client < 0) return -EAGAIN;
    if (client >= 0) desk_focus(d, client);
    if (!s) return press_button(d, &windows[child]); /* interactive default */
    switch (s->action) {
    case TP_DEFAULT: chord(d, 0x1c); return 0;
    case TP_BUTTON: return press_button(d, &windows[child]);
    case TP_KEYS_ACTION:
        for (unsigned i = 0; i < s->key_count; i++) chord(d, s->keys[i]);
        return 0;
    case TP_TEXT:
        if (s->flags & TP_REPLACE) { chord(d, 0x147); chord(d, 0x14f | TP_SHIFT); }
        return type_text(d, s->value);
    case TP_CLICK:
        if (client < 0) return -EAGAIN;
        click(d, client, s->x, s->y);
        return 0;
    default: return 0;
    }
}
/* The newest visible tracked top-level window, for steps without a target. */
static int newest_top(void)
{
    for (unsigned i = 0; i < window_count; i++)
        if (!windows[i].parent && (windows[i].style & TP_WS_VISIBLE)) return (int)i;
    return -1;
}
static int step_match(const struct tp_step *s, int *child)
{
    int target = s->title[0] || s->cls[0] || s->button[0] || s->text[0];
    *child = -1;
    if (!target) return newest_top();
    return tp_match(s, windows, window_count, title.tracked, title.tracked_count, child);
}
/* ---- registry facts through the Add/Remove Programs view --------------- */
static int uninstall_keys(char (*keys)[260], unsigned cap)
{
    struct desk_uninstall_entry *entries = calloc(DESK_UNINSTALL_MAX, sizeof(*entries));
    if (!entries) return -ENOMEM;
    int n = ciuki_win32_uninstall_list(entries, DESK_UNINSTALL_MAX);
    for (int i = 0; i < n && (unsigned)i < cap; i++) snprintf(keys[i], 260, "%s", entries[i].key);
    if (n > 0 && entries[0].inventory_more) n = -EOVERFLOW;
    free(entries);
    return n;
}
static int find_uninstall(const char *key, struct desk_uninstall_entry *out)
{
    struct desk_uninstall_entry *entries = calloc(DESK_UNINSTALL_MAX, sizeof(*entries));
    if (!entries) return -ENOMEM;
    int n = ciuki_win32_uninstall_list(entries, DESK_UNINSTALL_MAX), found = -ENOENT;
    for (int i = 0; i < n; i++)
        if (!strcmp(entries[i].key, key)) { *out = entries[i]; found = 0; break; }
    free(entries);
    return n < 0 ? n : found;
}
static unsigned installed_facts(void)
{
    static char after[DESK_UNINSTALL_MAX][260];
    int n = uninstall_keys(after, DESK_UNINSTALL_MAX);
    if (n < 0) return TITLE_F_REGISTRY;
    result->uninstall_keys_after = (uint32_t)n;
    result->new_keys = 0;
    for (int i = 0; i < n; i++) {
        int old = 0;
        for (unsigned k = 0; k < title.keys_before_count; k++) if (!strcmp(after[i], title.keys_before[k])) old = 1;
        if (old) continue;
        if (result->new_keys < TITLE_KEYS)
            snprintf(result->keys[result->new_keys], TITLE_KEY_BYTES, "%s", after[i]);
        if (!title.uninstall_key[0]) snprintf(title.uninstall_key, sizeof(title.uninstall_key), "%s", after[i]);
        result->new_keys++;
    }
    if (result->new_keys > TITLE_KEYS) result->new_keys = TITLE_KEYS;
    if (title.uninstall_key[0]) {
        static struct desk_uninstall_entry entry; /* desktop thread only; keeps tick's frame small */
        if (!find_uninstall(title.uninstall_key, &entry))
            snprintf(result->uninstall_command, sizeof(result->uninstall_command), "%s", entry.command);
    }
    return 0;
}
/* ---- spawning --------------------------------------------------------- */
static int spawn_win32(const char *native, const char *dos, const char *arguments, const char *directory)
{
    static char command[1100];
    int n = snprintf(command, sizeof(command), "\"%s\"%s%s", dos, arguments[0] ? " " : "", arguments);
    if (n < 0 || n >= (int)sizeof(command)) return -E2BIG;
    char saved[260];
    if (!getcwd(saved, sizeof(saved))) return -errno;
    if (directory[0] && chdir(directory)) return -errno;
    char *argv[] = {(char *)dos, command, NULL};
    char tz[DESK_TZ_ENV_BYTES];
    /* sec-user-05: the installer runs with the session's profile temporaries. */
    struct profile_env profile;
    char *env[10] = {"LC_ALL=C", desk_tz_env(tz, sizeof(tz)), "PATH=/Programs"};
    env[profile_env_add(&profile, env, 3)] = NULL;
    /* A new process group: the installer tree is followed by group as well. */
    int pid = ciuki_spawn(native, argv, env, NULL, 0, CIUKI_SPAWN_NEW_GROUP);
    int error = pid < 0 ? -errno : 0;
    if (chdir(saved) && !error) error = -errno;
    return error ? error : pid;
}
static int spawn_installer(void)
{
    char native[300], dos[300], directory[300];
    int a = snprintf(native, sizeof(native), "%s/%c/%s", TITLE_MNT, title.cd_drive, program.entry);
    int b = snprintf(dos, sizeof(dos), "%c:\\%s", title.cd_drive - 32, program.entry);
    if (a < 0 || a >= (int)sizeof(native) || b < 0 || b >= (int)sizeof(dos)) return -ENAMETOOLONG;
    for (char *p = native; *p; p++) if (*p == '\\') *p = '/';
    for (char *p = dos; *p; p++) if (*p == '/') *p = '\\';
    snprintf(directory, sizeof(directory), "%s", native);
    char *slash = strrchr(directory, '/');
    if (slash) *slash = 0;
    return spawn_win32(native, dos, program.arguments, directory);
}
static int spawn_uninstaller(void)
{
    static struct desk_uninstall_entry entry;
    static struct desk_launch_record record;
    if (!title.uninstall_key[0] || find_uninstall(title.uninstall_key, &entry) ||
        !desk_uninstall_command(entry.command, &record)) return -ENOENT;
    char native[300], directory[300];
    int e = desk_w32_native_path(record.target, native, sizeof(native));
    if (e) return e;
    snprintf(directory, sizeof(directory), "%s", native);
    char *slash = strrchr(directory, '/');
    if (slash && slash != directory) *slash = 0;
    return spawn_win32(native, record.target, record.arguments, directory);
}
static void begin_segment(struct desktop *d, int pid)
{
    title.root = (uint32_t)pid;
    title.root_reported = title.root_reaped = 0;
    title.tracked_count = 0;
    title.restart_pending = 0;
    title.step = 0;
    title.repeat_left = 0;
    title.matched_at = title.apply_at = 0;
    title.pending_kind = 0;
    title.quiet_since = d->now_ns;
    title.last_signature = 0;
    track((uint32_t)pid);
    result->root_pid = (uint32_t)pid;
    result->root_exit = 0;
    unsigned timeout = title.segment ? program.uninstall_timeout_s : program.timeout_s;
    title.segment_deadline = d->now_ns + (uint64_t)timeout * 1000000000ull;
    title.step_deadline = 0;
    report(TITLE_STARTED);
}
/* Restart prompt and crash dialog of a tracked process (desktop v3 dialogs). */
static int dialogs(struct desktop *d)
{
    if (!d->v3_dialog_kind || !tracked(d->v3_dialog_pid)) return 0;
    if (d->v3_dialog_kind == 1) {
        result->crashes++;
        ui_report(TITLE_A_CRASH, NULL, NULL);
        (void)desk_v3_dialog_accept(d);
        for (int i = 0; i < DESK_CLIENTS; i++)
            if (d->clients[i].internal == DESK_INTERNAL_MESSAGE) desk_request_close(d, i);
        finish(TITLE_F_CRASH);
        return 1;
    }
    if (d->v3_dialog_kind == 2) {
        uint32_t pid = d->v3_dialog_pid, flags = d->v3_restart_flags;
        result->restarts++;
        ui_report(TITLE_A_RESTART, NULL, NULL);
        d->v3_dialog_kind = 0;
        d->v3_dialog_pid = 0;
        for (int i = 0; i < DESK_CLIENTS; i++)
            if (d->clients[i].internal == DESK_INTERNAL_MESSAGE) desk_request_close(d, i);
        if (program.restart_accept) {
            if (!d->v3_restart || d->v3_restart(d, pid, flags) < 0) { finish(TITLE_F_RESTART); return 1; }
            title.restart_pending = 1;
        }
        return 1;
    }
    return 0;
}
void desk_title_notice(unsigned kind, uint32_t pid, uint32_t flags)
{
    (void)pid; (void)flags;
    if (title.active && (kind == 3 || kind == 4)) title.restart_pending = 0;
}
static void segment_complete(struct desktop *d)
{
    /* Commit w32srv's hives before the kernel walks C: (and before the
     * runner's host inventory reads them from the overlay). */
    if (desk_w32_flush() < 0) { finish(TITLE_F_REGISTRY); return; }
    if (!title.segment) {
        unsigned f = installed_facts();
        if (f) { finish(f); return; }
        report(TITLE_INSTALLED);
        if (!program.uninstall || !title.uninstall_key[0]) { finish(TITLE_OK); return; }
        title.state = S_UNINSTALL;
        title.segment = 1;
        title.deadline = d->now_ns;
        return;
    }
    int present = ciuki_win32_uninstall_registered(title.uninstall_key);
    if (present < 0) { finish(TITLE_F_REGISTRY); return; }
    result->key_removed = !present;
    (void)installed_facts();
    report(TITLE_UNINSTALLED);
    finish(TITLE_OK);
}
static void run(struct desktop *d)
{
    if (d->now_ns < title.next_poll) return;
    title.next_poll = d->now_ns + 100000000ull;
    if (d->now_ns >= title.segment_deadline) { finish(TITLE_F_TIMEOUT); return; }
    if (dialogs(d)) return;
    unsigned live = update_tracking();
    if (title.root && !title.root_reaped) {
        int zombie = root_zombie();
        if (zombie == 1 && !title.root_reported) { title.root_reported = 1; report(TITLE_ROOT_EXITED); }
        if (zombie == 1 && title.root_reported) {
            int status = 0;
            if (waitpid((int)title.root, &status, WNOHANG) == (int)title.root) {
                title.root_reaped = 1;
                result->root_exit = WIFEXITED(status) ? (uint32_t)WEXITSTATUS(status) : ~0u;
            }
        } else if (zombie < 0) title.root_reaped = 1; /* reaped elsewhere: exit status was not observed */
    }
    /* A pending action waits out its screenshot pause first. */
    if (title.pending_kind) {
        if (d->now_ns < title.apply_at) return;
        snapshot_windows();
        int top = -1;
        for (unsigned i = 0; i < window_count; i++) if (windows[i].hwnd == title.pending_hwnd) top = (int)i;
        int child = -1;
        for (unsigned i = 0; i < window_count; i++) if (windows[i].hwnd == title.pending_child) child = (int)i;
        const struct tp_step *s = title.pending_kind == TITLE_A_STEP ? current() : NULL;
        unsigned kind = title.pending_kind;
        title.pending_kind = 0;
        if (top < 0 || ((kind == TITLE_A_DEFAULT || (s && s->action == TP_BUTTON)) && child < 0)) return; /* window changed: rematch */
        int e = apply(d, s, top, child);
        if (e == -EAGAIN) return;
        if (e) { finish(TITLE_F_INPUT); return; }
        title.quiet_since = d->now_ns;
        if (kind == TITLE_A_DEFAULT) { title.defaults++; result->defaults_used = title.defaults; return; }
        if (title.repeat_left > 1) { title.repeat_left--; title.matched_at = 0; return; }
        title.step++; title.repeat_left = 0; title.matched_at = 0; title.step_deadline = 0;
        result->steps_done++;
        return;
    }
    if (!live && title.root_reaped && !title.restart_pending && !d->v3_dialog_kind) {
        /* The installer ended before the program's mandatory pages: the
         * program does not describe this run (recalibrate), never a pass. */
        for (const struct tp_step *left = current(); left; left = current()) {
            if (!(left->flags & TP_OPTIONAL)) { finish(TITLE_F_STEPS_LEFT); return; }
            title.step++;
        }
        segment_complete(d);
        return;
    }
    snapshot_windows();
    uint32_t signature = window_signature();
    if (signature != title.last_signature) { title.last_signature = signature; title.quiet_since = d->now_ns; }
    const struct tp_step *s = current();
    if (s) {
        if (!title.step_deadline) {
            title.step_deadline = d->now_ns + (uint64_t)s->timeout_s * 1000000000ull;
            title.repeat_left = s->repeat;
        }
        int child = -1, top = step_match(s, &child);
        int satisfied = s->action == TP_GONE ? top < 0 : top >= 0;
        if (satisfied) {
            if (!title.matched_at) title.matched_at = d->now_ns;
            if (d->now_ns - title.matched_at < (uint64_t)s->settle_ms * 1000000ull) return;
            if (s->action == TP_WAIT || s->action == TP_GONE || s->action == TP_EXPECT) {
                title.step++; title.matched_at = 0; title.step_deadline = 0; result->steps_done++;
                return;
            }
            ui_report(TITLE_A_STEP, &windows[top], child >= 0 ? &windows[child] : NULL);
            title.pending_kind = TITLE_A_STEP;
            title.pending_hwnd = windows[top].hwnd;
            title.pending_child = child >= 0 ? windows[child].hwnd : 0;
            title.apply_at = d->now_ns + (uint64_t)program.shot_ms * 1000000ull;
            return;
        }
        title.matched_at = 0;
        if (d->now_ns >= title.step_deadline) {
            if (s->flags & TP_OPTIONAL) { title.step++; title.step_deadline = 0; return; }
            finish(s->action == TP_EXPECT ? TITLE_F_EXPECT : TITLE_F_STEP_TIMEOUT);
        }
        return;
    }
    /* Interactive defaults: Finish/Install/Next/Yes/... on a settled page. */
    if (!program.defaults || title.defaults >= program.max_defaults) return;
    if (d->now_ns - title.quiet_since < (uint64_t)program.idle_ms * 1000000ull) return;
    for (unsigned i = 0; i < window_count; i++) {
        if (windows[i].parent || !(windows[i].style & TP_WS_VISIBLE)) continue;
        int button = tp_default_button(windows, window_count, i);
        if (button < 0) continue;
        ui_report(TITLE_A_DEFAULT, &windows[i], &windows[button]);
        title.pending_kind = TITLE_A_DEFAULT;
        title.pending_hwnd = windows[i].hwnd;
        title.pending_child = windows[button].hwnd;
        title.apply_at = d->now_ns + (uint64_t)program.shot_ms * 1000000ull;
        return;
    }
}
static unsigned load_program(void)
{
    char path[300];
    snprintf(path, sizeof(path), "%s/%c/%s", TITLE_MNT, title.run_drive, TITLE_PROGRAM_FILE);
    FILE *f = fopen(path, "rb");
    if (!f) return TITLE_F_PROGRAM;
    static char text[64 * 1024];
    size_t n = fread(text, 1, sizeof(text), f);
    int bad = ferror(f) || n == sizeof(text);
    if (fclose(f)) bad = 1;
    if (bad || tp_parse(&program, text, (unsigned)n)) return TITLE_F_PROGRAM;
    result->choices_default = program.choices_default;
    result->uninstall_planned = program.uninstall;
    return TITLE_OK;
}
void desk_title_tick(struct desktop *d)
{
    if (!title.active) return;
    switch (title.state) {
    case S_INIT: {
        if (d->now_ns >= title.deadline) { finish(TITLE_F_SERVICE); return; }
        if (!ready()) return;
        unsigned f = load_program();
        if (f) { finish(f); return; }
        int n = uninstall_keys(title.keys_before, DESK_UNINSTALL_MAX);
        if (n < 0) { finish(TITLE_F_REGISTRY); return; }
        title.keys_before_count = (unsigned)n;
        result->uninstall_keys_before = (uint32_t)n;
        int count = process_list();
        for (int i = 0; i < count && title.baseline_count < TRACKED; i++) {
            struct ciuki_proc_record r = record_at(i);
            if (r.ppid == title.self) title.baseline[title.baseline_count++] = r.pid;
        }
        report(TITLE_READY); /* the kernel takes its pre-install snapshot here */
        title.state = S_SPAWN;
        return;
    }
    case S_SPAWN: {
        int pid = spawn_installer();
        if (pid <= 0) { finish(TITLE_F_SPAWN); return; }
        title.state = S_RUN;
        begin_segment(d, pid);
        return;
    }
    case S_UNINSTALL: {
        int pid = spawn_uninstaller();
        if (pid <= 0) { finish(TITLE_F_UNINSTALL_SPAWN); return; }
        title.state = S_RUN;
        begin_segment(d, pid);
        return;
    }
    case S_RUN: run(d); return;
    default: return;
    }
}
