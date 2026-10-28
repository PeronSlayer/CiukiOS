/* SPDX-License-Identifier: MIT
 * fx-uaccounts: the production Control Panel as one session process of the
 * kernel host harness (control_panel_test.h). apps/control/main.c is
 * compiled whole: its native loop (control_run/idle), completion channel,
 * workers and the User Accounts glue (link over accounts_sys, QUERY_SELF,
 * cpa_execute on a cp_job thread, notify_completion), with app.c's page.
 * Uses production control_init/control_destroy, libdeskui client FIFO/flush
 * and desktop desk_receive -> desk_session_password_set. Only channels,
 * surfaces, native service calls and the bounded poll wait are host adapters.
 * The real desk_logon_secure callback uses the LOGON syscall adapter.
 */
#include "../desktop/replay_base.h"
#include "../desktop/host_socket_undef.h"
#include <netdb.h>
#include <assert.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
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
#include "../../../apps/desktop/desktop.h"
#include "../../../apps/desktop/logon_session.h"

void du_emit(struct du_ui *, struct du_event);   /* libdeskui widgets.c */

static struct cp_main *panel;
static int stopped, completion_seen, hang, timed_out;
static unsigned password_notices;
static struct desktop shell;
static int desktop_channel, desktop_peer;
static uint32_t control_pid;
void cpt_desktop_bind(const struct lgn_client_sys *,uint32_t);
void cpt_desktop_unbind(void);
static struct ciuki_message inbound[64];
static unsigned inbound_head, inbound_count;

static int shell_send(int fd,const struct ciuki_message *m,uint32_t flags)
{
    (void)fd; (void)flags;
    assert(inbound_count<64);
    inbound[(inbound_head+inbound_count)%64]=*m;
    inbound[(inbound_head+inbound_count)%64].sender_pid=1;
    inbound_count++; return 0;
}
static int worker_running(void)
{ return panel->accounts_job.started || panel->users_worker.job.started || panel->network_worker.active || panel->assets.started || settings_busy(panel); }
/* The production loop's only wait. A completion token is consumed by
 * control_run, which then runs du_window_turn and idle() before it comes
 * back here: by then the page must have adopted the finished result. */
