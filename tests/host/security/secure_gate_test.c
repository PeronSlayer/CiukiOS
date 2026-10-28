/* H09: the trusted gate with the real secure-console backend.
 * Production C: security.c (k01 coordinator), secure_console.c (k07 backend),
 * grants.c (domains, fences, neutral frame), spawn.c (SESSION_LAUNCH),
 * channel.c (readiness, kernel notification), surface/fbdev presentation
 * and, in secure_input_queue.c, the merged native input queue (PS/2 set-2
 * decoder, USB and firmware producers, secure attention). Fakes stand only at
 * the boundaries: RAM/scheduler, the framebuffer memory, the owner store,
 * executable bytes, the session console device and the DOS/Win32/display
 * worker subsystems whose calls are counted. Expected results are literal.
 * SPDX-License-Identifier: GPL-2.0-only */
#define CIUKI_DESKTOP_REAL_INPUT
#define CIUKI_DESKTOP_HOST_MAIN secure_unused_desktop_main
#include "../proc/desktop_test.c"
#undef main
#include <stdarg.h>
#include <ciuki/fwinput.h>
#include <ciuki/init.h>
#include "../../../src/kernel/proc/secure_console.c"

void secure_test_native(bool firmware, gen_t generation);
void secure_test_ps2(uint8_t byte, uint64_t tick);
bool secure_test_held(uint16_t code);
unsigned secure_test_pending(void);

/* ---- boundary fakes --------------------------------------------------- */
static unsigned attention_logs, drained_logs;
void klog(const char *fmt, ...)
{
    char record[256]; va_list ap; va_start(ap, fmt);
    kvsnprintf(record, sizeof(record), fmt, ap); va_end(ap);
    if (strstr(record, "[security]")) printf("  log: %s\n", record);
    if (strstr(record, "[security] secure attention source=keyboard")) attention_logs++;
    if (strstr(record, "[security] session drained")) drained_logs++;
}
void klog_periodic(const char *fmt, ...) __attribute__((alias("klog")));
bool drivers_input_firmware(void) { return false; }
/* core/sync.c semantics; that unit carries scheduler code this fixture fakes. */
bool gen_matches(gen_t current, gen_t supplied) { return supplied && current == supplied; }
static bool fake_fullscreen, fake_present_lease;
static unsigned worker_wakes, console_changes, dos_events, dos_resyncs, dos_grabs;
bool display_fullscreen_active(void) { return fake_fullscreen; }
bool display_present_active(void) { return fake_present_lease; }
void display_worker_wake(void) { worker_wakes++; }
void display_console_changed(void) { console_changes++; }
/* DOS full-screen hook: records what the merged queue offers the guest. */
static bool dos_grab;
bool dosvm_fullscreen_input(uint16_t type, uint16_t code, int32_t value)
{
    (void)code; (void)value;
    if (type == INPUT_RESYNC) { dos_resyncs++; return false; }
    dos_events++;
    if (dos_grab) dos_grabs++;
    return dos_grab;
}
static unsigned dos_revokes, w32_revokes, dos_left, w32_left;
static uint32_t revoked_session;
unsigned dosvm_session_revoke(uint32_t session)
{ dos_revokes++; revoked_session = session; unsigned left = dos_left; if (dos_left) dos_left--; return left; }
unsigned w32_session_revoke(uint32_t session) { w32_revokes++; CHECK(session == revoked_session); return w32_left; }

/* Session console device: one object at 0..3, as fdtable.c builds it (the
 * real /dev/console description is qualified by the files program). */
struct console_object { struct proc_object object; unsigned refs; };
static unsigned consoles;
static void console_retain(struct proc_object *o) { ((struct console_object *)o)->refs++; }
static void console_release(struct proc_object *o)
{ struct console_object *c = (struct console_object *)o; if (!--c->refs) { consoles--; kfree(c); } }
int files_session_console(struct process *p, int32_t *console)
{
    CHECK(p && !p->published && p->state == PROC_PREPARING);
    for (unsigned i = 0; i < 4; i++) if (p->fds[i].object) return -EBUSY;
    struct console_object *c = kzalloc(sizeof(*c));
    if (!c) return -ENOMEM;
    c->object = (struct proc_object){ console_retain, console_release, false }; c->refs = 4; consoles++;
    for (unsigned i = 0; i < 4; i++) p->fds[i] = (struct proc_fd){ &c->object, 0 };
    *console = 3;
    return 0;
}
int security_profile_timezone(const struct security_subject *subject, const struct ciuki_security_profile *profile,
                              char *out, uint32_t bytes)
{ (void)subject; (void)profile; (void)out; (void)bytes; return -ENOENT; }
int security_exec_open(struct process *p, const char *path, struct ciuki_file *out)
{
    CHECK(!p->published && p->state == PROC_PREPARING && security_credential(p)->session_id);
    CHECK(!strcmp(path, "/Programs/desktop") || !strcmp(path, "/System/Win32/w32srv"));
    *out = fixture_file(); return 0;
}
int security_owner_verify(const struct ciuki_security_profile *p) { (void)p; return 0; }
int px_security_cwd(struct process *p, const char *path)
{ CHECK(p->state == PROC_PREPARING && !p->published && strstr(path, "/Documents")); return 0; }

/* ---- call helpers (same wire as H01) ---------------------------------- */
#define SEC_ARGS (CIUKI_IMAGE_BASE + PAGE_SIZE + 0x100)
#define SEC_OUT (SEC_ARGS + 0x100)
#define SEC_SPAWN (SEC_ARGS + 0x400)
#define INBUF (CIUKI_IMAGE_BASE + PAGE_SIZE + 0x800)
/* Direct host dispatch has no return-to-user epilogue: hand the CPU back to
 * the fixture controller so a later proc_stop sees a parked thread. */
