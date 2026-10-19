/* F3-42: production hardware frames, virtual PIC arbitration and host IRET
 * continuations. Synthetic CPU boundaries; the DOOM replay executes IRET. */
static void test_isr_recovery_boundary(void)
{
    for (unsigned bits = 0; bits < 2; bits++) {
        struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, bits != 0);
        uint16_t code = (uint16_t)cur.tf.cs;
        v->dpmi.vec[8] = (struct dpmi_vec){.sel=code, .off=0x700};
        v->dpmi.vec[15] = (struct dpmi_vec){.sel=code, .off=0x720};
        v->dpmi.vec[0x1C] = (struct dpmi_vec){.sel=code, .off=0x740};
        device_out(v, 0x21, 0x7E); /* IRQ0 and IRQ7 */
        struct v86_frame original = cur;
        CHECK(dosvm_irq_raise(v->info.key, 7)); step(v, 0x20, 0);
        CHECK(v->dpmi.depth == 1 && !v->monitor.vif && cur.tf.eip == 0x720);
        uint64_t start = g_ticks;
        CHECK(dosvm_irq_raise(v->info.key, 0));
        /* A live hardware handler excludes ordinary recovery, including
         * nested software calls, until the separate ISR bound expires. */
        cur.tf.eip++; step(v, 0x20, 0);
        CHECK(!v->dpmi.if_step && !v->dpmi.if_waiting && !(cur.tf.eflags & V86_TF));
        CHECK(!dpmi_enter_pm_handler(v, &cur, 0x1C, 0)); /* nested software frame */
        g_ticks += DPMI_IF_WAIT_MS + 1;
        cur.tf.eip++; step(v, 0x20, 0);
        CHECK(v->dpmi.depth == 2 && !v->monitor.vif && !v->dpmi.if_step &&
              !v->dpmi.if_waiting && !v->perf.if_enable[DOSVM_IF_FORCED] && !(cur.tf.eflags & V86_TF));
        pm_iret(v); at_stub(v, DPMI_PM_SOFTRET);
        CHECK(v->dpmi.depth == 1 && !v->monitor.vif && !v->monitor.stats.delivered[0]);
        cur.tf.eax = 0x20; const uint8_t eoi[] = {0xE6, 0x20};
        exec(v, eoi, sizeof(eoi), 13, 0);
        v->dpmi.shadow = true; /* return retires the handler's shadow */
        pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
        CHECK(v->perf.isr[7].count == 1 && v->perf.isr[7].ms == g_ticks - start &&
              v->perf.if_enable[DOSVM_IF_TRAMPOLINE] == 1 && !v->dpmi.shadow);
        CHECK(cur.tf.eip == 0x700 && v->dpmi.depth == 1 && !v->monitor.vif &&
              v->monitor.stats.delivered[0] == 1 && !(cur.tf.eflags & V86_TF));
        cur.tf.eax = 0x20; exec(v, eoi, sizeof(eoi), 13, 0);
        pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
        CHECK(!v->dpmi.depth && v->monitor.vif && !v->dpmi.shadow && !v->monitor.shadow &&
              cur.tf.cs == original.tf.cs && cur.tf.eip == original.tf.eip &&
              cur.tf.user_ss == original.tf.user_ss && cur.tf.user_esp == original.tf.user_esp &&
              v->perf.isr[0].count == 1 && v->perf.if_enable[DOSVM_IF_TRAMPOLINE] == 2);
        destroy(v, true); empty();
    }
    puts("f3-42 ISR return: 16/32-bit, pending IRQ, nested software frame, no recovery TF/timeout, immediate return delivery PASS");
}

