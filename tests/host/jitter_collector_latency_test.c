/* f6-21: the jitter collector's production turn policy (src/kernel/core/
 * entropy.c, extracted by test_kernel_latency.py into entropy_turns.inc) on
 * the production scheduler and CPU sampler of kernel_latency_test.c.
 * Time model: one cycle = 1 us, a PIT tick every 1000 cycles, IRQ work free.
 * The collector's glue mirrors jitter_boundary(): pause (sleep one tick)
 * when turns_round() says so, else a preemption point, and turns_resume()
 * at the moment the collector is dispatched again.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main kernel_latency_fixture_main
#include "kernel_latency_test.c"
#undef main
#include "entropy_turns.inc"

enum { SPIN_A, SPIN_B, DRAIN, JIT, REFILL, DESK, TASKS };
static struct task tk[TASKS];
static uint64_t next_tick, left[TASKS], woke_at[TASKS], latency_max[TASKS];
/* Device-driven tasks (refill: AC'97 buffer IRQ; desktop: frame/input IRQ)
 * block and are woken by an interrupt between PIT ticks. */
static uint64_t irq_period[TASKS], next_irq[TASKS];
static unsigned irq_missed;
static bool asleep[TASKS], sleep_due[TASKS];
static struct entropy_turns jt;
static bool jit_open, jit_resume, jit_legacy, spinning;
static uint64_t jit_round, jit_long_round, jit_rounds, jit_rounds_spinning, jit_cycles;
static uint64_t jit_since, jit_run_max, spin_tick;
static unsigned jit_violations;

static void sim_tick(void)
{
    g_ticks++;
    struct trap_frame tf={.vector=0x20,.cs=g_current->user ? 0x1b : 8,.eip=0x6216};
    uint64_t start=cpu_irq_enter(&tf);sched_tick();cpu_irq_leave(start);
    uint32_t saved=flags;flags=0x200;
    for(unsigned n=0;deferred_fn && n<1024;n++){kwork_fn fn=deferred_fn;deferred_fn=0;fn(deferred_arg);}
    /* The probe's two spinners become ready at an IRQ, inside whatever runs. */
    if(spin_tick && g_ticks==spin_tick){task_start(&tk[SPIN_A]);task_start(&tk[SPIN_B]);spinning=true;}
    flags=saved;
}
static void device_irq(unsigned i)
{
    struct trap_frame tf={.vector=0x2b,.cs=g_current->user ? 0x1b : 8,.eip=0x6216};
    uint64_t start=cpu_irq_enter(&tf);
    if(tk[i].state==T_BLOCKED){woke_at[i]=cycles;task_start(&tk[i]);}
    else irq_missed++;                            /* the previous period's work is unfinished */
    cpu_irq_leave(start);
}
static uint64_t next_event(void)
{
    uint64_t event=next_tick;
    for(unsigned i=0;i<TASKS;i++)if(irq_period[i] && next_irq[i]<event)event=next_irq[i];
    return event;
}
/* Run the current task for up to `work` cycles. A user task is preempted at
 * the return of an interrupt that requested a reschedule, and the idle task
 * leaves HLT at every interrupt; kernel code continues to its next point. */
