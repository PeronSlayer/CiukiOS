/* f1-98: T23 replay model of the desktop beside a priority-9 DOS VM (trial
 * b453bc92). Production run queues, priorities, starvation boost, focused-VM
 * preference, device budget, timers, cpustat and sync.c (donation and the
 * f1-98 reservation dependencies); only CPU time, the stack switch and the
 * workload are modeled, at 1 us resolution.
 *
 * Trial facts (t23_trial.h, docs/validation/f1-93b-readdir-holds.md):
 * dosvm#54 at effective 9 used 62-74 % of the CPU and never idled
 * (busy_pm ~740, idle_* = 0), device-class work was 13-20 %, desktop
 * workers (base 7) under 3 % each, getting the CPU only at their 100 ms
 * starvation boost. Workload assumptions (not measured, stated here and in
 * the validation record): the folder worker's compare of /Documents/Desktop
 * costs 20 ms of CPU in 40 readdir steps (12 ms of FAT decoding for the
 * f1-93b 2,000-entry geometry at ~2 us per decoded entry, plus syscall and
 * compare work), each step holding drive C for 100 us; a window open is
 * 3 ms of compositor drawing, a launch worker's 10 lookups of 1 ms (one
 * of them keeping drive C across a 10 ms device read, as the unconverted
 * guarded paths of f1-93b still do), 5 ms of launch work and 8 ms of
 * drawing; the compositor repaints the clock for 1 ms every 50 ms and needs
 * one 100 us stat on drive C during the open. Device class at the trial's
 * 19 % ([cpu] top: audio-refill 4.5-7 %, device-work 7.5-8.6 %, net-input
 * 2.8-3.3 %): audio refill 350 us every 5 ms; device-work 800 us every
 * 10 ms, with the measured 2110 us section every 250 ms (t23_trial.h);
 * net-input 300 us every 10 ms.
 *
 * Policies: share=0 is the pre-f1-98 scheduler (negative control of the
 * desktop share); inherit=0 means waiters never name the drive's holder
 * (negative control of the reservation boost).
 * SPDX-License-Identifier: GPL-2.0-only */
#define HOST_SYNC_TIMERS
#define main latency_fixture_main
#include "kernel_latency_test.c"
#undef main
#include "t23_trial.h"
#define ktime_cycles sync_unused_cycles
#include "../../src/kernel/core/sync.c"
#undef ktime_cycles
static void timer_fixture_reset(void)
{
    timer_root=timer_first=timer_due_head=timer_due_tail=0;
    memset(&timer_stats,0,sizeof(timer_stats));deferred_fn=0;
}
bool g_cpu_tsc;
void klog(const char *fmt,...) { (void)fmt; }

#define RUN_MS 12000u
#define REFILL_US 350u
#define REFILL_PERIOD_MS 5u
#define FOLDER_STEPS 40u
#define FOLDER_STEP_US 500u
#define HOLD_US 100u
#define FOLDER_PERIOD_MS 1000u
#define CLOCK_PERIOD_MS 50u
#define CLOCK_US 1000u
#define DRAW1_US 3000u
#define DRAW2_US 8000u
#define LOOKUPS 10u
#define LOOKUP_US 1000u
#define COLD_LOOKUP 4u
#define COLD_READ_MS 10u
#define LAUNCH_US 5000u
#define GATE_MS 300u
#define FRAME_PERIOD_US 14286u /* 70 Hz VGA retrace */

struct actor {
    struct task *t;
    uint64_t remaining, cpu;
    void (*done)(struct actor *);
    void (*resume)(struct actor *);
    unsigned phase, step;
    uint64_t job_start, waits;
};
static struct task vm, refill, work, compositor, folder, launcher, files, net;
static struct actor actors[8];
static unsigned actor_count;
static bool inherit, cadence, continuous, foreign, vsync;
static unsigned vm_level;          /* VM effective level; 13 = Win32 game (HIGH) */
static bool vm_focused;
static uint16_t group_ms[RUN_MS], vm_ms[RUN_MS];
/* Drive C, modeled at the f1-98 contract: the holder's token, waiters name
 * it (fs_depend) and sleep 1 ms (native file_pause) between polls. */
