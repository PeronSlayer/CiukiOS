/* F3-52: VM producer dependency, production scheduler/sync, fake ticks.
 * Included by scheduler_priorities_test.c. SPDX-License-Identifier: GPL-2.0-only */
static struct kwait frame_wait;
static bool frame_ready;
static unsigned frame_scenario;
static bool frame_condition(void *arg) { (void)arg; return frame_ready; }
static void frame_wait_hook(void)
{
    uint32_t saved=flags; flags=0x200;
    CHECK(g_current==&normal && hog.state==T_BLOCKED);
    CHECK(normal.base_priority==7 && normal.effective_priority==11 && normal.inherited_priority==11);
    CHECK(hog.waiting_on==&frame_wait);
    unsigned wakes=scheduler_stats.wakeups;
    for (unsigned i=0;i<100;i++) kwait_wake_ready(&frame_wait);
    CHECK(hog.state==T_BLOCKED && normal.effective_priority==11 && scheduler_stats.wakeups==wakes);
    if (frame_scenario==3) {
        ksync_task_exit(&hog); hog.state=T_ZOMBIE;
        CHECK(normal.effective_priority==7 && !waiters && !frame_wait.head);
        latency_switch_hook=0; flags=saved; longjmp(cancelled,1);
    }
    if (frame_scenario==4) {
        /* Producer ownership contributes no hold. Its death must not
         * consume an unrelated transaction's preemption hold. */
        frame_ready=true; normal.preempt_holds=1;
        ksync_task_exit(&normal); normal.state=T_ZOMBIE;
        CHECK(normal.preempt_holds==1 && !frame_wait.owner && hog.state==T_READY);
        CHECK(!normal.inherited_priority && !hog.waiting_on);
        normal.preempt_holds=0;
        latency_switch_hook=0; schedule(); CHECK(g_current==&hog);
        flags=saved; return;
    }
    /* Expiry propagates through the request edge, including a blocked UI. */
    for (unsigned i=0;i<20;i++) run_tick(0);
    CHECK(normal.effective_priority==9 && hog.effective_priority==9);
    if (frame_scenario==1) {
        for (unsigned i=20;i<50;i++) run_tick(0);
        CHECK(hog.state==T_READY && !normal.inherited_priority);
    } else {
        frame_ready=true; /* frame available, or caught-signal predicate */
        kwait_wake_ready(&frame_wait);
        CHECK(hog.state==T_READY && normal.effective_priority==7);
        CHECK(!hog.io_priority && !scheduler_stats.io_granted);
    }
    latency_switch_hook=0; park(); CHECK(g_current==&hog);
    flags=saved;
}
static void dosvm_wait_dependency_test(void)
{
    for (frame_scenario=0;frame_scenario<5;frame_scenario++) {
        priority_reset(); kwait_init(&frame_wait); frame_ready=false;
        frame_wait.owner_preemptible=true;
        task_start(&normal); task_set_base(&ui,P_NORMAL,8); task_start(&ui);
        running(&hog); task_set_class(&hog,P_NORMAL); task_focus(&hog); task_input_delivered(&hog);
        CHECK(hog.base_priority==9 && hog.effective_priority==11);
        kwait_set_owner(&frame_wait,&normal); latency_switch_hook=frame_wait_hook;
        if (!setjmp(cancelled)) {
            bool ready=kwait_wait_until(&frame_wait,frame_condition,0,50);
            CHECK(ready==(frame_scenario!=1));
        } else CHECK(frame_scenario==3);
        kwait_set_owner(&frame_wait,0);
        CHECK(!waiters && !owned_queues && !normal.inherited_priority && !hog.waiting_on);
    }
    puts("F3-52 VM wait donation: base7 <- focused9/input11; expiry ->9; false notifications stay asleep; wake/timeout/signal/waiter or producer death withdraw; producer retires no lock hold; no I/O boost PASS");
}

