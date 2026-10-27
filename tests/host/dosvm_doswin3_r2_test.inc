/* fx-doswin3 round 2: lean V86 CLI/STI against the ordinary path, the
 * page-flip presenter wake and the presenter dispatch yield, with the
 * production monitor, client and presenter. No guest image or game payload.
 * Included by dosvm_test.c before dosvm_doswin3_test.inc (which runs it).
 * SPDX-License-Identifier: GPL-2.0-only */
struct r2_state { uint32_t eip, eflags; bool vif, shadow; uint16_t shadow_ip; uint8_t irr, isr; uint64_t cli, sti, stif; };
static struct r2_state r2_snapshot(const struct dosvm *v)
{
    return (struct r2_state){cur.tf.eip, cur.tf.eflags & (V86_TF | V86_IF), v->monitor.vif, v->monitor.shadow,
        v->monitor.shadow ? v->monitor.shadow_ip : 0, v->monitor.pic[0].irr, v->monitor.pic[0].isr,
        v->monitor.stats.insn[V86_CLI], v->monitor.stats.insn[V86_STI], v->perf.if_enable[DOSVM_IF_STI]};
}
/* One V86 #GP at CS:0 over CODE, ordinary (scalar) or lean-eligible, from
 * the same starting state; returns the number of lean exits it took. */
static unsigned r2_flags_exit(bool scalar, const uint8_t *code, unsigned n, bool vif, int pending_irq,
                              uint32_t eip, bool fullscreen, struct r2_state *out)
{
    struct dosvm *v = fx27_vm(false);
    v->fullscreen = fullscreen;   /* VGA full screen: the ports pass to the hardware */
    CHECK(!dosvm_write(v, 0x30000, code, n));
    v->monitor.vif = vif;
    if (pending_irq >= 0) {
        v->monitor.pic[0].imr = (uint8_t)~(1u << pending_irq);
        v86_irq_raise(&v->monitor, (unsigned)pending_irq);
    }
    unsigned lean0 = v->perf.lean_exits, gp0 = v->perf.exits[0];
    unsigned exits = 0;
    cur.tf.eip = eip; cur.tf.eflags = (cur.tf.eflags | V86_VM | V86_IF) & ~V86_TF;
    for (uint32_t end = eip + n; cur.tf.eip < end && exits < 4;) {
        g_need_resched = scalar;   /* the f3-14 scalar control: no lean exit */
        step(v, 13, 0); exits++;
        g_need_resched = false;
        CHECK(!left && v->info.state < DOSVM_STOPPING);
        if (cur.tf.eflags & V86_TF) break;     /* the shadowed successor is single-stepped */
    }
    CHECK(v->perf.exits[0] == gp0 + exits);
    *out = r2_snapshot(v);
    unsigned lean = v->perf.lean_exits - lean0;
    v->fullscreen = false;
    destroy(v, true); empty();
    return lean;
}
static void test_doswin3_v86_flags(void)
{
    static const struct { const char *name; uint8_t code[4]; unsigned n; bool vif; int irq; unsigned lean; } cases[] = {
        {"cli", {0xFA}, 1, true, -1, 1},
        {"cli-if-clear", {0xFA}, 1, false, -1, 1},
        {"sti", {0xFB}, 1, false, -1, 1},
        {"sti-if-set", {0xFB}, 1, true, -1, 1},
        /* STI that enables a deliverable IRQ: the ordinary tail plants the
         * single-step of the shadowed successor, exactly as without lean. */
        {"sti-pending", {0xFB}, 1, false, 1, 0},
        /* CLI masks the pending IRQ: nothing can be delivered after it, but
         * lean_after still sends a pending request to the ordinary tail. */
        {"cli-pending", {0xFA}, 1, true, 1, 0},
        /* STI; CLI: the CLI is the shadowed successor, ordinary; then the
         * shadow is retired. */
        {"sti-cli", {0xFB, 0xFA}, 2, false, -1, 1},
        {"o32-cli", {0x66, 0xFA}, 2, true, -1, 0},
    };
    unsigned fullscreen_lean = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(cases); i++) {
        struct r2_state ordinary, lean, fs_ordinary, fs_lean;
        unsigned scalar = r2_flags_exit(true, cases[i].code, cases[i].n, cases[i].vif, cases[i].irq, 0, false, &ordinary);
        unsigned fast = r2_flags_exit(false, cases[i].code, cases[i].n, cases[i].vif, cases[i].irq, 0, false, &lean);
        CHECK(!scalar);
        if (fast != cases[i].lean) printf("doswin3 r2 %s: lean exits %u, expected %u\n", cases[i].name, fast, cases[i].lean);
        CHECK(fast == cases[i].lean);
        CHECK(!memcmp(&ordinary, &lean, sizeof(lean)));
        /* Round 3: VGA full screen. Whether its CLI/STI exits are lean
         * depends on lean_before (on main since fx-doomfs2 they are); the
         * result is the ordinary path's, and the window's, either way. */
        scalar = r2_flags_exit(true, cases[i].code, cases[i].n, cases[i].vif, cases[i].irq, 0, true, &fs_ordinary);
        fast = r2_flags_exit(false, cases[i].code, cases[i].n, cases[i].vif, cases[i].irq, 0, true, &fs_lean);
        CHECK(!scalar && (fast == cases[i].lean || !fast));
        fullscreen_lean += fast;
        CHECK(!memcmp(&fs_ordinary, &fs_lean, sizeof(fs_lean)) && !memcmp(&fs_ordinary, &ordinary, sizeof(ordinary)));
    }
    printf("fx-doswin3 r3 lean V86 CLI/STI in VGA full screen: %u lean exits, same IF, shadow, TF, PIC, counters as the ordinary path and the window PASS\n",
           fullscreen_lean);
    /* A lean STI's software shadow ends at the next boundary that is not
     * its own: the CLI two instructions later is lean too. */
    struct dosvm *v = fx27_vm(false);
    const uint8_t code[] = {0xFB, 0x90, 0xFA};
    CHECK(!dosvm_write(v, 0x30000, code, sizeof(code)));
    v->monitor.vif = false;
    unsigned lean0 = v->perf.lean_exits;
    cur.tf.eip = 0; step(v, 13, 0);
    CHECK(cur.tf.eip == 1 && v->monitor.vif && v->monitor.shadow && v->monitor.shadow_ip == 1 && !(cur.tf.eflags & V86_TF));
    cur.tf.eip = 2; step(v, 13, 0);
    CHECK(cur.tf.eip == 3 && !v->monitor.vif && !v->monitor.shadow && v->perf.lean_exits == lean0 + 2);
    CHECK(v->perf.cost_count[DOSVM_COST_RM_CLI] >= 1 && v->perf.cost_count[DOSVM_COST_RM_STI] >= 1);
    destroy(v, true); empty();
    puts("fx-doswin3 r2 lean V86 CLI/STI: same IF, shadow, TF, PIC, counters as the ordinary path in 8 cases; shadow retired at the next boundary PASS");
}

