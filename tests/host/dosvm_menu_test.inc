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
    g_ticks += 10000; dosvm_heartbeat(v, &cur); CHECK(records == before + 5);
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
    g_ticks = UINT64_MAX - DOSVM_HEARTBEAT_TICKS;
    cur.tf.eip = UINT32_MAX;
    dosvm_heartbeat(v, &cur);
    CHECK(records == before + 5 && strstr(recent_records[before % 8], "vif=0 arm=1 sh=1"));
    CHECK(strstr(recent_records[(before + 1) % 8], "irq1=4294967295") &&
          strstr(recent_records[(before + 3) % 8], "units=4294967295"));
    CHECK(strstr(last_record, "linear=4294967295 planes=4294967295"));
    v->info.key.generation = gen;
    v->kbd.count = 0;
    for (unsigned i = v->trace.heartbeats; i < DOSVM_HEARTBEAT_LIMIT; i++) {
        before = records; g_ticks += DOSVM_HEARTBEAT_TICKS;
        dosvm_heartbeat(v, &cur); CHECK(records == before + 5);
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
            unsigned before = invalidations, harvests = v->video_linear_harvests;
            uint8_t value = (uint8_t)(active * 3 + frame + 1);
            linear_store(v, 0x1011, value);
            dosvm_vga_sync(v, false);
            CHECK(v->vga.plane[1][0x1010] == value && v->video_linear_harvests == harvests + 1);
            CHECK(!(*window_pte(v, 0xA1000) & DOSVM_PTE_DIRTY));
            CHECK(invalidations == before + active && (!active || invalidated_va == 0xA1000));
            dosvm_vga_sync(v, false); CHECK(v->video_linear_harvests == harvests + 1);
        }
    }
    write_cr3(kpd_phys); destroy(v, true); empty();
    puts("f3-10 VGA: repeated stores harvested, active CR3 INVLPG on each D-bit clear, inactive mapping ready for CR3 reload PASS");
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
    g_boot.flags = CBI_F_VERBOSE;
    unsigned before = records;
    device_hook_arm(v, 2100, device_hang_stop); step(v, 0x20, 0);
    CHECK(left && !sleep_hook && v->trace.heartbeats == 1 && records == before + 5);
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
        CHECK(actual == (variant ? image : image | V86_TF));
        CHECK(v->dpmi.return_steps == (variant ? 0 : DPMI_RETURN_STEPS) && v->monitor.vif);
        if (!variant) {
            /* IRETD -> RET -> PUSHFD boundary, before PUSHFD saves TF. */
            cur.tf.cs = DPMI_SEL(901); cur.tf.eip = 0x40; cur.tf.user_esp = 0x53E;
            cur.tf.eflags = V86_IF | V86_TF | 2; debug_dr6 = (1u << 14) | 15;
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
    puts("f3-10 return seed: mixed-size epilogue/IRETD/RET, unused selector high half, flags-clean PUSHFD boundary; privileged/VM/NT/debugger/flags-save targets rejected PASS");
}
static void test_menu_return_steps(void)
{
    struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, true);
    uint32_t base = seg_base(v, (uint16_t)cur.tf.cs);
    uint8_t nops[DPMI_RETURN_STEPS]; memset(nops, 0x90, sizeof(nops)); CHECK(!dosvm_write(v, base + 0x300, nops, sizeof(nops)));
    v->dpmi.return_cs = (uint16_t)cur.tf.cs; v->dpmi.return_ss = (uint16_t)cur.tf.user_ss;
    v->dpmi.return_depth = (uint8_t)v->dpmi.depth; v->dpmi.return_client = (uint8_t)v->dpmi.clients;
    v->dpmi.return_steps = DPMI_RETURN_STEPS;
    for (unsigned i = 0; i < DPMI_RETURN_STEPS; i++) {
        cur.tf.eip = 0x300 + i; cur.tf.eflags |= V86_TF; debug_dr6 = 1u << 14;
        step(v, 1, 0);
        CHECK(v->dpmi.return_steps == DPMI_RETURN_STEPS - i - 1 && v->monitor.vif);
        CHECK(!!(cur.tf.eflags & V86_TF) == (i + 1 < DPMI_RETURN_STEPS));
    }
    static const uint8_t stops[][3] = {{0x9C}, {0x66,0x9C}, {0x9D}, {0xFA}, {0xFB}, {0x17},
        {0x8E,0xD0}, {0x0F,0xB2,0x00}, {0xCF}, {0xCD,0x31}, {0xFF,0x18},
        {0xF3,0x9C}, {0xF2,0x9C}, {0xF0,0x9C}, {0x0F,0x34}, {0x0F,0x35}, {0x0F,0x05}, {0x0F,0x07}, {0x0F,0xAA}};
    for (unsigned i = 0; i < ARRAY_SIZE(stops); i++) {
        CHECK(!dosvm_write(v, base + 0x500, stops[i], sizeof(stops[i])));
        cur.tf.eip = 0x500; cur.tf.eflags |= V86_TF; debug_dr6 = 1u << 14;
        v->dpmi.return_steps = 5; step(v, 1, 0);
        CHECK(!v->dpmi.return_steps && !(cur.tf.eflags & V86_TF) && v->monitor.vif);
    }
    static const uint8_t near[][4] = {{0xFF,0xD0}, {0xFF,0xE0}, {0xFF,0x14,0x24},
        {0x0F,0xAC,0xD0,0x10}, {0x0F,0xAD,0xD0}, {0x0F,0xA4,0xD0,0x10}, {0x0F,0xA5,0xD0}, {0xF3,0xA5}};
    for (unsigned i = 0; i < ARRAY_SIZE(near); i++) {
        CHECK(!dosvm_write(v, base + 0x500, near[i], sizeof(near[i])));
        cur.tf.eip = 0x500; cur.tf.eflags |= V86_TF; debug_dr6 = 1u << 14;
        v->dpmi.return_steps = 5; step(v, 1, 0);
        CHECK(v->dpmi.return_steps == 4 && (cur.tf.eflags & V86_TF) && v->monitor.vif);
        const uint8_t save = 0x9C; CHECK(!dosvm_write(v, base + 0x540, &save, 1));
        cur.tf.eip = 0x540; debug_dr6 = 1u << 14; step(v, 1, 0);
        CHECK(!v->dpmi.return_steps && !(cur.tf.eflags & V86_TF) && v->monitor.vif);
    }
    const uint8_t nop = 0x90; CHECK(!dosvm_write(v, base + 0x610, &nop, 1));
    v->dpmi.shadow = true; v->dpmi.shadow_cs = (uint16_t)cur.tf.cs; v->dpmi.shadow_eip = 0x600;
    v->dpmi.return_steps = 5; cur.tf.eip = 0x610; cur.tf.eflags |= V86_TF;
    debug_dr6 = 1u << 14; step(v, 1, 0);
    CHECK(!v->dpmi.shadow && v->dpmi.return_steps == 4 && v->monitor.vif);
    /* An IF-clear context never gets re-enabled by a boundary step. */
    cur.tf.eip = 0x300; cur.tf.eflags |= V86_TF; debug_dr6 = 1u << 14;
    v->dpmi.return_steps = 5; v->monitor.vif = false; step(v, 1, 0);
    CHECK(!v->monitor.vif && !v->dpmi.return_steps && !(cur.tf.eflags & V86_TF));
    destroy(v, true); empty();
    puts("f3-10 return steps: 512-instruction cap, stop before PUSHF/POPF/CLI/STI/INT/SS/far/system transfers, no IF forcing PASS");
}
static void test_menu_return_nested(void)
{
    struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, true);
    uint32_t base = seg_base(v, (uint16_t)cur.tf.cs);
    uint8_t nops[2] = {0x90, 0x90}; CHECK(!dosvm_write(v, base + 0x300, nops, sizeof(nops)));
    cur.tf.eip = 0x300;
    v->dpmi.return_cs = (uint16_t)cur.tf.cs; v->dpmi.return_ss = (uint16_t)cur.tf.user_ss;
    v->dpmi.return_depth = (uint8_t)v->dpmi.depth; v->dpmi.return_client = (uint8_t)v->dpmi.clients;
    v->dpmi.return_steps = 100; cur.tf.eflags |= V86_TF;
    /* An actual PM IRQ preserves TF in its kernel-owned saved frame. */
    v->dpmi.vec[8] = (struct dpmi_vec){.sel=(uint16_t)cur.tf.cs, .off=0x700};
    CHECK(dosvm_irq_raise(v->info.key, 0)); step(v, 0x20, 0);
    CHECK(v->dpmi.depth == 1 && v->dpmi.return_steps == 100 && !(cur.tf.eflags & V86_TF));
    struct v86_frame handler = cur;
    /* A callback can reuse the client's CS/SS. It cannot cancel that
     * client's pending step or acquire TF while guest IF is clear. */
    cur.tf.cs = v->dpmi.return_cs; cur.tf.user_ss = v->dpmi.return_ss; cur.tf.eip = 0x300;
    step(v, 0x20, 0);
    CHECK(v->dpmi.return_steps == 100 && !(cur.tf.eflags & V86_TF) && !v->monitor.vif);
    cur = handler;
    uint8_t eoi[] = {0xE6, 0x20}; cur.tf.eax = 0x20; exec(v, eoi, sizeof(eoi), 13, 0);
    pm_iret(v); at_stub(v, DPMI_PM_IRQRET);
    CHECK(!v->dpmi.depth && v->dpmi.return_steps == 100 && (cur.tf.eflags & V86_TF) && v->monitor.vif);
    cur.tf.eip++; debug_dr6 = 1u << 14; step(v, 1, 0);
    CHECK(v->dpmi.return_steps == 99 && (cur.tf.eflags & V86_TF) && v->monitor.vif);
    /* A child with identical selectors also belongs to a different scope. */
    v->dpmi.clients++; cur.tf.eflags &= ~V86_TF; v->monitor.vif = false;
    return_boundary_seed(v, &cur);
    CHECK(v->dpmi.return_steps == 99 && !(cur.tf.eflags & V86_TF));
    v->dpmi.clients--; v->monitor.vif = true;
    return_boundary_seed(v, &cur); CHECK(cur.tf.eflags & V86_TF);
    destroy(v, true); empty();
    /* An enabled client watchpoint must still receive its exception. */
    v = dpmi_vm(true); v86_start(); enter(v, true);
    v->dpmi.return_cs = (uint16_t)cur.tf.cs; v->dpmi.return_ss = (uint16_t)cur.tf.user_ss;
    v->dpmi.return_client = (uint8_t)v->dpmi.clients; v->dpmi.return_steps = 10;
    v->dpmi.watch[0] = (struct dpmi_watch){.used=true, .owner=1, .type=1, .size=1};
    v->dpmi.dr7 = 1;
    v->dpmi.exc[1] = (struct dpmi_vec){.sel=(uint16_t)cur.tf.cs, .off=0x700};
    cur.tf.eip = 0x301; cur.tf.eflags |= V86_TF; debug_dr6 = (1u << 14) | 1;
    step(v, 1, 0);
    CHECK(!left && !v->dpmi.return_steps && v->dpmi.watch[0].hit && cur.tf.eip == 0x700 &&
          v->dpmi.stats.exceptions == 1 && !(cur.tf.eflags & V86_TF));
    destroy(v, true); empty();
    puts("f3-10 return scope: actual IRQ saves/resumes TF, same-CS/SS nested callback and child cannot cancel or inherit parent tracking PASS");
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
        CHECK(!!(cur.tf.eflags & V86_TF) == (!variant || variant == 5));
        if (!variant) {
            cur.tf.eip = 0x400; cur.tf.user_esp += consumed + w + (bits ? 12 : 2);
            debug_dr6 = 1u << 14; step(v, 1, 0);
            CHECK(!v->dpmi.return_steps && !(cur.tf.eflags & V86_TF) && v->monitor.vif);
            cur.tf.eip = 0x402; cur.tf.eax = V86_IF | 2; step(v, 13, 0);
            CHECK(v->dpmi.if_armed && !v->monitor.vif && (cur.tf.eax & V86_TF));
            /* A restored image returns to DMX's value-computing epilogue.
             * It must acquire the next caller boundary with IF unchanged. */
            cur.tf.eip = 0x100; cur.tf.user_esp = 0x600; cur.tf.eflags |= V86_TF;
            debug_dr6 = 1u << 14; step(v, 1, 0);
            CHECK(!v->dpmi.if_armed && v->monitor.vif && v->dpmi.return_steps == DPMI_RETURN_STEPS &&
                  (cur.tf.eflags & V86_TF) && v->dpmi.stats.if_restored == 1);
        }
        destroy(v, true); empty();
    }
    puts("f3-10 near returns: 16/32-bit RET cleanup, DMX register-value epilogue after flags restore, stop before next save; SP assignment/memory store/bad target/IF-clear/debugger refused PASS");
}
static void test_menu(void)
{
    test_menu_quiet_records(); test_menu_heartbeat(); test_menu_vga_dirty_repeat(); test_menu_pm_devices();
    test_menu_halted_heartbeat();
    test_menu_return_seed(); test_menu_return_steps(); test_menu_return_nested();
    test_menu_near_return();
}