static struct task *drive_holder;
static char drive_object;
static uint64_t hold_start, hold_max, drive_waits;
/* results */
static uint64_t folder_max, folder_runs, window_max, window_runs, window_start;
static uint64_t refill_last, refill_gap_max, vm_last_run, vm_gap_max;
static bool input_pending, launch_done, compositor_waiting, opening;
static unsigned inputs_skipped;
static uint64_t frames, frame_start, frame_progress, frames_late;
static unsigned frame_work_us;

static uint64_t boundary(void) { return 1 + (g_ticks + 1) * 1000; }
static uint64_t now_us(void) { return cycles - 1; }
static struct actor *actor_of(struct task *t)
{
    for (unsigned i=0;i<actor_count;i++) if (actors[i].t==t) return &actors[i];
    return 0;
}
static void park(void) { g_current->state=T_BLOCKED; g_current->wake_tick=0; schedule(); }
static void wake(struct task *t) { if (t->state==T_BLOCKED) { task_start(t); } }
static void sleep_then(struct actor *a, uint32_t ms_, void (*resume)(struct actor *))
{ a->resume=resume; task_sleep_ms(ms_ ? ms_ : 1); }

/* Event injection and per-tick bookkeeping at the PIT boundary. */
static uint64_t window_due;
static void tick(void)
{
    cycles=boundary(); g_ticks++;
    struct trap_frame tf={.vector=0x20,.cs=g_current->user ? 0x1b : 8};
    uint64_t start=cpu_irq_enter(&tf); sched_tick(); cpu_irq_leave(start);
    uint32_t saved=flags; flags=0x200;
    for (unsigned n=0;deferred_fn && n<1024;n++) { kwork_fn fn=deferred_fn; deferred_fn=0; fn(deferred_arg); }
    if (window_due && g_ticks==window_due) {
        window_due=0;
        if (opening || input_pending) inputs_skipped++; /* the previous open is still running */
        else { window_start=now_us(); input_pending=true; wake(&compositor); }
    }
    flags=saved;
    /* f1-98 invariant: an edge always names the drive's current holder, and
     * nothing is inherited once no edge is left (bounded to the hold). */
    for (struct task *d=dependents;d;d=d->depend_next) CHECK(drive_holder && d->depends_on==drive_holder);
    if (!dependents) for (unsigned i=0;i<actor_count;i++) CHECK(!actors[i].t->inherited_priority);
}

/* VM frames: a game renders W us of work per frame, then (vsync) polls
 * for the next 70 Hz retrace, a frame finished after its retrace slipping
 * one, or (free-running) starts the next frame at once. */
static void vm_progress(uint64_t from, uint64_t step)
{
    uint64_t t=from, end=from+step;
    while (t<end) {
        if (t<frame_start) { t=frame_start<end ? frame_start : end; continue; }
        uint64_t need=frame_work_us-frame_progress;
        if (end-t<need) { frame_progress+=end-t; break; }
        t+=need; frames++; frame_progress=0;
        if (t>frame_start+FRAME_PERIOD_US) frames_late++;
        frame_start=vsync ? (t+FRAME_PERIOD_US-1)/FRAME_PERIOD_US*FRAME_PERIOD_US : t;
    }
}
static void consume(struct actor *a, uint64_t step)
{
    uint64_t from=now_us();
    cycles+=step; a->remaining-=step; a->cpu+=step;
    unsigned slot=(unsigned)g_ticks;
    if (slot<RUN_MS) {
        if (a->t->share_group) group_ms[slot]+=(uint16_t)((step+5)/10);
        if (a->t==&vm) vm_ms[slot]+=(uint16_t)((step+5)/10);
    }
    if (a->t==&vm) {
        if (from-vm_last_run>vm_gap_max) vm_gap_max=from-vm_last_run;
        vm_last_run=now_us();
        if (cadence) vm_progress(from,step);
    }
}

