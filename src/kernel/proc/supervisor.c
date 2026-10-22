/* PID 1 bootstrap extension; process.c remains owner of orphan collection.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/supervisor.h>
#include <ciuki/probe.h>
#include <ciuki/audio.h>
#include <ciuki/desktop.h>
#include <ciuki/pager.h>
#include <ciuki/splash.h>
#pragma weak console_splash_phase
extern void dosvm_client_poll(void) __attribute__((weak));
extern void entropy_desktop_presented(void) __attribute__((weak)); /* f6-21 */
#pragma weak audio_mixer_delegate
#pragma weak proc_authority_install

static const struct supervisor_io_ops *supervisor_io;
extern void w32_service_delegate(struct process *,struct process *,const char *) __attribute__((weak));
static uint32_t desktop_pid, desktop_deaths;
static bool bootstrap_attempted;
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
uint32_t supervisor_desktop_pid(void) { return desktop_pid; }

int supervisor_spawn(const char *path, const char *const argv[], const char *cwd,
                      bool desktop, struct process **out)
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
    const char *env[] = { "LC_ALL=C", "TZ=UTC0", "HOME=/Documents", "TMPDIR=/tmp", "PATH=/Programs" };
    for (unsigned i = 0; i < ARRAY_SIZE(env) && !err; i++)
        err = proc_strings_add(strings, env[i], (uint32_t)strlen(env[i]) + 1, true);
    /* Prepare privately; stdio/cwd can block, so proc_spawn_file's immediate
     * publication cannot be used here. No provisional child becomes runnable. */
    struct process *p = 0;
    struct elf_image image;
    struct proc_thread *thread = 0;
    uint32_t esp = 0;
    if (!err) err = proc_prepare(proc_supervisor(), &p);
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
        err = supervisor_io && supervisor_io->stdio ? supervisor_io->stdio(p) : -ENODEV;
    }
    if (!err && cwd) err = supervisor_io && supervisor_io->cwd ? supervisor_io->cwd(p, cwd) : -ENODEV;
    if (!err && desktop) { int32_t grants[2]; err = grants_install(p, grants); }
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
    if (!err) err = proc_stack_build(p->memory, strings, image.entry, thread->tls, &esp);
    if (!err) {
        task_native_frame(thread->task, esp);
        if (desktop) {
            task_set_class(thread->task, P_INTERACTIVE);
            task_focus(thread->task);
        }
    }
    if (file.close) file.close(file.cookie);
    proc_strings_free(strings);
    if (err) { if (p) proc_discard(p); return err; }
    if(w32_service_delegate)w32_service_delegate(proc_supervisor(),p,path);
    proc_publish(p, CIUKI_SPAWN_NEW_GROUP);
    *out = p;
    return (int)p->pid;
}

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
    child->dosvm_capability = authorized && !strncmp(path,"/Programs/dosbox",sizeof("/Programs/dosbox"));
    child->network_capability = authorized &&
        (!strncmp(path,"/Programs/desktop",sizeof("/Programs/desktop")) || !strncmp(path,"/Programs/control",sizeof("/Programs/control")));
    child->programs_capability = authorized &&
        (!strncmp(path,"/Programs/desktop",sizeof("/Programs/desktop")) || !strncmp(path,"/Programs/control",sizeof("/Programs/control")));
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

void supervisor_bootstrap(void)
{
    if (bootstrap_attempted || (g_boot.flags & CBI_F_TEST_REQUEST)) return;
    bootstrap_attempted = true;
    const char *argv[] = { "desktop", 0 };
    struct process *p = 0;
    klog("[supervisor] bootstrap: spawning /Programs/desktop");
    if (console_splash_phase) console_splash_phase(SPLASH_PHASE_SUPERVISOR);
    int result = supervisor_spawn("/Programs/desktop", argv, 0, true, &p);
    if (result == -ENOENT) {
        result = supervisor_spawn_standin(proc_supervisor(), &p);
        if (result >= 0 && pager_bootstrap_complete) pager_bootstrap_complete();
        if (result >= 0 && entropy_desktop_presented) entropy_desktop_presented();
    }
    if (result < 0) {
        if (pager_bootstrap_complete) pager_bootstrap_complete();
        if (entropy_desktop_presented) entropy_desktop_presented(); /* f6-21: no desktop frame */
        console_wake();   /* a quiet boot shows its log instead of the desktop */
        klog("[supervisor] desktop unavailable: %d; console ready", result);
        return;
    }
    desktop_pid = p->pid;
    klog("[supervisor] desktop pid=%u pgid=%u", p->pid, p->pgid);
}

void supervisor_poll(void)
{
    if (dosvm_client_poll) dosvm_client_poll();
    if (desktop_pid) {
        struct process *p = proc_find(desktop_pid);
        if (!p || p->state == PROC_STOPPING || p->state == PROC_ZOMBIE) {
            uint32_t pid=desktop_pid;
            desktop_pid = 0;
            if (pager_bootstrap_complete) pager_bootstrap_complete();
            if (entropy_desktop_presented) entropy_desktop_presented();
            desktop_deaths++;
            console_wake();
            klog("[supervisor] desktop stopped; console ready pid=%u status=%d deaths=%u", pid, p ? p->status : -1, desktop_deaths);
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
