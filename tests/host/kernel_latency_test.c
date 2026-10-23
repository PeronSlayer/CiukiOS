/* Production scheduler and CPU sampler; only CPU time/stack switching faked.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define CIUKI_CPU_H
#include <ciuki/kernel.h>
#include <ciuki/task.h>
#include <ciuki/sync.h>
#include <ciuki/cpustat.h>
#include <ciuki/timing.h>
#include <ciuki/bootlog.h>
#include <ciuki/work.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"latency:%d: %s\n",__LINE__,#c); exit(1); } } while (0)
struct ciuki_boot_info g_boot;
struct task *g_current;
volatile uint64_t g_ticks;
uint64_t g_tsc_per_ms = 1000, g_task_switches, g_task_switches_other, g_switch_target;
uint32_t g_quantum_ticks = 10, g_starvation_boosts;
volatile bool g_need_resched, g_measure_stop;
static uint64_t cycles = 1;
static uint64_t read_tsc_unserialized(void) { return cycles; }
static uint32_t fpu_cost;
static uint32_t flags = 0x200;
static void (*timing_begin_hook)(void),(*timing_end_hook)(void);
bool crit_begin_inactive(void) __attribute__((weak));
static uint32_t irq_save(void) { uint32_t f=flags; flags=0; if ((f&0x200) && timing_begin_hook) timing_begin_hook(); return f; }
static void irq_restore(uint32_t f) { if ((f&0x200) && timing_end_hook) timing_end_hook(); flags=f; }
static uint32_t read_eflags(void) { return flags; }
uint64_t ktime_cycles(void) { return cycles; }
void file_clock_tick(void) { }
#define biosvm_task_cr3(t,c) ((void)(t),(c))
#define biosvm_task_esp0(t,s) ((void)(t),0u)
#define write_cr3(c) ((void)(c))
#define vmm_kernel_pd() 0u
#define tss_set_kernel_stack(s) ((void)(s))
#define proc_task_switch(t) ((void)(t))
#define fpu_task_switch(t) ((void)(t),cycles+=fpu_cost)
#define crit_end() do { if (timing_end_hook) timing_end_hook(); } while (0)
#define crit_begin() do { if (timing_begin_hook) timing_begin_hook(); } while (0)
static void (*latency_switch_hook)(void);
#define switch_context(a,b) ((void)(a),(void)(b),latency_switch_hook ? latency_switch_hook() : (void)0)
extern uint32_t dosvm_task_cr3(const struct task *,uint32_t) __attribute__((weak));
extern uint32_t dosvm_task_esp0(const struct task *,uint32_t) __attribute__((weak));
extern void dosvm_task_switch(const struct task *) __attribute__((weak));
extern void dosvm_tick(void) __attribute__((weak));
static void timer_fixture_reset(void);
static kwork_fn deferred_fn;
static void *deferred_arg;
#ifndef HOST_REAL_WORK
bool kwork_queue(kwork_fn fn, void *arg) { deferred_fn=fn; deferred_arg=arg; return true; }
#endif
#ifndef HOST_SYNC_TIMERS
uint64_t deadline_after_ms(uint32_t ms) { return g_ticks+ms; }
gen_t gen_alloc(void) { static gen_t generation; return ++generation; }
#include "deadline_timers.inc"
static void timer_fixture_reset(void)
{
    timer_root=timer_first=timer_due_head=timer_due_tail=0;
    memset(&timer_stats,0,sizeof(timer_stats));deferred_fn=0;
}
#endif
#include "latency_scheduler.inc"
static struct task idle, normal, hog, ui, reporter;
static const struct task *audio_focus;
static const struct task *irq_focus;
const struct task *dosvm_task_focus(void) { return audio_focus; }
const struct task *dosvm_task_irq(void) { return irq_focus; }
uint32_t dosvm_task_quantum(const struct task *t,uint32_t quantum)
{ return t==audio_focus && quantum>2 ? 2 : quantum; }
static char records[4096];
static unsigned record_bytes;
void klog_periodic(const char *fmt,...)
{
    va_list ap; va_start(ap,fmt);
    int n=vsnprintf(records+record_bytes,sizeof(records)-record_bytes,fmt,ap); va_end(ap);
    CHECK(n>0 && n<=240); record_bytes+=(unsigned)n;
    CHECK(record_bytes+2<sizeof(records)); records[record_bytes++]='\n';records[record_bytes]=0;
}
static struct timing_stats timing_fixture;
void timing_snapshot(struct timing_stats *s) { *s=timing_fixture; }
void bootlog_snapshot(struct bootlog_stats *s) { memset(s,0,sizeof(*s)); }
#ifndef HOST_REAL_WORK
void kwork_snapshot(struct kwork_stats *s) { memset(s,0,sizeof(*s)); }
#endif
uint32_t serial_dropped(void) { return 0; }
struct task *task_create_kernel(const char *n,void (*fn)(void *),void *arg,enum task_prio p)
{ (void)n;(void)fn;(void)arg;(void)p; return &reporter; }
__attribute__((noreturn)) void panic(const char *fmt,...)
{ fprintf(stderr,"panic: %s\n",fmt); exit(1); }
#include "../../src/kernel/core/cpustat.c"
#ifndef HOST_SYNC_TIMERS /* harnesses with the real sync.c report its counters */
/* fx-5 follow-up: the sync.c counters of continued lock-order reports. */
static struct ksync_order_stats order_fixture;
void ksync_order_snapshot(struct ksync_order_stats *s) { *s=order_fixture; }
#endif
#include "vm_creation_model.inc"
static void reset(void)
{
    timer_fixture_reset();
    (void)next_id;
    latency_switch_hook=0;
    memset(rq_head,0,sizeof(rq_head));memset(rq_tail,0,sizeof(rq_tail));
    idle=(struct task){.id=1,.prio=P_IDLE,.state=T_RUNNING};strcpy(idle.name,"idle");
    normal=(struct task){.id=2,.prio=P_NORMAL,.state=T_READY,.user=true};strcpy(normal.name,"worker");
    hog=(struct task){.id=3,.prio=P_DEVICE,.state=T_READY};strcpy(hog.name,"hog");
    ui=(struct task){.id=4,.prio=P_INTERACTIVE,.state=T_READY,.user=true};strcpy(ui.name,"desktop");
    idle.all_next=&normal; normal.all_next=&hog; hog.all_next=&ui; all_tasks=&idle;
    idle.generation=normal.generation=hog.generation=ui.generation=1;
    g_current=idle_task=&idle;g_ticks=0;cycles=1;slice_left=10;flags=0x200;
    focus_task=audio_focus=irq_focus=0;ui_focus=input_proxy=0;focus_ticks=ordinary_ticks=0;device_used=device_history=0;memset(&scheduler_stats,0,sizeof(scheduler_stats));g_need_resched=false;g_starvation_boosts=0;
    memset(device_cpu,0,sizeof(device_cpu));device_cpu_total=device_cpu_tick=0;
    device_cpu_slot=0;device_cpu_scale=1;device_cpu_refresh=false;
    memset(device_consumers,0,sizeof(device_consumers));audio_over=0;device_seq++;device_attribution=true;
    /* f1-98 desktop share and inheritance records */
    share_ready=0;share_credit=0;share_tick=0;share_scale=1;share_turn=share_picked=false;desktop_share=true;
    memset(&share_stats,0,sizeof(share_stats));memset(&share_base,0,sizeof(share_base));
    inherit_used=inherit_lost=0;
    /* f1-101 audio producer boost and records */
    memset(producer_sources,0,sizeof(producer_sources));memset(producer_cpu,0,sizeof(producer_cpu));
    producer_cpu_total=producer_tick=0;producer_scale=1;producer_slot=0;producer_over=false;audio_producer=true;
    producer_used=producer_lost=0;
    last_measured=0;last_preempted=false;
    g_task_switches=g_task_switches_other=g_switch_target=0;g_measure_stop=false;
    memset(classes,0,sizeof(classes));memset(paths,0,sizeof(paths));
    irq_cycles=exited_cycles=stretch_max=resched_max=0;switches=overruns=0;
    overrun_used=overrun_lost=audio_used=audio_lost=0;
    forced=threshold=resched_pending=false;record_bytes=0;records[0]=0;g_boot.flags=0;
    memset(&timing_fixture,0,sizeof(timing_fixture));
    memset(&irqoff,0,sizeof(irqoff)); memset(&irqoff_window,0,sizeof(irqoff_window)); fpu_cost=0;
    irqoff_credited=irqoff_last_end=pit_last=pit_phase=0; firmware_window=firmware_max=0; firmware_count=0; /* f2-96 */
    cpu_switch(0,&idle);
}
static void run_tick(uint32_t eip)
{
    cycles+=1000;g_ticks++;
    struct trap_frame tf={.vector=0x20,.cs=g_current->user?0x1b:8,.eip=eip};
    uint64_t start=cpu_irq_enter(&tf);sched_tick();cpu_irq_leave(start);
#ifndef HOST_REAL_WORK
    uint32_t saved_flags=flags; flags=0x200;
    for(unsigned n=0;deferred_fn && n<1024;n++) { kwork_fn fn=deferred_fn;deferred_fn=0;fn(deferred_arg); }
    flags=saved_flags;
#endif
}
static void creation_cpu_service(uint64_t ns)
{
    cycles += ns;
    kpreempt_point();
}
static void vm_creation_test(void)
{
    struct creation_result result[2];
    for (unsigned points = 0; points < 2; points++) {
        reset(); normal.user = false; strcpy(normal.name, "dosbox");
        rq_push(&normal); schedule();
        g_tsc_per_ms = 1000000; /* nanoseconds, deterministic cost model */
        result[points] = creation_replay(points != 0, creation_cpu_service);
        cpu_point();
        char name[16]; uint32_t duration;
        cpu_stretch_snapshot(name, &duration);
        CHECK(!strcmp(name, "dosbox"));
        CHECK(stretch_max == result[points].maximum);
        if (points) CHECK(stretch_max <= 2000000 && duration <= 2);
        else CHECK(stretch_max >= 110000000 && duration >= 110);
    }
    CHECK(result[1].pages == result[0].pages && result[1].pages == 6144);
    CHECK(result[1].ns >= result[0].ns && result[1].ns <= result[0].ns * 110 / 100);
    CHECK(result[1].points == 384);
    printf("F0-41 creation replay: pages=%u total=%llu->%lluns max=%llu->%lluns, production dosbox stretch<=2ms PASS\n",
           result[1].pages, (unsigned long long)result[0].ns, (unsigned long long)result[1].ns,
           (unsigned long long)result[0].maximum, (unsigned long long)result[1].maximum);
    g_tsc_per_ms = 1000;
}
static void wake_test(void)
{
    reset();rq_push(&normal);schedule();CHECK(g_current==&normal);
    hog.state=T_BLOCKED;ktimer_wait_task(&hog,1);
    run_tick(0x1234);CHECK(g_need_resched && hog.state==T_READY);
    schedule();CHECK(g_current==&hog && normal.state==T_READY);
}
static void device_test(void)
{
    reset();rq_push(&normal);rq_push(&hog);schedule();CHECK(g_current==&hog);
    for(unsigned i=0;i<4;i++) {run_tick(0x8888);if(i==1) schedule();}
    CHECK(hog.device_demoted && hog.prio==P_DEVICE && scheduler_stats.budget_exhausted);
    kpreempt_hold();kpreempt_point();CHECK(g_current==&hog);
    kpreempt_release();flags=0;kpreempt_point();CHECK(g_current==&hog);flags=0x200;
    kpreempt_point();CHECK(g_current==&normal && g_ticks==4);
    reset();hog.prio=P_NORMAL;rq_push(&normal);rq_push(&hog);rq_push(&ui);
    schedule();CHECK(g_current==&ui);
}
/* The production scheduler runs a continuously READY focused VM beside a
 * refill worker. A ready-return loop is the negative control: it eventually
 * demotes refill. Blocking between services preserves P_DEVICE and cadence. */