static unsigned frame_load_vm, frame_load_desktop, frame_load_device, frame_load_max_ready;
static bool frame_load_boost;
static void frame_load_hook(void)
{
    uint32_t saved=flags; flags=0x200;
    latency_switch_hook=0; /* This hook drives real dispatches, without recursion. */
    uint64_t start=g_ticks, desktop_ready=g_ticks;
    CHECK(middle.state==T_BLOCKED && middle.waiting_on==&frame_wait);
    CHECK(normal.inherited_priority==(frame_load_boost ? 11u : 9u));
    bool notified=false;
    for (unsigned turns=0;g_current!=&middle;turns++) {
        CHECK(turns<100 && g_ticks-start<50);
        struct task *ran=g_current;
        if (ran==&normal) frame_load_vm++;
        if (ran==&ui) {
            unsigned latency=(unsigned)(g_ticks-desktop_ready);
            if (latency>frame_load_max_ready) frame_load_max_ready=latency;
            frame_load_desktop++;
        }
        if (ran==&hog) frame_load_device++;
        run_tick(0);
        CHECK(device_used<=4 && device_cpu_total<=4*g_tsc_per_ms);
        if (!notified && g_ticks-start>=30 && ran==&normal) {
            frame_ready=true; kwait_wake_ready(&frame_wait); notified=true;
            CHECK(!normal.inherited_priority && middle.state==T_READY);
        }
        if (ran==&hog) { task_device_deadline(&hog,g_ticks+3); task_sleep_ms(2); }
        else if (ran==&ui) { desktop_ready=g_ticks+1; task_sleep_ms(1); }
        else kpreempt_point();
    }
    CHECK(notified); flags=saved;
}
static void dosvm_wait_combined_load(void)
{
    for (unsigned boost=0;boost<2;boost++) {
        priority_reset(); frame_load_boost=boost!=0;
        frame_load_vm=frame_load_desktop=frame_load_device=frame_load_max_ready=0;
        normal.user=false; strcpy(normal.name,"dosvm"); strcpy(middle.name,"dosbox");
        task_set_base(&ui,P_NORMAL,9); /* Desktop remains at priority 9. */
        task_focus(&middle); audio_focus=&normal;
        task_start(&normal); task_start(&ui); task_start(&hog);
        running(&middle); kwait_init(&frame_wait); frame_wait.owner_preemptible=true;
        for (unsigned frame=0;frame<30;frame++) {
            frame_ready=false;
            if (boost) task_input_delivered(&middle);
            kwait_set_owner(&frame_wait,&normal); latency_switch_hook=frame_load_hook;
            CHECK(kwait_wait_until(&frame_wait,frame_condition,0,g_ticks+50));
            CHECK(g_current==&middle && !normal.inherited_priority && !middle.waiting_on);
            kwait_set_owner(&frame_wait,0);
        }
        /* Eight ordinary focus ticks can straddle two admitted 4-ms device
         * windows (16 ms total). A donated input boost adds its existing
         * 20-ms expiry. This is a combined-load bound, not the device-free
         * F3-41 fixture's 8-ms ready delay. No scheduler policy changes. */
        CHECK(frame_load_max_ready<=(boost ? 36u : 16u));
        CHECK(frame_load_vm && frame_load_desktop>=30 && frame_load_device>=30);
        CHECK(!scheduler_stats.audio_urgent && !scheduler_stats.io_granted && ms(stretch_max)<=2);
        printf("F3-52 combined donation=%u focused_vm=1 desktop_priority=9 frames=30 desktop_max_ready_ms=%u vm_ticks=%u desktop_turns=%u device_turns=%u device_max=4/10 urgent=0 stretch_ms=%u PASS\n",
               boost ? 11u : 9u,frame_load_max_ready,frame_load_vm,frame_load_desktop,frame_load_device,ms(stretch_max));
        CHECK(!waiters && !owned_queues);
    }
    /* The presenter makes no calls for 300 ms. Its VM retains base 9 and
     * competes with the busy desktop and admitted device work without a
     * live WAIT donation. Measure pending-IRQ service opportunities. */
    priority_reset(); normal.user=false;
    task_set_base(&normal,P_NORMAL,9); task_set_base(&ui,P_NORMAL,9);
    audio_focus=irq_focus=&normal;
    task_start(&normal); task_start(&hog); running(&ui); flags=0x200;
    uint64_t last=g_ticks; unsigned gap=0,vm=0,desktop=0;
    for (unsigned t=0;t<300;t++) {
        if (g_current==&normal) {
            unsigned wait=(unsigned)(g_ticks-last); if (wait>gap) gap=wait;
            last=g_ticks; vm++;
        }
        if (g_current==&ui) desktop++;
        run_tick(0); kpreempt_point();
    }
    CHECK(vm && desktop && gap<=33 && g_ticks-last<=33 && !normal.inherited_priority);
    printf("F1-87 presenter absent=300ms busy desktop/device load, VM base=9 donation=0 IRQ opportunity_max_ms=%u PASS\n",gap);
}