static void park(void) { if (g_current != &controller) g_current->state = T_READY; g_current = &controller; }
static int invoke(struct proc_thread *t, struct ciuki_security_args *a)
{
    desktop_select(t);
    CHECK(!ua_write(t->process->memory, SEC_ARGS, a, 64));
    struct trap_frame tf = { .eax = 101, .ebx = SEC_ARGS, .ecx = 64 };
    int err = proc_syscall(&tf);
    struct ciuki_security_args after;
    CHECK(!ua_read(t->process->memory, &after, SEC_ARGS, 64));
    if (err) CHECK(!memcmp(a, &after, 64)); else *a = after;
    park();
    return err;
}
static int extended_call(struct proc_thread *t, uint32_t op, uint32_t session, const void *in, void *out, uint32_t size)
{
    CHECK(!ua_write(t->process->memory, SEC_SPAWN, in, size));
    struct ciuki_security_args a = { .version = 1, .size = 64, .operation = op, .session_id = session,
        .spawn_address = SEC_SPAWN, .output_address = SEC_OUT, .output_bytes = size };
    int err = invoke(t, &a);
    if (!err && out) CHECK(!ua_read(t->process->memory, out, SEC_OUT, size));
    return err;
}
static uint32_t session_end(struct proc_thread *logon, uint32_t id, uint32_t flags)
{
    struct ciuki_security_args a = { .version = 1, .size = 64, .operation = 6, .session_id = id, .flags = flags };
    CHECK(!invoke(logon, &a)); return a.state;
}
static struct ciuki_security_launch launch_input(uint32_t id, uint32_t purpose)
{ return (struct ciuki_security_launch){ .version = 1, .size = 96, .session_id = id,
    .session_generation = security_session_generation(id), .purpose = purpose }; }
static int launch_call(struct proc_thread *t, uint32_t id, uint32_t purpose, struct ciuki_security_launch *out)
{ struct ciuki_security_launch in = launch_input(id, purpose); return extended_call(t, 8, id, &in, out, 96); }
static int poll_call(struct proc_thread *t, struct ciuki_security_launch *l)
{
    struct ciuki_security_launch in = launch_input(l->session_id, l->purpose);
    in.launch_id = l->launch_id; in.launch_generation = l->launch_generation;
    return extended_call(t, 9, l->session_id, &in, l, 96);
}
static int enter_call(struct proc_thread *t, uint32_t id, uint32_t reason, struct ciuki_security_console *out)
{
    struct ciuki_security_console in = { .version = 1, .size = 64, .session_id = id,
        .session_generation = id ? security_session_generation(id) : 0, .reason = reason };
    return extended_call(t, 10, id, &in, out, 64);
}
static int leave_call(struct proc_thread *t, struct ciuki_security_console *in)
{
    in->state = 0; struct ciuki_security_console out;
    return extended_call(t, 11, in->session_id, in, &out, 64);
}
static struct proc_thread *thread_of(struct process *p)
{
    for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++)
        if (proc_thread_slot(i) && proc_thread_slot(i)->process == p) return proc_thread_slot(i);
    CHECK(false); return 0;
}
static struct proc_thread *role_process_fixture(uint32_t role, struct process **out)
{
    CHECK(!proc_prepare(proc_supervisor(), out));
    CHECK(!security_role_prepare(proc_supervisor(), *out, role, security_role_image(role)));
    struct ciuki_file f = fixture_file(); struct proc_strings *s = proc_strings_new();
    CHECK(s && !proc_strings_add(s, "role", 5, false)); CHECK(!prepare_image(*out, &f, s, 0)); proc_strings_free(s);
    if (role == CIUKI_SECURITY_ROLE_LOGON_UI) CHECK(!security_trusted_prepare(*out));
    proc_publish(*out, 0);
    (*out)->actions[SIGCHLD].handler = SIG_IGN;
    return thread_of(*out);
}
static void ready_child(const struct ciuki_security_launch *l, int status)
{
    struct process *p = proc_find(l->pid); CHECK(p);
    desktop_select(thread_of(p));
    struct ciuki_message m = { .length = 16 }; uint32_t words[] = { 1, l->launch_id, l->launch_generation, (uint32_t)status };
    memcpy(m.data, words, 16); CHECK(!ua_write(p->memory, SEC_OUT, &m, sizeof(m)));
    CHECK(!channel_send(p, l->child_ready_fd, SEC_OUT, DONTWAIT));
    park();
}
/* PID 1's pass (supervisor_main): supervisor_poll's k01 deadlines and k07
 * attention/neutral/drain work, then orphan/zombie collection. */
static void pid1(void) { g_current = &controller; security_poll(); secure_console_poll(); proc_collect(); }

/* ---- display and input observation ------------------------------------ */
#define FB_W 16u
#define FB_H 4u
static uint32_t framebuffer[FB_W * FB_H];
static void display_fixture(void)
{
    device = (struct fb_device){ .present = true, .width = FB_W, .height = FB_H, .pitch = FB_W * 4,
        .size = sizeof(framebuffer), .mapped = (uint8_t *)framebuffer, .bpp = 32, .red_size = 8, .red_pos = 16,
        .green_size = 8, .green_pos = 8, .blue_size = 8, .blue_pos = 0, .handle = 0, .generation = 1 };
    initialized = true; kmutex_init(&presenter);
}
static int surface_of(struct process *p, uint32_t colour)
{
    int fd = surface_create(p, FB_W, FB_H, 1); CHECK(fd >= 0);
    struct surface *s = desktop_fd(p, fd)->u.surface;
    uint32_t *pixels = P2V(surface_page(s, 0));
    for (unsigned i = 0; i < FB_W * FB_H; i++) pixels[i] = colour;
    return fd;
}
static int present(struct process *p, int32_t display, int32_t surface)
{
    desktop_select(thread_of(p));
    struct ciuki_rect r = { 0, 0, 0, 0, FB_W, FB_H };
    int err = grant_present(p, display, surface, &r);
    park();
    return err;
}
static bool screen_is(uint32_t colour)
{
    for (unsigned i = 0; i < FB_W * FB_H; i++) if ((framebuffer[i] & 0xffffffu) != (colour & 0xffffffu)) return false;
    return true;
}
static int read_input(struct process *p, int32_t fd, struct ciuki_input_event *out, unsigned capacity)
{
    desktop_select(thread_of(p));
    int n = grant_input_read(p, fd, INBUF, capacity);
    if (n > 0) CHECK(!ua_read(p->memory, out, INBUF, (unsigned)n * sizeof(*out)));
    park();
    return n;
}
/* USB keyboard producer (usb_hid.c input_usb_event) */
static uint64_t tick = 1000;
static void usb_key(uint16_t code, int value) { input_usb_event(INPUT_KEY, code, value, 71, ++tick); }
static void ctrl_alt_del_usb(void) { usb_key(0x1d, 1); usb_key(0x38, 1); usb_key(0x153, 1); }
static void release_usb(void) { usb_key(0x153, 0); usb_key(0x38, 0); usb_key(0x1d, 0); }
static bool has_event(const struct ciuki_input_event *e, int n, uint32_t type, int32_t code)
{ for (int i = 0; i < n; i++) if (e[i].type == type && e[i].code == code) return true; return false; }
static void drain_reader(struct process *p, int32_t fd)
{ struct ciuki_input_event e[16]; while (read_input(p, fd, e, 16) > 0) {} }