static void fullscreen_refill_test(void)
{
    unsigned gaps[2]={0};
    for (unsigned blocking=0;blocking<2;blocking++) {
        reset();ui.state=T_BLOCKED;audio_focus=&normal;
        rq_push(&normal);rq_push(&hog);schedule();
        unsigned services=0;uint64_t last=g_ticks;
        while (g_ticks<200) {
            bool refill=g_current==&hog;
            if (refill) {
                if (services && g_ticks-last>gaps[blocking]) gaps[blocking]=(unsigned)(g_ticks-last);
                last=g_ticks;services++;
            }
            run_tick(0x1234);
            if (refill && blocking) task_sleep_ms(2);
            else if (g_need_resched || refill) schedule();
        }
        CHECK(services>20);
        if (blocking) CHECK(hog.prio==P_DEVICE);
        else CHECK(overruns);
    }
    CHECK(gaps[0]>2 && gaps[1]<=4);
    printf("f3-39 scheduler busy focus: ready-return refill gap=%ums, blocking refill gap=%ums, device guard retained PASS\n",gaps[0],gaps[1]);
    audio_focus=0;
}
static void measured_wake_test(void)
{
    reset();normal.measured=ui.measured=true;ui.prio=P_NORMAL;
    rq_push(&normal);rq_push(&ui);schedule();CHECK(g_current==&normal);
    hog.state=T_BLOCKED;ktimer_wait_task(&hog,1);g_switch_target=2;
    run_tick(0);schedule();CHECK(g_current==&hog && !g_task_switches);
    task_sleep_ms(1);CHECK(g_current==&ui && g_task_switches==1);
    run_tick(0);schedule();CHECK(g_current==&hog && g_task_switches==1);
    task_sleep_ms(1);CHECK(g_current==&normal && g_task_switches==2 && g_measure_stop);
    run_tick(0);schedule();task_sleep_ms(1);
    CHECK(normal.state==T_BLOCKED && ui.state==T_BLOCKED && !g_task_switches_other);
}
static void focused_irq_test(void)
{
    reset(); hog.prio = ui.prio = P_NORMAL;
    rq_push(&hog); rq_push(&ui); rq_push(&normal); schedule();
    CHECK(g_current == &hog && !g_need_resched);
    kpreempt_point(); CHECK(g_current == &hog); /* no pending IRQ */
    irq_focus = audio_focus = &normal;
    kpreempt_hold(); kpreempt_point(); CHECK(g_current == &hog);
    kpreempt_release(); flags = 0; kpreempt_point(); CHECK(g_current == &hog);
    flags = 0x200; kpreempt_point(); CHECK(g_current == &normal);
    for (unsigned i = 0; i < FOCUS_BUDGET; i++) run_tick(0);
    kpreempt_point(); CHECK(g_current != &normal && focus_ticks == FOCUS_BUDGET);
    struct task *peer = g_current;
    g_need_resched = false; kpreempt_point(); CHECK(g_current == peer);
    /* At the next period the deadline is eligible again, even with an
     * unexpired peer slice. A device worker still outranks that deadline. */
    run_tick(0); run_tick(0);
    kpreempt_point(); CHECK(g_current == &normal);
    task_set_class(&hog, P_DEVICE);
    g_need_resched = true; kpreempt_point(); CHECK(g_current == &hog);

    reset(); audio_focus = irq_focus = &normal;
    rq_push(&normal); rq_push(&ui); schedule();
    unsigned responses = 0, max_latency = 0;
    while (g_ticks < 240) {
        if (g_current == &ui) {
            unsigned latency = (unsigned)(g_ticks - ui.ready_since);
            if (latency > max_latency) max_latency = latency;
            responses++; task_sleep_ms(10);
        } else {
            run_tick(0); kpreempt_point();
        }
    }
    CHECK(responses == 24 && max_latency <= 8);
    printf("f3-41 scheduler: IRQ at next safe point, budget/device/IF/lock guards, desktop=%u/24 max_ready_ms=%u PASS\n",
           responses, max_latency);
}
static void accounting_test(void)
{
    reset();g_boot.flags=CBI_F_VERBOSE;
    cycles=100001;cpu_switch(&idle,&normal);g_current=&normal;
    /* User intervals and guest CS=0 are not ring-0 stretches. */
    struct trap_frame guest={.vector=0x20,.cs=0,.eflags=1u<<17};
    cpu_trap_enter(&guest);uint64_t start=cpu_irq_enter(&guest);cycles+=100;cpu_irq_leave(start);cpu_user_return();
    CHECK(normal.user_ticks==1 && !normal.stretch_start);
    cycles=700001;cpu_switch(&normal,&hog);g_current=&hog;
    hog.state=T_RUNNING;
    for(unsigned i=0;i<25;i++) {cycles+=1000;struct trap_frame tf={.vector=0x20,.cs=8,.eip=0xdeadbeef};start=cpu_irq_enter(&tf);cpu_irq_leave(start);}
    CHECK(threshold && stretch_eip==0xdeadbeef);
    cycles=2000001;cpu_switch(&hog,&normal);g_current=&normal;
    cpu_reap(&hog);normal.all_next=&ui; /* exited CPU stays in its class */
    timing_fixture=(struct timing_stats){.critical_us=2120,.critical_site=0xc011e445,
        .critical_section="serial_tx_byte",.budget_violations=1};
    cpu_emit();
    unsigned win,id,ir,de,in,no;
    CHECK(sscanf(strstr(records,"[cpu] window_ms="),"[cpu] window_ms=%u idle_pm=%u irq_pm=%u device_pm=%u interactive_pm=%u normal_pm=%u",&win,&id,&ir,&de,&in,&no)==6);
    CHECK(win==2000 && id+ir+de+in+no==1000 && id==50 && de==650 && no==300);
    CHECK(strstr(records,"eip=deadbeef") && strstr(records,"task=hog#3"));
    CHECK(!(g_boot.flags & CBI_F_DIAG_VERBOSE));
    CHECK(strstr(records,"[kernel] irqoff_section=serial_tx_byte site=c011e445 irqoff_max_us=2120 violations=1 work_us=0 firmware_us=0 unclassified=0 owner=none\n"));
    CHECK(strstr(records,"[kernel] long_paths") && strstr(records,"timer_gaps=0/0"));
    record_bytes=0;records[0]=0;g_boot.flags=0;cycles+=2000000;cpu_emit();CHECK(!record_bytes);
    /* A desktop stall wakes the reporter immediately, even in a quiet boot. */
    reporter=(struct task){.prio=P_DEVICE,.state=T_BLOCKED,.wake_tick=9999};stat_task=&reporter;
    cpu_stall_record("[desktop] stall stage=frame\n",27);CHECK(forced && reporter.state==T_READY && !reporter.wake_tick);
    cycles+=1000;cpu_emit();CHECK(strstr(records,"[cpu] window_ms=1"));
    g_need_resched=true;start=cpu_irq_enter(&guest);cpu_irq_leave(start);
    cycles+=21000;cpu_switch(&normal,&normal);
    CHECK(resched_max==21000 && threshold);
    record_bytes=0;records[0]=0;cpu_emit();CHECK(strstr(records,"resched_lat_max_ms=21"));
}
static void maximum_tasks_test(void)
{
    /* Native threads are capped at 256; include the harder 1360 kernel
     * stack-slot ceiling without changing the scheduler's tick policy. */
    static struct task peers[1360];
    reset(); all_tasks=&idle; idle.all_next=peers;
    for (unsigned i=0;i<1360;i++) {
        peers[i]=(struct task){.id=i+10,.prio=P_NORMAL,.state=T_BLOCKED,.generation=1,
            .all_next=i+1<1360 ? &peers[i+1] : 0};
        ktimer_wait_task(&peers[i],1);
    }
    /* F0-46: the timer IRQ moves at most KTIMER_IRQ_BATCH due entries with
     * interrupts disabled and the wakes run as bounded deferred work; the
     * F0-44 task population walk no longer exists (F0-44 bound by design). */
    unsigned ready=0, ticks=0;
    while (ready<1360 && ticks<8) {
        run_tick(0); ticks++; ready=0;
        for (unsigned i=0;i<1360;i++) ready+=peers[i].state==T_READY;
    }
    struct ktimer_stats stats; ktimer_snapshot(&stats);
    CHECK(stats.max_irq_batch>0 && stats.max_irq_batch<=KTIMER_IRQ_BATCH);
    CHECK(stats.completed==1360 && ready==1360 && g_need_resched);
    kpreempt_point(); CHECK(g_current==&peers[0]);
    printf("F0-44 maximum tasks: 1360 stack slots / 256 native cap, IRQ batch<=%u (f0-46 timer queue, %u tick(s)), wake and next-point dispatch PASS\n",
           (unsigned)KTIMER_IRQ_BATCH, ticks);
}
static void irqoff_phase_test(void)
{
    reset(); rq_push(&normal); fpu_cost=1600;
    cpu_irqoff_begin(cycles,0xc0123456); schedule(); cpu_irqoff_end(cycles);
    CHECK(irqoff_window.cycles==1600 && irqoff_window.phase==CPU_IRQOFF_FPU && !irqoff_window.gap);
    CHECK(!strcmp(irqoff_window.from,"idle") && !strcmp(irqoff_window.to,"worker"));
    cpu_emit();
    CHECK(strstr(records,"[kernel] irqoff_long site=c0123456 us=1600 phase=fpu phase_us=1600 from=idle to=worker\n"));
    record_bytes=0; records[0]=0; cycles+=2000; cpu_emit();
    CHECK(!strstr(records,"irqoff_long"));
    /* Sub-millisecond sections are ignored; only the maximum is retained. */
    cpu_irqoff_begin(cycles,1); cpu_irqoff_mark(CPU_IRQOFF_PICK); cycles+=999;
    cpu_irqoff_end(cycles); CHECK(!irqoff_window.cycles);
    cpu_irqoff_begin(cycles,1); cycles+=500; cpu_irqoff_mark(CPU_IRQOFF_PICK); cycles+=500;
    cpu_irqoff_end(cycles); CHECK(irqoff_window.cycles==1000 && !irqoff_window.gap);
    irqoff_window.cycles=0;
    for (unsigned i=0;i<3;i++) {
        cpu_irqoff_begin(cycles,2+i); cycles+=1200+i*100;
        cpu_irqoff_mark(CPU_IRQOFF_ADDRESS); cycles+=100;
        cpu_irqoff_mark(CPU_IRQOFF_NONE); cpu_irqoff_end(cycles);
    }
    CHECK(irqoff_window.cycles==1500 && irqoff_window.gap && irqoff_window.phase_cycles==1400);
    record_bytes=0; records[0]=0; cpu_emit();
    CHECK(strstr(records,"[kernel] irqoff_long site=00000004 us=1500 phase=gap phase_us=1400"));
    g_tsc_per_ms=0; cpu_irqoff_begin(cycles,5); cycles+=3000; cpu_irqoff_end(cycles);
    CHECK(!irqoff_window.cycles); g_tsc_per_ms=1000;
    puts("f1-86 IRQ-off: production schedule/FPU, exact 1ms boundary, gap, longest once per window, uncalibrated clock PASS");
}
/* f1-90: production per-consumer device budget accounting. One consumer
 * exhausts the shared budget beside a light device task and an audio-class
 * refill: only the consumer is demoted and named with its own consumption;
 * the old aggregate policy (negative control) demotes and names all three. */