static void test_isr_flat_private_stack(void)
{
    struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, true);
    struct dpmi *d = &v->dpmi;
    uint16_t code = (uint16_t)cur.tf.cs;
    d->vec[8] = (struct dpmi_vec){.sel=code, .off=0x700};
    d->vec[15] = (struct dpmi_vec){.sel=code, .off=0x720};
    d->vec[0x1C] = (struct dpmi_vec){.sel=code, .off=0x740};
    device_out(v, 0x21, 0x7E);
    struct v86_frame original = cur;
    /* An enclosing handler gives nested delivery its real private-stack
     * scope. Both IRQs then use one flat selector, as in DOS/4GW. */
    CHECK(!dpmi_enter_pm_handler(v, &cur, 0x1C, 0));
    struct v86_frame software = cur;
    client_desc(v, 902, 0, 0xFFFFFFFFu, 0xF2, 0xC0);
    cur.tf.user_ss = DPMI_SEL(902); cur.tf.user_esp = 0x18000;
    v->monitor.vif = true;
    CHECK(dosvm_irq_raise(v->info.key, 7)); step(v, 0x20, 0);
    CHECK(d->depth == 2 && cur.tf.user_ss == DPMI_SEL(902) && cur.tf.user_esp == 0x18000 - 12);
    struct v86_frame handler = cur;
    cur.tf.user_esp = 0x1A000; cur.tf.eip++;
    v->monitor.vif = true;
    CHECK(dosvm_irq_raise(v->info.key, 0)); step(v, 0x20, 0);
    CHECK(cur.tf.eip == 0x700);
    const uint8_t eoi[] = {0xE6, 0x20}; cur.tf.eax = 0x20;
    exec(v, eoi, sizeof(eoi), 13, 0); pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
    /* Return from the private stack to the original IRQ7 IRET frame.
     * F3-44 already discarded that record at the nested IRQ0 delivery. */
    cur = handler; cur.tf.eax = 0x20;
    exec(v, eoi, sizeof(eoi), 13, 0); pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
    if (v->info.state >= DOSVM_STOPPING)
        fprintf(stderr, "f3-45 IRQRET failure: reason=%u vector=%u pc=%08x depth=%u retired=%u irq7=%u irq0=%u\n",
                v->info.reason, v->info.fault_vector, v->info.fault_eip, d->depth,
                v->perf.isr_retired, v->perf.isr[7].count, v->perf.isr[0].count);
    CHECK(v->info.state < DOSVM_STOPPING && d->depth == 1);
    CHECK(v->perf.isr[7].count == 1 && v->perf.isr[0].count == 1 && !v->perf.isr_retired);
    CHECK(!v->monitor.pic[0].isr && !v->perf.isr_late_return);
    cur = software; pm_iret(v); at_stub(v, DPMI_PM_SOFTRET);
    CHECK(!d->depth && cur.tf.eip == original.tf.eip && cur.tf.user_esp == original.tf.user_esp);
    destroy(v, true); empty();
    puts("f3-45 flat private stack: nested IRQ0 above IRQ7 frame on same selector, IRQRET survives, each ISR closes once PASS");
}

static void test_isr_return_evidence(void)
{
    for (unsigned bits = 0; bits < 2; bits++) for (unsigned form = 0; form < 3; form++) {
        struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, bits != 0);
        uint16_t code = (uint16_t)cur.tf.cs;
        v->dpmi.vec[15] = (struct dpmi_vec){.sel=code, .off=0x720};
        device_out(v, 0x21, 0x7E);
        if (form == 2) {
            /* The DOS prompt may have been interrupted in V86. A raw
             * extender return can bypass the PM trampoline completely. */
            cur.tf.cs = 0xF000; cur.tf.eip = 0x102;
            cur.tf.user_ss = STACK_SEG; cur.tf.user_esp = 0xFF00;
            cur.tf.eflags = V86_VM | V86_IF | 2;
            v->frame_pm = false;
        }
        struct v86_frame original = cur;
        CHECK(dosvm_irq_raise(v->info.key, 7)); step(v, 0x20, 0);
        CHECK(v->dpmi.depth == 1 && dosvm_in_irq(v));
        struct v86_frame handler = cur;
        if (!form) {
            /* F3-45: an edited target and ESP above the frame cannot
             * distinguish return from a still-live private stack. */
            cur.tf.user_esp += 3 * dpmi_width(v);
            cur.tf.eip = 0x760;
        } else {
            cur = original; v->frame_pm = form != 2;
            /* Each part of the exact saved context matters independently. */
            for (unsigned mismatch = 0; mismatch < 5; mismatch++) {
                struct v86_frame near = original;
                if (!mismatch) near.tf.cs ^= 8;
                else if (mismatch == 1) near.tf.eip++;
                else if (mismatch == 2) near.tf.user_ss ^= 8;
                else if (mismatch == 3) near.tf.user_esp++;
                else v->frame_pm = !v->frame_pm;
                dpmi_irq_resync(v, &near);
                CHECK(v->dpmi.depth == 1 && !v->perf.isr_retired && !v->perf.isr[7].count);
                v->frame_pm = form != 2;
            }
        }
        cur.tf.eax = 0x12345678;
        struct v86_frame returned = cur;
        dpmi_irq_resync(v, &cur);
        CHECK(!memcmp(&cur, &returned, sizeof(cur)));
        if (!form) {
            CHECK(v->dpmi.depth == 1 && dosvm_in_irq(v) && !v->monitor.vif &&
                  !v->perf.isr_retired && !v->perf.isr[7].count);
            dpmi_irq_resync(v, &cur); CHECK(!v->perf.isr[7].count);
            cur = handler; pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
            CHECK(!v->dpmi.depth && v->monitor.vif && v->perf.isr[7].count == 1 && !v->perf.isr_retired);
        } else {
            CHECK(!v->dpmi.depth && !dosvm_in_irq(v) && v->monitor.vif && v->perf.isr_retired == 1);
            CHECK(v->perf.isr[7].count == 1);
        }
        CHECK(v->monitor.pic[0].isr == 0x80); /* return is never a PIC EOI */
        dpmi_irq_resync(v, &cur); CHECK(v->perf.isr[7].count == 1);
        destroy(v, true); empty();
    }
    /* Returning to an inner interrupted boundary retires only that IRQ. */
    struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, true);
    uint16_t code = (uint16_t)cur.tf.cs;
    v->dpmi.vec[15] = (struct dpmi_vec){.sel=code, .off=0x720};
    v->dpmi.vec[8] = (struct dpmi_vec){.sel=code, .off=0x700};
    device_out(v, 0x21, 0x7E);
    CHECK(dosvm_irq_raise(v->info.key, 7)); step(v, 0x20, 0);
    op1(v, 0xFB); cur.tf.eip++;
    CHECK(dosvm_irq_raise(v->info.key, 0)); step(v, 0x20, 0);
    CHECK(v->dpmi.depth == 2);
    cur = v->dpmi.rec[1].saved; dpmi_irq_resync(v, &cur);
    CHECK(v->dpmi.depth == 1 && dosvm_in_irq(v) && v->perf.isr[0].count == 1);
    cur = v->dpmi.rec[0].saved; dpmi_irq_resync(v, &cur);
    CHECK(!v->dpmi.depth && !dosvm_in_irq(v) && v->perf.isr[7].count == 1 &&
          v->monitor.pic[0].isr == 0x81);
    destroy(v, true); empty();
    puts("f3-45 ISR evidence: 16/32-bit edited target retains continuation, exact PM/V86 prompt return retires, nested retirement, actual registers preserved, no synthetic EOI PASS");
}

