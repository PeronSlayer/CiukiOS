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
        /* fx-doomwin: the proven site is cached and wants no seed. The case
         * below was written when every proof restarted the demand: forget
         * the site so its seed (a live near-return slot) is wanted again. */
        CHECK(!dpmi_seed_wanted(v));
        dpmi_cli_sites_reset(v);
        /* POPF; NOP retired: the saved-image #DB and a PIT edge coincide.
         * A live near-return slot seeds the sparse return checkpoint. */
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
              !(v->dpmi.rec[0].saved.tf.eflags & V86_TF) && v->dpmi.return_slot == DPMI_WATCH);
        /* Another edge while in service must wait; EOI is still emulated
         * with boundary/shadow state present and IF clear. */
        CHECK(dosvm_irq_raise(v->info.key, 0));
        v->dpmi.shadow = true;
        v->dpmi.if_armed = true;
        cur.tf.eax = 0x20; const uint8_t eoi[] = {0xE6, 0x20};
        exec(v, eoi, sizeof(eoi), 13, 0);
        CHECK(!v->monitor.pic[0].isr && !(v->monitor.pic[0].irr & 1) &&
              (v->irq.owed & 1) && cur.tf.eip == 0x702);
        v->dpmi.if_armed = false;
        pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
        CHECK(cur.tf.eip == 0x700 && v->dpmi.depth == 1 && v->monitor.stats.delivered[0] == 2);
        cur.tf.eax = 0x20; exec(v, eoi, sizeof(eoi), 13, 0);
        pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
        CHECK(cur.tf.eip == 0x340 && !v->dpmi.depth && !v->monitor.pic[0].isr &&
              v->monitor.vif && !(cur.tf.eflags & V86_TF) && v->dpmi.return_slot < DPMI_WATCH);
        destroy(v, true); empty();
    }
    puts("f3-11/f3-34 restore/IRQ replay: arm, BS at restore + PIT edge, return checkpoint suspended, handler EOI with armed/shadow state, pending IRQ and both returns PASS");
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
    CHECK(!v->dpmi.if_armed && v->monitor.shadow && !(cur.tf.eflags & V86_TF));
    cur.tf.eip = 0x102; step(v, 0x20, 0); /* later timer observes the retired successor */
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
static struct dosvm *timed_diag_vm;
static unsigned timed_diag_writes;
static void irq_diag_time(void)
{
    /* Real IRQs remain enabled during UART writes. The PIT is allowed to
     * produce another edge, but a guest IRQ must not be suspended here. */
    CHECK(!dosvm_in_irq(timed_diag_vm));
    timed_diag_writes++;
    g_ticks += 60;
    dosvm_pit_advance(timed_diag_vm, 60);
}

