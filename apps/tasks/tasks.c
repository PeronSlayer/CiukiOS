/* CiukiOS Task Manager (f2-33): Applications from the desktop's v2 window
 * list, Processes and Performance from kernel call 70, End Process through
 * the desktop's close path or the delegated authority (call 71).
 * SPDX-License-Identifier: MIT */
#include "model.h"
#include "view.h"
#include "../desktop/gate.h"
#include <ciuki/proc.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
enum { T_REFRESH = 1, T_SAMPLE };
#define QUERY_RETRY_NS 2000000000ull
struct tasks_app {
    struct du_window window; struct du_ui ui; struct tasks_view view; struct tm_model model;
    int authority, test, checkpoint_sent;
    uint32_t serial, query_serial, launch_serial, end_serial, launched_pid, ended_pid, end_target, pending_pid;
    uint64_t query_ns;
    uint8_t buffer[TM_LIST_BYTES];
};
static void message_box(struct tasks_app *a, const char *title, const char *text, int icon)
{ du_message(&a->ui, D_INFO, title, text, icon, NULL, 0, 0, 0); }
static void error_box(struct tasks_app *a, const char *what, int error)
{
    char text[256];
    snprintf(text, sizeof(text), "%s\n%s (error %d)", what, error == EPERM ? "Access is denied." : strerror(error), error);
    message_box(a, "Task Manager", text, DU_DLG_ICON_ERROR);
}
static int request(struct tasks_app *a, unsigned opcode, uint32_t first, uint32_t second, const char *name)
{
    struct ciuki_message m; unsigned n = name ? (unsigned)strlen(name) : 0;
    unsigned length = opcode == DESK_WINDOWS_QUERY ? 8 : opcode == DESK_SWITCH_TO ? 12 : opcode == DESK_END_TASK ? 16 : 12 + n;
    desk_message(&m, opcode, length);
    uint32_t serial = ++a->serial ? a->serial : ++a->serial;
    desk_put32(m.data + 4, serial);
    if (opcode == DESK_LAUNCH) { desk_put32(m.data + 8, n); memcpy(m.data + 12, name, n); }
    else if (opcode != DESK_WINDOWS_QUERY) { desk_put32(m.data + 8, first); if (opcode == DESK_END_TASK) desk_put32(m.data + 12, second); }
    return du_window_queue(&a->window, &m) ? 0 : (int)serial;
}
static void query_windows(struct tasks_app *a, uint64_t now)
{
    if (a->query_serial && now && now - a->query_ns < QUERY_RETRY_NS) return;
    if (!now) now = du_now();
    int serial = request(a, DESK_WINDOWS_QUERY, 0, 0, NULL);
    if (serial > 0) { a->query_serial = (uint32_t)serial; a->query_ns = now; tm_windows_begin(&a->model, (uint32_t)serial); }
}
static int refused(struct tasks_app *a, uint32_t pid, int signo, int error)
{ return ciuki_proc_terminate((ciuki_pid_t)pid, signo, a->authority) == -1 && errno == error; }
/* Test scenario: call 71 refuses the ended PID (ESRCH, which also proves the
 * delegated descriptor is a valid authority: EBADF comes first), PID 1 and
 * the desktop (EPERM). */
