/* f3-13 production launch gate, persistent mapping, frame cadence, band
 * transport, focused share and suspended-clock accounting.
 * SPDX-License-Identifier: GPL-2.0-only */
static void test_speed_window(void)
{
    memset(fake_proc, 0, sizeof(fake_proc));
    fake_proc[0].pid = 10; fake_proc[0].state = PROC_LIVE; fake_proc[0].dosvm_capability = true;
    fake_current = &fake_proc[0];
    fake_surface[0] = (struct surface){.info = {sizeof(struct ciuki_surface_info), 640, 480, 2560,
        CIUKI_SURFACE_XRGB8888, 640 * 480 * 4}, .pixels = frame_buffer, .references = 1};
    fake_desc[0] = (struct desktop_description){.kind = DESKTOP_SURFACE, .maximum = PROT_READ | PROT_WRITE, .u.surface = &fake_surface[0]};
    desc_owner[0] = fake_current;
    struct ciuki_dosvm_command w, c;
    CHECK(!create_window(0, "DOOM", &w));
    struct dosvm *v = dosvm_lookup((struct dosvm_key){w.id, w.generation});
    CHECK(v);
    int10(v, 0x0E41, 7, 0, 0);
    CHECK(v->boot_log_count && v->boot_log[v->boot_log_count - 1] == 'A');
    c = command_for(w.id, w.generation);
    CHECK(dosvm_call(CIUKI_DOSVM_LAUNCH, &c, sizeof(c)) == -EAGAIN);
    v->info.dos_ready = true;
    c = command_for(w.id, w.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_LAUNCH, &c, sizeof(c)));
    c = command_for(w.id, w.generation);
    CHECK(dosvm_call(CIUKI_DOSVM_LAUNCH, &c, sizeof(c)) == -EBUSY);
    int10(v, 0x0E42, 7, 0, 0); /* shell echoes are not a program frame */
    c = command_for(w.id, w.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_PRESENT_BANDS, &c, sizeof(c)) && !c.changed && !c.presentation.bands[0]);
    struct v86_frame exec = vf(0x4B00, 0, 0, 0);
    exec.tf.cs = 0x3000; exec.tf.eip = 20; exec.tf.user_ss = 0x2000; exec.tf.user_esp = 0x100;
    CHECK(interrupt_service(v, &exec, 0x21) == -ENOSYS && v->launch_exec && !v->launch_ready);
    int10(v, 0x13, 0, 0, 0);
    CHECK(v->launch_ready && v->video_direct == VIDEO_LINEAR);
    g_ticks = 1000;
    c = command_for(w.id, w.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_PRESENT_BANDS, &c, sizeof(c)) && c.changed == 1 &&
          c.presentation.bands[0] == UINT32_MAX && c.presentation.bands[1] == 0x0FFFFFFF);
    unsigned maps = v->video_linear_maps, harvests = v->perf.harvests;
    linear_store(v, 17 * 320 + 5, 200);
    g_ticks += 15;
    c = command_for(w.id, w.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_PRESENT_BANDS, &c, sizeof(c)) && c.changed == 1 && c.damage.dst_y == 74 && c.damage.height == 2);
    CHECK(c.presentation.bands[0] == (1u << 9) && !c.presentation.bands[1]);
    CHECK(v->video_direct == VIDEO_LINEAR && v->video_linear_maps == maps && v->perf.harvests == harvests + 1);
    CHECK((*window_pte(v, 0xA1000) & (DOSVM_PTE_DIRTY | PTE_U | PTE_W)) == (PTE_U | PTE_W));
    /* A flip bypasses the refresh fallback; no fixed 33 ms ceiling. */
    linear_store(v, 6 * 320, 100); linear_store(v, 150 * 320, 100);
    port_out(v, 0x3D4, 2, 0x000C);
    g_ticks++;
    c = command_for(w.id, w.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_PRESENT_BANDS, &c, sizeof(c)) && c.changed);
    CHECK(c.presentation.bands[0] == (1u << 6) &&
          c.presentation.bands[1] == (1u << 10));
    harvests = v->perf.harvests;
    g_ticks += 15;
    c = command_for(w.id, w.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_PRESENT_BANDS, &c, sizeof(c)) && !c.changed && v->perf.harvests == harvests);
    CHECK(dosvm_task_quantum(&caller, 10) == 10);
    c = command_for(w.id, w.generation); c.focused = 1;
    CHECK(!dosvm_call(CIUKI_DOSVM_FOCUS, &c, sizeof(c)) && dosvm_task_quantum(v->task, 10) == 8 && dosvm_task_focus() == v->task);
    c = command_for(w.id, w.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_FOCUS, &c, sizeof(c)) && dosvm_task_quantum(v->task, 10) == 4);
    c = command_for(w.id, w.generation); c.presentation.bands[0] = 1;
    CHECK(dosvm_call(CIUKI_DOSVM_PRESENT_BANDS, &c, sizeof(c)) == -EINVAL);
    c = command_for(w.id, w.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_CLOSE, &c, sizeof(c)));
    fake_current = 0; empty();
    puts("f3-13 launch and speed: shell hidden until EXEC/video, persistent writable chain-4, sparse band transport, completion cadence and focused share PASS");
}