static void test_irq_trace_latency(void)
{
    struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, true);
    uint16_t code = (uint16_t)cur.tf.cs;
    v->dpmi.vec[8] = (struct dpmi_vec){.sel=code, .off=0x700};
    g_boot.flags = CBI_F_VERBOSE;
    timed_diag_vm = v; timed_diag_writes = 0; diag_time_hook = irq_diag_time;
    unsigned before = records;
    CHECK(dosvm_irq_raise(v->info.key, 0)); step(v, 0x20, 0);
    CHECK(cur.tf.eip == 0x700 && v->dpmi.depth == 1 && !timed_diag_writes && records == before);
    v->trace.timer_programmed = true; /* retain the handler's 16-bit CLI sample */
    op1(v, 0xFA); /* capture CLI without spending a UART period in the IRQ */
    CHECK(!timed_diag_writes && v->deferred_trace.count == 2);
    /* Early EOI does not end the guest IRQ context. Its nested real-mode
     * call, callback, or child must not make the diagnostic flush early. */
    cur.tf.eax = 0x20; const uint8_t eoi[] = {0xE6, 0x20}; exec(v, eoi, sizeof(eoi), 13, 0);
    struct dpmi_record *r;
    CHECK(!dpmi_push_record(v, &cur, DPMI_REC_REALCALL, &r));
    dosvm_diag(v, "group=defer_test stage=nested");
    dosvm_trace_flush(v); CHECK(!timed_diag_writes && v->deferred_trace.count == 3);
    dpmi_unwind(v, 1);
    /* Drop excess diagnostics explicitly, never flush inside an IRQ. */
    for (unsigned i = 0; i < 9; i++) dosvm_diag(v, "group=defer_test stage=full n=%u", i);
    CHECK(v->deferred_trace.count == 8 && v->deferred_trace.dropped == 4 && records == before);
    pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
    CHECK(timed_diag_writes == 9 && records == before + 9 && !v->deferred_trace.dropped);
    CHECK(strstr(last_record, "group=vm_trace_drop") && strstr(last_record, "count=4"));
    /* Flush generated real PIT edges. A new IRQ is delivered after the
     * outer return, on a fresh frame; its entry diagnostic is queued. */
    CHECK(v->monitor.stats.delivered[0] == 2 && v->dpmi.depth == 1 && cur.tf.eip == 0x700 &&
          v->monitor.pic[0].isr == 1 && v->deferred_trace.count == 1);
    diag_time_hook = 0;
    cur.tf.eax = 0x20; exec(v, eoi, sizeof(eoi), 13, 0); pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
    /* fx-dospic: the completed handler releases one tick already paced
     * during the UART flush. Further owed ticks still wait half a period. */
    CHECK(v->monitor.stats.delivered[0] == 3 && v->dpmi.depth == 1 && cur.tf.eip == 0x700);
    cur.tf.eax = 0x20; exec(v, eoi, sizeof(eoi), 13, 0); pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
    CHECK(!v->dpmi.depth && !v->deferred_trace.count && !v->monitor.pic[0].isr);
    /* Quiet boots allocate no queue entries. */
    g_boot.flags = 0; before = records;
    dosvm_diag(v, "group=defer_test quiet=1"); CHECK(records == before && !v->deferred_trace.count);
    destroy(v, true); empty(); timed_diag_vm = 0;
    puts("f3-12 timed diagnostics: no UART wait inside guest IRQ/nested call even after early EOI, bounded snapshots/drop count, PIT edges delivered after outer IRET PASS");
}

static void test_irq_target_diagnostics(void)
{
    struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, true);
    uint16_t code = (uint16_t)cur.tf.cs;
    g_boot.flags = CBI_F_VERBOSE;
    v->trace.used[DOSVM_TRACE_VECTOR] = 255;
    unsigned logs = dosvm_logs;
    for (unsigned vec = 8; vec <= 15; vec++) {
        cur.tf.ebx = vec; cur.tf.ecx = code; cur.tf.edx = 0x700; int31(v, 0x0205);
        CHECK(!cf() && dosvm_logs == ++logs && strstr(last_dosvm_log, "event=pm_irq_vector") &&
              strstr(last_dosvm_log, "valid=1 present=1 live=1") && strstr(last_dosvm_log, "kind=1 owner=1"));
    }
    /* Successful repeated sets are all logged, including the host stub. */
    cur.tf.ebx = 8; cur.tf.ecx = DPMI_HOST_CS; cur.tf.edx = 8; int31(v, 0x0205);
    CHECK(!cf() && dosvm_logs == ++logs && strstr(last_dosvm_log, "to=1fff:00000008"));
    /* Never-allocated, freed, non-present, data and out-of-limit targets:
     * delivery logs the rejected descriptor and leaves the PIC untouched. */
    for (unsigned failure = 0; failure < 5; failure++) {
        uint16_t sel = DPMI_SEL(900);
        if (failure) client_desc(v, 900, 0x30000, 0xFFFF, failure == 2 ? 0x7A : failure == 3 ? 0xF2 : 0xFA, 0);
        if (failure == 1) dpmi_free_ldt(v, 900);
        v->dpmi.vec[8] = (struct dpmi_vec){.sel=sel, .off=failure == 4 ? 0x10000 : 0x20};
        CHECK(dosvm_irq_raise(v->info.key, 0));
        CHECK(dpmi_deliver(v, &cur) == -EFAULT && dosvm_logs == ++logs &&
              strstr(last_dosvm_log, "event=pm_irq_invalid") && strstr(last_dosvm_log, "live=0"));
        CHECK(v->monitor.pic[0].irr == 1 && !v->monitor.pic[0].isr && !v->dpmi.depth);
        if (failure >= 2) dpmi_free_ldt(v, 900);
    }
    g_boot.flags = 0;
    CHECK(dpmi_deliver(v, &cur) == -EFAULT && dosvm_logs == logs);
    destroy(v, true); empty();
    puts("f3-12 target diagnostics: every 0205h 08-0F past startup budget, repeat/default, unallocated/freed/non-present/data/limit rejection, quiet boot and unchanged PIC PASS");
}

