/* H10 (sec-user-03): the production LOGON coordinator (apps/logon) with the
 * production account service (apps/accounts server + kernel adapter) on the
 * kernel host harness: k01 sessions/launch/secure console (security.c,
 * spawn.c), the k07 backend (secure_console.c, grants.c), channels and the
 * merged native input queue (secure_input_queue.c). LOGON_UI and the session
 * desktop are role processes whose requests go through the production CSC1
 * client (apps/logon/logon_proto.c). Fakes stand only at the boundaries:
 * RAM/scheduler, framebuffer, executable bytes, session console device,
 * OWNER/protected storage (H03 fake) and the power provider.
 * Usage: logon_service <ready|setup|safe|restart>
 * SPDX-License-Identifier: GPL-2.0-only */
#define CIUKI_DESKTOP_REAL_INPUT
#define CIUKI_DESKTOP_HOST_MAIN logon_unused_desktop_main
#include "../proc/desktop_test.c"
#undef main
#include <stdarg.h>
#include <ciuki/fwinput.h>
#include <ciuki/init.h>
#include "../../../src/kernel/proc/secure_console.c"
#include "accounts.h"
#include "kernel_adapter.h"
#include "kernel_sys.h"
#include "server.h"
#include "fake.h"
#include "logon.h"
#include "../../../apps/logon/ui.h"
#include "wire.h"

void secure_test_native(bool firmware, gen_t generation);

/* ---- boundary fakes (as H09) --------------------------------------------- */
static int verbose;
void klog(const char *fmt, ...)
{
    char record[256]; va_list ap; va_start(ap, fmt);
    kvsnprintf(record, sizeof(record), fmt, ap); va_end(ap);
    if (verbose && strstr(record, "[security]")) printf("  kernel: %s\n", record);
}
void klog_periodic(const char *fmt, ...) __attribute__((alias("klog")));
bool drivers_input_firmware(void) { return false; }
bool gen_matches(gen_t current, gen_t supplied) { return supplied && current == supplied; }
bool display_fullscreen_active(void) { return false; }
bool display_present_active(void) { return false; }
void display_worker_wake(void) {}
static unsigned console_changes;
void display_console_changed(void) { console_changes++; }
bool dosvm_fullscreen_input(uint16_t type, uint16_t code, int32_t value) { (void)type; (void)code; (void)value; return false; }
unsigned dosvm_session_revoke(uint32_t session) { (void)session; return 0; }
unsigned w32_session_revoke(uint32_t session) { (void)session; return 0; }
struct console_object { struct proc_object object; unsigned refs; };
static unsigned consoles;
static void console_retain(struct proc_object *o) { ((struct console_object *)o)->refs++; }
static void console_release(struct proc_object *o)
{ struct console_object *c = (struct console_object *)o; if (!--c->refs) { consoles--; kfree(c); } }
int files_session_console(struct process *p, int32_t *console)
{
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
static char last_profile[64];
int security_exec_open(struct process *p, const char *path, struct ciuki_file *out)
{
    CHECK(!p->published && p->state == PROC_PREPARING && security_credential(p)->session_id);
    CHECK(!strcmp(path, "/Programs/desktop") || !strcmp(path, "/System/Win32/w32srv"));
    *out = fixture_file(); return 0;
}
static unsigned owner_verifies;
int security_owner_verify(const struct ciuki_security_profile *p)
{ owner_verifies++; memcpy(last_profile, p->path, sizeof(last_profile)); return 0; }
int px_security_cwd(struct process *p, const char *path)
{ CHECK(p->state == PROC_PREPARING && !p->published && strstr(path, "/Documents")); return 0; }

/* ---- process bindings ------------------------------------------------------ */
#define U_ARGS (CIUKI_IMAGE_BASE + PAGE_SIZE + 0x100)
#define U_OUT (U_ARGS + 0x100)
#define U_IN (U_ARGS + 0x200)
#define U_MSG (U_ARGS + 0x300)
#define INBUF (CIUKI_IMAGE_BASE + PAGE_SIZE + 0x800)
static void park(void) { if (g_current != &controller) g_current->state = T_READY; g_current = &controller; }
static struct proc_thread *thread_of(struct process *p)
{
    for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++)
        if (proc_thread_slot(i) && proc_thread_slot(i)->process == p) return proc_thread_slot(i);
    CHECK(false); return 0;
}
struct bind { struct proc_thread *t; };
static int b_security(void *ctx, struct ciuki_security_args *a, const void *in, uint32_t in_bytes, void *out, uint32_t out_bytes)
{
    struct bind *b = ctx; struct process *p = b->t->process;
    desktop_select(b->t);
    struct ciuki_security_args c = *a;
    if (a->operation == CIUKI_SECURITY_SESSION_CREATE) {
        CHECK(in && in_bytes == 176); CHECK(!ua_write(p->memory, U_IN, in, 176));
        c.credential_address = U_IN; c.profile_address = U_IN + 64;
    } else if (in) { CHECK(in_bytes <= 0xc0); CHECK(!ua_write(p->memory, U_IN, in, in_bytes)); c.spawn_address = U_IN; }
    if (out) { c.output_address = U_OUT; c.output_bytes = out_bytes; }
    CHECK(!ua_write(p->memory, U_ARGS, &c, 64));
    struct trap_frame tf = { .eax = 101, .ebx = U_ARGS, .ecx = 64 };
    desktop_select(b->t);
    int e = proc_syscall(&tf);
    if (!e) {
        CHECK(!ua_read(p->memory, &c, U_ARGS, 64));
        a->result = c.result; a->state = c.state;
        if (out) CHECK(!ua_read(p->memory, out, U_OUT, out_bytes));
    }
    park();
    return e;
}
static int b_send(void *ctx, int fd, const struct ciuki_message *m)
{
    struct bind *b = ctx; struct process *p = b->t->process;
    desktop_select(b->t); CHECK(!ua_write(p->memory, U_MSG, m, sizeof(*m)));
    int e = channel_send(p, fd, U_MSG, DONTWAIT);
    park(); return e;
}
static int b_recv(void *ctx, int fd, struct ciuki_message *m)
{
    struct bind *b = ctx; struct process *p = b->t->process;
    desktop_select(b->t);
    int r = channel_recv(p, fd, U_MSG, DONTWAIT);
    if (r == 1) CHECK(!ua_read(p->memory, m, U_MSG, sizeof(*m)));
    park(); return r;
}
static int b_close(void *ctx, int fd)
{ struct bind *b = ctx; desktop_select(b->t); int e = desktop_close(b->t->process, fd); park(); return e; }
static uint64_t b_now(void *ctx) { (void)ctx; return g_ticks; }

