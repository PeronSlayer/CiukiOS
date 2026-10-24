/* fx-22: T23 replay of the full-screen return beside the desktop (trials
 * e831e685 and f441c5bd, preview images of 2026-10-23). Production run
 * queues, priorities, focused-VM and urgent-IRQ preference (task.c rq_pop,
 * kpreempt_point), the f1-98 desktop share, starvation boosts, device
 * budget, ordered timers and kwait (sync.c), cpustat, the VGA transport
 * (vm/vga_hardware.c) and the fx-22 switch policy (sliced from
 * vm/dosvm_fullscreen.c: transaction boost and parked-VM wait) run
 * unmodified; every task is a coroutine (ucontext) so blocking waits really
 * block. Modeled: CPU time at 1 us resolution, the stack switch, the PIT.
 *
 * Trial facts (private records, cited in docs/validation/fx-22-*.md):
 * e831e685 BOOT.LOG L1513-1516: the entry's 256 KiB planar save took
 * 226 ms (0.86 us per byte); L1608-1610: the return's capture of the same
 * 256 KiB took 2230 ms; serial L6214-6216: [cpu] window_ms=2714 idle_pm=0,
 * dosvm#59 418 permille, desktop#40 (desktop worker, share group) 338,
 * the display worker below the top five (< 34 permille); restore_us=133139.
 * f441c5bd BOOT.LOG L14156-14158: the return ends at vga-capture.
 *
 * Workload assumptions (not measurements): the VM traps every 250 us while
 * the guest runs and parks at the next trap; its focus level is 9 (the
 * window's frame wait donates SCHED_FOREGROUND); while parked its PIC keeps
 * IRQ0 requested (the frozen fx-16 clock never acknowledges it), which
 * makes it the urgent focus task of task.c whenever it is ready (a variant
 * without it is run too); the desktop worker (share group) is busy 90 % of
 * the time at level 8 (base 7 plus an I/O boost), the e831e685 calibration
 * (desktop#40 338 permille of a 2714 ms window, the display worker < 34),
 * or CPU-bound at level 7; one variant adds a CPU-bound level-9 task (a
 * focused native or DOS window owner); the storage writer runs 300 us every
 * 10 ms and the audio refill 300 us every 5 ms.
 *
 * Policies: `pre` is the pre-fx-22 checkpoint (1 ms sleep loop, verbatim)
 * with no transaction boost (negative control); `fx22` is the production
 * policy. One scenario per process: production statics start clean.
 * SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#define HOST_SYNC_TIMERS
#define main latency_fixture_main
#include "kernel_latency_test.c"
#undef main
#define ktime_cycles sync_unused_cycles
#include "../../src/kernel/core/sync.c"
#undef ktime_cycles
#include <ucontext.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../src/kernel/vm/vga_hardware.h"
static void timer_fixture_reset(void)
{
    timer_root=timer_first=timer_due_head=timer_due_tail=0;
    memset(&timer_stats,0,sizeof(timer_stats));deferred_fn=0;
}
bool g_cpu_tsc=true;
void klog(const char *fmt,...) { (void)fmt; }

/* ---- the production fx-22 policy ---- */
#define DOSVM_STOPPING 3u
struct dosvm { struct { unsigned state; } info; bool fullscreen_hold, fullscreen_suspended, fullscreen_parked, window_marking, clipboard_mark; /* G5/G17: a window mark selection also parks the VM */ struct kwait wake; };
#include "fullscreen_return_policy.inc"
/* Negative control: the pre-fx-22 checkpoint of dosvm_fullscreen.c. */
static void pre_checkpoint(struct dosvm *v)
{
    if (!v->fullscreen_hold && !v->fullscreen_suspended) return;
    v->fullscreen_parked=true;
    while ((v->fullscreen_hold || v->fullscreen_suspended) && v->info.state<DOSVM_STOPPING)
        task_sleep_ms(1);
    v->fullscreen_parked=false;
}