/* Drive C acquisition by the running actor; false: it slept one poll. */
static bool drive_take(struct actor *a, void (*retry)(struct actor *))
{
    if (drive_holder && drive_holder!=a->t) {
        drive_waits++; a->waits++;
        if (inherit) ksync_depend(drive_holder,&drive_object,KSYNC_DEPEND_DRIVE);
        sleep_then(a,1,retry);
        return false;
    }
    if (inherit) ksync_depend(0,0,0);
    drive_holder=a->t; hold_start=now_us();
    a->t->preempt_holds++; /* f1-93b: no preemption point inside a cached hold */
    return true;
}
static void drive_give(struct actor *a)
{
    if (a->t->preempt_holds) a->t->preempt_holds--;
    uint64_t held=now_us()-hold_start;
    if (held>hold_max) hold_max=held;
    drive_holder=0;
    ksync_depend_release(&drive_object);
}

/* VM: busy loop forever. */
static void vm_resume(struct actor *a) { a->remaining=UINT64_MAX/4; }
static void vm_done(struct actor *a) { (void)a; CHECK(false); }

/* Audio refill (P_DEVICE, audio class) and device-work. */
static void refill_resume(struct actor *a)
{
    uint64_t now=now_us();
    if (refill_last && now-refill_last>refill_gap_max) refill_gap_max=now-refill_last;
    refill_last=now; a->remaining=REFILL_US;
}
static void refill_done(struct actor *a) { sleep_then(a,REFILL_PERIOD_MS,refill_resume); }
static void work_resume(struct actor *a) { a->remaining=++a->step%25 ? 800u : T23_DEVICE_WORK_US; }
static void work_done(struct actor *a) { sleep_then(a,10,work_resume); }
static void net_resume(struct actor *a) { a->remaining=300; }
static void net_done(struct actor *a) { sleep_then(a,10,net_resume); }

/* Folder worker: a compare every second (directory-compare). */
static void folder_step(struct actor *a);
static void folder_resume(struct actor *a)
{
    a->step=0; folder_step(a); /* job_start: the wake time set when it slept */
}
static void folder_step(struct actor *a)
{
    if (continuous) { a->phase=2; a->remaining=FOLDER_STEP_US; return; }
    if (!drive_take(a,folder_step)) return;
    a->phase=1; a->remaining=HOLD_US;
}
static void folder_done(struct actor *a)
{
    if (a->phase==2) { a->remaining=FOLDER_STEP_US; return; } /* continuously ready */
    if (a->phase==1) { drive_give(a); a->phase=0; a->remaining=FOLDER_STEP_US-HOLD_US; kpreempt_point(); return; }
    if (++a->step<FOLDER_STEPS) { folder_step(a); return; }
    uint64_t latency=now_us()-a->job_start;
    if (latency>folder_max) folder_max=latency;
    folder_runs++;
    uint32_t next=FOLDER_PERIOD_MS-(uint32_t)(g_ticks%FOLDER_PERIOD_MS);
    a->job_start=(g_ticks+next)*1000;
    sleep_then(a,next,folder_resume);
}

/* Launch worker: lookups on drive C (one across a device read), launch. */
static void launcher_step(struct actor *a);
static void launcher_resume(struct actor *a) { a->step=0; launcher_step(a); }
static void launcher_cold_back(struct actor *a) { a->t->preempt_holds++; a->phase=1; a->remaining=HOLD_US; }
static void launcher_step(struct actor *a)
{
    if (!drive_take(a,launcher_step)) return;
    if (a->step==COLD_LOOKUP) {
        /* A guarded device read: the drive stays held, the CPU is free. */
        a->t->preempt_holds--; a->phase=3; sleep_then(a,COLD_READ_MS,launcher_cold_back); return;
    }
    a->phase=1; a->remaining=HOLD_US;
}
static void launcher_done(struct actor *a)
{
    if (a->phase==1) { drive_give(a); a->phase=0; a->remaining=LOOKUP_US-HOLD_US; kpreempt_point(); return; }
    if (a->phase==0 && ++a->step<LOOKUPS) { launcher_step(a); return; }
    if (a->phase==0) { a->phase=2; a->remaining=LAUNCH_US; return; }
    launch_done=true;
    if (compositor_waiting) { compositor_waiting=false; wake(&compositor); }
    a->resume=launcher_resume; park();
}

/* A non-desktop Win32 task (base 7) reading 256 KiB with ReadFile every
 * 500 ms: the call keeps drive C for its whole duration (f1-93b, not
 * converted), four 64 KiB device commands of 10 ms, 200 us of copy each. */