/* LOGON's seam. */
static struct bind logon_bind;
static uint32_t boot_flags_value;
static unsigned power_calls; static unsigned power_last; static int power_result;
static int l_wait(void *ctx, const int *fds, unsigned count, unsigned ms) { (void)ctx; (void)fds; (void)ms; return (int)count; }
static int l_boot_flags(void *ctx, uint32_t *flags) { (void)ctx; *flags = boot_flags_value; return 0; }
static int l_power(void *ctx, unsigned action) { (void)ctx; power_calls++; power_last = action; return power_result; }
static int l_pids(void *ctx, uint32_t *pids, unsigned capacity)
{
    (void)ctx; unsigned n = 0;
    for (unsigned i = 1; i < CIUKI_PROCESS_MAX && n < capacity; i++) {
        struct process *p = proc_at_slot(i);
        if (p && p->state == PROC_LIVE) pids[n++] = p->pid;
    }
    return (int)n;
}
static unsigned logon_logs;
static void l_log(void *ctx, const char *line) { (void)ctx; logon_logs++; if (verbose) printf("  %s\n", line); }
/* sec-k08 (U4-3): call-73 firmware resume/critical counts LOGON samples. */
static uint32_t fw_resumes;
static int l_resumes(void *ctx, uint32_t *count) { (void)ctx; *count = fw_resumes; return 0; }
static const struct lgn_sys logon_sys = { &logon_bind, b_security, b_send, b_recv, b_close, l_wait, b_now,
                                          l_boot_flags, l_power, l_pids, l_log, l_resumes };
static struct lgn logon;
static struct process *logon_p, *ui_p, *account_p;

/* ACCOUNT's seam (kernel_sys.h), as sec-int-01. */
static struct proc_thread *account_thread;
static int in_responder;
static void account_responder(void);
int acct_sys_security(struct ciuki_security_args *a, const void *in, uint32_t in_bytes, void *out, uint32_t out_bytes)
{
    struct bind b = { account_thread };
    return b_security(&b, a, in, in_bytes, out, out_bytes);
}
int acct_sys_channel_send(int fd, const struct ciuki_message *m) { struct bind b = { account_thread }; return b_send(&b, fd, m); }
int acct_sys_channel_recv(int fd, struct ciuki_message *m, int wait) { (void)wait; struct bind b = { account_thread }; return b_recv(&b, fd, m); }
int acct_sys_close(int fd) { struct bind b = { account_thread }; return b_close(&b, fd); }
/* ACCOUNT waits for LOGON's AUTH_OUTCOME acknowledgment: LOGON runs meanwhile. */
int acct_sys_wait_readable(const int *fds, unsigned count, unsigned timeout_ms)
{
    (void)fds; (void)timeout_ms;
    g_ticks += 1;
    if (!in_responder) { in_responder = 1; account_responder(); in_responder = 0; }
    return (int)count;
}
uint32_t acct_sys_pid(void) { return account_thread->process->pid; }
int acct_sys_pids(uint32_t *pids, unsigned capacity) { return l_pids(NULL, pids, capacity); }
uint64_t acct_sys_monotonic_ms(void) { return g_ticks; }
static unsigned entropy_counter;
int acct_sys_entropy(void *buffer, size_t bytes)
{ uint8_t *b = buffer; for (size_t i = 0; i < bytes; i++) b[i] = (uint8_t)(++entropy_counter * 7u); return 0; }
int acct_sys_boot_flags(uint32_t *flags) { *flags = boot_flags_value; return 0; }
int acct_sys_file_present(const char *path) { return strstr(path, "/System/Security/Pictures/") != NULL; }
void acct_sys_log(const char *line) { if (verbose) printf("  %s\n", line); }
static struct acct_server acct_srv;
static struct fake fake;
static void account_step(void)
{
    if (!acct_srv.opened) (void)acct_server_open(&acct_srv, g_ticks);
    acct_server_accept(&acct_srv);
    int fds[ACCT_KERNEL_LINKS]; unsigned n = acct_kernel_adapter_fds(&acct_srv.kernel, fds, ACCT_KERNEL_LINKS);
    for (unsigned i = 0; i < n; i++) acct_server_serve(&acct_srv, fds[i]);
    (void)acct_server_work(&acct_srv, 250);
    acct_server_sweep(&acct_srv);
}
static void account_responder(void) { if (logon_bind.t) lgn_pass(&logon); }

/* PID 1's pass and the session children's readiness. */
static void pid1(void) { g_current = &controller; security_poll(); secure_console_poll(); proc_collect(); }
static int auto_ready = 1, ready_status;
static uint32_t readied[2];
static void ready_child(const struct ciuki_security_launch *l, int status)
{
    struct process *p = proc_find(l->pid); CHECK(p);
    desktop_select(thread_of(p));
    struct ciuki_message m = { .length = 16 }; uint32_t words[] = { 1, l->launch_id, l->launch_generation, (uint32_t)status };
    memcpy(m.data, words, 16); CHECK(!ua_write(p->memory, U_OUT, &m, sizeof(m)));
    CHECK(!channel_send(p, l->child_ready_fd, U_OUT, DONTWAIT));
    park();
}
static void children(void)
{
    if (!auto_ready || logon.session.phase != LGN_POLL) return;
    for (unsigned i = 0; i < 2; i++) {
        const struct ciuki_security_launch *l = &logon.session.launch[i];
        if (!l->launch_id || readied[i] == l->launch_id || l->state != CIUKI_SECURITY_LAUNCH_PREPARING) continue;
        if (i == 0 && readied[1] != logon.session.launch[1].launch_id) continue; /* w32srv first */
        ready_child(l, i == 0 ? ready_status : 0); readied[i] = l->launch_id;
    }
}
static int world_depth;
static void world(void)
{
    if (world_depth++) { world_depth--; return; }
    account_step();
    if (logon_bind.t) lgn_pass(&logon);
    children();
    pid1();
    g_ticks += 5;
    world_depth--;
}
#define RUN_UNTIL(cond, steps) do { unsigned _n = 0; while (!(cond)) { CHECK(++_n < (steps)); world(); } } while (0)