static void attribution_test(void)
{
    static struct task light, refill;
    for (unsigned aggregate=0;aggregate<2;aggregate++) {
        reset(); device_attribution=!aggregate; g_boot.flags=CBI_F_VERBOSE;
        light=(struct task){.id=9,.generation=1,.prio=P_DEVICE,.state=T_BLOCKED}; strcpy(light.name,"light");
        refill=(struct task){.id=10,.generation=1,.prio=P_DEVICE,.state=T_BLOCKED}; strcpy(refill.name,"audio-refill");
        task_set_audio_class(&refill);
        ui.all_next=&light; light.all_next=&refill; hog.state=ui.state=T_BLOCKED;
        rq_push(&normal);
        task_start(&light); schedule(); CHECK(g_current==&light);
        cycles+=300; task_sleep_ms(50);
        task_start(&refill); schedule(); CHECK(g_current==&refill);
        cycles+=500; task_sleep_ms(50);
        CHECK(device_cpu_total==(aggregate ? 800u : 300u)); /* the refill's own budget */
        task_start(&hog); schedule(); CHECK(g_current==&hog);
        for (unsigned i=0;i<4;i++) run_tick(0x9090);
        CHECK(hog.device_demoted && device_throttled());
        schedule(); CHECK(g_current==&normal);
        CHECK(light.device_demoted==(bool)aggregate && refill.device_demoted==(bool)aggregate);
        unsigned named=0;
        for (unsigned i=0;i<overrun_used;i++) {
            const struct overrun_event *e=&overrun_events[i];
            named|=e->id==hog.id ? 1u : e->id==light.id ? 2u : e->id==refill.id ? 4u : 8u;
            CHECK(e->count==1 && e->consumed_us==(e->id==hog.id ? 4000u : e->id==light.id ? 300u : 500u));
        }
        CHECK(named==(aggregate ? 7u : 1u) && !audio_used);
        record_bytes=0; records[0]=0; cpu_emit();
        CHECK(strstr(records,"[sched] device_overrun task=hog#3 count=1 consumed_us=4000\n"));
        CHECK(!strstr(records,"device_overrun task=light") == !aggregate);
        CHECK(strstr(records,"[cpu] device_top task=hog#3 us=4000\n"));
    }
    /* Max-min fair share: the water level, not an equal split, decides. */
    struct task a={.id=11,.prio=P_DEVICE,.priority_valid=true,.effective_priority=SCHED_DEVICE},
                b={.id=12,.prio=P_DEVICE,.priority_valid=true,.effective_priority=SCHED_DEVICE},
                c={.id=13,.prio=P_DEVICE,.priority_valid=true,.effective_priority=SCHED_DEVICE};
    static const unsigned shares[2][3]={{300,1800,2000},{300,2000,2000}};
    for (unsigned k=0;k<2;k++) {
        reset(); device_cpu_scale=1000;
        a.device_slot=b.device_slot=c.device_slot=0;
        sched_charge_runtime(&a,shares[k][0],1000); sched_charge_runtime(&b,shares[k][1],1000);
        sched_charge_runtime(&c,shares[k][2],1000);
        CHECK(device_throttled() && !device_demote(&a) && device_demote(&c));
        CHECK(device_demote(&b)==(k==1)); /* 1800 < level 1900; 2000 >= level 1850 */
    }
    /* The refill's own budget: exhaustion demotes it alone, is recorded
     * apart from device_overrun, leaves the shared budget untouched and
     * recovers when its window drains. */
    reset(); refill=(struct task){.id=10,.generation=1,.prio=P_DEVICE,.state=T_BLOCKED};
    strcpy(refill.name,"audio-refill"); task_set_audio_class(&refill); ui.all_next=&refill;
    hog.state=ui.state=T_BLOCKED; rq_push(&normal);
    task_start(&refill); schedule(); CHECK(g_current==&refill);
    for (unsigned i=0;i<3;i++) run_tick(0x7070);
    CHECK(refill.device_demoted && !overrun_used && audio_used==1 && audio_events[0].id==refill.id);
    CHECK(audio_events[0].consumed_us>=AUDIO_CLASS_BUDGET_US && !device_cpu_total && !device_used && audio_over==1);
    record_bytes=0; records[0]=0; cpu_emit();
    CHECK(strstr(records,"[sched] audio_refill_overrun task=audio-refill#10 count=1 consumed_us=") && !strstr(records,"device_overrun"));
    task_sleep_ms(50); CHECK(g_current==&normal);
    for (unsigned i=0;i<10;i++) run_tick(0);
    CHECK(!audio_over && device_cpu_refresh); schedule(); CHECK(!refill.device_demoted);
    puts("f1-90 device budget: consumer alone demoted and named consumed_us, max-min share, refill own budget/record/recovery, aggregate negative control PASS");
}
#ifndef HOST_SYNC_TIMERS
/* fx-5 follow-up: a normal boot continues after a lock-order report; the
 * kernel statistics publish its counters in the window that added some,
 * even when quiet, and in every published window afterwards. */
