/* Production kwait, FIFO locks, admission, scheduler and cpustat. Only the
 * stack-switch/device/clock boundaries are modeled. No FS_HOST pthread locks.
 * SPDX-License-Identifier: GPL-2.0-only */
#define HOST_SYNC_TIMERS
#define main latency_fixture_main
#include "../kernel_latency_test.c"
#undef main
#define P2V(p) ((void *)(uintptr_t)(p))
#define V2P(p) ((uint32_t)(uintptr_t)(p))
/* The clock instruction is irrelevant to wait policy; keep sync's production
 * deadlines, waiter lists and sleep/wake code unmodified. */
#define ktime_cycles sync_unused_cycles
#include "../../../src/kernel/core/sync.c"
#undef ktime_cycles
static void timer_fixture_reset(void) { timer_root=timer_first=timer_due_head=timer_due_tail=0; memset(&timer_stats,0,sizeof(timer_stats)); deferred_fn=0; }
#include "../../../src/kernel/fs/vfs.c"
#include "../../../src/kernel/fs/blkreq.c"
bool g_cpu_tsc;
uint32_t ticks_lo(void) { return (uint32_t)g_ticks; }
void klog(const char *fmt,...) { (void)fmt; }
void klog_diag(const char *fmt,...) { (void)fmt; }
static struct vfs space;
static fs_lock mutex;
static fs_lock outer;
static struct fs_event event;
static struct task *waiter;
static struct vfs_io_state holder_io;
static unsigned scenario,other_reads,device_reads,errors;
static bool pumping;
static uint64_t waiter_cpu;

/* The suspended waiter's C stack stays here while the production scheduler
 * dispatches the modeled device owner/independent-volume reader. Return only
 * when that same stack is selected again, as switch_context does on i686. */
static void dispatch(void)
{
    if (pumping) return;
    pumping=true;
    while (g_current!=waiter) {
        CHECK(g_ticks<2000);
        if (g_current==&normal) {
            vfs_lock(&space,vfs_drive_mask(2));
            struct vfs_io_state io=vfs_io_begin_locked(&space);
            other_reads++;run_tick(0x2222);kpreempt_point();
            vfs_io_end_locked(&space,io);vfs_unlock(&space);
            task_sleep_ms(1);
        } else if (g_current==&hog) {
            /* Each device completion arrives after 25 ms, then the device
             * is quarantined. No CPU time belongs to its blocked owner. */
            device_reads++;run_tick(0x3333);kpreempt_point();
            if (device_reads<3) task_sleep_ms(25);
            else {
                errors++;
                if (scenario==0) fs_lock_drop(&mutex);
                else if (scenario==1) fs_event_wake(&event);
                else { vfs_io_end_locked(&space,holder_io);vfs_unlock(&space); }
                hog.state=T_BLOCKED;hog.wake_tick=0;schedule();
            }
        } else {
            CHECK(g_current==&idle);run_tick(0);
            if (g_need_resched) schedule();
        }
    }
    CHECK(g_current->state==T_RUNNING);
    CHECK(g_current->run_cycles==waiter_cpu);
    pumping=false;
}
static void wait_case(unsigned which)
{
    reset();scenario=which;other_reads=device_reads=errors=0;
    vfs_init(&space);fs_lock_init(&mutex);fs_lock_init(&outer);fs_event_init(&event);
    hog.user=normal.user=ui.user=false;
    hog.prio=normal.prio=ui.prio=P_NORMAL;
    /* Seed a real owner and a real in-flight volume reservation, then block
     * that owner in the device before admitting the second caller. */
    idle.state=T_READY;hog.state=T_RUNNING;g_current=&hog;
    cpu_switch(&idle,&hog);
    if (which==0) fs_lock_take(&mutex);
    else if (which==2) { vfs_lock(&space,vfs_drive_mask(3));holder_io=vfs_io_begin_locked(&space); }
    hog.state=T_BLOCKED;ktimer_wait_task(&hog,25);
    cpu_switch(&hog,&ui);ui.state=T_RUNNING;g_current=&ui;
    waiter=&ui;waiter_cpu=ui.run_cycles;
    rq_push(&normal);
    latency_switch_hook=dispatch;
    /* Match nested namespace/volume/cache callers: an existing preemption
     * hold must not turn an explicit kwait sleep into a ready-return loop. */
    fs_lock_take(&outer);
    if (which==0) { fs_lock_take(&mutex);fs_lock_drop(&mutex); }
    else if (which==1) { fs_lock_take(&mutex);fs_event_wait(&event,&mutex);fs_lock_drop(&mutex); }
    else { vfs_lock(&space,vfs_drive_mask(3));vfs_unlock(&space); }
    fs_lock_drop(&outer);
    latency_switch_hook=0;
    cpu_point();
    CHECK(g_current==waiter && errors==1 && device_reads==3 && other_reads>=25);
    CHECK(!ui.preempt_holds && !hog.preempt_holds && !normal.preempt_holds);
    CHECK(ui.run_cycles==waiter_cpu && ui.max_stretch<=4*per_ms());
    CHECK(hog.max_stretch<=4*per_ms() && normal.max_stretch<=4*per_ms());
    CHECK(!waiters && !space.reserved && !space.active);
    printf("F1-80 production wait=%s device_reads=%u errors=%u other_volume_reads=%u elapsed_ms=%llu waiter_cpu_ms=%u stretch_max_ms=%u PASS\n",
           which==0 ? "fs_lock_take" : which==1 ? "fs_event_wait" : "vfs_admission",
           device_reads,errors,other_reads,(unsigned long long)g_ticks,
           ms(ui.run_cycles-waiter_cpu),ms(stretch_max));
    vfs_destroy(&space);fs_lock_destroy(&mutex);fs_lock_destroy(&outer);fs_event_destroy(&event);
}
/* f1-98 (R2): a priority-9 waiter for drive C (the production reservation
 * loop, with and without a request pause) or for the cache lock (a request
 * polling fs_lock_take_request) lends its priority to the 7 holder blocked
 * in the device; the holder returns to 7 when it releases. */
