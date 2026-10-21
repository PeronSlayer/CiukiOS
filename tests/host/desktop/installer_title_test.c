/* f5-15C: the real installers-tier driver (apps/desktop/installer_title.c)
 * against mocked desktop input, w32srv window directory and registry, the
 * process list and spawn. Every report is checked as the kernel observer
 * would: phase/serial consistency of the checkpoint page at 0x10100000.
 * SPDX-License-Identifier: MIT */
#include "desktop.h"
#include "win32_adapter.h"
#include <ciuki/spawn.h>
#include <ciuki/proc.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char mnt[256];
#define TITLE_MNT mnt
/* ---- mocked services ---------------------------------------------------- */
struct mock_window { uint32_t hwnd, pid, parent, style; const char *cls, *title; };
static struct mock_window windows_now[16];
static unsigned windows_count, service_up, flushes, uninstall_registered = 1;
static struct desk_uninstall_entry entries[4];
static unsigned entry_count;
static struct ciuki_proc_record procs[8];
static unsigned proc_count;
static char spawned_path[300], spawned_argv0[300], spawned_command[1100], chdir_path[300];
static unsigned spawn_flags, spawns;
static int next_pid = 40;
static unsigned keys[256], key_count, buttons, focus_calls, accepted, closes, restart_calls;
static uint32_t reaped[8];
static unsigned reaped_count;
static unsigned phases[64], phase_count, last_serial;

uint32_t desk_w32_exchange(const void *input, uint32_t n, void *output, uint32_t cap, uint32_t *used)
{
    const struct w32r_request *a = input;
    assert(n >= sizeof(*a) + 2);
    struct w32r_reply r = {.version = W32R_VERSION};
    uint8_t *out = output;
    if (a->op == W32R_WINDOW_ENUM) {
        assert(a->flags == 1);
        if (a->index >= windows_count) r.error = 259;
        else {
            const struct mock_window *w = &windows_now[a->index];
            struct w32r_window v = {w->hwnd, w->pid, 1, w->parent, 0xc001, w->style};
            memcpy(out + sizeof(r), &v, sizeof(v));
            strcpy((char *)out + sizeof(r) + sizeof(v), w->title);
            r.data_bytes = (uint32_t)(sizeof(v) + strlen(w->title) + 1);
        }
    } else if (a->op == W32R_WINDOW_CLASS) {
        r.error = 1400;
        for (unsigned i = 0; i < windows_count; i++)
            if (windows_now[i].hwnd == a->key) {
                strcpy((char *)out + sizeof(r), windows_now[i].cls);
                r.data_bytes = (uint32_t)strlen(windows_now[i].cls) + 1;
                r.error = 0;
            }
    } else assert(!"unexpected w32srv request");
    assert(cap >= sizeof(r) + r.data_bytes);
    memcpy(out, &r, sizeof(r));
    *used = (uint32_t)sizeof(r) + (r.error ? 0 : r.data_bytes);
    if (r.error) ((struct w32r_reply *)output)->data_bytes = 0;
    return 0;
}
int desk_w32_ready(void) { return service_up ? 0 : -EPIPE; }
int desk_w32_flush(void) { flushes++; return 0; }
int ciuki_win32_uninstall_list(struct desk_uninstall_entry *out, unsigned cap)
{
    assert(cap >= entry_count);
    memcpy(out, entries, entry_count * sizeof(*out));
    return (int)entry_count;
}
int ciuki_win32_uninstall_registered(const char *key) { assert(!strcmp(key, "AoETrial")); return (int)uninstall_registered; }
int desk_w32_native_path(const char *path, char *out, size_t cap)
{
    assert(path[0] == 'C' && path[1] == ':');
    snprintf(out, cap, "%s", path + 2);
    for (char *p = out; *p; p++) if (*p == '\\') *p = '/';
    return 0;
}
int ciuki_spawn(const char *path, char *const argv[], char *const env[], const struct ciuki_spawn_fd *fds, uint32_t n, uint32_t flags)
{
    assert(argv && argv[0] && argv[1] && !argv[2] && env && !fds && !n);
    snprintf(spawned_path, sizeof(spawned_path), "%s", path);
    snprintf(spawned_argv0, sizeof(spawned_argv0), "%s", argv[0]);
    snprintf(spawned_command, sizeof(spawned_command), "%s", argv[1]);
    spawn_flags = flags;
    spawns++;
    int pid = next_pid++;
    procs[proc_count++] = (struct ciuki_proc_record){.version = CIUKI_PROC_VERSION, .size = sizeof(struct ciuki_proc_record),
        .pid = (uint32_t)pid, .ppid = 30, .pgid = (uint32_t)pid, .state = CIUKI_PROC_LIVE};
    return pid;
}
int ciuki_proc_list(void *out, uint32_t size)
{
    struct ciuki_proc_system system = {.version = CIUKI_PROC_VERSION, .size = sizeof(system), .records = proc_count};
    assert(size >= sizeof(system) + proc_count * sizeof(procs[0]));
    memcpy(out, &system, sizeof(system));
    memcpy((char *)out + sizeof(system), procs, proc_count * sizeof(procs[0]));
    return (int)proc_count;
}
static struct ciuki_proc_record *proc(uint32_t pid)
{
    for (unsigned i = 0; i < proc_count; i++) if (procs[i].pid == pid) return &procs[i];
    return NULL;
}
static void proc_remove(uint32_t pid)
{
    for (unsigned i = 0; i < proc_count; i++)
        if (procs[i].pid == pid) { procs[i] = procs[--proc_count]; return; }
}
static int fixture_getpid(void) { return 30; }
static int fixture_waitpid(int pid, int *status, int flags)
{
    assert(flags == WNOHANG);
    struct ciuki_proc_record *p = proc((uint32_t)pid);
    if (!p || p->state != CIUKI_PROC_ZOMBIE) return 0;
    *status = 0;
    reaped[reaped_count++] = (uint32_t)pid;
    proc_remove((uint32_t)pid);
    return pid;
}
static int fixture_chdir(const char *path) { snprintf(chdir_path, sizeof(chdir_path), "%s", path); return 0; }
static char *fixture_getcwd(char *buffer, size_t size) { snprintf(buffer, size, "/"); return buffer; }
void desk_input(struct desktop *d, const struct ciuki_input_event *e)
{
    (void)d;
    if (e->type == CIUKI_INPUT_KEY) { assert(key_count < 256); keys[key_count++] = (unsigned)e->code | (e->value ? 0x8000u : 0); }
    else if (e->type == CIUKI_INPUT_BUTTON) buttons++;
}
void desk_focus(struct desktop *d, int index) { (void)d; assert(index == 0); focus_calls++; }
struct desk_box desk_content(const struct desk_client *c) { (void)c; return (struct desk_box){100, 50, 400, 300}; }
int desk_v3_dialog_accept(struct desktop *d) { accepted++; d->v3_dialog_kind = 0; return 0; }
void desk_request_close(struct desktop *d, int i) { (void)d; (void)i; closes++; }
static int restart_call(struct desktop *d, uint32_t pid, uint32_t flags) { (void)d; (void)pid; (void)flags; restart_calls++; return 0; }