static unsigned speed_sample_rows, speed_sample_n[16];
static void speed_dump_rows(void)
{
    if (!strstr(last_record, "group=vm_perf_video ")) return;
    const char *n = strstr(last_record, " n=");
    CHECK(n && speed_sample_rows < ARRAY_SIZE(speed_sample_n));
    speed_sample_n[speed_sample_rows++] = (unsigned)strtoul(n + 3, 0, 10);
}
static void test_speed_accounting(void)
{
    struct dosvm *v = create();
    v->info.dos_ready = false; /* first interval includes DOS startup */
    g_boot.flags = 0; g_tsc_per_ms = 1000; fake_tsc = 1000;
    unsigned quiet_records = records;
    unsigned disk_before=disk_records;
    g_ticks += 1000; dosvm_perf_report(v);
    CHECK(disk_records==disk_before);
    dosvm_perf_dump(v);
    CHECK(dosvm_perf_clock() == fake_tsc && !v->perf.sample_count && records == quiet_records);
    /* Quiet boots collect exactly the same CPU accounting. */
    g_ticks -= 1000;
    dosvm_task_switch(v->task);
    g_current = v->task;
    dosvm_perf_enter(); CHECK(v->perf.phase == 1); /* no guest continuation */
    v->continuation = 1;
    fake_tsc = 2000; dosvm_perf_enter();
    CHECK(v->perf.phase == 2 && v->perf.guest_cycles == 1000);
    v->continuation = 0; g_current = &caller;
    v->perf.trap_active = true; v->perf.trap_cycles = 0; v->perf.cause = DOSVM_COST_VGA_IO;
    fake_tsc = 3000; dosvm_task_switch(&caller);
    fake_tsc = 9000; dosvm_task_switch(v->task); /* six milliseconds off CPU */
    fake_tsc = 10000; perf_exit_end(v); perf_phase(v, 1);
    fake_tsc = 11000;
    unsigned records_before = records;
    v->video_frames = 35; v->video_starts = 35; v->pit.edges = 140;
    v->perf.harvests = 140; v->perf.direct_pages = 700; v->perf.harvest_cycles = 123;
    v->perf.port_remaps = 1278; v->perf.remap_cycles = 456; v->perf.burst_insn = 1024;
    v->perf.port_count[DOSVM_VGA_SR_MASK] = 1279; v->perf.port_cycles[DOSVM_VGA_SR_MASK] = 789;
    v->monitor.stats.delivered[0] = 140; v->monitor.stats.insn[V86_OUT] = 200;
    v->idle.blocks = 5; v->idle.count[DOSVM_IDLE_28] = 3; v->idle.count[DOSVM_IDLE_16] = 2;
    v->perf.return_hits = 17; v->perf.return_segmented = 11; v->perf.return_fallbacks = 3;
    v->perf.return_armed = 23; v->perf.return_expired = 2; v->perf.return_reacquired = 19;
    g_ticks += DOSVM_PERF_INTERVAL; dosvm_perf_report(v);
    CHECK(disk_records==disk_before+10);
    CHECK(v->perf.sample_count == 1 && records == records_before);
    const struct dosvm_perf_sample *s = &v->perf.sample[0];
    CHECK(s->frames == 35 && s->guest_us == 2000 && s->monitor_us == 2000 &&
          s->edges == 140 && s->irqs[0] == 140 && s->insn[V86_OUT] == 200);
    CHECK(s->return_hits == 17 && s->return_segmented == 11 && s->return_fallbacks == 3);
    CHECK(s->return_armed == 23 && s->return_expired == 2 && s->return_reacquired == 19);
    CHECK(strstr(tracker_record, "armed=23 hits=17 expired=2 fallback=3 reacquired=19 segmented=11"));
    CHECK(s->cost_count[DOSVM_COST_VGA_IO] == 1 && s->cost_us[DOSVM_COST_VGA_IO] == 2000);
    CHECK(s->harvests == 140 && s->pages == 700 && s->harvest_us == 123);
    CHECK(s->port_remaps == 1278 && s->remap_us == 456 && s->burst_insn == 1024 &&
          s->port_count[DOSVM_VGA_SR_MASK] == 1279 && s->port_us[DOSVM_VGA_SR_MASK] == 789);
    CHECK(s->port_exits[DOSVM_VGA_SR_INDEX] == 1 && s->port_exit_us[DOSVM_VGA_SR_INDEX] == 2000);
    CHECK(s->idle_blocks == 5 && s->idle_reasons == 6 && s->idle_count[DOSVM_IDLE_28] == 3 &&
          s->idle_count[DOSVM_IDLE_16] == 2 && s->busy_permille == 0 && v->idle.busy_permille == 0);
    for (unsigned i = 1; i < 81; i++) {
        v->monitor.pic[0].isr = i & 1; /* guest IRQ context cannot suppress memory sampling */
        g_ticks += DOSVM_PERF_INTERVAL; fake_tsc += 1000; v->video_frames += i;
        dosvm_perf_report(v);
    }
    /* harvest counters are per-interval deltas: only the first interval carries them */
    for (unsigned i = 1; i < 16; i++) CHECK(v->perf.sample[i].harvest_us == 0 && v->perf.sample[i].pages == 0 &&
        !v->perf.sample[i].port_remaps && !v->perf.sample[i].remap_us && !v->perf.sample[i].burst_insn &&
        !v->perf.sample[i].port_count[DOSVM_VGA_SR_MASK] && !v->perf.sample[i].port_us[DOSVM_VGA_SR_MASK] &&
        !v->perf.sample[i].return_armed && !v->perf.sample[i].return_expired &&
        !v->perf.sample[i].return_reacquired && !v->perf.sample[i].return_hits &&
        !v->perf.sample[i].return_segmented && !v->perf.sample[i].return_fallbacks);
    CHECK(v->perf.sample_count == 81 && v->perf.sample[0].frames == 35 &&
          v->perf.sample[1].frames == 1 && v->perf.sample[2].frames == 2 &&
          v->perf.sample[3].frames == 3 && records == records_before);
    unsigned seen = 0;
    for (unsigned i = 4; i < 16; i++) {
        uint32_t frame = v->perf.sample[i].frames;
        CHECK(frame >= 69 && frame <= 80);
        seen |= 1u << (frame - 69);
    }
    CHECK(seen == 0xFFFu); /* latest twelve, independently of the slot helper */
    for (unsigned i = 1; i < 16; i++) CHECK(!v->perf.sample[i].idle_blocks && !v->perf.sample[i].idle_reasons);
    v->monitor.pic[0].isr = 0;
    dosvm_task_switch(&caller);
    uint64_t guest_cycles = v->perf.guest_cycles, monitor_cycles = v->perf.monitor_cycles;
    fake_tsc += 900000; g_ticks += DOSVM_PERF_INTERVAL; dosvm_perf_report(v); /* native STATUS */
    CHECK(v->perf.phase == 0 && v->perf.guest_cycles == guest_cycles && v->perf.monitor_cycles == monitor_cycles);
    g_boot.flags = CBI_F_DIAG_VERBOSE;
    speed_sample_rows = 0; diag_time_hook = speed_dump_rows;
    dosvm_perf_dump(v); diag_time_hook = 0;
    CHECK(speed_sample_rows == 16);
    for (unsigned i = 0; i < 16; i++) CHECK(speed_sample_n[i] == (i < 4 ? i + 1 : i + 67));
    CHECK(!v->perf.sample_count && records > records_before);
    memset(v->perf.sample, 0xFF, sizeof(v->perf.sample));
    v->perf.sample_count = UINT32_MAX;
    dosvm_perf_dump(v); /* widest counters still fit the record envelope */
    CHECK(!v->perf.sample_count && max_record <= 240);
    g_tsc_per_ms = 0; g_boot.flags = 0; fake_tsc = 0;
    destroy(v, true); empty();
    puts("f3-13 accounting: scheduled CPU time excludes suspension, five-second quiet-boot deltas and bounded history, no UART during sampling PASS");
}
static struct dosvm *speed_wait_vm;
static void speed_key_wake(void)
{ CHECK(dosvm_keyboard(dosvm_key(speed_wait_vm), 'x')); sleep_hook = 0; }
static void test_speed_launch_errors(void)
{
    struct dosvm *v = create();
    CHECK(!dosvm_boot(v)); v->info.dos_ready = true; v->launch_armed = true;
    struct v86_frame exec = vf(0x4B00, 0, 0, 0);
    exec.tf.cs = 0x3000; exec.tf.eip = 0x20; exec.tf.user_ss = 0x2000; exec.tf.user_esp = 0x100;
    CHECK(interrupt_service(v, &exec, 0x21) == -ENOSYS);
    uint8_t iret = 0xCF; uint16_t returned[3] = {0x20, 0x3000, V86_IF | V86_CF | 2};
    CHECK(!dosvm_write(v, 0x50000, &iret, 1) && !dosvm_write(v, 0x200FA, returned, sizeof(returned)));
    struct v86_frame f = vf(5, 0, 0, 0);
    f.tf.cs = 0x5000; f.tf.user_ss = 0x2000; f.tf.user_esp = 0xFA;
    f.tf.eflags = V86_VM | V86_IF | 2; f.tf.vector = 13;
    struct task *saved = g_current; g_current = v->task; v->continuation = 1;
    v->monitor.pic[0].imr = 0xFF;
    CHECK(dosvm_trap(&f.tf)); sti();
    CHECK(v->launch_failed && v->launch_error == 5 && f.tf.eip == 0x20 && (f.tf.eflags & V86_CF));
    g_current = saved; v->continuation = 0;
    destroy(v, true); empty();

    v = create(); v->info.dos_ready = true; v->launch_armed = true;
    CHECK(dosvm_keyboard(dosvm_key(v), 0x1C0D));
    f = vf(0, 0, 0, 0);
    CHECK(!dosvm_bios(v, &f, 0x16) && v->launch_enter_consumed && !v->launch_failed);
    int10(v, 0x0E45, 7, 0, 0); /* missing-command output is still hidden */
    CHECK(!v->launch_ready);
    speed_wait_vm = v; sleep_hook = speed_key_wake; f = vf(0, 0, 0, 0);
    CHECK(!dosvm_bios(v, &f, 0x16) && v->launch_failed && !v->launch_exec);
    destroy(v, true); empty();

    v = create(); v->info.dos_ready = true; v->launch_armed = true;
    CHECK(interrupt_service(v, &exec, 0x21) == -ENOSYS);
    int10(v, 4, 0, 0, 0); CHECK(!v->launch_ready); /* refused mode */
    int10(v, 0x0F00, 0, 0, 0); CHECK(!v->launch_ready); /* read-only BIOS */
    int10(v, 0x0E58, 7, 0, 0); CHECK(v->launch_ready); /* text child, same mode */
    destroy(v, true); empty();
    puts("f3-13 launch errors: completed DOS EXEC IRET/CF, final Enter consumed before shell-error detection, first text frame and refused/read-only mode calls PASS");
}
static void test_planar_completions(void)
{
    struct dosvm *v = create();
    int10(v, 0x13, 0, 0, 0);
    port_out(v, 0x3C4, 2, 0x0604);
    port_out(v, 0x3D4, 2, 0x0014);
    port_out(v, 0x3D4, 2, 0xE317);
    port_out(v, 0x3C4, 2, 0x0102);
    cpu_cr3 = v->as.pd_phys;
    unsigned faults = v->video_faults;
    for (unsigned frame = 0; frame < 3; frame++) {
        unsigned base = frame * 0x4000, pages = v->perf.direct_pages,
                 harvests = v->perf.harvests, invalidated = cr3_reloads;
        for (unsigned plane = 0; plane < 4; plane++) {
            port_out(v, 0x3C4, 2, ((1u << plane) << 8) | 2);
            for (unsigned at = 0; at < 16000; at++) {
                *window_memory(v, base + at) = (uint8_t)(at + plane + frame);
                *window_pte(v, 0xA0000 + base + at) |= DOSVM_PTE_DIRTY;
            }
        }
        /* A low-address write is a completion boundary too. */
        port_out(v, 0x3D4, 2, 0x000D);
        CHECK(v->perf.direct_pages == pages && v->perf.harvests == harvests &&
              v->video_direct == 4 && v->video_faults == faults);
        dosvm_vga_sync(v, false);
        CHECK(v->perf.direct_pages == pages + 16 && v->perf.harvests == harvests + 1);
        CHECK(cr3_reloads >= invalidated + 4);
        for (unsigned page = 0; page < VIDEO_WINDOW_PAGES; page++)
            CHECK(!(*window_pte(v, 0xA0000 + page * PAGE_SIZE) & DOSVM_PTE_DIRTY));
        port_out(v, 0x3D4, 2, (base & 0xFF00u) | 0x0C);
        port_out(v, 0x3D4, 2, (base & 0xFF00u) | 0x0C);
        CHECK(v->perf.direct_pages == pages + 16 && v->perf.harvests == harvests + 1);
        struct dosvm_frame out;
        CHECK(!dosvm_frame_digest(v, frame, &out) && v->video_direct == 4);
        /* A snapshot cannot strand the current plane, even with the same
         * map mask: subsequent native stores remain writable. */
        *window_memory(v, base + 16000) = 0xA5;
        *window_pte(v, 0xA0000 + base + 16000) |= DOSVM_PTE_DIRTY;
        port_out(v, 0x3D4, 2, (base & 0xFF00u) | 0x0C);
        CHECK(v->perf.harvests == harvests + 1);
        dosvm_vga_sync(v, false);
        CHECK(v->perf.harvests == harvests + 2 && v->perf.direct_pages == pages + 17 &&
              v->vga.plane[3][base + 16000] == 0xA5 && v->video_faults == faults);
    }
    cpu_cr3 = 0;
    destroy(v, true); empty();
    puts("f3-19 planar completions: deferred 16 dirty pages, harvest-free CRTC flips, batch invalidation and snapshot persistence PASS");
}
/* f3-19: production batch primitive and the real per-column port pattern. */
static void fast_planar(struct dosvm *v)
{
    int10(v, 0x13, 0, 0, 0);
    port_out(v, 0x3C4, 2, 0x0604);
    port_out(v, 0x3D4, 2, 0x0014);
    port_out(v, 0x3D4, 2, 0xE317);
    port_out(v, 0x3C4, 2, 0x0102);
}
static void test_video_pte_batch(void)
{
    struct dosvm *v = create();
    uint32_t frames[16], old[16], dirty, accessed;
    for (unsigned i = 0; i < 16; i++) {
        frames[i] = v->frames[0xA0 + i];
        old[i] = *window_pte(v, 0xA0000 + i * PAGE_SIZE);
    }
    unsigned pages = v->as.pages, tables_before = v->as.tables, mappings = g_user_mappings;
    cpu_cr3 = v->as.pd_phys;
    *window_pte(v, 0xA0000) |= 0x60;
    *window_pte(v, 0xAF000) |= 0x40;
    unsigned flush = cr3_reloads, single = invalidations;
    CHECK(!as_replace_pages(&v->as, 0xA0000, frames, 16, PTE_U | PTE_W, &dirty, &accessed));
    CHECK(dirty == 0x8001 && accessed == 1 && cr3_reloads == flush + 1 && invalidations == single);
    CHECK(v->as.pages == pages && v->as.tables == tables_before && g_user_mappings == mappings);
    CHECK(!as_replace_pages(&v->as, 0xA0000, 0, 16, 0, &dirty, 0) && !dirty && cr3_reloads == flush + 1);
    uint32_t snapshot[16];
    for (unsigned i = 0; i < 16; i++) snapshot[i] = *window_pte(v, 0xA0000 + i * PAGE_SIZE);
    frames[15] |= 1;
    CHECK(as_replace_pages(&v->as, 0xA0000, frames, 16, 0, 0, 0) == -EFAULT);
    CHECK(as_replace_pages(&v->as, 0x3FF000, 0, 2, 0, 0, 0) == -EINVAL);
    CHECK(as_replace_pages(&v->as, 0xA0001, 0, 16, 0, 0, 0) == -EINVAL);
    CHECK(as_replace_pages(&v->as, KERNEL_VBASE, 0, 16, 0, 0, 0) == -EINVAL);
    for (unsigned i = 0; i < 16; i++) CHECK(snapshot[i] == *window_pte(v, 0xA0000 + i * PAGE_SIZE));
    cpu_cr3 = 0; frames[15] &= ~1u;
    CHECK(!as_replace_pages(&v->as, 0xA0000, frames, 16, dosvm_low_flags(0xA0), 0, 0));
    CHECK(cr3_reloads == flush + 1);
    for (unsigned i = 0; i < 16; i++) CHECK(old[i] == *window_pte(v, 0xA0000 + i * PAGE_SIZE));
    destroy(v, true); empty();
    puts("f3-19 PTE batch: D/A masks, one active CR3 reload, no inactive/clean flush, unchanged ownership and atomic rejection PASS");
}
static void test_doom_column_replay(void)
{
    struct dosvm *v = create(); fast_planar(v);
    static cvga_state oracle;
    oracle = v->vga;
    struct ciuki_rect damage;
    CHECK(dosvm_vga_present(v, capture_row, frame_buffer, 0, &damage) == 1);
    cpu_cr3 = v->as.pd_phys;
    for (unsigned frame = 0; frame < 3; frame++) {
        unsigned base = frame * 0x4000, harvests = v->perf.harvests, pages = v->perf.direct_pages;
        unsigned flush = cr3_reloads, writes = v->perf.port_count[DOSVM_VGA_SR_MASK];
        for (unsigned n = 0; n < 1279; n++) {
            unsigned p = n < 320 ? n & 3 : (n - 320) & 3;
            port_out(v, 0x3C5, 1, 1u << p);
            cvga_write_port(&oracle, 0x3C5, (uint8_t)(1u << p));
            unsigned count = n < 320 ? 200 : 80;
            for (unsigned i = 0; i < count; i++) {
                unsigned at = base + (n < 320 ? i * 80 + n / 4 : ((n - 320) / 4 % 200) * 80 + i);
                uint8_t b = (uint8_t)(at + p + frame);
                *window_memory(v, at) = b;
                *window_pte(v, 0xA0000 + at) |= 0x60;
                cvga_write_vram(&oracle, 0xA0000 + at, b);
            }
        }
        CHECK(v->perf.port_count[DOSVM_VGA_SR_MASK] == writes + 1279);
        CHECK(v->perf.harvests == harvests && v->perf.direct_pages == pages);
        CHECK(cr3_reloads - flush >= 1278 && cr3_reloads - flush <= 1279);
        unsigned remaps = v->perf.port_remaps;
        port_out(v, 0x3D4, 2, (base & 0xff00u) | 0x0C);
        CHECK(v->perf.harvests == harvests && v->perf.port_remaps == remaps);
        CHECK(!memcmp(v->vga.plane, oracle.plane, sizeof(oracle.plane)));
        CHECK(dosvm_vga_present(v, capture_row, frame_buffer, (frame + 1) * 40, &damage) == 1);
        CHECK(damage.dst_y == 40 && damage.height == 400); /* black borders unchanged on flips */
        CHECK(v->perf.harvests == harvests + 1 && v->perf.direct_pages == pages + 16);
    }
    /* Explicit GC read-map selection (DOOM fuzz columns): read-only when
     * read/write planes differ; stores continue through the exact model. */
    port_out(v, 0x3C4, 2, 0x0102);
    v->vga.plane[0][10] = 11; v->vga.plane[2][10] = 99;
    port_out(v, 0x3CE, 2, 0x0204);
    CHECK(v->video_direct == 3 && !(*window_pte(v, 0xA0000) & PTE_W) && *window_memory(v, 10) == 99);
    uint8_t byte = 77;
    CHECK(!dosvm_write(v, 0xA000A, &byte, 1) && v->vga.plane[0][10] == 77 && v->vga.plane[2][10] == 99);
    port_out(v, 0x3C5, 1, 1); /* same mask resumes the writable draw window */
    CHECK(v->video_direct == 1 && (*window_pte(v, 0xA0000) & PTE_W));
    *window_pte(v, 0xA0000) |= 0x20;
    port_out(v, 0x3CE, 2, 0x4105);
    CHECK(!v->video_direct && v->perf.latch_read_windows);
    cpu_cr3 = 0; destroy(v, true); empty();
    puts("f3-19 DOOM columns/spans: 1279 OUTs/frame, triple buffers, exact planes, one harvest/frame, border skip and GC04 read window PASS");
}
static void test_modex_conversion(void)
{
    struct dosvm *v = create(); fast_planar(v);
    uint8_t indexes[640]; uint32_t native[1280], lut[256];
    for (unsigned i = 0; i < 256; i++) lut[i] = i * 0x010101u;
    for (unsigned p = 0; p < 4; p++) for (unsigned i = 0; i < 65536; i++)
        v->vga.plane[p][i] = (uint8_t)(i * 7 + p * 53);
    for (unsigned pan = 0; pan < 8; pan++) for (unsigned split = 0; split < 2; split++) {
        v->vga.attr[19] = (uint8_t)pan; v->vga.attr[16] |= 0x20;
        v->vga.crtc[12] = 0xff; v->vga.crtc[13] = 0xe0;
        v->vga.crtc[24] = split ? 79 : 255;
        v->vga.crtc[7] = split ? v->vga.crtc[7] & ~0x10u : v->vga.crtc[7] | 0x10;
        v->vga.crtc[9] = split ? v->vga.crtc[9] & ~0x40u : v->vga.crtc[9] | 0x40;
        v->vga.dac_mask = 0x5f;
        cvga_geometry g; CHECK(cvga_get_geometry(&v->vga, &g));
        for (unsigned y = 0; y < g.height; y++) {
            unsigned base, shift; CHECK(cvga_modex_row(&v->vga, y, &base, &shift));
            CHECK(cvga_render_row8(&v->vga, y, indexes, sizeof(indexes), 0) == g.width);
            cvp_modex_row32(&v->vga, base, shift, g.width, 2, lut, native);
            for (unsigned x = 0; x < g.width; x++) CHECK(native[2*x] == lut[indexes[x]] && native[2*x+1] == lut[indexes[x]]);
            memset(v->video_taken, 0, sizeof(v->video_taken));
            unsigned at = (base + (g.width - 1 + shift) / 4) & 0xffff;
            v->video_taken[3][at >> 6] |= 1u << ((at >> 3) & 7);
            CHECK(cvga_modex_dirty(base, shift, g.width, v->video_taken) == cvga_row_reads_dirty(&v->vga, y, v->video_taken));
        }
    }
    v->vga.crtc[20] |= 0x40;
    unsigned base, pan; CHECK(!cvga_modex_row(&v->vga, 0, &base, &pan));
    destroy(v, true); empty();
    puts("f3-19 Mode X/Y: fused RGB/2x equals generic scanout with pan, split/reset, 64 KiB wrap and DAC mask; dword fallback PASS");
}
static void test_planar_rep_and_pm_burst(void)
{
    struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, true); fast_planar(v);
    client_desc(v, 900, 0x30000, 0xffff, 0xFA, 0x40);
    client_desc(v, 901, 0, 0xffffffffu, 0xF2, 0x40);
    cur.tf.cs = DPMI_SEL(900); cur.tf.ds = cur.tf.es = DPMI_SEL(901); cur.tf.eip = 0;
    v->monitor.vif = false; v->dpmi.if_armed = false; v->dpmi.return_steps = 0;
    port_out(v, 0x3C4, 2, 0x0F02);
    const uint8_t fill[] = {0xF3,0xAB,0x90};
    CHECK(!dosvm_write(v, 0x30000, fill, sizeof(fill)));
    cur.tf.edi = 0xA0000; cur.tf.ecx = 16000; cur.tf.eax = 0x76543210;
    unsigned faults = v->video_faults; fake_cr2 = 0xA0000;
    step(v, 14, 7);
    CHECK(!left && cur.tf.eip == 2 && !cur.tf.ecx && cur.tf.edi == 0xA0000 + 64000 && v->video_faults == faults + 1);
    for (unsigned p = 0; p < 4; p++) for (unsigned i = 0; i < 64000; i++)
        CHECK(v->vga.plane[p][i] == (uint8_t)(0x76543210u >> (8 * (i & 3))));
    /* After the initial trapped store: register setup, RAM read/planar
     * write, INC, LOOP and VGA OUT all execute within this same PM exit. */
    const uint8_t loop[] = {0x88,0x07, 0xB9,0x04,0,0,0, 0x8A,0x06, 0x88,0x07,
                            0x46,0x47, 0xE2,0xF8, 0xBA,0xC5,0x03,0,0, 0xB0,0x03,0xEE,0x90};
    const uint8_t source[] = {9,8,7,6};
    CHECK(!dosvm_write(v, 0x30000, loop, sizeof(loop)) && !dosvm_write(v, 0x40000, source, sizeof(source)));
    cur.tf.eip = 0; cur.tf.esi = 0x40000; cur.tf.edi = 0xA0100; cur.tf.eax = 1; cur.tf.eflags &= ~V86_TF;
    faults = v->video_faults; unsigned burst = v->perf.burst_insn;
    step(v, 14, 7);
    CHECK(!left && cur.tf.eip == sizeof(loop) - 1 && !cur.tf.ecx && cur.tf.esi == 0x40004 && cur.tf.edi == 0xA0104);
    CHECK(v->video_faults == faults + 1 && v->perf.burst_insn > burst + 20 && v->vga.seq[2] == 3);
    for (unsigned p = 0; p < 4; p++) CHECK(!memcmp(v->vga.plane[p] + 0x100, source, sizeof(source)));
    /* A pending IRQ, reschedule or client TF leaves the continuation native. */
    for (unsigned guard = 0; guard < 3; guard++) {
        cur.tf.eip = 0; cur.tf.edi = 0xA0200;
        if (guard == 0) g_need_resched = true;
        if (guard == 1) cur.tf.eflags |= V86_TF;
        if (guard == 2) { v->monitor.vif = true; v->monitor.pic[0].irr |= 1; v->monitor.pic[0].imr &= ~1u; }
        /* Call the production fault entry; trap IRQ delivery is independent. */
        struct v86_frame f = cur; v->frame_pm = true; burst = v->perf.burst_insn;
        CHECK(!dosvm_vga_fault_pm(v, &f) && f.tf.eip == 2 && v->perf.burst_insn == burst);
        g_need_resched = false; cur.tf.eflags &= ~V86_TF; v->monitor.vif = false; v->monitor.pic[0].irr = 0;
    }
    /* The continuation must not emulate past a PM descriptor limit. */
    client_desc(v, 902, 0x40000, 3, 0xF2, 0);
    client_desc(v, 903, 0xA0000, 0xffff, 0xF2, 0);
    const uint8_t limited[] = {0x26,0x88,0x07, 0x8B,0x06, 0x90};
    CHECK(!dosvm_write(v, 0x30000, limited, sizeof(limited)));
    struct v86_frame f = cur;
    f.tf.ds = DPMI_SEL(902); f.tf.es = DPMI_SEL(903);
    f.tf.eip = 0; f.tf.esi = 3; f.tf.edi = 0x100; f.tf.eax = 0x19;
    burst = v->perf.burst_insn;
    CHECK(!dosvm_vga_fault_pm(v, &f) && f.tf.eip == 3 && f.tf.eax == 0x19 && v->perf.burst_insn == burst);
    destroy(v, true); empty();
    puts("f3-19 PM planar: one-exit 64 KiB REP fill; ALU/LOOP, VGA ports and scaler burst; IRQ/TF/reschedule guards PASS");
}
static void test_planar_strings_differential(void)
{
    struct dosvm *v = create(); fast_planar(v);
    static cvga_state expected;
    for (unsigned mode = 0; mode < 2; mode++) for (unsigned back = 0; back < 2; back++)
        for (unsigned mask = 1; mask <= 15; mask += 7) for (unsigned size = 1; size <= 4; size *= 2) {
            for (unsigned p = 0; p < 4; p++) for (unsigned i = 0; i < 80; i++)
                v->vga.plane[p][i] = (uint8_t)(i * 7 + p * 41);
            v->vga.seq[2] = (uint8_t)mask; v->vga.gc[5] = (uint8_t)(0x40 | mode); v->vga.gc[4] = 2;
            memset(v->vga.latch, 0xA5, 4); expected = v->vga;
            unsigned src = back ? 48 : 8, dst = back ? 47 : 9;
            int done = cvga_planar_run(&v->vga, dst, 0, src, size, 7, back, true, 0);
            if (mode && size != 1) { CHECK(!done && !memcmp(&v->vga, &expected, sizeof(expected))); continue; }
            CHECK(done);
            for (unsigned n = 0; n < 7; n++) {
                uint8_t b[4];
                for (unsigned i = 0; i < size; i++) b[i] = cvga_read_vram(&expected, 0xA0000 + src + i);
                for (unsigned i = 0; i < size; i++) cvga_write_vram(&expected, 0xA0000 + dst + i, b[i]);
                src += back ? 0u - size : size; dst += back ? 0u - size : size;
            }
            CHECK(!memcmp(v->vga.plane, expected.plane, sizeof(expected.plane)) &&
                  !memcmp(v->vga.latch, expected.latch, sizeof(expected.latch)));
        }
    /* REP MOVSW from fragmented RAM with elements across physical-page
     * boundaries, both DF directions; 16-bit high halves stay unchanged. */
    const uint8_t code[] = {0xF3,0xA5};
    CHECK(!dosvm_write(v, 0x50000, code, sizeof(code)));
    uint8_t data[6000]; for (unsigned i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i * 17);
    CHECK(!dosvm_write(v, 0x30FFF, data, sizeof(data)));
    port_out(v, 0x3CE, 2, 0x4005); port_out(v, 0x3C4, 2, 0x0F02);
    for (unsigned back = 0; back < 2; back++) {
        struct v86_frame f = vf(0, 0, 0xFACE0000u | 3000, 0);
        f.tf.cs = 0x5000; f.ds = 0x3000; f.es = 0xA000;
        f.tf.esi = 0xCAFE0000u | (0x0FFF + (back ? 5998u : 0));
        f.tf.edi = 0xBEEF0000u | (0x2000 + (back ? 5998u : 0));
        if (back) f.tf.eflags |= 0x400;
        unsigned faults = v->video_faults;
        CHECK(!dosvm_vga_fault(v, &f) && f.tf.eip == 2 && f.tf.ecx == 0xFACE0000u &&
              (f.tf.esi & 0xFFFF0000u) == 0xCAFE0000u && (f.tf.edi & 0xFFFF0000u) == 0xBEEF0000u &&
              v->video_faults == faults + 1);
        for (unsigned p = 0; p < 4; p++) CHECK(!memcmp(v->vga.plane[p] + 0x2000, data, sizeof(data)));
    }
    destroy(v, true); empty();
    puts("f3-19 planar strings: scalar-model equivalence, overlapping DF copies, all latch bytes, word-latch fallback, fragmented RAM MOVSW and high halves PASS");
}
/* F3-31: scalar VGA cycles remain the oracle for the block harvester. */
static void test_chain4_conversion(void)
{
    struct dosvm *v = create(); int10(v, 0x13, 0, 0, 0);
    static cvga_state reference;
    uint8_t bytes[128];
    for (unsigned i = 0; i < sizeof(bytes); i++) bytes[i] = (uint8_t)(i * 37);
    for (unsigned start = 0; start < 16; start++) for (unsigned n = 0; n <= 64; n++) {
        memset(v->vga.dirty, 0, sizeof(v->vga.dirty));
        reference = v->vga;
        unsigned at = (start & 8 ? 65456 : 0) + (start & 7);
        cvga_store_chain4_span(&v->vga, at, bytes, n);
        for (unsigned k = 0; k < n; k++)
            cvga_store_plane(&reference, (at+k)&3, (at+k)&~3u, bytes[k]);
        CHECK(!memcmp(v->vga.plane, reference.plane, sizeof(reference.plane)) &&
              !memcmp(v->vga.dirty, reference.dirty, sizeof(reference.dirty)) &&
              !memcmp(v->vga.latch, reference.latch, sizeof(reference.latch)));
    }
    uint8_t indexes[640]; uint32_t pixels[1280], lut[256];
    for (unsigned p = 0; p < 4; p++) for (unsigned at = 0; at < 65536; at++)
        v->vga.plane[p][at] = (uint8_t)(at * 7 + p * 53);
    for (unsigned i = 0; i < 256; i++) lut[i] = i * 0x010101u;
    for (unsigned pan = 0; pan < 8; pan++) for (unsigned split = 0; split < 2; split++) {
        v->vga.attr[19] = pan; v->vga.attr[16] |= 0x20; v->vga.dac_mask = 0x5f;
        v->vga.crtc[12] = 0xff; v->vga.crtc[13] = 0xe0;
        v->vga.crtc[24] = split ? 79 : 255;
        v->vga.crtc[7] = split ? v->vga.crtc[7] & ~0x10u : v->vga.crtc[7] | 0x10;
        v->vga.crtc[9] = split ? v->vga.crtc[9] & ~0x40u : v->vga.crtc[9] | 0x40;
        cvga_geometry g; CHECK(cvga_get_geometry(&v->vga, &g));
        for (unsigned y = 0; y < g.height; y++) {
            unsigned base, shift; CHECK(cvga_chain4_row(&v->vga, y, &base, &shift));
            CHECK(cvga_render_row8(&v->vga, y, indexes, sizeof(indexes), 0) == g.width);
            cvp_chain4_row32(&v->vga, base, shift, g.width, 2, lut, pixels);
            for (unsigned x = 0; x < g.width; x++)
                CHECK(pixels[2*x] == lut[indexes[x]] && pixels[2*x+1] == lut[indexes[x]]);
            for (unsigned edge = 0; edge < 3; edge++) {
                memset(v->video_taken, 0, sizeof(v->video_taken));
                unsigned at = (base + ((g.width - 1 + shift) / 4) * 4 + edge * 8) & 0xffff;
                v->video_taken[3][at >> 6] |= 1u << ((at >> 3) & 7);
                CHECK(cvga_chain4_dirty(base, shift, g.width, v->video_taken) ==
                      cvga_row_reads_dirty(&v->vga, y, v->video_taken));
            }
        }
    }
    v->vga.crtc[20] &= ~0x40u;
    unsigned base, pan; CHECK(!cvga_chain4_row(&v->vga, 0, &base, &pan));
    destroy(v, true); empty();
    puts("f3-31 chain-4: exact planes/latches/damage for unaligned spans; fused rows match scalar pan, split, wrap and DAC mask PASS");
}
static void test_perf_ranking(void)
{
    struct dosvm *v = create();
    g_boot.flags = 0; g_tsc_per_ms = 1000; fake_tsc = 100;
    dosvm_task_switch(v->task); perf_phase(v, 2);
    /* Collision in the fixed port table, with cost order unlike count order. */
    for (unsigned i = 0; i < 7; i++) {
        v->perf.io_port = 0x20 + i * 256; v->perf.io_valid = true;
        v->perf.trap_active = true; v->perf.trap_cycles = 0;
        v->perf.cause = DOSVM_COST_IO; fake_tsc += (i + 1) * 100;
        perf_exit_end(v);
        uint64_t begin = perf_monitor_stamp(v); fake_tsc += (i + 1) * 50;
        perf_vector_end(v, i % 3, 0x10 + i, 0, begin);
    }
    g_ticks += DOSVM_PERF_INTERVAL - 1; dosvm_perf_report(v);
    CHECK(!v->perf.sample_count);
    g_ticks++; dosvm_perf_report(v);
    CHECK(v->perf.sample_count == 1);
    const struct dosvm_perf_sample *s = &v->perf.sample[0];
    for (unsigned i = 0; i < 5; i++) {
        CHECK(s->top_port[i].id == 0x20 + (6-i)*256 && s->top_port[i].count == 1 &&
              s->top_port[i].cycles == (7-i)*100);
        CHECK(s->top_vector[i].id == ((6-i)%3)*256 + 0x16-i &&
              s->top_vector[i].cycles == (7-i)*50);
    }
    CHECK(!s->port_overflow && s->cost_count[DOSVM_COST_IO] == 7);
    dosvm_task_switch(&caller);
    g_ticks += DOSVM_PERF_INTERVAL; dosvm_perf_report(v);
    CHECK(!v->perf.sample[1].top_port[0].count && !v->perf.sample[1].top_vector[0].count);
    /* Saturation is explicit; existing ports keep accumulating after it. */
    for (unsigned i = 0; i <= DOSVM_PERF_PORTS; i++) {
        v->perf.io_port = i; v->perf.io_valid = true; v->perf.trap_cycles = 0;
        perf_exit_end(v);
    }
    CHECK(v->perf.port_overflow == 1);
    struct dosvm_perf_sample widest; memset(&widest, 0xff, sizeof(widest));
    uint32_t gen = v->info.key.generation; v->info.key.generation = UINT32_MAX;
    perf_live(v, &widest, UINT32_MAX); CHECK(max_record <= 240);
    v->info.key.generation = gen;
    g_tsc_per_ms = 0; fake_tsc = 0;
    destroy(v, true); empty();
    puts("f3-31 perf: quiet 5s cadence, ranked port collisions/vector paths, interval reset, explicit saturation and widest record bounds PASS");
}
static void test_speed(void)
{ test_speed_window(); test_speed_accounting(); test_perf_ranking(); test_chain4_conversion(); test_speed_launch_errors(); test_planar_completions(); test_video_pte_batch();
  test_doom_column_replay(); test_modex_conversion(); test_planar_rep_and_pm_burst(); test_planar_strings_differential(); }
