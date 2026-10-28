/* PID 1 bootstrap extension; process.c remains owner of orphan collection.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/supervisor.h>
#include <ciuki/probe.h>
#include <ciuki/audio.h>
#include <ciuki/desktop.h>
#include <ciuki/pager.h>
#include <ciuki/setup_mode.h>
#include <ciuki/storage.h>
#include <ciuki/splash.h>
#include <ciuki/systime.h>
#include <ciuki/security.h>
#pragma weak timezone_env
#pragma weak console_splash_phase
extern void dosvm_client_poll(void) __attribute__((weak));
extern void entropy_desktop_presented(void) __attribute__((weak)); /* f6-21 */
extern void dosvm_image_desktop_presented(void) __attribute__((weak)); /* fx-4b */
extern void w32_services_desktop_died(uint32_t) __attribute__((weak));
#pragma weak audio_mixer_delegate
#pragma weak proc_authority_install

static const struct supervisor_io_ops *supervisor_io;
extern void w32_service_delegate(struct process *,struct process *,const char *) __attribute__((weak));
extern int px_security_cwd(struct process *, const char *) __attribute__((weak));
static uint32_t desktop_pid, desktop_deaths;

static bool bootstrap_attempted;
static struct { uint32_t pid, generation; uint64_t retry_at; bool enabled; } trusted_roles[6];
/* G13 (ER-3): an abnormal death of the bootstrap desktop restarts it, as
 * Windows 9x restarts a crashed Explorer shell. At most three restarts in a
 * minute; after that (or when the restart cannot start) the console takes
 * over as before. Probe and stand-in desktops are never restarted. */
#define DESKTOP_RESTART_MAX 3u
#define DESKTOP_RESTART_WINDOW_MS 60000u
#define DESKTOP_RESTART_WAIT_MS 5000u
static bool desktop_restartable, desktop_restarting;
static uint32_t desktop_restarts, desktop_restart_old;
static uint64_t desktop_restart_first, desktop_restart_deadline;
static char observed_probe[24];
static uint32_t observed_pid;
static uint64_t report_bytes;
static struct supervisor_capture captures[2];

void supervisor_capture_init(struct supervisor_capture *c)
{
    memset(c, 0, sizeof(*c));
    sha256_init(&c->digest);
}
void supervisor_capture_add(struct supervisor_capture *c, const void *data, uint32_t length)
{
    const uint8_t *bytes = data;
    sha256_update(&c->digest, data, length);
    c->bytes += length;
    for (uint32_t i = 0; i < length; i++) {
        if (c->head_bytes < SUPERVISOR_CAPTURE_BYTES) c->head[c->head_bytes++] = bytes[i];
        /* Scan the entire stream, including the discarded middle. The Lua
         * suite documents "final OK": https://www.lua.org/tests/ . */
        if (c->scan_bytes == sizeof(c->scan)) {
            memmove(c->scan, c->scan + 1, sizeof(c->scan) - 1);
            c->scan_bytes--;
        }
        c->scan[c->scan_bytes++] = bytes[i];
        static const char final[] = "final OK", failed[] = "assertion failed";
        if (c->scan_bytes >= sizeof(final)-1 &&
            !memcmp(c->scan + c->scan_bytes - (sizeof(final)-1), final, sizeof(final)-1)) c->final_ok++;
        if (c->scan_bytes >= sizeof(failed)-1 &&
            !memcmp(c->scan + c->scan_bytes - (sizeof(failed)-1), failed, sizeof(failed)-1)) c->assertion_failures++;
        c->tail[c->tail_at] = bytes[i];
        c->tail_at = (c->tail_at + 1) % SUPERVISOR_CAPTURE_BYTES;
        if (c->tail_bytes < SUPERVISOR_CAPTURE_BYTES) c->tail_bytes++;
    }
}
void supervisor_capture_digest(const struct supervisor_capture *c, char out[65])
{
    struct sha256_ctx snapshot = c->digest;
    uint8_t digest[32];
    sha256_final(&snapshot, digest);
    sha256_hex(digest, out);
}
void supervisor_capture_tail(const struct supervisor_capture *c, uint8_t *out)
{
    unsigned start = c->tail_bytes == SUPERVISOR_CAPTURE_BYTES ? c->tail_at : 0;
    for (unsigned i = 0; i < c->tail_bytes; i++) out[i] = c->tail[(start + i) % SUPERVISOR_CAPTURE_BYTES];
}
void supervisor_set_io_ops(const struct supervisor_io_ops *ops) { supervisor_io = ops; }
uint32_t supervisor_desktop_deaths(void) { return desktop_deaths; }
uint32_t supervisor_desktop_pid(void)
{
    if (!security_roles_active()) return desktop_pid;
    for (unsigned i=1;i<CIUKI_PROCESS_MAX;i++) {
        struct process *p=proc_at_slot(i);
        if (p && p->state==PROC_LIVE && security_role(p)==CIUKI_SECURITY_ROLE_DESKTOP && security_process_live(p))
            return p->pid;
    }
    return 0;
}
uint32_t supervisor_desktop_restarts(void) { return desktop_restarts; }
/* A desktop is running or about to be restarted: faults of other programs
 * still have somebody to tell (proc/win32/diagnostics.c). */
