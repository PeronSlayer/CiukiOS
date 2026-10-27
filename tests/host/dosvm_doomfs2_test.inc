/* fx-doomfs2: T23 5bb73dba, DOOM generation 12642, BOOT.LOG n=6/8.
 * Replay real monitor exits. The device handlers, flags and DMA semantics
 * are production; only the physical transport and per-exit cost are fake.
 * SPDX-License-Identifier: GPL-2.0-only */
static uint8_t doomfs2_port(struct dosvm *v, bool pm, uint16_t port, bool write, uint8_t value)
{
    const uint8_t code[] = {write ? 0xEE : 0xEC, 0x90};
    CHECK(!dosvm_write(v, fx27_linear(pm), code, sizeof(code)));
    cur.tf.eip = fx27_code(pm);
    cur.tf.edx = port; cur.tf.eax = 0x12340000u | value;
    step(v, 13, 0);
    CHECK(!left && cur.tf.eip == fx27_code(pm) + 1);
    CHECK((cur.tf.eax & 0xFFFFFF00u) == 0x12340000u);
    return (uint8_t)cur.tf.eax;
}

static void test_doomfs2_ports(void)
{
    for (unsigned pm = 0; pm < 2; pm++) {
        struct dosvm *v = fx27_vm(pm);
        v->fullscreen = true; v->vbe_mode = 0;
        unsigned lean = v->perf.lean_exits;
        /* The OPL timer stays stopped: status reads and register writes
         * are identical to the window's emulated device, including aliases. */
        doomfs2_port(v, pm, 0x388, true, 0xA0);
        doomfs2_port(v, pm, 0x389, true, 0x37);
        CHECK(v->audio.opl.regs[0xA0] == 0x37);
        CHECK(v->perf.lean_exits == lean + 2);
        CHECK(doomfs2_port(v, pm, 0x388, false, 0xFF) == 0);
        CHECK(doomfs2_port(v, pm, 0x389, false, 0) == 0xFF);
        CHECK(v->perf.lean_exits == lean + 4);
        doomfs2_port(v, pm, v->audio.sb.base + 8, true, 0xA1);
        doomfs2_port(v, pm, v->audio.sb.base + 9, true, 0x45);
        CHECK(v->audio.opl.regs[0xA1] == 0x45 && v->perf.lean_exits == lean + 6);
        /* Physical VGA is still excluded. An unexpected trapped VGA access
         * takes the ordinary physical-port dispatch, never model bookkeeping. */
        unsigned dac = v->perf.dac_batched, remaps = v->perf.port_remaps;
        uint8_t index = v->vga.seq_index;
        doomfs2_port(v, pm, 0x3C4, true, 2);
        CHECK(v->perf.lean_exits == lean + 6 && v->perf.dac_batched == dac && v->perf.port_remaps == remaps);
        CHECK(fs_card.seq_index == 2 && v->vga.seq_index == index);
        /* DMA register changes still reach the ordinary pin/settle point
         * when the pool owns pages; no stale dirty flag escapes the trap. */
        lean = v->perf.lean_exits;
        doomfs2_port(v, pm, 0x0A, true, 5);
        CHECK(!v->memory.pool.dma_dirty && v->perf.lean_exits == lean + (v->memory.pool.pages ? 0u : 1u));
        /* Every existing boundary obligation applies in full screen too. */
        for (unsigned gate = 0; gate < 4; gate++) {
            lean = v->perf.lean_exits;
            if (gate == 0) v->fullscreen_hold = true;
            if (gate == 1) v->fullscreen_suspended = true;
            if (gate == 2) g_need_resched = true;
            if (gate == 3) cur.tf.eflags |= V86_TF;
            if (gate < 2) {
                /* A full trap would park until the switch worker resumes
                 * it. Check refusal before that blocking checkpoint. */
                int error = 0;
                CHECK(lean_io(v, &cur, &error) == 0 && !error);
            } else doomfs2_port(v, pm, 0x388, true, 0xA0);
            CHECK(v->perf.lean_exits == lean);
            v->fullscreen_hold = v->fullscreen_suspended = g_need_resched = false;
            cur.tf.eflags &= ~V86_TF;
        }
        /* A newly pending IRQ must run the ordinary tail after exactly one
         * emulated OUT. Do not merely check the fast-path predicate. */
        v->monitor.pic[0].imr = 0xFD;
        CHECK(dosvm_irq_raise(v->info.key, 1));
        unsigned delivered = v->monitor.stats.delivered[1];
        lean = v->perf.lean_exits;
        cur.tf.eip = fx27_code(pm); cur.tf.edx = 0x388; cur.tf.eax = 0xA2;
        step(v, 13, 0);
        /* A PM client with no PM keyboard vector switches to its RM
         * handler through v86_resume (the harness's `left` longjmp). */
        CHECK(v->audio.opl.address == 0xA2 && v->monitor.stats.delivered[1] == delivered + 1);
        CHECK(v->perf.lean_exits == lean);
        v->fullscreen = false;
        destroy(v, true); empty();
    }
    puts("fx-doomfs2 full-screen V86/PM: OPL reads/writes/aliases, physical VGA exclusion, transition/TF/reschedule gates and pending IRQ tail PASS");
}