/* ---- cases --------------------------------------------------------------- */
static void test_interim_desktop(void)
{
    /* No LOGON role yet: the interim uid-1000 desktop keeps Ctrl+Alt+Del as
     * an ordinary sequence (Task Manager) and its single grant. */
    CHECK(!security_roles_active() && !security_attention_armed());
    struct process *p;
    CHECK(!proc_prepare(proc_supervisor(), &p));
    CHECK(!security_bootstrap_desktop(p));
    struct ciuki_file f = fixture_file(); struct proc_strings *s = proc_strings_new();
    CHECK(s && !proc_strings_add(s, "desktop", 8, false)); CHECK(!prepare_image(p, &f, s, 0)); proc_strings_free(s);
    proc_publish(p, 0);
    int32_t grants[2]; CHECK(!grants_install(p, grants));
    CHECK(!desktop_fcntl(p, grants[1], F_SETFL, O_NONBLOCK));
    CHECK(grants_display_pid() == p->pid && grants_display_owner(p));
    int surface = surface_of(p, 0x00123456);
    CHECK(!present(p, grants[0], surface) && screen_is(0x123456));
    dos_events = 0; ctrl_alt_del_usb();
    struct ciuki_input_event e[16]; int n = read_input(p, grants[1], e, 16);
    CHECK(n == 3 && has_event(e, n, CIUKI_INPUT_KEY, 0x1d) && has_event(e, n, CIUKI_INPUT_KEY, 0x38) &&
          has_event(e, n, CIUKI_INPUT_KEY, 0x153));
    CHECK(dos_events == 3 && !attention_pending && !input_secure_trusted());
    release_usb(); drain_reader(p, grants[1]);
    /* Ctrl+Alt+Del (keypad Del, PS/2 set 2) is equally ordinary here. */
    secure_test_ps2(0x14, ++tick); secure_test_ps2(0x11, ++tick); secure_test_ps2(0x71, ++tick);
    n = read_input(p, grants[1], e, 16);
    CHECK(n == 3 && has_event(e, n, CIUKI_INPUT_KEY, 0x53));
    secure_test_ps2(0xF0, ++tick); secure_test_ps2(0x71, ++tick); secure_test_ps2(0xF0, ++tick);
    secure_test_ps2(0x11, ++tick); secure_test_ps2(0xF0, ++tick); secure_test_ps2(0x14, ++tick);
    drain_reader(p, grants[1]);
    struct grants_console_state st; grants_console_snapshot(&st);
    CHECK(!st.trusted && !st.trusted_owner && st.user_owner == p->pid);
    uint32_t bootstrap = security_credential(p)->session_id, state;
    stop_collect(p);
    CHECK(!grants_display_pid() && !desktop_objects.grants);
    /* The interim session survives its desktop (k01 restart rule). A boot
     * with LOGON never has one; retire it so this harness can continue. */
    CHECK(!security_session_end(bootstrap, true, &state) && state == CIUKI_SECURITY_SESSION_CLOSING);
    pid1();
    CHECK(!security_session_end(bootstrap, false, &state) && state == CIUKI_SECURITY_SESSION_DEAD);
    puts("H09 interim: no LOGON, Ctrl+Alt+Del (USB and PS/2 keypad) reaches the bootstrap desktop and the DOS hook as before PASS");
}

struct gate {
    struct proc_thread *logon, *ui, *account;
    struct process *logon_p, *ui_p, *account_p;
    int32_t ui_display, ui_input, ui_surface;
};
static void ui_descriptors(struct gate *g)
{
    g->ui_display = g->ui_input = -1;
    for (int32_t fd = 0; fd < CIUKI_OPEN_MAX; fd++) {
        struct desktop_description *d = desktop_fd(g->ui_p, fd);
        if (!d) continue;
        CHECK(d->grant_trusted && !d->object.inheritable && (g->ui_p->fds[fd].flags & FD_CLOEXEC));
        if (d->kind == DESKTOP_DISPLAY) g->ui_display = fd;
        if (d->kind == DESKTOP_INPUT) g->ui_input = fd;
    }
    CHECK(g->ui_display == 4 && g->ui_input == 5);
    for (unsigned i = 0; i < 4; i++) CHECK(g->ui_p->fds[i].object == g->ui_p->fds[0].object);
    CHECK(!desktop_fcntl(g->ui_p, g->ui_input, F_SETFL, O_NONBLOCK));
    g->ui_surface = surface_of(g->ui_p, 0x000000ff);
}
static void ui_start(struct gate *g)
{
    g->ui = role_process_fixture(CIUKI_SECURITY_ROLE_LOGON_UI, &g->ui_p);
    ui_descriptors(g);
}
static void test_boot_gate(struct gate *g)
{
    CHECK(security_grants == &secure_console_ops); /* security_init's production registration */
    g->account = role_process_fixture(CIUKI_SECURITY_ROLE_ACCOUNT, &g->account_p);
    g->logon = role_process_fixture(CIUKI_SECURITY_ROLE_LOGON, &g->logon_p);
    CHECK(security_attention_armed());
    ui_start(g);
    struct grants_console_state st; grants_console_snapshot(&st);
    CHECK(st.trusted && st.trusted_owner == g->ui_p->pid && !st.user_owner && input_secure_trusted());
    CHECK(!grants_display_pid()); /* no user owner: DOS full screen and present leases refuse */
    CHECK(!present(g->ui_p, g->ui_display, g->ui_surface) && screen_is(0xff));
    /* Physical Ctrl+Alt+Del at the boot gate: consumed, ATTENTION to LOGON_UI. */
    struct ciuki_input_event first[4];
    CHECK(read_input(g->ui_p, g->ui_input, first, 4) == 1 && first[0].type == CIUKI_INPUT_RESYNC);
    unsigned before_attention = (unsigned)attention_handled;
    dos_events = 0;
    ctrl_alt_del_usb();
    CHECK(dos_events == 0 && attention_pending == 1);
    struct ciuki_input_event e[16]; int n = read_input(g->ui_p, g->ui_input, e, 16);
    CHECK(n == 3 && e[0].type == CIUKI_INPUT_KEY && e[0].code == 0x1d && e[1].code == 0x38 &&
          e[2].type == CIUKI_INPUT_ATTENTION && e[2].code == CIUKI_SECURITY_SECURE_ATTENTION);
    uint32_t epoch = boot_console.authentication_epoch;
    pid1();
    CHECK(attention_handled == before_attention + 1 && !attention_result && attention_logs == 1);
    CHECK(boot_console.authentication_epoch == epoch + 1 && boot_console.reason == CIUKI_SECURITY_SECURE_ATTENTION);
    usb_key(0x153, 2); usb_key(0x153, 0); /* repeat and release stay swallowed */
    n = read_input(g->ui_p, g->ui_input, e, 16); CHECK(n == -EAGAIN);
    usb_key(0x38, 0); usb_key(0x1d, 0);
    n = read_input(g->ui_p, g->ui_input, e, 16);
    CHECK(n == 2 && e[0].code == 0x38 && !e[0].value && e[1].code == 0x1d);
    /* LOGON's session-0 ENTER(ATTENTION) is idempotent for the bound tuple. */
    struct ciuki_security_console boot, again;
    CHECK(!enter_call(g->logon, 0, CIUKI_SECURITY_SECURE_ATTENTION, &boot));
    CHECK(!enter_call(g->logon, 0, CIUKI_SECURITY_SECURE_ATTENTION, &again) && !memcmp(&boot, &again, 64));
    CHECK(leave_call(g->logon, &boot) == -EACCES);
    puts("H09 boot gate: production backend registered; LOGON_UI private console 0..3, trusted pair 4/5; physical Ctrl+Alt+Del consumed, ATTENTION delivered, epochs advanced, swallowed repeat/release PASS");
}