bool supervisor_desktop_expected(void) { return supervisor_desktop_pid() || desktop_restarting; }

static int spawn_process(const char *path, const char *const argv[], const char *cwd,
                         bool desktop, bool setup, uint32_t role, struct process **out);
int supervisor_spawn(const char *path, const char *const argv[], const char *cwd,
                      bool desktop, struct process **out)
{
    return spawn_process(path, argv, cwd, desktop, false, 0, out);
}
static int spawn_process(const char *path, const char *const argv[], const char *cwd,
                         bool desktop, bool setup, uint32_t role, struct process **out)
{
    const struct ciuki_file_ops *files = proc_get_file_ops();
    if (!files || !files->open) return -ENOENT;
    struct ciuki_file file = { 0 };
    int err = files->open(proc_supervisor()->cwd, path, &file);
    if (err) return err;
    struct proc_strings *strings = proc_strings_new();
    if (!strings) { if (file.close) file.close(file.cookie); return -ENOMEM; }
    for (unsigned i = 0; argv[i] && !err; i++)
        err = proc_strings_add(strings, argv[i], (uint32_t)strlen(argv[i]) + 1, false);
    /* G1: the system time zone rule, UTC0 when none is configured. */
    char tz[8 + CIUKI_TIME_RULE_MAX];
    if (!timezone_env || !timezone_env(tz, sizeof(tz))) memcpy(tz, "TZ=UTC0", sizeof("TZ=UTC0"));
    const char *env[] = { "LC_ALL=C", tz, "HOME=/Documents", "TMPDIR=/tmp", "PATH=/Programs" };
    for (unsigned i = 0; i < ARRAY_SIZE(env) && !err; i++)
        err = proc_strings_add(strings, env[i], (uint32_t)strlen(env[i]) + 1, true);
    /* Prepare privately; stdio/cwd can block, so proc_spawn_file's immediate
     * publication cannot be used here. No provisional child becomes runnable. */
    struct process *p = 0;
    struct elf_image image;
    struct proc_thread *thread = 0;
    uint32_t esp = 0;
    if (!err) err = proc_prepare(proc_supervisor(), &p);
    if (!err && role) err=security_role_prepare(proc_supervisor(),p,role,path);
    if (!err && desktop) err = security_bootstrap_desktop(p);
    if (!err && desktop && proc_supervisor()->cwd)
        err = px_security_cwd ? px_security_cwd(p, "/") : -EIO;
    /* f1-98: the desktop's threads keep a minimum CPU share beside a game. */
    if (!err && desktop) proc_share_group(p);
    if (!err) proc_set_name(p, path);
    if (!err) err = elf_validate(&file, &image);
    if (!err) err = elf_load(&file, &image, p->memory);
    /* f0-55: the desktop image is in memory; the grant below completes the
     * boot splash bar (console_display_owned). */
    if (!err && desktop && console_splash_phase) console_splash_phase(SPLASH_PHASE_DESKTOP);
    if (!err) err = proc_thread_prepare(p, image.entry, 0, 0, CIUKI_THREAD_STACK_DEFAULT, 0, 0, true, &thread);
    if (!err) {
        if (role==CIUKI_SECURITY_ROLE_LOGON_UI) err=security_trusted_prepare(p);
        else err = supervisor_io && supervisor_io->stdio ? supervisor_io->stdio(p) : -ENODEV;
    }
    if (!err && cwd) err = supervisor_io && supervisor_io->cwd ? supervisor_io->cwd(p, cwd) : -ENODEV;
    if (!err && (desktop || setup)) { int32_t grants[2]; err = grants_install(p, grants); }
    if (!err && desktop && audio_mixer_delegate) {
        int fd = audio_mixer_delegate(proc_supervisor(), p);
        if (fd < 0) err = fd;
        else {
            char mixer_env[32];
            int length = ksnprintf(mixer_env, sizeof(mixer_env), "CIUKI_MIXER_FD=%d", fd);
            err = proc_strings_add(strings, mixer_env, (uint32_t)length + 1, true);
        }
    }
    if (!err && desktop && proc_authority_install) {
        /* f2-33: termination authority, delegated like the mixer handle. The
         * desktop may pass it explicitly to the Task Manager it launches. */
        int fd = proc_authority_install(p);
        if (fd < 0) err = fd;
        else {
            char tasks_env[32];
            int length = ksnprintf(tasks_env, sizeof(tasks_env), "CIUKI_TASKS_FD=%d", fd);
            err = proc_strings_add(strings, tasks_env, (uint32_t)length + 1, true);
        }
    }
    if (!err) supervisor_dosvm_delegate(proc_supervisor(), p, path);
    /* f9-01: the Setup capability belongs to this one spawn (D5). */
    if (!err) p->setup_capability = setup;
    if (!err) err = proc_stack_build(p->memory, strings, image.entry, thread->tls, &esp);
    if (!err) {
        task_native_frame(thread->task, esp);
        if (desktop || setup) {
            task_set_class(thread->task, P_INTERACTIVE);
            task_focus(thread->task);
        }
    }
    if (file.close) file.close(file.cookie);
    proc_strings_free(strings);
    if (!err && !security_process_live(p)) err = -EACCES;
    if (err) { if (p) proc_discard(p); return err; }
    if(w32_service_delegate)w32_service_delegate(proc_supervisor(),p,path);
    if (!security_process_live(p)) { proc_discard(p); return -EACCES; }
    proc_publish(p, CIUKI_SPAWN_NEW_GROUP);
    *out = p;
    return (int)p->pid;
}