/* ---- coroutines: one per task; the scheduler's switch_context swaps ---- */
#define CORO_STACK (256u * 1024u)
struct coro { struct task *t; void (*fn)(void *); ucontext_t ctx; char *stack; bool preemptible; };
static struct coro coros[12];
static unsigned coro_count;
static ucontext_t main_ctx;
static struct task *on_cpu;
static struct coro *coro_of(const struct task *t)
{
    for (unsigned i=0;i<coro_count;i++) if (coros[i].t==t) return &coros[i];
    CHECK(false); return 0;
}
static void coro_entry(unsigned i) { flags=0x200; coros[i].fn(0); CHECK(false); }
static void coro_add(struct task *t, void (*fn)(void *), bool preemptible)
{
    CHECK(coro_count<12);
    struct coro *c=&coros[coro_count];
    *c=(struct coro){.t=t,.fn=fn,.preemptible=preemptible,.stack=malloc(CORO_STACK)};
    CHECK(c->stack && !getcontext(&c->ctx));
    c->ctx.uc_stack.ss_sp=c->stack; c->ctx.uc_stack.ss_size=CORO_STACK; c->ctx.uc_link=0;
    makecontext(&c->ctx,(void (*)(void))coro_entry,1,coro_count);
    coro_count++;
}
static void replay_switch(void)
{
    struct task *prev=on_cpu, *next=g_current;
    if (prev==next) return;
    on_cpu=next;
    CHECK(!swapcontext(&coro_of(prev)->ctx,&coro_of(next)->ctx));
}

/* ---- scenario and results ---- */
struct scenario {
    const char *name;
    bool fx22;             /* production policy, else the pre-fx-22 control */
    bool desktop_busy;     /* desktop worker (share group) runs busy_us, blocks 1 ms */
    unsigned desktop_level;/* its effective level (7, or 8 with an I/O boost) */
    unsigned busy_us;
    unsigned peer_level;   /* a CPU-bound ordinary task at this level (0 = none) */
    bool irq_pending;      /* the parked VM's PIC keeps IRQ0 requested */
};
struct result {
    bool completed;
    unsigned park_ms, capture_ms, restore_ms, total_ms;
    unsigned display_permille, desktop_permille, vm_permille, vm_wakes;
};
static struct scenario sc;
static struct result res;
#define RETURN_AT_MS 1000u
#define GIVE_UP_MS 30000u
#define BYTE_NS 860u          /* e831e685 entry save: 226 ms / 262144 bytes */
#define RESTORE_STEPS 133u    /* e831e685 restore_us=133139, safe point per ms */
static struct task vm, desktop, peer, storage, refill, display;
static struct dosvm guest;
static uint64_t task_us[6], return_start_us;
static unsigned slot_of(const struct task *t)
{ return t==&display ? 0 : t==&desktop ? 1 : t==&vm ? 2 : t==&peer ? 3 : t==&storage ? 4 : 5; }

static uint64_t boundary(void) { return 1+(g_ticks+1)*1000; }
static void finish(void);
static void do_tick(void)
{
    uint32_t saved=flags; flags=0;
    g_ticks++;
    struct trap_frame tf={.vector=0x20,.cs=coro_of(on_cpu)->preemptible ? 0x1b : 8};
    uint64_t start=cpu_irq_enter(&tf); sched_tick(); cpu_irq_leave(start);
    flags=0x200;
    for (unsigned n=0;deferred_fn && n<1024;n++) { kwork_fn fn=deferred_fn; deferred_fn=0; fn(deferred_arg); }
    flags=saved;
    if (g_ticks>=RETURN_AT_MS+GIVE_UP_MS) finish();
}
static void cpu(uint64_t us)
{
    while (us) {
        uint64_t room=boundary()-cycles, step=us<room ? us : room;
        cycles+=step; us-=step;
        if (return_start_us) task_us[slot_of(on_cpu)]+=step;
        if (cycles==boundary()) {
            do_tick();
            /* IRQ return to ring 3 preempts; the kernel only at safe points. */
            if (coro_of(on_cpu)->preemptible && g_need_resched) schedule();
        }
    }
}

/* ---- the adapter: 0.86 us per uncached aperture byte ---- */
static unsigned bus_ns;
static uint8_t bus_in(void *a,uint16_t p) { (void)a;(void)p; return 0; }
static void bus_out(void *a,uint16_t p,uint8_t b) { (void)a;(void)p;(void)b; }
static uint8_t bus_read(void *a,unsigned o)
{ (void)a;(void)o; bus_ns+=BYTE_NS; if (bus_ns>=1000) { cpu(bus_ns/1000); bus_ns%=1000; } return 0x5a; }
static void bus_write(void *a,unsigned o,uint8_t b) { (void)a;(void)o;(void)b; }
static cvga_state model;

