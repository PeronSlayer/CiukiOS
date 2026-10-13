/* f3-11: actual monitor return/EOI paths and failed IRQ-frame transactions.
 * The CPU frame is synthetic; this does not attribute the T23 stall.
 * SPDX-License-Identifier: GPL-2.0-only */
static void test_irq_restore_delivery(void)
{
    for (unsigned bits = 0; bits < 2; bits++) {
        struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, bits != 0);
        uint32_t base = seg_base(v, (uint16_t)cur.tf.cs);
        uint16_t code = (uint16_t)cur.tf.cs;
        const uint8_t save[] = {0x9C, 0x58, 0xFA};
        const uint8_t epilogue[] = {0x90, 0xC3};
        CHECK(!dosvm_write(v, base + 0x300, save, sizeof(save)) &&
              !dosvm_write(v, base + 0x340, epilogue, sizeof(epilogue)));
        v->dpmi.vec[8] = (struct dpmi_vec){.sel=code, .off=0x700};
        cur.tf.eip = 0x300; step(v, 0x20, 0);
        cur.tf.eip = 0x302; cur.tf.eax = V86_IF | 2; op1(v, 0xFA);
        CHECK(v->dpmi.if_armed && !v->monitor.vif && (cur.tf.eax & V86_TF));
        /* POPF; NOP retired: the saved-image #DB and a PIT edge coincide.
         * A live near-return slot makes f3-10 seed its bounded tracing. */
        pm_push(v, 0x400);
        cur.tf.eip = 0x340; cur.tf.eflags |= V86_TF;
        uint64_t edges = v->pit.edges;
        dosvm_pit_advance(v, 60);
        CHECK(v->pit.edges > edges && (v->monitor.pic[0].irr & 1));
        debug_dr6 = 0xFFFF4FFFu; step(v, 1, 0);
        CHECK(!left && cur.tf.eip == 0x700 && v->dpmi.depth == 1 &&
              !v->dpmi.if_armed && !v->monitor.vif && !(cur.tf.eflags & V86_TF));
        CHECK(v->dpmi.return_steps == DPMI_RETURN_STEPS && v->monitor.pic[0].isr == 1 &&
              v->monitor.stats.delivered[0] == 1 && v->dpmi.rec[0].saved.tf.eip == 0x340 &&
              (v->dpmi.rec[0].saved.tf.eflags & V86_TF));
        /* Another edge while in service must wait; EOI is still emulated
         * with boundary/shadow state present and IF clear. */
        CHECK(dosvm_irq_raise(v->info.key, 0));
        v->dpmi.shadow = true;
        v->dpmi.if_armed = true;
        cur.tf.eax = 0x20; const uint8_t eoi[] = {0xE6, 0x20};
        exec(v, eoi, sizeof(eoi), 13, 0);
        CHECK(!v->monitor.pic[0].isr && (v->monitor.pic[0].irr & 1) && cur.tf.eip == 0x702);
        v->dpmi.if_armed = false;
        pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
        CHECK(cur.tf.eip == 0x700 && v->dpmi.depth == 1 && v->monitor.stats.delivered[0] == 2);
        cur.tf.eax = 0x20; exec(v, eoi, sizeof(eoi), 13, 0);
        pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
        CHECK(cur.tf.eip == 0x340 && !v->dpmi.depth && !v->monitor.pic[0].isr &&
              v->monitor.vif && (cur.tf.eflags & V86_TF));
        destroy(v, true); empty();
    }
    puts("f3-11 restore/IRQ replay: arm, BS at restore + PIT edge, owned return TF saved, handler EOI with armed/shadow state, pending IRQ and both returns PASS");
}

