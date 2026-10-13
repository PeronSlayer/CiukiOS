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
    CHECK(!dosvm_call(CIUKI_DOSVM_PRESENT_BANDS, &c, sizeof(c)) && c.changed == 1 && c.damage.dst_y == 72 && c.damage.height == 16);
    CHECK(c.presentation.bands[0] == ((1u << 9) | (1u << 10)) && !c.presentation.bands[1]);
    CHECK(v->video_direct == VIDEO_LINEAR && v->video_linear_maps == maps && v->perf.harvests == harvests + 1);
    CHECK((*window_pte(v, 0xA1000) & (DOSVM_PTE_DIRTY | PTE_U | PTE_W)) == (PTE_U | PTE_W));
    /* A flip bypasses the refresh fallback; no fixed 33 ms ceiling. */
    linear_store(v, 6 * 320, 100); linear_store(v, 150 * 320, 100);
    port_out(v, 0x3D4, 2, 0x000C);
    g_ticks++;
    c = command_for(w.id, w.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_PRESENT_BANDS, &c, sizeof(c)) && c.changed);
    CHECK(c.presentation.bands[0] == ((1u << 5) | (1u << 6)) &&
          c.presentation.bands[1] == ((1u << 9) | (1u << 10)));
    harvests = v->perf.harvests;
    g_ticks += 15;
    c = command_for(w.id, w.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_PRESENT_BANDS, &c, sizeof(c)) && !c.changed && v->perf.harvests == harvests);
    CHECK(dosvm_task_quantum(&caller, 10) == 10);
    c = command_for(w.id, w.generation); c.focused = 1;
    CHECK(!dosvm_call(CIUKI_DOSVM_FOCUS, &c, sizeof(c)) && dosvm_task_quantum(v->task, 10) == 8);
    c = command_for(w.id, w.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_FOCUS, &c, sizeof(c)) && dosvm_task_quantum(v->task, 10) == 4);
    c = command_for(w.id, w.generation); c.presentation.bands[0] = 1;
    CHECK(dosvm_call(CIUKI_DOSVM_PRESENT_BANDS, &c, sizeof(c)) == -EINVAL);
    c = command_for(w.id, w.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_CLOSE, &c, sizeof(c)));
    fake_current = 0; empty();
    puts("f3-13 launch and speed: shell hidden until EXEC/video, persistent writable chain-4, sparse band transport, completion cadence and focused share PASS");
}

static void test_speed_accounting(void)
{
    struct dosvm *v = create();
    v->info.dos_ready = true;
    g_boot.flags = CBI_F_VERBOSE; g_tsc_per_ms = 1000; fake_tsc = 1000;
    dosvm_task_switch(v->task);
    fake_tsc = 2000; perf_phase(v, 2);
    fake_tsc = 3000; dosvm_task_switch(&caller);
    fake_tsc = 9000; dosvm_task_switch(v->task); /* six milliseconds off CPU */
    fake_tsc = 10000; perf_phase(v, 1);
    fake_tsc = 11000;
    unsigned records_before = records;
    v->video_frames = 35; v->video_starts = 35; v->pit.edges = 140;
    v->monitor.stats.delivered[0] = 140; v->monitor.stats.insn[V86_OUT] = 200;
    g_ticks += 1000; dosvm_perf_report(v);
    CHECK(v->perf.sample_count == 1 && records == records_before);
    const struct dosvm_perf_sample *s = &v->perf.sample[0];
    CHECK(s->frames == 35 && s->guest_us == 2000 && s->monitor_us == 2000 &&
          s->edges == 140 && s->irqs[0] == 140 && s->insn[V86_OUT] == 200);
    for (unsigned i = 1; i < 18; i++) {
        g_ticks += 1000; fake_tsc += 1000; v->video_frames += 70;
        dosvm_perf_report(v);
    }
    CHECK(v->perf.sample_count == 18 && v->perf.sample[1].frames == 70 && records == records_before);
    dosvm_task_switch(&caller);
    dosvm_perf_dump(v);
    CHECK(!v->perf.sample_count && records > records_before);
    memset(v->perf.sample, 0xFF, sizeof(v->perf.sample));
    v->perf.sample_count = UINT32_MAX;
    dosvm_perf_dump(v); /* widest counters still fit the record envelope */
    CHECK(!v->perf.sample_count && max_record <= 240);
    g_tsc_per_ms = 0; g_boot.flags = 0; fake_tsc = 0;
    destroy(v, true); empty();
    puts("f3-13 accounting: scheduled CPU time excludes suspension, per-second deltas and bounded history, no UART during sampling PASS");
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
static void test_speed(void)
{ test_speed_window(); test_speed_accounting(); test_speed_launch_errors(); }
