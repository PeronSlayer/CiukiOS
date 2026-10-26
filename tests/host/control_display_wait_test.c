/* Production native poll, lwIP event wait, kwait, channels, input publication,
 * signals, timers, scheduler and CPU accounting on one emulated CPU.
 * Only privileged instructions, user memory and allocation are host boundaries.
 * Dispatch/draw costs are assumptions, not T23 hardware measurements.
 * SPDX-License-Identifier: MIT */
#define HOST_SYNC_TIMERS
#define CIUKI_CPU_H
#include <ciuki/process.h>
#include <ciuki/biosvm.h>
#define P2V(p) ((void *)(uintptr_t)(p))
#define V2P(p) ((uint32_t)(uintptr_t)(p))
#define main latency_fixture_main
#include "kernel_latency_test.c"
#undef main
#define ktime_cycles sync_unused_cycles
#include "../../src/kernel/core/sync.c"
#undef ktime_cycles
static void timer_fixture_reset(void)
{
    timer_root=timer_first=timer_due_head=timer_due_tail=0;
    memset(&timer_stats,0,sizeof(timer_stats));deferred_fn=0;
}
bool g_cpu_tsc;
uint32_t ticks_lo(void) { return (uint32_t)g_ticks; }
void klog(const char *fmt,...) { (void)fmt; }
void klog_diag(const char *fmt,...) { (void)fmt; }
static uint8_t inb(uint16_t p) { (void)p; return 0xff; }
static void outb(uint16_t p,uint8_t v) { (void)p;(void)v; }

/* Keep the complete production native port. Rename only its exported wait so
 * the harness can count entries and inject the readiness/registration races. */
#define cu_wait_change native_cu_wait_change
#include "native_poll_port.inc"
#undef cu_wait_change
static unsigned wait_calls, race_phase;
static bool entering_wait;
static void publish(void);
void cu_wait_change(unsigned seq,uint64_t deadline)
{
    wait_calls++;
    CHECK(wait_calls<10000); /* fail a regressed infinite ready-return loop */
    cycles+=150; /* explicit cost of a kernel wait turn; negative control below */
    if (race_phase==1) { race_phase=0; publish(); }
    entering_wait=true;
    native_cu_wait_change(seq,deadline);
    entering_wait=false;
}
#include <ciuki/desktop.h>
extern int channel_poll_events(struct desktop_description *) __attribute__((weak));
#pragma weak channel_fd_closed
#include "../../src/kernel/proc/channel.c"
/* This pacing fixture has no credential/session store. Model one live subject
 * and count its channel references; credential policy is exercised by H01. */
static unsigned subject_token, subject_refs;
static struct security_subject *fixture_subject(void)
{ return (struct security_subject *)&subject_token; }
bool security_process_live(const struct process *p)
{ return p && p->state==PROC_LIVE && p->subject==fixture_subject(); }
bool security_subject_live(const struct security_subject *s)
{ return s==fixture_subject(); }
bool security_subject_matches(const struct security_subject *s,const struct process *p)
{ return security_process_live(p) && s==p->subject; }
uint32_t security_subject_pid(const struct security_subject *s)
{ CHECK(s==fixture_subject()); return 42; }
int security_subject_get(struct security_subject *s)
{ CHECK(s==fixture_subject()); subject_refs++; return 0; }
void security_subject_put(struct security_subject *s)
{ if (s) { CHECK(s==fixture_subject() && subject_refs); subject_refs--; } }
void pmm_free(uint32_t page) { (void)page; CHECK(false); }
uint32_t pmm_alloc(void) { CHECK(false); return 0; }
void security_process_stopping(struct process *p)
{ CHECK(p && p->subject==fixture_subject()); channel_security_revoke(p); }
#include "../../src/kernel/drivers/i8042.c"
#include "../../src/kernel/net/lwip/sockets.c"