static void test_if_stall_watchdog(void)
{
    struct dosvm *v=dpmi_vm(true); v86_start(); enter(v,true);
    uint16_t code=(uint16_t)cur.tf.cs; uint32_t base=seg_base(v,code);
    uint8_t spin[16]={0xEB,0xFE}; uint32_t image[3]={0x202,0x12345678,0xA5A5A5A5};
    CHECK(!dosvm_write(v,base+0x300,spin,sizeof(spin)));
    uint32_t stack=seg_base(v,(uint16_t)cur.tf.user_ss)+cur.tf.user_esp;
    CHECK(!dosvm_write(v,stack,image,sizeof(image)));
    v->dpmi.vec[8]=(struct dpmi_vec){.sel=code,.off=0x700};
    cur.tf.eip=0x300; cur.tf.eflags=V86_IF|2; v->monitor.vif=false;
    v->dpmi.decode_valid=true; v->dpmi.decode_cs=code; v->dpmi.decode_eip=0x248205;
    g_boot.flags=CBI_F_VERBOSE;
    CHECK(dosvm_irq_raise(v->info.key,0)); step(v,0x20,0);
    CHECK(v->dpmi.if_waiting && v->dpmi.if_step && !v->monitor.stats.delivered[0]);
    unsigned before=records; uint64_t began=v->dpmi.if_wait_tick;
    v->trace.used[DOSVM_TRACE_TIMER]=255;
    v->trace.used[DOSVM_TRACE_IRQ0]=255;
    g_ticks=began+DPMI_IF_WAIT_MS-1; step(v,0x20,0);
    CHECK(records==before && !v->monitor.vif && !v->dpmi.depth);
    g_ticks=began+DPMI_IF_WAIT_MS; step(v,0x20,0);
    CHECK(records==before+2 && v->dpmi.if_records==1 && !v->dpmi.if_waiting && !v->dpmi.if_step &&
          cur.tf.eip==0x700 && v->monitor.stats.delivered[0]==1 && v->monitor.pic[0].isr==1);
    CHECK(strstr(recent_records[before%8],"group=vm_if_stall") &&
          strstr(recent_records[before%8],"vif=0 arm=0") && strstr(recent_records[before%8],"ms=2") &&
          strstr(recent_records[before%8],"reason=time"));
    CHECK(strstr(recent_records[(before+1)%8],"code=ebfe0000000000000000000000000000") &&
          strstr(recent_records[(before+1)%8],"stack=00000202,12345678,a5a5a5a5"));
    g_ticks++; step(v,0x20,0); CHECK(v->dpmi.if_records==1); /* same IRQ episode */
    /* Full-width generation/sequence/age must fit the 240-byte envelope. */
    uint32_t generation=v->info.key.generation; unsigned saved_records=records;
    unsigned saved_state=v->info.state; v->info.state=DOSVM_STOPPING; /* dump outside resumable IRQ work */
    v->info.key.generation=UINT32_MAX; records=UINT32_MAX-2;
    if_recovery_record(v,&cur,UINT64_MAX,"budget");
    CHECK(strstr(last_record,"gen=4294967295") && strstr(last_record,"path=trap"));
    records=saved_records; v->info.key.generation=generation; v->info.state=saved_state;
    g_boot.flags=0; destroy(v,true); empty();
    /* Instruction bound even when host time is fixed; masked requests do
     * not start a wait. Recovery BS must not masquerade as saved-image BS. */
    v=dpmi_vm(true); v86_start(); enter(v,true); base=seg_base(v,(uint16_t)cur.tf.cs);
    CHECK(!dosvm_write(v,base+0x300,spin,sizeof(spin)));
    v->dpmi.vec[8]=(struct dpmi_vec){.sel=cur.tf.cs,.off=0x700};
    cur.tf.eip=0x300; cur.tf.eflags=V86_IF|2; v->monitor.vif=false; v->dpmi.if_armed=true;
    v->monitor.pic[0].imr=0xFF; CHECK(dosvm_irq_raise(v->info.key,0)); step(v,0x20,0);
    CHECK(!v->dpmi.if_waiting && !v->dpmi.if_step);
    v->monitor.pic[0].imr=0xFE; step(v,0x20,0);
    /* fx-16: an armed image reports its own restore: no recovery step. */
    CHECK(v->dpmi.if_waiting && !v->dpmi.if_step && !v->monitor.vif && !v->monitor.stats.delivered[0] &&
          !(cur.tf.eflags & V86_TF));
    v->dpmi.if_armed=false; step(v,0x20,0);    /* unarmed: the budgeted steps */
    for(unsigned i=0;i<DPMI_IF_STEPS;i++) {
        CHECK(v->dpmi.if_step && !v->monitor.vif && !v->monitor.stats.delivered[0]);
        debug_dr6=1u<<14; step(v,1,0);
    }
    CHECK(!v->monitor.vif && !v->monitor.stats.delivered[0] && !v->dpmi.if_step);
    step(v,0x20,0); CHECK(!v->dpmi.if_step && !v->monitor.vif); /* bounded tracing, no early enable */
    v->dpmi.if_armed=true;                      /* a patched image owner across the forced restore */
    g_ticks += DPMI_IF_WAIT_MS; step(v,0x20,0);
    CHECK(cur.tf.eip==0x700 && v->monitor.stats.delivered[0]==1 && !v->dpmi.if_step &&
          !v->dpmi.stats.if_restored && v->dpmi.if_armed); /* patched image owner survives */
    destroy(v,true); empty();
    puts("f3-44 watchdog: 1/2ms snapshot once, code/stack/PIC/seed, original gate then handler commit; masked IRQ waits; 4096 BS budget with fixed time and saved-image ownership PASS");
}
static void test_if_trace_barriers(void)
{
    static const uint8_t unsafe[][4]={{0x9C},{0xF3,0x9C},{0x8E,0xD0},{0x17},{0xCD,0x31},{0xEA}};
    for(unsigned i=0;i<ARRAY_SIZE(unsafe)+2;i++) {
        struct dosvm *v=dpmi_vm(true); v86_start(); enter(v,true);
        uint32_t base=seg_base(v,(uint16_t)cur.tf.cs);
        uint8_t nop=0x90;
        CHECK(!dosvm_write(v,base+0x300,i<ARRAY_SIZE(unsafe)?unsafe[i]:&nop,i<ARRAY_SIZE(unsafe)?4:1));
        cur.tf.eip=0x300; cur.tf.eflags=V86_IF|2; v->monitor.vif=false;
        if(i==ARRAY_SIZE(unsafe)) cur.tf.eflags|=V86_TF;
        if(i==ARRAY_SIZE(unsafe)+1) v->dpmi.dr7=1;
        CHECK(dosvm_irq_raise(v->info.key,0)); step(v,0x20,0);
        CHECK(!v->dpmi.if_step && !v->monitor.vif &&
              !!(cur.tf.eflags&V86_TF)==(i==ARRAY_SIZE(unsafe)));
        destroy(v,true); empty();
    }
    puts("f3-15 trace barriers: PUSHFD/prefixed save, SS load/shadow, INT/far transfer, client TF/watchpoint preserved PASS");
}
/* Hardware snapshot replay. CPU frames model the extender's LSS to a
 * private stack; actual trapped OUTs and all IRQ transactions are production
 * code. This reproduces the register state, not the T23 instruction schedule. */