/* ---- Page-flip presenter wake and presenter dispatch ---- */
static struct dosvm *r2_vm;
static unsigned r2_action;
static void r2_flip_hook(void)
{
    sleep_hook = 0;
    struct dosvm *v = r2_vm;
    if (r2_action == 1) {                                           /* back-buffer plane select */
        uint32_t changes = v->vga.changes;
        port_out(v, 0x3C5, 1, v->vga.seq[2] == 4 ? 8 : 4);
        CHECK(v->vga.changes != changes);
    }
    else if (r2_action == 2) port_out(v, 0x3D4, 2, 0x800C);        /* flip: start high 80h */
    else if (r2_action == 3) { port_out(v, 0x3C8, 1, 7); port_out(v, 0x3C9, 1, 0x3F); }
    else if (r2_action == 4) port_out(v, 0x3D4, 2, 0x000C);        /* flip back */
    dosvm_present_checkpoint(v);                                    /* the VM's next ordinary exit */
}
static uint32_t r2_wait(struct dosvm *v, unsigned action, uint32_t timeout)
{
    r2_vm = v; r2_action = action; sleep_hook = action ? r2_flip_hook : 0;
    struct ciuki_dosvm_command c = command_for(present_window.id, present_window.generation);
    c.wait.timeout_ms = timeout;
    CHECK(!dosvm_call(CIUKI_DOSVM_WAIT_PRESENT, &c, sizeof(c)));
    if (getenv("R2_DEBUG")) {
        struct binding *b = &bindings[v->info.key.id - 1];
        printf("r2_wait action=%u events=%x ticks=%llu present_due=%llu video_due=%llu presented=%u changes=%u/%u display=%u/%u starts=%u/%u pending=%u store=%u\n",
               action, c.wait.events, (unsigned long long)g_ticks, (unsigned long long)v->present_due_us,
               (unsigned long long)v->video_due_us, v->video_presented, b->changes, v->vga.changes, b->display,
               v->vga.display_changes, b->starts, v->video_starts, b->video_pending, dosvm_present_store_wake(v, g_ticks));
    }
    CHECK(!sleep_hook);                     /* the guest action ran while WAIT slept */
    CHECK(!v->present_dispatch);            /* the presenter runs: the yield ended */
    return c.wait.events;
}
static void r2_present(struct dosvm *v)
{
    struct ciuki_dosvm_command c = command_for(present_window.id, present_window.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_PRESENT_BANDS, &c, sizeof(c)));
    (void)v;
}
static void test_doswin3_flip_wake(void)
{
    window_fixtures(); g_ticks = 10000;
    CHECK(!create_window(0, "Flip wake", &present_window));
    struct dosvm *v = dosvm_lookup((struct dosvm_key){present_window.id, present_window.generation});
    int10(v, 0x13, 0, 0, 0);
    port_out(v, 0x3C4, 2, 0x0604); port_out(v, 0x3D4, 2, 0x0014); port_out(v, 0x3D4, 2, 0xE317);
    port_out(v, 0x3C4, 2, 0x0102);                                  /* Mode Y */
    CHECK(r2_wait(v, 0, 50) & CIUKI_DOSVM_EVENT_VIDEO); r2_present(v);
    g_ticks += 40;
    /* Not yet flipping: a back-buffer store still wakes the presenter. */
    CHECK(dosvm_present_store_wake(v, g_ticks));
    CHECK(r2_wait(v, 1, 50) & CIUKI_DOSVM_EVENT_VIDEO); r2_present(v);
    g_ticks += 40;
    CHECK(r2_wait(v, 2, 50) & CIUKI_DOSVM_EVENT_VIDEO); r2_present(v);   /* the first flip */
    CHECK(!dosvm_present_store_wake(v, g_ticks) && v->video_flip_tick && v->video_flip_seen);
    g_ticks += 20;
    CHECK(r2_wait(v, 4, 50) & CIUKI_DOSVM_EVENT_VIDEO); r2_present(v);   /* 20 ms later: a 40 ms hold */
    CHECK(v->video_flip_hold_ms >= 40 && v->video_flip_hold_ms <= 42);
    g_ticks += 2;
    /* Flipping: register writes and back-buffer stores do not wake it... */
    uint32_t events = r2_wait(v, 1, 20);
    CHECK(!(events & CIUKI_DOSVM_EVENT_VIDEO));
    /* ...the palette (display registers) and the next flip do. */
    CHECK(r2_wait(v, 3, 50) & CIUKI_DOSVM_EVENT_VIDEO); r2_present(v);
    g_ticks += 40;
    CHECK(r2_wait(v, 2, 50) & CIUKI_DOSVM_EVENT_VIDEO); r2_present(v);
    /* Flipping stops: after DOSVM_FLIP_HOLD_MS stores wake it again. */
    g_ticks += DOSVM_FLIP_HOLD_MS + 1;
    CHECK(dosvm_present_store_wake(v, g_ticks));
    CHECK(r2_wait(v, 1, 50) & CIUKI_DOSVM_EVENT_VIDEO); r2_present(v);
    /* A VBE mode or a text mode (hardware scrolling) never holds it. */
    v->video_flip_tick = g_ticks;
    CHECK(!dosvm_present_store_wake(v, g_ticks));
    v->vbe_mode = 0x101; CHECK(dosvm_present_store_wake(v, g_ticks)); v->vbe_mode = 0;
    v->video_geometry.text = 1; CHECK(dosvm_present_store_wake(v, g_ticks)); v->video_geometry.text = 0;
    struct ciuki_dosvm_command c = command_for(present_window.id, present_window.generation);
    CHECK(!finish_test_vm(&c, sizeof(c))); empty();
    puts("fx-doswin3 r2 page-flip wake: stores wake until the first flip, then only flips/display/palette, stores again after the hold PASS");
}
/* Round 3: the flip hold adapts to the flip rate (twice the last gap, one
 * refresh to DOSVM_FLIP_HOLD_MS). DOOM-like 15 Hz flips, then the guest
 * stops flipping and draws into the shown page (a fizzle, a menu): VIDEO
 * arrives within two flip gaps. 70 Hz flips hold about two refreshes. */