static struct process client, supervisor;
static struct proc_fd test_fds[CIUKI_OPEN_MAX];
static struct proc_thread ui_thread, worker_thread;
static struct ciuki_sigaction actions[SIGCHLD+1];
struct desktop_ledger desktop_objects;
static unsigned heap_live, pins;
void *kzalloc(size_t n) { void *p=calloc(1,n);CHECK(p);heap_live++;return p; }
void kfree(void *p) { if(p) { CHECK(heap_live);heap_live--;free(p); } }
static void fixture_retain(struct proc_object *o) { ((struct desktop_description *)o)->references++; }
bool desktop_object_is_channel(const struct proc_object *o)
{ return o && o->retain==fixture_retain && ((const struct desktop_description *)o)->kind==DESKTOP_CHANNEL; }
static void fixture_release(struct proc_object *o)
{
    struct desktop_description *d=(void *)o;
    CHECK(d->references);
    if (!--d->references) {
        if (d->kind==DESKTOP_CHANNEL) channel_release(d);
        desktop_objects.descriptions--;kfree(d);
    }
}
struct desktop_description *desktop_description_new(enum desktop_kind k,uint32_t f)
{
    struct desktop_description *d=kzalloc(sizeof(*d));
    d->object=(struct proc_object){fixture_retain,fixture_release,true};
    d->kind=k;d->flags=f;d->references=1;desktop_objects.descriptions++;return d;
}
static struct proc_object reserved_slot;
struct file_description *file_fd(const struct process *p,int32_t fd) { (void)p;(void)fd;return NULL; }
int file_description_close(struct file_description *d) { (void)d;CHECK(false);return -EIO; }
#include "native_poll_fd.inc"
struct desktop_description *desktop_fd(const struct process *p,int32_t fd)
{ return (struct desktop_description *)file_fd_object(p,fd); }
int desktop_fd_slot(const struct process *p,unsigned start) { return file_fd_slot(p,start); }
uint32_t file_description_count(void) { return 0; }
int surface_attachment(struct process *p,int32_t fd,struct desktop_description **d)
{ (void)p;(void)fd;(void)d;return -EBADF; }
struct proc_thread *proc_thread_for(const struct task *t)
{ return t==&ui ? &ui_thread : t==&normal ? &worker_thread : NULL; }
struct proc_thread *proc_thread_slot(unsigned i)
{ return i==0 ? &ui_thread : i==1 ? &worker_thread : NULL; }
bool task_alive(const struct task *t) { return t && t->state!=T_ZOMBIE && t->state!=T_DEAD; }
void ww_interrupt(struct ww_waiter *w,int e) { (void)w;(void)e; }
void task_kill(struct task *t,int e) { (void)e;CHECK(t!=g_current);t->state=T_ZOMBIE; }
void proc_notify(void) { }
extern void w32_process_stopping(struct process *,uint32_t) __attribute__((weak));
#include "native_poll_process.inc"
#include "../../src/kernel/proc/signal.c"

static struct ciuki_message user_messages[2];
static struct ciuki_message *user_message(uint32_t address)
{ CHECK(address==4096 || address==8192);return &user_messages[address==8192]; }
int copy_from_user(void *out,uint32_t address,uint32_t n)
{ CHECK(n==sizeof(struct ciuki_message));memcpy(out,user_message(address),n);return 0; }
int copy_to_user(uint32_t address,const void *in,uint32_t n)
{ CHECK(n==sizeof(struct ciuki_message));memcpy(user_message(address),in,n);return 0; }
int ua_pin(struct uaddr *a,struct ua_pin *p,uint32_t address,uint32_t n,bool write)
{ (void)a;(void)p;(void)write;CHECK(n==sizeof(struct ciuki_message));(void)user_message(address);pins++;return 0; }
void ua_unpin(struct uaddr *a,struct ua_pin *p) { (void)a;(void)p;CHECK(pins);pins--; }