static void test_isr_expiry(void)
{
    for (unsigned bits = 0; bits < 2; bits++) {
        struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, bits != 0);
        uint16_t code = (uint16_t)cur.tf.cs;
        v->dpmi.vec[15] = (struct dpmi_vec){.sel=code, .off=0x720};
        v->dpmi.vec[8] = (struct dpmi_vec){.sel=code, .off=0x700};
        device_out(v, 0x21, 0x7E);
        CHECK(dosvm_irq_raise(v->info.key, 7)); step(v, 0x20, 0);
        struct v86_frame handler = cur;
        /* A private stack alone is never return evidence. */
        client_desc(v, 902, 0x30000, 0xFFFF, 0xF2, bits ? 0x40 : 0);
        cur.tf.user_ss = DPMI_SEL(902); cur.tf.user_esp = bits ? 0x8000 : 0xABCD8000;
        uint64_t start = v->dpmi.rec[0].isr_tick;
        g_ticks = start + DPMI_ISR_WAIT_MS - 1;
        dpmi_irq_resync(v, &cur);
        CHECK(v->dpmi.depth == 1 && dosvm_in_irq(v) && !v->perf.isr_expired);
        g_ticks++;
        dpmi_irq_resync(v, &cur);
        CHECK(v->dpmi.depth == 1 && !dosvm_in_irq(v) && v->perf.isr_expired == 1 &&
              v->dpmi.rec[0].isr_expired && v->perf.isr[7].count == 1 && !v->monitor.vif);
        CHECK(dosvm_irq_raise(v->info.key, 0));
        CHECK(!dpmi_deliver(v, &cur) && v->dpmi.if_waiting && !v->monitor.vif);
        g_ticks += DPMI_IF_WAIT_MS;
        CHECK(!dpmi_deliver(v, &cur) && v->dpmi.depth == 2 && cur.tf.eip == 0x700 &&
              v->monitor.stats.delivered[0] == 1 && v->perf.if_enable[DOSVM_IF_FORCED] == 1);
        cur.tf.eax = 0x20; const uint8_t eoi[] = {0xE6, 0x20};
        exec(v, eoi, sizeof(eoi), 13, 0); pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
        CHECK(v->dpmi.depth == 1 && !dosvm_in_irq(v));
        /* The expired obligation still holds a valid late continuation. */
        cur = handler; cur.tf.eax = 0x20;
        exec(v, eoi, sizeof(eoi), 13, 0); pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
        CHECK(!v->dpmi.depth && v->perf.isr[7].count == 1 && v->monitor.vif &&
              !v->monitor.pic[0].isr && !v->monitor.pic[0].irr);
        destroy(v, true); empty();
    }
    puts("f3-44 ISR expiry: private stack preserved, 49/50ms bound, recovery resumes, nested IRQ and late trampoline return once PASS");
}