static void test_irq_private_stack(void)
{
    for (unsigned bits = 0; bits < 2; bits++) for (unsigned big = 0; big < 2; big++) {
        struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, bits != 0);
        unsigned w = dpmi_width(v);
        uint16_t code = (uint16_t)cur.tf.cs;
        v->dpmi.vec[15] = (struct dpmi_vec){.sel=code, .off=0x700};
        v->dpmi.vec[8] = (struct dpmi_vec){.sel=code, .off=0x720};
        device_out(v, 0x21, 0x78);
        CHECK(dosvm_irq_raise(v->info.key, 7)); step(v, 0x20, 0);
        CHECK(v->dpmi.depth == 1 && v->monitor.pic[0].isr == 0x80);
        /* The outer wrapper saves an EOI value before LSS. The former
         * host-stack cursor still points ABOVE this live saved register. */
        pm_push(v, 0x20);
        uint32_t saved_sp = cur.tf.user_esp, saved_top = v->dpmi.lock_top;
        uint8_t saved[32], actual[32];
        uint32_t host = v->dpmi.hstack.base + saved_sp - 24;
        CHECK(!dosvm_read(v, host, saved, sizeof(saved)));
        client_desc(v, 902, 0x30000, 0x1FFFF, 0xF2, big ? 0x40 : 0);
        uint16_t private_ss = DPMI_SEL(902);
        cur.tf.user_ss = private_ss; cur.tf.user_esp = big ? 0x18000 : 0xABCD8000;
        uint32_t private_sp = cur.tf.user_esp;
        op1(v, 0xFB); cur.tf.eip++; /* STI and its successor retire on the private stack */
        CHECK(dosvm_irq_raise(v->info.key, 0)); step(v, 0x20, 0);
        CHECK(dosvm_irq_raise(v->info.key, 0) && dosvm_irq_raise(v->info.key, 1) &&
              dosvm_irq_raise(v->info.key, 7));
        CHECK(v->monitor.pic[0].isr == 0x81 && v->monitor.pic[0].irr == 0x02 && v->irq.owed == 0x81 &&
              v->monitor.pic[0].imr == 0x78 && v86_irq_pending(&v->monitor) < 0);
        printf("f3-23 nested snapshot: client=%u stack=%u isr=%02x irr=%02x imr=%02x depth=%u\n",
               bits ? 32 : 16, big ? 32 : 16, v->monitor.pic[0].isr,
               v->monitor.pic[0].irr, v->monitor.pic[0].imr, v->dpmi.depth);
        CHECK(!dosvm_read(v, host, actual, sizeof(actual)));
        CHECK(!memcmp(saved, actual, sizeof(saved)));
        CHECK(cur.tf.user_ss == private_ss && cur.tf.user_esp == private_sp - 3 * w &&
              v->dpmi.lock_top == saved_top && !(cur.tf.eflags & V86_TF) && !v->monitor.vif);
        /* Keep pending requests latched while checking both complete
         * returns. PIC masking changes eligibility, never the saved frames. */
        device_out(v, 0x21, 0xFF);
        const uint8_t eoi[] = {0xE6, 0x20}; cur.tf.eax = 0x20;
        exec(v, eoi, sizeof(eoi), 13, 0);
        CHECK(v->monitor.pic[0].isr == 0x80);
        pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
        CHECK(v->dpmi.depth == 1 && cur.tf.user_ss == private_ss && cur.tf.user_esp == private_sp);
        /* LSS back, POP the saved command and execute the outer EOI. */
        cur.tf.user_ss = DPMI_HOST_SS; cur.tf.user_esp = saved_sp;
        cur.tf.eax = pm_pop(v); CHECK(cur.tf.eax == 0x20);
        exec(v, eoi, sizeof(eoi), 13, 0);
        pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
        CHECK(!v->dpmi.depth && !v->monitor.pic[0].isr && v->monitor.pic[0].irr == 0x83);
        device_out(v, 0x21, 0x78); step(v, 0x20, 0);
        CHECK(cur.tf.eip == 0x720 && v->dpmi.depth == 1 &&
              v->monitor.stats.delivered[0] == 2 && v->monitor.stats.delivered[7] == 1);
        destroy(v, true); empty();
    }
    puts("f3-23 nested PM IRQ: 16/32-bit clients and stacks, suspended host bytes intact, ISR=81 IRR=02 held=81, IRR=83 after returns, guest EOIs and pending timer delivered PASS");
}