struct session { uint32_t id; struct ciuki_security_launch desk, server; struct process *dp, *sp; int32_t surface; };
static void session_create(struct session *s, uint32_t uid)
{
    struct ciuki_credential c = { .version = 1, .size = 64, .uid = uid, .primary_gid = 545,
        .group_count = 2, .groups = { 544, 545 }, .flags = CIUKI_CRED_INTERACTIVE };
    struct ciuki_security_profile profile = { .version = 1, .size = 112, .uid = uid,
        .volume_id_lo = 31, .owner_generation_lo = 17, .owner_key_lo = 23 };
    ksnprintf(profile.path, sizeof(profile.path), "/Windows/Profiles/u%08x", uid);
    profile.path_bytes = (uint32_t)strlen(profile.path) + 1;
    memset(s, 0, sizeof(*s));
    CHECK(!security_session_create(&c, &profile, &s->id));
}
static void session_launch_pair(struct gate *g, struct session *s)
{
    CHECK(!launch_call(g->logon, s->id, CIUKI_SECURITY_LAUNCH_W32SRV, &s->server));
    CHECK(!launch_call(g->logon, s->id, CIUKI_SECURITY_LAUNCH_DESKTOP, &s->desk));
    s->dp = proc_find(s->desk.pid); s->sp = proc_find(s->server.pid);
    CHECK(s->dp && s->sp && security_role(s->dp) == 6 && security_role(s->sp) == 7);
    /* Real backend layout: one console at 0..3, desktop pair at 4/5. */
    CHECK(s->desk.child_console_fd == 3 && s->desk.child_display_fd == 4 && s->desk.child_input_fd == 5 &&
          s->desk.child_service_fd == -1);
    CHECK(s->server.child_console_fd == 3 && s->server.child_service_fd == 4 &&
          s->server.child_display_fd == -1 && s->server.child_input_fd == -1);
    for (unsigned i = 0; i < 4; i++) CHECK(s->dp->fds[i].object == s->dp->fds[3].object &&
                                           s->sp->fds[i].object == s->sp->fds[3].object);
    struct desktop_description *display = desktop_fd(s->dp, 4), *input = desktop_fd(s->dp, 5);
    CHECK(display && input && !display->grant_trusted && display->grant_session == s->id &&
          display->kind == DESKTOP_DISPLAY && input->kind == DESKTOP_INPUT);
    CHECK(desktop_object_is_channel(s->sp->fds[4].object) && !s->sp->fds[4].object->inheritable);
    CHECK(!desktop_fcntl(s->dp, 5, F_SETFL, O_NONBLOCK));
    s->surface = surface_of(s->dp, 0x00ff0000);
    /* Suspended until activation: no frame, no input. */
    uint32_t shown = framebuffer[0];
    CHECK(present(s->dp, 4, s->surface) == -EBUSY && screen_is(shown));
    struct ciuki_input_event e[4]; CHECK(read_input(s->dp, 5, e, 4) == -EAGAIN);
    CHECK(!grants_display_pid());
}
static void session_activate(struct gate *g, struct session *s)
{
    ready_child(&s->server, 0); CHECK(!poll_call(g->logon, &s->server) && s->server.state == 2);
    unsigned changes = console_changes;
    ready_child(&s->desk, 0); CHECK(!poll_call(g->logon, &s->desk) && s->desk.state == 3);
    CHECK(console_changes == changes + 1 && !input_secure_trusted() && grants_display_pid() == s->dp->pid);
    CHECK(!present(s->dp, 4, s->surface) && screen_is(0xff0000));
    CHECK(present(g->ui_p, g->ui_display, g->ui_surface) == -EBUSY && screen_is(0xff0000));
    struct ciuki_input_event e[4];
    CHECK(read_input(g->ui_p, g->ui_input, e, 4) == -EAGAIN);
    int n = read_input(s->dp, 5, e, 4); CHECK(n == 1 && e[0].type == CIUKI_INPUT_RESYNC);
}
static void test_launch_rollback(struct gate *g)
{
    struct session s; session_create(&s, 1201);
    struct proc_ledger baseline, after; proc_snapshot(&baseline);
    struct desktop_ledger desk_before; desktop_snapshot(&desk_before);
    unsigned heap = heap_blocks, pages = pages_used, failures = 0, console_before = consoles; bool ok = false;
    for (int fail = 0; fail < 200 && !ok; fail++) {
        struct ciuki_security_launch in = launch_input(s.id, CIUKI_SECURITY_LAUNCH_DESKTOP), out;
        CHECK(!ua_write(g->logon->process->memory, SEC_SPAWN, &in, 96));
        struct ciuki_security_args a = { .version = 1, .size = 64, .operation = 8, .session_id = s.id,
            .spawn_address = SEC_SPAWN, .output_address = SEC_OUT, .output_bytes = 96 };
        CHECK(!ua_write(g->logon->process->memory, SEC_ARGS, &a, 64)); desktop_select(g->logon);
        struct trap_frame tf = { .eax = 101, .ebx = SEC_ARGS, .ecx = 64 }; alloc_fail = fail;
        int err = proc_syscall(&tf); alloc_fail = -1;
        if (!err) { CHECK(!ua_read(g->logon->process->memory, &out, SEC_OUT, 96)); s.desk = out; ok = true; break; }
        CHECK(err == -ENOMEM || err == -EIO); failures++;
        proc_snapshot(&after); CHECK(!memcmp(&baseline, &after, sizeof(after)));
        struct desktop_ledger desk_after; desktop_snapshot(&desk_after);
        CHECK(desk_after.grants == desk_before.grants && desk_after.descriptions == desk_before.descriptions);
        CHECK(heap_blocks == heap && pages_used == pages && consoles == console_before);
        struct grants_console_state st; grants_console_snapshot(&st); CHECK(!st.user_owner);
    }
    CHECK(ok && failures > 10);
    struct grants_console_state st; grants_console_snapshot(&st);
    CHECK(st.user_owner == s.desk.pid && st.user_session == s.id && st.trusted);
    /* A second user grant set is refused while the first lingers. */
    struct process *other; CHECK(!proc_prepare(proc_supervisor(), &other)); int32_t fds[2];
    CHECK(grants_install_session(other, s.id + 7, fds) == -EBUSY); proc_discard(other);
    CHECK(session_end(g->logon, s.id, CIUKI_SECURITY_END_BEGIN) == CIUKI_SECURITY_SESSION_CLOSING);
    proc_collect();
    while (session_end(g->logon, s.id, CIUKI_SECURITY_END_COLLECT) != CIUKI_SECURITY_SESSION_DEAD) { pid1(); proc_collect(); }
    grants_console_snapshot(&st); CHECK(!st.user_owner && st.trusted);
    printf("H09 launch rollback: %u allocation/console/grant failures publish nothing and leave no grant, console or description PASS\n", failures);
}
static uint32_t paint_count;
static void conceal_mid_present(void)
{
    if (paint_count++ != 0) return;
    on_preempt_point = 0;
    /* Kernel physical path at a tile boundary of an admitted user present. */
    CHECK(security_console_attention(CIUKI_SECURITY_SECURE_ATTENTION) == -EBUSY);
    struct grants_console_state st; grants_console_snapshot(&st);
    CHECK(st.trusted && st.neutral_pending && st.draining);
}
static void test_activation_and_attention(struct gate *g, struct session *s)
{
    session_create(s, 1300);
    session_launch_pair(g, s);
    session_activate(g, s);
    /* w32srv accept notification on child_service_fd, level-triggered. */
    secure_service_notify(s->id); secure_service_notify(s->id);
    desktop_select(thread_of(s->sp));
    CHECK(channel_recv(s->sp, 4, BUFFER, DONTWAIT) == 1);
    struct ciuki_message m; CHECK(!ua_read(s->sp->memory, &m, BUFFER, sizeof(m)));
    uint32_t words[4]; memcpy(words, m.data, 16);
    CHECK(m.length == 16 && !m.sender_pid && words[0] == 1 && words[1] == 1 && words[2] == s->id);
    CHECK(channel_recv(s->sp, 4, BUFFER, DONTWAIT) == -EAGAIN);
    /* Admitted user present concealed at a tile boundary: no later row. */
    uint32_t *pixels = P2V(surface_page(desktop_fd(s->dp, s->surface)->u.surface, 0));
    for (unsigned i = 0; i < FB_W * FB_H; i++) pixels[i] = 0x0000ff00;
    paint_count = 0; on_preempt_point = conceal_mid_present;
    CHECK(present(s->dp, 4, s->surface) == -EBUSY);
    CHECK(on_preempt_point == 0);
    for (unsigned y = 0; y < FB_H; y++) for (unsigned x = 0; x < FB_W; x++)
        CHECK((framebuffer[y * FB_W + x] & 0xffffff) == (y == 0 ? 0x00ff00u : 0xff0000u));
    CHECK(present(g->ui_p, g->ui_display, g->ui_surface) == -EBUSY); /* neutral frame owed first */
    pid1();
    struct grants_console_state st; grants_console_snapshot(&st);
    CHECK(!st.neutral_pending && st.neutral_frames && screen_is(0));
    CHECK(!present(g->ui_p, g->ui_display, g->ui_surface) && screen_is(0xff));
    CHECK(present(s->dp, 4, s->surface) == -EBUSY && grants_display_pid() == 0 && !grants_display_owner(s->dp));
    /* The fence failed under the physical path: LOGON's ENTER mints the tuple. */
    struct ciuki_security_console menu;
    CHECK(!enter_call(g->logon, s->id, CIUKI_SECURITY_SECURE_ATTENTION, &menu) && !menu.authentication_required &&
          (menu.restore_cookie_lo | menu.restore_cookie_hi));
    /* Physical Ctrl+Alt+Del from USER: immediate input cut, before PID 1. */
    CHECK(!leave_call(g->logon, &menu) && !input_secure_trusted() && grants_display_pid() == s->dp->pid);
    drain_reader(s->dp, 5);
    CHECK(!present(s->dp, 4, s->surface));
    usb_key(0x1d, 1); usb_key(0x38, 1);
    struct ciuki_input_event e[16]; int n = read_input(s->dp, 5, e, 16); CHECK(n == 2);
    usb_key(0x153, 1);
    CHECK(input_secure_trusted() && read_input(s->dp, 5, e, 16) == -EAGAIN);
    n = read_input(g->ui_p, g->ui_input, e, 16);
    CHECK(n == 2 && e[0].type == CIUKI_INPUT_RESYNC && e[1].type == CIUKI_INPUT_ATTENTION);
    /* Keys typed into the trusted UI never reach the user after LEAVE. */
    usb_key(0x153, 0); usb_key(0x38, 0); usb_key(0x1d, 0); usb_key(0x1e, 1); usb_key(0x1e, 0);
    pid1();
    CHECK(attention_logs == 2 && attention_result == -EBUSY && screen_is(0)); /* conceal fenced, neutral painted */
    const struct ciuki_credential before = *security_credential(s->dp);
    s->dp->cpu_ticks += 23;
    CHECK(!enter_call(g->logon, s->id, CIUKI_SECURITY_SECURE_ATTENTION, &menu) && !menu.authentication_required);
    CHECK(!leave_call(g->logon, &menu));
    n = read_input(s->dp, 5, e, 16);
    CHECK(n == 1 && e[0].type == CIUKI_INPUT_RESYNC); /* trusted keystrokes discarded */
    CHECK(s->dp->state == PROC_LIVE && s->dp->cpu_ticks == 23 && !memcmp(&before, security_credential(s->dp), 64));
    CHECK(leave_call(g->logon, &menu) == -EAGAIN); /* one-use cookie */
    puts("H09 activation/attention: suspended launch grants, initial activation, w32srv notification, mid-present conceal (no later row), neutral frame before trusted pixels, immediate input cut, trusted keystrokes discarded, one-use cookie, unchanged credential/progress PASS");
}
static void test_lock_attention_cancel(struct gate *g, struct session *s)
{
    /* Sequence (2): LOCK -> physical Ctrl+Alt+Delete -> Cancel. */
    struct ciuki_security_console lock;
    int err = enter_call(g->logon, s->id, CIUKI_SECURITY_SECURE_LOCK, &lock);
    CHECK(err == -EBUSY); pid1();                 /* neutral frame owed */
    CHECK(!enter_call(g->logon, s->id, CIUKI_SECURITY_SECURE_LOCK, &lock) && lock.authentication_required);
    ctrl_alt_del_usb(); pid1();
    CHECK(attention_result == 0); /* already trusted: no fence, new epochs/cookie */
    CHECK(session_find(s->id)->console.authentication_required && session_find(s->id)->console.authentication_epoch > lock.authentication_epoch);
    CHECK(leave_call(g->logon, &lock) == -EAGAIN);
    struct ciuki_input_event e[16];
    CHECK(present(s->dp, 4, s->surface) == -EBUSY && read_input(s->dp, 5, e, 16) == -EAGAIN);
    struct ciuki_security_console menu;
    CHECK(!enter_call(g->logon, s->id, CIUKI_SECURITY_SECURE_ATTENTION, &menu) && menu.authentication_required);
    release_usb(); drain_reader(g->ui_p, g->ui_input);
    /* Cancel = no LEAVE: a later desktop POLL cannot release the gate. */
    CHECK(!poll_call(g->logon, &s->desk) && present(s->dp, 4, s->surface) == -EBUSY && input_secure_trusted());
    /* Correlated owner success: LOGON's current LEAVE restores the same session. */
    CHECK(!leave_call(g->logon, &menu) && !present(s->dp, 4, s->surface));
    drain_reader(s->dp, 5);
    puts("H09 sequence 2: LOCK, physical Ctrl+Alt+Delete keeps authentication_required, stale LOCK cookie EAGAIN, no frame/input, POLL cannot release, current LEAVE restores PASS");
}
static void test_pending_launch_lock(struct gate *g)
{
    /* Sequence (1): pending launch -> SECURE_ENTER(LOCK) -> ready -> POLL. */
    struct session s; session_create(&s, 1400);
    session_launch_pair(g, &s);
    struct ciuki_security_console lock;
    CHECK(!enter_call(g->logon, s.id, CIUKI_SECURITY_SECURE_LOCK, &lock) && lock.authentication_required);
    ready_child(&s.server, 0); CHECK(!poll_call(g->logon, &s.server) && s.server.state == 2);
    ready_child(&s.desk, 0); CHECK(!poll_call(g->logon, &s.desk) && s.desk.state == 2);
    CHECK(present(s.dp, 4, s.surface) == -EBUSY && input_secure_trusted() && !grants_display_pid());
    CHECK(!leave_call(g->logon, &lock) && !present(s.dp, 4, s.surface) && screen_is(0xff0000));
    CHECK(!poll_call(g->logon, &s.desk) && s.desk.state == 3);
    /* Logoff drain: DOS VMs and Win32 objects keep the session CLOSING. */
    dos_left = 2; w32_left = 0; unsigned dos_before = dos_revokes;
    CHECK(session_end(g->logon, s.id, CIUKI_SECURITY_END_BEGIN) == CIUKI_SECURITY_SESSION_CLOSING);
    CHECK(input_secure_trusted() && !grants_display_pid() && s.dp->state != PROC_LIVE);
    pid1(); CHECK(screen_is(0)); /* neutral frame at logoff */
    proc_collect();
    struct ciuki_security_console probe;
    CHECK(enter_call(g->logon, s.id, CIUKI_SECURITY_SECURE_LOCK, &probe) == -ESRCH);
    CHECK(leave_call(g->logon, &lock) == -ESRCH);       /* stale tuple cannot resurrect */
    CHECK(session_end(g->logon, s.id, CIUKI_SECURITY_END_COLLECT) == CIUKI_SECURITY_SESSION_CLOSING);
    uint32_t blocked;
    struct ciuki_credential c = *security_credential(g->logon_p); (void)c;
    struct session next; next.id = 0;
    struct ciuki_credential tmpl = { .version = 1, .size = 64, .uid = 1500, .primary_gid = 545, .group_count = 1,
        .groups = { 545 }, .flags = CIUKI_CRED_INTERACTIVE };
    struct ciuki_security_profile profile = { .version = 1, .size = 112, .uid = 1500, .volume_id_lo = 31,
        .owner_generation_lo = 17, .owner_key_lo = 23 };
    ksnprintf(profile.path, sizeof(profile.path), "/Windows/Profiles/u%08x", 1500u);
    profile.path_bytes = (uint32_t)strlen(profile.path) + 1;
    CHECK(security_session_create(&tmpl, &profile, &blocked) == -EBUSY); /* undrained CLOSING blocks logon */
    pid1(); pid1(); pid1();
    CHECK(dos_revokes >= dos_before + 3 && revoked_session == s.id && w32_revokes >= 3 && drained_logs >= 1);
    CHECK(session_end(g->logon, s.id, CIUKI_SECURITY_END_COLLECT) == CIUKI_SECURITY_SESSION_DEAD);
    CHECK(!record_for(s.id, false));
    struct grants_console_state st; grants_console_snapshot(&st);
    CHECK(!st.user_owner && st.trusted && input_secure_trusted());
    /* Session 0 survives: the trusted UI keeps presenting and reading. */
    CHECK(!present(g->ui_p, g->ui_display, g->ui_surface) && screen_is(0xff));
    usb_key(0x1e, 1); usb_key(0x1e, 0);
    struct ciuki_input_event e[8]; CHECK(read_input(g->ui_p, g->ui_input, e, 8) > 0);
    CHECK(!security_session_create(&tmpl, &profile, &next.id));
    CHECK(session_end(g->logon, next.id, CIUKI_SECURITY_END_BEGIN) == CIUKI_SECURITY_SESSION_CLOSING);
    while (session_end(g->logon, next.id, CIUKI_SECURITY_END_COLLECT) != CIUKI_SECURITY_SESSION_DEAD) pid1();
    puts("H09 sequence 1 + logoff: pending launch stays concealed after ready/POLL, LEAVE activates; END conceals at once, neutral frame, DOS/Win32 drain keeps CLOSING and blocks the next logon, stale ENTER/LEAVE ESRCH, session 0 survives PASS");
}
static void test_service_restarts(struct gate *g, struct session *s)
{
    /* Sequence (3): LOGON_UI, ACCOUNT and LOGON restart between transitions. */
    struct ciuki_security_console lock;
    CHECK(enter_call(g->logon, s->id, CIUKI_SECURITY_SECURE_LOCK, &lock) == -EBUSY); pid1();
    CHECK(!enter_call(g->logon, s->id, CIUKI_SECURITY_SECURE_LOCK, &lock));
    /* LOGON_UI dies: its trusted pair goes, the gate stays concealed. */
    stop_collect(g->ui_p);
    struct grants_console_state st; grants_console_snapshot(&st);
    CHECK(!st.trusted_owner && st.trusted && input_secure_trusted() && present(s->dp, 4, s->surface) == -EBUSY);
    CHECK(leave_call(g->logon, &lock) == -EAGAIN);
    ui_start(g);
    CHECK(!present(g->ui_p, g->ui_display, g->ui_surface) && screen_is(0xff));
    struct ciuki_security_console fresh;
    CHECK(!enter_call(g->logon, s->id, CIUKI_SECURITY_SECURE_ATTENTION, &fresh) && fresh.authentication_required);
    /* ACCOUNT restart between AUTH success and LEAVE retires the proof. */
    stop_collect(g->account_p);
    CHECK(leave_call(g->logon, &fresh) == -EAGAIN && present(s->dp, 4, s->surface) == -EBUSY);
    g->account = role_process_fixture(CIUKI_SECURITY_ROLE_ACCOUNT, &g->account_p);
    /* LOGON restart: old binding void; a new LOGON's fresh ENTER/LEAVE restores. */
    CHECK(!enter_call(g->logon, s->id, CIUKI_SECURITY_SECURE_ATTENTION, &fresh));
    task_kill(g->logon->task, 0); stop_collect(g->logon_p);
    g->logon = role_process_fixture(CIUKI_SECURITY_ROLE_LOGON, &g->logon_p);
    CHECK(leave_call(g->logon, &fresh) == -EAGAIN && input_secure_trusted());
    CHECK(!enter_call(g->logon, s->id, CIUKI_SECURITY_SECURE_ATTENTION, &fresh) && fresh.authentication_required);
    CHECK(!leave_call(g->logon, &fresh) && !present(s->dp, 4, s->surface) && !input_secure_trusted());
    CHECK(s->dp->state == PROC_LIVE && s->dp->cpu_ticks == 23);
    drain_reader(s->dp, 5);
    puts("H09 sequence 3: LOGON_UI, ACCOUNT and LOGON restarts keep concealment, retire old cookies/proofs, a fresh LOGON binding alone restores the same live session PASS");
}
static void test_fences(struct gate *g, struct session *s)
{
    /* DOS full screen and the Win32/3D present lease fence the trusted UI. */
    uint32_t *pixels = P2V(surface_page(desktop_fd(s->dp, s->surface)->u.surface, 0));
    for (unsigned i = 0; i < FB_W * FB_H; i++) pixels[i] = 0x00ff0000;
    CHECK(!present(s->dp, 4, s->surface) && screen_is(0xff0000));
    fake_fullscreen = true; unsigned wakes = worker_wakes;
    struct ciuki_security_console lock;
    CHECK(enter_call(g->logon, s->id, CIUKI_SECURITY_SECURE_LOCK, &lock) == -EBUSY && worker_wakes > wakes);
    pid1(); struct grants_console_state st; grants_console_snapshot(&st);
    CHECK(st.neutral_pending && st.draining && !screen_is(0) &&
          present(g->ui_p, g->ui_display, g->ui_surface) == -EBUSY);
    CHECK(enter_call(g->logon, s->id, CIUKI_SECURITY_SECURE_LOCK, &lock) == -EBUSY);
    fake_fullscreen = false; fake_present_lease = true;
    pid1(); CHECK(enter_call(g->logon, s->id, CIUKI_SECURITY_SECURE_LOCK, &lock) == -EBUSY && !screen_is(0));
    fake_present_lease = false;
    pid1(); CHECK(screen_is(0));
    CHECK(!enter_call(g->logon, s->id, CIUKI_SECURITY_SECURE_LOCK, &lock) && lock.authentication_required);
    /* No DOS guest receives trusted input; it got one RESYNC at the switch. */
    unsigned events = dos_events; dos_grab = true;
    usb_key(0x1e, 1); usb_key(0x1e, 0);
    CHECK(dos_events == events && !dos_grabs);
    drain_reader(g->ui_p, g->ui_input);
    /* A user batch is refused while concealed; the trusted UI has no batch. */
    CHECK(grant_batch_authorized(s->dp, desktop_fd(s->dp, 4)) == -EBUSY);
    CHECK(grant_batch_authorized(g->ui_p, desktop_fd(g->ui_p, g->ui_display)) == -EOPNOTSUPP);
    CHECK(grants_display_suspend(true) == -EBUSY); /* no DOS/present handoff from a concealed desktop */
    CHECK(!leave_call(g->logon, &lock) && dos_resyncs);
    dos_grab = false;
    usb_key(0x1e, 1); usb_key(0x1e, 0);
    CHECK(dos_events == events + 3); /* KEY down, TEXT, KEY up offered again */
    drain_reader(s->dp, 5);
    puts("H09 fences: DOS full screen and present lease keep ENTER EBUSY with no neutral/trusted pixels until returned; concealed batch/handoff refused; DOS guest never sees trusted keys PASS");
}
static void test_physical_producers(struct gate *g, struct session *s)
{
    /* PS/2 set 2 (Ctrl 14, Alt 11, Delete E0 71) and the firmware adapter. */
    secure_test_native(false, 9);
    secure_test_ps2(0x14, ++tick); secure_test_ps2(0x11, ++tick);
    struct ciuki_input_event e[16]; CHECK(read_input(s->dp, 5, e, 16) == 2);
    secure_test_ps2(0xE0, ++tick); secure_test_ps2(0x71, ++tick);
    CHECK(input_secure_trusted() && attention_pending == 1 && read_input(s->dp, 5, e, 16) == -EAGAIN);
    int n = read_input(g->ui_p, g->ui_input, e, 16);
    CHECK(n == 2 && e[1].type == CIUKI_INPUT_ATTENTION);
    pid1();
    secure_test_ps2(0xE0, ++tick); secure_test_ps2(0xF0, ++tick); secure_test_ps2(0x71, ++tick);
    secure_test_ps2(0xF0, ++tick); secure_test_ps2(0x11, ++tick); secure_test_ps2(0xF0, ++tick); secure_test_ps2(0x14, ++tick);
    n = read_input(g->ui_p, g->ui_input, e, 16);
    CHECK(n == 2 && e[0].code == 0x38 && e[1].code == 0x1d); /* Delete release swallowed */
    struct ciuki_security_console menu;
    CHECK(!enter_call(g->logon, s->id, CIUKI_SECURITY_SECURE_ATTENTION, &menu) && !leave_call(g->logon, &menu));
    drain_reader(s->dp, 5);
    /* Alt+Delete or Ctrl+Delete alone stay ordinary keys. */
    usb_key(0x38, 1); usb_key(0x153, 1); usb_key(0x153, 0); usb_key(0x38, 0);
    usb_key(0x11d, 1); usb_key(0x53, 1); usb_key(0x53, 0); usb_key(0x11d, 0);
    n = read_input(s->dp, 5, e, 16); CHECK(n == 8 && !input_secure_trusted() && !attention_pending);
    /* Firmware producer: right Ctrl + right Alt + keypad Del. */
    secure_test_native(true, 11);
    tick += 3;
    struct fwinput_event fw[] = { { .type = FWINPUT_KEY, .code = 0x11d, .value = 1, .tick = tick - 2 },
        { .type = FWINPUT_KEY, .code = 0x138, .value = 1, .tick = tick - 1 },
        { .type = FWINPUT_KEY, .code = 0x53, .value = 1, .tick = tick } };
    for (unsigned i = 0; i < 3; i++) input_firmware_event(&fw[i], 11);
    CHECK(input_secure_trusted() && attention_pending == 1);
    pid1();
    for (unsigned i = 0; i < 3; i++) { fw[i].value = 0; input_firmware_event(&fw[2 - i], 11); }
    drain_reader(g->ui_p, g->ui_input);
    CHECK(!enter_call(g->logon, s->id, CIUKI_SECURITY_SECURE_ATTENTION, &menu) && !leave_call(g->logon, &menu));
    drain_reader(s->dp, 5);
    secure_test_native(false, 9);
    CHECK(!secure_test_held(0x1d) && !secure_test_held(0x38) && !secure_test_held(0x153));
    puts("H09 physical producers: PS/2 set-2 E0 71, firmware keypad Del with right Ctrl/Alt and USB all reach the same consumed transition; partial chords stay ordinary PASS");
}
static void test_logoff(struct gate *g, struct session *s)
{
    dos_left = 0; w32_left = 1;
    CHECK(session_end(g->logon, s->id, CIUKI_SECURITY_END_BEGIN) == CIUKI_SECURITY_SESSION_CLOSING);
    CHECK(s->dp->state != PROC_LIVE && input_secure_trusted() && !grants_display_pid());
    proc_collect();
    pid1(); CHECK(session_end(g->logon, s->id, CIUKI_SECURITY_END_COLLECT) == CIUKI_SECURITY_SESSION_CLOSING);
    w32_left = 0; pid1();
    CHECK(session_end(g->logon, s->id, CIUKI_SECURITY_END_COLLECT) == CIUKI_SECURITY_SESSION_DEAD);
    CHECK(session_end(g->logon, s->id, CIUKI_SECURITY_END_BEGIN) == CIUKI_SECURITY_SESSION_DEAD);
    struct ciuki_security_launch stale = s->desk;
    CHECK(poll_call(g->logon, &stale) == -ESRCH);
    struct grants_console_state st; grants_console_snapshot(&st);
    CHECK(!st.user_owner && st.trusted && !proc_find(s->desk.pid) && !proc_find(s->server.pid));
    puts("H09 logoff: END stops the pair, Win32 session objects keep CLOSING, drained session becomes DEAD idempotently, stale POLL ESRCH, no user grant remains PASS");
}

