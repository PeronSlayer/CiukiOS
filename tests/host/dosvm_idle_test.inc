/* f3-17: production monitor trap/IVT, waits, event races and focus policy.
 * Fake time/scheduling are transport seams, not CPU-share evidence.
 * SPDX-License-Identifier: GPL-2.0-only */
static struct dosvm *idle_vm;
static unsigned idle_action;
static uint64_t idle_at;
static void idle_test_event(void)
{
    struct dosvm *v = idle_vm;
    CHECK(v->idle.sleeping && v->info.state == DOSVM_BLOCKED);
    CHECK(!dosvm_task_focus() && dosvm_task_quantum(v->task, 10) == 1);
    if (g_ticks < idle_at) return;
    sleep_hook = wait_register_hook = 0;
    switch (idle_action) {
    case 0: CHECK(dosvm_keyboard(v->info.key, 0x1E61)); break;
    case 1: CHECK(dosvm_irq_raise(v->info.key, 7)); break;
    case 2: {
        struct ciuki_input_event e = {.type=CIUKI_INPUT_MOTION, .value=15, .value2=20};
        CHECK(dosvm_input_event(v->info.key, &e)); break;
    }
    case 3: dosvm_idle_wake(v); break; /* presentation notification */
    case 4: CHECK(!dosvm_stop(v->info.key, DOSVM_TERMINATE)); break;
    }
}
static struct dosvm *idle_test_vm(void)
{
    struct dosvm *v = create();
    v->info.dos_ready = v->started = true; v->info.state = DOSVM_RUNNABLE;
    v->ticks = g_ticks; v->monitor.vif = true;
    v->owner_pid = 10; CHECK(dosvm_input_focus(v->info.key, true));
    uint8_t iret = 0xCF;
    CHECK(!dosvm_write(v, IRET_SEG << 4, &iret, 1));
    idle_vm = v;
    return v;
}
static void test_idle_events(void)
{
    for (unsigned action = 0; action < 5; action++) {
        struct dosvm *v = idle_test_vm();
        CHECK(dosvm_task_focus() == v->task && dosvm_task_quantum(v->task, 10) == 8);
        uint64_t start = g_ticks;
        idle_action = action; idle_at = start + 3; sleep_hook = idle_test_event;
        unsigned waits = wait_calls;
        idle_wait(v, DOSVM_IDLE_2F);
        CHECK(g_ticks == start + 3 && wait_calls == waits + 1 && wait_deadline <= start + 55);
        CHECK(v->idle.blocks == 1 && v->idle.reasons == 1 && v->idle.count[DOSVM_IDLE_2F] == 1);
        CHECK(!v->idle.sleeping && !dosvm_task_focus());
        if (action != 4) {
            g_ticks += DOSVM_IDLE_WINDOW;
            CHECK(dosvm_task_focus() == v->task && dosvm_task_quantum(v->task, 10) == 8);
            CHECK(dosvm_input_focus(v->info.key, false) && dosvm_task_quantum(v->task, 10) == 4);
        }
        destroy(v, true); empty();
    }
    /* An event after the initial test but before kwait registration cannot
     * be lost even when it has no pending PIC bit (presentation/mouse). */
    struct dosvm *v = idle_test_vm();
    idle_action = 3; idle_at = g_ticks; wait_register_hook = idle_test_event;
    uint64_t start = g_ticks;
    idle_wait(v, DOSVM_IDLE_28);
    CHECK(g_ticks == start && v->idle.blocks == 1);
    destroy(v, true); empty();
    puts("f3-17 idle events: keyboard, mouse, device IRQ, presentation, stop and wait-registration race PASS");
}
static void test_idle_reflection(void)
{
    struct dosvm *v = idle_test_vm(); v86_start();
    /* Keep the actual IVT callout: a guest handler runs after yielding. */
    uint16_t hook[] = {0, IRET_SEG};
    CHECK(!dosvm_write(v, 0x28 * 4, hook, sizeof(hook)));
    idle_action = 3; idle_at = g_ticks + 2; sleep_hook = idle_test_event;
    v86_int(v, 0x28);
    CHECK(cur.tf.cs == IRET_SEG && !v->monitor.vif && v->idle.count[DOSVM_IDLE_28] == 1);
    step(v, 13, 0); CHECK(v->monitor.vif);
    idle_action = 3; idle_at = g_ticks + 2; sleep_hook = idle_test_event;
    cur.tf.eax = 0x1680; rom_int(v, 0x2F);
    CHECK((cur.tf.eax & 255) == 0 && v->idle.count[DOSVM_IDLE_2F] == 1);
    /* Default INT 28h must not mutate AX or condition codes. */
    struct v86_frame f = vf(0x1234, 0, 0, 0); f.tf.eflags |= V86_CF | V86_ZF;
    struct v86_frame before = f;
    CHECK(!dosvm_bios(v, &f, 0x28) && !memcmp(&before, &f, sizeof(f)));
    /* IF clear, interrupt shadows and guest IRQ service are not heuristics
     * for idleness. Nor is a game that executes ordinary timer traps. */
    uint32_t blocks = v->idle.blocks;
    v->monitor.vif = false; cur.tf.eax = 0x1680; v86_int(v, 0x2F);
    CHECK(v->idle.blocks == blocks);
    v->monitor.vif = true; v->monitor.shadow = true;
    CHECK(!idle_safe(v)); v->monitor.shadow = false;
    v->monitor.pic[0].isr = 1; CHECK(!idle_safe(v)); v->monitor.pic[0].isr = 0;
    v86_start();
    for (unsigned i = 0; i < 20; i++) step(v, 0x20, 0);
    CHECK(v->idle.blocks == blocks);
    destroy(v, true); empty();
    puts("f3-17 idle reflection: IVT/TSR handlers retained, 1680 success, INT28 registers, IF/shadow/ISR and busy guest PASS");
}
static void test_idle_keyboard(void)
{
    struct dosvm *v = idle_test_vm(); v86_start();
    v->monitor.pic[0].imr = 0xFF; /* isolate poll detection from IRQ injection */
    for (unsigned i = 0; i < DOSVM_IDLE_POLLS - 1; i++) {
        cur.tf.eax = i & 1 ? 0x1100 : 0x0100; rom_int(v, 0x16);
        CHECK((cur.tf.eflags & V86_ZF) && !v->idle.blocks);
    }
    idle_action = 3; idle_at = g_ticks + 2; sleep_hook = idle_test_event;
    cur.tf.eax = 0x0100; rom_int(v, 0x16);
    CHECK(v->idle.blocks == 1 && v->idle.count[DOSVM_IDLE_16] == 1);
    /* A key-bearing peek keeps the key; AH=00 consumes it, never sleeps. */
    CHECK(dosvm_keyboard(v->info.key, 0x3062));
    cur.tf.eax = 0x1100; rom_int(v, 0x16);
    CHECK(!(cur.tf.eflags & V86_ZF) && (uint16_t)cur.tf.eax == 0x3062 && v->key_count == 1 && !v->idle.polls);
    cur.tf.eax = 0; rom_int(v, 0x16);
    CHECK((uint16_t)cur.tf.eax == 0x3062 && !v->key_count && v->idle.blocks == 1);
    /* Sparse empty peeks, separated by host ticks, never cross threshold. */
    for (unsigned i = 0; i < DOSVM_IDLE_POLLS + 2; i++) {
        g_ticks++; cur.tf.eax = 0x0100; rom_int(v, 0x16);
    }
    CHECK(v->idle.blocks == 1);
    /* A hooked keyboard ISR's final ZF is observed at its caller's return,
     * without replacing the guest vector with the synthetic BIOS. */
    uint16_t hook[] = {0, IRET_SEG}; CHECK(!dosvm_write(v, 0x16 * 4, hook, sizeof(hook)));
    v->idle.polls = 0;
    for (unsigned i = 0; i < DOSVM_IDLE_POLLS; i++) {
        if (i == DOSVM_IDLE_POLLS - 1) { idle_action = 3; idle_at = g_ticks + 2; sleep_hook = idle_test_event; }
        cur.tf.eax = 0x1100; cur.tf.eflags |= V86_ZF;
        v86_int(v, 0x16); CHECK(cur.tf.cs == IRET_SEG); step(v, 13, 0);
    }
    CHECK(v->idle.blocks == 2);
    destroy(v, true); empty();
    /* Blocking reads retry the firmware entry after a timer edge. This
     * lets IRQ0 run instead of pinning its IRR in a native keyboard loop. */
    for (unsigned fn = 0; fn <= 0x10; fn += 0x10) {
        v = idle_test_vm();
        struct v86_frame f = vf(fn << 8, 0, 0, 0); f.tf.eip = 0x162;
        uint64_t start = g_ticks;
        CHECK(!dosvm_bios(v, &f, 0x16));
        CHECK(g_ticks > start && g_ticks - start <= 55 && f.tf.eip == 0x160 && v->monitor.vif);
        CHECK(v->idle.blocks == 1 && v->monitor.pic[0].irr & 1);
        destroy(v, true); empty();
    }
    puts("f3-17 keyboard: 00/10 waits, 01/11 empty-return threshold, hooked vectors, successful keys and sparse polls PASS");
}
static void test_idle_timer_halt(void)
{
    struct dosvm *v = idle_test_vm();
    uint64_t start = g_ticks;
    idle_wait(v, DOSVM_IDLE_28);
    CHECK(g_ticks - start == 55 && v->pit.edges == 1 && v->monitor.pic[0].irr & 1);
    v->monitor.pic[0].irr = 0;
    device_out(v, 0x43, 0x36); device_out(v, 0x40, 0xA9); device_out(v, 0x40, 4); /* 1193 = 1ms */
    CHECK(idle_deadline(v) - g_ticks <= 2);
    start = g_ticks; idle_wait(v, DOSVM_IDLE_28);
    CHECK(g_ticks - start <= 2 && v->pit.edges >= 2);
    destroy(v, true); empty();
    /* Execute a real HLT trap and deliver the timer's real IVT frame. */
    v = idle_test_vm(); v86_start(); start = g_ticks;
    op1(v, 0xF4);
    CHECK(!left && g_ticks - start == 55 && v->idle.count[DOSVM_IDLE_HLT] == 1 &&
          v->monitor.stats.delivered[0] == 1 && v->monitor.state == V86_RUNNING);
    destroy(v, true); empty();
    puts("f3-17 timers/HLT: PIT-bounded sleep, reprogrammed 1kHz PIT, actual HLT trap/IRQ0 injection PASS");
}
static void idle_pm_rom(struct dosvm *v, uint8_t vector)
{
    pm_int(v, vector);
    CHECK(!in_pm() && cur.tf.cs == 0xF000 && cur.tf.eip == (uint32_t)vector * 16);
    step(v, 13, 0); step(v, 13, 0);
    CHECK(cur.tf.cs == 0xF000 && cur.tf.eip == DPMI_RM_RETURN);
    step(v, 13, 0); CHECK(in_pm());
}
static void test_idle_pm_reflection(void)
{
    for (unsigned bits = 0; bits < 2; bits++) {
        struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, bits != 0);
        idle_vm = v; v->owner_pid = 10; CHECK(dosvm_input_focus(v->info.key, true));
        idle_action = 3; idle_at = g_ticks + 2; sleep_hook = idle_test_event;
        cur.tf.eax = 0x1680; idle_pm_rom(v, 0x2F);
        CHECK((cur.tf.eax & 255) == 0 && v->idle.count[DOSVM_IDLE_2F] == 1);
        idle_action = 3; idle_at = g_ticks + 2; sleep_hook = idle_test_event;
        idle_pm_rom(v, 0x28);
        CHECK(v->idle.count[DOSVM_IDLE_28] == 1);
        for (unsigned i = 0; i < DOSVM_IDLE_POLLS; i++) {
            if (i == DOSVM_IDLE_POLLS - 1) { idle_action = 3; idle_at = g_ticks + 2; sleep_hook = idle_test_event; }
            cur.tf.eax = i & 1 ? 0x1100 : 0x0100; idle_pm_rom(v, 0x16);
            CHECK(cur.tf.eflags & V86_ZF);
        }
        CHECK(v->idle.count[DOSVM_IDLE_16] == 1 && !v->idle.depth && !v->dpmi.depth);
        destroy(v, true); empty();
    }
    puts("f3-17 PM16/32: committed default INT2F/INT28/INT16 reflections and caller return detection PASS");
}
static void test_idle_launch_return(void)
{
    struct dosvm *v = idle_test_vm(); v86_start();
    v->launch_armed = v->launch_enter_consumed = true;
    idle_action = 3; idle_at = g_ticks + 2; sleep_hook = idle_test_event;
    v86_int(v, 0x28);
    CHECK(v->launch_failed && !v->launch_exec && !v->launch_ready);
    destroy(v, true); empty();
    v = idle_test_vm(); v86_start();
    v->launch_armed = v->launch_enter_consumed = v->launch_exec = v->launch_ready = true;
    idle_action = 3; idle_at = g_ticks + 2; sleep_hook = idle_test_event;
    v86_int(v, 0x28);
    CHECK(!v->launch_failed && v->launch_ready);
    destroy(v, true); empty();
    puts("f3-17 launch: cooperative prompt still detects failed start, a ready program stays ready PASS");
}
static void test_idle(void)
{
    test_idle_events(); test_idle_reflection(); test_idle_keyboard(); test_idle_timer_halt();
    test_idle_pm_reflection();
    test_idle_launch_return();
    idle_vm = 0;
}