/* Fixed sealed service entry; never dispatched by a public spawn path or
 * selected using argv/environment. The service image lands in sec-user-01. */
int supervisor_spawn_security_role(uint32_t role, struct process **out)
{
    const char *path=security_role_image(role);
    if (!path || role==CIUKI_SECURITY_ROLE_DESKTOP || role==CIUKI_SECURITY_ROLE_W32SRV ||
        role==CIUKI_SECURITY_ROLE_SETUP) return -EINVAL;
    const char *argv[]={path,0};
    int result=spawn_process(path,argv,0,false,false,role,out);
    if (result>0) {
        trusted_roles[role].enabled=true;
        trusted_roles[role].pid=(*out)->pid;
        trusted_roles[role].generation=security_process_generation(*out);
        trusted_roles[role].retry_at=0;
    }
    return result;
}
int supervisor_spawn_account_service(struct process **out)
{ return supervisor_spawn_security_role(CIUKI_SECURITY_ROLE_ACCOUNT,out); }

/* f3-02: the DOS window authority (syscall 70) follows only an exact
 * "/Programs/dosbox" spawned by the supervisor or by a process that owns both a
 * display and an input grant (the desktop). Grants never leave their owner
 * process, so no other launcher can confer it. Program-management authority
 * follows the same parent check, only to the desktop and Control Panel. */