#define FILES_PERIOD_MS 500u
#define FILES_CHUNKS 4u
static void files_take(struct actor *a);
static void files_resume(struct actor *a) { files_take(a); }
static void files_back(struct actor *a) { a->t->preempt_holds++; a->remaining=2*HOLD_US; }
static void files_take(struct actor *a)
{
    if (!drive_take(a,files_take)) return;
    a->step=0; a->t->preempt_holds--; sleep_then(a,COLD_READ_MS,files_back);
}
static void files_done(struct actor *a)
{
    if (++a->step<FILES_CHUNKS) { a->t->preempt_holds--; sleep_then(a,COLD_READ_MS,files_back); return; }
    drive_give(a);
    sleep_then(a,FILES_PERIOD_MS,files_resume);
}

/* Compositor (P_INTERACTIVE 9): clock repaint, window open. */
enum { C_CLOCK, C_DRAW1, C_STAT, C_WAIT, C_DRAW2 };
static void compositor_stat(struct actor *a);
static void compositor_after_launch(struct actor *a) { a->phase=C_DRAW2; a->remaining=DRAW2_US; }
static void compositor_resume(struct actor *a)
{
    if (input_pending) { input_pending=false; launch_done=false; opening=true; a->phase=C_DRAW1; a->remaining=DRAW1_US; }
    else { a->phase=C_CLOCK; a->remaining=CLOCK_US; }
}
static void compositor_stat(struct actor *a)
{
    if (!drive_take(a,compositor_stat)) return;
    a->phase=C_STAT; a->remaining=HOLD_US;
}
static void compositor_done(struct actor *a)
{
    switch (a->phase) {
    case C_CLOCK: if (input_pending) { compositor_resume(a); return; } break;
    case C_DRAW1: wake(&launcher); compositor_stat(a); return;
    case C_STAT:
        drive_give(a);
        if (launch_done) { compositor_after_launch(a); return; }
        compositor_waiting=true; a->resume=compositor_after_launch; park(); return;
    case C_DRAW2: {
        uint64_t latency=now_us()-window_start;
        if (latency>window_max) window_max=latency;
        window_runs++; opening=false;
        break;
    }
    }
    uint32_t next=CLOCK_PERIOD_MS-(uint32_t)(g_ticks%CLOCK_PERIOD_MS);
    sleep_then(a,next,compositor_resume);
}

