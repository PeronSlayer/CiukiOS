/* DMX early-EOI protocol through the production trap/PIT/PIC/DPMI paths.
 * Synthetic CPU boundaries, no game binary or physical audio claim.
 * SPDX-License-Identifier: GPL-2.0-only */
static struct dosvm *dospic_vm(unsigned mode)
{
    /* V86; native PM16/32; PM16/32 reflected to V86; DPMI-active V86. */
    struct dosvm *v = dpmi_vm(mode != 0);
    v86_start();
    if (mode) {
        enter(v, mode != 1 && mode != 3);
        if (mode <= 2) {
            uint16_t code = (uint16_t)cur.tf.cs;
            v->dpmi.vec[8] = (struct dpmi_vec){.sel=code, .off=0x700};
            v->dpmi.vec[15] = (struct dpmi_vec){.sel=code, .off=0x720};
        }
        if (mode == 5) { v86_start(); v->frame_pm = false; }
    }
    uint16_t ivt[2] = {0, IRET_SEG};
    CHECK(!dosvm_write(v, 8 * 4, ivt, sizeof(ivt)));
    CHECK(!dosvm_write(v, 9 * 4, ivt, sizeof(ivt)));
    CHECK(!dosvm_write(v, 15 * 4, ivt, sizeof(ivt)));
    device_pic_reset(v, 1);
    device_out(v, 0x21, 0x7E); device_out(v, 0xA1, 0xFF);
    v->monitor.pic[0].irr = 0;
    v->monitor.vif = true;
    return v;
}

static void dospic_eoi_sti(struct dosvm *v, unsigned irq)
{
    const uint8_t eoi[] = {0xE6, 0x20};
    cur.tf.eax = 0x60 | irq; exec(v, eoi, sizeof(eoi), 13, 0);
    op1(v, 0xFB);
    cur.tf.eip++; step(v, 0x20, 0); /* the STI successor retires */
    CHECK(v->monitor.vif && !(v->monitor.pic[0].isr & (1u << irq)));
}

static void dospic_iret(struct dosvm *v, unsigned mode)
{
    if (mode == 1 || mode == 2) { pm_iret(v); at_stub(v, DPMI_PM_IRQRET); }
    else {
        bool reflected = mode == 3 || mode == 4;
        for (unsigned irq = 0; irq < 16; irq++)
            if (v->irq.rm[irq].active && v->irq.rm[irq].ss == (uint16_t)cur.tf.user_ss &&
                v->irq.rm[irq].sp == (uint16_t)cur.tf.user_esp) reflected = false;
        op1(v, 0xCF);
        if (reflected) {
            if (in_pm() || cur.tf.cs != 0xF000 || cur.tf.eip != DPMI_RM_RETURN)
                fprintf(stderr, "dospic reflection mode=%u pc=%04x:%08x depth=%u hits=%u/%u held=%u/%u state=%u\n",
                        mode, cur.tf.cs, cur.tf.eip, v->dpmi.depth, v->monitor.stats.delivered[0],
                        v->monitor.stats.delivered[7], dosvm_irq_held(v, 0), dosvm_irq_held(v, 7), v->info.state);
            CHECK(!in_pm() && cur.tf.cs == 0xF000 && cur.tf.eip == DPMI_RM_RETURN);
            step(v, 13, 0);
        }
    }
    CHECK(v->info.state < DOSVM_STOPPING);
}

static void test_dospic_dmx_stalls(void)
{
    static const unsigned stalls[] = {5, 8, 11, 20};
    for (unsigned mode = 0; mode < 6; mode++) for (unsigned s = 0; s < ARRAY_SIZE(stalls); s++) {
        struct dosvm *v = dospic_vm(mode);
        device_out(v, 0x43, 0x34);
        device_out(v, 0x40, 8523 & 255); device_out(v, 0x40, 8523 >> 8);
        v->started = true; v->ticks = g_ticks;
        uint64_t edges0 = v->pit.edges, resume = 0;
        unsigned started = 0, next_stall = 0, injected = 0;
        bool open = false;
        for (unsigned t = 0; t < 5032; t++) {
            ms(1);
            if (open && g_ticks < resume) continue; /* host off CPU, clock still runs */
            if (open) {
                unsigned before = v->monitor.stats.delivered[0];
                step(v, 0x20, 0); /* first boundary on returning from the host stall */
                CHECK(v->monitor.stats.delivered[0] == before && dosvm_irq_held(v, 0));
                dospic_iret(v, mode); open = false;
            } else step(v, 0x20, 0);
            while (v->monitor.stats.delivered[0] > started) {
                CHECK(v->monitor.stats.delivered[0] == started + 1 && dosvm_irq_held(v, 0));
                started++;
                dospic_eoi_sti(v, 0); /* early EOI, then more guest work before IRET */
                CHECK(v->monitor.stats.delivered[0] == started);
                if (t < 5000 && t >= next_stall) {
                    next_stall += 100; injected++;
                    resume = g_ticks + stalls[s]; open = true;
                    break;
                }
                dospic_iret(v, mode);
            }
            CHECK(v->pit.edges - edges0 == v->monitor.stats.delivered[0] + v->pit.owed +
                  !!(v->monitor.pic[0].irr & 1) + v->pit.lost_edges);
        }
        uint64_t edges = v->pit.edges - edges0;
        printf("fx-dospic DMX mode=%u stall_ms=%u stalls=%u edges=%llu ticks=%u gametics=%u reinjected=%llu lost=%llu owed_max=%u depth_max=%u\n",
               mode, stalls[s], injected, (unsigned long long)edges, started, started / 4,
               (unsigned long long)v->pit.reinjected, (unsigned long long)v->pit.lost_edges,
               v->pit.owed_max, v->perf.isr_max_depth);
        CHECK(injected == 50 && started == edges && edges >= 700 && !open && !dosvm_irq_held(v, 0));
        CHECK(!v->pit.owed && !v->pit.lost_edges && v->pit.owed_max <= 3 && !v->perf.isr_expired);
        CHECK(v->perf.isr_max_depth <= 1);
        destroy(v, true); empty();
    }
    puts("fx-dospic DMX early EOI: 5/8/11/20 ms stalls in six delivery modes, all timer ticks recovered, no recursive IRQ0 or permanent stall PASS");
}