void supervisor_dosvm_delegate(struct process *parent, struct process *child, const char *path)
{
    bool display = false, input = false;
    if (parent != proc_supervisor()) {
        for (unsigned i = 0; i < CIUKI_OPEN_MAX; i++) {
            struct desktop_description *d = desktop_fd(parent, i);
            if (!d || d->owner != parent->pid) continue;
            display |= d->kind == DESKTOP_DISPLAY;
            input |= d->kind == DESKTOP_INPUT;
        }
    }
    bool authorized = parent == proc_supervisor() || (display && input);
    child->setup_capability = false; /* f9-01: never inherited or path-derived */
    child->dosvm_capability = authorized && !strncmp(path,"/Programs/dosbox",sizeof("/Programs/dosbox"));
    /* sec-k04: compatibility flags describe actual rights, never image paths.
     * Consumers authorize the actual issuer's credential on every operation. */
    bool admin = security_process_live(child) && security_admin(child);
    child->dhcp_capability = admin;
    child->network_capability = admin;
    child->programs_capability = authorized &&
        (!strncmp(path,"/Programs/desktop",sizeof("/Programs/desktop")) || !strncmp(path,"/Programs/control",sizeof("/Programs/control")));
    child->time_capability = admin;
    child->recovery_capability = admin;
}

int supervisor_spawn_gate(bool supplement, struct process **out)
{
    const char *basic[] = { "lua", "-e", "_U=true", "all.lua", 0 };
    const char *extra[] = { "lua", "ciuki-f2.lua", 0 };
    return supervisor_spawn("/Programs/lua", supplement ? extra : basic,
                             "/system/tests/lua-5.4.8-tests", false, out);
}

/* Probe entry reuses the normal payload/grants/group preparation. Tracking
 * here makes unexpected desktop death visible through the production ledger. */
int supervisor_spawn_desktop_probe(struct process **out)
{
    const char *argv[] = { "desktop", "--test=crash-isolation", "--gate", 0 };
    int result = supervisor_spawn("/Programs/desktop", argv, 0, true, out);
    if (result > 0) desktop_pid = (*out)->pid;
    return result;
}

/* f9-01 (D11): Setup mode spawns /Programs/setup from the boot CD's C:
 * instead of the desktop; without the boot CD no process starts. */
static bool setup_bootstrap(struct process **out, int *result)
{
    if (!setup_mode()) return false;
    console_wake();   /* the placeholder and the Setup messages are console text */
    if (!storage_setup_media_ready()) {
        klog("%s", SETUP_MEDIA_TEXT);
        *result = -ENOENT;
        return true;
    }
    const char *argv[] = { "setup", 0 };
    klog("[supervisor] bootstrap: spawning /Programs/setup");
    *result = spawn_process("/Programs/setup", argv, 0, false, true, CIUKI_SECURITY_ROLE_SETUP, out);
    if (*result == -ENOENT) *result = supervisor_spawn_standin(proc_supervisor(), out);
    if (*result >= 0) klog("[supervisor] setup pid=%u pgid=%u setup_capability=%u",
                           (*out)->pid, (*out)->pgid, (*out)->setup_capability);
    else klog("[supervisor] setup unavailable: %d", *result);
    return true;
}