static void test_doswin3_flip_hold(void)
{
    window_fixtures(); g_ticks = 30000;
    CHECK(!create_window(0, "Flip hold", &present_window));
    struct dosvm *v = dosvm_lookup((struct dosvm_key){present_window.id, present_window.generation});
    int10(v, 0x13, 0, 0, 0);
    port_out(v, 0x3C4, 2, 0x0604); port_out(v, 0x3D4, 2, 0x0014); port_out(v, 0x3D4, 2, 0xE317);
    port_out(v, 0x3C4, 2, 0x0102);
    CHECK(r2_wait(v, 0, 50) & CIUKI_DOSVM_EVENT_VIDEO); r2_present(v);
    unsigned period_ms = (unsigned)((vga_frame_us(&v->vga) + 999) / 1000);
    CHECK(period_ms >= 13 && period_ms <= 17);
    g_ticks += 66;
    CHECK(r2_wait(v, 2, 50) & CIUKI_DOSVM_EVENT_VIDEO); r2_present(v);   /* a first flip: one refresh */
    CHECK(v->video_flip_hold_ms == period_ms);
    for (unsigned i = 1; i < 5; i++) {
        g_ticks += 66;
        CHECK(r2_wait(v, i & 1 ? 4 : 2, 50) & CIUKI_DOSVM_EVENT_VIDEO); r2_present(v);
    }
    CHECK(v->video_flip_hold_ms == DOSVM_FLIP_HOLD_MS);               /* 2 x 66 ms, capped at 50 */
    uint64_t last = v->video_flip_tick;
    g_ticks = last + 1;
    CHECK(!(r2_wait(v, 1, 20) & CIUKI_DOSVM_EVENT_VIDEO));             /* a store inside the hold */
    uint32_t events = r2_wait(v, 0, 40);                               /* the hold ends: the store shows */
    CHECK((events & CIUKI_DOSVM_EVENT_VIDEO) && g_ticks - last <= 2 * 66 && g_ticks - last > DOSVM_FLIP_HOLD_MS);
    r2_present(v);
    /* 70 Hz flipping (Wolfenstein 3-D's refresh-locked frames), one start
     * address change per refresh at exact ticks. */
    for (unsigned i = 0; i < 4; i++) {
        g_ticks += period_ms;
        port_out(v, 0x3D4, 2, i & 1 ? 0x800C : 0x000C);
    }
    CHECK(v->video_flip_hold_ms >= 2 * period_ms - 2 && v->video_flip_hold_ms <= 2 * period_ms + 2);
    unsigned hold70 = v->video_flip_hold_ms;
    struct ciuki_dosvm_command c = command_for(present_window.id, present_window.generation);
    CHECK(!finish_test_vm(&c, sizeof(c))); empty();
    printf("fx-doswin3 r3 flip hold: first flip %u ms, 15 Hz %u ms (stopped: store shown within two gaps), 70 Hz %u ms PASS\n",
           period_ms, DOSVM_FLIP_HOLD_MS, hold70);
}
/* The presenter woken for a frame may run ahead of the focused VM (round
 * 3: dosvm_task_present, the VM keeps the focus) until it runs, at most
 * DOSVM_PRESENT_DISPATCH_MS, never while a guest IRQ handler is visibly in
 * progress (a DPMI hardware-interrupt record or a virtual PIC in-service bit). */
