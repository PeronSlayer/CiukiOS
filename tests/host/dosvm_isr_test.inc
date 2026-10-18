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
        /* A higher-priority pending IRQ must not single-step a hardware
         * handler, or force IF even after the ordinary recovery timeout. */
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