void supervisor_bootstrap(void)
{
    if (bootstrap_attempted || ((g_boot.flags & CBI_F_TEST_REQUEST) && !supervisor_interactive_test(&g_boot))) return;
    bootstrap_attempted = true;
    struct process *setup = 0;
    int setup_result = 0;
    if (setup_bootstrap(&setup, &setup_result)) {
        if (pager_bootstrap_complete) pager_bootstrap_complete();
        return;
    }
    /* sec-own: the system volume's OWNER store, policy and lease are bound
     * before any service or desktop starts (fs/owner_system.c). */
    extern void owner_system_boot(void) __attribute__((weak));
    if (owner_system_boot) owner_system_boot();
    /* end sec-own */
    /* sec-user-01 supplies this sealed image. Its absence during the k01
     * transition leaves the existing boot working; other failures are visible. */
    struct process *accounts = 0;
    int account_result = supervisor_spawn_account_service(&accounts);
    if (account_result < 0 && account_result != -ENOENT && account_result != -EBUSY)
        klog("[supervisor] account service unavailable: %d", account_result);
    struct process *trusted=0;
    (void)supervisor_spawn_security_role(CIUKI_SECURITY_ROLE_REGISTRY,&trusted);
    (void)supervisor_spawn_security_role(CIUKI_SECURITY_ROLE_LOGON_UI,&trusted);
    int logon_result=supervisor_spawn_security_role(CIUKI_SECURITY_ROLE_LOGON,&trusted);
    if (logon_result>0) return;
    if (logon_result!=-ENOENT) {
        klog("[supervisor] trusted logon unavailable: %d",logon_result);
        return;
    }
    /* G14 (docs/design/startup-recovery.md 2.3): Safe mode's corner labels
     * and notice; Command prompt only runs one Terminal and no shell. */
    const char *argv[4] = { "desktop", 0, 0, 0 };
    unsigned argc = 1;
    if (g_boot.flags & CBI_F_SAFE_MODE) argv[argc++] = "--safe";
    if (g_boot.flags & CBI_F_CMD_ONLY) argv[argc++] = "--shell=terminal";
    struct process *p = 0;
    bool stand_in = false;
    klog("[supervisor] bootstrap: spawning /Programs/desktop");
    if (argc > 1) klog("[supervisor] desktop mode safe=%u shell=%s", !!(g_boot.flags & CBI_F_SAFE_MODE),
                       (g_boot.flags & CBI_F_CMD_ONLY) ? "terminal" : "explorer");
    if (console_splash_phase) console_splash_phase(SPLASH_PHASE_SUPERVISOR);
    int result = supervisor_spawn("/Programs/desktop", argv, 0, true, &p);
    if (result == -ENOENT) {
        stand_in = true;
        result = supervisor_spawn_standin(proc_supervisor(), &p);
        if (result >= 0 && pager_bootstrap_complete) pager_bootstrap_complete();
        if (result >= 0 && entropy_desktop_presented) entropy_desktop_presented();
        if (result >= 0 && dosvm_image_desktop_presented) dosvm_image_desktop_presented();
    }
    if (result < 0) {
        if (pager_bootstrap_complete) pager_bootstrap_complete();
        if (entropy_desktop_presented) entropy_desktop_presented(); /* f6-21: no desktop frame */
        if (dosvm_image_desktop_presented) dosvm_image_desktop_presented(); /* fx-4b */
        console_wake();   /* a quiet boot shows its log instead of the desktop */
        klog("[supervisor] desktop unavailable: %d; console ready", result);
        return;
    }
    desktop_pid = p->pid;
    desktop_restartable = !stand_in;
    klog("[supervisor] desktop pid=%u pgid=%u", p->pid, p->pgid);
}

/* The console takes over: the pager, the entropy collector and the log
 * screen learn that no desktop frame will come. */
static void desktop_gone(uint32_t pid, int status)
{
    if (pager_bootstrap_complete) pager_bootstrap_complete();
    if (entropy_desktop_presented) entropy_desktop_presented();
    console_wake();
    klog("[supervisor] desktop stopped; console ready pid=%u status=%d deaths=%u", pid, status, desktop_deaths);
}

/* Throttle: the window opens at the first restart and closes after a
 * minute; the fourth abnormal death inside it gives up. */