static void test_desktop_crash(struct gate *g)
{
    /* A crashed desktop after RUNNING: trusted presentation, pair stopped,
     * session CLOSING; no fallback desktop, fresh logon only after drain. */
    struct session s; session_create(&s, 1600);
    session_launch_pair(g, &s); session_activate(g, &s);
    CHECK(screen_is(0xff0000));
    stop_collect(s.dp);
    uint32_t state = 0; CHECK(!security_session_query(proc_supervisor(), s.id, &state) && state == CIUKI_SECURITY_SESSION_CLOSING);
    struct process *server = proc_find(s.server.pid);
    CHECK(input_secure_trusted() && !grants_display_pid() && (!server || server->state != PROC_LIVE));
    struct ciuki_security_console c; CHECK(enter_call(g->logon, s.id, CIUKI_SECURITY_SECURE_LOCK, &c) == -ESRCH);
    pid1(); CHECK(screen_is(0));
    CHECK(!present(g->ui_p, g->ui_display, g->ui_surface) && screen_is(0xff));
    while (session_end(g->logon, s.id, CIUKI_SECURITY_END_COLLECT) != CIUKI_SECURITY_SESSION_DEAD) pid1();
    struct grants_console_state st; grants_console_snapshot(&st); CHECK(!st.user_owner && st.trusted);
    drain_reader(g->ui_p, g->ui_input);
    puts("H09 desktop crash: RUNNING desktop death conceals (neutral frame), stops w32srv, keeps the trusted UI, no unlock, drains to DEAD PASS");
}