static void test_irq_frame_commit(void)
{
    for (unsigned failure = 0; failure < 4; failure++) {
        struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, true);
        struct v86_frame before = cur;
        uint16_t code = (uint16_t)cur.tf.cs;
        v->dpmi.vec[8] = (struct dpmi_vec){.sel=code, .off=0x700};
        uint32_t lock_top = v->dpmi.lock_top, va = v->dpmi.hstack.base;
        uint32_t old_flags = 0, phys = 0;
        if (failure == 0) v->dpmi.vec[8].sel = 0; /* stale code selector */
        if (failure == 1) v->dpmi.lock_top = 0; /* locked-stack exhaustion */
        if (failure == 2) va += (lock_top - 4) & ~(PAGE_SIZE - 1); /* frame write refused */
        if (failure == 3) { /* default real reflection: inaccessible IVT */
            v->dpmi.vec[8] = (struct dpmi_vec){DPMI_PM_INT + 8, DPMI_HOST_CS};
            va = 0;
        }
        if (failure >= 2) {
            phys = as_lookup(&v->as, va, &old_flags);
            CHECK(phys && !as_map(&v->as, va, phys, failure == 2 ? PTE_U : PTE_W));
        }
        CHECK(dosvm_irq_raise(v->info.key, 0));
        uint64_t reflected = v->monitor.stats.reflected_irqs;
        uint32_t delivered = v->monitor.stats.delivered[0], pm = v->dpmi.stats.irqs_pm;
        unsigned depth = v->dpmi.depth;
        int e = dpmi_deliver(v, &cur);
        CHECK(e < 0 && !memcmp(&cur, &before, sizeof(cur)) && v->frame_pm && v->monitor.vif);
        CHECK(v->monitor.pic[0].irr == 1 && !v->monitor.pic[0].isr &&
              v->monitor.stats.delivered[0] == delivered && v->monitor.stats.reflected_irqs == reflected &&
              v->dpmi.stats.irqs_pm == pm && v->dpmi.depth == depth);
        v->dpmi.lock_top = lock_top;
        if (failure >= 2) CHECK(!as_map(&v->as, va, phys, old_flags));
        v->dpmi.vec[8] = (struct dpmi_vec){.sel=code, .off=0x700};
        CHECK(!dpmi_deliver(v, &cur) && cur.tf.eip == 0x700 && v->monitor.pic[0].isr == 1 &&
              !(v->monitor.pic[0].irr & 1) && v->monitor.stats.delivered[0] == delivered + 1);
        destroy(v, true); empty();
    }
    puts("f3-11 IRQ commit: stale vector, exhausted/unwritable PM stack and invalid RM reflection leave frame/PIC/delivery unchanged; valid retry delivers once PASS");
}

