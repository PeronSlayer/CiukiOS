/* Production scheduler + sleeping mutexes; only CPU/context boundaries faked.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/process.h>
#define HOST_SYNC_TIMERS
#define main latency_fixture_main
#include "kernel_latency_test.c"
#undef main
#include <setjmp.h>
#define ktime_cycles sync_unused_cycles
#include "../../src/kernel/core/sync.c"
#undef ktime_cycles
static void timer_fixture_reset(void)
{
    timer_root=timer_first=timer_due_head=timer_due_tail=0;
    memset(&timer_stats,0,sizeof(timer_stats));deferred_fn=0;
}
#define P2V(p) ((void *)(uintptr_t)(p))
#define V2P(p) ((uint32_t)(uintptr_t)(p))
#include "../../src/kernel/fs/fs_port.h"
bool g_cpu_tsc;
static unsigned cycle_records;
void klog(const char *fmt, ...) { if (strstr(fmt,"donation_cycle")) cycle_records++; }
static struct kmutex locks[2];
static struct task middle;
static unsigned stage, scenario;
static unsigned inversion_priority;
static jmp_buf cancelled;

static void priority_reset(void)
{
    reset(); waiters=0; held=0; owned_queues=0; propagating=false; cycle_records=0; dependents=0;
    normal.state=hog.state=ui.state=T_BLOCKED;
    middle=(struct task){.id=5,.generation=1,.prio=P_NORMAL,.state=T_BLOCKED};
    strcpy(middle.name,"middle");ui.all_next=&middle;
    kmutex_init(&locks[0]);kmutex_init(&locks[1]);
}
static void running(struct task *t)
{
    if (t->state==T_READY) rq_remove(t);
    t->state=T_RUNNING;g_current=t;priority_init(t);
}
static void park(void) { g_current->state=T_BLOCKED;g_current->wake_tick=0;schedule(); }
static void input_and_io(void)
{
    priority_reset(); task_start(&normal); task_start(&ui);
    task_focus(&normal); CHECK(normal.base_priority==9 && normal.effective_priority==9);
    task_input_delivered(&ui);CHECK(!ui.input_priority);
    task_input_delivered(&normal);CHECK(normal.effective_priority==11 && normal.input_expiry==20);
    schedule();CHECK(g_current==&normal);
    for(unsigned i=0;i<10;i++)run_tick(0);
    task_input_delivered(&normal);CHECK(normal.effective_priority==11 && normal.input_expiry==30);
    for(unsigned i=0;i<19;i++)run_tick(0);
    CHECK(normal.input_priority==11);run_tick(0);CHECK(normal.effective_priority==9);
    task_input_delivered(&normal);task_focus(&ui);
    CHECK(normal.base_priority==7 && !normal.input_priority && normal.effective_priority==7);
    CHECK(scheduler_stats.input_granted==3 && scheduler_stats.input_expired==2);
    task_focus(0);normal.state=T_BLOCKED;
    task_io_completed(&normal);CHECK(normal.io_priority==8 && normal.io_ticks==10);
    task_start(&normal);running(&normal);
    for(unsigned i=0;i<9;i++)run_tick(0);
    CHECK(normal.io_priority==8);run_tick(0);
    CHECK(!normal.io_priority && normal.effective_priority==7 && scheduler_stats.io_expired==1);
    task_io_completed(&normal);CHECK(scheduler_stats.io_granted==1); /* polling */
    priority_reset();task_focus(&normal);g_ticks=UINT64_MAX-10;
    task_input_delivered(&normal);
    for(unsigned i=0;i<19;i++)run_tick(0);
    CHECK(normal.input_priority==11);run_tick(0);CHECK(!normal.input_priority);
    priority_reset();task_input_delivered(&ui);
    CHECK(ui.focused && ui.input_priority==11); /* shell/internal window */
    puts("f0-45 input: focus base 9, +2 refresh/20ms expiry/focus loss; I/O +1/10 running ticks/no polling PASS");
}
static void input_forwarding(void)
{
    priority_reset(); task_focus(&ui);
    task_input_forward(&middle,&normal); CHECK(!normal.input_priority);
    task_input_forward(&ui,&normal);
    CHECK(normal.base_priority==7 && normal.input_priority==9 && normal.input_expiry==20);
    for (unsigned i=0;i<19;i++) run_tick(0);
    CHECK(normal.input_forwarded); run_tick(0);
    CHECK(!normal.input_priority && !normal.input_forwarded && !input_proxy);
    task_input_forward(&ui,&normal); task_focus(&middle);
    CHECK(!normal.input_priority && !normal.input_expiry && !input_proxy);
    task_input_forward(&middle,&normal); task_input_withdraw(&normal);
    CHECK(normal.base_priority==7 && !normal.input_priority && !input_proxy);
    puts("F1-87 D forwarded input: focused source only, base7 unchanged, 20ms expiry and native/DOS focus withdrawal PASS");
}
static struct kwait completion;
static bool completed;
static bool completion_ready(void *arg) { (void)arg;return completed; }
static void complete_hook(void)
{
    CHECK(g_current==&idle);
    kwait_wake_io(&completion);CHECK(normal.state==T_BLOCKED && !normal.io_priority);
    completed=true;kwait_wake_io(&completion);
    CHECK(normal.state==T_READY && normal.io_priority==8);
    kwait_wake_io(&completion);CHECK(scheduler_stats.io_granted==1);
    latency_switch_hook=0;schedule();CHECK(g_current==&normal);
}
static void completion_test(void)
{
    priority_reset();kwait_init(&completion);completed=false;running(&normal);
    latency_switch_hook=complete_hook;
    CHECK(kwait_wait_until(&completion,completion_ready,0,10));
    CHECK(!waiters && scheduler_stats.io_granted==1 && scheduler_stats.wakeups==1);
    puts("f0-45 completion: production wait, false IRQ predicate, one wake/boost, repeated wake ignored PASS");
}
static void inversion_hook(void)
{
    uint32_t switched_flags=flags;flags=0x200;
    if(stage==0) {
        CHECK(g_current==&normal && normal.effective_priority==inversion_priority);
        stage=1;
        CHECK(kmutex_lock_until(&locks[1],100));
        CHECK(stage==2 && g_current==&normal);
        kmutex_unlock(&locks[1]);kmutex_unlock(&locks[0]);
        CHECK(normal.effective_priority==7);
        stage=3;park();CHECK(g_current==&hog);
    } else if(stage==1) {
        CHECK(g_current==&middle && middle.effective_priority==inversion_priority);
        CHECK(ui.effective_priority==9 && ui.state==T_READY);
        if(inversion_priority==SCHED_DEVICE)cycles+=500;
        stage=2;kmutex_unlock(&locks[1]);CHECK(middle.effective_priority==7);park();
        CHECK(g_current==&normal);
    }
    flags=switched_flags;
}
static void inversion_test(void)
{
    for(unsigned service=0;service<2;service++) {
    priority_reset();running(&normal);kmutex_lock(&locks[0]);normal.state=T_BLOCKED;
    running(&middle);kmutex_lock(&locks[1]);middle.state=T_BLOCKED;
    task_start(&normal);task_start(&middle);task_start(&ui);
    inversion_priority=service ? SCHED_DEVICE : SCHED_HIGH;
    task_set_class(&hog,service ? P_DEVICE : P_HIGH);running(&hog);stage=0;latency_switch_hook=inversion_hook;
    CHECK(kmutex_lock_until(&locks[0],100));
    CHECK(stage==3 && g_current==&hog && !waiters);
    kmutex_unlock(&locks[0]);CHECK(!held);
    if(service)CHECK(device_cpu_total>=500); /* debit inherited service before unlock */
    }
    puts("f0-45 inversion: high -> low -> middle inherit 13/24, competitor 9 bypassed, unlock withdrawal/service debit PASS");
}
static void withdrawal_hook(void)
{
    uint32_t switched_flags=flags;flags=0x200;
    CHECK(g_current==&normal && normal.effective_priority==13);
    if(scenario==1) {
        ksync_task_exit(&hog);hog.state=T_ZOMBIE;
        CHECK(normal.effective_priority==7 && !waiters && !locks[0].wait.head);
        latency_switch_hook=0;longjmp(cancelled,1);
    }
    if(scenario==2) {
        ksync_task_exit(&normal);normal.state=T_ZOMBIE;
        CHECK(!normal.preempt_holds && !locks[0].owner);
    } else {
        for(unsigned i=0;i<3;i++)run_tick(0);
        CHECK(hog.state==T_READY && normal.effective_priority==7 && !hog.waiting_on);
    }
    latency_switch_hook=0;schedule();CHECK(g_current==&hog);
    flags=switched_flags;
}
static void withdrawal_tests(void)
{
    for(scenario=0;scenario<3;scenario++) {
        priority_reset();running(&normal);kmutex_lock(&locks[0]);normal.state=T_BLOCKED;
        task_start(&normal);task_set_class(&hog,P_HIGH);running(&hog);
        latency_switch_hook=withdrawal_hook;
        if(setjmp(cancelled)) { CHECK(scenario==1);continue; }
        bool acquired=kmutex_lock_until(&locks[0],3);
        CHECK(acquired==(scenario==2) && !waiters);
        if(acquired)kmutex_unlock(&locks[0]);
        else {running(&normal);kmutex_unlock(&locks[0]);}
    }
    puts("f0-45 donation: timeout withdraws before waiter resumes, cancellation unlinks stack, owner death wakes/retires locks PASS");
}
static void cycle_hook(void)
{
    uint32_t switched_flags=flags;flags=0x200;
    if(stage==0) {
        CHECK(g_current==&normal);stage=1;
        CHECK(!kmutex_lock_until(&locks[0],2));
        CHECK(cycle_records==1 && !normal.waiting_on);
        kmutex_unlock(&locks[1]);stage=2;park();
    } else if(stage==1) {
        CHECK(g_current==&idle);stage=10;run_tick(0);run_tick(0);schedule();
    }
    flags=switched_flags;
}
static void cycle_test(void)
{
    priority_reset();task_set_class(&hog,P_HIGH);
    running(&hog);kmutex_lock(&locks[0]);hog.state=T_BLOCKED;
    running(&normal);kmutex_lock(&locks[1]);normal.state=T_BLOCKED;task_start(&normal);
    running(&hog);stage=0;latency_switch_hook=cycle_hook;
    CHECK(kmutex_lock_until(&locks[1],10));
    CHECK(stage==2 && cycle_records==1 && !waiters);
    kmutex_unlock(&locks[1]);kmutex_unlock(&locks[0]);
    puts("f0-45 cycle: protected record, no cyclic donation, timed wait progresses and recovers PASS");
}
static fs_lock fifo;
static void fifo_hook(void)
{
    if(stage) return;
    uint32_t saved=flags;flags=0x200;stage=1;
    CHECK(g_current==&normal && normal.effective_priority==13 && fifo.wait.owner==&normal);
    CHECK(fifo.serving==0 && fifo.next==2);
    if(scenario) {ksync_task_exit(&normal);normal.state=T_ZOMBIE;}
    else fs_lock_drop(&fifo);
    CHECK(normal.effective_priority==7 && fifo.serving==1 && !fifo.held);
    CHECK(!fs_lock_try_take(&fifo)); /* an unserved ticket cannot be overtaken */
    if(!scenario)normal.state=T_BLOCKED;
    schedule();CHECK(g_current==&hog);flags=saved;
}
static void fifo_test(void)
{
    for(scenario=0;scenario<2;scenario++) {
        priority_reset();fs_lock_init(&fifo);running(&normal);fs_lock_take(&fifo);
        normal.state=T_BLOCKED;task_start(&normal);task_start(&ui);
        task_set_class(&hog,P_HIGH);running(&hog);stage=0;latency_switch_hook=fifo_hook;
        fs_lock_take(&fifo);
        CHECK(fifo.serving==1 && fifo.next==2 && fifo.wait.owner==&hog && !waiters);
        fs_lock_drop(&fifo);CHECK(fifo.serving==2 && !owned_queues);
    }
    puts("f0-45 FIFO fs_lock: holder inherits 13, ticket order/try-lock retained, unlock and owner-death withdrawal PASS");
}
static uint32_t ticket_base;
static void fifo_cancel_hook(void)
{
    uint32_t saved=flags;flags=0x200;
    if(stage==0) {
        CHECK(g_current==&normal);stage=1;
        normal.state=T_READY;rq_push(&normal);running(&middle);
        fs_lock_take(&fifo);
        CHECK(stage==2 && g_current==&middle && fifo.serving==ticket_base+1u);
        fs_lock_drop(&fifo);latency_switch_hook=0;longjmp(cancelled,1);
    } else if(stage==1) {
        CHECK(g_current==&normal && fifo.next==ticket_base+3u);
        ksync_task_exit(&hog);hog.state=T_ZOMBIE;
        CHECK(fifo.next==ticket_base+2u && normal.effective_priority==7);
        CHECK(fifo.wait.head && *fifo.wait.head->ticket==ticket_base+1u);
        fs_lock_drop(&fifo);CHECK(!fs_lock_try_take(&fifo));stage=2;park();
    }
    flags=saved;
}
static void fifo_cancellation(void)
{
    for(unsigned wrap=0;wrap<2;wrap++) {
        priority_reset();fs_lock_init(&fifo);ticket_base=wrap ? UINT32_MAX : 0;
        fifo.next=fifo.serving=ticket_base;
        running(&normal);fs_lock_take(&fifo);normal.state=T_BLOCKED;task_start(&normal);
        task_set_class(&hog,P_HIGH);running(&hog);stage=0;latency_switch_hook=fifo_cancel_hook;
        if(!setjmp(cancelled)) {fs_lock_take(&fifo);CHECK(false);}
        CHECK(stage==2 && fifo.serving==ticket_base+2u && fifo.next==fifo.serving);
        CHECK(!held && !waiters && !owned_queues);
    }
    puts("f0-45 FIFO cancellation: middle ticket removed, later real waiter retains order/progress, uint32 wrap PASS");
}
static void combined_load(void)
{
    priority_reset(); task_focus(&ui);audio_focus=&normal;
    task_start(&normal);task_start(&ui);task_start(&middle);task_start(&hog);schedule();
    unsigned max_input=0, max_device=0, ui_runs=0, services=0, vm_ticks=0, ordinary=0;
    uint64_t delivered=0,last_service=0;
    bool pending=false;
    for(unsigned tick=0;tick<1000;tick++) {
        if(tick%17==0) {delivered=g_ticks;pending=true;task_input_delivered(&ui);}
        if(g_current==&ui && pending) {
            unsigned delay=(unsigned)(g_ticks-delivered);if(delay>max_input)max_input=delay;
            pending=false;ui_runs++;
        }
        bool service=g_current==&hog;
        if(service) {
            if(services && g_ticks-last_service>max_device)max_device=g_ticks-last_service;
            last_service=g_ticks;services++;
        } else { ordinary++; if(g_current==&normal)vm_ticks++; }
        run_tick(0);
        if(service) { task_device_deadline(&hog,g_ticks+3);task_sleep_ms(2); }
        else if(g_current==&ui)task_sleep_ms(1);
        else kpreempt_point();
        CHECK(device_used<=4); /* admitted load, no urgent exception */
        CHECK(device_cpu_total<=4*g_tsc_per_ms);
    }
    CHECK(max_input<=10 && middle.max_wait_ticks<=100 && max_device<=4);
    CHECK(ms(stretch_max)<=2);
    CHECK(vm_ticks*10<=ordinary*8+8 && ui_runs>40 && middle.dispatches);
    CHECK(scheduler_stats.starvation_granted==scheduler_stats.starvation_expired && !middle.boosted);
    printf("f0-45 combined: input_max=%ums normal_wait=%llums audio_service_gap=%ums stretch_max=%ums device_max=4/10 vm=%u/%u ui_responses=%u dispatches=%u/%u/%u/%u wakeups=%u urgent=%u PASS\n",
        max_input,(unsigned long long)middle.max_wait_ticks,max_device,ms(stretch_max),vm_ticks,ordinary,ui_runs,
        normal.dispatches,ui.dispatches,middle.dispatches,hog.dispatches,scheduler_stats.wakeups,scheduler_stats.audio_urgent);
}
static void saturation_test(void)
{
    priority_reset();task_start(&normal);task_start(&hog);schedule();
    for(unsigned i=0;i<4;i++)run_tick(0);
    CHECK(hog.device_demoted && device_used==4);
    task_device_deadline(&hog,g_ticks);schedule();
    CHECK(g_current==&hog && scheduler_stats.audio_urgent==1);
    run_tick(0);kpreempt_point();CHECK(g_current==&normal);
    priority_reset();task_start(&normal);task_start(&hog);schedule();
    for(unsigned i=0;i<8;i++) { cycles+=500;schedule(); }
    CHECK(g_current==&normal && hog.device_demoted && !device_used);
    CHECK(device_cpu_total==4000 && scheduler_stats.budget_exhausted);
    puts("f0-45 overload: aggregate exhaustion requeues ordinary work, urgent audio dispatch served/reported (outside admission) PASS");
    puts("f0-45 short device turns: 8 x 500us without a PIT tick charge 4ms and throttle at handoff PASS");
}
/* f1-98 (R2): reservation dependencies of polling waiters (ksync_depend):
 * the holder of a drive reservation or the cache lock runs at least at the
 * highest waiter's priority, transitively, recorded, withdrawn at release,
 * at the waiter's withdrawal and at either task's exit. */