static void test_irq_private_stack_rejection(void)
{
    for (unsigned failure = 0; failure < 4; failure++) {
        struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, true);
        uint16_t code = (uint16_t)cur.tf.cs;
        v->dpmi.vec[15] = (struct dpmi_vec){.sel=code, .off=0x700};
        v->dpmi.vec[8] = (struct dpmi_vec){.sel=code, .off=0x720};
        device_out(v, 0x21, 0x78);
        CHECK(dosvm_irq_raise(v->info.key, 7)); step(v, 0x20, 0);
        client_desc(v, 902, 0x30000, 0xFFFF, failure == 2 ? 0x72 : 0xF2, 0x40);
        cur.tf.user_ss = DPMI_SEL(902); cur.tf.user_esp = failure == 0 ? 8 : failure == 1 ? 0x10020 : 0x1008;
        /* A frame straddling a writable/read-only page must fail before
         * touching even its first writable byte. */
        uint32_t phys = 0, old_flags = 0;
        if (failure == 3) {
            phys = as_lookup(&v->as, 0x31000, &old_flags);
            CHECK(phys && !as_map(&v->as, 0x31000, phys, PTE_U));
        }
        uint8_t old[16], actual[16];
        CHECK(!dosvm_read(v, 0x30FF0, old, sizeof(old)));
        v->monitor.vif = true;
        CHECK(dosvm_irq_raise(v->info.key, 0));
        struct v86_frame before = cur;
        uint32_t lock = v->dpmi.lock_top, delivered = v->monitor.stats.delivered[0];
        CHECK(dpmi_deliver(v, &cur) < 0 && !memcmp(&cur, &before, sizeof(cur)) &&
              v->monitor.pic[0].isr == 0x80 && (v->monitor.pic[0].irr & 1) &&
              v->monitor.stats.delivered[0] == delivered && v->dpmi.depth == 1 &&
              v->dpmi.lock_top == lock && v->monitor.vif);
        CHECK(!dosvm_read(v, 0x30FF0, actual, sizeof(actual)) && !memcmp(old, actual, sizeof(old)));
        if (failure == 3) CHECK(!as_map(&v->as, 0x31000, phys, old_flags));
        destroy(v, true); empty();
    }
    puts("f3-23 nested frame rejection: short/out-of-limit/non-present/read-only private stacks leave guest bytes, frame, PIC and delivery unchanged PASS");
}