static void r2_dispatch_hook(void)
{
    sleep_hook = 0;
    struct dosvm *v = r2_vm;
    CHECK(dosvm_task_focus() == v->task && !v->present_dispatch && !dosvm_task_present());
    CHECK(v->present_waiter == g_current);       /* the task inside WAIT_PRESENT */
    port_out(v, 0x3D4, 2, 0x400C);
    dosvm_present_checkpoint(v);
    CHECK(v->present_dispatch && v->present_dispatch_tick == g_ticks);
    CHECK(dosvm_task_focus() == v->task && dosvm_task_present() == v->present_waiter);
    g_ticks += DOSVM_PRESENT_DISPATCH_MS;
    CHECK(!dosvm_task_present());                /* bounded: the turn expired */
    g_ticks -= DOSVM_PRESENT_DISPATCH_MS;
    CHECK(dosvm_task_present() == v->present_waiter);
    /* Inside a guest hardware-interrupt handler the VM runs first. */
    v->dpmi.rec[0] = (struct dpmi_record){.flags = DPMI_RF_HWINT}; v->dpmi.depth = 1;
    CHECK(!dosvm_task_present());
    v->dpmi.depth = 0; v->dpmi.rec[0] = (struct dpmi_record){0};
    v->monitor.pic[0].isr = 1;                   /* a real-mode IRQ0 handler before its EOI */
    CHECK(!dosvm_task_present());
    v->monitor.pic[0].isr = 0; v->monitor.pic[1].isr = 0x10;
    CHECK(!dosvm_task_present());
    v->monitor.pic[1].isr = 0;
    CHECK(dosvm_task_present() == v->present_waiter && dosvm_task_focus() == v->task);
    /* Round 3 review (P3): any requested, unmasked IRQ keeps the VM first,
     * not only the timer/SB ones dosvm_task_irq makes urgent: a keyboard
     * IRQ1 and a UART IRQ4. */
    uint8_t imr = v->monitor.pic[0].imr, irr = v->monitor.pic[0].irr;
    static const unsigned irqs[] = {1, 4};
    for (unsigned k = 0; k < ARRAY_SIZE(irqs); k++) {
        v->monitor.pic[0].imr = (uint8_t)(imr & ~(1u << irqs[k])); v->monitor.pic[0].irr = (uint8_t)(irr | 1u << irqs[k]);
        CHECK(dosvm_pic_pending(&v->monitor) == (int)irqs[k] && !dosvm_task_irq() && !dosvm_task_present());
        v->monitor.pic[0].imr = imr; v->monitor.pic[0].irr = irr;
    }
    CHECK(dosvm_task_present() == v->present_waiter);
}
static void test_doswin3_present_dispatch(void)
{
    window_fixtures(); g_ticks = 20000;
    CHECK(!create_window(0, "Dispatch", &present_window));
    struct dosvm *v = dosvm_lookup((struct dosvm_key){present_window.id, present_window.generation});
    struct ciuki_dosvm_command c = command_for(present_window.id, present_window.generation);
    c.focused = 1; CHECK(!dosvm_call(CIUKI_DOSVM_FOCUS, &c, sizeof(c)));
    v->info.state = DOSVM_RUNNABLE; v->idle.sleeping = v->idle.seen = false;
    int10(v, 0x13, 0, 0, 0);
    port_out(v, 0x3C4, 2, 0x0604); port_out(v, 0x3D4, 2, 0x0014); port_out(v, 0x3D4, 2, 0xE317);
    CHECK(r2_wait(v, 0, 50) & CIUKI_DOSVM_EVENT_VIDEO); r2_present(v);
    g_ticks += 40;
    r2_vm = v; sleep_hook = r2_dispatch_hook;
    c = command_for(present_window.id, present_window.generation); c.wait.timeout_ms = 50;
    CHECK(!dosvm_call(CIUKI_DOSVM_WAIT_PRESENT, &c, sizeof(c)) && (c.wait.events & CIUKI_DOSVM_EVENT_VIDEO));
    CHECK(!sleep_hook && !v->present_dispatch && !v->present_waiter && dosvm_task_focus() == v->task);
    CHECK(!dosvm_task_present());
    /* Full screen has no waiting presenter: the flag never yields to one. */
    v->present_dispatch = true; v->present_dispatch_tick = g_ticks; v->present_waiter = v->task;
    CHECK(dosvm_task_present() == v->task);
    v->fullscreen = true; CHECK(!dosvm_task_present() && dosvm_task_focus() == v->task);
    v->fullscreen = false; v->present_dispatch = false; v->present_waiter = 0;
    c = command_for(present_window.id, present_window.generation);
    CHECK(!finish_test_vm(&c, sizeof(c))); empty();
    puts("fx-doswin3 r3 presenter dispatch: the focused VM keeps its focus; its woken presenter may run first until it runs, at most 20 ms, never inside a visible guest IRQ handler PASS");
}
static void test_doswin3_r2(void)
{
    test_doswin3_v86_flags();
    test_doswin3_flip_wake();
    test_doswin3_flip_hold();
    test_doswin3_present_dispatch();
}