static char drive_c, cache_object, other_object;
static void reservation_inheritance_test(void)
{
    priority_reset(); task_start(&normal); task_start(&middle);
    running(&ui); CHECK(ui.effective_priority==9 && normal.effective_priority==7);
    ksync_depend(&normal,&drive_c,KSYNC_DEPEND_DRIVE);
    CHECK(ui.depends_on==&normal && normal.inherited_priority==9 && normal.effective_priority==9);
    CHECK(inherit_used==1 && inherit_events[0].from_id==ui.id && inherit_events[0].to_id==normal.id);
    CHECK(inherit_events[0].level==9 && inherit_events[0].reason==KSYNC_DEPEND_DRIVE && inherit_events[0].count==1);
    ksync_depend(&normal,&drive_c,KSYNC_DEPEND_DRIVE); /* the next poll changes nothing */
    CHECK(inherit_events[0].count==1 && dependents==&ui && !ui.depend_next);
    ui.state=T_BLOCKED; /* asleep in its request pause: still lending */
    running(&normal); ksync_depend_release(&other_object); CHECK(normal.effective_priority==9);
    ksync_depend_release(&drive_c);
    CHECK(normal.effective_priority==7 && !normal.inherited_priority && !ui.depends_on && !dependents);
    cycles+=1000; record_bytes=0; records[0]=0; g_boot.flags=CBI_F_VERBOSE; cpu_emit(); g_boot.flags=0;
    CHECK(strstr(records,"[sched] inherit from=desktop#4 to=worker#2 level=9 reason=drive count=1\n"));

    /* Transitive: desktop(9) -> middle (drive) -> worker (cache); the
     * waiter's input boost propagates and expires along the chain. */
    priority_reset(); task_start(&normal);
    running(&middle); ksync_depend(&normal,&cache_object,KSYNC_DEPEND_CACHE);
    CHECK(middle.depends_on==&normal && normal.effective_priority==7 && !inherit_used); /* no raise, no record */
    middle.state=T_BLOCKED;
    running(&ui); ksync_depend(&middle,&drive_c,KSYNC_DEPEND_DRIVE); ui.state=T_BLOCKED;
    CHECK(middle.effective_priority==9 && normal.effective_priority==9 && inherit_used==2);
    CHECK(inherit_events[1].from_id==middle.id && inherit_events[1].to_id==normal.id &&
          inherit_events[1].reason==KSYNC_DEPEND_CACHE && inherit_events[1].level==9);
    task_input_delivered(&ui);
    CHECK(ui.effective_priority==11 && middle.effective_priority==11 && normal.effective_priority==11);
    for (unsigned i=0;i<20;i++) run_tick(0);
    CHECK(ui.effective_priority==9 && middle.effective_priority==9 && normal.effective_priority==9);
    /* A dependency closing a cycle lends nothing. */
    running(&normal); ksync_depend(&ui,&other_object,KSYNC_DEPEND_DRIVE);
    CHECK(!normal.depends_on && normal.effective_priority==9 && ui.effective_priority==9);
    /* The waiter's withdrawal (admitted, cancelled, timed out). */
    running(&ui); ksync_depend(0,0,0);
    CHECK(!ui.depends_on && middle.effective_priority==7 && normal.effective_priority==7);
    /* Exits: a dying holder drops its waiters' edges, a dying waiter its own. */
    ksync_depend(&middle,&drive_c,KSYNC_DEPEND_DRIVE); CHECK(normal.effective_priority==9);
    ksync_task_exit(&normal); CHECK(!middle.depends_on && middle.effective_priority==9);
    ksync_task_exit(&ui); CHECK(!dependents && middle.effective_priority==7 && !middle.inherited_priority);

    /* Share lending: a desktop member waiting for a non-member holder lends
     * it the group's share, only for the dependency. */
    priority_reset(); task_start(&normal); task_set_share_group(&ui,true);
    running(&ui); ksync_depend(&normal,&drive_c,KSYNC_DEPEND_DRIVE);
    CHECK(normal.share_lent && share_ready==1);
    ui.state=T_BLOCKED; running(&normal); ksync_depend_release(&drive_c);
    CHECK(!normal.share_lent && !share_ready);
    task_set_share_group(&ui,false);

    /* kmutex lock-order panics keep the kwait-only graph. */
    CHECK(!lock_cycle(&ui,&normal));
    puts("F1-98 reservation inheritance: holder raised to the waiter (9), transitive, input boost +2 and expiry propagate, cycle refused, release/withdrawal/exit restore 7, [sched] inherit record, share lent only while waited for PASS");
}
/* A priority-9 waiter for drive C beside a busy focused VM at 9: the 7
 * holder that still has 5 ms of work inside its hold. */