static void actor_add(struct task *t, const char *name, enum task_prio prio, uint32_t id,
                      void (*resume)(struct actor *), void (*done)(struct actor *))
{
    *t=(struct task){.id=id,.generation=1,.prio=prio,.state=T_BLOCKED,.user=prio!=P_DEVICE};
    strcpy(t->name,name);
    struct actor *a=&actors[actor_count++];
    *a=(struct actor){.t=t,.resume=resume,.done=done};
    t->all_next=idle.all_next; idle.all_next=t;
}
static void model_reset(bool share)
{
    reset(); desktop_share=share;
    normal.state=hog.state=ui.state=T_BLOCKED; idle.all_next=0;
    waiters=0; held=0; owned_queues=0; propagating=false; dependents=0;
    actor_count=0;
    memset(group_ms,0,sizeof(group_ms)); memset(vm_ms,0,sizeof(vm_ms));
    drive_holder=0; hold_start=hold_max=drive_waits=0;
    folder_max=folder_runs=window_max=window_runs=window_start=window_due=0;
    refill_last=refill_gap_max=vm_last_run=vm_gap_max=0;
    input_pending=launch_done=compositor_waiting=opening=false; inputs_skipped=0;
    frames=frame_start=frame_progress=frames_late=0;
    actor_add(&vm,"dosvm",P_NORMAL,54,vm_resume,vm_done);
    actor_add(&refill,"audio-refill",P_DEVICE,16,refill_resume,refill_done);
    actor_add(&work,"device-work",P_DEVICE,9,work_resume,work_done);
    actor_add(&compositor,"desktop",P_INTERACTIVE,27,compositor_resume,compositor_done);
    actor_add(&folder,"desktop",P_NORMAL,31,folder_resume,folder_done);
    actor_add(&launcher,"desktop",P_NORMAL,32,launcher_resume,launcher_done);
    actor_add(&files,"files",P_NORMAL,39,files_resume,files_done);
    actor_add(&net,"net-input",P_DEVICE,21,net_resume,net_done);
    task_set_audio_class(&refill);
    task_set_base(&vm,P_NORMAL,vm_level);
    audio_focus=vm_focused ? &vm : 0;
    task_set_share_group(&compositor,true); task_set_share_group(&folder,true);
    task_set_share_group(&launcher,true);
    task_start(&vm); task_start(&refill); task_start(&work); task_start(&net); task_start(&compositor);
    task_start(&folder);
    if (foreign) task_start(&files);
    schedule();
}
static void run_until(uint64_t end_tick)
{
    while (g_ticks<end_tick) {
        struct actor *a=actor_of(g_current);
        if (!a) { /* idle to the next tick */
            tick();
            if (g_need_resched || rq_any()) schedule();
            continue;
        }
        if (!a->remaining) {
            void (*resume)(struct actor *)=a->resume; a->resume=0;
            CHECK(resume); resume(a);
            continue;
        }
        uint64_t room=boundary()-cycles;
        uint64_t step=a->remaining<room ? a->remaining : room;
        if (step) consume(a,step);
        if (!a->remaining) { a->done(a); continue; }
        tick();
        kpreempt_point();
    }
}
/* Group/VM CPU in every 100 ms window after the first second. */
static void windows(unsigned *group_min, unsigned *group_max, unsigned *vm_min)
{
    *group_min=UINT32_MAX; *group_max=0; *vm_min=UINT32_MAX;
    for (unsigned at=1000;at+100<=RUN_MS;at++) {
        unsigned g=0,v=0;
        for (unsigned i=at;i<at+100;i++) { g+=group_ms[i]; v+=vm_ms[i]; }
        if (g<*group_min) *group_min=g;
        if (g>*group_max) *group_max=g;
        if (v<*vm_min) *vm_min=v;
    }
}

struct result {
    uint64_t folder_ms, window_ms, refill_gap_us, vm_gap_us, hold_ms;
    uint64_t vm_cpu, group_cpu, device_cpu, frames, late;
    unsigned inherits, share_turns, folders, windows, skipped;
    uint64_t waits, files_cpu, folder_raw, window_raw;
};
static struct result scenario(bool share, bool inherit_, bool cadence_, bool continuous_, unsigned work_us)
{
    inherit=inherit_; cadence=cadence_; continuous=continuous_; frame_work_us=work_us;
    model_reset(share);
    uint64_t base_cpu[8]={0};
    for (uint64_t at=1500;at<RUN_MS-500;at+=2000) {
        window_due=at;
        run_until(at+1);
        if (at==1500) for (unsigned i=0;i<actor_count;i++) base_cpu[i]=actors[i].cpu;
    }
    run_until(RUN_MS);
    struct result r={folder_max/1000,window_max/1000,refill_gap_max,vm_gap_max,hold_max/1000,
        actors[0].cpu-base_cpu[0],actors[3].cpu+actors[4].cpu+actors[5].cpu-base_cpu[3]-base_cpu[4]-base_cpu[5],
        actors[1].cpu+actors[2].cpu+actors[7].cpu-base_cpu[1]-base_cpu[2]-base_cpu[7],frames,frames_late,inherit_used,share_stats.turns,
        (unsigned)folder_runs,(unsigned)window_runs,inputs_skipped,drive_waits,actors[6].cpu,folder_max/1000,window_max/1000};
    /* An open still running at the end, or skipped, never met the gate. */
    if (!continuous && (opening || input_pending || r.skipped)) r.window_ms=RUN_MS;
    if (!continuous && folder_runs<RUN_MS/FOLDER_PERIOD_MS-1) r.folder_ms=RUN_MS;
    return r;
}

