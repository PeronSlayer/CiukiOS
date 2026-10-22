/* SPDX-License-Identifier: MIT
 * Production Control Panel loop/jobs with host channels and a virtual clock.
 * Only the desktop turn and clock/poll boundary are replaced. Native wait
 * scheduling is independently exercised by control_display_wait_test.c. */
#include "replay_base.h"
#include <assert.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
static int loop_poll(struct pollfd *,nfds_t,int);
#define poll loop_poll
#define main cp_native_main
#include "../../../apps/control/main.c"
#undef main
#undef poll

static uint64_t virtual_ns;
static int stopped, expected_timeout, waits, fail_send, event_mode;
static struct cp_main *event_panel;
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
        assert(event_panel->programs_ready && event_panel->program_count==1 && !event_panel->program_scan.started);
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
    return n==(ssize_t)sizeof(*msg) ? 0 : -1;
}
/* Native services are outside the fixture; program inventory has one record. */
int ciuki_dosvm(uint32_t op,struct ciuki_dosvm_command *c)
{ (void)op; (void)c; return -1; }
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
    while (!ciuki_channel_recv(m->completion[0],&msg,DONTWAIT)) { assert(msg.length==4 && ++count<2048); }
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
        expected_timeout=m.app.page==CP_DATETIME || m.app.page==CP_SYSTEM || m.app.page==CP_SOUNDS ? 1000 : -1;
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
    puts("PASS every Control Panel page: one event/deadline wait, no service polling; clocks/mixer 1s, graphics query 2s only");
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
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);
    assert(argc==2); idle_pages(); jobs(argv[1]);
    puts("PASS Control Panel event-driven jobs"); fflush(stdout); return 0;
}