/* ---- clients through the production CSC1 client ------------------------- */
static int c_wait(void *ctx, int fd, unsigned ms) { (void)ctx; (void)fd; (void)ms; world(); return 1; }
static struct bind ui_bind, desk_bind;
static const struct lgn_client_sys ui_sys = { &ui_bind, b_security, b_send, b_recv, c_wait, b_close, b_now };
static const struct lgn_client_sys desk_sys = { &desk_bind, b_security, b_send, b_recv, c_wait, b_close, b_now };
static struct lgn_client ui_logon, ui_auth, desk_logon;
static int call(struct lgn_client *c, uint32_t op, uint64_t generation, const void *body, size_t n, void *reply,
                size_t cap, size_t *used)
{
    int status = 0;
    for (unsigned attempt = 0;; attempt++) {
        int e = lgn_client_call(c, op, generation, body, n, reply, cap, used, &status, 20000);
        if (!e) return status;
        if (verbose) printf("  client op=%u transport=%d\n", op, e);
        CHECK(attempt < 3);        /* a link to a dead LOGON is reconnected once */
        world();
    }
}
static struct lgn_state ui_state(uint32_t command, uint32_t session, uint32_t console, int *status)
{
    struct lgn_ui_command c = { command, session, console }; uint8_t body[32], reply[96]; size_t used = 0;
    CHECK(!lgn_ui_command_encode(&c, body, 32));
    int s = call(&ui_logon, LGN_OP_UI_STATE, 0, body, 32, reply, sizeof(reply), &used);
    if (status) *status = s; else CHECK(!s);
    struct lgn_state st; memset(&st, 0, sizeof(st));
    if (!s) { CHECK(used == 96); CHECK(!lgn_state_decode(&st, reply, 96)); }
    return st;
}
static struct lgn_state state(void) { return ui_state(LGN_CMD_QUERY, 0, 0, NULL); }
static int ui_event(uint32_t op, uint32_t action, uint32_t session, uint32_t console, struct lgn_progress *p)
{
    uint8_t body[32] = { 0 }, reply[16]; size_t used = 0;
    uint32_t words[4] = { 1, action, session, console }; memcpy(body, words, 16);
    int s = call(&ui_logon, op, 0, body, 32, reply, 16, &used);
    if (!s) { CHECK(used == 16 && !lgn_progress_decode(p, reply, 16)); }
    return s;
}
static int ui_begin(uint32_t action, uint32_t uid, const char *name, uint32_t session, uint32_t console, uint8_t challenge[64])
{
    struct acct_begin b; memset(&b, 0, sizeof(b));
    b.action = action; b.session = session; b.console = console; b.selector = name ? 2 : 1; b.uid = name ? 0 : uid;
    if (name) strcpy(b.name, name);
    uint8_t body[64]; size_t used = 0;
    CHECK(!acct_begin_encode(&b, 0, body, 64));
    int s = call(&ui_logon, LGN_OP_BEGIN_AUTH, 0, body, 64, challenge, 64, &used);
    if (!s) CHECK(used == 64);
    return s;
}
static int ui_submit(const uint8_t challenge[64], const char *name, const char *secret, struct lgn_progress *p)
{
    uint8_t body[208], reply[16]; size_t used = 0;
    CHECK(!lgn_submit_encode(challenge, name, secret, "", body));
    int s = call(&ui_logon, LGN_OP_SUBMIT_AUTH, 0, body, 208, reply, 16, &used);
    lgn_wipe(body, sizeof(body));
    if (!s) CHECK(used == 16 && !lgn_progress_decode(p, reply, 16));
    return s;
}
/* One complete Welcome/unlock exchange; returns the progress disposition. */
static struct lgn_progress authenticate(uint32_t action, uint32_t uid, const char *name, const char *secret)
{
    struct lgn_state st = state();
    uint8_t challenge[64]; struct lgn_progress p = { 0, 0, 0 };
    int s = ui_begin(action, uid, name, action == 3 ? st.session : 0, action == 3 ? st.console : 0, challenge);
    if (s) { p.disposition = LGN_FAILED; p.status = s; return p; }
    CHECK(a32(challenge + 16) && a32(challenge + 16) != CIUKI_UID_INVALID);
    CHECK(!ui_submit(challenge, name && action == 2 ? name : "", secret, &p));
    lgn_wipe(challenge, sizeof(challenge));
    return p;
}
static unsigned ui_list(char names[ACCT_MAX][32])
{
    uint8_t reply[16 + 144 * ACCT_MAX]; size_t used = 0;
    CHECK(!call(&ui_auth, LGN_AUTH_LIST, 0, NULL, 0, reply, sizeof(reply), &used));
    unsigned n = a32(reply + 4); CHECK(used == 16 + 144 * n);
    for (unsigned i = 0; i < n; i++) memcpy(names[i], reply + 16 + 144 * i + 16, 32);
    return n;
}
static void desk_connect(void)
{
    struct process *d = proc_find(logon.session.launch[0].pid); CHECK(d);
    desk_bind.t = thread_of(d);
    lgn_client_close(&desk_logon);
    lgn_client_init(&desk_logon, &desk_sys, LGN_SERVICE_LOGON, LGN_ROLE_LOGON);
}
static struct lgn_session_event desk_event(uint32_t notice)
{
    struct lgn_session_notice n = { notice, logon.session.id }; uint8_t body[32], reply[64]; size_t used = 0;
    CHECK(!lgn_notice_encode(&n, body, 32));
    CHECK(!call(&desk_logon, LGN_OP_SESSION_EVENT, 0, body, 32, reply, 64, &used));
    struct lgn_session_event ev; CHECK(used == 64 && !lgn_event_decode(&ev, reply, 64));
    return ev;
}
static int desk_lock(struct lgn_progress *p)
{
    struct acct_ui_event ev = { 1, logon.session.id, 0, 0 }; uint8_t body[32], reply[16]; size_t used = 0;
    CHECK(!acct_event_encode(5, 0, &ev, body, 32));
    int s = call(&desk_logon, LGN_OP_SESSION_LOCK, 0, body, 32, reply, 16, &used);
    if (!s) CHECK(used == 16 && !lgn_progress_decode(p, reply, 16));
    return s;
}

