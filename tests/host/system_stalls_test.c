/* F0-20: f3-17's production scheduler replay with P_DEVICE controllers and
 * a P_NORMAL desktop. Fake only time, architecture switches and record sinks.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <ciuki/kernel.h>
#include <ciuki/task.h>
#include <ciuki/abi.h>
#define CIUKI_CPU_H
static uint32_t flags=0x200;
static uint32_t irq_save(void) { uint32_t old=flags; flags=0; return old; }
static void irq_restore(uint32_t f) { flags=f; }
static uint32_t read_eflags(void) { return flags; }
static uint64_t extra_cycles;
uint64_t g_tsc_per_ms=1000;
bool g_cpu_tsc=true;
volatile uint64_t g_ticks;
struct ciuki_boot_info g_boot;
struct task *g_current;
uint32_t g_quantum_ticks=10, g_starvation_boosts;
uint64_t g_task_switches, g_task_switches_other, g_switch_target;
volatile bool g_measure_stop, g_need_resched;
uint64_t ktime_cycles(void) { return g_ticks*1000+extra_cycles; }
void file_clock_tick(void) {}
extern uint32_t dosvm_task_cr3(const struct task *,uint32_t) __attribute__((weak));
extern uint32_t dosvm_task_esp0(const struct task *,uint32_t) __attribute__((weak));
extern void dosvm_task_switch(const struct task *) __attribute__((weak));
extern void dosvm_tick(void) __attribute__((weak));
#define write_cr3(value) ((void)(value))
#define biosvm_task_cr3(task, normal) ((void)(task), (normal))
#define biosvm_task_esp0(task, normal) ((void)(task), 0u)
#define vmm_kernel_pd() 0u
#define tss_set_kernel_stack(value) ((void)(value))
#define crit_begin() ((void)0)
#define crit_end() ((void)0)
#define switch_context(old, next) ((void)(old), (void)(next))
#define proc_task_switch(task) ((void)(task))
#define fpu_task_switch(task) ((void)(task))
#include "system_stalls_scheduler.inc"
#undef crit_begin
#undef crit_end
#include "../../src/kernel/core/attribution.c"

static char records[8192];
static unsigned record_count;
int ksnprintf(char *out,size_t n,const char *fmt,...)
{ va_list ap;va_start(ap,fmt);int r=vsnprintf(out,n,fmt,ap);va_end(ap);return r; }
void klog_deferred(const char *fmt,...)
{
    assert(flags&0x200);
    va_list ap;va_start(ap,fmt);
    size_t at=strlen(records);int n=vsnprintf(records+at,sizeof(records)-at,fmt,ap);
    va_end(ap);assert(n>0 && n<255);strcat(records,"\n");record_count++;
    /* Recursive polling must not recursively produce records. */
    attribution_poll(true);
}
static void reset(void)
{
    g_ticks=extra_cycles=0; flags=0x200; initialized=emitting=false;
    g_need_resched=false; focus_task=NULL; focus_window=focus_ticks=0;
    all_tasks=NULL; g_current=idle_task=NULL;
    memset(rq_head,0,sizeof(rq_head));memset(rq_tail,0,sizeof(rq_tail));
    memset(records,0,sizeof(records));record_count=0;
    g_boot.flags=CBI_F_VERBOSE;
}
static struct task current(unsigned id,const char *name)
{
    struct task t={.id=id,.generation=1,.state=T_RUNNING,.prio=P_NORMAL};
    snprintf(t.name,sizeof(t.name),"%s",name);return t;
}
static void switch_to(struct task *t)
{ attribution_switch(t);g_current=t; }
static void accounting(void)
{
    reset();struct task hog=current(1,"elf-spawn"),usb=current(2,"usb-2474"),idle=current(3,"idle");
    usb.prio=P_DEVICE;idle.prio=P_IDLE;g_current=&hog;
    struct long_path_scope path=long_path_enter(LONG_ELF,"elf_load");
    g_ticks=25;long_path_leave(path); /* 25 ms of CPU, then 1000 ms asleep */
    switch_to(&idle);g_ticks=1025;switch_to(&usb);g_ticks=1040;
    switch_to(&hog);g_ticks=2000;attribution_poll(false);
    assert(record_count==2);
    assert(strstr(records,"[cpu] window_ms=2000 idle=500 top=elf-spawn#1:492,usb-2474#2:7"));
    assert(strstr(records,"elf_load_ms=25 max_single_ms=25 site=elf_load"));
    /* A different task can own the same path while the first sleeps. */
    records[0]=0;record_count=0;
    path=long_path_enter(LONG_BOOTLOG,"bootlog_drain");g_ticks+=10;
    switch_to(&usb);struct long_path_scope peer=long_path_enter(LONG_BOOTLOG,"peer_drain");
    g_ticks+=12;long_path_leave(peer);g_ticks+=978;switch_to(&hog);
    g_ticks+=8;long_path_leave(path);g_ticks=4000;attribution_poll(false);
    assert(record_count==0); /* 18 + 12 ms total is not a >20 ms stretch */
    attribution_poll(true);assert(record_count==2); /* forced desktop stall */
    reset();g_current=&hog;hog.attribution_epoch=0;hog.attribution_path=0;
    path=long_path_enter(LONG_CONSOLE,"console_scroll");
    flags=0;extra_cycles=50000;long_path_leave(path); /* no PIT IRQ during repaint */
    attribution_poll(true);assert(!record_count);flags=0x200;
    attribution_poll(true);assert(strstr(records,"console_repaint_ms=50") && strstr(records,"max_single_ms=50 site=console_scroll"));
    /* Copied identities survive task destruction and slot reuse. */
    memset(&hog,0,sizeof(hog));g_current=&usb;usb.attribution_epoch=0;
    g_ticks=2000;attribution_poll(true);assert(record_count==4);
    reset();g_current=&usb;g_boot.flags=0;
    path=long_path_enter(LONG_ELF,"elf_load");g_ticks=9999;long_path_leave(path);
    attribution_poll(true);assert(!record_count && !initialized);
    /* PIT fallback and quiet, all-idle two-second windows stay silent. */
    reset();g_current=&idle;idle.attribution_epoch=idle.attribution_path=0;g_cpu_tsc=false;
    attribution_switch(&idle);g_ticks=2000;attribution_poll(false);assert(!record_count);
    g_cpu_tsc=true;
    reset();struct task nested=current(4,"desktop");g_current=&nested;
    path=long_path_enter(LONG_SYSCALL,"syscall_dispatch");g_ticks=10;
    peer=long_path_enter(LONG_CONSOLE,"scroll");g_ticks=22;long_path_leave(peer);
    g_ticks=31;long_path_leave(path);g_ticks=2000;attribution_poll(false);
    assert(strstr(records,"console_repaint_ms=12") && strstr(records,"max_single_ms=31 site=syscall_dispatch"));
    puts("PASS F0-20 attribution: CPU deltas/idle, sleep exclusion, >20ms gate, forced/IF-off/nested-safe output, quiet/PIT fallback");
}
static void ranking(void)
{
    reset();struct task tasks[7];
    for(unsigned i=0;i<7;i++)tasks[i]=current(i+1,"worker");
    g_current=&tasks[0];attribution_switch(g_current);
    for(unsigned i=0;i<7;i++) {switch_to(&tasks[i]);g_ticks+=(i+1)*10;}
    attribution_poll(true);
    assert(strstr(records,"top=worker#7:250,worker#6:214,worker#5:178,worker#4:142,worker#3:107"));
    assert(!strstr(records,"worker#2:"));
    /* Ended tasks still rank: no dangling pointers in the window. */
    reset();g_current=&tasks[0];tasks[0].attribution_epoch=0;
    attribution_switch(g_current);g_ticks=30;switch_to(&tasks[1]);
    memset(&tasks[0],0,sizeof(tasks[0]));g_ticks=40;attribution_poll(true);
    assert(strstr(records,"worker#1:750"));
    reset();struct task churn=current(1,"short-worker");g_current=&churn;
    attribution_switch(g_current);
    for(unsigned i=1;i<CPU_SLOTS+10;i++) {
        g_ticks++;attribution_switch(g_current);
        churn=current(i+1,"short-worker");
    }
    g_ticks++;attribution_poll(true);
    assert(bucket_count==0 && strstr(records,"other#4294967295:"));
    puts("PASS F0-20 attribution: five descending identities and reaped CPU owner retained");
}
struct replay_result {unsigned scans[3],frames,usb_delay,desktop_gap;};
static struct replay_result replay(bool cooperative)
{
    reset();g_boot.flags=0;
    struct task idle=current(99,"idle"),desktop=current(1,"desktop"),hog=current(2,"elf-spawn"),usb[3];
    idle.prio=P_IDLE;g_current=idle_task=&idle;
    for(unsigned i=0;i<3;i++) {usb[i]=current(3+i,"usb-worker");usb[i].state=T_READY;usb[i].prio=P_DEVICE;usb[i].all_next=all_tasks;all_tasks=&usb[i];rq_push(&usb[i]);}
    desktop.state=hog.state=T_READY;desktop.all_next=all_tasks;all_tasks=&desktop;
    hog.all_next=all_tasks;all_tasks=&hog;rq_push(&desktop);rq_push(&hog);schedule();
    struct replay_result out={0};unsigned remaining=1024,chunk=0,last_frame=0;
    uint64_t scan_due[3]={0};
    while(g_ticks<3000) {
        if(g_current==&hog && remaining) {
            /* Fixed 4 MiB zero work: 1 ms per page, before a single loop;
             * after, the production 64 KiB boundary permits a sleep. */
            remaining--;chunk++;g_ticks++;sched_tick();
            if(!remaining || (cooperative && chunk==16)) {
                chunk=0;g_current->wake_tick=remaining ? g_ticks+1 : 0;
                g_current->state=T_BLOCKED;schedule();
            }
            continue; /* ring 0 is deliberately not timer-preempted */
        }
        if(g_current==&desktop) {
            unsigned gap=(unsigned)g_ticks-last_frame;
            if(gap>out.desktop_gap)out.desktop_gap=gap;
            last_frame=(unsigned)g_ticks;out.frames++;
            desktop.wake_tick=g_ticks+16;desktop.state=T_BLOCKED;schedule();continue;
        }
        bool serviced=false;
        for(unsigned i=0;i<3;i++)if(g_current==&usb[i]) {
            unsigned delay=(unsigned)(g_ticks-scan_due[i]);
            if(delay>out.usb_delay)out.usb_delay=delay;
            out.scans[i]++;scan_due[i]=usb[i].wake_tick=g_ticks+250;usb[i].state=T_BLOCKED;schedule();serviced=true;break;
        }
        if(serviced)continue;
        g_ticks++;sched_tick();if(g_need_resched)schedule();
    }
    return out;
}
int main(void)
{
    accounting();ranking();
    struct replay_result before=replay(false),after=replay(true);
    printf("F0-20 replay before usb_scans=%u,%u,%u usb_max_delay_ms=%u frames=%u frame_max_gap_ms=%u\n",before.scans[0],before.scans[1],before.scans[2],before.usb_delay,before.frames,before.desktop_gap);
    printf("F0-20 replay after usb_scans=%u,%u,%u usb_max_delay_ms=%u frames=%u frame_max_gap_ms=%u\n",after.scans[0],after.scans[1],after.scans[2],after.usb_delay,after.frames,after.desktop_gap);
    fflush(stdout);
    assert(before.usb_delay>=750 && before.desktop_gap>=1000);
    assert(after.usb_delay<=16 && after.desktop_gap<=32 && after.frames>before.frames);
    for(unsigned i=0;i<3;i++)assert(after.scans[i]>=11 && after.scans[i]>before.scans[i]);
    puts("PASS F0-20 f3-17 scheduler replay: three P_DEVICE workers and P_NORMAL desktop progress between 64KiB zero chunks");
    return 0;
}