extern int cp_display_wait_ms(uint64_t now,uint64_t deadline);
static int32_t pair[2];
static int input_fd;
static struct task *suspended;
static unsigned source, event_ms, depth, worker_ticks;
static bool sent;
static uint64_t blocked_cpu, blocked_kernel;
static bool no_timer(const struct task *t)
{ return !t->wake_tick && (t->wake_timer.state==KTIMER_IDLE || t->wake_timer.state==KTIMER_CANCELLED); }
enum { COMPLETION, RAW_INPUT, CLOSE_FD, PEER_CLOSE, CAUGHT_SIGNAL, STOP, SPURIOUS, TIMEOUT, IGNORED_SIGNAL, MASKED_SIGNAL, FORWARDED_INPUT, DRAIN_PEER, SPURIOUS_TIMEOUT };
static void publish(void)
{
    sent=true;
    switch(source) {
    case COMPLETION:
    case SPURIOUS:
    case IGNORED_SIGNAL:
    case MASKED_SIGNAL:
        CHECK(!channel_send(&client,pair[1],4096,0));break;
    case FORWARDED_INPUT:
        CHECK(!channel_send(&client,pair[1],4096,CIUKI_CHANNEL_INPUT));
        CHECK(ui.effective_priority==11);break;
    case DRAIN_PEER: CHECK(channel_recv(&client,pair[1],8192,DONTWAIT)==1);break;
    case RAW_INPUT: {
        /* Actual input producer and deferred IRQ wake, including IF=0. */
        uint32_t f=irq_save();enqueue(&native,INPUT_KEY,0x1e,1,g_ticks);irq_restore(f);break;
    }
    case CLOSE_FD: {
        struct proc_object *old=test_fds[pair[0]].object;
        CHECK(!file_close(&client,pair[0]));
        /* Reuse the same slot and exact object: the generation must reject it. */
        old->retain(old);test_fds[pair[0]]=(struct proc_fd){old,0};
        net_fd_changed(&client,(unsigned)pair[0]);break;
    }
    case PEER_CLOSE: CHECK(!file_close(&client,pair[1]));break;
    case CAUGHT_SIGNAL: proc_signal_post(&client,&ui_thread,SIGUSR1,client.pid);break;
    case STOP: proc_stop(&client,0,SIGTERM);break;
    default: CHECK(false);
    }
}
static void registration_race(void)
{
    if (g_current!=&ui || !entering_wait) return;
    if ((race_phase==2 && !ui.sync_waiter) ||
        (race_phase==3 && ui.sync_waiter && ui.state==T_BLOCKED)) {
        race_phase=0;timing_end_hook=0;publish();
    }
}
/* Nested dispatch preserves each suspended native C stack until that exact
 * task is selected again, as switch_context does on the real single CPU. */
