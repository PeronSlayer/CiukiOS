/* SPDX-License-Identifier: MIT
 * Production Control Panel loop/jobs with host channels and a virtual clock.
 * Only the desktop turn and clock/poll boundary are replaced. Native wait
 * scheduling is independently exercised by control_display_wait_test.c. */
#include "replay_base.h"
#include "host_socket_undef.h"
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
static int loop_poll(struct pollfd *,nfds_t,int);
#define poll loop_poll
#define main cp_native_main
#include "../../../apps/control/main.c"
#undef main
#undef poll

static uint64_t virtual_ns;
static int stopped, expected_timeout, waits, fail_send, event_mode;
static struct cp_main *event_panel;
static int (*adopted)(struct cp_main *);   /* fx-uaccounts: event_mode 2 check; NULL = program inventory */
static uint32_t pixels[CP_WIDTH*CP_HEIGHT];
int cp_decoder_fd(void) { return -1; } /* No preview requested by these fixtures. */
uint64_t du_now(void) { return virtual_ns; }
int du_window_queue(struct du_window *w,struct ciuki_message *msg)
{ (void)w; (void)msg; return 0; }
int du_window_set_title(struct du_window *w,const char *title)
{ (void)w; (void)title; return 0; }
void du_window_close(struct du_window *w) { w->closing=1; }
int du_window_turn(struct du_window *w,uint64_t t)
{
    if (stopped || w->closed || w->closing) return 0;
    du_tick(w->ui,t); if (w->ui->dirty) du_render(w->ui);
    return 1;
}
static int loop_poll(struct pollfd *fds,nfds_t n,int timeout)
{
    assert(n==3 && fds[2].fd>=0 && timeout==expected_timeout);
    if (event_mode==1) {
        /* Execute the real channel wait/dispatch once, then prove the next
         * native-loop idle turn has adopted the result without a heartbeat. */
        assert(timeout==-1); waits++;
        int r=poll(fds,n,3000);
        assert(r==1 && fds[2].revents&POLLIN); event_mode=2; return r;
    }
    if (event_mode==2) {
        if (adopted) assert(adopted(event_panel));
        else assert(event_panel->programs_ready && event_panel->program_count==1 && !event_panel->program_scan.started);
        event_mode=0;
    }
    /* An input event after 65 idle seconds, or exactly the real deadline.
     * A 10/50/1000 ms service fallback fails this assertion on static pages. */
    assert(timeout==-1 || timeout>=1000);
    waits++; virtual_ns+=timeout<0 ? 65000000000ull : (uint64_t)timeout*1000000ull;
    stopped=1; return 0;
}
int ciuki_channel_pair(int fd[2])
{
    /* Local packet IPC; read/write also work under the sandbox's network filter. */
    if (socketpair(AF_UNIX,SOCK_SEQPACKET,0,fd)) return -1;
    for (unsigned i=0;i<2;i++) assert(!fcntl(fd[i],F_SETFL,O_NONBLOCK));
    return 0;
}
int ciuki_channel_send(int fd,const struct ciuki_message *msg,uint32_t flags)
{
    if (fail_send) { errno=EIO; return -1; }
    (void)flags; ssize_t n=write(fd,msg,sizeof(*msg));
    return n==(ssize_t)sizeof(*msg) ? 0 : -1;
}
int ciuki_channel_recv(int fd,struct ciuki_message *msg,uint32_t flags)
{
    (void)flags; ssize_t n=read(fd,msg,sizeof(*msg));
    if (!n) { memset(msg,0,sizeof(*msg)); return 0; }
    return n==(ssize_t)sizeof(*msg) ? 1 : -1;
}
/* Native services are outside the fixture; program inventory has one record. */
int ciuki_dosvm(uint32_t op,struct ciuki_dosvm_command *c)
{ (void)op; (void)c; return -1; }
/* fx-uaccounts: a machine without network adapters; IPX setting refused. */
int ciuki_net_control(unsigned op,struct ciuki_net_control *c)
{ (void)op; (void)c; errno=ENODEV; return -1; }
int ciuki_system_setting_set(unsigned setting,const char *text,size_t length)
{ (void)setting; (void)text; (void)length; errno=ENOSYS; return -1; }
int cp_program_list(struct cp_program_store *store,struct desk_uninstall_entry *out,unsigned cap)
{ (void)store; assert(cap); memset(out,0,sizeof(*out)); strcpy(out->name,"Fixture"); return 1; }
static int load_config(void *ctx,char *text,size_t cap,size_t *length)
{
    (void)ctx; const char *cfg="wallpaper=none\nclock_seconds=1\n";
    assert(strlen(cfg)<cap); strcpy(text,cfg); *length=strlen(cfg); return 0;
}
static uint64_t epoch(void *ctx) { (void)ctx; return 1791711660+virtual_ns/1000000000ull; }
static void init_panel(struct cp_main *m,int page)
{
    memset(m,0,sizeof(*m)); assert(!completion_start(m));
    m->assets_ready=1; m->title_page=-99;
    du_init(&m->ui,pixels,CP_WIDTH,CP_HEIGHT);
    m->window.ui=&m->ui; m->window.configured=1; m->window.channel=-1;
    m->saved_w=CP_WIDTH; m->saved_h=CP_HEIGHT;
    m->app.host=(struct cp_host){.load=load_config,.now=epoch,.ctx=m,.job_notify=notify_completion};
    assert(!cp_attach(&m->app,&m->ui,page));
    stopped=waits=event_mode=0;
}
static void finish_panel(struct cp_main *m)
{
    cp_job_finish(&m->assets); cp_job_finish(&m->program_scan);
    cp_detach(&m->app); completion_stop(m);
    free(m->program_next); free(m->hwid_pci); free(m->hwid_usb);
}
static void drain(struct cp_main *m)
{
    struct ciuki_message msg;
    unsigned count=0;
    while (ciuki_channel_recv(m->completion[0],&msg,DONTWAIT)>0) { assert(msg.length==4 && ++count<2048); }
    assert(errno==EAGAIN);
}
static void await_completion(struct cp_main *m)
{
    struct pollfd fd={.fd=m->completion[0],.events=POLLIN};
    assert(poll(&fd,1,3000)==1 && fd.revents&POLLIN);
    drain(m);
}
static void idle_pages(void)
{
    static struct cp_main m;
    for (int page=CP_HOME;page<CP_PAGES;page++) {
        virtual_ns=10000000000ull; init_panel(&m,page);
        /* f1-113: the Game controllers page samples live axes and save status once per second.
         * f1-110/f1-121: the Power page refreshes the cached ACPI/APM status once per second. */
        expected_timeout=m.app.page==CP_DATETIME || m.app.page==CP_SYSTEM || m.app.page==CP_SOUNDS ||
            m.app.page==CP_JOYSTICK || m.app.page==CP_POWER || m.app.page==CP_ACCESS ? 1000 : -1;   /* G25: the Accessibility page samples its live status once per second */
        assert(!control_run(&m) && waits==1);
        finish_panel(&m);
    }
    init_panel(&m,CP_DISPLAY); m.engine_next=virtual_ns+2000000000ull;
    expected_timeout=2000; assert(!control_run(&m) && waits==1); finish_panel(&m);
    /* An unconfigured Display must wait for configuration, without 1 ms retries. */
    init_panel(&m,CP_DISPLAY); m.window.configured=0;
    expected_timeout=-1; assert(!control_run(&m) && waits==1); finish_panel(&m);
    init_panel(&m,CP_HOME); m.app.close_pending=1;
    assert(!control_run(&m) && !waits && m.window.closing==1); finish_panel(&m);
    puts("PASS every Control Panel page: one event/deadline wait, no service polling; clocks/mixer/joystick/power 1s, graphics query 2s only");
}
static int gate, result;
static pthread_mutex_t gate_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_changed=PTHREAD_COND_INITIALIZER;
static void gated_work(void *ctx)
{
    (void)ctx; pthread_mutex_lock(&gate_lock);
    while (!gate) pthread_cond_wait(&gate_changed,&gate_lock);
    pthread_mutex_unlock(&gate_lock); result=42;
}
static void *release_gate(void *ctx)
{
    (void)ctx; nanosleep(&(struct timespec){.tv_nsec=50000000},NULL);
    pthread_mutex_lock(&gate_lock); gate=1;
    pthread_cond_signal(&gate_changed); pthread_mutex_unlock(&gate_lock); return NULL;
}
static int write_fixture(struct cfg_writer *writer,const char *text,size_t length)
{ (void)writer; assert(length==4 && !memcmp(text,"test",4)); return 0; }
static void jobs(const char *fonts)
{
    static struct cp_main m;
    init_panel(&m,CP_HOME); m.programs_requested=1;
    event_mode=1; event_panel=&m; expected_timeout=-1;
    assert(!control_run(&m) && waits==2 && !event_mode); finish_panel(&m);
    init_panel(&m,CP_HOME);
    assert(!mkdir(DOSBOX_SETTINGS_DIR,0700));
    struct game_ports_settings ports;game_ports_defaults(&ports);
    assert(!game_begin(&m,1,&ports));ports.com[0]=0; /* copied before the worker starts */
    await_completion(&m);struct cp_game_result game_result;
    assert(game_poll_result(&m,&game_result)==1 && !game_result.error && game_result.saved && game_result.ports.com[0]==1);
    assert(!game_begin(&m,1,NULL));await_completion(&m);
    assert(game_poll_result(&m,&game_result)==1 && !game_result.error && !game_result.saved && game_result.ports.sequence==1);
    struct cp_job job={0}; pthread_t release;
    assert(!cp_job_start(&job,gated_work,NULL,notify_completion,&m));
    assert(!cp_job_ready(&job)); assert(cp_job_start(&job,gated_work,NULL,NULL,NULL)==-EBUSY);
    assert(!pthread_create(&release,NULL,release_gate,NULL));
    await_completion(&m); assert(cp_job_ready(&job)); cp_job_finish(&job);
    assert(result==42); pthread_join(release,NULL);
    /* Publication before registration stays readable; a full queue coalesces. */
    notify_completion(&m); await_completion(&m);
    for (unsigned i=0;i<1000;i++) notify_completion(&m);
    await_completion(&m);
    /* Production lazy assets start only after first paint and are adopted once. */
    m.assets_ready=0; m.assets_requested=1; m.ui.dirty=0; m.ui.frame_count=1;
    idle(&m); assert(m.assets.started); await_completion(&m);
    m.ui.capture=1; idle(&m); assert(!m.assets_ready && m.assets.started);
    m.ui.capture=-1; idle(&m); assert(m.assets_ready && !m.assets.started);
    /* Program inventory uses that same publication; adoption waits for modal close. */
    m.programs_requested=1; m.ui.dirty=0;
    idle(&m); assert(m.program_scan.started); await_completion(&m);
    m.ui.dialog.kind=DU_DLG_MESSAGE; idle(&m); assert(!m.programs_ready);
    m.ui.dialog.kind=0; idle(&m); assert(m.programs_ready && m.program_count==1 && !m.program_scan.started);
    /* Fonts complete away from Appearance and stay retained when returning. */
    m.app.host.fonts_async=1; m.app.host.fonts_path=fonts;
    cp_open(&m.app,CP_FONTS); du_render(&m.ui); idle(&m); assert(m.app.font_job);
    cp_open(&m.app,CP_HOME); du_render(&m.ui);
    await_completion(&m); idle(&m);
    assert(m.app.fonts_loaded==1 && m.app.font_count==12 && m.app.font_face && !m.ui.dirty);
    struct ciuki_font_face *face=m.app.font_face;
    cp_open(&m.app,CP_FONTS); assert(m.app.font_face==face && !m.app.font_job);
    cp_open(&m.app,CP_HOME); du_render(&m.ui);
    /* Durable writes publish a result before notifying, with no 50 ms check. */
    m.writer.write=write_fixture; m.writer.notify=notify_completion; m.writer.notify_ctx=&m;
    assert(!cfg_writer_start(&m.writer,"unused","unused")); m.writer_started=1;
    assert(!cfg_writer_submit(&m.writer,"test",4)); await_completion(&m);
    int error=-1; assert(settings_result(&m,&error) && !error && !settings_busy(&m));
    assert(!settings_result(&m,&error));
    cfg_writer_stop(&m.writer); m.writer_started=0;
    /* Pending ownership is joined on shutdown, including fonts. */
    cp_fonts_close(&m.app); cp_open(&m.app,CP_FONTS); du_render(&m.ui); idle(&m);
    assert(m.app.font_job); finish_panel(&m);
    puts("PASS jobs: registered/pre-wait wake, queue saturation, assets, deferred adoption, programs, retained fonts, durable settings and shutdown");
    init_panel(&m,CP_HOME); fail_send=1; notify_completion(&m); fail_send=0;
    struct pollfd fd={.fd=m.completion[0],.events=POLLIN};
    assert(poll(&fd,1,100)==1 && fd.revents&POLLHUP); finish_panel(&m);
    puts("PASS failed completion publication wakes peer HUP");
}
/* fx-uaccounts: pages without a one-second heartbeat adopt their worker's
 * result on the loop turn after its completion token. User Accounts kept
 * "Reading the accounts..." on the target because idle() ran cp_tick (the
 * only adopter) on clock pages alone. The production accounts glue runs here
 * without an account service seam (ENOSYS): the failed READ is reported once
 * and not re-submitted by the rebuild. The real ACCOUNTS service path is
 * tests/host/security/test_control_panel_accounts.py. */