static void test_irq_private_exception(void)
{
    for (unsigned bits = 0; bits < 2; bits++) {
        struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, bits != 0);
        uint16_t code = (uint16_t)cur.tf.cs;
        v->dpmi.vec[15] = (struct dpmi_vec){.sel=code, .off=0x700};
        v->dpmi.exc[0] = (struct dpmi_vec){.sel=code, .off=0x720};
        device_out(v, 0x21, 0x78);
        CHECK(dosvm_irq_raise(v->info.key, 7)); step(v, 0x20, 0);
        pm_push(v, 0x20); uint32_t host_sp = cur.tf.user_esp;
        client_desc(v, 902, 0x30000, 0xFFFF, 0xF2, 0x40);
        cur.tf.user_ss = DPMI_SEL(902); cur.tf.user_esp = 0x800;
        uint32_t pc = cur.tf.eip; step(v, 0, 0);
        CHECK(cur.tf.user_ss == DPMI_SEL(902) && cur.tf.user_esp == 0x800 - 8 * dpmi_width(v) &&
              cur.tf.eip == 0x720 && v->dpmi.depth == 2);
        pm_retf(v); at_stub(v, DPMI_PM_EXCRET);
        CHECK(cur.tf.eip == pc && cur.tf.user_ss == DPMI_SEL(902) && cur.tf.user_esp == 0x800 &&
              v->dpmi.depth == 1 && v->monitor.pic[0].isr == 0x80);
        cur.tf.user_ss = DPMI_HOST_SS; cur.tf.user_esp = host_sp;
        CHECK(pm_pop(v) == 0x20);
        destroy(v, true); empty();
    }
    puts("f3-23 nested exception: 16/32-bit private IRQ stacks, exception frame and far return, outer saved register intact PASS");
}