static bool supervisor_restart_allowed(uint64_t now_ms)
{
    if (!desktop_restarts || now_ms - desktop_restart_first >= DESKTOP_RESTART_WINDOW_MS) {
        desktop_restarts = 0;
        desktop_restart_first = now_ms;
    }
    if (desktop_restarts >= DESKTOP_RESTART_MAX) return false;
    desktop_restarts++;
    return true;
}

/* Runs on every pass of PID 1 while a restart is pending. The old desktop
 * must have released its display and input grants (close_resources, which
 * the collection after this poll performs) before the new one can take them. */
static void desktop_restart_step(void)
{
    struct process *old = proc_find(desktop_restart_old);
    bool released = !old || old->state == PROC_ZOMBIE;
    if (!released && g_ticks < desktop_restart_deadline) {
        task_sleep_ms(1);
        proc_notify();
        return;
    }
    int result = -EBUSY;
    struct process *p = 0;
    if (released) {
        const char *argv[] = { "desktop", 0 };
        result = supervisor_spawn("/Programs/desktop", argv, 0, true, &p);
        if (result == -EPERM && g_ticks < desktop_restart_deadline) {
            task_sleep_ms(1);       /* the grants are still being released */
            proc_notify();
            return;
        }
    }
    desktop_restarting = false;
    if (result > 0) {
        desktop_pid = p->pid;
        klog("[supervisor] desktop restarted pid=%u pgid=%u restarts=%u", p->pid, p->pgid, desktop_restarts);
        return;
    }
    klog("[supervisor] desktop restart failed: %d", result);
    desktop_gone(desktop_restart_old, -1);
}

void supervisor_poll(void)
{
    security_poll();
    extern void owner_service_poll(void) __attribute__((weak));
    if(owner_service_poll) owner_service_poll();
    /* Restart only roles that this manifest actually started. Missing images
     * on transitional/test boots never become a public registration route. */
    for (unsigned role=1;role<ARRAY_SIZE(trusted_roles);role++) {
        if (!trusted_roles[role].enabled || role==CIUKI_SECURITY_ROLE_OWNER) continue;
        struct process *p=proc_find(trusted_roles[role].pid);
        if (p && p->state==PROC_LIVE && security_process_live(p) &&
            security_process_generation(p)==trusted_roles[role].generation) continue;
        if (g_ticks<trusted_roles[role].retry_at) continue;
        trusted_roles[role].retry_at=g_ticks+1000;
        struct process *replacement=0;
        int err=supervisor_spawn_security_role(role,&replacement);
        if (err<0 && err!=-EBUSY) klog("[supervisor] trusted role %u restart failed: %d",role,err);
    }
    if (dosvm_client_poll) dosvm_client_poll();
    if (desktop_restarting) desktop_restart_step();
    if (desktop_pid) {
        struct process *p = proc_find(desktop_pid);
        if (!p || p->state == PROC_STOPPING || p->state == PROC_ZOMBIE) {
            uint32_t pid=desktop_pid;
            desktop_pid = 0;
            desktop_deaths++;
            int status = p ? p->status : -1;
            /* The window server it spawned is an orphan now (adopted by PID 1)
             * and would keep the Win32 service registration from the next
             * desktop's server (q3crash 2026-10-25). */
            if (w32_services_desktop_died) w32_services_desktop_died(pid);
            /* A clean exit (status 0) leaves the console; a fault, a signal
             * or an unknown end restarts the shell. */
            if (desktop_restartable && status != 0 && supervisor_restart_allowed(g_ticks)) {
                desktop_restarting = true;
                desktop_restart_old = pid;
                desktop_restart_deadline = g_ticks + DESKTOP_RESTART_WAIT_MS;
                klog("[supervisor] desktop died pid=%u status=%d deaths=%u; restarting", pid, status, desktop_deaths);
                proc_notify();
                return;
            }
            desktop_gone(pid, status);
        }
    }
}

