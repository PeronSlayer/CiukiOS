/* f3-10: production diagnostics, quiet launches and repeated VGA harvesting.
 * Device/routing regressions are also covered by test_timer/test_audio and
 * the guest runner's menu: replay of the real DOOM executable.
 * SPDX-License-Identifier: GPL-2.0-only */
static void test_menu_quiet_records(void)
{
    unsigned before = records;
    evidence_active = false;
    struct dosvm_config c = {.commitment = DOSVM_LOW_CAP, .probe = "vm-window"};
    struct dosvm *v = 0;
    CHECK(!dosvm_create(&c, &v) && !dosvm_boot(v));
    line(v, "DOSREADY 05.00 free=8000 C");
    CHECK(v->info.dos_ready);
    dosvm_expect(v, "RUNREC"); line(v, "RUNREC PASS");
    CHECK(v->info.verdict_done && v->info.verdict_pass && v->info.guest_records == 2);
    run_worker(v); destroy(v, true); empty();
    CHECK(records == before); /* admission, guest, stop/fault/stack, cleanup */
    evidence_active = true;
    v = create();
    CHECK(records == before + 1 && strstr(last_record, "group=admission"));
    line(v, "DOSREADY 05.00 free=8000 C");
    CHECK(strstr(last_record, "group=guest"));
    destroy(v, true); CHECK(strstr(last_record, "group=vm_cleanup")); empty();
    puts("f3-10 quiet records: desktop launch/guest/fault/cleanup silent, latches preserved, selected evidence retained PASS");
}
static void test_menu_platform_selection(void)
{
    for (unsigned qemu = 0; qemu < 2; qemu++) {
        g_boot.flags = qemu ? CBI_F_SMBIOS_QEMU : 0;
        struct dosvm *v = dpmi_vm(true);
        CHECK(v->return_tracker_tf == (qemu != 0));
        CHECK(strstr(vm_start_record, qemu ? " tracker=tf" : " tracker=breakpoint"));
        g_boot.flags ^= CBI_F_SMBIOS_QEMU; /* selection survives later boot flags and client reset */
        v86_start(); enter(v, true);
        CHECK(v->return_tracker_tf == (qemu != 0));
        dpmi_close(v);
        CHECK(v->return_tracker_tf == (qemu != 0));
        destroy(v, true); empty();
    }
    g_boot.flags = 0;
    puts("f3-35 tracker selection: production VM creation, immutable across boot-flag changes and DPMI reset PASS");
}
static void test_menu_heartbeat(void)
{
    struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, true);
    cur.tf.eip = 0x300;
    unsigned before = records;
    dosvm_heartbeat(v, &cur); CHECK(records == before && !v->trace.heartbeat_started);
    int10(v, 0x0013, 0, 0, 0);
    dosvm_heartbeat(v, &cur); CHECK(records == before && !v->trace.heartbeat_started);
    g_boot.flags = CBI_F_VERBOSE;
    dosvm_heartbeat(v, &cur); CHECK(records == before && v->trace.heartbeat_started);
    g_ticks += 1999; dosvm_heartbeat(v, &cur); CHECK(records == before);
    /* Timer arrival replaces the frame with a PM handler after the snapshot.
     * IRQ0 is still pending, delivery still zero, and the PC is the client. */
    v->dpmi.vec[8] = (struct dpmi_vec){.sel=(uint16_t)cur.tf.cs, .off=0x700};
    CHECK(dosvm_irq_raise(v->info.key, 0));
    g_ticks++; step(v, 0x20, 0);
    CHECK(v->trace.heartbeats == 1 && cur.tf.eip == 0x700);
    CHECK(strstr(recent_records[before % 8], "group=vm_heartbeat") &&
          strstr(recent_records[before % 8], "eip=00000300") &&
          strstr(recent_records[before % 8], "vif=1"));
    CHECK(strstr(recent_records[(before + 1) % 8], "irr=0001") &&
          strstr(recent_records[(before + 1) % 8], "irq0=0"));
    CHECK(v->monitor.stats.delivered[0] == 1);
    /* f3-12 buffers routine snapshots until this handler returns. Complete
     * it before testing the heartbeat's independent wall-clock cadence. */
    cur.tf.eax = 0x20; const uint8_t eoi[] = {0xE6, 0x20};
    exec(v, eoi, sizeof(eoi), 13, 0); pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
    /* No duplicate at the same tick and no catch-up burst after a delay. */
    before = records; dosvm_heartbeat(v, &cur); CHECK(records == before);
    g_ticks += 10000; dosvm_heartbeat(v, &cur); CHECK(records == before + 8);
    before = records; dosvm_heartbeat(v, &cur); CHECK(records == before);
    v->monitor.vif = false; v->dpmi.if_armed = true;
    v->dpmi.shadow = true; v->monitor.pic[1].irr = 0x80;
    uint32_t gen = v->info.key.generation;
    v->info.key.generation = UINT32_MAX;
    v->monitor.stats.delivered[0] = v->monitor.stats.delivered[1] = UINT32_MAX;
    v->kbd.reads = UINT32_MAX; v->kbd.count = DOSVM_KBD_BYTES;
    v->pit.edges = v->pit.dropped_ticks = UINT64_MAX;
    v->audio.sb.units = v->audio.sb.irq_edges = v->audio.sb.services = UINT32_MAX;
    v->monitor.stats.delivered[7] = UINT32_MAX;
    v->video_frames = v->video_starts = v->video_linear_harvests = v->video_flushes = v->video_faults = UINT32_MAX;
    v->perf.harvests = v->perf.direct_pages = UINT32_MAX;
    g_ticks = UINT64_MAX - DOSVM_HEARTBEAT_TICKS;
    cur.tf.eip = UINT32_MAX;
    dosvm_heartbeat(v, &cur);
    CHECK(records == before + 8 && strstr(recent_records[before % 8], "vif=0 arm=1 sh=1"));
    CHECK(strstr(recent_records[(before + 1) % 8], "irq1=4294967295") &&
          strstr(recent_records[(before + 3) % 8], "units=4294967295"));
    CHECK(strstr(recent_records[(before + 4) % 8], "linear=4294967295 planes=4294967295"));
    CHECK(strstr(last_record, "group=vm_beat_vga_mem") &&
          strstr(last_record, "harvests=4294967295 dirty_pages=4294967295 harvest_us=0"));
    v->info.key.generation = gen;
    v->kbd.count = 0;
    for (unsigned i = v->trace.heartbeats; i < DOSVM_HEARTBEAT_LIMIT; i++) {
        before = records; g_ticks += DOSVM_HEARTBEAT_TICKS;
        dosvm_heartbeat(v, &cur); CHECK(records == before + 8);
    }
    before = records; g_ticks += DOSVM_HEARTBEAT_TICKS;
    dosvm_heartbeat(v, &cur); CHECK(records == before && v->trace.heartbeats == DOSVM_HEARTBEAT_LIMIT);
    g_boot.flags = 0; destroy(v, true); empty();
    v = create(); CHECK(!v->trace.heartbeat_started && !v->trace.heartbeats);
    destroy(v, true); empty();
    puts("f3-10 heartbeat: pre-delivery PC/PIC, 2s cadence, delayed/wrapped clock, IF-clear state, full-width rows <=240 bytes, lifetime cap PASS");
}
static void test_menu_vga_dirty_repeat(void)
{
    struct dosvm *v = create();
    int10(v, 0x0013, 0, 0, 0);
    for (unsigned active = 0; active < 2; active++) {
        write_cr3(active ? v->as.pd_phys : kpd_phys);
        for (unsigned frame = 0; frame < 3; frame++) {
            unsigned before = cr3_reloads, harvests = v->video_linear_harvests;
            uint8_t value = (uint8_t)(active * 3 + frame + 1);
            linear_store(v, 0x1011, value);
            dosvm_vga_sync(v, false);
            CHECK(v->vga.plane[1][0x1010] == value && v->video_linear_harvests == harvests + 1);
            CHECK(!(*window_pte(v, 0xA1000) & DOSVM_PTE_DIRTY));
            CHECK(cr3_reloads == before + active);
            dosvm_vga_sync(v, false); CHECK(v->video_linear_harvests == harvests + 1);
        }
    }
    write_cr3(kpd_phys); destroy(v, true); empty();
    puts("f3-10 VGA: repeated stores harvested, one active CR3 reload on each dirty collection, inactive mapping ready for CR3 reload PASS");
}
static void test_menu_pm_devices(void)
{
    for (unsigned bits = 0; bits < 2; bits++) {
        struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, bits != 0);
        v->started = true; v->ticks = g_ticks;
        uint16_t code = (uint16_t)cur.tf.cs;
        v->dpmi.vec[8] = (struct dpmi_vec){.sel=code, .off=0x700};
        v->dpmi.vec[9] = (struct dpmi_vec){.sel=code, .off=0x800};
        v->dpmi.vec[15] = (struct dpmi_vec){.sel=code, .off=0x900};
        device_out(v, 0x21, 0x7C);
        device_out(v, 0x43, 0x36); device_out(v, 0x40, 0x4A); device_out(v, 0x40, 0x21);
        dsp_reset(v);
        uint8_t ring[4096]; memset(ring, 0x80, sizeof(ring));
        CHECK(!dosvm_write(v, 0x30000, ring, sizeof(ring)));
        dma8(v, 1, 0x30000, sizeof(ring) - 1, true);
        dsp(v, 0x41); dsp(v, 11025 >> 8); dsp(v, 11025 & 255);
        dsp(v, 0x48); dsp(v, 255); dsp(v, 0); dsp(v, 0x1C);
        CHECK(dosvm_input_focus(v->info.key, true));
        struct ciuki_input_event key = {.type=CIUKI_INPUT_KEY, .code=0x01, .value=1};
        CHECK(dosvm_input_event(v->info.key, &key)); key.value=0;
        CHECK(dosvm_input_event(v->info.key, &key));
        /* Standard DPMI disable: no client IRQ, but DMA/PIT must progress. */
        int31(v, 0x0900); CHECK(!v->monitor.vif);
        ms(100);
        CHECK(v->pit.channel[0].reload == 8522 && v->pit.edges == 13);
        CHECK(!v->monitor.stats.delivered[0] && !v->monitor.stats.delivered[1] && !v->monitor.stats.delivered[7]);
        CHECK(v->audio.sb.units >= 1024 && v->audio.sb.units <= 1103 && v->audio.sb.boundaries >= 4);
        CHECK(v->audio.dma.ch[1].count == 4095 - v->audio.sb.units && v->kbd.count == 2);
        int31(v, 0x0901); CHECK(cur.tf.eip == 0x700); /* pending timer has priority */
        uint8_t eoi[] = {0xE6, 0x20}, in[] = {0xE4, 0x60};
        cur.tf.eax = 0x20; exec(v, eoi, sizeof(eoi), 13, 0); pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
        for (unsigned byte = 0; byte < 2; byte++) {
            CHECK(cur.tf.eip == 0x800 && !v->monitor.vif);
            exec(v, in, sizeof(in), 13, 0); CHECK((cur.tf.eax & 255) == (byte ? 0x81 : 0x01));
            cur.tf.eax = 0x20; exec(v, eoi, sizeof(eoi), 13, 0); pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
        }
        CHECK(cur.tf.eip == 0x900 && !v->kbd.count && v->kbd.reads == 2);
        a_in(v, SB + 0xE);
        cur.tf.eax = 0x20; exec(v, eoi, sizeof(eoi), 13, 0); pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
        CHECK(v->monitor.vif && v->monitor.stats.delivered[0] == 1 && v->monitor.stats.delivered[1] == 2 &&
              v->monitor.stats.delivered[7] == 1 && !v->monitor.pic[0].isr);
        destroy(v, true); empty();
    }
    puts("f3-10 PM devices: 16/32-bit 140Hz timer, 8042 make/break and SB DMA progress while IF clear, delivery/EOI after 0901h PASS");
}
static void test_menu_halted_heartbeat(void)
{
    struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, true);
    int10(v, 0x0013, 0, 0, 0); cur.tf.eip = 0x300;
    v->monitor.vif = false; v->monitor.state = V86_HALTED;
    v->monitor.pic[0].imr = v->monitor.pic[1].imr = 0xFF; /* no recoverable pending IRQ */
    g_boot.flags = CBI_F_VERBOSE;
    unsigned before = records;
    device_hook_arm(v, 2100, device_hang_stop); step(v, 0x20, 0);
    CHECK(left && !sleep_hook && v->trace.heartbeats == 1 && records == before + 8);
    CHECK(strstr(recent_records[before % 8], "eip=00000300") &&
          strstr(recent_records[before % 8], "vif=0"));
    g_boot.flags = 0; destroy(v, true); empty();
    puts("f3-10 halted heartbeat: HLT with virtual IF clear still sampled while worker sleeps, no IRQ/IF forcing PASS");
}
static void test_menu_return_seed(void)
{
    static const uint8_t epilogue[] = {0x1F, 0x07, 0x0F, 0xA1, 0x0F, 0xA9,
        0x66, 0x61, 0x66, 0x83, 0xC4, 0x06, 0x66, 0xCF};
    static const uint8_t save[] = {0x9C, 0x58, 0xFA};
    for (unsigned variant = 0; variant < 7; variant++) {
        struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, true);
        client_desc(v, 900, 0x30000, 0xFFFF, 0xFA, 0);
        client_desc(v, 901, 0x31000, 0xFFFF, 0xFA, 0x40);
        client_desc(v, 902, 0x32000, 0xFFFF, 0xF2, 0x40);
        uint8_t ret = variant == 5 ? 0x9C : 0xC3;
        CHECK(!dosvm_write(v, 0x30000, epilogue, sizeof(epilogue)) && !dosvm_write(v, 0x31020, &ret, 1) &&
              !dosvm_write(v, 0x31040, save, sizeof(save)));
        uint8_t stack[64] = {0};
        uint32_t target = 0x20, selector = 0x00010000u | DPMI_SEL(901), image = V86_IF | 2, caller = 0x40;
        if (variant == 1) selector &= ~3u;
        if (variant == 2) image |= V86_VM;
        if (variant == 3) image |= 1u << 14;
        if (variant == 4) image |= V86_TF;
        memcpy(stack + 46, &target, 4); memcpy(stack + 50, &selector, 4);
        memcpy(stack + 54, &image, 4); memcpy(stack + 58, &caller, 4);
        CHECK(!dosvm_write(v, 0x32500, stack, sizeof(stack)));
        cur.tf.cs = DPMI_SEL(900); cur.tf.user_ss = cur.tf.ds = DPMI_SEL(902);
        cur.tf.user_esp = 0x500; cur.tf.eip = 0; cur.tf.eflags = V86_IF | 2;
        if (variant == 6) cur.tf.eflags |= 1u << 14; /* current NT selects a task return */
        v->dpmi.decode_valid = false; step(v, 0x20, 0);
        uint32_t actual; CHECK(!dosvm_read(v, 0x32536, &actual, 4));
        CHECK(actual == image); /* acquisition never patches the IRET image */
        CHECK(v->dpmi.return_steps == (variant ? 0 : DPMI_RETURN_STEPS) && v->monitor.vif);
        if (!variant) {
            /* IRETD -> RET -> execute fault at the actual PUSHFD boundary. */
            cur.tf.cs = DPMI_SEL(901); cur.tf.eip = 0x40; cur.tf.user_esp = 0x53E;
            cur.tf.eflags = V86_IF | 2; debug_dr6 = 15; /* disabled comparisons filtered */
            step(v, 1, 0);
            CHECK(!left && !v->dpmi.return_steps && !(cur.tf.eflags & V86_TF) && v->monitor.vif);
            cur.tf.eip = 0x42; cur.tf.eax = V86_IF | 2; step(v, 13, 0);
            CHECK(v->dpmi.if_armed && (cur.tf.eax & V86_TF) && !v->monitor.vif);
        }
        destroy(v, true); empty();
    }
    /* A 16-bit ADD SP on a big stack can wrap its low half. Do not patch
     * an otherwise valid return image at the predicted, incorrect offset. */
    struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, true);
    client_desc(v, 900, 0x30000, 0xFFFF, 0xFA, 0);
    client_desc(v, 901, 0x31000, 0xFFFF, 0xFA, 0x40);
    client_desc(v, 902, 0x32000, 0x1FFFF, 0xF2, 0x40);
    const uint8_t wrap[] = {0x83,0xC4,0x08,0x66,0xCF}, ret = 0xC3;
    uint32_t frame[] = {0x20, DPMI_SEL(901), V86_IF | 2, 0x40}, actual;
    CHECK(!dosvm_write(v, 0x30000, wrap, sizeof(wrap)) && !dosvm_write(v, 0x31020, &ret, 1) &&
          !dosvm_write(v, 0x42004, frame, sizeof(frame)));
    cur.tf.cs = DPMI_SEL(900); cur.tf.user_ss = cur.tf.ds = DPMI_SEL(902);
    cur.tf.user_esp = 0xFFFC; cur.tf.eip = 0; cur.tf.eflags = V86_IF | 2;
    v->dpmi.decode_valid = false; step(v, 0x20, 0);
    CHECK(!v->dpmi.return_steps && !dosvm_read(v, 0x4200C, &actual, 4) && actual == (V86_IF | 2));
    /* Same-selector IRET cannot distinguish the pending epilogue from
     * the destination on a physical IRQ. It must leave the image alone. */
    uint8_t iret = 0xCF;
    uint16_t same[] = {0x20, DPMI_SEL(900), V86_IF | 2, 0x40}, small;
    CHECK(!dosvm_write(v, 0x30000, &iret, 1) && !dosvm_write(v, 0x30020, &ret, 1) &&
          !dosvm_write(v, 0x32500, same, sizeof(same)));
    cur.tf.user_esp = 0x500; cur.tf.eip = 0; step(v, 0x20, 0);
    CHECK(!v->dpmi.return_steps && !dosvm_read(v, 0x32504, &small, 2) && small == (V86_IF | 2));
    destroy(v, true); empty();
    puts("f3-34 return seed: mixed-size IRETD/RET, untouched flags image, exact PUSHFD boundary; privileged/VM/NT/debugger/flags-save targets rejected PASS");
}
/* Seed through the production epilogue admission, then synthesize only
 * the CPU's execute-fault frame. Whole-instruction coverage follows below. */