static void test_dospic_completion(void)
{
    for (unsigned mode = 0; mode < 6; mode++) {
        struct dosvm *v = dospic_vm(mode);
        CHECK(dosvm_irq_raise(v->info.key, 7)); step(v, 0x20, 0);
        CHECK(v->monitor.stats.delivered[7] == 1 && dosvm_irq_held(v, 7));
        dospic_eoi_sti(v, 7);
        CHECK(dosvm_irq_raise(v->info.key, 7));
        CHECK(!(v->monitor.pic[0].irr & 0x80) && (v->irq.owed & 0x80));
        /* A different IRQ may nest, even while IRQ7's request is held. */
        CHECK(dosvm_irq_raise(v->info.key, 0)); step(v, 0x20, 0);
        CHECK(v->monitor.stats.delivered[0] == 1 && v->monitor.stats.delivered[7] == 1);
        dospic_eoi_sti(v, 0); dospic_iret(v, mode);
        CHECK(!dosvm_irq_held(v, 0) && dosvm_irq_held(v, 7));
        /* A nested software IRET from another stack slot is not IRQ7's return. */
        if (mode == 0 || mode == 5) {
            v86_int(v, 0x60); CHECK(cur.tf.cs == IRET_SEG);
            op1(v, 0xCF);
            CHECK(dosvm_irq_held(v, 7) && v->monitor.stats.delivered[7] == 1);
        }
        dospic_iret(v, mode);
        CHECK(v->monitor.stats.delivered[7] == 2 && !(v->irq.owed & 0x80));
        dospic_eoi_sti(v, 7); dospic_iret(v, mode);
        CHECK(!dosvm_irq_held(v, 7) && !v->dpmi.depth);
        destroy(v, true); empty();
    }
    puts("fx-dospic IRQ completion: six modes, early EOI held until matching return, nested different IRQ and software IRET, deferred request released once PASS");
}

static void test_dospic_timeout(void)
{
    for (unsigned mode = 0; mode < 6; mode++) for (unsigned eoi = 0; eoi < 2; eoi++) {
        struct dosvm *v = dospic_vm(mode);
        CHECK(dosvm_irq_raise(v->info.key, 0)); step(v, 0x20, 0);
        uint64_t start = g_ticks;
        if (eoi) dospic_eoi_sti(v, 0);
        CHECK(dosvm_irq_raise(v->info.key, 0));
        g_ticks = start + DPMI_ISR_WAIT_MS - 1;
        dosvm_irq_release(v);
        CHECK(dosvm_irq_held(v, 0) && !(v->monitor.pic[0].irr & 1));
        g_ticks++;
        dosvm_irq_release(v);
        CHECK(!dosvm_irq_held(v, 0) && (v->monitor.pic[0].irr & 1) && !(v->irq.owed & 1));
        CHECK(!!(v->monitor.pic[0].isr & 1) == !eoi); /* expiry must never forge EOI */
        CHECK((dosvm_pic_pending(&v->monitor) == 0) == !!eoi);
        /* The expired continuation remains valid for a late real return. */
        if (!eoi) { device_out(v, 0x20, 0x20); v->monitor.vif = true; }
        step(v, 0x20, 0);
        CHECK(v->monitor.stats.delivered[0] == 2);
        dospic_eoi_sti(v, 0); dospic_iret(v, mode);
        dospic_iret(v, mode);
        CHECK(!v->dpmi.depth && !dosvm_irq_held(v, 0));
        destroy(v, true); empty();
    }
    puts("fx-dospic expiry: 49/50 ms in six modes, EOI/IF/PIC priority preserved, nested tick and late continuation return PASS");
}