static int accounts_adopted(struct cp_main *m)
{
    char want[96]; snprintf(want,sizeof(want),"The account service is not available (%d). Try again later.",-ENOSYS);
    return !m->app.account_busy && m->app.account_loaded && !m->accounts_job.started && !m->app.accounts.valid &&
           !strcmp(m->app.account_note,want);
}
static int network_adopted(struct cp_main *m)
{
    return !m->app.network_busy && m->app.network_loaded && !m->network_worker.active &&
           !strcmp(m->app.network_message,"Network status refreshed.");
}
static void worker_pages(void)
{
    static struct cp_main m;
    init_panel(&m,CP_HOME); accounts_attach(&m.app.host);
    cp_open(&m.app,CP_USERS); assert(m.app.account_busy && m.accounts_job.started);
    event_mode=1; event_panel=&m; adopted=accounts_adopted; expected_timeout=-1;
    assert(!control_run(&m) && waits==2 && !event_mode);
    struct du_widget *line=du_get(&m.ui,1622);   /* UA_LINE0: no stale "Reading the accounts..." */
    assert(line && !line->text[0]);
    finish_panel(&m);
    init_panel(&m,CP_HOME);
    m.network_worker.notify=notify_completion; m.network_worker.notify_ctx=&m;
    m.app.host.network_begin=network_begin; m.app.host.network_poll=network_poll;
    cp_open(&m.app,CP_NETWORK); assert(m.app.network_busy && m.network_worker.active);
    event_mode=1; event_panel=&m; adopted=network_adopted; expected_timeout=-1;
    assert(!control_run(&m) && waits==2 && !event_mode);
    network_worker_stop(&m.network_worker); finish_panel(&m); adopted=NULL;
    puts("PASS User Accounts and Network adopt their worker results on the turn after the completion, without a heartbeat; a failed READ is reported once");
}
/* A game result cannot consume the independent Start Menu mailbox. */
static void no_work(void *ctx) { (void)ctx; }
static void independent_jobs(void)
{
    static struct cp_main m; init_panel(&m,CP_HOME);
    assert(!cp_job_start(&m.menu_job,no_work,NULL,notify_completion,&m));
    await_completion(&m);
    assert(!game_begin(&m,1,NULL)); await_completion(&m);
    struct cp_game_result game;
    assert(game_poll_result(&m,&game)==1);
    assert(m.menu_job.started && cp_job_ready(&m.menu_job));
    m.app.host.menu_poll=menu_poll_io; m.app.menu_busy=1;
    cp_menu_poll(&m.app); assert(!m.app.menu_busy && !m.menu_job.started);
    finish_panel(&m); puts("PASS game adoption preserves the independent menu completion");
}
static void guarded_adoption(void)
{
    static struct cp_main m;
    for (int capture=0;capture<2;capture++) {
        init_panel(&m,CP_HOME); m.app.host.game_begin=game_begin; m.app.host.game_poll=game_poll_result;
        cp_game_open(&m.app,1); assert(m.app.game_busy); await_completion(&m);
        if (capture) m.ui.capture=1;
        else { static const char *const buttons[]={"OK"};
            du_message(&m.ui,999,"Keep open","A modal dialog",0,buttons,1,0,0); }
        /* All callers: direct adopter, tick, ready-game idle, heartbeat,
         * pending navigation and pending close. */
        cp_jobs_poll(&m.app); assert(m.app.game_busy);
        cp_tick(&m.app); assert(m.app.game_busy);
        idle(&m); assert(m.app.game_busy);
        m.app.display_write=1; m.app.navigation_pending=1; m.app.next_page=CP_JOYSTICK;
        idle(&m); assert(m.app.game_busy);
        m.app.close_pending=1;
        idle(&m); assert(m.app.game_busy);
        assert(capture ? m.ui.capture==1 : m.ui.dialog.kind==DU_DLG_MESSAGE);
        m.app.display_write=m.app.navigation_pending=m.app.close_pending=m.app.quit=0;
        m.ui.capture=-1; if (!capture) du_dialog_close(&m.ui);
        cp_jobs_poll(&m.app); assert(!m.app.game_busy && !m.game_job.started);
        finish_panel(&m);
    }
    puts("PASS modal/capture guard applies to every shared adoption caller");
}
static struct ciuki_settings input_saved;
static unsigned input_applied;
static int input_load(void *ctx,char *text,size_t size,size_t *length)
{(void)ctx;*length=cfg_format(&input_saved,text,size);return *length ? 0 : -1;}
static int input_save(void *ctx,const char *text,size_t length)
{(void)ctx;cfg_defaults(&input_saved);return cfg_parse(&input_saved,text,length) ? -1 : 0;}
static int input_apply(void *ctx,unsigned request,int value,int flag)
{(void)ctx;(void)value;(void)flag;assert(request==DESK_SETTINGS_RELOAD);input_applied++;return 0;}
static void input_properties(void)
{
    static struct cp_main m;cfg_defaults(&input_saved);strcpy(input_saved.extra,"foreign=preserved\n");
    init_panel(&m,CP_MOUSE);m.app.host.load=input_load;m.app.host.save=input_save;m.app.host.apply=input_apply;
    du_post(&m.ui,DU_EV_CHANGE,1503,1);assert(input_saved.mouse_swap);
    du_post(&m.ui,DU_EV_CHANGE,1504,7);assert(input_saved.mouse_trails==7);
    du_post(&m.ui,DU_EV_CHANGE,1505,101);assert(input_saved.wheel_lines==101);
    du_post(&m.ui,DU_EV_ACTIVATE,1506,0);assert(input_saved.pointer_scheme==1);
    struct du_widget *test=du_get(&m.ui,1508);assert(test);
    m.ui.now=1000000000;du_post(&m.ui,DU_EV_ACTIVATE,1508,0);
    m.ui.now+=200000000;du_post(&m.ui,DU_EV_ACTIVATE,1508,0);assert(!strcmp(test->text,"Double click detected"));
    cp_open(&m.app,CP_KEYBOARD);
    du_post(&m.ui,DU_EV_CHANGE,1500,3);du_post(&m.ui,DU_EV_CHANGE,1501,0);
    du_post(&m.ui,DU_EV_CHANGE,1502,0);du_post(&m.ui,DU_EV_CHANGE,1507,0);
    assert(input_saved.repeat_delay==3 && !input_saved.repeat_speed && !input_saved.cursor_blink_ms && !input_saved.layout_switch);
    assert(input_saved.mouse_swap && input_saved.mouse_trails==7 && strstr(input_saved.extra,"foreign=preserved") && input_applied==8);
    assert(du_get(&m.ui,1515) && du_get(&m.ui,1515)->kind==DU_EDIT);
    finish_panel(&m);puts("PASS G12 Keyboard/Mouse controls, double-click test, durable merge and apply");
}

int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);
    setvbuf(stdout,NULL,_IONBF,0);
    assert(argc==2); input_properties();idle_pages(); jobs(argv[1]); worker_pages(); independent_jobs(); guarded_adoption();
    puts("PASS Control Panel event-driven jobs"); fflush(stdout); return 0;
}