static void test_if_cli_epoch(void)
{
    for (unsigned api = 0; api < 2; api++) {
        struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, true);
        uint16_t code = (uint16_t)cur.tf.cs;
        uint32_t base = seg_base(v, code);
        const uint8_t spin[] = {0xEB, 0xFE};
        CHECK(!dosvm_write(v, base + 0x300, spin, sizeof(spin)));
        v->dpmi.vec[8] = (struct dpmi_vec){.sel=code, .off=0x700};
        cur.tf.eip = 0x300; v->monitor.vif = false;
        CHECK(dosvm_irq_raise(v->info.key, 0)); step(v, 0x20, 0);
        CHECK(v->dpmi.if_waiting);
        g_ticks += DPMI_IF_WAIT_MS - 1;
        if (api) int31(v, 0x0900); else op1(v, 0xFA);
        CHECK(!v->monitor.vif && !v->dpmi.depth);
        g_ticks++;
        cur.tf.eip = 0x300; step(v, 0x20, 0);
        CHECK(!v->monitor.vif && !v->dpmi.depth);
        g_ticks++;
        step(v, 0x20, 0);
        CHECK(v->dpmi.depth == 1 && cur.tf.eip == 0x700 && v->monitor.stats.delivered[0] == 1);
        destroy(v, true); empty();
    }
    puts("f3-44 virtual IF: every CLI/DPMI disable restarts pending ordinary-code grace; physical IF alone never enables PASS");
}

static void test_if_handler_scope(void)
{
    for (unsigned kind = 0; kind < 3; kind++) {
        struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, true);
        uint16_t code = (uint16_t)cur.tf.cs;
        v->dpmi.vec[8] = (struct dpmi_vec){.sel=code, .off=0x700};
        v->dpmi.vec[0x1C] = (struct dpmi_vec){.sel=code, .off=0x740};
        CHECK(!dpmi_enter_pm_handler(v, &cur, 0x1C, 0));
        /* All these continuations are host-owned; their bodies cannot be
         * classified as ordinary code by the shorter compatibility bound. */
        if (kind) v->dpmi.rec[0].kind = kind == 1 ? DPMI_REC_EXCEPTION : DPMI_REC_CALLBACK;
        CHECK(dosvm_irq_raise(v->info.key, 0));
        g_ticks += DPMI_IF_WAIT_MS + 1;
        CHECK(!dpmi_deliver(v, &cur) && !v->monitor.vif && !v->dpmi.if_waiting &&
              !v->dpmi.if_step && !v->monitor.stats.delivered[0]);
        v->dpmi.rec[0].kind = DPMI_REC_PMINT;
        pm_iret(v); at_stub(v, DPMI_PM_SOFTRET);
        CHECK(v->monitor.stats.delivered[0] == 1 && cur.tf.eip == 0x700);
        destroy(v, true); empty();
    }
    puts("f3-44 ordinary-code scope: software ISR, exception and callback retain IF; IRQ delivers at host return PASS");
}
static void test_isr_nested_hardware(void)
{
    for (unsigned bits = 0; bits < 2; bits++) {
        struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, bits != 0);
        uint16_t code = (uint16_t)cur.tf.cs;
        v->dpmi.vec[8] = (struct dpmi_vec){.sel=code, .off=0x700};
        v->dpmi.vec[15] = (struct dpmi_vec){.sel=code, .off=0x720};
        device_out(v, 0x21, 0x7E);
        CHECK(dosvm_irq_raise(v->info.key, 7)); step(v, 0x20, 0);
        CHECK(dosvm_irq_raise(v->info.key, 0));
        op1(v, 0xFB); /* IRQ7's STI: one instruction of shadow */
        CHECK(v->monitor.vif && v->dpmi.shadow && v->dpmi.depth == 1);
        step(v, 0x20, 0); CHECK(v->dpmi.depth == 1);
        cur.tf.eip++; step(v, 0x20, 0);
        CHECK(v->dpmi.depth == 2 && !v->monitor.vif && cur.tf.eip == 0x700 && v->perf.isr_max_depth == 2);
        g_ticks += 2;
        cur.tf.eax = 0x20; const uint8_t eoi[] = {0xE6, 0x20};
        exec(v, eoi, sizeof(eoi), 13, 0); pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
        CHECK(v->dpmi.depth == 1 && v->monitor.vif && v->perf.isr[0].count == 1 &&
              v->perf.isr[0].ms == 2 && v->monitor.pic[0].isr == 0x80);
        op1(v, 0xFA); CHECK(!v->monitor.vif); /* outer handler may disable again */
        g_ticks += 3;
        cur.tf.eax = 0x20; exec(v, eoi, sizeof(eoi), 13, 0);
        pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
        CHECK(!v->dpmi.depth && v->monitor.vif && v->perf.isr[7].count == 1 &&
              v->perf.isr[7].ms == 5 && !v->monitor.pic[0].isr && !(cur.tf.eflags & V86_TF));
        destroy(v, true); empty();
    }
    puts("f3-42 nested IRQ7/IRQ0: STI shadow, independent inclusive durations, outer CLI/IRET restores IF PASS");
}