/* ---- fixture ------------------------------------------------------------- */
static struct proc_thread *role_fixture(uint32_t role, struct process **out)
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
static void ui_start(void)
{
    ui_bind.t = role_fixture(CIUKI_SECURITY_ROLE_LOGON_UI, &ui_p);
    lgn_client_init(&ui_logon, &ui_sys, LGN_SERVICE_LOGON, LGN_ROLE_LOGON);
    lgn_client_init(&ui_auth, &ui_sys, LGN_SERVICE_AUTH, LGN_ROLE_ACCOUNT);
    int32_t input = -1;
    for (int32_t fd = 0; fd < CIUKI_OPEN_MAX; fd++) {
        struct desktop_description *d = desktop_fd(ui_p, fd);
        if (d && d->kind == DESKTOP_INPUT) input = fd;
    }
    CHECK(input >= 0 && !desktop_fcntl(ui_p, input, F_SETFL, O_NONBLOCK));
}
static void logon_start(void)
{
    logon_bind.t = role_fixture(CIUKI_SECURITY_ROLE_LOGON, &logon_p);
    CHECK(!lgn_init(&logon, &logon_sys));
}
static void boot(int ready)
{
    fake_provision(&fake, ready);
    struct acct_store store = fake_store(&fake);
    CHECK(!acct_server_init(&acct_srv, &store, 1));
    account_thread = role_fixture(CIUKI_SECURITY_ROLE_ACCOUNT, &account_p);
    logon_start();
    ui_start();
    CHECK(security_attention_armed());
}
static const struct ciuki_credential *desktop_credential(void)
{
    struct process *d = proc_find(logon.session.launch[0].pid); CHECK(d);
    return security_credential(d);
}
static bool gate_trusted(void) { struct grants_console_state st; grants_console_snapshot(&st); return st.trusted; }
static uint64_t key_tick = 1000;
static void usb_key(uint16_t code, int value) { input_usb_event(INPUT_KEY, code, value, 71, ++key_tick); }
/* Physical Ctrl+Alt+Del: consumed by the kernel queue, PID 1 enters the
 * trusted console, LOGON_UI reads ATTENTION and relays it (UI_STATE). */
static struct lgn_state attention(void)
{
    usb_key(0x1d, 1); usb_key(0x38, 1); usb_key(0x153, 1);
    pid1();
    usb_key(0x153, 0); usb_key(0x38, 0); usb_key(0x1d, 0);
    int32_t input = -1;
    for (int32_t fd = 0; fd < CIUKI_OPEN_MAX; fd++) { struct desktop_description *d = desktop_fd(ui_p, fd); if (d && d->kind == DESKTOP_INPUT) input = fd; }
    struct ciuki_input_event e[16]; bool seen = false;
    desktop_select(ui_bind.t); int n = grant_input_read(ui_p, input, INBUF, 16);
    if (n > 0) CHECK(!ua_read(ui_p->memory, e, INBUF, (unsigned)n * sizeof(e[0])));
    park();
    for (int i = 0; i < n; i++) seen |= e[i].type == CIUKI_INPUT_ATTENTION && e[i].code == CIUKI_SECURITY_SECURE_ATTENTION;
    CHECK(seen && gate_trusted());
    return ui_state(LGN_CMD_ATTENTION, 0, 0, NULL);
}
static void desktop_exit(void)
{
    struct process *d = proc_find(logon.session.launch[0].pid); CHECK(d);
    lgn_client_close(&desk_logon); desk_bind.t = NULL;
    stop_collect(d);
}
static void active(void)
{
    readied[0] = readied[1] = 0;
    RUN_UNTIL(logon.session.phase == LGN_ACTIVE && logon.mode == LGN_MODE_HIDDEN, 4000);
    CHECK(!gate_trusted() && grants_display_pid() == logon.session.launch[0].pid);
    desk_connect();
}
static void welcome(void) { RUN_UNTIL(!logon.session.phase && logon.mode == LGN_MODE_WELCOME, 4000); CHECK(gate_trusted()); }