static int loop_poll(struct pollfd *fds, nfds_t n, int timeout)
{
    (void)timeout;
    if (completion_seen && panel->ui.capture<0 && !panel->ui.dialog.kind && panel->app.account_busy && cp_job_ready(&panel->accounts_job)) {
        printf("  Control Panel: the completion woke the loop and idle() ran, but the User Accounts page still "
               "waits for its finished worker (\"Reading the accounts...\" hang)\n");
        hang = 1; stopped = 1; errno=ECANCELED; return -1;
    }
    completion_seen = 0;
    if (panel->window.count || inbound_count) { fds[0].revents=POLLOUT; return 1; }
    if (!worker_running()) { stopped = 1; errno=ECANCELED; return -1; }
    /* The worker drives the kernel model (and the ACCOUNTS server) until it
     * publishes; this thread touches nothing meanwhile. */
    int r = poll(fds, n, 120000);
    if (r <= 0) { timed_out = 1; stopped = 1; errno=ETIMEDOUT; return -1; }
    if (fds[2].revents & POLLIN) completion_seen = 1;
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
{
    if (fd==desktop_channel) {
        struct ciuki_message m=*msg; m.sender_pid=control_pid;
        for (unsigned i=0;i<m.fd_count;i++) { m.fds[i]=dup(msg->fds[i]); assert(m.fds[i]>=0); }
        desk_receive(&shell,0,&m); desk_flush(&shell,0);
        if (m.data[1]==DESK_SETTINGS && desk_get32(m.data+8)==DESK_SETTINGS_PASSWORD_SET) password_notices++;
        return 0;
    }
    (void)flags; ssize_t n=write(fd,msg,sizeof(*msg)); return n==(ssize_t)sizeof(*msg) ? 0 : -1;
}
int ciuki_channel_recv(int fd, struct ciuki_message *msg, uint32_t flags)
{
    if (fd==desktop_channel) {
        if (!inbound_count) { errno=EAGAIN; return -1; }
        *msg=inbound[inbound_head]; inbound_head=(inbound_head+1)%64; inbound_count--; return 1;
    }
    (void)flags; ssize_t n = read(fd, msg, sizeof(*msg));
    if (!n) { memset(msg, 0, sizeof(*msg)); return 0; }
    return n == (ssize_t)sizeof(*msg) ? 1 : -1;
}
int ciuki_dosvm(uint32_t op, struct ciuki_dosvm_command *c) { (void)op; (void)c; return -1; }
int ciuki_surface_create(uint32_t w,uint32_t h,uint32_t format)
{
    assert(w==CP_WIDTH && h==CP_HEIGHT && format==CIUKI_SURFACE_XRGB8888);
    char path[512];
    assert(snprintf(path,sizeof(path),"%s/cpt-surface-XXXXXX",getenv("TMPDIR"))<(int)sizeof(path));
    int fd=mkstemp(path); assert(fd>=0); unlink(path);
    assert(!ftruncate(fd,((off_t)w*h*4+4095)&~(off_t)4095)); return fd;
}
int ciuki_surface_info(int fd,struct ciuki_surface_info *info)
{
    (void)fd; *info=(struct ciuki_surface_info){sizeof(*info),CP_WIDTH,CP_HEIGHT,CP_WIDTH*4,
        CIUKI_SURFACE_XRGB8888,(CP_WIDTH*CP_HEIGHT*4+4095)&~4095u}; return 0;
}
void *ciuki_surface_map(int fd,int prot)
{ return mmap(NULL,(CP_WIDTH*CP_HEIGHT*4+4095)&~4095u,prot,MAP_SHARED,fd,0); }
int ciuki_proc_list(void *out,uint32_t cap) { (void)out; (void)cap; errno=ENOSYS; return -1; }

void cpt_open(const struct lgn_client_sys *sys,const struct lgn_client_sys *desktop_sys,
              const char *page,uint32_t pid,uint32_t session,const char *profile)
{
    static struct cp_main m;
    panel=&m; memset(&m,0,sizeof(m));
    memset(&accounts_link,0,sizeof(accounts_link)); accounts_sys=sys;
    control_pid=pid; password_notices=0; cpt_desktop_bind(desktop_sys,session);
    inbound_head=inbound_count=0;
    struct desk_ops ops={.send=shell_send,.info=ciuki_surface_info,.map=ciuki_surface_map,
        .close=close,.unmap=munmap};
    desk_init(&shell,&ops,1024,768); shell.shell=1; shell.logon_session=1; shell.session_secure=desk_logon_secure;
    int channel[2]; assert(!ciuki_channel_pair(channel)); desktop_channel=channel[0]; desktop_peer=channel[1];
    assert(desk_add_client(&shell,dup(desktop_channel))==0);
    shell.clients[0].program=DESK_APP_CONTROL;
    assert(!setenv("USERPROFILE",profile,1));
    assert(!control_init(&m,desktop_channel,cp_page_index(page),0,NULL));
}
int cpt_settle(void)
{
    stopped=completion_seen=hang=timed_out=0;
    int r=control_run(panel);
    if ((!stopped && r) || timed_out) return -2;
    return hang ? -1 : 0;
}
void cpt_close(void)
{
    control_destroy(panel);
    if (shell.clients[0].channel>=0) desk_drop_client(&shell,0);
    close(desktop_peer); cpt_desktop_unbind();
    accounts_sys=NULL; panel=NULL; desktop_channel=-1;
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

void cpt_request_close(void)
{
    struct ciuki_message m; desk_message(&m,DESK_CLOSE_REQUEST,4);
    m.data[0]=DESK_SHELL_VERSION; m.data[2]=DESK_RESIZE_MINOR;
    assert(!shell_send(0,&m,0));
    assert(du_window_turn(&panel->window,du_now())>0);
    assert(!panel->window.closing && panel->app.close_pending);
}
int cpt_closed(void) { return panel->window.closed; }
void cpt_wait_ready(void)
{
    uint64_t until=du_now()+120000000000ull;
    while (!cp_job_ready(&panel->accounts_job)) { assert(du_now()<until); sched_yield(); }
}

/* Unused native calls fail at the syscall boundary, never at a UI seam. */
int ciuki_devctl(int fd,uint32_t op,void *record,uint32_t size)
{ (void)fd; (void)op; (void)record; (void)size; errno=ENOSYS; return -1; }
int ciuki_system_setting_set(unsigned op,const char *text,size_t n)
{ (void)op; (void)text; (void)n; errno=ENOSYS; return -1; }
int ciuki_boot_config_set(const char *text,size_t n)
{ (void)text; (void)n; errno=ENOSYS; return -1; }
int ciuki_system_log_read(unsigned scope,const char *name,uint32_t offset,void *buffer,size_t cap,size_t *got,uint32_t *total)
{ (void)scope; (void)name; (void)offset; (void)buffer; (void)cap; (void)got; (void)total; errno=ENOSYS; return -1; }
int ciuki_time_zone(struct ciuki_time_zone *zone,uint32_t op)
{ (void)zone; (void)op; errno=ENOSYS; return -1; }
int ciuki_devices(struct ciuki_devices_info *record)
{ (void)record; errno=ENOSYS; return -1; }
int ciuki_display_monitor(struct ciuki_monitor_info *record)
{ (void)record; errno=ENOSYS; return -1; }
int ciuki_display_query(struct ciuki_display_info *record)
{ (void)record; errno=ENOSYS; return -1; }
int ciuki_display_refresh(struct ciuki_display_refresh *record)
{ (void)record; errno=ENOSYS; return -1; }
int ciuki_driver_pack(struct ciuki_driver_pack_job *record)
{ (void)record; errno=ENOSYS; return -1; }
int ciuki_firmware_inventory(struct ciuki_firmware_page *record)
{ (void)record; errno=ENOSYS; return -1; }
int ciuki_power_events(struct ciuki_power_events *record)
{ (void)record; errno=ENOSYS; return -1; }
int ciuki_power_status(struct ciuki_power_status *record)
{ (void)record; errno=ENOSYS; return -1; }
int ciuki_recovery_status(struct ciuki_recovery_status *record)
{ (void)record; errno=ENOSYS; return -1; }
int ciuki_net_control(unsigned op,struct ciuki_net_control *record)
{ (void)op; (void)record; errno=ENOSYS; return -1; }

void cpt_hold(int capture)
{
    if (capture) {
        struct du_widget *w=du_get(&panel->ui,CPT_LIST); assert(w);
        du_input(&panel->ui,&(struct ciuki_input_event){.type=CIUKI_INPUT_MOTION,.value=w->box.x+8,.value2=w->box.y+8});
        du_input(&panel->ui,&(struct ciuki_input_event){.type=CIUKI_INPUT_BUTTON,.code=1,.value=1});
        assert(panel->ui.capture>=0);
    } else {
        static const char *const buttons[]={"OK"};
        du_message(&panel->ui,999,"Keep open","Account completion must wait.",0,buttons,1,0,0);
        assert(panel->ui.dialog.kind);
    }
}
int cpt_guarded_turn(int capture)
{
    assert(du_window_turn(&panel->window,du_now())>0);
    cp_tick(&panel->app); idle(panel);
    return !panel->window.closing && panel->app.account_busy && (capture ? panel->ui.capture>=0 : panel->ui.dialog.kind!=0);
}
void cpt_release(int capture)
{ if (capture) du_release(&panel->ui); else du_dialog_close(&panel->ui); }

ciuki_pid_t ciuki_spawn(const char *path,char *const argv[],char *const env[],const struct ciuki_spawn_fd *fds,uint32_t n,uint32_t flags)
{ (void)path; (void)argv; (void)env; (void)fds; (void)n; (void)flags; errno=ENOSYS; return -1; }
uint32_t ciuki_syscall(uint32_t call,uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f)
{ (void)call; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return (uint32_t)-ENOSYS; }

void cpt_accept_close(void) { cp_display_accept(&panel->app); }

void cpt_full_queue(void)
{
    cpt_wait_ready();
    struct ciuki_message m; desk_message(&m,DESK_PING,8);
    while (panel->window.count<16) assert(!du_window_queue(&panel->window,&m));
    cp_jobs_poll(&panel->app);
    assert(!panel->app.account_busy && panel->app.account_notice_pending);
    cpt_request_close(); assert(!panel->window.closing);
}
