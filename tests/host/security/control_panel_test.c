/* SPDX-License-Identifier: MIT
 * fx-uaccounts: the production Control Panel as one session process of the
 * kernel host harness (control_panel_test.h). apps/control/main.c is
 * compiled whole: its native loop (control_run/idle), completion channel,
 * workers and the User Accounts glue (link over accounts_sys, QUERY_SELF,
 * cpa_execute on a cp_job thread, notify_completion), with app.c's page.
 * Replaced, as in tests/host/desktop/control_jobs_test.c: the desktop window
 * turn, the clock, poll()'s fd set (a real poll with a 120 s bound) and the
 * transport of the process-private completion pair (a host socketpair). The
 * accounts link's seam is the caller's: the kernel model's syscall boundary
 * for the control process. */
#include "../desktop/replay_base.h"
#include "../desktop/host_socket_undef.h"
#include <netdb.h>
#include <assert.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#undef SOCK_STREAM
#undef SOCK_NONBLOCK
#undef IPPROTO_IP
#undef IPPROTO_TCP
#undef IPPROTO_UDP
static int loop_poll(struct pollfd *, nfds_t, int);
#define poll loop_poll
#define main cp_native_main
#include "../../../apps/control/main.c"
#undef main
#undef poll
#include "control_panel_test.h"

void du_emit(struct du_ui *, struct du_event);   /* libdeskui widgets.c */

static struct cp_main *panel;
static uint32_t pixels[CP_WIDTH * CP_HEIGHT];
static uint64_t virtual_ns = 1000000000ull;
static int stopped, completion_seen, hang, timed_out;
static unsigned password_notices;

int cp_decoder_fd(void) { return -1; }   /* no wallpaper preview on these pages */
uint64_t du_now(void) { return virtual_ns; }
int du_window_queue(struct du_window *w, struct ciuki_message *msg)
{
    (void)w;
    if (msg->data[1] == DESK_SETTINGS && desk_get32(msg->data + 8) == DESK_SETTINGS_PASSWORD_SET) password_notices++;
    return 0;
}
int du_window_set_title(struct du_window *w, const char *title) { (void)w; (void)title; return 0; }
void du_window_close(struct du_window *w) { w->closing = 1; }
int du_window_turn(struct du_window *w, uint64_t t)
{
    if (stopped || w->closed || w->closing) return 0;
    du_tick(w->ui, t);
    if (w->ui->dirty) du_render(w->ui);
    return 1;
}
static int worker_running(void)
{ return panel->accounts_job.started || panel->users_worker.job.started || panel->network_worker.active; }
/* The production loop's only wait. A completion token is consumed by
 * control_run, which then runs du_window_turn and idle() before it comes
 * back here: by then the page must have adopted the finished result. */
static int loop_poll(struct pollfd *fds, nfds_t n, int timeout)
{
    (void)timeout;
    if (completion_seen && panel->app.account_busy && cp_job_ready(&panel->accounts_job)) {
        printf("  Control Panel: the completion woke the loop and idle() ran, but the User Accounts page still "
               "waits for its finished worker (\"Reading the accounts...\" hang)\n");
        hang = 1; stopped = 1; return 0;
    }
    completion_seen = 0;
    if (!worker_running()) { stopped = 1; return 0; }
    /* The worker drives the kernel model (and the ACCOUNTS server) until it
     * publishes; this thread touches nothing meanwhile. */
    int r = poll(fds, n, 120000);
    if (r <= 0) { timed_out = 1; stopped = 1; return 0; }
    if (fds[2].revents & POLLIN) completion_seen = 1;
    virtual_ns += 1000000ull;
    return r;
}
/* The completion pair is process-private (call 31 CHANNEL_PAIR); packets
 * of one message each, as the kernel's channel delivers them. */