static void test_irq_stall_diagnostics(void)
{
    struct dosvm *v = create(); struct v86_frame f = v->frame;
    f.tf.cs = 0x1DF; f.tf.eip = 0x2483E2;
    uint64_t saved_ticks = g_ticks;
    uint32_t saved_generation = v->info.key.generation;
    unsigned before = dosvm_logs;
    g_ticks = 100;
    v86_irq_raise(&v->monitor, 0); CHECK(dosvm_pic_ack(&v->monitor) == 8);
    v->monitor.stats.last_vector = 8;
    v->trace.injected_cs = 0xFF; v->trace.injected_eip = 0x20;
    dosvm_boundary(v, &f, "if_restore"); /* quiet boundary does not record */
    g_ticks += 1001; dosvm_pic_stall(v, &f); CHECK(dosvm_logs == before);
    g_boot.flags = CBI_F_VERBOSE;
    dosvm_boundary(v, &f, "if_restore");
    g_ticks = 1100; dosvm_pic_stall(v, &f); CHECK(dosvm_logs == before);
    v86_irq_raise(&v->monitor, 0);
    g_ticks++; dosvm_pic_stall(v, &f); CHECK(dosvm_logs == before + 1);
    CHECK(strstr(last_dosvm_log, "[dosvm] event=pic_stall") && strstr(last_dosvm_log, "ms=1001") &&
          strstr(last_dosvm_log, "cs=01df eip=002483e2 isr=01 irr=01 imr=f8 vec=08") &&
          strstr(last_dosvm_log, "to=00ff:00000020 boundary=if_restore at=01df:002483e2"));
    dosvm_pic_stall(v, &f); CHECK(dosvm_logs == before + 1 && v->monitor.pic[0].isr == 1);
    device_out(v, 0x20, 0x20); CHECK(dosvm_pic_ack(&v->monitor) == 8);
    g_ticks += 1000; dosvm_pic_stall(v, &f); CHECK(dosvm_logs == before + 1);
    g_ticks++; dosvm_pic_stall(v, &f); CHECK(dosvm_logs == before + 2);
    /* EOI and reinjection without an intervening sample reset the age. */
    device_out(v, 0x20, 0x60); v86_irq_raise(&v->monitor, 0); dosvm_pic_ack(&v->monitor);
    dosvm_pic_stall(v, &f); CHECK(dosvm_logs == before + 2);
    device_out(v, 0x20, 0x60);
    device_pic_reset(v, 3); v86_irq_raise(&v->monitor, 0); dosvm_pic_ack(&v->monitor);
    g_ticks += 1001; dosvm_pic_stall(v, &f); CHECK(dosvm_logs == before + 2); /* AEOI */
    device_pic_reset(v, 1);
    v86_irq_raise(&v->monitor, 13); dosvm_pic_ack(&v->monitor);
    g_ticks += 1001; dosvm_pic_stall(v, &f); CHECK(dosvm_logs == before + 4);
    CHECK(strstr(last_dosvm_log, "pic=1 irq=13") && v->monitor.pic[1].isr == 0x20);
    /* Poll also acknowledges an interrupt; timestamps work at tick zero
     * and across the 64-bit wrap. Boundary state outlives trace budgets. */
    device_pic_reset(v, 1); g_ticks = UINT64_MAX - 500;
    v86_irq_raise(&v->monitor, 0); device_out(v, 0x20, 0x0C); CHECK(device_in(v, 0x20) == 0x80);
    v->trace.used[DOSVM_TRACE_FLAGS] = 64;
    dosvm_trace(v, DOSVM_TRACE_FLAGS, &f, 1, 1, 0);
    CHECK(!strcmp(v->trace.boundary, "if_restore"));
    g_ticks += 1001; dosvm_pic_stall(v, &f); CHECK(dosvm_logs == before + 5);
    v->info.key.generation = UINT32_MAX; f.tf.cs = 0xFFFF; f.tf.eip = UINT32_MAX;
    for (unsigned i = v->trace.pic_stalls; i < DOSVM_PIC_STALL_LIMIT; i++) {
        device_out(v, 0x20, 0x60); v86_irq_raise(&v->monitor, 0); dosvm_pic_ack(&v->monitor);
        g_ticks += 1001; dosvm_pic_stall(v, &f);
    }
    before = dosvm_logs;
    device_out(v, 0x20, 0x60); v86_irq_raise(&v->monitor, 0); dosvm_pic_ack(&v->monitor);
    g_ticks += 1001; dosvm_pic_stall(v, &f); CHECK(dosvm_logs == before);
    CHECK(v->monitor.pic[0].isr == 1);
    v->info.key.generation = saved_generation;
    g_boot.flags = 0; g_ticks = saved_ticks;
    destroy(v, true); empty();
    puts("f3-11 PIC diagnostics: >1s, no automatic EOI, one per episode, reinjection/AEOI/reinit/poll/cascade/wrap, last boundary past trace cap, quiet/lifetime bound PASS");
}
static void test_irq_real_tf(void)
{
    struct dosvm *v = dpmi_vm(true); v86_start();
    uint32_t base = (uint32_t)CODE_SEG << 4;
    const uint8_t code[] = {0x17, 0x90, 0x90, 0x90}; /* POP SS; successor */
    CHECK(!dosvm_write(v, base + 0x100, code, sizeof(code)));
    step(v, 0x20, 0);
    CHECK(!v->dpmi.if_armed && v->monitor.shadow && (cur.tf.eflags & V86_TF));
    cur.tf.eip = 0x102; debug_dr6 = 1u << 14; step(v, 1, 0);
    CHECK(!v->monitor.shadow && !(cur.tf.eflags & V86_TF));
    /* Live monitor TF is never saved as guest FLAGS. A POPF loading an
     * arbitrary saved register cannot turn on permanent single stepping. */
    cur.tf.eflags |= V86_TF; op1(v, 0x9C);
    uint16_t image = rm_pop16(v);
    CHECK((image & V86_IF) && !(image & V86_TF) && !(cur.tf.eflags & V86_TF));
    rm_push16(v, V86_IF | V86_TF | 0x0C55u | 2u); op1(v, 0x9D);
    CHECK(v->monitor.vif && !(cur.tf.eflags & V86_TF));
    CHECK(dosvm_irq_raise(v->info.key, 0));
    v->monitor.vif = false; v86_prepare_return(&v->monitor, &cur);
    CHECK(!(cur.tf.eflags & V86_TF));
    v->monitor.vif = true; v86_prepare_return(&v->monitor, &cur);
    CHECK((cur.tf.eflags & V86_TF) && !v->dpmi.if_armed);
    cur.tf.eip++; debug_dr6 = 1u << 14; step(v, 1, 0);
    CHECK(v->monitor.stats.delivered[0] == 1 && !(cur.tf.eflags & V86_TF));
    destroy(v, true); empty();
    puts("f3-11 V86 TF: unarmed SS/successor and pending-IRQ boundaries only, clear on next return, PUSHF hides monitor TF and arbitrary POPF cannot leak TF PASS");
}
static void test_irq_stall(void)
{
    test_irq_restore_delivery(); test_irq_frame_commit(); test_irq_stall_diagnostics(); test_irq_real_tf();
}