#ifdef CIUKI_RUNTIME_UI
/* sec-runtime1: the production Welcome UI and both real service peers. */
static uint64_t runtime_ui_now(void *p){(void)p;return g_ticks;}
static int runtime_ui_call(void *p,unsigned service,uint32_t op,const void *body,size_t n,void *reply,size_t cap,size_t *used,int *status)
{
    (void)p;CHECK(service==LGN_SERVICE_AUTH||service==LGN_SERVICE_LOGON);
    *status=call(service==LGN_SERVICE_AUTH?&ui_auth:&ui_logon,op,0,body,n,reply,cap,used);return 0;
}
static void runtime_tile_click(struct lui *u)
{
    static uint32_t pixels[LUI_W*LUI_H];struct du_canvas c={pixels,LUI_W,LUI_H,{0,0,LUI_W,LUI_H}};
    lui_paint(u,&c);int found=0,x=0,y=0;
    for(unsigned i=0;i<u->item_count;i++)if(u->items[i].action==LUI_TILE&&u->items[i].index==0){
        x=u->items[i].box.x+2;y=u->items[i].box.y+2;found=1;break;
    }
    CHECK(found);struct ciuki_input_event e={.type=CIUKI_INPUT_MOTION,.value=x,.value2=y};lui_input(u,&e);
    e=(struct ciuki_input_event){.type=CIUKI_INPUT_BUTTON,.code=CIUKI_BUTTON_LEFT,.value=1};lui_input(u,&e);
    e.value=0;lui_input(u,&e);
}
static void runtime_logoff(void)
{
    struct lgn_state st=attention();(void)ui_state(LGN_CMD_LOGOFF,st.session,st.console,NULL);
    CHECK(desk_event(LGN_NOTICE_QUERY).action==LGN_ACT_LOGOFF);
    (void)desk_event(LGN_NOTICE_LOGOFF);desktop_exit();welcome();
}
static void scenario_welcome_tile(void)
{
    boot(1);active();runtime_logoff();
    static const struct lui_host host={NULL,runtime_ui_now,runtime_ui_call,NULL};struct lui u;
    lui_init(&u,&host,NULL,0);lui_tick(&u);
    CHECK(u.view==LUI_VIEW_LIST&&u.count==1&&u.accounts[0].uid==1000&&u.accounts[0].blank_logon);
    uint32_t previous=logon.session.id;readied[0]=readied[1]=0;runtime_tile_click(&u);
    RUN_UNTIL(logon.session.phase==LGN_ACTIVE&&logon.mode==LGN_MODE_HIDDEN,4000);desk_connect();
    CHECK(logon.session.id>previous&&logon.session.blank&&desktop_credential()->uid==1000&&!u.password[0]);
    lui_wipe(&u);runtime_logoff();
    /* A protected fixture record: the list must stop advertising the hint. */
    CHECK(!acct_set_password(&acct_srv.service,&acct_srv.service.db.records[2],"password1"));
    lui_init(&u,&host,NULL,0);lui_tick(&u);
    CHECK(u.count==1&&!u.accounts[0].blank_logon);runtime_tile_click(&u);
    CHECK(u.view==LUI_VIEW_PASSWORD&&!logon.session.phase);lui_wipe(&u);
    puts("runtime Welcome tile: blank authenticated, protected prompts PASS");
}

#endif

