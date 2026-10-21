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
