/* Replay native worker orchestration while one controller's bind sleeps.
 * USB packet engines and HID completion timing have their separate register
 * replay in latency_test.c. No scheduler/worker loop is replaced here.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <ciuki/kernel.h>
#include <ciuki/usb.h>
#include <ciuki/task.h>
volatile uint64_t g_ticks;
struct ciuki_boot_info g_boot;
struct task *g_current;
static struct usb_host hosts[USB_HOST_MAX];
static unsigned host_count=2, created, started, scans, polls, gap_max, host_turns;
static unsigned current_worker;
static uint64_t last_poll;
static struct { struct task task; void (*fn)(void *); void *arg; uint64_t due; } workers[3];
static struct kwait *waiting[3];
static jmp_buf turn_done[3];
static bool slow_binding, quiet_irq;
static uint32_t irq_save(void) { return 0x200; }
static void irq_restore(uint32_t f) { (void)f; }
void klog(const char *fmt,...) { (void)fmt; }
struct task *task_create_kernel(const char *n,void (*fn)(void *),void *arg,enum task_prio p)
{
    assert(created<3 && p==P_DEVICE && fn);
    unsigned i=created++;
    assert(!strcmp(n,i==2?"usb-hid":"usb-host"));
    assert(arg==(i<2?&hosts[i]:NULL));
    workers[i].fn=fn;workers[i].arg=arg;workers[i].task.state=T_BLOCKED;
    return &workers[i].task;
}
void task_start(struct task *t) { assert(t->state==T_BLOCKED);t->state=T_READY;started++; }
void task_sleep_ms(uint32_t ms) { workers[current_worker].due=g_ticks+ms;longjmp(turn_done[current_worker],1); }
uint64_t deadline_after_ms(uint32_t ms) { return g_ticks+ms; }
bool kwait_wait_until(struct kwait *q,kwait_cond_fn condition,void *arg,uint64_t deadline)
{ waiting[current_worker]=q;if(!condition(arg))task_sleep_ms((uint32_t)(deadline-g_ticks));return true; }
static bool host_pending(void *arg) { return ((struct usb_host *)arg)->irq_pending; }
static bool input_pending;
static struct kwait input_changed;
static bool hid_pending(void *arg) { (void)arg;return input_pending; }
void usb_host_quarantine(struct usb_host *h,const char *why) { (void)h;(void)why;assert(0); }
void usb_hid_service(struct usb_host *h)
{
    if(h!=&hosts[0]) return;
    if(polls && g_ticks-last_poll>gap_max) gap_max=(unsigned)(g_ticks-last_poll);
    last_poll=g_ticks;polls++;
    assert(slow_binding); /* independent HID service while host 1 is reserved */
}
static void dispatch(unsigned i)
{
    if(g_ticks<workers[i].due) return;
    current_worker=i;
    if(!setjmp(turn_done[i])) workers[i].fn(workers[i].arg);
}
void usb_step(struct usb_host *h)
{
    if(h==&hosts[0]) {
        assert(slow_binding);
        host_turns++;
        if(g_ticks>=h->next_scan) { scans++;h->next_scan=g_ticks+250; }
    } else {
        assert(h==&hosts[1]);h->busy=true;slow_binding=true;
        for(unsigned i=0;i<500;i++) {
            /* Two actual completion notifications, no frame heartbeat. */
            if(hosts[0].irq_attached && !quiet_irq && (i==100 || i==300)) {
                hosts[0].irq_pending=input_pending=true;
                for(unsigned n=0;n<3;n++) if(waiting[n]==&hosts[0].changed || waiting[n]==&input_changed)
                    workers[n].due=g_ticks;
            }
            dispatch(0);dispatch(2);g_ticks++;
        }
        slow_binding=false;h->busy=false;
        /* End the host-1 service turn at its actual outer sleep. */
        current_worker=1;
    }
}
#include "usb_worker_replay.inc"
int main(void)
{
    for(unsigned i=0;i<host_count;i++) { hosts[i].ready=true; fs_lock_init(&hosts[i].transfer_lock); }
    for(unsigned mode=0;mode<3;mode++) {
        memset(workers,0,sizeof(workers));memset(waiting,0,sizeof(waiting));
        created=started=scans=polls=gap_max=host_turns=0;g_ticks=last_poll=0;
        hosts[0].next_scan=hosts[1].next_scan=0;hosts[0].irq_attached=mode!=0;
        quiet_irq=mode==2;
        start_services();assert(created==3 && started==3);dispatch(1);
        assert(scans==2 && polls==500 && gap_max==1 && g_ticks==500);
        assert(host_turns==(mode==1 ? 4u:2u));
    }
    for(unsigned i=0;i<host_count;i++) fs_lock_destroy(&hosts[i].transfer_lock);
    puts("USB workers: real tasks, 500 ms bind on host 1; host 0 has 2 scan turns + 2 completion turns, no heartbeat; 4/s root scans, 1 ms HID fallback PASS");
    puts("USB workers idle IRQ host: 4 scan + 1000 HID turns/s = 1004 wakes/s, 0 interrupts PASS");
}