int ciuki_channel_pair(int fd[2])
{
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fd)) return -1;
    for (unsigned i = 0; i < 2; i++) assert(!fcntl(fd[i], F_SETFL, O_NONBLOCK));
    return 0;
}
int ciuki_channel_send(int fd, const struct ciuki_message *msg, uint32_t flags)
{ (void)flags; ssize_t n = write(fd, msg, sizeof(*msg)); return n == (ssize_t)sizeof(*msg) ? 0 : -1; }
int ciuki_channel_recv(int fd, struct ciuki_message *msg, uint32_t flags)
{
    (void)flags; ssize_t n = read(fd, msg, sizeof(*msg));
    if (!n) { memset(msg, 0, sizeof(*msg)); return 0; }
    return n == (ssize_t)sizeof(*msg) ? 1 : -1;
}
int ciuki_dosvm(uint32_t op, struct ciuki_dosvm_command *c) { (void)op; (void)c; return -1; }
int cp_program_list(struct cp_program_store *store, struct desk_uninstall_entry *out, unsigned cap)
{ (void)store; (void)out; (void)cap; return 0; }
static int load_config(void *ctx, char *text, size_t cap, size_t *length)
{
    (void)ctx; const char *cfg = "wallpaper=none\n";
    assert(strlen(cfg) < cap); strcpy(text, cfg); *length = strlen(cfg); return 0;
}
static uint64_t epoch(void *ctx) { (void)ctx; return 1791711660 + virtual_ns / 1000000000ull; }

void cpt_open(const struct lgn_client_sys *sys, const char *page)
{
    static struct cp_main m;
    panel = &m; memset(&m, 0, sizeof(m));
    /* A fresh process: the static link starts unbound (main.c). */
    memset(&accounts_link, 0, sizeof(accounts_link));
    accounts_sys = sys;
    assert(!completion_start(&m));
    m.network_worker.notify = notify_completion; m.network_worker.notify_ctx = &m;
    m.assets_ready = 1; m.title_page = -99;
    du_init(&m.ui, pixels, CP_WIDTH, CP_HEIGHT);
    m.window.ui = &m.ui; m.window.configured = 1; m.window.channel = -1;
    m.saved_w = CP_WIDTH; m.saved_h = CP_HEIGHT;
    m.app.host = (struct cp_host){ .load = load_config, .now = epoch, .apply = apply, .ctx = &m, .job_notify = notify_completion };
    accounts_attach(&m.app.host);   /* main(): a profile session's User Accounts */
    password_notices = 0;
    assert(!cp_attach(&m.app, &m.ui, cp_page_index(page)));
}
int cpt_settle(void)
{
    stopped = completion_seen = hang = timed_out = 0;
    int r = control_run(panel);
    if (r || timed_out) return -2;
    return hang ? -1 : 0;
}
void cpt_close(void)
{
    struct cp_main *m = panel;
    cp_job_finish(&m->users_worker.job);
    cp_job_finish(&m->accounts_job); cpa_wipe(&m->accounts_request, sizeof(m->accounts_request));
    lgn_client_close(&accounts_link);
    cp_job_finish(&m->menu_job); cp_job_finish(&m->game_job);
    cp_job_finish(&m->program_scan); free(m->program_next);
    cp_job_finish(&m->assets);
    network_worker_stop(&m->network_worker);
    cp_detach(&m->app); completion_stop(m);
    free(m->hwid_pci); free(m->hwid_usb);
    accounts_sys = NULL; panel = NULL;
}
int cpt_busy(void) { return panel->app.account_busy; }
const struct cpa_view *cpt_view(void) { return &panel->app.accounts; }
const char *cpt_note(void)
{
    static char note[256];
    const char *first = cpt_text(CPT_NOTE), *second = cpt_text(CPT_NOTE2);
    snprintf(note, sizeof(note), "%s%s%s", first ? first : "", second && *second ? " " : "", second ? second : "");
    return note;
}
const char *cpt_text(int id) { struct du_widget *w = du_get(&panel->ui, id); return w ? w->text : NULL; }
int cpt_disabled(int id) { struct du_widget *w = du_get(&panel->ui, id); return w ? w->disabled : -1; }
unsigned cpt_rows(void) { struct du_widget *w = du_get(&panel->ui, CPT_LIST); return w ? w->count : 0; }
const char *cpt_row(unsigned index)
{
    struct du_widget *w = du_get(&panel->ui, CPT_LIST);
    return w && index < w->count ? w->items[index].text[0] : NULL;
}
int cpt_task(void) { return panel->app.account_task; }
unsigned cpt_password_notices(void) { return password_notices; }
void cpt_activate(int id) { du_post(&panel->ui, DU_EV_ACTIVATE, id, 0); }
void cpt_select_row(unsigned index) { du_post(&panel->ui, DU_EV_SELECT, CPT_LIST, (int)index); }
void cpt_edit(int id, const char *text) { struct du_widget *w = du_get(&panel->ui, id); assert(w); du_set_text(w, text); }
void cpt_type(int id, const char *text)
{
    for (const char *p = text; *p; p++) du_emit(&panel->ui, (struct du_event){ .type = DU_EV_KEY, .id = id, .ch = (unsigned char)*p });
}