static void lock_order_record_test(void)
{
    reset();
    record_bytes=0;records[0]=0;cycles+=2000000;cpu_emit();CHECK(!record_bytes);
    order_fixture=(struct ksync_order_stats){.violations=3,.pairs=2};
    record_bytes=0;records[0]=0;cycles+=2000000;cpu_emit();
    CHECK(!strcmp(records,"[kernel] lock_order violations=3 pairs=2 dropped=0\n"));
    record_bytes=0;records[0]=0;cycles+=2000000;cpu_emit();CHECK(!record_bytes); /* unchanged, quiet */
    g_boot.flags=CBI_F_VERBOSE;record_bytes=0;records[0]=0;cycles+=2000000;cpu_emit();
    CHECK(strstr(records,"[kernel] lock_order violations=3 pairs=2 dropped=0\n") && strstr(records,"[cpu] window_ms="));
    g_boot.flags=0;order_fixture=(struct ksync_order_stats){.violations=40,.pairs=16,.dropped=5};
    record_bytes=0;records[0]=0;cycles+=2000000;cpu_emit();
    CHECK(!strcmp(records,"[kernel] lock_order violations=40 pairs=16 dropped=5\n"));
    order_fixture=(struct ksync_order_stats){0};
    puts("fx-5 lock-order counters: published in the window that added reports (also quiet), then in every published window PASS");
}
#endif
int main(void)
{
    attribution_test();
    irqoff_phase_test();
    wake_test();device_test();fullscreen_refill_test();focused_irq_test();measured_wake_test();accounting_test();vm_creation_test();maximum_tasks_test();
#ifndef HOST_SYNC_TIMERS
    lock_order_record_test();
#endif
    puts("kernel latency: wake preemption, bounded device hog, lock/IF deferral, UI priority, CPU/exited shares, guest and stretch EIP, forced record PASS");
    return 0;
}