void supervisor_observe(const char *probe, uint32_t pid)
{
    memset(observed_probe, 0, sizeof(observed_probe));
    /* Only controller-selected identifiers, never a user-supplied string. */
    if (strncmp(probe, "libc-smoke", sizeof("libc-smoke")) &&
        strncmp(probe, "app-gate", sizeof("app-gate"))) return;
    memcpy(observed_probe, probe, strlen(probe));
    observed_pid = pid;
    report_bytes = 0;
    supervisor_capture_init(&captures[0]);
    supervisor_capture_init(&captures[1]);
}

static void supervisor_frame(uint32_t pid, uint32_t tid, const char *stream,
                              uint64_t offset, const uint8_t *bytes, uint32_t length)
{
    static const char hex[] = "0123456789abcdef";
    for (unsigned off = 0; off < length;) {
        unsigned n = length - off > 24 ? 24 : length - off;
        char encoded[49];
        for (unsigned i = 0; i < n; i++) {
            encoded[2*i] = hex[bytes[off+i] >> 4];
            encoded[2*i+1] = hex[bytes[off+i] & 15];
        }
        encoded[2*n] = 0;
        rec_emit(observed_probe, "DATA", "group=app pid=%u tid=%u stream=%s offset=%llu bytes=%u data_hex=%s",
                 pid, tid, stream, offset + off, n, encoded);
        off += n;
    }
}

bool supervisor_report(struct task *task, const void *bytes, uint32_t length)
{
    struct proc_thread *t = proc_thread_for(task);
    if (!observed_probe[0] || !t || t->process->pid != observed_pid) return false;
    supervisor_frame(t->process->pid, t->tid, "report", report_bytes, bytes, length);
    report_bytes += length;
    return true;
}

bool supervisor_output(struct task *task, unsigned stream, const void *bytes, uint32_t length)
{
    struct proc_thread *t = proc_thread_for(task);
    if (!observed_probe[0] || !t || t->process->pid != observed_pid || stream < 1 || stream > 2)
        return false;
    supervisor_capture_add(&captures[stream - 1], bytes, length);
    return true;
}
const struct supervisor_capture *supervisor_captured(unsigned stream)
{
    return stream >= 1 && stream <= 2 ? &captures[stream - 1] : 0;
}

void supervisor_observe_end(void)
{
    if (!observed_probe[0]) return;
    /* One controller owns the observer; finalization cannot be re-entered.
     * Keep the bounded tail off its 8 KiB kernel stack, like captures[]. */
    static uint8_t tail[SUPERVISOR_CAPTURE_BYTES];
    for (unsigned i = 0; i < 2; i++) {
        const struct supervisor_capture *c = &captures[i];
        const char *stream = i ? "stderr" : "stdout";
        char digest[65];
        supervisor_capture_digest(c, digest);
        rec_emit(observed_probe, "DATA", "group=capture pid=%u stream=%s bytes=%llu truncated=%u",
                 observed_pid, stream, c->bytes, c->bytes > 2 * SUPERVISOR_CAPTURE_BYTES);
        rec_emit(observed_probe, "DATA", "group=capture_scan pid=%u stream=%s final_ok=%u assertion_failures=%u",
                 observed_pid, stream, c->final_ok, c->assertion_failures);
        rec_emit(observed_probe, "DATA", "group=capture_digest pid=%u stream=%s sha256=%s", observed_pid, stream, digest);
        supervisor_frame(observed_pid, 0, stream, 0, c->head, c->head_bytes);
        supervisor_capture_tail(c, tail);
        uint32_t skip = c->bytes < c->head_bytes + c->tail_bytes ?
            c->head_bytes + c->tail_bytes - (uint32_t)c->bytes : 0;
        supervisor_frame(observed_pid, 0, stream, c->bytes - c->tail_bytes + skip,
                          tail + skip, c->tail_bytes - skip);
    }
    observed_probe[0] = 0;
    observed_pid = 0;
}
