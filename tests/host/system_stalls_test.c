/* F0-20 device/desktop replay on the retained f0-21 scheduler and cpustat.
 * Reuse the host clock, record sinks and scheduler extraction fixture.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main kernel_latency_fixture_main
#include "kernel_latency_test.c"
#undef main

static struct task replay_task(unsigned id,const char *name)
{
    struct task t={.id=id,.generation=1,.state=T_RUNNING,.prio=P_NORMAL};
    snprintf(t.name,sizeof(t.name),"%s",name);return t;
}
struct replay_result {unsigned scans[3],frames,usb_delay,desktop_gap;};
static struct replay_result replay(bool cooperative)
{
    reset();
    struct task idle=replay_task(99,"idle"),desktop=replay_task(1,"desktop"),hog=replay_task(2,"elf-spawn"),usb[3];
    idle.prio=P_IDLE;g_current=idle_task=&idle;all_tasks=&idle;cpu_switch(0,&idle);
    for(unsigned i=0;i<3;i++) {usb[i]=replay_task(3+i,"usb-worker");usb[i].state=T_READY;usb[i].prio=P_DEVICE;usb[i].all_next=all_tasks;all_tasks=&usb[i];rq_push(&usb[i]);}
    desktop.state=hog.state=T_READY;desktop.all_next=all_tasks;all_tasks=&desktop;
    hog.all_next=all_tasks;all_tasks=&hog;rq_push(&desktop);rq_push(&hog);schedule();
    struct replay_result out={0};unsigned remaining=1024,chunk=0,last_frame=0;
    uint64_t scan_due[3]={0};
    while(g_ticks<3000) {
        if(g_current==&hog && remaining) {
            /* Fixed 4 MiB zero work: 1 ms per page, before a single loop;
             * after, the production 64 KiB boundary reaches kpreempt_point. */
            remaining--;chunk++;run_tick(0x12345678);
            if(!remaining) {
                g_current->wake_tick=0;g_current->state=T_BLOCKED;schedule();
            } else if(cooperative && chunk==16) {
                chunk=0;kpreempt_point();
            }
            continue; /* ring 0 is deliberately not timer-preempted */
        }
        if(g_current==&desktop) {
            unsigned gap=(unsigned)g_ticks-last_frame;
            if(gap>out.desktop_gap)out.desktop_gap=gap;
            last_frame=(unsigned)g_ticks;out.frames++;
            task_sleep_ms(16);continue;
        }
        bool serviced=false;
        for(unsigned i=0;i<3;i++)if(g_current==&usb[i]) {
            unsigned delay=(unsigned)(g_ticks-scan_due[i]);
            if(delay>out.usb_delay)out.usb_delay=delay;
            out.scans[i]++;scan_due[i]=g_ticks+250;task_sleep_ms(250);serviced=true;break;
        }
        if(serviced)continue;
        run_tick(0);if(g_need_resched)schedule();
    }
    g_boot.flags=CBI_F_VERBOSE;
    cpu_emit();
    CHECK(strstr(records,"[cpu] window_ms=3000") && strstr(records,"[kernel] long_paths"));
    return out;
}
int main(void)
{
    struct replay_result before=replay(false),after=replay(true);
    printf("F0-20 replay before usb_scans=%u,%u,%u usb_max_delay_ms=%u frames=%u frame_max_gap_ms=%u\n",before.scans[0],before.scans[1],before.scans[2],before.usb_delay,before.frames,before.desktop_gap);
    printf("F0-20 replay after usb_scans=%u,%u,%u usb_max_delay_ms=%u frames=%u frame_max_gap_ms=%u\n",after.scans[0],after.scans[1],after.scans[2],after.usb_delay,after.frames,after.desktop_gap);
    CHECK(before.usb_delay>=750 && before.desktop_gap>=1000);
    CHECK(after.usb_delay<=16 && after.desktop_gap<=32 && after.frames>before.frames);
    for(unsigned i=0;i<3;i++)CHECK(after.scans[i]>=11 && after.scans[i]>before.scans[i]);
    puts("PASS F0-20 replay ported to cpustat: three P_DEVICE workers and P_NORMAL desktop progress at 64KiB preemption points");
    return 0;
}