#define getpid fixture_getpid
#define waitpid fixture_waitpid
#define chdir fixture_chdir
#define getcwd fixture_getcwd
#include "../../../apps/desktop/installer_title.c"
#undef getpid
#undef waitpid
#undef chdir
#undef getcwd

void desk_v3_fixture_report(const char *line)
{
    unsigned phase, serial;
    assert(sscanf(line, "case=w32-title phase=%u serial=%u", &phase, &serial) == 2);
    assert(title_valid(result) && result->phase == phase && result->serial == serial && serial == last_serial + 1);
    last_serial = serial;
    assert(phase_count < 64);
    phases[phase_count++] = phase;
}
static unsigned last_phase(void) { return phase_count ? phases[phase_count - 1] : 0; }
static void tick(struct desktop *d, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        d->now_ns += 100000000ull;
        desk_title_tick(d);
    }
}
static void write_program(const char *text)
{
    char path[300];
    snprintf(path, sizeof(path), "%s/d", mnt);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/d/CIUKI-OP.TXT", mnt);
    FILE *f = fopen(path, "wb");
    assert(f && fputs(text, f) >= 0 && !fclose(f));
}
static void reset(struct desktop *d)
{
    memset(d, 0, sizeof(*d));
    d->v3_restart = restart_call;
    d->clients[0].window = 1;
    d->clients[0].v3_win32_hwnd = 0x100;
    windows_count = entry_count = proc_count = key_count = buttons = focus_calls = accepted = closes = restart_calls = 0;
    reaped_count = phase_count = last_serial = spawns = flushes = 0;
    service_up = 0;
    uninstall_registered = 1;
    memset(&title, 0, sizeof(title));
}
static const char *program_text =
    "CIUKIOP 1\nworkload aoe-trial\nchoices default\nentry MSAoE.exe\ntimeout 120\n"
    "defaults 1 idle=4000 max=5\nrestart accept\nuninstall 1 timeout=60\nshot 200\n"
    "step install button title=Setup* button=Next*\n"
    "step install expect text=C:%5CProgram%20Files*\n"
    "end\n";