static unsigned holder_dispatch(bool edge)
{
    priority_reset();
    task_set_base(&middle,P_NORMAL,9); audio_focus=&middle; task_start(&middle);
    running(&ui); if (edge) ksync_depend(&normal,&drive_c,KSYNC_DEPEND_DRIVE);
    ui.state=T_BLOCKED; task_start(&normal); schedule();
    unsigned work=5; uint64_t start=g_ticks;
    while (work && g_ticks-start<2000) {
        if (g_current==&normal) work--;
        run_tick(0); kpreempt_point();
    }
    CHECK(!work);
    unsigned elapsed=(unsigned)(g_ticks-start);
    struct task *was=g_current; running(&normal); ksync_depend_release(&drive_c);
    CHECK(normal.effective_priority==7 && !dependents);
    (void)was; audio_focus=0;
    return elapsed;
}
static void holder_dispatch_test(void)
{
    unsigned without=holder_dispatch(false), with=holder_dispatch(true);
    printf("F1-98 drive hold beside a busy focused VM=9, waiter at 9: holder's 5 ms finish in %u ms without inheritance (starvation boosts), %u ms with PASS\n",without,with);
    CHECK(without>=300 && with<=40);
}
#include "../../src/kernel/proc/priority.c"
static void mappings(void)
{
    CHECK(proc_priority_posix(0,0,0)==7 && proc_priority_posix(0,0,-20)==13 && proc_priority_posix(0,0,19)==1);
    CHECK(proc_priority_posix(1,0,0)==-EPERM && proc_priority_posix(0,0,20)==-EINVAL);
    CHECK(proc_priority_win32(0x20,0,false)==7 && proc_priority_win32(0x20,0,true)==9);
    CHECK(proc_priority_win32(0x80,2,false)==15 && proc_priority_win32(0x40,-15,false)==1);
    CHECK(proc_priority_win32(0x100,0,false)==-EPERM && proc_priority_win32(0x20,3,false)==-EINVAL);
}
static uint64_t bench_cycles(void)
{
    unsigned lo,hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo),"=d"(hi) :: "memory");
    return ((uint64_t)hi<<32)|lo;
}
static int benchmark(void)
{
    reset();rq_push(&normal);rq_push(&ui);schedule();
    const unsigned iterations=1000000;
    uint64_t start=bench_cycles();
    for(unsigned i=0;i<iterations;i++)schedule();
    uint64_t schedule_cost=bench_cycles()-start;
    start=bench_cycles();
    for(unsigned i=0;i<iterations;i++){g_ticks++;sched_tick();}
    uint64_t tick_cost=bench_cycles()-start;
    printf("schedule_cycles=%.2f tick_cycles=%.2f iterations=%u tasks=4\n",
        (double)schedule_cost/iterations,(double)tick_cost/iterations,iterations);
    return 0;
}
#include "proc/dosvm_wait_test.inc"
int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"--benchmark"))return benchmark();
    mappings();
    input_and_io();completion_test();inversion_test();withdrawal_tests();cycle_test();fifo_test();fifo_cancellation();combined_load();saturation_test();
    input_forwarding();dosvm_wait_dependency_test();dosvm_wait_combined_load();
    reservation_inheritance_test();holder_dispatch_test();
    return 0;
}