static void test_sb_irq_wait_record(void)
{
    struct dosvm *v = audio_vm_create();
    g_boot.flags = CBI_F_VERBOSE; dsp_reset(v);
    dma8(v, 1, 0x30000, 255, true);
    dsp(v, 0x41); dsp(v, 0x2B); dsp(v, 0x11); /* 11025 Hz, C6 auto-init */
    dsp(v, 0xC6); dsp(v, 0); dsp(v, 63); dsp(v, 0);
    while (!v->audio.sb.causes) ms(1);
    uint64_t edge = v->audio.sb.irq_tick;
    CHECK(dosvm_pic_ack(&v->monitor) == 15);
    device_out(v, 0x21, 0x78); v86_irq_raise(&v->monitor, 0);
    CHECK(dosvm_pic_ack(&v->monitor) == 8);
    v86_irq_raise(&v->monitor, 0); v86_irq_raise(&v->monitor, 1); v86_irq_raise(&v->monitor, 7);
    unsigned logs = vm_audio_logs;
    ms(500); CHECK(g_ticks - edge == 500 && !v->audio.diag.irq_wait.pending);
    ms(1); CHECK(vm_audio_logs == logs && v->audio.diag.irq_wait.pending &&
                 v->audio.diag.irq_wait.age == 501 && v->audio.diag.irq_wait_records == 1);
    /* Output is deferred through a complete guest IRQ chain. The coherent
     * snapshot survives EOI/ack and queue saturation before it is drained. */
    v->dpmi.depth = 1; v->dpmi.rec[0].flags = DPMI_RF_HWINT;
    dosvm_audio_log(v); CHECK(vm_audio_logs == logs);
    device_out(v, 0x20, 0x60); device_out(v, 0x20, 0x67);
    a_in(v, SB + 0xE);
    v->dpmi.depth = 0;
    dosvm_audio_log(v);
    CHECK(vm_audio_logs == logs + 1 && strstr(last_vm_audio_log, "irq_wait ms=501 irq=7") &&
          strstr(last_vm_audio_log, "isr=0081 irr=0083 imr=ff78"));
    ms(510); dosvm_audio_log(v); /* next unacknowledged edge is a new episode */
    CHECK(v->audio.diag.irq_wait_records == 2);
    unsigned count = v->audio.diag.irq_wait_records;
    ms(1000); dosvm_audio_log(v); CHECK(v->audio.diag.irq_wait_records == count);
    /* Unsigned age works across clock wrap; quiet boots keep device state
     * and acquire a snapshot when verbose evidence is enabled later. */
    dsp_reset(v); g_boot.flags = 0; uint64_t tick = g_ticks;
    g_ticks = UINT64_MAX - 250; sb_cause(v, 1, v->audio.clock); ms(501);
    CHECK(!v->audio.diag.irq_wait.pending);
    g_boot.flags = CBI_F_VERBOSE; ms(1);
    CHECK(v->audio.diag.irq_wait.pending && v->audio.diag.irq_wait.age == 502);
    v->audio.diag.next_log = g_ticks; dosvm_audio_log(v);
    CHECK(strstr(last_vm_audio_log, "irq_wait ms=502 irq=7"));
    /* Full-width values fit the existing short audio record envelope. */
    uint32_t id = v->info.key.id, generation = v->info.key.generation;
    v->info.key.id = v->info.key.generation = UINT32_MAX;
    v->audio.diag.irq_wait.pending = true; v->audio.diag.irq_wait.age = UINT64_MAX;
    v->audio.diag.irq_wait.irq = 15; v->audio.diag.next_log = g_ticks;
    dosvm_audio_log(v);
    CHECK(strstr(last_vm_audio_log, "ms=18446744073709551615 irq=15"));
    v->info.key.id = id; v->info.key.generation = generation;
    for (unsigned i = v->audio.diag.irq_wait_records; i < 18; i++) {
        a_in(v, SB + 0xE); sb_cause(v, 1, v->audio.clock); ms(501);
        v->audio.diag.next_log = g_ticks; dosvm_audio_log(v);
    }
    CHECK(v->audio.diag.irq_wait_records == 16 && !v->audio.diag.irq_wait.pending);
    g_ticks = tick; g_boot.flags = 0;
    destroy(v, true); empty();
    puts("f3-23 SB IRQ wait: first edge across late C6 blocks, >500ms once, coherent ISR/IRR/IMR, IRQ deferral, ack/new episode, quiet boot and tick wrap PASS");
}

static void test_irq_stall(void)
{
    test_irq_restore_delivery(); test_irq_frame_commit(); test_irq_stall_diagnostics(); test_irq_real_tf();
    test_irq_trace_latency(); test_irq_target_diagnostics();
    test_if_stall_watchdog(); test_if_trace_barriers();
    test_irq_private_stack(); test_irq_private_stack_rejection(); test_irq_private_exception();
    test_sb_irq_wait_record();
}