static void install_flow(struct desktop *d, int with_restart)
{
    reset(d);
    write_program(program_text);
    assert(desk_title_begin(d, "de") == 0 && desk_title_active());
    assert(desk_title_begin(d, "de") == -EINVAL);
    tick(d, 3);
    assert(!phase_count);                       /* w32srv not answering yet */
    service_up = 1;
    tick(d, 1);
    assert(last_phase() == TITLE_READY && result->choices_default && result->uninstall_planned);
    tick(d, 1);
    assert(last_phase() == TITLE_STARTED && spawns == 1 && spawn_flags == CIUKI_SPAWN_NEW_GROUP);
    char expected[300];
    snprintf(expected, sizeof(expected), "%s/e/MSAoE.exe", mnt);
    assert(!strcmp(spawned_path, expected) && !strcmp(spawned_argv0, "E:\\MSAoE.exe"));
    assert(!strcmp(spawned_command, "\"E:\\MSAoE.exe\""));
    snprintf(expected, sizeof(expected), "%s/e", mnt);
    assert(!strcmp(chdir_path, "/"));           /* the desktop's cwd is restored */
    uint32_t root = result->root_pid;
    /* A setup child in the root's group, orphaned to PID 1, stays tracked. */
    procs[proc_count++] = (struct ciuki_proc_record){.version = CIUKI_PROC_VERSION, .size = sizeof(struct ciuki_proc_record),
        .pid = 77, .ppid = 1, .pgid = root, .state = CIUKI_PROC_LIVE};
    windows_now[0] = (struct mock_window){0x100, 77, 0, TP_WS_VISIBLE, "#32770", "Setup - Age of Empires"};
    windows_now[1] = (struct mock_window){0x101, 77, 0x100, TP_WS_VISIBLE, "Button", "&Next >"};
    windows_now[2] = (struct mock_window){0x102, 77, 0x100, TP_WS_VISIBLE, "Button", "Cancel"};
    windows_now[3] = (struct mock_window){0x103, 77, 0x100, TP_WS_VISIBLE, "Edit", "C:\\Program Files\\Microsoft Games"};
    windows_now[4] = (struct mock_window){0x200, 99, 0, TP_WS_VISIBLE, "#32770", "Somebody else"};
    windows_count = 5;
    tick(d, 6);                                 /* settle 500 ms, then the shot */
    assert(last_phase() == TITLE_UI && result->ui_kind == TITLE_A_STEP && result->ui_step == 1 && result->ui_owner == 77);
    assert(!strcmp(result->ui_title, "Setup - Age of Empires") && !strcmp(result->ui_class, "#32770"));
    tick(d, 3);                                 /* 200 ms screenshot pause */
    assert(key_count == 4 && keys[0] == (0x38 | 0x8000) && keys[1] == (0x31 | 0x8000) && keys[2] == 0x31 && keys[3] == 0x38);
    assert(focus_calls == 1 && result->steps_done == 1);
    tick(d, 6);                                 /* expect: the directory answer */
    assert(result->steps_done == 2 && key_count == 4);
    /* Interactive defaults: the Finish page, never Cancel. */
    windows_now[1].title = "&Finish";
    key_count = 0;
    tick(d, 46);                                /* 4 s settled, then the shot */
    assert(result->ui_kind == TITLE_A_DEFAULT && result->defaults_used == 1);
    assert(key_count == 4 && keys[1] == (0x21 | 0x8000));  /* Alt+F */
    if (with_restart) {
        d->v3_dialog_kind = 2; d->v3_dialog_pid = 77; d->v3_restart_flags = 2;
        tick(d, 1);
        assert(restart_calls == 1 && result->restarts == 1 && !d->v3_dialog_kind && result->ui_kind == TITLE_A_RESTART);
        /* RunOnce's helper, started by the desktop during the restart, is followed. */
        procs[proc_count++] = (struct ciuki_proc_record){.version = CIUKI_PROC_VERSION, .size = sizeof(struct ciuki_proc_record),
            .pid = 80, .ppid = 30, .pgid = 30, .state = CIUKI_PROC_LIVE};
        tick(d, 1);
        assert(tp_tracked(title.tracked, title.tracked_count, 80));
        desk_title_notice(3, 77, 2);
        proc_remove(80);
    }
    /* The installer tree ends: the root is a zombie, the child is gone. */
    windows_count = 0;
    proc_remove(77);
    proc(root)->state = CIUKI_PROC_ZOMBIE;
    entries[0] = (struct desk_uninstall_entry){.key = "AoETrial", .name = "Age of Empires Trial",
                                               .command = "\"C:\\Program Files\\AoE\\Uninstal.exe\" /U"};
    entry_count = 1;
    tick(d, 1);
    assert(phases[phase_count - 2] == TITLE_ROOT_EXITED && reaped_count == 1 && reaped[0] == root);
    assert(flushes == 1 && phases[phase_count - 1] == TITLE_INSTALLED && result->new_keys == 1);
    assert(!strcmp(result->keys[0], "AoETrial") && strstr(result->uninstall_command, "Uninstal.exe"));
}
int main(void)
{
    setbuf(stdout, NULL);
    void *page = mmap((void *)(uintptr_t)TITLE_RESULT_VA, TITLE_RESULT_PAGES * 4096, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(page == (void *)(uintptr_t)TITLE_RESULT_VA);
    const char *base = getenv("TITLE_TEST_DIR"); /* a build/host directory, never /tmp */
    assert(base && strlen(base) < sizeof(mnt));
    snprintf(mnt, sizeof(mnt), "%s", base);
    static struct desktop d;
    /* Full flow: install, defaults, uninstall through Add/Remove Programs. */
    install_flow(&d, 0);
    tick(&d, 1);                                 /* S_UNINSTALL: spawn the uninstaller */
    assert(last_phase() == TITLE_STARTED && result->segment == 1 && spawns == 2);
    assert(!strcmp(spawned_path, "/Program Files/AoE/Uninstal.exe") && !strcmp(spawned_argv0, "C:\\Program Files\\AoE\\Uninstal.exe"));
    assert(!strcmp(spawned_command, "\"C:\\Program Files\\AoE\\Uninstal.exe\" /U"));
    uint32_t uninstaller = result->root_pid;
    proc(uninstaller)->state = CIUKI_PROC_ZOMBIE;
    uninstall_registered = 0;
    tick(&d, 2);
    assert(phases[phase_count - 2] == TITLE_UNINSTALLED && result->key_removed == 1);
    assert(last_phase() == TITLE_DONE && result->failure == TITLE_OK && !desk_title_active());
    /* With a virtual restart accepted on the way. */
    install_flow(&d, 1);
    assert(result->restarts == 1);
    /* A crash dialog of a tracked process fails the run with its reason. */
    reset(&d);
    write_program(program_text);
    assert(!desk_title_begin(&d, "de"));
    service_up = 1;
    tick(&d, 2);
    d.v3_dialog_kind = 1;
    d.v3_dialog_pid = result->root_pid;
    tick(&d, 1);
    assert(accepted == 1 && result->crashes == 1 && last_phase() == TITLE_DONE && result->failure == TITLE_F_CRASH);
    /* The installer ends before the program's mandatory pages: never a pass. */
    reset(&d);
    write_program(program_text);
    assert(!desk_title_begin(&d, "de"));
    service_up = 1;
    tick(&d, 2);
    proc(result->root_pid)->state = CIUKI_PROC_ZOMBIE;
    tick(&d, 1);
    assert(phases[phase_count - 2] == TITLE_ROOT_EXITED && last_phase() == TITLE_DONE);
    assert(result->failure == TITLE_F_STEPS_LEFT && result->failed_step == 1);
    /* A step that never matches times out; an unreadable program fails at once. */
    reset(&d);
    write_program(program_text);
    assert(!desk_title_begin(&d, "de"));
    service_up = 1;
    tick(&d, 2 + 700);
    assert(last_phase() == TITLE_DONE && result->failure == TITLE_F_STEP_TIMEOUT && result->failed_step == 1);
    reset(&d);
    write_program("CIUKIOP 2\nend\n");
    assert(!desk_title_begin(&d, "de"));
    service_up = 1;
    tick(&d, 1);
    assert(last_phase() == TITLE_DONE && result->failure == TITLE_F_PROGRAM);
    assert(desk_title_begin(&d, "d") == -EINVAL && desk_title_begin(&d, "dA") == -EINVAL);
    puts("PASS installers-tier driver: readiness, CD launch, group tracking, scripted button, directory expect, "
         "interactive defaults, virtual restart, RunOnce adoption, root exit, Add/Remove uninstall, crash, step timeout, program refusal");
    return 0;
}