static void menu_checkpoint_start(struct dosvm *v, uint32_t target)
{
    uint32_t base = seg_base(v, (uint16_t)cur.tf.cs);
    const uint8_t ret = 0xC3;
    CHECK(!dosvm_write(v, base + 0x200, &ret, 1));
    cur.tf.eip = 0x200; cur.tf.eflags = V86_IF | 2;
    pm_push(v, target);
    step(v, 0x20, 0);
    CHECK(v->dpmi.return_steps && v->dpmi.return_slot < DPMI_WATCH && !(cur.tf.eflags & V86_TF));
    cur.tf.eip = pm_pop(v);
}
static void menu_checkpoint_hit(struct dosvm *v)
{
    CHECK(v->dpmi.return_slot < DPMI_WATCH);
    debug_dr6 = 1u << v->dpmi.return_slot;
    step(v, 1, 0);
    CHECK(!left && !(cur.tf.eflags & V86_TF));
}
static void test_menu_return_steps(void)
{
    struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, true);
    v->dpmi.ldt[(uint16_t)cur.tf.cs >> 3][1] |= 0x400000u;
    uint32_t base = seg_base(v, (uint16_t)cur.tf.cs);
    uint8_t nops[DPMI_RETURN_STEPS]; memset(nops, 0x90, sizeof(nops));
    CHECK(!dosvm_write(v, base + 0x300, nops, sizeof(nops)));
    menu_checkpoint_start(v, 0x300); menu_checkpoint_hit(v);
    CHECK(!v->dpmi.return_steps && !v->dpmi.dr7 && v->monitor.vif);
    static const uint8_t stops[][3] = {{0x9C}, {0x66,0x9C}, {0x9D}, {0xFA}, {0xFB}, {0x17},
        {0x8E,0xD0}, {0x0F,0xB2,0x00}, {0xCF}, {0xCD,0x31}, {0xFF,0x18},
        {0xF3,0x9C}, {0xF2,0x9C}, {0xF0,0x9C}, {0x0F,0x34}, {0x0F,0x35}, {0x0F,0x05}, {0x0F,0x07}, {0x0F,0xAA}};
    for (unsigned i = 0; i < ARRAY_SIZE(stops); i++) {
        CHECK(!dosvm_write(v, base + 0x500, stops[i], sizeof(stops[i])));
        menu_checkpoint_start(v, 0x500); menu_checkpoint_hit(v);
        CHECK(!v->dpmi.return_steps && !v->dpmi.dr7 && v->monitor.vif && v->dpmi.decode_eip == 0x500);
    }
    static const uint8_t near[][4] = {{0xFF,0xD0}, {0xFF,0xE0}, {0xFF,0x14,0x24},
        {0x0F,0xAC,0xD0,0x10}, {0x0F,0xAD,0xD0}, {0x0F,0xA4,0xD0,0x10}, {0x0F,0xA5,0xD0}, {0xF3,0xA5}};
    static const unsigned lengths[] = {2,2,3,4,3,4,3,2};
    const uint8_t save = 0x9C;
    for (unsigned i = 0; i < ARRAY_SIZE(near); i++) {
        uint32_t end = i < 3 ? 0x540 : 0x500 + lengths[i];
        CHECK(!dosvm_write(v, base + 0x500, near[i], lengths[i]) && !dosvm_write(v, base + end, &save, 1));
        cur.tf.eax = 0x540;
        pm_push(v, 0x540); /* memory operand for CALL [ESP] */
        menu_checkpoint_start(v, 0x500); menu_checkpoint_hit(v);
        CHECK(v->dpmi.return_steps && v->dpmi.return_eip == end && v->monitor.vif);
        cur.tf.eip = end; menu_checkpoint_hit(v);
        CHECK(!v->dpmi.return_steps && !v->dpmi.dr7 && v->monitor.vif);
        (void)pm_pop(v);
    }
    /* Execute #DB at the shadowed PC has not retired that instruction. */
    CHECK(!dosvm_write(v, base + 0x610, &save, 1));
    menu_checkpoint_start(v, 0x610);
    v->dpmi.shadow = true; v->dpmi.shadow_cs = (uint16_t)cur.tf.cs; v->dpmi.shadow_eip = 0x610;
    menu_checkpoint_hit(v); CHECK(v->dpmi.shadow);
    cur.tf.eip++; step(v, 0x20, 0); CHECK(!v->dpmi.shadow);
    menu_checkpoint_start(v, 0x610); v->monitor.vif = false;
    menu_checkpoint_hit(v); CHECK(!v->monitor.vif && !v->dpmi.return_steps && !v->dpmi.dr7);
    destroy(v, true); empty();
    puts("f3-34 checkpoints: 512 NOPs with one execute fault, flags/SS/far/system barriers, near indirect transfers/shifts/REP, pre-instruction STI shadow, no IF forcing PASS");
}
static void test_menu_return_nested(void)
{
    struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, true);
    uint32_t base = seg_base(v, (uint16_t)cur.tf.cs);
    const uint8_t save = 0x9C, ret = 0xC3;
    CHECK(!dosvm_write(v, base + 0x400, &ret, 1) && !dosvm_write(v, base + 0x440, &save, 1));
    pm_push(v, 0x440);
    menu_checkpoint_start(v, 0x400);
    uint32_t parent_ip = cur.tf.eip;
    uint16_t parent_ss = (uint16_t)cur.tf.user_ss;
    v->dpmi.vec[8] = (struct dpmi_vec){.sel=(uint16_t)cur.tf.cs, .off=0x700};
    CHECK(dosvm_irq_raise(v->info.key, 0)); step(v, 0x20, 0);
    CHECK(v->dpmi.depth == 1 && v->dpmi.return_steps && !v->dpmi.dr7 && !(cur.tf.eflags & V86_TF));
    struct v86_frame handler = cur;
    cur.tf.user_ss = parent_ss; cur.tf.eip = parent_ip;
    step(v, 0x20, 0);
    CHECK(v->dpmi.return_steps && !v->dpmi.dr7 && !v->monitor.vif);
    cur = handler;
    uint8_t eoi[] = {0xE6, 0x20}; cur.tf.eax = 0x20; exec(v, eoi, sizeof(eoi), 13, 0);
    pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
    CHECK(!v->dpmi.depth && v->dpmi.return_steps && v->dpmi.dr7 && !(cur.tf.eflags & V86_TF) && v->monitor.vif);
    /* A child with the same selectors gets no parent slot. */
    v->dpmi.clients++; dpmi_reload_debug(v);
    return_boundary_seed(v, &cur); CHECK(v->dpmi.return_steps && !v->dpmi.dr7);
    v->dpmi.clients--; dpmi_reload_debug(v);
    cur.tf.eip = pm_pop(v);
    menu_checkpoint_hit(v); CHECK(!v->dpmi.return_steps && !v->dpmi.dr7);
    destroy(v, true); empty();
    /* The private B bit is consumed independently of a real client hit/BS. */
    static const uint32_t client_causes[] = {1, 1u << 14, 1u << 13, 1u << 15};
    for (unsigned cause = 0; cause < ARRAY_SIZE(client_causes); cause++) {
        v = dpmi_vm(true); v86_start(); enter(v, true);
        v->dpmi.watch[0] = (struct dpmi_watch){.used=true, .owner=1, .type=1, .size=1, .addr=0x12000};
        dpmi_reload_debug(v);
        v->dpmi.exc[1] = (struct dpmi_vec){.sel=(uint16_t)cur.tf.cs, .off=0x700};
        menu_checkpoint_start(v, 0x400);
        debug_dr6 = (1u << v->dpmi.return_slot) | client_causes[cause];
        if (cause == 1) cur.tf.eflags |= V86_TF;
        step(v, 1, 0);
        CHECK(!left && !v->dpmi.return_steps && v->dpmi.watch[0].hit == (cause == 0) && cur.tf.eip == 0x700 &&
              v->dpmi.stats.exceptions == 1 && !(cur.tf.eflags & V86_TF));
        CHECK(!!(v->dpmi.rec[0].saved.tf.eflags & V86_TF) == (cause == 1));
        destroy(v, true); empty();
    }
    /* A fourth allocation preserves acquisition and every client slot. */
    v = dpmi_vm(true); v86_start(); enter(v, true);
    menu_checkpoint_start(v, 0x400);
    for (unsigned i = 0; i < DPMI_WATCH; i++) {
        cur.tf.eax = 0x0B00; cur.tf.ebx = 1; cur.tf.ecx = 0x4000 + i * 4; cur.tf.edx = 0x0104;
        CHECK(!dpmi_int31(v, &cur) && !cf() && (uint16_t)cur.tf.ebx == i);
        CHECK(v->dpmi.watch[i].used && v->dpmi.watch[i].addr == 0x14000 + i * 4 && !v->dpmi.watch[i].hit);
    }
    CHECK(v->dpmi.return_steps && v->dpmi.return_slot == DPMI_WATCH && (v->dpmi.dr7 & 0x55) == 0x55);
    dosvm_task_switch(v->task);
    for (unsigned i = 0; i < DPMI_WATCH; i++) CHECK(debug_addr[i] == 0x14000 + i * 4);
    dosvm_task_switch(&caller); CHECK(!debug_dr7);
    destroy(v, true); empty();
    puts("f3-34 return scope: IRQ/callback/child suspension, TF untouched, simultaneous client BS/watchpoint, four debugger slots, task switching PASS");
}
static void test_menu_hint_expiry(void)
{
    for (unsigned by_irq = 0; by_irq < 2; by_irq++) {
        struct dosvm *v = dpmi_vm(true); v86_start(); enter(v,true);
        menu_checkpoint_start(v,0x400);
        uint32_t base = seg_base(v,(uint16_t)cur.tf.cs);
        const uint8_t loop[] = {0xEB,0xFE};
        CHECK(!dosvm_write(v,base+0x600,loop,2));
        cur.tf.eip = 0x600; /* diverted prediction never hits */
        uint64_t start = g_ticks;
        if (by_irq) {
            for (unsigned i = 0; i < 64; i++) {
                /* Every physical entry spends lifetime even when scanning
                 * cannot advance and no private #DB has arrived. */
                v->dpmi.return_irqs++;
                return_resume(v,&cur);
            }
        } else { g_ticks += 100; return_resume(v,&cur); }
        CHECK(!v->dpmi.return_steps && !v->dpmi.dr7 && v->monitor.vif && !v->perf.return_hits);
        CHECK(v->perf.return_armed == 1 && v->perf.return_expired == 1);
        menu_checkpoint_start(v,0x440); CHECK(v->dpmi.return_steps && v->dpmi.return_irqs == 0);
        v->monitor.vif = false; g_ticks += 100; return_resume(v,&cur);
        CHECK(!v->dpmi.return_steps && !v->dpmi.dr7 && !v->monitor.vif);
        g_ticks = start;
        destroy(v,true); empty();
    }
    puts("f3-34 hint lifetime: missed target expires by time or IRQ budget, releases DR slot, allows reseeding, never forces IF PASS");
}
static void test_menu_fallback_ownership(void)
{
    static const uint32_t causes[] = {0, 1, 1u << 13, 1u << 15};
    for (unsigned case_no = 0; case_no < ARRAY_SIZE(causes); case_no++) {
        struct dosvm *v = dpmi_vm(true); v86_start(); enter(v,true);
        for (unsigned i = 0; i < DPMI_WATCH; i++) v->dpmi.watch[i] =
            (struct dpmi_watch){.used=true,.owner=1,.type=1,.size=4,.addr=0x34000+i*4};
        dpmi_reload_debug(v);
        uint32_t base = seg_base(v,(uint16_t)cur.tf.cs);
        uint8_t code[515]; memset(code,0x90,sizeof(code)); code[0] = 0xC3;
        CHECK(!dosvm_write(v,base+0x200,code,sizeof(code)));
        cur.tf.eip = 0x200; cur.tf.eflags = V86_IF | 2; pm_push(v,0x201);
        v->dpmi.exc[1] = (struct dpmi_vec){.sel=(uint16_t)cur.tf.cs,.off=0x700};
        step(v,0x20,0); CHECK(v->dpmi.return_step && (cur.tf.eflags & V86_TF));
        if (case_no) {
            cur.tf.eip = pm_pop(v);
            debug_dr6 = (1u<<14) | causes[case_no]; step(v,1,0);
            CHECK(!left && cur.tf.eip == 0x700 && v->dpmi.depth == 1 &&
                  !v->dpmi.return_step && !v->dpmi.return_steps &&
                  !(v->dpmi.rec[0].saved.tf.eflags & V86_TF) &&
                  v->dpmi.watch[0].hit == (case_no == 1));
        } else {
            for (unsigned i = 0; i < DPMI_RETURN_STEPS; i++) {
                cur.tf.eip = 0x201+i; debug_dr6 = 1u<<14; step(v,1,0);
                CHECK(!left && v->monitor.vif && v->dpmi.return_step == (i+1 < DPMI_RETURN_STEPS));
            }
            CHECK(!v->dpmi.return_steps && !(cur.tf.eflags & V86_TF) &&
                  v->perf.return_fallbacks == DPMI_RETURN_STEPS && !v->perf.return_hits);
        }
        for (unsigned i = 0; i < DPMI_WATCH; i++) CHECK(v->dpmi.watch[i].used && v->dpmi.watch[i].addr == 0x34000+i*4);
        destroy(v,true); empty();
    }
    puts("f3-34 fallback ownership: 512-step bound, no forced IF, all client DR slots retained, simultaneous B/BD/BT delivered without monitor TF PASS");
}
static void test_menu_checkpoint_invalid(void)
{
    for (unsigned variant = 0; variant < 7; variant++) {
        struct dosvm *v = dpmi_vm(true); v86_start(); enter(v,true);
        uint32_t base = seg_base(v,(uint16_t)cur.tf.cs);
        const uint8_t save = 0x9C;
        CHECK(!dosvm_write(v,base+0x400,&save,1));
        menu_checkpoint_start(v,0x400);
        if (variant == 0) v->dpmi.ldt[(uint16_t)cur.tf.cs >> 3][0] ^= 0x10000; /* code base */
        if (variant == 1) v->dpmi.ldt[(uint16_t)cur.tf.user_ss >> 3][0] ^= 1; /* stack limit */
        if (variant == 2) cur.tf.user_ss = (uint16_t)cur.tf.ds;
        if (variant == 3) cur.tf.eflags |= V86_TF;
        if (variant == 4) cur.tf.eflags |= 1u << 14;
        if (variant == 5) dpmi_set_pm(v,false); /* raw mode switch without a nesting record */
        if (variant == 6) v->dpmi.clients = 0;
        return_resume(v,&cur);
        CHECK(!v->dpmi.return_steps && !v->dpmi.dr7 && v->monitor.vif);
        CHECK(!!(cur.tf.eflags & V86_TF) == (variant == 3));
        if (variant == 6) v->dpmi.clients = 1;
        destroy(v,true); empty();
    }
    /* Acquisition may not hide the CPU's fault vector/error/PC. */
    static const unsigned vectors[] = {0,6,11,12,13,14};
    for (unsigned i = 0; i < ARRAY_SIZE(vectors); i++) {
        struct dosvm *v = dpmi_vm(true); v86_start(); enter(v,true);
        menu_checkpoint_start(v,0x400);
        unsigned vector = vectors[i];
        uint32_t err = vector >= 10 ? 0x87 : 0;
        v->dpmi.exc[vector] = (struct dpmi_vec){.sel=(uint16_t)cur.tf.cs,.off=0x700};
        cur.tf.eip = 0x300;
        fake_cr2 = 0xE0000000u; /* outside the VGA device-resolution path */
        step(v,vector,err);
        CHECK(!left && !v->dpmi.return_steps && !v->dpmi.dr7 && v->dpmi.depth == 1 &&
              cur.tf.eip == 0x700 && v->dpmi.rec[0].saved.tf.eip == 0x300 &&
              v->dpmi.rec[0].saved.tf.vector == vector && v->dpmi.rec[0].saved.tf.err == err);
        destroy(v,true); empty();
    }
    puts("f3-34 invalidation: code/stack descriptor, SS, TF/NT, raw mode, client lifetime; DE/UD/NP/SS/GP/PF preserved PASS");
}
static void test_menu_near_return(void)
{
    static const uint8_t code32[] = {0x8B,0x59,0x22,0xC1,0xE0,0x08,0x01,0xD8,
        0x83,0xC4,0x18,0x5D,0x5F,0x5E,0xC2,0x0C,0x00};
    static const uint8_t code16[] = {0x58,0x5B,0x59,0xC2,0x02,0x00};
    static const uint8_t save[] = {0x9C,0x58,0xFA};
    for (unsigned bits = 0; bits < 2; bits++) for (unsigned variant = 0; variant < 6; variant++) {
        struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, true);
        client_desc(v, 900, 0x30000, variant == 3 ? 0x7FF : 0xFFFF, 0xFA, bits ? 0x40 : 0);
        client_desc(v, 902, 0x32000, 0xFFFF, 0xF2, 0x40);
        uint8_t code[sizeof(code32)]; unsigned n = bits ? sizeof(code32) : sizeof(code16);
        memcpy(code, bits ? code32 : code16, n);
        if (variant == 1) { code[0] = 0x8B; code[1] = 0xE0; } /* MOV SP/ESP,AX/EAX */
        if (variant == 2) { code[0] = 0x89; code[1] = 0x18; } /* MOV [BX/EAX],BX/EBX */
        CHECK(!dosvm_write(v, 0x30000, code, n) && !dosvm_write(v, 0x30100, code, n) &&
              !dosvm_write(v, 0x30400, save, sizeof(save)));
        uint8_t stack[40] = {0}; unsigned consumed = bits ? 36 : 6, w = bits ? 4 : 2;
        uint32_t target = variant == 3 ? 0xFFFFFFu : 0x400;
        memcpy(stack + consumed, &target, w);
        CHECK(!dosvm_write(v, 0x32500, stack, sizeof(stack)) && !dosvm_write(v, 0x32600, stack, sizeof(stack)));
        cur.tf.cs = DPMI_SEL(900); cur.tf.user_ss = cur.tf.ds = DPMI_SEL(902);
        cur.tf.user_esp = 0x500; cur.tf.eip = 0; cur.tf.eflags = V86_IF | 2;
        if (variant == 4) v->monitor.vif = false;
        if (variant == 5) cur.tf.eflags |= V86_TF;
        v->dpmi.decode_valid = false; step(v, 0x20, 0);
        CHECK(v->dpmi.return_steps == (variant ? 0 : DPMI_RETURN_STEPS));
        CHECK(!!(cur.tf.eflags & V86_TF) == (variant == 5));
        if (!variant) {
            cur.tf.eip = 0x400; cur.tf.user_esp += consumed + w + (bits ? 12 : 2);
            debug_dr6 = 1u << v->dpmi.return_slot; step(v, 1, 0);
            CHECK(!v->dpmi.return_steps && !(cur.tf.eflags & V86_TF) && v->monitor.vif);
            cur.tf.eip = 0x402; cur.tf.eax = V86_IF | 2; step(v, 13, 0);
            CHECK(v->dpmi.if_armed && !v->monitor.vif && (cur.tf.eax & V86_TF));
            /* A restored image returns to DMX's value-computing epilogue.
             * It must acquire the next caller boundary with IF unchanged. */
            cur.tf.eip = 0x100; cur.tf.user_esp = 0x600; cur.tf.eflags |= V86_TF;
            debug_dr6 = 1u << 14; step(v, 1, 0);
            CHECK(!v->dpmi.if_armed && v->monitor.vif && v->dpmi.return_steps == DPMI_RETURN_STEPS &&
                  !(cur.tf.eflags & V86_TF) && v->dpmi.stats.if_restored == 1);
        }
        destroy(v, true); empty();
    }
    puts("f3-10 near returns: 16/32-bit RET cleanup, DMX register-value epilogue after flags restore, stop before next save; SP assignment/memory store/bad target/IF-clear/debugger refused PASS");
}
#ifdef CIUKI_DOSVM_GUEST_CPU
static void test_menu_checkpoint_cpu(void)
{
    const struct gcpu_ops ops = {if_cpu_read, if_cpu_write, if_cpu_descriptor, if_cpu_unexpected, 0};
    gcpu_init(&ops);
    for (unsigned bits = 0; bits < 2; bits++) {
        struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, bits != 0);
        unsigned w = bits ? 4 : 2;
        client_desc(v, 900, 0x30000, 0xFFFF, 0xFA, bits ? 0x40 : 0);
        client_desc(v, 902, 0x32000, 0xFFFF, 0xF2, bits ? 0x40 : 0);
        cur.tf.cs = DPMI_SEL(900); cur.tf.user_ss = cur.tf.ds = cur.tf.es = DPMI_SEL(902);
        cur.tf.user_esp = 0x500; cur.tf.eip = 0x100; cur.tf.eflags = V86_IF | 2;
        cur.tf.eax = 0x1234; cur.tf.ebx = cur.tf.edi = 0x700; cur.tf.ebp = 0x888;
        const uint8_t ret = 0xC3;
        const uint8_t copier[] = {0x55,0x89,0xE5,0x89,0x07,0xC9,0xC2,0x04,0x00};
        const uint8_t save[] = {0x9C,0x58,0xFA,0x50,0x9D,0x90};
        uint32_t target = 0x200, caller_ip = 0x300;
        CHECK(!dosvm_write(v,0x30100,&ret,1) && !dosvm_write(v,0x30200,copier,sizeof(copier)) &&
              !dosvm_write(v,0x30300,save,sizeof(save)) && !dosvm_write(v,0x32500,&target,w) &&
              !dosvm_write(v,0x32500+w,&caller_ip,w));
        step(v,0x20,0);
        CHECK(v->dpmi.return_steps && !(cur.tf.eflags & V86_TF));
        static const unsigned endpoints[] = {0x200,0x206,0x300};
        for (unsigned i = 0; i < ARRAY_SIZE(endpoints); i++) {
            struct gcpu_event ev;
            CHECK(if_cpu_run(v,&ev) == GCPU_TRAP && ev.vector == 1 && ev.fault &&
                  ev.dr6 == (1u << v->dpmi.return_slot) && !(cur.tf.eflags & V86_TF));
            CHECK(cur.tf.eip == endpoints[i] && ev.executed == (i == 1 ? 4u : 1u));
            debug_dr6 = ev.dr6; step(v,1,0);
            CHECK(!left && !(cur.tf.eflags & V86_TF) && v->monitor.vif);
        }
        uint32_t stored = 0;
        CHECK(!v->dpmi.return_steps && !v->dpmi.dr7 && cur.tf.ebp == 0x888 &&
              cur.tf.user_esp == 0x500 + 2*w + 4 && !dosvm_read(v,0x32700,&stored,w) && stored == 0x1234);
        struct gcpu_event ev;
        CHECK(if_cpu_run(v,&ev) == GCPU_TRAP && ev.vector == 13 && cur.tf.eip == 0x302);
        step(v,13,0); CHECK(v->dpmi.if_armed && !v->monitor.vif && (cur.tf.eax & V86_TF));
        CHECK(if_cpu_run(v,&ev) == GCPU_TRAP && ev.vector == 1 && ev.dr6 == (1u << 14));
        debug_dr6 = ev.dr6; step(v,1,0);
        CHECK(!left && !v->dpmi.if_armed && v->monitor.vif && !(cur.tf.eflags & V86_TF));
        destroy(v,true); empty();
    }
    gcpu_done(); if_cpu_vm = 0;
    puts("f3-34 CPU: 16/32-bit real RET, result store, LEAVE/RET cleanup, three execute faults, PUSHF/CLI/POPF virtual-IF restoration PASS");
}
/* Review regressions execute the real guest instructions through the CPU
 * adapter. No synthetic BS stands in for CPUID, IRET or flags restoration. */