static void latency_gate(void)
{
    vm_level=9; vm_focused=true;
    struct result off=scenario(false,false,false,false,0);
    struct result boost=scenario(false,true,false,false,0);
    struct result on=scenario(true,true,false,false,0);
    struct result share_only=scenario(true,false,false,false,0);
    printf("F1-98 replay, focused VM at 9 busy, trial device load, %u s: folder compare max ms (runs), window open max ms (done/5), refill gap us, VM gap us\n",RUN_MS/1000);
    const struct { const char *name; const struct result *r; } rows[]={
        {"pre-f1-98 (negative control)",&off},{"holder boost only",&boost},
        {"desktop share only",&share_only},{"f1-98: share + holder boost",&on}};
    for (unsigned i=0;i<4;i++) {
        const struct result *r=rows[i].r;
        printf("  %-30s %6llu (%2u)  %6llu (%u/5)  %5llu  %5llu  turns=%u inherits=%u\n",rows[i].name,
            (unsigned long long)r->folder_raw,r->folders,(unsigned long long)r->window_raw,r->windows,
            (unsigned long long)r->refill_gap_us,(unsigned long long)r->vm_gap_us,r->share_turns,r->inherits);
    }
    /* The negative control reproduces the trial: the desktop workers wait
     * for their 100 ms starvation boosts. */
    CHECK(off.folder_ms>GATE_MS && off.window_ms>GATE_MS);
    CHECK(on.folder_ms<=GATE_MS && on.window_ms<=GATE_MS);
    CHECK(share_only.folder_ms<=GATE_MS && share_only.window_ms<=GATE_MS);
    /* Audio keeps its budget: the refill's service gap does not grow. */
    CHECK(on.refill_gap_us<=off.refill_gap_us);
    /* The share adds at most one banked turn and one tick to the VM's
     * longest wait; device bursts already set the rest. */
    CHECK(on.vm_gap_us<=off.vm_gap_us+SHARE_BURST_US+1000);
    CHECK(on.share_turns && boost.inherits);
    CHECK(on.windows==5 && !on.skipped && on.folders>=RUN_MS/FOLDER_PERIOD_MS-1);
    /* The VM keeps the rest: the desktop takes what it uses, no more. */
    double total=(double)(RUN_MS-1500)*1000;
    printf("  CPU after 1.5 s: VM %.1f%% -> %.1f%%, desktop %.1f%% -> %.1f%%, device %.1f%% -> %.1f%% PASS\n",
        100*off.vm_cpu/total,100*on.vm_cpu/total,100*off.group_cpu/total,100*on.group_cpu/total,
        100*off.device_cpu/total,100*on.device_cpu/total);

    /* A non-desktop holder of drive C: a member waiting for it lends the
     * holder its priority and its share (sync.c donation walk). */
    foreign=true;
    struct result foreign_off=scenario(false,true,false,false,0);
    struct result foreign_share=scenario(true,false,false,false,0);
    struct result foreign_on=scenario(true,true,false,false,0);
    foreign=false;
    printf("F1-98 replay VM=9 + Win32 ReadFile holding C every %u ms: folder/window max ms: pre-f1-98 %llu/%llu, share without holder edges %llu/%llu, f1-98 %llu/%llu (lent_turns=%u, drive waits %llu/%llu/%llu) PASS\n",
        FILES_PERIOD_MS,(unsigned long long)foreign_off.folder_ms,(unsigned long long)foreign_off.window_ms,
        (unsigned long long)foreign_share.folder_ms,(unsigned long long)foreign_share.window_ms,
        (unsigned long long)foreign_on.folder_ms,(unsigned long long)foreign_on.window_ms,share_stats.lent_turns,
        (unsigned long long)foreign_off.waits,(unsigned long long)foreign_share.waits,(unsigned long long)foreign_on.waits);
    printf("  waits by actor (f1-98): compositor=%llu folder=%llu launcher=%llu files=%llu\n",
        (unsigned long long)actors[3].waits,(unsigned long long)actors[4].waits,(unsigned long long)actors[5].waits,(unsigned long long)actors[6].waits);
    CHECK(foreign_on.folder_ms<=GATE_MS && foreign_on.window_ms<=GATE_MS && share_stats.lent_turns);
    /* Without the holder edges the share cannot reach a non-member holder. */
    CHECK(foreign_share.window_ms>GATE_MS && foreign_off.window_ms>GATE_MS);

    /* A Win32 game at HIGH_PRIORITY_CLASS (13), not a focused VM. */
    vm_level=13; vm_focused=false;
    struct result game_off=scenario(false,true,false,false,0);
    struct result game=scenario(true,true,false,false,0);
    printf("F1-98 replay Win32 game=13: folder_compare_max_ms %llu -> %llu, window_open_max_ms %llu -> %llu PASS\n",
        (unsigned long long)game_off.folder_ms,(unsigned long long)game.folder_ms,
        (unsigned long long)game_off.window_ms,(unsigned long long)game.window_ms);
    CHECK(game_off.folder_ms>GATE_MS && game.folder_ms<=GATE_MS && game.window_ms<=GATE_MS);
}