/* ---- scenarios ------------------------------------------------------------ */
static void scenario_ready(void)
{
    boot(1);
    active();
    CHECK(logon.autologon_done && logon.session.blank && !strcmp(logon.session.name, "CiukiUser"));
    const struct ciuki_credential *c = desktop_credential();
    CHECK(c->uid == 1000 && c->primary_gid == 545 && c->group_count == 2 && c->groups[0] == 544 && c->groups[1] == 545 &&
          c->session_id == logon.session.id && c->flags == CIUKI_CRED_INTERACTIVE);
    CHECK(!strcmp(last_profile, "/Windows/Profiles/u000003e8") && owner_verifies == 1);
    /* The session desktop keeps the interim desktop's termination authority (spawn.c). */
    struct process *dp = proc_find(logon.session.launch[0].pid); int authority = 0;
    for (unsigned i = 0; i < CIUKI_OPEN_MAX; i++) authority += dp->fds[i].object && proc_authority_object(dp->fds[i].object);
    CHECK(authority == 1);
    struct lgn_state st = state();
    CHECK(st.mode == LGN_MODE_HIDDEN && st.owner_uid == 1000 && !strcmp(st.owner_name, "CiukiUser") &&
          (st.flags & LGN_STATE_BLANK) && (st.flags & LGN_STATE_SESSION) && !(st.flags & LGN_STATE_SAFE));
    struct lgn_session_event ev = desk_event(LGN_NOTICE_QUERY);
    CHECK(ev.action == LGN_ACT_NONE && ev.uid == 1000 && !strcmp(ev.name, "CiukiUser") && ev.flags == LGN_EVENT_BLANK);
    CHECK((state().flags & LGN_STATE_DESKTOP) && console_changes);
    /* w32srv's end-session writer: the desktop's call-101 service-6 link is
     * attested by the ACCEPT record (peer role DESKTOP, its session, its pid). */
    {
        struct process *w = proc_find(logon.session.launch[1].pid); CHECK(w);
        struct bind wb = { thread_of(w) };
        struct ciuki_security_link in = { .version = 1, .size = 64, .service = CIUKI_SECURITY_SERVICE_W32SRV }, out;
        struct ciuki_security_args a = { .version = 1, .size = 64, .operation = CIUKI_SECURITY_SERVICE_CONNECT };
        CHECK(!b_security(&desk_bind, &a, &in, 64, &out, 64) && out.server_role == CIUKI_SECURITY_ROLE_W32SRV);
        int32_t client = out.endpoint_fd;
        a = (struct ciuki_security_args){ .version = 1, .size = 64, .operation = CIUKI_SECURITY_SERVICE_ACCEPT };
        CHECK(!b_security(&wb, &a, &in, 64, &out, 64));
        CHECK(out.peer_role == CIUKI_SECURITY_ROLE_DESKTOP && out.peer_pid == logon.session.launch[0].pid &&
              out.session_id == logon.session.id);
        CHECK(!b_close(&wb, out.endpoint_fd) && !b_close(&desk_bind, client));
    }
    puts("H10 ready boot: autologon of password-less CiukiUser through AUTOLOGON, SESSION_CREATE, w32srv then desktop launch, initial activation; uid 1000, groups 544+545, profile u000003e8 PASS");

    /* Lock of a password-less owner: guidance, never a false lock. */
    struct lgn_progress p;
    CHECK(!desk_lock(&p) && p.status == LGN_STATUS_NO_PASSWORD && !gate_trusted() && logon.mode == LGN_MODE_HIDDEN);
    /* Secure attention -> security menu; Cancel restores the same session. */
    uint32_t session = logon.session.id;
    st = attention();
    CHECK(st.mode == LGN_MODE_SECURITY && st.session == session && gate_trusted());
    CHECK(!ui_event(LGN_OP_LOCK_EVENT, LGN_EVENT_MENU_RETURN, st.session, st.console, &p) && !p.status);
    CHECK(!gate_trusted() && logon.mode == LGN_MODE_HIDDEN && logon.session.id == session);
    /* Task Manager: back to the desktop, which receives the action once. */
    st = attention();
    st = ui_state(LGN_CMD_TASKMGR, st.session, st.console, NULL);
    CHECK(st.mode == LGN_MODE_HIDDEN && !gate_trusted());
    CHECK(desk_event(LGN_NOTICE_QUERY).action == LGN_ACT_TASKMGR && desk_event(LGN_NOTICE_QUERY).action == LGN_ACT_NONE);
    /* Lock from the menu for a blank owner: guidance, the menu stays. */
    st = attention();
    CHECK(!ui_event(LGN_OP_LOCK_EVENT, LGN_EVENT_LOCK, st.session, st.console, &p) && p.status == LGN_STATUS_NO_PASSWORD);
    st = state(); CHECK(st.mode == LGN_MODE_SECURITY && st.message == LGN_MSG_BLANK_LOCK && gate_trusted());
    int status = 0;
    (void)ui_state(LGN_CMD_LOGOFF, st.session, st.console + 1, &status); CHECK(status == -EAGAIN);
    (void)ui_state(LGN_CMD_LOGOFF, st.session + 1, st.console, &status); CHECK(status == -EACCES);
    /* Log Off from the menu: the desktop runs its query/veto, then ends. */
    st = ui_state(LGN_CMD_LOGOFF, st.session, st.console, NULL);
    CHECK(st.mode == LGN_MODE_HIDDEN && desk_event(LGN_NOTICE_QUERY).action == LGN_ACT_LOGOFF);
    CHECK(desk_event(LGN_NOTICE_LOGOFF).action == LGN_ACT_NONE && logon.session.end == LGN_END_LOGOFF);
    desktop_exit();
    welcome();
    st = state(); CHECK(st.message == LGN_MSG_NONE && !(st.flags & LGN_STATE_SESSION));
    for (unsigned i = 0; i < 400; i++) world();
    CHECK(!logon.session.phase && logon.mode == LGN_MODE_WELCOME); /* no automatic logon after Log Off */
    puts("H10 ready session: blank lock guidance, Ctrl+Alt+Del menu Cancel/Task Manager/Lock guidance, stale and foreign console refused, menu Log Off through the desktop, Welcome without autologon PASS");

    /* Welcome: enabled accounts only; disabled/unknown names refused. */
    char names[ACCT_MAX][32];
    CHECK(ui_list(names) == 1 && !strcmp(names[0], "CiukiUser"));
    uint8_t challenge[64];
    /* Generic refusal; the account service then holds every authentication
     * request for its bounded 1 s retry delay (contract 3.2). */
    CHECK(ui_begin(2, 0, "Guest", 0, 0, challenge) == -EACCES);
    CHECK(ui_begin(2, 0, "CiukiUser", 0, 0, challenge) == -EAGAIN);
    g_ticks += 1000;
    CHECK(ui_begin(2, 0, "CiukiAdministrator", 0, 0, challenge) == -EACCES);
    g_ticks += 1000;
    CHECK(ui_begin(2, 0, "Nobody", 0, 0, challenge) == -EACCES);
    g_ticks += 1000;
    CHECK(ui_begin(2, 501, NULL, 0, 0, challenge) == -EACCES);
    g_ticks += 1000;
    struct acct_begin bad = { .action = 2, .selector = 2 }; strcpy(bad.name, "bad name"); uint8_t body[64];
    CHECK(acct_begin_encode(&bad, 0, body, 64) == -EINVAL && !lgn_name_syntax("bad name"));
    CHECK(!logon.session.phase && gate_trusted());
    p = authenticate(2, 1000, NULL, "wrongpassword");
    CHECK(p.disposition == LGN_FAILED && p.status == -EACCES && p.retry_after_ms == 1000 && !logon.session.phase);
    CHECK(authenticate(2, 1000, NULL, "").status == -EAGAIN && !logon.session.phase);
    g_ticks += 1000;
    p = authenticate(2, 0, "ciukiuser", "");
    CHECK(p.disposition == LGN_AUTHENTICATED && !p.status);
    active();
    CHECK(logon.session.id == session + 1 && logon.session.blank && desktop_credential()->uid == 1000);
    /* The desktop crashes: trusted gate, session collected, notice. */
    desktop_exit();
    welcome();
    st = state(); CHECK(st.message == LGN_MSG_SESSION_ENDED);
    st = ui_state(LGN_CMD_ACKNOWLEDGE, 0, 0, NULL); CHECK(st.message == LGN_MSG_NONE);
    puts("H10 Welcome: list hides disabled Guest/500, typed Guest/500/unknown names and uid 501 refused, wrong password bounded retry, typed name case-insensitive logon, desktop crash ends the session with a notice PASS");

    /* Logon-screen power: fixed request, orderly path, once. */
    power_result = -EBUSY;
    CHECK(ui_event(LGN_OP_POWER_EVENT, LGN_EVENT_SHUTDOWN, 0, st.console + 1, &p) == -EAGAIN);
    CHECK(!ui_event(LGN_OP_POWER_EVENT, LGN_EVENT_SHUTDOWN, 0, st.console, &p) && !p.status);
    RUN_UNTIL(power_calls == 1, 100);
    CHECK(power_last == LGN_EVENT_SHUTDOWN);
    RUN_UNTIL(logon.mode == LGN_MODE_WELCOME, 100);
    CHECK(state().message == LGN_MSG_POWER_FAILED && power_calls == 1);
    power_result = 0;
    st = state();
    CHECK(!ui_event(LGN_OP_POWER_EVENT, LGN_EVENT_RESTART, 0, st.console, &p) && !p.status);
    for (unsigned i = 0; i < 50; i++) world();
    CHECK(power_calls == 2 && power_last == LGN_EVENT_RESTART && logon.mode == LGN_MODE_POWER);
    puts("H10 logon-screen power: Shut Down/Restart through LOGON's orderly call, refused call returns to Welcome with a notice, stale console refused PASS");
}