static uint64_t run(uint64_t work)
{
    struct task *self=g_current;
    while(work){
        uint64_t step=next_event()-cycles;if(step>work)step=work;
        cycles+=step;work-=step;
        bool interrupted=false;
        for(unsigned i=0;i<TASKS;i++)if(irq_period[i] && cycles==next_irq[i]){next_irq[i]+=irq_period[i];device_irq(i);interrupted=true;}
        if(cycles==next_tick){next_tick+=1000;sim_tick();interrupted=true;}
        if(interrupted && self==&idle)return work;
        if(interrupted && self->user && g_need_resched){schedule();return work;}
    }
    return 0;
}
static void block_for_irq(unsigned i)
{
    uint32_t f=irq_save();g_current->state=T_BLOCKED;irq_restore(f);
    asleep[i]=true;sleep_due[i]=false;schedule();
}
static void sleep_for(unsigned i,uint32_t ms)
{
    asleep[i]=true;sleep_due[i]=false;woke_at[i]=(g_ticks+ms)*1000;
    task_sleep_ms(ms);
}
static bool higher_ready(void)
{
    for(unsigned p=1;p<SCHED_LEVELS;p++)if(rq_head[p])return true;
    return false;
}
static void jit_step(void)
{
    struct task *self=&tk[JIT];
    if(!jit_open){turns_open(&jt,cycles,self->dispatches);jit_open=true;}
    if(jit_resume){turns_resume(&jt,cycles,self->dispatches);jit_resume=false;}
    if(!jit_legacy){
        /* Never a round while anything else is runnable; never boosted. */
        if(higher_ready())jit_violations++;
        CHECK(self->prio==P_IDLE && self->effective_priority==0 && !self->boosted);
    }
    if(spinning)jit_rounds_spinning++;
    uint64_t cost=jit_long_round && spinning && jit_rounds_spinning==1 ? jit_long_round : jit_round;
    run(cost);jit_rounds++;jit_cycles+=cost;
    if(jit_legacy){kpreempt_point();return;}      /* f6-19: a preemption point per round only */
    uint32_t seen=self->dispatches;
    if(turns_round(&jt,cycles,seen)==TURN_PAUSE)task_sleep_ms(1);
    else kpreempt_point();
    if(g_current!=self)jit_resume=true;
}
static void simulate(uint64_t until)
{
    struct task *last=g_current;
    while(g_ticks<until){
        struct task *t=g_current;
        if(t!=last){
            if(last==&tk[JIT] && cycles-jit_since>jit_run_max)jit_run_max=cycles-jit_since;
            if(t==&tk[JIT])jit_since=cycles;
            last=t;
        }
        if(t==&idle){run(next_event()-cycles);if(g_need_resched || rq_any())schedule();continue;}
        unsigned i=(unsigned)(t-tk);
        CHECK(i<TASKS);
        if(asleep[i]){
            asleep[i]=false;
            if(cycles-woke_at[i]>latency_max[i])latency_max[i]=cycles-woke_at[i];
        }
        if(sleep_due[i]){block_for_irq(i);continue;}
        switch(i){
        case SPIN_A: case SPIN_B: run(next_tick-cycles);break;
        case DRAIN: run(20);sleep_for(DRAIN,10);break;
        case JIT: jit_step();break;
        case REFILL: case DESK:
            if(!left[i])left[i]=i==REFILL ? 400 : 2000;
            left[i]=run(left[i]);
            if(!left[i]){if(g_current==t)block_for_irq(i);else sleep_due[i]=true;}
            break;
        }
    }
}
static void setup(enum task_prio jit_class,uint64_t round)
{
    reset();
    static const char *const names[TASKS]={"spin-a","spin-b","entropy","jitter","audio-refill","desktop"};
    static const enum task_prio classes[TASKS]={P_NORMAL,P_NORMAL,P_NORMAL,P_IDLE,P_DEVICE,P_INTERACTIVE};
    for(unsigned i=0;i<TASKS;i++){
        tk[i]=(struct task){.id=20+i,.generation=1,.prio=i==JIT ? jit_class : classes[i],.state=T_BLOCKED,
                            .user=i==SPIN_A || i==SPIN_B || i==DESK};
        snprintf(tk[i].name,sizeof(tk[i].name),"%s",names[i]);
        tk[i].all_next=i+1<TASKS ? &tk[i+1] : 0;
        left[i]=woke_at[i]=latency_max[i]=irq_period[i]=next_irq[i]=0;asleep[i]=sleep_due[i]=false;
    }
    irq_missed=0;
    idle.all_next=&tk[0];
    task_set_audio_class(&tk[REFILL]);
    memset(&jt,0,sizeof(jt));jt.bound=1000*ENTROPY_TURN_US/1000;
    jit_open=jit_resume=spinning=false;jit_legacy=jit_class!=P_IDLE;
    jit_round=round;jit_long_round=0;jit_rounds=jit_rounds_spinning=jit_cycles=0;
    jit_since=jit_run_max=0;jit_violations=0;spin_tick=0;next_tick=1000;
}
/* The QEMU f0-core preempt probe's shape: two always-ready P_NORMAL spinners
 * at a 1-tick quantum beside the drain worker and the collector. */