static void dispatch(void)
{
    struct task *return_to=suspended;
    if (depth) return;
    depth++;
    blocked_cpu=ui.run_cycles;blocked_kernel=ui.kernel_ticks;
    while (g_current!=return_to) {
        CHECK(g_ticks<70001);
        CHECK(ui.state==T_BLOCKED && ui.run_cycles==blocked_cpu && ui.kernel_ticks==blocked_kernel);
        if (source==TIMEOUT || source==SPURIOUS_TIMEOUT) CHECK(ui.wake_tick==event_ms);
        else CHECK(no_timer(&ui));
        if (g_current==&normal) worker_ticks++;
        run_tick(g_current==&normal ? 0x400000 : 0);
        if ((source==SPURIOUS || source==SPURIOUS_TIMEOUT) && g_ticks==25) cu_changed();
        if ((source==IGNORED_SIGNAL || source==MASKED_SIGNAL) && g_ticks==25)
            proc_signal_post(&client,&ui_thread,SIGUSR1,client.pid);
        if (!sent && source!=TIMEOUT && source!=SPURIOUS_TIMEOUT && g_ticks>=event_ms) publish();
        if (g_need_resched) schedule();
    }
    depth--;
}
static void setup(void)
{
    CHECK(!heap_live && !pins && !waiters && !subject_refs);
    reset();normal.state=hog.state=ui.state=T_BLOCKED;
    task_set_class(&ui,P_NORMAL);task_focus(&ui);
    memset(test_fds,0,sizeof(test_fds));memset(actions,0,sizeof(actions));
    client=(struct process){.pid=42,.state=PROC_LIVE,.fds=test_fds,.actions=actions,.subject=fixture_subject()};
    ui_thread=(struct proc_thread){.task=&ui,.process=&client,.tid=ui.id,.slot=0,.in_syscall=true};
    worker_thread=(struct proc_thread){.task=&normal,.process=&client,.tid=normal.id,.slot=1,.in_syscall=true};
    actions[SIGUSR1].handler=0x400000;
    memset(&native.queue,0,sizeof(native.queue));
    memset(user_messages,0,sizeof(user_messages));user_messages[0].length=1;user_messages[0].data[0]=42;
    CHECK(!channel_pair(&client,pair));
    input_fd=desktop_fd_slot(&client,0);CHECK(input_fd>=0);
    struct desktop_description *input=desktop_description_new(DESKTOP_INPUT,O_RDONLY);
    input->owner=client.pid;test_fds[input_fd].object=&input->object;
    task_start(&ui);schedule();CHECK(g_current==&ui);
    /* Register the UI receiver using the production channel handshake. */
    CHECK(!channel_send(&client,pair[0],4096,0));
    task_start(&normal);ui.state=T_BLOCKED;schedule();CHECK(g_current==&normal);
    CHECK(channel_recv(&client,pair[1],8192,DONTWAIT)==1);
    task_start(&ui);normal.state=T_BLOCKED;schedule();CHECK(g_current==&ui);
    task_start(&normal);
    CHECK(normal.base_priority==7 && ui.base_priority==9);
    wait_calls=race_phase=worker_ticks=depth=0;sent=false;
    source=COMPLETION;event_ms=250;suspended=&ui;
    blocked_cpu=ui.run_cycles+150;blocked_kernel=ui.kernel_ticks;
    latency_switch_hook=dispatch;
}
static void finish(void)
{
    latency_switch_hook=0;timing_end_hook=0;
    for (unsigned i=0;i<CIUKI_OPEN_MAX;i++) if(test_fds[i].object) CHECK(!file_close(&client,(int)i));
    CHECK(!heap_live && !pins && !desktop_objects.messages && !desktop_objects.channels && !subject_refs);
    CHECK(!ui.sync_waiter && !normal.sync_waiter && !waiters && !ui_thread.cleanup && !worker_thread.cleanup);
}
static int poll_ui(int timeout)
{
    struct ciuki_pollfd fds[2]={{pair[0],POLLIN,0},{input_fd,POLLIN,0}};
    int result=net_poll(&client,fds,2,timeout,NULL);
    if (source==RAW_INPUT && result==1) CHECK(fds[1].revents==POLLIN);
    if (source==CLOSE_FD && result==1) CHECK(fds[0].revents==POLLNVAL);
    if (source==PEER_CLOSE && result==1) CHECK(fds[0].revents&POLLHUP);
    /* Stopping revokes the subject's channels before the wait resumes.
     * Poll observes ready HUP before its interruption check. */
    if (source==STOP) CHECK(client.state==PROC_STOPPING && result==1 && (fds[0].revents&POLLHUP));
    return result;
}
static void dispatch_idle_worker(void)
{
    if(depth) return;
    depth++;
    uint64_t worker_cpu=normal.run_cycles;
    while(g_current!=&normal) {
        CHECK(g_current==&idle && g_ticks<70000);
        CHECK(ui.state==T_BLOCKED && normal.state==T_BLOCKED);
        CHECK(no_timer(&ui) && no_timer(&normal));
        CHECK(normal.run_cycles==worker_cpu && !normal.kernel_ticks && !ui.run_cycles);
        run_tick(0);
        if(g_ticks==65000) CHECK(!channel_send(&client,pair[0],4096,0));
        if(g_need_resched) schedule();
    }
    depth--;
}
int main(void)
{
    CHECK(cp_display_wait_ms(0,UINT64_MAX)==-1);
    CHECK(cp_display_wait_ms(1000,1000)==1);
    CHECK(cp_display_wait_ms(0,20000000)==20);
    for (unsigned kind=COMPLETION;kind<=SPURIOUS_TIMEOUT;kind++) {
        if(kind==DRAIN_PEER) continue;
        setup();source=kind;
        if (kind==IGNORED_SIGNAL) actions[SIGUSR1].handler=SIG_IGN;
        if (kind==MASKED_SIGNAL) ui_thread.mask=CIUKI_SIGBIT(SIGUSR1);
        bool timed=kind==TIMEOUT || kind==SPURIOUS_TIMEOUT;
        int result=poll_ui(timed ? (int)event_ms : -1);
        if (result!=(kind==CAUGHT_SIGNAL ? -EINTR : timed ? 0 : 1))
            fprintf(stderr,"native poll kind=%u result=%d\n",kind,result);
        CHECK(result==(kind==CAUGHT_SIGNAL ? -EINTR : timed ? 0 : 1));
        CHECK(g_ticks==event_ms+(kind==RAW_INPUT) && wait_calls==((kind==SPURIOUS || kind==SPURIOUS_TIMEOUT) ? 2u : 1u));
        CHECK(ui.kernel_ticks==0 && ui.run_cycles==wait_calls*150);
        finish();
    }
    puts("native poll: completion, raw/forwarded input, close/reuse, peer HUP, caught signal, stop, spurious notification, original deadline, ignored/masked signals PASS");
    for (unsigned phase=1;phase<=3;phase++) for (unsigned kind=COMPLETION;kind<=CAUGHT_SIGNAL;kind++) {
        setup();source=kind;race_phase=phase;
        if (phase>1) timing_end_hook=registration_race;
        int result=poll_ui(-1);
        CHECK(result==(kind==CAUGHT_SIGNAL ? -EINTR : 1));
        CHECK(sent && !race_phase && wait_calls==1 && g_ticks==(phase==3 && kind==RAW_INPUT));
        finish();
    }
    puts("native poll races: readiness-to-wait, event-to-registration, registered-before-schedule; all producers PASS");
    setup();source=COMPLETION;event_ms=65000;
    rq_remove(&normal);normal.state=T_BLOCKED;
    CHECK(poll_ui(-1)==1 && wait_calls==1 && g_ticks==65000);
    CHECK(ui.run_cycles==150 && ui.kernel_ticks==0 && !normal.run_cycles);
    finish();
    setup();latency_switch_hook=0;
    ui.state=T_BLOCKED;schedule();CHECK(g_current==&normal);
    latency_switch_hook=dispatch_idle_worker;
    CHECK(channel_recv(&client,pair[1],8192,0)==1 && g_ticks==65000);
    CHECK(!normal.run_cycles && !normal.kernel_ticks && !ui.run_cycles);
    finish();
    puts("native UI poll and idle worker receive: each waits 65000 ms, no timers or periodic kernel time PASS");
    for(unsigned send=0;send<2;send++) for(unsigned event=0;event<3;event++) {
        setup();source=event==0 ? (send ? DRAIN_PEER : COMPLETION) : event==1 ? CAUGHT_SIGNAL : PEER_CLOSE;
        if(send) for(unsigned i=0;i<CIUKI_CHANNEL_QUEUE_MAX;i++) CHECK(!channel_send(&client,pair[0],4096,DONTWAIT));
        int result=send ? channel_send(&client,pair[0],4096,0) : channel_recv(&client,pair[0],8192,0);
        CHECK(result==(event==0 ? (send ? 0 : 1) : event==1 ? -EINTR : send ? -EPIPE : 0));
        CHECK(g_ticks==event_ms && !wait_calls && !ui.run_cycles && !ui.kernel_ticks);
        finish();
    }
    puts("native channel full-send/empty-receive: event, caught signal and peer close, no deadline timers PASS");
    /* Each tab uses the same native wait set while the worker stays runnable.
     * Input is delivered every 250 ms. Count actual wait entries and service
     * each input with an explicitly assumed 1 ms of kernel dispatch/draw. */
    for (unsigned tab=0;tab<3;tab++) {
        setup();source=FORWARDED_INPUT;
        unsigned worst=0;
        for (unsigned n=0;n<12;n++) {
            event_ms=(unsigned)g_ticks+250;sent=false;blocked_cpu=ui.run_cycles+150;blocked_kernel=ui.kernel_ticks;
            CHECK(poll_ui(-1)==1 && g_ticks==event_ms);
            CHECK(channel_recv(&client,pair[0],8192,DONTWAIT)==1);
            task_input_delivered(&ui);
            uint64_t injected=g_ticks;
            ui.user=false;run_tick(0);ui.user=true;
            unsigned latency=(unsigned)(g_ticks-injected);if(latency>worst)worst=latency;
        }
        CHECK(wait_calls==12 && worker_ticks>=3000 && worst<100 && ui.kernel_ticks==12);
        CHECK(ui.run_cycles==12000+12*150);
        finish();
        printf("Display tab %u: native poll, 3000 worker ticks, 12 input turns, worst=%u ms, periodic waiting CPU=0 PASS\n",tab,worst);
    }
    /* Negative control: old 1 ms deadline under the same production wait
     * queues/scheduler, charging the same 150 us per native wait turn. */
    setup();source=TIMEOUT;
    for (unsigned n=0;n<1000;n++) {
        event_ms=(unsigned)g_ticks+1;blocked_cpu=ui.run_cycles+150;
        CHECK(poll_ui(1)==0);
    }
    CHECK(wait_calls==1000 && ui.run_cycles==150000);
    finish();
    puts("negative control: 1000 native 1 ms waits, 150000 us kernel work (>1 percent) PASS");
    return 0;
}
