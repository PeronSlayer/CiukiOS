/* Production scheduler and CPU sampler; only CPU time/stack switching faked.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define CIUKI_CPU_H
#include <ciuki/kernel.h>
#include <ciuki/task.h>
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
static uint32_t flags = 0x200;
static uint32_t irq_save(void) { uint32_t f=flags; flags=0; return f; }
static void irq_restore(uint32_t f) { flags=f; }
static uint32_t read_eflags(void) { return flags; }
uint64_t ktime_cycles(void) { return cycles; }
void file_clock_tick(void) { }
#define biosvm_task_cr3(t,c) ((void)(t),(c))
#define biosvm_task_esp0(t,s) ((void)(t),0u)
#define write_cr3(c) ((void)(c))
#define vmm_kernel_pd() 0u
#define tss_set_kernel_stack(s) ((void)(s))
#define proc_task_switch(t) ((void)(t))
#define fpu_task_switch(t) ((void)(t))
#define crit_end() ((void)0)
#define crit_begin() ((void)0)
#define switch_context(a,b) ((void)(a),(void)(b))
extern uint32_t dosvm_task_cr3(const struct task *,uint32_t) __attribute__((weak));
extern uint32_t dosvm_task_esp0(const struct task *,uint32_t) __attribute__((weak));
extern void dosvm_task_switch(const struct task *) __attribute__((weak));
extern void dosvm_tick(void) __attribute__((weak));
#include "latency_scheduler.inc"
static struct task idle, normal, hog, ui, reporter;
static const struct task *audio_focus;
const struct task *dosvm_task_focus(void) { return audio_focus; }
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
void kwork_snapshot(struct kwork_stats *s) { memset(s,0,sizeof(*s)); }
uint32_t serial_dropped(void) { return 0; }
struct task *task_create_kernel(const char *n,void (*fn)(void *),void *arg,enum task_prio p)
{ (void)n;(void)fn;(void)arg;(void)p; return &reporter; }
__attribute__((noreturn)) void panic(const char *fmt,...)
{ fprintf(stderr,"panic: %s\n",fmt); exit(1); }
#include "../../src/kernel/core/cpustat.c"
static void reset(void)
{
    (void)next_id;
    memset(rq_head,0,sizeof(rq_head));memset(rq_tail,0,sizeof(rq_tail));
    idle=(struct task){.id=1,.prio=P_IDLE,.state=T_RUNNING};strcpy(idle.name,"idle");
    normal=(struct task){.id=2,.prio=P_NORMAL,.state=T_READY,.user=true};strcpy(normal.name,"worker");
    hog=(struct task){.id=3,.prio=P_DEVICE,.state=T_READY};strcpy(hog.name,"hog");
    ui=(struct task){.id=4,.prio=P_INTERACTIVE,.state=T_READY,.user=true};strcpy(ui.name,"desktop");
    idle.all_next=&normal; normal.all_next=&hog; hog.all_next=&ui; all_tasks=&idle;
    g_current=idle_task=&idle;g_ticks=0;cycles=1;slice_left=10;flags=0x200;
    focus_task=audio_focus=0;g_need_resched=false;g_starvation_boosts=0;
    last_measured=0;last_preempted=false;
    g_task_switches=g_task_switches_other=g_switch_target=0;g_measure_stop=false;
    memset(classes,0,sizeof(classes));memset(paths,0,sizeof(paths));
    irq_cycles=exited_cycles=stretch_max=resched_max=0;switches=overruns=0;
    overrun_used=overrun_lost=0;
    forced=threshold=resched_pending=false;record_bytes=0;records[0]=0;g_boot.flags=0;
    memset(&timing_fixture,0,sizeof(timing_fixture));
    cpu_switch(0,&idle);
}
static void run_tick(uint32_t eip)
{
    cycles+=1000;g_ticks++;
    struct trap_frame tf={.vector=0x20,.cs=g_current->user?0x1b:8,.eip=eip};
    uint64_t start=cpu_irq_enter(&tf);sched_tick();cpu_irq_leave(start);
}
static void wake_test(void)
{
    reset();rq_push(&normal);schedule();CHECK(g_current==&normal);
    hog.state=T_BLOCKED;hog.wake_tick=1;
    run_tick(0x1234);CHECK(g_need_resched && hog.state==T_READY);
    schedule();CHECK(g_current==&hog && normal.state==T_READY);
}
static void device_test(void)
{
    reset();rq_push(&normal);rq_push(&hog);schedule();CHECK(g_current==&hog);
    /* Yields cannot restart a device's runtime allowance. */
    for(unsigned i=0;i<10;i++) {run_tick(0x8888);if(i==4) schedule();}
    CHECK(hog.device_demoted && hog.prio==P_NORMAL && overruns==1);
    kpreempt_hold();kpreempt_point();CHECK(g_current==&hog);
    kpreempt_release();flags=0;kpreempt_point();CHECK(g_current==&hog);flags=0x200;
    kpreempt_point();CHECK(g_current==&normal && g_ticks<=20);
    schedule();CHECK(g_current==&hog && hog.device_demoted);
    task_sleep_ms(5);CHECK(hog.prio==P_DEVICE && !hog.device_demoted && !hog.device_ticks);
    /* The UI is selected ahead of two busy normal tasks. */
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
            if (refill && blocking) task_sleep_ms(1);
            else if (g_need_resched || refill) schedule();
        }
        CHECK(services>20);
        if (blocking) CHECK(!hog.device_demoted && hog.prio==P_DEVICE && !overruns);
        else CHECK(hog.device_demoted && overruns);
    }
    CHECK(gaps[0]>2 && gaps[1]<=2);
    printf("f3-39 scheduler busy focus: ready-return refill gap=%ums, blocking refill gap=%ums, device guard retained PASS\n",gaps[0],gaps[1]);
    audio_focus=0;
}
static void measured_wake_test(void)
{
    reset();normal.measured=ui.measured=true;ui.prio=P_NORMAL;
    rq_push(&normal);rq_push(&ui);schedule();CHECK(g_current==&normal);
    hog.state=T_BLOCKED;hog.wake_tick=1;g_switch_target=2;
    run_tick(0);schedule();CHECK(g_current==&hog && !g_task_switches);
    task_sleep_ms(1);CHECK(g_current==&ui && g_task_switches==1);
    run_tick(0);schedule();CHECK(g_current==&hog && g_task_switches==1);
    task_sleep_ms(1);CHECK(g_current==&normal && g_task_switches==2 && g_measure_stop);
    run_tick(0);schedule();task_sleep_ms(1);
    CHECK(normal.state==T_BLOCKED && ui.state==T_BLOCKED && !g_task_switches_other);
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
    CHECK(sscanf(records,"[cpu] window_ms=%u idle_pm=%u irq_pm=%u device_pm=%u interactive_pm=%u normal_pm=%u",&win,&id,&ir,&de,&in,&no)==6);
    CHECK(win==2000 && id+ir+de+in+no==1000 && id==50 && de==650 && no==300);
    CHECK(strstr(records,"eip=deadbeef") && strstr(records,"task=hog#3"));
    CHECK(!(g_boot.flags & CBI_F_DIAG_VERBOSE));
    CHECK(strstr(records,"[kernel] irqoff_section=serial_tx_byte site=c011e445 irqoff_max_us=2120 violations=1\n"));
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
int main(void)
{
    wake_test();device_test();fullscreen_refill_test();measured_wake_test();accounting_test();
    puts("kernel latency: wake preemption, bounded device hog, lock/IF deferral, UI priority, CPU/exited shares, guest and stretch EIP, forced record PASS");
    return 0;
}
