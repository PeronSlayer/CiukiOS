/* f3-09: production trap path with Pentium III DR6/IRQ interleavings. */
static void test_timer_debug_status(void)
{
    for (unsigned bits = 0; bits < 2; bits++) {
        struct dosvm *v = dpmi_vm(true);
        v86_start(); enter(v, bits != 0);
        uint16_t code = (uint16_t)cur.tf.cs;
        v->dpmi.exc[1] = (struct dpmi_vec){.sel=code, .off=0x400};
        for (unsigned form = 0; form < 3; form++) {
            cur.tf.eip = 0x300; cur.tf.eflags = V86_IF | 2;
            v->monitor.vif = false; v->dpmi.if_armed = true;
            /* Disabled B bits may accompany BS. A used but disabled slot
             * also cannot turn this host restoration into a client trap. */
            v->dpmi.watch[0].used = form == 1;
            v->dpmi.dr7 = 0;
            debug_dr6 = 0xFFFF0FF0u | (1u << 14) | (form == 2 ? 0 : 15);
            unsigned exceptions = v->dpmi.stats.exceptions;
            step(v, 1, 0);
            CHECK(!left && cur.tf.eip == 0x300 && v->monitor.vif && !v->dpmi.if_armed);
            CHECK(v->dpmi.stats.exceptions == exceptions && !debug_dr6 && !v->dpmi.watch[0].hit);
        }
        /* Genuine enabled breakpoint + BS: restore IF but still invoke the
         * client's exception handler, whose stacked flags must show IF=1. */
        v->dpmi.watch[0] = (struct dpmi_watch){.used=true, .owner=1, .type=1, .size=1};
        v->dpmi.dr7 = 1;
        v->monitor.vif = false; v->dpmi.if_armed = true;
        cur.tf.eip = 0x300; debug_dr6 = (1u << 14) | 15;
        step(v, 1, 0);
        CHECK(!left && cur.tf.eip == 0x400 && v->dpmi.watch[0].hit && !v->dpmi.if_armed);
        uint32_t image = 0;
        CHECK(!dosvm_read(v, v->dpmi.hstack.base + cur.tf.user_esp + 5 * dpmi_width(v), &image, dpmi_width(v)));
        CHECK((image & V86_IF) && !(image & V86_TF));
        /* A native task can leave BS sticky. Scheduling this VM drains it
         * before a breakpoint-only trap, which must leave the flags image
         * armed and must not restore virtual IF. */
        dpmi_unwind(v, 0); cur.tf.user_ss = v->dpmi.rec[0].saved.tf.user_ss; cur.tf.user_esp = 0x800;
        cur.tf.cs = code; cur.tf.eip = 0x300; cur.tf.eflags = V86_IF | 2;
        v->monitor.vif = false; v->dpmi.if_armed = true;
        debug_dr6 = 1u << 14; dosvm_task_switch(v->task);
        CHECK(!debug_dr6);
        debug_dr6 |= 1; step(v, 1, 0);
        CHECK(!left && cur.tf.eip == 0x400 && v->dpmi.if_armed && !v->monitor.vif);
        destroy(v, true); empty();
    }
    puts("f3-09 debug status: disabled B bits and previous-task BS ignored, enabled hit preserved, BS/TF-clear restore PASS");
}
static void test_timer_irq_flags_boundary(void)
{
    for (unsigned bits = 0; bits < 2; bits++) {
        struct dosvm *v = dpmi_vm(true);
        v86_start(); enter(v, bits != 0);
        uint32_t base = seg_base(v, (uint16_t)cur.tf.cs);
        static const uint8_t save[] = {0x9C, 0x58, 0xFA};
        CHECK(!dosvm_write(v, base + 0x200, save, sizeof(save)));
        cur.tf.eip = 0x200; step(v, 0x20, 0);
        cur.tf.eip = 0x201; step(v, 0x20, 0); /* IRQ between PUSHF and POP */
        cur.tf.eip = 0x202; step(v, 0x20, 0); /* IRQ between POP and CLI */
        cur.tf.eflags = cur.tf.eax = V86_IF | 2;
        step(v, 13, 0);
        CHECK(!left && v->dpmi.if_armed && !v->monitor.vif && (cur.tf.eax & V86_TF));
        /* fx-doomwin: the proven site is cached and wants no seed; each
         * scenario below starts unproven again, so its CLI needs the
         * boundary the IRQ return keeps. */
        CHECK(v->dpmi.cli.proved == 1 && !dpmi_seed_wanted(v));
        dpmi_cli_sites_reset(v);
        /* The same proof survives an actual PM IRQ handler and its IRET. */
        v->dpmi.if_armed = false; v->monitor.vif = true;
        v->dpmi.vec[8] = (struct dpmi_vec){.sel=(uint16_t)cur.tf.cs, .off=0x700};
        cur.tf.eip = 0x200; step(v, 0x20, 0);
        cur.tf.eip = 0x202; cur.tf.eax = cur.tf.eflags = V86_IF | 2;
        CHECK(dosvm_irq_raise(v->info.key, 0)); step(v, 0x20, 0);
        CHECK(!left && cur.tf.eip == 0x700 && v->dpmi.depth == 1);
        cur.tf.eax = 0x20; uint8_t eoi[] = {0xE6, 0x20}; exec(v, eoi, 2, 13, 0);
        pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
        CHECK(cur.tf.eip == 0x202 && v->monitor.vif && v->dpmi.decode_eip == 0x200);
        step(v, 13, 0);
        CHECK(!left && v->dpmi.if_armed && (cur.tf.eax & V86_TF));
        dpmi_cli_sites_reset(v);
        /* A reflected real-mode IRQ preserves the same interrupted proof. */
        v->dpmi.if_armed = false; v->monitor.vif = true;
        v->dpmi.vec[8] = (struct dpmi_vec){DPMI_PM_INT + 8, DPMI_HOST_CS};
        uint16_t ivt[] = {0, IRET_SEG}; CHECK(!dosvm_write(v, 32, ivt, sizeof(ivt)));
        cur.tf.eip = 0x200; step(v, 0x20, 0);
        cur.tf.eip = 0x202; cur.tf.eax = cur.tf.eflags = V86_IF | 2;
        CHECK(dosvm_irq_raise(v->info.key, 0)); step(v, 0x20, 0);
        CHECK(left && !in_pm() && cur.tf.cs == IRET_SEG);
        device_out(v, 0x20, 0x20); op1(v, 0xCF);
        CHECK(cur.tf.cs == DPMI_RSTACK_SEG || cur.tf.cs == 0xF000);
        step(v, 13, 0);
        CHECK(in_pm() && cur.tf.eip == 0x202 && v->dpmi.decode_eip == 0x200);
        step(v, 13, 0);
        CHECK(!left && v->dpmi.if_armed && (cur.tf.eax & V86_TF));
        dpmi_cli_sites_reset(v);
        /* A transfer breaks the forward proof: acquire the IRQ boundary
         * rather than treating bytes embedded in an operand as PUSHF. */
        static const uint8_t jump[] = {0xEB, 0x01, 0x9C, 0x90, 0xFA};
        CHECK(!dosvm_write(v, base + 0x500, jump, sizeof(jump)));
        v->dpmi.if_armed = false; v->monitor.vif = true;
        cur.tf.eip = 0x500; step(v, 0x20, 0);
        cur.tf.eip = 0x503; step(v, 0x20, 0);
        CHECK(v->dpmi.decode_eip == 0x503);
        cur.tf.eip = 0x504; cur.tf.eax = V86_IF | 2; step(v, 13, 0);
        CHECK(!v->dpmi.if_armed && cur.tf.eax == (V86_IF | 2));
        destroy(v, true); empty();
    }
    puts("f3-09 IRQ flags boundary: physical/PM/reflected IRQ returns retain proof, transfers invalidate it PASS");
}
static void test_timer_diagnostics(void)
{
    struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, true);
    unsigned before = records;
    uint16_t code = (uint16_t)cur.tf.cs;
    cur.tf.ebx = 8; cur.tf.ecx = code; cur.tf.edx = 0x700; int31(v, 0x0205);
    CHECK(records == before && !v->trace.irq_epochs);
    g_boot.flags = CBI_F_VERBOSE;
    int31(v, 0x0205);
    CHECK(v->trace.irq_epochs == 1 && strstr(last_record, "group=vm_vec") && strstr(last_record, "api=0205 vec=08"));
    cur.tf.ebx = 8; cur.tf.ecx = IRET_SEG; cur.tf.edx = 0; int31(v, 0x0201);
    CHECK(strstr(last_record, "api=0201 vec=08"));
    uint32_t control = 0xDEAD0036u;
    CHECK(!dosvm_io(v, 0x43, 1, true, &control));
    CHECK(strstr(last_record, "group=vm_pit") && strstr(last_record, "byte=36"));
    device_out(v, 0x40, 0x4B); device_out(v, 0x40, 0x21);
    CHECK(strstr(last_record, "group=vm_pit") && strstr(last_record, "pending=8523 half=0"));
    CHECK(v->trace.used[DOSVM_TRACE_PIT] == 3);
    for (unsigned i = 0; i < 12; i++) {
        CHECK(dosvm_irq_raise(v->info.key, 0)); step(v, 0x20, 0);
        CHECK(cur.tf.eip == 0x700 && !v->monitor.vif);
        cur.tf.eax = 0x20; uint8_t eoi[] = {0xE6, 0x20}; exec(v, eoi, sizeof(eoi), 13, 0);
        pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
    }
    CHECK(v->trace.used[DOSVM_TRACE_IRQ0] == 10 && v->monitor.stats.delivered[0] == 12);
    for (unsigned i = 0; i < 36; i++) {
        cur.tf.eflags = V86_IF | 2; v->dpmi.if_armed = true; v->monitor.vif = false;
        debug_dr6 = 0xFFFF4FFFu; step(v, 1, 0);
    }
    CHECK(v->trace.used[DOSVM_TRACE_DB_PM] == 32 && !v->dpmi.if_armed && v->monitor.vif);
    uint32_t gen = v->info.key.generation;
    v->info.key.generation = UINT32_MAX;
    v->info.fault_vector = 14; v->info.fault_eip = v->info.fault_address = v->info.fault_error = UINT32_MAX;
    dosvm_trace(v, DOSVM_TRACE_FAULT, &cur, 0, 0, 0);
    CHECK(strstr(fault_pc_record, "group=vm_fault_pc") && strstr(fault_pc_record, "eip=") &&
          strstr(fault_pc_record, "linear=ffffffff") && strstr(fault_pc_record, "depth=0 kind=0 flags=0"));
    v->info.key.generation = gen;
    before = records; g_boot.flags = 0;
    dosvm_trace(v, DOSVM_TRACE_FLAGS, &cur, 1, 1, 0);
    CHECK(records == before);
    destroy(v, true); empty();
    puts("f3-09 diagnostics: quiet off, vector/PIT PCs, first 10 IRQ0, bounded raw DR6/IF/fault rows and enabled host IF PASS");
}
static void test_timer_call_flags_boundary(void)
{
    for (unsigned bits = 0; bits < 2; bits++) {
        struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, bits != 0);
        unsigned w = dpmi_width(v); uint32_t base = seg_base(v, (uint16_t)cur.tf.cs);
        if (bits) v->dpmi.ldt[cur.tf.cs >> 3][1] |= 1u << 22;
        uint8_t call[5] = {0xE8}; uint32_t rel = 0x200u - (0x600u + 1 + w);
        memcpy(call + 1, &rel, w);
        static const uint8_t root[] = {0x58, 0x5A, 0xC3};
        static const uint8_t save[] = {0x9C, 0x58, 0xFA};
        CHECK(!dosvm_write(v, base + 0x500, root, sizeof(root)) &&
              !dosvm_write(v, base + 0x600, call, 1 + w) &&
              !dosvm_write(v, base + 0x200, save, sizeof(save)));
        cur.tf.user_esp = 0x700;
        uint32_t sp = cur.tf.user_esp;
        pm_push(v, 0x600); pm_push(v, 0); pm_push(v, 0);
        v->dpmi.decode_valid = false; cur.tf.eip = 0x500; step(v, 0x20, 0);
        CHECK(v->dpmi.decode_stack_valid);
        cur.tf.user_esp = sp - w; cur.tf.eip = 0x202; cur.tf.eax = cur.tf.eflags = V86_IF | 2;
        struct dpmi_cli_stats proof = v->dpmi.cli;
        step(v, 13, 0);
        CHECK(!left && v->dpmi.if_armed && (cur.tf.eax & V86_TF));
        /* fx-doomwin: a control-flow proof is cached like a forward proof
         * and restarts no seed demand; its next execution is a hit. */
        CHECK(v->dpmi.cli.control == proof.control + 1 && !dpmi_seed_wanted(v));
        v->dpmi.if_armed = false; v->monitor.vif = true; v->dpmi.decode_valid = false;
        cur.tf.user_esp = sp - w; cur.tf.eip = 0x202; cur.tf.eax = cur.tf.eflags = V86_IF | 2;
        step(v, 13, 0);
        CHECK(!left && v->dpmi.if_armed && (cur.tf.eax & V86_TF) &&
              v->dpmi.cli.hits == proof.hits + 1 && v->dpmi.cli.control == proof.control + 1);
        v->dpmi.if_armed = false; dpmi_cli_sites_reset(v);
        /* A store could replace a snapshotted root return: never use that
         * cached target, even when it would lead to a save byte pattern. */
        uint8_t changed[] = {0x89, 0x00, 0x58, 0x5A, 0xC3};
        CHECK(!dosvm_write(v, base + 0x500, changed, sizeof(changed)));
        cur.tf.user_esp = sp; pm_push(v, 0x600); pm_push(v, 0); pm_push(v, 0);
        v->monitor.vif = true; v->dpmi.if_armed = false;
        v->dpmi.decode_valid = false; cur.tf.eip = 0x500; step(v, 0x20, 0);
        cur.tf.user_esp = sp - w; cur.tf.eip = 0x202; cur.tf.eax = cur.tf.eflags = V86_IF | 2;
        struct dpmi_cli_stats was = v->dpmi.cli;
        step(v, 13, 0);
        /* fx-16: the control proof still refuses the cached target; the
         * Windows 95 rule arms on the genuine PUSHF; POP EAX before the CLI
         * (no decode reached it, so nothing disproves the pattern). */
        CHECK(!left && v->dpmi.cli.control == was.control && v->dpmi.cli.pattern == was.pattern + 1 &&
              v->dpmi.if_armed && (cur.tf.eax & V86_TF));
        v->dpmi.if_armed = false; dpmi_cli_sites_reset(v);
        /* An absolute DS store is provably outside the snapshotted stack. */
        uint8_t absolute[9] = {0x89, bits ? 0x05 : 0x06};
        uint32_t off = 0x10; memcpy(absolute + 2, &off, w);
        memcpy(absolute + 2 + w, root, sizeof(root));
        CHECK(!dosvm_write(v, base + 0x500, absolute, 2 + w + sizeof(root)));
        cur.tf.user_esp = sp; pm_push(v, 0x600); pm_push(v, 0); pm_push(v, 0);
        v->monitor.vif = true; v->dpmi.if_armed = false; v->dpmi.decode_valid = false;
        cur.tf.eip = 0x500; step(v, 0x20, 0);
        cur.tf.user_esp = sp - w; cur.tf.eip = 0x202; cur.tf.eax = cur.tf.eflags = V86_IF | 2;
        step(v, 13, 0);
        CHECK(!left && v->dpmi.if_armed && (cur.tf.eax & V86_TF));
        v->dpmi.if_armed = false; dpmi_cli_sites_reset(v);   /* fx-doomwin: unproven again */
        /* POP of the call return followed by a different PUSH forfeits the
         * return proof; it cannot lead to a caller's save/CLI by guessing. */
        static const uint8_t forged[] = {0x58, 0x53, 0xC3};
        CHECK(!dosvm_write(v, base + 0x200, forged, sizeof(forged)));
        CHECK(!dosvm_write(v, base + 0x600 + 1 + w, save, sizeof(save)));
        cur.tf.user_esp = sp; v->monitor.vif = true; v->dpmi.if_armed = false; v->dpmi.decode_valid = false;
        cur.tf.eip = 0x600; step(v, 0x20, 0);
        cur.tf.eip = 0x600 + 1 + w + 2; cur.tf.eax = cur.tf.eflags = V86_IF | 2;
        was = v->dpmi.cli;
        step(v, 13, 0);
        CHECK(!left && v->dpmi.cli.control == was.control && v->dpmi.cli.pattern == was.pattern + 1 &&
              v->dpmi.if_armed && (cur.tf.eax & V86_TF));   /* fx-16, as above */
        v->dpmi.if_armed = false; dpmi_cli_sites_reset(v);
        if (bits) {
            /* rel16 CALL from a high 32-bit PC pushes only a 16-bit return
             * offset. RET16 cannot prove a save at the high fall-through. */
            dpmi_desc_set(v->dpmi.ldt[cur.tf.cs >> 3], base, 0x1FFFF, 0xFA, 0x40);
            static const uint8_t call16[] = {0x66, 0xE8, 0xFC, 0xFC, 0x9C, 0x58, 0xFA};
            static const uint8_t ret16[] = {0x66, 0xC3};
            CHECK(!dosvm_write(v, base + 0x10500, call16, sizeof(call16)) &&
                  !dosvm_write(v, base + 0x200, ret16, sizeof(ret16)));
            cur.tf.user_esp = sp; v->monitor.vif = true; v->dpmi.if_armed = false; v->dpmi.decode_valid = false;
            cur.tf.eip = 0x10500; step(v, 0x20, 0);
            cur.tf.eip = 0x10506; cur.tf.eax = cur.tf.eflags = V86_IF | 2;
            was = v->dpmi.cli; step(v, 13, 0);
            CHECK(!left && v->dpmi.cli.control == was.control && v->dpmi.cli.proved == was.proved &&
                  v->dpmi.cli.pattern == was.pattern + 1 && v->dpmi.if_armed && (cur.tf.eax & V86_TF));
        }
        destroy(v, true); empty();
    }
    puts("f3-09 call flags boundary: checked RET/backward CALL, absolute-store bounds, unknown stores/overwritten returns rejected PASS");
}
static void test_timer(void)
{
    const char *only = getenv("F3_TIMER");
    if (!only || !strcmp(only, "debug")) test_timer_debug_status();
    if (!only || !strcmp(only, "boundary")) test_timer_irq_flags_boundary();
    if (!only || !strcmp(only, "diagnostics")) test_timer_diagnostics();
    if (!only || !strcmp(only, "calls")) test_timer_call_flags_boundary();
}