static void preempt_case(bool legacy)
{
    setup(legacy ? P_NORMAL : P_IDLE,127);
    g_quantum_ticks=1;
    task_start(&tk[DRAIN]);task_start(&tk[JIT]);schedule();
    spin_tick=50;jit_long_round=57000;       /* one 57 ms stretch, as in the failed run */
    simulate(5050);
    g_quantum_ticks=10;
    uint64_t wait=tk[SPIN_A].max_wait_ticks>tk[SPIN_B].max_wait_ticks ? tk[SPIN_A].max_wait_ticks : tk[SPIN_B].max_wait_ticks;
    if(legacy){
        CHECK(wait>=50 && jit_rounds_spinning>0);
        printf("f6-21 negative control (f6-19 worker: P_NORMAL, a point per round): spinner max_wait=%llu ticks\n",
               (unsigned long long)wait);
        return;
    }
    CHECK(wait<=2 && tk[SPIN_A].dispatches>=2000 && tk[SPIN_B].dispatches>=2000);
    CHECK(jit_rounds>100 && !jit_rounds_spinning && !jit_violations && !g_starvation_boosts && !scheduler_stats.starvation_granted);
    printf("f6-21 preempt shape: collector rounds before the spinners=%llu, while they run=0, spinner max_wait=%llu/%llu ticks dispatches=%u/%u, no boost PASS\n",
           (unsigned long long)jit_rounds,(unsigned long long)tk[SPIN_A].max_wait_ticks,(unsigned long long)tk[SPIN_B].max_wait_ticks,
           tk[SPIN_A].dispatches,tk[SPIN_B].dispatches);
}
/* The T23 desktop: the AC'97 refill every 10 ms (0.4 ms), a 2 ms desktop
 * frame every 16 ms, both woken by interrupts that land inside collector
 * turns; the drain worker every 10 ms; the collector on the rest. */
static void desktop_case(uint64_t round)
{
    setup(P_IDLE,round);
    irq_period[REFILL]=10000;next_irq[REFILL]=10437;   /* off the PIT ticks */
    irq_period[DESK]=16000;next_irq[DESK]=16731;
    task_start(&tk[DRAIN]);task_start(&tk[JIT]);
    schedule();
    simulate(3000);
    unsigned audio_events=audio_used;
    g_boot.flags=CBI_F_VERBOSE;record_bytes=0;records[0]=0;cpu_emit();g_boot.flags=0;
    CHECK(!audio_events && !irq_missed && !strstr(records,"audio_refill_overrun") && !strstr(records,"device_overrun"));
    CHECK(!jit_violations && !g_starvation_boosts && !scheduler_stats.starvation_granted);
    CHECK(latency_max[REFILL]<=round && latency_max[DESK]<=round+400+20);
    uint64_t share=jit_cycles*1000/cycles;
    if(round<=1000){
        CHECK(jit_run_max<=1000 && jt.turn_max<=1000 && share>=500);
    }else{
        CHECK(jit_run_max<=round && jt.turn_max==round && jt.pauses+1>=jt.turns);   /* one round per turn */
    }
    printf("f6-21 T23 desktop, %llu us rounds: refill max_latency=%lluus desktop=%lluus audio_refill_overrun=0, collector share=%llu permille turn_max=%lluus run_max=%lluus turns=%u pauses=%u PASS\n",
           (unsigned long long)round,(unsigned long long)latency_max[REFILL],(unsigned long long)latency_max[DESK],
           (unsigned long long)share,(unsigned long long)jt.turn_max,(unsigned long long)jit_run_max,jt.turns,jt.pauses);
}
int main(void)
{
    preempt_case(true);
    preempt_case(false);
    desktop_case(127);    /* T23: 2644 rounds in 335 ms */
    desktop_case(1500);   /* a QEMU TCG-like round longer than a turn */
    puts("f6-21 jitter collector: idle class never runs beside runnable work, never boosted, 1 ms turns, zero audio refill overruns PASS");
    return 0;
}