static void test_dospic_pacing(void)
{
    for (unsigned mode = 0; mode < 6; mode++) {
        struct dosvm *v = dospic_vm(mode);
        device_out(v, 0x43, 0x34);
        device_out(v, 0x40, 8523 & 255); device_out(v, 0x40, 8523 >> 8);
        v->started = true; v->ticks = g_ticks;
        ms(8); step(v, 0x20, 0); dospic_eoi_sti(v, 0);
        ms(15); CHECK(v->pit.owed == 2 && !(v->monitor.pic[0].irr & 1));
        dospic_iret(v, mode);
        CHECK(v->monitor.stats.delivered[0] == 2 && v->pit.owed == 1);
        dospic_eoi_sti(v, 0); dospic_iret(v, mode);
        CHECK(!dosvm_irq_held(v, 0) && v->monitor.stats.delivered[0] == 2 && v->pit.owed == 1);
        ms(3); step(v, 0x20, 0);
        CHECK(v->monitor.stats.delivered[0] == 2 && !(v->monitor.pic[0].irr & 1));
        ms(1); step(v, 0x20, 0);
        CHECK(v->monitor.stats.delivered[0] == 3 && !v->pit.owed);
        dospic_eoi_sti(v, 0); dospic_iret(v, mode);
        destroy(v, true); empty();
    }
    puts("fx-dospic pacing: immediate return releases one owed tick, next waits the half-period in all six modes PASS");

    struct dosvm *v = dospic_vm(2);
    device_out(v, 0x43, 0x34);
    device_out(v, 0x40, 8523 & 255); device_out(v, 0x40, 8523 >> 8);
    v->started = true; v->ticks = g_ticks;
    ms(8); step(v, 0x20, 0); dospic_eoi_sti(v, 0);
    ms(40);
    CHECK(v->pit.owed == DOSVM_PIT_OWED_MAX && v->pit.lost_edges == 1);
    dospic_iret(v, 2);
    for (unsigned t = 0; t < 40; t++) {
        if (dosvm_irq_held(v, 0)) { dospic_eoi_sti(v, 0); dospic_iret(v, 2); }
        ms(1); step(v, 0x20, 0);
    }
    if (dosvm_irq_held(v, 0)) { dospic_eoi_sti(v, 0); dospic_iret(v, 2); }
    CHECK(!v->pit.owed && v->pit.edges == v->monitor.stats.delivered[0] + v->pit.lost_edges);
    destroy(v, true); empty();
    puts("fx-dospic overflow: four owed ticks retained, excess counted once, backlog drains without stall PASS");
}

static void test_dospic_cancellation(void)
{
    struct dosvm *v = dospic_vm(0);
    device_out(v, 0x21, 0xFD); /* IRQ1 */
    CHECK(dosvm_kbd_event(v, 0x1E, 1)); step(v, 0x20, 0);
    CHECK(dosvm_irq_held(v, 1)); dospic_eoi_sti(v, 1);
    CHECK(dosvm_kbd_event(v, 0x30, 1) && (v->irq.owed & 2));
    CHECK(device_in(v, 0x60) == 0x1E && device_in(v, 0x60) == 0x30);
    CHECK(!(v->irq.owed & 2)); dospic_iret(v, 0);
    CHECK(v->monitor.stats.delivered[1] == 1 && !v->monitor.pic[0].irr);
    destroy(v, true); empty();

    v = dospic_vm(2);
    CHECK(dosvm_irq_raise(v->info.key, 0)); step(v, 0x20, 0); dospic_eoi_sti(v, 0);
    CHECK(dosvm_irq_raise(v->info.key, 0) && (v->irq.owed & 1));
    device_out(v, 0x43, 0x34); CHECK(!(v->irq.owed & 1));
    CHECK(dosvm_irq_raise(v->info.key, 0));
    device_pic_reset(v, 1); CHECK(!v->irq.owed);
    dospic_iret(v, 2);
    CHECK(v->monitor.stats.delivered[0] == 1);
    destroy(v, true); empty();
    puts("fx-dospic cancellation: keyboard drain, PIT reprogram and PIC ICW1 cancel held requests, no stale delivery PASS");
}

static void test_dospic_exact_return(void)
{
    for (unsigned mode = 1; mode <= 2; mode++) {
        struct dosvm *v = dospic_vm(mode);
        CHECK(dosvm_irq_raise(v->info.key, 0)); step(v, 0x20, 0);
        struct v86_frame saved = v->dpmi.rec[0].saved;
        dospic_eoi_sti(v, 0);
        CHECK(dosvm_irq_raise(v->info.key, 0) && !(v->monitor.pic[0].irr & 1));
        cur = saved; step(v, 0x20, 0); /* guest chained to its exact interrupted context */
        CHECK(v->perf.isr_retired == 1 && v->monitor.stats.delivered[0] == 2 && !v->irq.owed);
        dospic_eoi_sti(v, 0); dospic_iret(v, mode);
        CHECK(!v->dpmi.depth && !dosvm_irq_held(v, 0));
        destroy(v, true); empty();
    }
    puts("fx-dospic exact native return: 16/32-bit resynchronization releases held request without host trampoline PASS");
}

static void test_dospic(void)
{
    test_dospic_dmx_stalls();
    test_dospic_completion();
    test_dospic_timeout();
    test_dospic_pacing();
    test_dospic_cancellation();
    test_dospic_exact_return();
}