static fs_lock cache_lock;
static unsigned inherit_which, holder_checks;
static uint64_t req_now(void *a) { (void)a; return g_ticks*1000; }
static bool req_cancelled(void *a) { (void)a; return false; }
static void req_pause(void *a) { (void)a; task_sleep_ms(1); }
static void inherit_dispatch(void)
{
    if (pumping) return;
    pumping=true;
    while (g_current!=waiter) {
        CHECK(g_ticks<2000);
        if (g_current==&hog) {
            CHECK(ui.depends_on==&hog && hog.inherited_priority==9 && hog.effective_priority==9);
            holder_checks++;
            device_reads++; run_tick(0x3333); kpreempt_point();
            if (device_reads<3) task_sleep_ms(25);
            else {
                if (inherit_which==2) fs_lock_drop(&cache_lock);
                else { vfs_io_end_locked(&space,holder_io); vfs_unlock(&space); }
                /* Bounded to the hold: nothing is inherited after release. */
                CHECK(hog.effective_priority==7 && !hog.inherited_priority && !ui.depends_on && !dependents);
                hog.state=T_BLOCKED; hog.wake_tick=0; schedule();
            }
        } else {
            CHECK(g_current==&idle); run_tick(0);
            if (g_need_resched) schedule();
        }
    }
    pumping=false;
}
static void inherit_case(unsigned which)
{
    reset(); inherit_which=which; device_reads=holder_checks=0;
    vfs_init(&space); fs_lock_init(&cache_lock); fs_lock_classify(&cache_lock,3,0);
    hog.user=normal.user=ui.user=false;
    task_set_class(&hog,P_NORMAL); task_set_class(&ui,P_INTERACTIVE); /* as task creation */
    normal.state=T_BLOCKED; idle.state=T_READY; hog.state=T_RUNNING; g_current=&hog;
    cpu_switch(&idle,&hog);
    if (which==2) fs_lock_take(&cache_lock);
    else { vfs_lock(&space,vfs_drive_mask(2)); holder_io=vfs_io_begin_locked(&space); }
    CHECK(space.hold_token[2]==(which==2 ? 0 : (uintptr_t)&hog));
    hog.state=T_BLOCKED; ktimer_wait_task(&hog,25);
    cpu_switch(&hog,&ui); ui.state=T_RUNNING; g_current=&ui; waiter=&ui;
    latency_switch_hook=inherit_dispatch;
    struct vfs_request request={.now=req_now,.cancelled=req_cancelled,.pause=req_pause,
        .deadline=UINT64_MAX/2,.operation="stat",.site="native"};
    if (which==0) { vfs_lock(&space,vfs_drive_mask(2)); vfs_unlock(&space); }
    else if (which==1) { CHECK(!vfs_admit(&space,vfs_drive_mask(2),&request)); vfs_unlock(&space); }
    else {
        struct fs_request_scope scope; fs_request_enter(&scope,&request);
        CHECK(!fs_lock_take_request(&cache_lock,VFS_WAIT_CACHE)); fs_lock_drop(&cache_lock);
        fs_request_leave(&scope);
    }
    latency_switch_hook=0;
    CHECK(g_current==&ui && device_reads==3 && holder_checks==3);
    CHECK(!ui.depends_on && !dependents && !ui.preempt_holds && !hog.preempt_holds);
    CHECK(!space.reserved && !space.active && !space.hold_token[2]);
    CHECK(inherit_used==1 && inherit_events[0].from_id==ui.id && inherit_events[0].to_id==hog.id && inherit_events[0].level==9);
    CHECK(inherit_events[0].reason==(which==2 ? KSYNC_DEPEND_CACHE : KSYNC_DEPEND_DRIVE));
    printf("F1-98 production %s: priority-9 waiter lends 9 to the 7 holder in all %u holder dispatches, reason=%s, released at unlock PASS\n",
           which==0 ? "drive reservation (vfs_lock)" : which==1 ? "drive reservation (request admission, 1 ms pause)" : "cache lock (fs_lock_take_request)",
           holder_checks,which==2 ? "cache" : "drive");
    vfs_destroy(&space); fs_lock_destroy(&cache_lock);
}
/* T23 trial 2ca3b0b6: after an appearance change the CPU stayed busy
 * (idle_pm=0) and ata0 reported svc_max_ms=105, usb-storage 1079. A queued
 * request runs on its controller's ordinary-priority "storage-io" worker,
 * which is dispatched twice per command (issue, then the continuation after
 * the completion interrupt). Replay: the production queue, scheduler,
 * ksync_depend and kwait; the worker is `hog` (base 7), a CPU-bound renderer
 * `normal` at the interactive level 9 never blocks, and the requester `ui`
 * is an interactive reader polling with a native request (1 ms pause) or a
 * device-class storage writer sleeping on the request event. The negative
 * control (no executor donation) reproduces the 100 ms starvation dispatch
 * at each worker step; with donation the worker runs at the requester's
 * level and the request completes in the device time plus at most one
 * renderer slice per step. */