/* The share accounting: a member that never blocks, beside the busy VM. */
static void share_accounting(void)
{
    vm_level=9; vm_focused=true;
    scenario(true,true,false,true,0);
    unsigned gmin,gmax,vmin; windows(&gmin,&gmax,&vmin);
    /* group_ms is in 10 us units: 2400 = 24 ms */
    printf("F1-98 share accounting: desktop member always ready beside VM=9: desktop per 100 ms window %u.%02u..%u.%02u ms, VM >= %u.%02u ms, turns=%u PASS\n",
        gmin/100,gmin%100,gmax/100,gmax%100,vmin/100,vmin%100,share_stats.turns);
    /* 25 % of every window, give or take one tick-granular turn. */
    CHECK(gmin>=2350 && gmax<=2500+SHARE_BURST_US/10+100);
    CHECK(vmin>=5000); /* 100 - 25 - device (19 % in the model, one turn of slack) */
    record_bytes=0; records[0]=0; g_boot.flags=CBI_F_VERBOSE; cpu_emit();
    CHECK(strstr(records,"[sched] desktop_share group_pm="));
    g_boot.flags=0;
    scenario(false,true,false,true,0);
    windows(&gmin,&gmax,&vmin);
    printf("F1-98 share accounting negative control (pre-f1-98): desktop per 100 ms window %u.%02u..%u.%02u ms PASS\n",
        gmin/100,gmin%100,gmax/100,gmax%100);
    CHECK(gmin<=300); /* the starvation boost alone: about 1 ms per 100 ms */
}

/* VM frame cadence with the share in place. */
static double cadence_row(const char *what, unsigned work_us, bool sync, bool saturated, bool gate)
{
    vsync=sync;
    struct result off=scenario(false,true,true,saturated,work_us);
    struct result on=scenario(true,true,true,saturated,work_us);
    double slower=100.0*((double)off.frames-(double)on.frames)/(double)off.frames;
    printf("F1-98 VM cadence (%s): frame work %u us (%u %% of the CPU at 70 Hz), %s desktop: frames %llu -> %llu, %.2f%% slower, late %llu -> %llu%s\n",
        what,work_us,work_us*100/FRAME_PERIOD_US,saturated ? "saturated" : "trial",
        (unsigned long long)off.frames,(unsigned long long)on.frames,slower,
        (unsigned long long)off.late,(unsigned long long)on.late,gate ? " PASS" : " (informational)");
    if (gate) CHECK(on.frames*100>=off.frames*95);
    return slower;
}
static void vm_cadence(void)
{
    vm_level=9; vm_focused=true;
    static const unsigned works[]={6000,7000,8000};
    for (unsigned i=0;i<sizeof(works)/sizeof(*works);i++) cadence_row("vsync 70 Hz",works[i],true,false,true);
    cadence_row("free-running",8000,false,false,true);
    /* Beyond the gate: a game needing 63 % of the CPU per 70 Hz frame loses
     * the retraces that fall in the desktop's bursts; a desktop using its
     * whole share costs a CPU-bound game that share. */
    cadence_row("vsync 70 Hz",9000,true,false,false);
    cadence_row("vsync 70 Hz",8000,true,true,false);
    double lost=cadence_row("free-running",8000,false,true,false);
    CHECK(lost<=25.0*100/75+1); /* never more than the share of what the VM had */
}

int main(void)
{
    g_tsc_per_ms=1000;
    latency_gate();
    share_accounting();
    vm_cadence();
    return 0;
}