static void test_menu_review_cpu(void)
{
    const struct gcpu_ops ops = {if_cpu_read, if_cpu_write, if_cpu_descriptor, if_cpu_unexpected, 0};
    gcpu_init(&ops);
    for (unsigned tracker = 0; tracker < 2; tracker++)
    for (unsigned bits = 0; bits < 2; bits++) for (unsigned restore = 0; restore < 2; restore++)
    for (unsigned scenario = 0; scenario < 6; scenario++) {
        g_boot.flags = tracker ? CBI_F_SMBIOS_QEMU : 0;
        struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, bits != 0);
        unsigned w = bits ? 4 : 2;
        client_desc(v, 900, 0x30000, 0xFFFF, 0xFA, bits ? 0x40 : 0);
        client_desc(v, 901, 0x31000, 0xFFFF, 0xFA, bits ? 0x40 : 0);
        client_desc(v, 902, 0x32000, 0xFFFF, 0xF2, bits ? 0x40 : 0);
        cur.tf.cs = DPMI_SEL(900); cur.tf.user_ss = cur.tf.ds = cur.tf.es = DPMI_SEL(902);
        cur.tf.user_esp = 0x500; cur.tf.eip = 0x100; cur.tf.eflags = V86_IF | 2;
        cur.tf.eax = 0; /* CPUID vendor leaf */
        const uint8_t ret = 0xC3, iret = 0xCF;
        const uint8_t cpuid[] = {0x0F,0xA2,0x9C,0x58,0xFA,0x50,0x9D,0x90};
        uint8_t body[32]; memcpy(body, cpuid, sizeof(cpuid));
        unsigned len = sizeof(cpuid);
        if (restore) {
            /* PUSH AX/EAX; PUSH CS; PUSH target; IRET; NOP */
            body[6] = 0x0E; body[7] = 0x68;
            uint32_t end = 0x200 + 9 + w;
            memcpy(body + 8, &end, w); body[8+w] = 0xCF; body[9+w] = 0x90;
            len = 10 + w;
        }
        uint32_t codebase = scenario == 1 ? 0x31000 : 0x30000;
        CHECK(!dosvm_write(v, 0x30100, scenario == 1 ? &iret : &ret, 1) &&
              !dosvm_write(v, codebase + 0x200, body, len));
        uint32_t stack[4] = {0x200, 0, 0, 0};
        if (scenario == 1) {
            stack[0] = 0x180; stack[1] = DPMI_SEL(901); stack[2] = V86_IF | 2; stack[3] = 0x200;
            CHECK(!dosvm_write(v, 0x31180, &ret, 1));
        }
        for (unsigned i = 0; i < (scenario == 1 ? 4u : 1u); i++)
            CHECK(!dosvm_write(v, 0x32500 + i*w, &stack[i], w));
        if (scenario == 1 || scenario == 4) {
            for (unsigned i = 0; i < DPMI_WATCH; i++) v->dpmi.watch[i] =
                (struct dpmi_watch){.used=true,.owner=1,.type=1,.size=4,.addr=0x34000+i*4};
            dpmi_reload_debug(v);
        }
        if (scenario == 2 || scenario == 3) {
            /* RET to a second RET or direct JMP. The IRQ edits the return
             * slot or branch after its checkpoint, before CPU execution. */
            uint32_t middle = 0x180, obsolete = 0x280;
            CHECK(!dosvm_write(v, 0x32500, &middle, w) &&
                  !dosvm_write(v, 0x32500+w, &obsolete, w));
            uint8_t branch[] = {0xEB,0x7E}; /* 180 -> 200 */
            CHECK(!dosvm_write(v, 0x30180, scenario == 2 ? &ret : branch, scenario == 2 ? 1 : 2));
            if (scenario == 3) {
                uint8_t before[32]; memset(before,0x90,sizeof(before));
                before[0] = 0x89; before[1] = 0x07; /* MOV [BX/EDI],AX/EAX */
                cur.tf.ebx = cur.tf.edi = 0x900;
                middle = 0x160;
                CHECK(!dosvm_write(v,0x30160,before,sizeof(before)) && !dosvm_write(v,0x32500,&middle,w));
            }
        }
        if (scenario == 5) {
            /* A store changes a direct transfer that the scanner has not
             * reached yet. A writable data alias models self-modifying code. */
            client_desc(v, 903, 0x30000, 0xFFFF, 0xF2, bits ? 0x40 : 0);
            cur.tf.ds = DPMI_SEL(903);
            uint8_t writer[34]; memset(writer,0x90,sizeof(writer));
            writer[0] = 0xC6; writer[1] = bits ? 0x05 : 0x06;
            uint32_t operand = 0x181, start = 0x160;
            memcpy(writer+2,&operand,w); writer[2+w] = 0x7E;
            writer[32] = 0xEB; writer[33] = 0x3E; /* initially points at 1C0 */
            CHECK(!dosvm_write(v,0x30160,writer,sizeof(writer)) && !dosvm_write(v,0x32500,&start,w));
        }
        v->dpmi.decode_valid = false;
        step(v, 0x20, 0);
        CHECK(v->dpmi.return_steps);
        if (scenario == 4) {
            CHECK(v->dpmi.return_step && (cur.tf.eflags & V86_TF));
            v->dpmi.vec[8] = (struct dpmi_vec){.sel=DPMI_SEL(900),.off=0x700};
            CHECK(dosvm_irq_raise(v->info.key,0)); step(v,0x20,0);
            CHECK(v->dpmi.depth == 1 && !v->dpmi.return_step && !(v->dpmi.rec[0].saved.tf.eflags & V86_TF));
            cur.tf.eax = 0x20; const uint8_t eoi[] = {0xE6,0x20}; exec(v,eoi,2,13,0);
            pm_iret(v); at_stub(v,DPMI_PM_IRQRET);
            CHECK(!v->dpmi.depth && v->dpmi.return_step && (cur.tf.eflags & V86_TF));
            cur.tf.eax = 0;
        }
        if (scenario == 2 || scenario == 3) {
            struct gcpu_event ev;
            for (unsigned count = 0; cur.tf.eip != 0x180 && count < 128; count++) {
                CHECK(if_cpu_run(v,&ev) == GCPU_TRAP && ev.vector == 1);
                debug_dr6 = ev.dr6; step(v,1,0);
            }
            CHECK(cur.tf.eip == 0x180);
            CHECK(v->dpmi.decode_valid && v->dpmi.decode_stack_valid && v->dpmi.decode_eip == 0x180);
            v->dpmi.vec[8] = (struct dpmi_vec){.sel=DPMI_SEL(900),.off=0x700};
            CHECK(dosvm_irq_raise(v->info.key, 0)); step(v, 0x20, 0);
            CHECK(v->dpmi.depth == 1 && !v->dpmi.return_step && !(v->dpmi.rec[0].saved.tf.eflags & V86_TF));
            uint32_t target = 0x200;
            if (scenario == 2) CHECK(!dosvm_write(v, 0x32500+w, &target, w));
            else {
                /* Change the instruction to a short jump via 1C0. */
                const uint8_t jump[] = {0xEB,0x3E};
                CHECK(!dosvm_write(v, 0x30180, jump, 2) && !dosvm_write(v, 0x301C0, jump, 2));
            }
            cur.tf.eax = 0x20; const uint8_t eoi[] = {0xE6,0x20}; exec(v,eoi,2,13,0);
            pm_iret(v); at_stub(v,DPMI_PM_IRQRET);
            CHECK(!v->dpmi.depth && v->monitor.vif && v->dpmi.return_eip != 0x280);
        }
        bool armed = false, restored = false;
        for (unsigned count = 0; count < 128 && !restored; count++) {
            struct gcpu_event ev;
            int rc = if_cpu_run(v, &ev);
            if (rc != GCPU_TRAP) fprintf(stderr,"review CPU bits=%u restore=%u scenario=%u rc=%d vec=%u pc=%x flags=%x step=%u remaining=%u slot=%u\n",bits,restore,scenario,rc,ev.vector,cur.tf.eip,cur.tf.eflags,v->dpmi.return_step,v->dpmi.return_steps,v->dpmi.return_slot);
            CHECK(rc == GCPU_TRAP);
            if (ev.vector == 13) {
                CHECK(cur.tf.eip == 0x204 && !(cur.tf.eflags & V86_TF) && !(cur.tf.eax & V86_TF));
                step(v,13,0); CHECK(v->dpmi.if_armed && !v->monitor.vif && (cur.tf.eax & V86_TF));
                armed = true;
            } else {
                CHECK(ev.vector == 1);
                debug_dr6 = ev.dr6; step(v,1,0);
                restored = armed && v->monitor.vif;
            }
            CHECK(!left);
        }
        CHECK(armed && restored && !v->dpmi.if_armed && !(cur.tf.eflags & V86_TF));
        CHECK(v->perf.return_fallbacks > 0);
        if (scenario == 1 || scenario == 4) {
            CHECK(!v->perf.return_hits && (v->dpmi.dr7 & 0x55) == 0x55);
            for (unsigned i = 0; i < DPMI_WATCH; i++)
                CHECK(v->dpmi.watch[i].used && !v->dpmi.watch[i].hit && v->dpmi.watch[i].addr == 0x34000+i*4);
        } else if (!tracker) CHECK(v->perf.return_hits && v->perf.return_hits == v->perf.return_segmented);
        if (tracker) CHECK(!v->perf.return_armed && !v->perf.return_hits && !v->perf.return_segmented);
        CHECK(v->perf.return_reacquired > 0);
        destroy(v,true); empty();
    }
    g_boot.flags = 0;
    gcpu_done(); if_cpu_vm = 0;
    puts("f3-34 review CPU: 16/32-bit CPUID to POPF/IRET, four untouched watchpoints with cross-CS IRET/RET, IRQ return-slot/direct-jump edits, self-modifying transfer, fallback IRQ suspension, independent proof, private segmented hits PASS");
}
static void test_menu_doom_cli_seeds(void)
{
    /* Byte-identical slices from the pinned DOOM.EXE, before LE relocation.
     * These are plain handler/callback CLIs, not PUSHFD/CLI critical sections.
     * Branch-heavy handler bodies are outside this bounded replay; append an
     * IRETD frame, and execute the complete callback arithmetic/RET epilogue. */
    static const uint8_t entry[] = {0xFA,0x89,0xEA,0x1E,0x07,0x8D,0x04,0xD5,0,0,0,0,0x01,0xD0};
    static const uint8_t callback[] = {0xFA,0x31,0xDB,0x31,0xD2,0x66,0x8B,0x1D,0x30,0x88,0x02,0,
        0xA1,0x20,0x88,0x02,0,0xF7,0xF3,0x85,0xD2,0x0F,0x94,0xC0,0x25,0xFF,0,0,0,
        0x5D,0x5F,0x5E,0x5A,0x59,0x5B,0xC3};
    static const uint8_t exit_slice[] = {0xFA,0xA1,0xF0,0x83,0x02,0,0x05,0,0x10,0,0,
        0x8B,0x1D,0xE8,0x83,0x02,0,0xA3,0xF0,0x83,0x02,0};
    static const uint32_t seeds[] = {0x248205,0x24821D,0x2482DA};
    const struct gcpu_ops ops = {if_cpu_read, if_cpu_write, if_cpu_descriptor, if_cpu_unexpected, 0};
    gcpu_init(&ops);
    for (unsigned variant=0; variant<3; variant++) {
        struct dosvm *v=dpmi_vm(true); v86_start(); enter(v,true);
        uint16_t code=(uint16_t)cur.tf.cs; uint32_t base=seg_base(v,code);
        v->dpmi.ldt[code >> 3][1] |= 0x400000u;
        client_desc(v,900,0,UINT32_MAX,0xF2,0x40);
        cur.tf.ds=cur.tf.es=cur.tf.fs=cur.tf.gs=DPMI_SEL(900);
        const uint8_t *bytes=variant==0?entry:variant==1?callback:exit_slice;
        unsigned size=variant==0?sizeof(entry):variant==1?sizeof(callback):sizeof(exit_slice);
        uint8_t iret=0xCF, nop=0x90;
        CHECK(!dosvm_write(v,base+0x300,bytes,size) && !dosvm_write(v,base+0x380,&iret,1) &&
              !dosvm_write(v,base+0x400,&nop,1));
        uint32_t divisor=1, tics=140;
        CHECK(!dosvm_write(v,0x28830,&divisor,2) && !dosvm_write(v,0x28820,&tics,4));
        pm_push(v,V86_IF|2); pm_push(v,code); pm_push(v,0x400);
        if (variant==1) { pm_push(v,0x380); for(unsigned i=0;i<6;i++) pm_push(v,0); }
        else CHECK(!dosvm_write(v,base+0x300+size,&iret,1));
        v->dpmi.decode_valid=true; v->dpmi.decode_cs=code; v->dpmi.decode_eip=seeds[variant];
        cur.tf.eip=0x300; cur.tf.eflags=V86_IF|2;
        step(v,13,0); CHECK(!v->dpmi.if_armed && !v->monitor.vif);
        v->dpmi.vec[8]=(struct dpmi_vec){.sel=code,.off=0x700};
        CHECK(dosvm_irq_raise(v->info.key,0)); step(v,0x20,0);
        for(unsigned i=0;i<64 && !v->dpmi.depth;i++) {
            struct gcpu_event ev;
            CHECK(if_cpu_run(v,&ev)==GCPU_TRAP && ev.vector==1 && ev.dr6==(1u<<14));
            debug_dr6=ev.dr6; step(v,ev.vector,ev.err);
        }
        CHECK(v->dpmi.depth==1 && cur.tf.eip==0x700 && v->monitor.stats.delivered[0]==1 &&
              v->dpmi.stats.if_restored==1 && !(cur.tf.eflags&V86_TF));
        destroy(v,true); empty();
    }
    gcpu_done(); if_cpu_vm=0;
    puts("f3-15 DOOM seeds: three extracted plain CLI slices, callback DIV/SETcc/pops/RET, IRETD retirement and IRQ0 delivery with unknown seed PASS");
}
#endif
static void test_menu(void)
{
    test_menu_platform_selection();
    test_menu_quiet_records(); test_menu_heartbeat(); test_menu_vga_dirty_repeat(); test_menu_pm_devices();
    test_menu_halted_heartbeat();
    test_menu_return_seed(); test_menu_return_steps(); test_menu_return_nested();
    test_menu_checkpoint_invalid(); test_menu_hint_expiry(); test_menu_fallback_ownership();
    test_menu_near_return();
#ifdef CIUKI_DOSVM_GUEST_CPU
    test_menu_checkpoint_cpu(); test_menu_review_cpu();
    test_menu_doom_cli_seeds();
#endif
}