/* The n=8 measured 5 s mix: 42280 I/O, 4945 PIC/PIT, 7770 PM flags.
 * Price ordinary accesses at that bank's measured averages (ns rounded),
 * lean at 6.6 us (DOOM's windowed n=3/4). No wall-clock timing budget.
 * The OPL cost is unchanged by PCM channel count: the guest mixes its SFX
 * into one DMA stream; this replays the rising accompanying port traffic. */
static void test_doomfs2_cost(void)
{
    uint64_t cost[2] = {0};
    for (unsigned pass = 0; pass < 2; pass++) {
        struct dosvm *v = fx27_vm(true);
        v->fullscreen = true;
        static const uint8_t out[] = {0xEE, 0x90};
        CHECK(!dosvm_write(v, fx27_linear(true), out, sizeof(out)));
        unsigned lean0 = v->perf.lean_exits;
        for (unsigned i = 0; i < 42280 + 4945 + 7770; i++) {
            unsigned lean = v->perf.lean_exits;
            cur.tf.eip = fx27_code(true);
            uint32_t ordinary;
            if (i < 42280) {
                cur.tf.edx = i % 9 < 7 ? 0x388 : 0x389;
                cur.tf.eax = i % 9 < 7 ? 0xA0 : 0x24;
                ordinary = 19946; /* 843330 us / 42280 */
            } else if (i < 42280 + 4945) {
                cur.tf.edx = 0x20; cur.tf.eax = 0x20;
                ordinary = 17728; /* 87666 us / 4945 */
            } else {
                /* The register is not a flags image; these CLIs need no
                 * proof. Alternate CLI/STI with an ordinary successor. */
                uint8_t flags[] = {i & 1 ? 0xFA : 0xFB, 0x90};
                CHECK(!dosvm_write(v, fx27_linear(true), flags, sizeof(flags)));
                cur.tf.eax = 0;
                ordinary = 18072; /* 140422 us / 7770 */
            }
            g_need_resched = !pass;
            step(v, 13, 0);
            g_need_resched = false;
            CHECK(!left && cur.tf.eip == fx27_code(true) + 1);
            cost[pass] += v->perf.lean_exits != lean ? 6600 : ordinary;
        }
        printf("fx-doomfs2 T23 n=8 %s: exits=54995 lean=%u selected_monitor_us=%llu\n",
               pass ? "eligible" : "ordinary control", v->perf.lean_exits - lean0,
               (unsigned long long)(cost[pass] / 1000));
        v->fullscreen = false; destroy(v, true); empty();
    }
    CHECK(cost[1] * 2 < cost[0]);
}

static void test_doomfs2(void)
{
    test_doomfs2_cost();
    test_doomfs2_ports();
}