/* ---- task bodies ---- */
static void idle_main(void *arg)
{
    (void)arg;
    for (;;) { cycles=boundary(); do_tick(); if (g_need_resched || rq_any()) schedule(); }
}
static void vm_main(void *arg)
{
    (void)arg;
    for (;;) {
        coro_of(&vm)->preemptible=true; cpu(250); coro_of(&vm)->preemptible=false;
        if (guest.fullscreen_hold) {          /* dosvm_trap's checkpoint */
            if (sc.fx22) dosvm_fullscreen_checkpoint(&guest); else pre_checkpoint(&guest);
        }
    }
}
static void busy_main(void *arg) { (void)arg; for (;;) cpu(1000); }
static void desktop_main(void *arg) { (void)arg; for (;;) { cpu(sc.busy_us); task_sleep_ms(1); } }
static void storage_main(void *arg) { (void)arg; for (;;) { cpu(300); task_sleep_ms(10); } }
static void refill_main(void *arg) { (void)arg; for (;;) { cpu(300); task_sleep_ms(5); } }
static void display_main(void *arg)
{
    (void)arg;
    task_sleep_ms(RETURN_AT_MS);
    /* dosvm_fullscreen_leave: boost, guest-revoke and park, capture,
     * display_fullscreen_end (native restore), unhold. */
    return_start_us=cycles;
    uint64_t t0=g_ticks;
    struct fs_boost b={0};
    if (sc.fx22) b=boost_begin();
    guest.fullscreen_hold=true; kwait_wake_all(&guest.wake);
    while (!guest.fullscreen_parked && g_ticks-t0<FS_PARK_MS) task_sleep_ms(1);
    uint64_t parked=g_ticks;
    struct vga_hardware_bus bus={0,bus_in,bus_out,bus_read,bus_write};
    (void)vga_hardware_capture_pages(&model,&bus,VGA_HARDWARE_ALL_PAGES,0,0);
    uint64_t captured=g_ticks;
    for (unsigned i=0;i<RESTORE_STEPS;i++) { cpu(1000); kpreempt_point(); }
    uint64_t restored=g_ticks;
    guest.fullscreen_hold=false; kwait_wake_all(&guest.wake);
    if (sc.fx22) boost_end(&b);
    res.completed=true;
    res.park_ms=(unsigned)(parked-t0); res.capture_ms=(unsigned)(captured-parked);
    res.restore_ms=(unsigned)(restored-captured); res.total_ms=(unsigned)(restored-t0);
    finish();
}
static void finish(void)
{
    uint64_t total=cycles-return_start_us;
    if (total) {
        res.display_permille=(unsigned)(task_us[0]*1000/total);
        res.desktop_permille=(unsigned)(task_us[1]*1000/total);
        res.vm_permille=(unsigned)(task_us[2]*1000/total);
    }
    res.vm_wakes=(unsigned)vm.dispatches;
    CHECK(!swapcontext(&coro_of(on_cpu)->ctx,&main_ctx));
    CHECK(false);
}
static struct task *add_task(struct task *t, const char *name, uint32_t id, enum task_prio prio, bool user)
{
    *t=(struct task){.id=id,.generation=1,.prio=prio,.state=T_BLOCKED,.user=user};
    strcpy(t->name,name);
    t->all_next=idle.all_next; idle.all_next=t;
    return t;
}
static void child(void)
{
    reset();
    normal.state=hog.state=ui.state=T_BLOCKED; idle.all_next=0;
    latency_switch_hook=replay_switch;
    waiters=0; held=0; owned_queues=0; propagating=false; dependents=0;
    guest=(struct dosvm){0}; kwait_init(&guest.wake); kwait_set_owner(&guest.wake,0);
    coro_add(&idle,idle_main,false); on_cpu=&idle;
    add_task(&vm,"dosvm",59,P_NORMAL,false); coro_add(&vm,vm_main,true);
    add_task(&display,"display-watch",33,P_NORMAL,false); coro_add(&display,display_main,false);
    add_task(&storage,"storage-writer",15,P_DEVICE,false); coro_add(&storage,storage_main,false);
    add_task(&refill,"audio-refill",18,P_DEVICE,false); coro_add(&refill,refill_main,false);
    task_set_audio_class(&refill);
    task_set_inherited(&vm,SCHED_FOREGROUND);
    if (sc.desktop_busy) {
        add_task(&desktop,"desktop",40,P_NORMAL,true); coro_add(&desktop,desktop_main,true);
        if (sc.desktop_level!=CIUKI_SCHED_NORMAL) task_set_base(&desktop,P_NORMAL,sc.desktop_level);
        task_set_share_group(&desktop,true); task_start(&desktop);
    }
    if (sc.peer_level) {
        add_task(&peer,"peer",71,P_NORMAL,true); coro_add(&peer,busy_main,true);
        task_set_base(&peer,P_NORMAL,sc.peer_level); task_start(&peer);
    }
    /* dosvm.c: the focused VM, and its pending IRQ0 makes it urgent. */
    audio_focus=&vm; irq_focus=sc.irq_pending ? &vm : 0;
    task_start(&vm); task_start(&display); task_start(&storage); task_start(&refill);
    CHECK(!swapcontext(&main_ctx,&coro_of(&idle)->ctx));
}
static struct result run(const struct scenario *s)
{
    int fd[2]; CHECK(!pipe(fd));
    fflush(stdout);
    pid_t pid=fork(); CHECK(pid>=0);
    if (!pid) {
        close(fd[0]); sc=*s; memset(&res,0,sizeof(res));
        child();
        CHECK(write(fd[1],&res,sizeof(res))==(ssize_t)sizeof(res));
        _exit(0);
    }
    close(fd[1]);
    struct result r; CHECK(read(fd[0],&r,sizeof(r))==(ssize_t)sizeof(r));
    close(fd[0]);
    int status=0; CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status) && !WEXITSTATUS(status));
    return r;
}
static void row(const struct scenario *s, const struct result *r)
{
    if (!r->completed)
        printf("  %-62s return not complete after %u s | CPU since return: display=%u desktop=%u vm=%u permille\n",
               s->name,GIVE_UP_MS/1000,r->display_permille,r->desktop_permille,r->vm_permille);
    else
        printf("  %-62s park=%u capture=%u restore=%u total=%u ms | CPU: display=%u desktop=%u vm=%u permille, vm dispatches=%u\n",
               s->name,r->park_ms,r->capture_ms,r->restore_ms,r->total_ms,
               r->display_permille,r->desktop_permille,r->vm_permille,r->vm_wakes);
}
int main(void)
{
    g_tsc_per_ms=1000;
    static const struct scenario cases[]={
        {"e831e685: desktop worker 90 % at level 8, pre-fx-22",false,true,8,9000,0,true},
        {"e831e685: desktop worker 90 % at level 8, fx-22",true,true,8,9000,0,true},
        {"desktop worker CPU-bound at level 7, pre-fx-22",false,true,7,100000,0,true},
        {"desktop worker CPU-bound at level 7, fx-22",true,true,7,100000,0,true},
        {"no pending VM IRQ, desktop worker 90 % at 8, pre-fx-22",false,true,8,9000,0,false},
        {"no pending VM IRQ, desktop worker 90 % at 8, fx-22",true,true,8,9000,0,false},
        {"CPU-bound level-9 task beside the worker, pre-fx-22",false,true,7,100000,9,true},
        {"CPU-bound level-9 task beside the worker, fx-22",true,true,7,100000,9,true},
        {"idle desktop (typical trials, 170-360 ms), pre-fx-22",false,false,7,0,0,true},
        {"idle desktop, fx-22",true,false,7,0,0,true},
    };
    enum { N=sizeof(cases)/sizeof(*cases) };
    struct result r[N];
    printf("FX-22 replay T23 e831e685/f441c5bd: full-screen return beside the desktop (capture 256 KiB at %u ns/byte, restore %u ms):\n",
           BYTE_NS,RESTORE_STEPS);
    for (unsigned i=0;i<N;i++) { r[i]=run(&cases[i]); row(&cases[i],&r[i]); }
    /* The control reproduces e831e685: a 226 ms capture stretched about
     * tenfold (trial 2230 ms), the display worker under 10 % of the CPU. */
    CHECK(r[0].completed && r[0].capture_ms>=1500 && r[0].capture_ms<=3500 && r[0].display_permille<150);
    /* A busier ordinary load starves the pre-fx-22 return past its 2 s
     * budget (the owner sees a frozen screen). */
    CHECK(!r[6].completed || r[6].total_ms>2000);
    /* fx-22: every scenario completes within the 2 s return budget, the
     * capture within 1.3x its CPU cost, whatever the ordinary load. */
    for (unsigned i=1;i<N;i+=2) {
        CHECK(r[i].completed && r[i].total_ms<=600 && r[i].capture_ms<=300);
        CHECK(!r[i-1].completed || r[i].total_ms<=r[i-1].total_ms);
    }
    puts("FX-22 gates: the pre-fx-22 control reproduces e831e685's tenfold capture and misses the 2 s budget under load; the fx-22 policy returns within 600 ms in every scenario PASS");
    return 0;
}