static void scenario_setup(void)
{
    boot(0);
    welcome();
    CHECK(logon.autologon_done && !logon.session.phase);
    char names[ACCT_MAX][32];
    CHECK(ui_list(names) == 1 && !strcmp(names[0], "Alice"));
    struct lgn_progress p = authenticate(2, 0, "Alice", "password");
    CHECK(p.disposition == LGN_AUTHENTICATED);
    active();
    uint32_t session = logon.session.id, desktop = logon.session.launch[0].pid;
    CHECK(!logon.session.blank && desk_event(LGN_NOTICE_QUERY).flags == 0);
    /* Start > Lock: SESSION_LOCK from the session desktop. */
    CHECK(!desk_lock(&p) && !p.status && gate_trusted() && logon.mode == LGN_MODE_LOCKED);
    RUN_UNTIL(!logon.enter_reason, 100);   /* an EBUSY entry completes after PID 1's neutral frame */
    CHECK(!desk_lock(&p) && !p.status);                     /* coalesced */
    struct lgn_state st = state();
    CHECK(st.mode == LGN_MODE_LOCKED && st.owner_uid == 1000 && !strcmp(st.owner_name, "Alice"));
    CHECK(ui_event(LGN_OP_LOCK_EVENT, LGN_EVENT_MENU_RETURN, st.session, st.console, &p) == -EACCES);
    CHECK(gate_trusted());
    st = attention(); CHECK(st.mode == LGN_MODE_LOCKED);  /* attention never lowers a lock */
    p = authenticate(3, 1000, NULL, "wrongpassword");
    CHECK(p.disposition == LGN_FAILED && p.status == -EACCES && gate_trusted() && logon.mode == LGN_MODE_LOCKED);
    g_ticks += 1000;
    p = authenticate(3, 1000, NULL, "");
    CHECK(p.disposition == LGN_FAILED && gate_trusted());
    g_ticks += 1000;
    p = authenticate(3, 1000, NULL, "password");
    CHECK(p.disposition == LGN_AUTHENTICATED && !p.status && !gate_trusted() && logon.mode == LGN_MODE_HIDDEN);
    CHECK(logon.session.id == session && logon.session.launch[0].pid == desktop && grants_display_pid() == desktop);
    puts("H10 lock: Start > Lock conceals, duplicate coalesced, menu return and attention cannot lower it, wrong and empty passwords stay locked, owner password restores the same session and desktop PASS");

    /* LOGON_UI dies: the session locks (A2-3); the new UI shows it. */
    lgn_client_close(&ui_logon); lgn_client_close(&ui_auth);
    stop_collect(ui_p);
    RUN_UNTIL(logon.mode == LGN_MODE_LOCKED && !logon.enter_reason, 400);
    CHECK(gate_trusted());
    ui_start();
    st = state(); CHECK(st.mode == LGN_MODE_LOCKED && st.message == LGN_MSG_SERVICE_RESTART);
    p = authenticate(3, 1000, NULL, "password");
    CHECK(p.disposition == LGN_AUTHENTICATED && logon.mode == LGN_MODE_HIDDEN && !gate_trusted());
    puts("H10 LOGON_UI restart: concealed and locked, fresh owner authentication restores the session PASS");

    /* Security menu Shut Down: the desktop's save/veto, then LOGON powers off. */
    st = attention();
    st = ui_state(LGN_CMD_SHUTDOWN, st.session, st.console, NULL);
    CHECK(st.mode == LGN_MODE_HIDDEN && desk_event(LGN_NOTICE_QUERY).action == LGN_ACT_SHUTDOWN);
    /* A veto keeps the same session and identity: nothing happens. */
    for (unsigned i = 0; i < 100; i++) world();
    CHECK(logon.session.phase == LGN_ACTIVE && logon.session.id == session && desktop_credential()->uid == 1000);
    CHECK(desk_event(LGN_NOTICE_SHUTDOWN).action == LGN_ACT_NONE);
    power_result = 0;
    desktop_exit();
    RUN_UNTIL(power_calls == 1, 4000);
    CHECK(power_last == LGN_EVENT_SHUTDOWN && logon.mode == LGN_MODE_POWER && !logon.session.phase);
    puts("H10 shutdown: security menu Shut Down handed to the desktop, veto keeps the session, after its notice the session is collected and LOGON powers off PASS");
}

static void scenario_safe(void)
{
    boot_flags_value = CIUKI_BOOT_SAFE_MODE; g_boot.flags |= CBI_F_SAFE_MODE;
    boot(0);
    welcome();
    CHECK(logon.safe && (state().flags & LGN_STATE_SAFE));
    char names[ACCT_MAX][32]; unsigned n = ui_list(names); int admin = 0;
    for (unsigned i = 0; i < n; i++) admin |= !strcmp(names[i], "CiukiAdministrator");
    CHECK(n == 2 && admin);
    struct lgn_progress p = authenticate(2, 0, "CiukiAdministrator", "wrongpassword");
    CHECK(p.disposition == LGN_FAILED && !logon.session.phase);
    g_ticks += 1000;
    p = authenticate(2, 0, "CiukiAdministrator", "password");
    CHECK(p.disposition == LGN_AUTHENTICATED);
    active();
    const struct ciuki_credential *c = desktop_credential();
    CHECK(c->uid == 500 && c->flags == (CIUKI_CRED_INTERACTIVE | CIUKI_CRED_SAFE_RECOVERY) && c->group_count == 2);
    CHECK(desk_event(LGN_NOTICE_QUERY).flags == LGN_EVENT_RECOVERY);
    puts("H10 Safe Mode: no automatic logon, disabled CiukiAdministrator listed and logged on with its password as SAFE_RECOVERY PASS");
}

