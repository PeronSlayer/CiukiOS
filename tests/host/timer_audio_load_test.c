/* Production scheduler, sync, ordered timers, deferred worker and cpustat.
 * Deterministic admitted load; audio CPU cost modeled at 500us/refill,
 * device callbacks at 80us and PIT at 90us. Real mixer/transport arithmetic
 * is independently replayed by audio/run.sh. No physical latency claim. */
#define HOST_SYNC_TIMERS
#define HOST_REAL_WORK
#define main latency_fixture_main
#include "kernel_latency_test.c"
#undef main
#define ktime_cycles sync_unused_cycles
#include "../../src/kernel/core/sync.c"
#undef ktime_cycles
static void timer_fixture_reset(void)
{
    timer_root=timer_first=timer_due_head=timer_due_tail=0;
    memset(&timer_stats,0,sizeof(timer_stats));deferred_fn=0;(void)deferred_arg;
}
bool g_cpu_tsc=true;
void klog(const char *fmt,...) { (void)fmt; }
void task_yield(void) { schedule(); }
#include "../../src/kernel/core/work.c"
void dosvm_tick(void) { }
void w32_shared_tick(void) __attribute__((weak));
void w32_shared_tick(void) { }
void serial_tick(void) __attribute__((weak));
void serial_tick(void) { }
void timing_timer_irq(void) { }
#include "pit_timer.inc"
static struct ktimer load_timers[1000];
static uint32_t load_generation=1;
static uint64_t next_irq, audio_due, audio_max_wake, audio_max_service;
static unsigned audio_services, vm_turns, ui_turns, timer_callbacks;
static bool pumping;
static void cpu_work(unsigned us)
{
    uint64_t end=cycles+us;
    while(next_irq<=end) {
        cycles=next_irq;next_irq+=1000;
        uint32_t f=flags;flags=0;
        struct trap_frame tf={.vector=0x20,.cs=8};
        uint64_t irq=cpu_irq_enter(&tf);timer_irq(&tf);
        cycles+=90;end+=90;cpu_irq_leave(irq);flags=f;
    }
    cycles=end;
}
static void deadline_work(void *owner,uint32_t generation,unsigned kind)
{
    (void)owner;CHECK(generation==load_generation && kind==9);
    timer_callbacks++;cpu_work(80);
}
static void peer_step(void)
{
    if(g_current==&hog) {
        uint64_t latency=cycles-audio_due;
        if(audio_services && latency>audio_max_wake)audio_max_wake=latency;
        uint64_t start=cpu_path_clock();
        cpu_work(500);
        uint64_t service=cpu_path_clock()-start;
        if(service>audio_max_service)audio_max_service=service;
        audio_services++;
        uint64_t due=g_ticks+5;audio_due=next_irq+(due-g_ticks-1)*1000;
        task_device_deadline(&hog,due);task_sleep_ms(5);
    } else if(g_current==&ui) {
        ui_turns++;cpu_work(250);task_sleep_ms(2);
    } else {
        CHECK(g_current==&normal || g_current==&idle);
        vm_turns+=g_current==&normal;cpu_work(250);kpreempt_point();
    }
}
static void peers_until_worker(void)
{
    if(pumping)return;
    uint32_t saved=flags; flags=0x200; pumping=true;
    while(g_current!=&reporter) {CHECK(g_ticks<1000);peer_step();}
    pumping=false;flags=saved;
}
static void empty_work(void *arg) { (void)arg; }
static void pit_full_ring(void)
{
    reset();reporter=(struct task){.id=5,.generation=1,.prio=P_DEVICE,.state=T_BLOCKED};
    CHECK(!kwork_init());
    for(unsigned i=0;i<1000;i++)ktimer_arm(&load_timers[i],0,9,0,&load_generation,deadline_work);
    for(unsigned i=0;i<KWORK_CAPACITY-2;i++)CHECK(kwork_queue(empty_work,(void *)(uintptr_t)i));
    CHECK(kwork_queue(shared_tick_work,0));CHECK(kwork_queue(empty_work,(void *)(uintptr_t)KWORK_CAPACITY));
    struct trap_frame tf={.vector=0x20};flags=0;timer_irq(&tf);flags=0x200;
    CHECK(timer_stats.due==KTIMER_IRQ_BATCH && stats.pending==KWORK_CAPACITY && stats.lost==1);
    for(unsigned i=0;i<1000;i++)CHECK(ktimer_cancel(&load_timers[i]));
    rq_remove(&reporter);g_current=&reporter;reporter.state=T_RUNNING;
    while(kwork_run_one());
    CHECK(!stats.pending && !timer_stats.due && !timer_stats.armed);
    puts("f0-46 PIT: 1000 due timers/full 64-entry ring, late coalesced tick notifications, bounded transfer, counted overflow and cancelled due entries PASS");
    worker=0;head=tail=0;memset(&stats,0,sizeof(stats));reported_lost=0;
}
int main(void)
{
    pit_full_ring();
    reset();next_irq=1001;
    normal.state=hog.state=ui.state=T_BLOCKED;
    reporter=(struct task){.id=5,.generation=1,.prio=P_DEVICE,.state=T_BLOCKED};
    strcpy(reporter.name,"device-work");ui.all_next=&reporter;
    CHECK(!kwork_init());
    audio_focus=&normal;task_start(&normal);task_start(&ui);task_start(&hog);
    for(unsigned i=0;i<1000;i++)
        ktimer_arm(&load_timers[i],1+i*4,9,0,&load_generation,deadline_work);
    latency_switch_hook=peers_until_worker;schedule();
    while(g_ticks<250) {
        CHECK(g_current==&reporter);
        if(!kwork_run_one())kwait_wait_until(&available,has_work,0,deadline_after_ms(1000));
    }
    latency_switch_hook=0;
    CHECK(audio_services>30 && timer_callbacks>50 && vm_turns && ui_turns);
    CHECK(audio_max_wake<=2000 && audio_max_service<=2000);
    CHECK(stats.max_run_us<=1000 && stats.max_service_us<=2000);
    CHECK(!stats.budget_violations && !stats.service_violations && !stats.lost);
    for(unsigned i=0;i<1000;i++)CHECK(ktimer_cancel(&load_timers[i]));
    printf("f0-46 combined timers=1000 callbacks=%u refill_services=%u wake_to_execution_max_us=%llu refill_cpu_max_us=%llu deferred_turn_max_us=%llu service_max_us=%llu vm_turns=%u ui_turns=%u PASS\n",
        timer_callbacks,audio_services,(unsigned long long)audio_max_wake,
        (unsigned long long)audio_max_service,(unsigned long long)stats.max_run_us,
        (unsigned long long)stats.max_service_us,vm_turns,ui_turns);
    return 0;
}