#define EXEC_DEVICE_MS 5u
static struct blk_queue exec_queue;
static struct blkdev exec_dev;
static struct blk_request exec_request;
static uint8_t exec_buffer[4096];
static unsigned exec_phase,exec_reads,exec_worker_turns,exec_renderer_ticks,exec_level;
static bool exec_donation;
static uint64_t exec_issue_tick,exec_done_tick;
static int exec_read(struct blkdev *d,uint64_t lba,uint32_t n,void *buf)
{ (void)d;(void)lba; exec_reads++; memset(buf,0x5a,(size_t)n*512u); return 0; }
static void exec_dispatch(void)
{
    if (pumping) return;
    pumping=true;
    while (g_current!=waiter) {
        CHECK(g_ticks<5000);
        if (g_current==&hog) {
            exec_worker_turns++;
            /* With donation the worker runs at least at the requester's
             * level for every dispatch of this request. */
            if (exec_donation) CHECK(waiter->depends_on==&hog && hog.effective_priority>=exec_level);
            else CHECK(!waiter->depends_on && !hog.inherited_priority);
            if (exec_phase==0) {
                /* Issue: the command now runs in the device for 5 ms. */
                exec_phase=1; exec_issue_tick=g_ticks; run_tick(0x5555);
                task_sleep_ms(EXEC_DEVICE_MS);
            } else {
                /* Continuation after the completion interrupt: the
                 * production run_one executes, finishes and wakes. */
                CHECK(exec_phase==1); exec_phase=2;
                CHECK(blk_queue_run_one(&exec_queue));
                exec_done_tick=g_ticks;
                /* The inheritance ends with the request it served. */
                CHECK(!hog.inherited_priority && !dependents && !waiter->depends_on);
                hog.state=T_BLOCKED; hog.wake_tick=0; schedule();
            }
        } else if (g_current==&normal) {
            /* The dispatcher runs inside switch_context (IF clear): the
             * renderer's preemption at its tick is an explicit schedule. */
            exec_renderer_ticks++; run_tick(0x4444);
            if (g_need_resched) schedule();
        } else {
            CHECK(g_current==&idle); run_tick(0);
            if (g_need_resched) schedule();
        }
    }
    pumping=false;
}
static uint64_t executor_case(bool donation,bool device_writer)
{
    reset(); exec_donation=executor_donation=donation;
    exec_phase=exec_reads=exec_worker_turns=exec_renderer_ticks=0; exec_issue_tick=exec_done_tick=0;
    hog.user=normal.user=ui.user=false;
    task_set_class(&hog,P_NORMAL);          /* blk_queue_start: "storage-io", P_NORMAL */
    task_set_class(&normal,P_INTERACTIVE);  /* desktop/client rendering after the change */
    task_set_class(&ui,device_writer ? P_DEVICE : P_INTERACTIVE);
    exec_level=ui.effective_priority;
    fs_lock_init(&pool_lock);
    blk_queue_init(&exec_queue,"storage-io",BLKREQ_COMMAND_BYTES); exec_queue.worker=&hog;
    exec_dev=(struct blkdev){.read=exec_read,.request_read=exec_read,.capacity=4096,.sector_size=512,.queue=&exec_queue};
    memset(&exec_request,0,sizeof(exec_request));
    exec_request.device=&exec_dev; exec_request.queue=&exec_queue; exec_request.buffer=exec_buffer;
    exec_request.length=sizeof(exec_buffer); exec_request.operation=BLK_READ; exec_request.priority=BLK_DEMAND;
    exec_request.timeout=30000; exec_request.state=BLK_PREPARED; fs_event_init(&exec_request.event);
    /* The renderer is ready and never blocks; the worker sleeps for work. */
    idle.state=T_READY; hog.state=T_BLOCKED; normal.state=T_READY; rq_push(&normal);
    cpu_switch(&idle,&ui); ui.state=T_RUNNING; g_current=&ui; waiter=&ui;
    struct vfs_request request={.now=req_now,.cancelled=req_cancelled,.pause=req_pause,
        .deadline=UINT64_MAX/2,.operation="read",.site="native"};
    struct fs_request_scope scope;
    if (!device_writer) { fs_request_enter(&scope,&request); exec_request.context=&request; }
    uint64_t start=g_ticks;
    latency_switch_hook=exec_dispatch;
    CHECK(!blk_request_submit(&exec_request));
    task_start(&hog); /* the queue's changed event wakes its worker */
    int e=blk_request_wait(&exec_request);
    latency_switch_hook=0;
    if (!device_writer) fs_request_leave(&scope);
    uint64_t latency=g_ticks-start;
    CHECK(!e && exec_reads==1 && exec_phase==2 && exec_worker_turns==2);
    CHECK(exec_request.state==BLK_COMPLETED && !exec_queue.busy && !exec_queue.head);
    CHECK(!ui.depends_on && !dependents && !hog.inherited_priority && hog.effective_priority==7);
    CHECK(!ui.preempt_holds && !hog.preempt_holds && !normal.preempt_holds);
    bool recorded=false;
    for (unsigned i=0;i<inherit_used;i++)
        if (inherit_events[i].reason==KSYNC_DEPEND_IO && inherit_events[i].from_id==ui.id &&
            inherit_events[i].to_id==hog.id && inherit_events[i].level>=exec_level) recorded=true;
    CHECK(recorded==donation);
    uint64_t service=exec_done_tick-start;
    /* With donation each worker step waits at most one renderer slice
     * (10 ms) and the requester its own turn after completion; without it
     * each step waits for the 100 ms starvation dispatch (svc_max_ms=105). */
    if (donation) CHECK(service<=EXEC_DEVICE_MS+2*10+2 && latency<=EXEC_DEVICE_MS+3*10+2);
    else CHECK(service>=2*100 && exec_issue_tick-start>=100);
    printf("T23 2ca3b0b6 storage-io executor: requester=%s level=%u donation=%s issue_ms=%llu service_ms=%llu "
           "requester_ms=%llu worker_turns=%u renderer_ms=%u PASS\n",device_writer ? "storage-writer" : "native-reader",
           exec_level,donation ? "on" : "off (negative control)",(unsigned long long)(exec_issue_tick-start),
           (unsigned long long)service,(unsigned long long)latency,exec_worker_turns,exec_renderer_ticks);
    fs_event_destroy(&exec_request.event); executor_donation=true;
    return latency;
}
int main(void)
{
    for (unsigned i=0;i<3;i++) wait_case(i);
    for (unsigned i=0;i<3;i++) inherit_case(i);
    for (unsigned writer=0;writer<2;writer++) {
        uint64_t slow=executor_case(false,writer!=0), fast=executor_case(true,writer!=0);
        CHECK(fast*4<slow);
    }
    return 0;
}