static void scenario_ready_boot_safe(void)
{
    /* A ready image in Safe Mode never logs on automatically. */
    boot_flags_value = CIUKI_BOOT_SAFE_MODE; g_boot.flags |= CBI_F_SAFE_MODE;
    boot(1);
    welcome();
    for (unsigned i = 0; i < 200; i++) world();
    CHECK(!logon.session.phase && logon.autologon_done);
    char names[ACCT_MAX][32]; CHECK(ui_list(names) == 1 && !strcmp(names[0], "CiukiUser")); /* unset 500 hidden */
    uint8_t challenge[64]; CHECK(ui_begin(2, 0, "CiukiAdministrator", 0, 0, challenge) == -EACCES);
    puts("H10 Safe Mode ready image: no autologon, unset CiukiAdministrator stays hidden and refused PASS");
}

static void scenario_restart(void)
{
    boot(1);
    /* A desktop that reports failure: trusted error, no session. */
    ready_status = -EIO;
    RUN_UNTIL(logon.session.phase == LGN_ENDING || (logon.session.phase == LGN_NONE && logon.message), 4000);
    welcome();
    CHECK(state().message == LGN_MSG_START_FAILED);
    ready_status = 0;
    struct lgn_progress p = authenticate(2, 1000, NULL, "");
    CHECK(p.disposition == LGN_AUTHENTICATED);
    active();
    uint32_t session = logon.session.id;
    puts("H10 start failure: negative desktop readiness ends the session, Welcome shows the notice, logon afterwards PASS");

    /* LOGON restarts: the kernel conceals and raises authentication; the new
     * instance adopts the locked session. A blank owner cannot unlock with an
     * empty password; only a confirmed end of the session is offered. */
    lgn_client_close(&desk_logon);
    stop_collect(logon_p);
    CHECK(gate_trusted());
    memset(&logon, 0, sizeof(logon));
    logon_start();
    RUN_UNTIL(logon.session.phase == LGN_ACTIVE && logon.mode == LGN_MODE_LOCKED && logon.accounts_ready && logon.own_generation &&
              logon.session.console_valid && !logon.enter_reason, 4000);
    CHECK(logon.session.adopted && logon.session.id == session && logon.session.uid == 1000);
    struct lgn_state st = state();
    CHECK(st.mode == LGN_MODE_LOCKED && st.message == LGN_MSG_SERVICE_RESTART && !strcmp(st.owner_name, "CiukiUser"));
    p = authenticate(3, 1000, NULL, "");
    CHECK(p.disposition == LGN_FAILED && p.status == -EPERM && gate_trusted());
    st = state(); CHECK(st.message == LGN_MSG_NO_EMPTY_UNLOCK && (st.flags & LGN_STATE_BLANK));
    int status = 0;
    (void)ui_state(LGN_CMD_LOGOFF, st.session, st.console, &status); CHECK(status == -EBUSY);
    st = ui_state(LGN_CMD_FORCE_LOGOFF, st.session, st.console, NULL);
    CHECK(st.mode == LGN_MODE_ENDING);
    welcome();
    CHECK(state().message == LGN_MSG_NO_EMPTY_UNLOCK || state().message == LGN_MSG_NONE);
    puts("H10 LOGON restart: session adopted locked, empty-password unlock refused for the blank owner, confirmed end of session returns to Welcome PASS");
}

/* --- sec-user-04 block: H12 scenarios (test_account_controls.py builds this
 * harness with -DCIUKI_H12_ACCOUNTS and the Control Panel's client). --- */
#ifdef CIUKI_H12_ACCOUNTS
#include "users.h"
#include "account_controls_test.inc"
#else
static int h12_scenario(const char *name) { (void)name; return 0; }
#endif
/* --- end sec-user-04 block --- */
int main(int argc, char **argv)
{
    setvbuf(stdout, 0, _IONBF, 0);
    CHECK(argc >= 2);
    verbose = argc > 2;
    ram = calloc(HOST_PAGES, PAGE_SIZE); CHECK(ram);
    controller.state = T_RUNNING; g_current = &controller;
    proc_init();
    desktop_resource.state = RS_ACTIVE;
    secure_test_native(false, 9);
    fake_init(&fake);
#ifdef CIUKI_RUNTIME_UI
    if (!strcmp(argv[1], "welcome-tile")) scenario_welcome_tile();
    else
#endif
    if (!strcmp(argv[1], "ready")) scenario_ready();
    else if (!strcmp(argv[1], "setup")) scenario_setup();
    else if (!strcmp(argv[1], "safe")) scenario_safe();
    else if (!strcmp(argv[1], "ready-safe")) scenario_ready_boot_safe();
    else if (!strcmp(argv[1], "restart")) scenario_restart();
    else if (h12_scenario(argv[1])) {}   /* sec-user-04 */
    else CHECK(false);
    /* Teardown: every session, service and link is released. */
    lgn_client_close(&ui_logon); lgn_client_close(&ui_auth); lgn_client_close(&desk_logon);
    if (logon.session.id && logon.session.phase) {
        uint32_t state_value;
        g_current = &controller;
        (void)security_session_end(logon.session.id, true, &state_value);
        for (unsigned i = 0; i < 100 && security_session_end(logon.session.id, false, &state_value) == 0 &&
             state_value != CIUKI_SECURITY_SESSION_DEAD; i++) pid1();
    }
    lgn_close(&logon);
    acct_server_close(&acct_srv);
    stop_collect(ui_p); stop_collect(logon_p); stop_collect(account_p);
    pid1();
    CHECK(!desktop_objects.channels && !desktop_objects.messages && !desktop_objects.grants);
    CHECK(!consoles && logon_logs);
    fake_destroy(&fake);
    printf("H10 logon service %s: PASS checks=%u\n", argv[1], checks);
    return 0;
}