static int refusals(struct tasks_app *a)
{
    return a->authority >= 0 && a->window.server_pid && refused(a, a->ended_pid, SIGKILL, ESRCH) &&
           refused(a, 1, SIGKILL, EPERM) && refused(a, a->window.server_pid, SIGTERM, EPERM);
}
static void status_text(struct tasks_app *a, const char *text)
{ du_set_text(du_get(&a->ui, A_STATUS), text); a->ui.dirty = 1; }
static void refresh(struct tasks_app *a)
{
    int n = ciuki_proc_list(a->buffer, sizeof(a->buffer));
    if (n < 0 || tm_update(&a->model, a->buffer, sizeof(a->buffer), n)) {
        char text[64]; snprintf(text, sizeof(text), "Process list unavailable (error %d)", n < 0 ? errno : EIO);
        status_text(a, text);
    } else tv_update(&a->view, &a->model);
    query_windows(a, du_now());
    /* Test scenario: the ended PID has left the kernel list (reaped). */
    if (a->test && a->ended_pid && !a->checkpoint_sent && n >= 0 && !tm_find(&a->model, a->ended_pid) &&
        refusals(a) && !du_window_ping(&a->window, DESK_TASKS_CHECKPOINT)) {
        a->checkpoint_sent = 1;
        tv_page(&a->view, TV_PROCESSES);
        tv_update(&a->view, &a->model);
    }
}
static void sample(struct tasks_app *a)
{
    uint8_t system[CIUKI_PROC_LIST_MIN];
    if (ciuki_proc_list(system, sizeof(system)) >= 0 && tm_sample(&a->model, system, sizeof(system)) > 0)
        tv_update(&a->view, &a->model);
}
static void timers(struct tasks_app *a)
{
    uint64_t interval = tv_interval_ns(a->view.speed);
    du_timer_stop(&a->ui, T_REFRESH); du_timer_stop(&a->ui, T_SAMPLE);
    if (!interval) return;
    du_timer_start(&a->ui, T_REFRESH, interval, 1);
    du_timer_start(&a->ui, T_SAMPLE, 1000000000ull, 1);
}
static void end_task(struct tasks_app *a)
{
    const struct tm_window *w = tv_window(&a->view, &a->model);
    if (!w) return;
    int serial = request(a, DESK_END_TASK, w->id, w->pid, NULL);
    if (serial > 0) { a->end_serial = (uint32_t)serial; a->end_target = w->pid; }
}
static void end_process(struct tasks_app *a, uint32_t pid)
{
    switch (tm_route(&a->model, pid, a->window.server_pid)) {
    case TM_REFUSE_SYSTEM: message_box(a, "Unable to Terminate Process", "System processes cannot be ended.", DU_DLG_ICON_WARNING); return;
    case TM_REFUSE_DESKTOP: message_box(a, "Unable to Terminate Process", "The desktop cannot be ended from Task Manager.", DU_DLG_ICON_WARNING); return;
    case TM_REFUSE_GONE: message_box(a, "Unable to Terminate Process", "The process has already exited.", DU_DLG_ICON_INFO); return;
    case TM_VIA_DESKTOP: {
        /* The desktop's group ends only through the desktop's close path. */
        int serial = request(a, DESK_END_TASK, 0, pid, NULL);
        if (serial > 0) { a->end_serial = (uint32_t)serial; a->end_target = pid; }
        return;
    }
    case TM_VIA_KERNEL:
        if (ciuki_proc_terminate((ciuki_pid_t)pid, SIGKILL, a->authority)) error_box(a, "Unable to terminate the process.", errno);
        refresh(a);
        return;
    }
}
static void confirm_end_process(struct tasks_app *a)
{
    if (!a->view.selected_pid) return;
    a->pending_pid = a->view.selected_pid;
    static const char *const buttons[] = { "&Yes", "&No" };
    du_message(&a->ui, D_CONFIRM, "Task Manager Warning",
               "Terminating a process can cause loss of unsaved data.\nDo you want to end this process?",
               DU_DLG_ICON_WARNING, buttons, 2, 1, 1);
}
static void launch(struct tasks_app *a, const char *text)
{
    char program[DESK_TITLE_MAX + 1];
    while (*text == ' ') text++;
    snprintf(program, sizeof(program), "%s", text);
    for (size_t n = strlen(program); n && program[n - 1] == ' '; n--) program[n - 1] = 0;
    /* The v2 LAUNCH name is 1..63 visible ASCII bytes; anything else would
     * be a protocol violation, so refuse it here. */
    int valid = program[0] != 0;
    for (const char *c = program; *c; c++) valid &= *c > 32 && *c < 127;
    if (!valid) {
        if (program[0]) message_box(a, "Create New Task", "Program names are visible ASCII without spaces.", DU_DLG_ICON_WARNING);
        return;
    }
    int serial = request(a, DESK_LAUNCH, 0, 0, program);
    if (serial > 0) a->launch_serial = (uint32_t)serial;
}
static void on_message(struct du_window *w, const struct desk_packet *p, void *ctx)
{
    (void)w; struct tasks_app *a = ctx;
    if (p->opcode == DESK_WINDOW_INFO) {
        int r = tm_window_info(&a->model, p->serial, p->index, p->count, p->window_id, p->pid, p->flags, p->title);
        if (r <= 0) return;
        a->query_serial = 0;
        if (a->launched_pid)
            for (unsigned i = 0; i < a->model.window_count; i++)
                if (a->model.windows[i].pid == a->launched_pid) { a->view.selected_window = a->model.windows[i].id; a->launched_pid = 0; }
        tv_update(&a->view, &a->model);
    } else if (p->opcode == DESK_TASK_RESULT) {
        if (p->request == DESK_WINDOWS_QUERY && p->serial == a->query_serial) {
            a->query_serial = 0;
            if (p->result == -EPERM) status_text(a, "Window list unavailable");
        } else if (p->request == DESK_LAUNCH && p->serial == a->launch_serial) {
            a->launch_serial = 0;
            if (p->result > 0) a->launched_pid = (uint32_t)p->result;
            else error_box(a, "The program could not be started.", -p->result);
            query_windows(a, 0);
        } else if (p->request == DESK_END_TASK && p->serial == a->end_serial) {
            a->end_serial = 0;
            if (p->result) error_box(a, "The task could not be ended.", -p->result);
            else if (a->test && a->end_target) a->ended_pid = a->end_target;
        } else if (p->request == DESK_SWITCH_TO && p->result) error_box(a, "The window could not be shown.", -p->result);
    }
}
static void event(struct du_ui *u, const struct du_event *e, void *ctx)
{
    struct tasks_app *a = ctx; struct tm_model *m = &a->model; struct tasks_view *v = &a->view;
    switch (e->type) {
    case DU_EV_TICK: if (e->id == T_REFRESH) refresh(a); else if (e->id == T_SAMPLE) sample(a); return;
    case DU_EV_SELECT:
        if (e->id == A_TABS) tv_page(v, e->index);
        else if (e->id == A_APPS) v->selected_window = e->index >= 0 && (unsigned)e->index < m->window_count ? m->windows[e->index].id : 0;
        else if (e->id == A_PROCS) v->selected_pid = e->index >= 0 && (unsigned)e->index < m->count ? m->processes[e->index].pid : 0;
        tv_update(v, m);
        return;
    case DU_EV_SORT:
        if (e->id == A_PROCS) { tm_sort(m, e->index, m->sort == e->index ? !m->descending : 0); tv_update(v, m); }
        return;
    case DU_EV_OPEN:
        if (e->id == A_APPS && tv_window(v, m)) request(a, DESK_SWITCH_TO, tv_window(v, m)->id, 0, NULL);
        return;
    case DU_EV_KEY:      /* Delete ends the selected task or process, as on Windows */
        if (e->key == DU_KEY_DELETE && e->id == A_APPS) end_task(a);
        else if (e->key == DU_KEY_DELETE && e->id == A_PROCS) confirm_end_process(a);
        return;
    case DU_EV_DIALOG:
        if (e->id == D_NEW && !e->index && e->text) launch(a, e->text);
        else if (e->id == D_CONFIRM) { if (!e->index && a->pending_pid) end_process(a, a->pending_pid); a->pending_pid = 0; }
        return;
    case DU_EV_ACTIVATE: break;
    default: return;
    }
    switch (e->id) {
    case A_SWITCH: if (tv_window(v, m)) request(a, DESK_SWITCH_TO, tv_window(v, m)->id, 0, NULL); break;
    case A_END_TASK: end_task(a); break;
    case A_NEW: case M_NEW:
        du_input_box(u, D_NEW, "Create New Task", "Type the name of a program in /Programs, and CiukiOS\nwill open it for you.", "");
        break;
    case A_END_PROCESS: confirm_end_process(a); break;
    case M_REFRESH: refresh(a); break;
    case M_HIGH: case M_NORMAL: case M_LOW: case M_PAUSED: tv_speed(v, (enum tv_speed)(e->id - M_HIGH)); timers(a); tv_update(v, m); break;
    case M_EXIT: du_window_close(&a->window); break;
    case M_ABOUT:
        message_box(a, "About Task Manager", "CiukiOS Task Manager - Classic computing, one woof at a time.\nProcesses: kernel call 70. End Process: the\ndesktop or the delegated authority (call 71).\nlibdeskui and Task Manager code: MIT.", DU_DLG_ICON_INFO);
        break;
    default: break;
    }
}
int main(int argc, char **argv)
{
    int channel = 3, authority = -1, test = 0;
    for (int i = 1; i < argc; i++) {
        char *end; long n;
        if (!strncmp(argv[i], "--channel-fd=", 13)) { n = strtol(argv[i] + 13, &end, 10); if (!argv[i][13] || *end || n < 3 || n >= CIUKI_OPEN_MAX) return 2; channel = (int)n; }
        else if (!strncmp(argv[i], "--authority-fd=", 15)) { n = strtol(argv[i] + 15, &end, 10); if (!argv[i][15] || *end || n < 3 || n >= CIUKI_OPEN_MAX) return 2; authority = (int)n; }
        else if (!strcmp(argv[i], "--test=tasks")) test = 1;
        else return 2;
    }
    if (authority == channel) return 2;
    struct tasks_app *a = calloc(1, sizeof(*a));
    if (!a) return 1;
    a->authority = authority; a->test = test;
    tm_init(&a->model);
    if (du_window_open_resizable(&a->window, &a->ui, channel, TV_WIDTH, TV_HEIGHT, "Task Manager", TV_WIDTH, TV_HEIGHT)) {
        fprintf(stderr, "tasks: desktop connection failed: %s\n", strerror(errno)); free(a); return 1;
    }
    a->window.message = on_message; a->window.user = a;
    a->ui.event = event; a->ui.user = a;
    tv_init(&a->view, &a->ui, du_icon_argb(du_icon_name("program:*"), 16));
    if (test) { tv_speed(&a->view, TV_HIGH); du_window_move(&a->window, 8, 40); }
    refresh(a); sample(a); timers(a);
    int r = du_run(&a->window, NULL, NULL);
    du_window_destroy(&a->window);
    free(a);
    return r < 0 ? 1 : 0;
}