int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0);
    ram = calloc(HOST_PAGES, PAGE_SIZE); CHECK(ram);
    controller.state = T_RUNNING; g_current = &controller;
    proc_init();
    display_fixture(); desktop_resource.state = RS_ACTIVE;
    secure_test_native(false, 9);
    test_interim_desktop();
    struct gate g; memset(&g, 0, sizeof(g));
    test_boot_gate(&g);
    test_launch_rollback(&g);
    struct session s;
    test_activation_and_attention(&g, &s);
    test_lock_attention_cancel(&g, &s);
    test_physical_producers(&g, &s);
    test_fences(&g, &s);
    test_service_restarts(&g, &s);
    test_logoff(&g, &s);
    test_pending_launch_lock(&g);
    test_desktop_crash(&g);
    stop_collect(g.ui_p); stop_collect(g.logon_p); stop_collect(g.account_p);
    CHECK(!desktop_objects.grants && !desktop_objects.descriptions && !desktop_objects.channels && !desktop_objects.messages);
    CHECK(!consoles && !pages_used && !g_user_mappings);
    kfree(supervisor_task); supervisor_task = 0; CHECK(!heap_blocks);
    free(ram); ram = 0;
    printf("H09 secure gate: PASS checks=%u\n", checks);
    return 0;
}
