/* t23-dosperf (2026-10-25): DOS window lag of the T23 trial c42f556e
 * (docs/validation/t23-dos-window-lag-2026-10-25.md), against the
 * production monitor with the fake trap transport (dosvm_dpmi_test.inc
 * step). Warcraft II demo (DOS/4GW Professional 1.97, WAR2.EXE v1.20):
 *
 * - its event loop at 01F7:0022BBC6 saves the flags into a local and
 *   restores them with TEST AH,2 / JZ / STI, never POPF:
 *     66 9C        pushfw
 *     66 58        pop ax
 *     BA 01000000  mov edx,1
 *     66 89 45 FC  mov [ebp-4],ax
 *     89 55 F4     mov [ebp-0Ch],edx
 *     FA           cli
 *   AX still holds the live image, so the CLI was an fx-16 candidate whose
 *   proofs failed on every execution (trial: cli_missed 9232 -> 25770,
 *   host replay: 1,886 misses per simulated second), and each miss kept
 *   the seed demand alive, so no port exit was lean (bank n=4: lean=0,
 *   pit_pic 38,261 exits at 20.2 us);
 * - its PM CLI/STI exits (50,715 per 5 s in bank n=4) cost 20.0 us each on
 *   the ordinary path against 6.0-7.4 us for a lean port exit (DOOM, bank
 *   n=4).
 * Budgets compare with the HEAD a347ca76b behaviour quoted beside them
 * (measured in the Warcraft II host replay, tests/host/dos/dos_extenders.py
 * with GUEST_PLAY_PERF=1). SPDX-License-Identifier: GPL-2.0-only */

static const uint8_t t23_war2_save[16] = {
    0x66, 0x9C, 0x66, 0x58, 0xBA, 0x01, 0x00, 0x00, 0x00, 0x66, 0x89, 0x45, 0xFC, 0x89, 0x55, 0xF4
};
/* The live flags image a PUSHF at the CLI would have stored. */
static uint32_t t23_image(void) { return (cur.tf.eflags & 0x0CD5u) | V86_IF | 2u; }

/* The event-loop CLI: no miss, no seed demand, no image, cheaply. Then the
 * PIT read the DOS/4GW INT 21h 2Ch handler makes under CLI (CLI; OUT 43h;
 * IN 40h; IN 40h; STI) is lean, one exit per instruction or burst. */
static void test_t23_unprovable_cli(void)
{
    struct dosvm *v = fx27_vm(true);
    uint32_t base = seg_base(v, (uint16_t)cur.tf.cs);
    static const uint8_t tail[] = {0xFA, 0x90, 0xFA, 0xE6, 0x43, 0xE4, 0x40, 0xE4, 0x40, 0xFB, 0x90, 0xFB, 0x90};
    CHECK(!dosvm_write(v, base + 0x400, t23_war2_save, sizeof(t23_war2_save)));
    CHECK(!dosvm_write(v, base + 0x410, tail, sizeof(tail)));
    struct dpmi *d = &v->dpmi;
    unsigned lean0 = v->perf.lean_exits, gp0 = v->perf.exits[0];
    unsigned missed = d->cli.missed, candidates = d->cli.candidates;
    for (unsigned i = 0; i < 1000; i++) {
        v->monitor.vif = true;
        cur.tf.eip = 0x410; cur.tf.eax = (cur.tf.eax & 0xFFFF0000u) | t23_image();
        step(v, 13, 0);                                   /* the event-loop CLI */
        CHECK(!left && cur.tf.eip == 0x411 && !v->monitor.vif && !d->if_armed && !(cur.tf.eax & V86_TF));
        cur.tf.eip = 0x412; step(v, 13, 0);               /* the time read: CLI, vif already clear */
        CHECK(!left && cur.tf.eip == 0x413);
        while (cur.tf.eip != 0x419) { CHECK(cur.tf.eip < 0x419); step(v, 13, 0); CHECK(!left); }
        step(v, 13, 0);                                   /* STI */
        CHECK(!left && cur.tf.eip == 0x41A && v->monitor.vif && d->shadow);
        cur.tf.eip = 0x41B; step(v, 13, 0);               /* STI with IF set: the shadow ends */
        CHECK(!left && cur.tf.eip == 0x41C && v->monitor.vif && !d->shadow);
        if (!(i % 10)) g_ticks++;
    }
    unsigned exits = v->perf.exits[0] - gp0, lean = v->perf.lean_exits - lean0;
    printf("t23-dosperf event loop x1000: gp exits=%u lean=%u missed=%u unprovable=%u seed_wanted=%u\n",
           exits, lean, d->cli.missed - missed, d->cli.unprovable, dpmi_seed_wanted(v));
    CHECK(d->cli.missed == missed && d->cli.candidates == candidates && d->cli.unprovable == 1000);
    CHECK(!dpmi_seed_wanted(v));
    /* HEAD: every exit of the loop ordinary once the first miss restarted
     * the seed demand (replay: lean 0 of 17,657 exits per second). */
    fx27_budget("war2_event_loop_ordinary_exits", exits - lean, 0, exits - lean == 0);
    /* A flags-saving CLI is still proven and armed: PUSHF; CLI. */
    static const uint8_t save[] = {0x9C, 0xFA, 0x90};
    CHECK(!dosvm_write(v, base + 0x500, save, sizeof(save)));
    review_boundary(v, 0x500);
    v->monitor.vif = true; d->if_armed = false;
    cur.tf.user_esp -= 4;
    uint32_t image = t23_image(), lin = seg_base(v, (uint16_t)cur.tf.user_ss) + cur.tf.user_esp;
    CHECK(!dosvm_write(v, lin, &image, 4));
    cur.tf.eip = 0x501; step(v, 13, 0);
    CHECK(!left && d->if_armed && !v->monitor.vif && d->cli.unprovable == 1000);
    CHECK(!dosvm_read(v, lin, &image, 4) && (image & V86_TF));
    destroy(v, true); empty();
    puts("t23-dosperf unprovable CLI: Warcraft II's flags-in-a-local CLI is no miss and no seed demand; the PIT read under CLI/STI stays lean; PUSHF; CLI is still armed PASS");
}

/* Lean PM CLI/STI give the ordinary path's results: virtual IF, the STI
 * shadow and its end, the armed image of a flags-saving CLI, a pending IRQ
 * delivered after the shadow, and a CLI that misses its proof (seed demand
 * restarts: the ordinary tail runs). Pass 0 is the scalar control
 * (g_need_resched, as fx27_run's): no lean exit; pass 1 lean. */
static void test_t23_lean_flags(void)
{
    struct { bool vif, shadow, armed, tf, seed; uint32_t eip, delivered, missed; } r[2];
    for (unsigned pass = 0; pass < 2; pass++) {
        struct dosvm *v = fx27_vm(true);
        struct dpmi *d = &v->dpmi;
        uint32_t base = seg_base(v, (uint16_t)cur.tf.cs);
        static const uint8_t code[] = {0xFA, 0xFB, 0xE6, 0x43, 0xFB, 0x9C, 0xFA, 0x90};
        CHECK(!dosvm_write(v, base + 0x600, code, sizeof(code)));
        unsigned lean0 = v->perf.lean_exits;
        bool scalar = !pass;
        /* CLI, not a candidate: virtual IF clear. */
        v->monitor.vif = true; cur.tf.eip = 0x600; cur.tf.eax = 0;
        g_need_resched = scalar; step(v, 13, 0); g_need_resched = false;
        CHECK(!left && cur.tf.eip == 0x601 && !v->monitor.vif && !d->if_waiting);
        CHECK(v->perf.lean_exits == lean0 + pass);
        /* STI: virtual IF set, one-instruction shadow. */
        g_need_resched = scalar; step(v, 13, 0); g_need_resched = false;
        CHECK(!left && cur.tf.eip == 0x602 && v->monitor.vif && d->shadow && d->shadow_eip == 0x602);
        CHECK(v->perf.lean_exits == lean0 + 2 * pass);
        /* The next #GP ends the shadow (pm_trap's rule) and is lean. */
        g_need_resched = scalar; step(v, 13, 0); g_need_resched = false;
        CHECK(!left && cur.tf.eip == 0x604 && !d->shadow && v->perf.lean_exits == lean0 + 3 * pass);
        /* PUSHF; CLI arms the pushed image on both paths. */
        review_boundary(v, 0x605);
        v->monitor.vif = true; d->if_armed = false;
        cur.tf.user_esp -= 4;
        uint32_t image = t23_image(), lin = seg_base(v, (uint16_t)cur.tf.user_ss) + cur.tf.user_esp;
        CHECK(!dosvm_write(v, lin, &image, 4));
        cur.tf.eip = 0x606;
        g_need_resched = scalar; step(v, 13, 0); g_need_resched = false;
        CHECK(!dosvm_read(v, lin, &image, 4));
        r[pass].armed = d->if_armed; r[pass].tf = image & V86_TF;
        CHECK(!left && cur.tf.eip == 0x607 && r[pass].armed && r[pass].tf && !v->monitor.vif);
        cur.tf.user_esp += 4; d->if_armed = false;
        /* STI with a deliverable IRQ pending: not delivered under the
         * shadow (ordinary tail, no lean exit); the next exit delivers it. */
        v->monitor.vif = false; cur.tf.eip = 0x604;
        v->monitor.pic[0].imr = 0xFD; v->monitor.pic[0].irr |= 2;
        uint32_t irqs = v->monitor.stats.delivered[1], lean1 = v->perf.lean_exits;
        g_need_resched = scalar; step(v, 13, 0); g_need_resched = false;
        CHECK(!left && v->monitor.stats.delivered[1] == irqs && d->shadow && v->perf.lean_exits == lean1);
        cur.tf.eip = 0x602;                               /* OUT 43h */
        g_need_resched = scalar; step(v, 13, 0); g_need_resched = false;
        r[pass].delivered = v->monitor.stats.delivered[1] - irqs;
        CHECK(r[pass].delivered == 1 && v->perf.lean_exits == lean1);
        r[pass].vif = v->monitor.vif; r[pass].shadow = d->shadow; r[pass].eip = cur.tf.eip;
        destroy(v, true); empty();
    }
    CHECK(r[0].vif == r[1].vif && r[0].shadow == r[1].shadow && r[0].eip == r[1].eip &&
          r[0].armed == r[1].armed && r[0].tf == r[1].tf);
    /* A candidate CLI whose proof fails (fx-16's rewritten-site case:
     * MOV BX,589Ch; CLI): the miss restarts the seed demand on both paths
     * and the lean path hands the exit to the ordinary tail. */
    for (unsigned pass = 0; pass < 2; pass++) {
        /* The host's 16-bit initial client CS, as test_fx16_cli_site_cache. */
        struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, true);
        v->info.dos_ready = true; v->monitor.state = V86_RUNNING;
        v->monitor.pic[0].imr = v->monitor.pic[1].imr = 0xFF;
        if (g_ticks < 5000) g_ticks = 5000;
        struct dpmi *d = &v->dpmi;
        uint32_t base = seg_base(v, (uint16_t)cur.tf.cs);
        static const uint8_t other[] = {0xBB, 0x9C, 0x58, 0xFA, 0x90};
        CHECK(!dosvm_write(v, base + 0x2FF, other, sizeof(other)));
        review_boundary(v, 0x2FF);
        d->cli_miss_tick = 0; d->cli_settled = true;
        CHECK(!dpmi_seed_wanted(v));
        unsigned lean0 = v->perf.lean_exits, missed = d->cli.missed;
        v->monitor.vif = true; d->if_armed = false;
        cur.tf.eip = 0x302; cur.tf.eax = cur.tf.eflags = V86_IF | 2;
        g_need_resched = !pass; step(v, 13, 0); g_need_resched = false;
        r[pass].missed = d->cli.missed - missed; r[pass].seed = dpmi_seed_wanted(v);
        CHECK(!left && !d->if_armed && cur.tf.eax == (V86_IF | 2) && r[pass].missed == 1 && r[pass].seed);
        CHECK(v->perf.lean_exits == lean0 && d->decode_valid && d->decode_eip == 0x303);
        destroy(v, true); empty();
    }
    puts("t23-dosperf lean PM CLI/STI: virtual IF, STI shadow and its end, armed PUSHF image, IRQ after the shadow and a missed proof match the ordinary path PASS");
}

/* Warcraft II's palette load (WAR2.EXE 01F7:00240689, every retrace):
 *     mov dx,3C8h / out dx,al / inc dx
 *   L: jmp $+2 / mov al,[ebx] / inc ebx / out dx,al / dec ecx / jnz L
 * while an enclosing region's flags image stays armed (if_armed was 1 for
 * the whole host replay). The DAC burst used to refuse any armed image:
 * 769 exits per load (trial: vm_perf_remap dac=0 in every bank, 184,514
 * DAC exits per 5 s in the window at 5.7 us). Now one burst retires the
 * load and the image is untouched. */
static void test_t23_war2_palette(void)
{
    struct dosvm *v = fx27_vm(true);
    struct dpmi *d = &v->dpmi;
    uint32_t base = seg_base(v, (uint16_t)cur.tf.cs);
    /* The enclosing region: PUSHF; CLI arms the pushed image, STI. */
    static const uint8_t region[] = {0x9C, 0xFA, 0xFB, 0x90};
    CHECK(!dosvm_write(v, base + 0x31000, region, sizeof(region)));
    review_boundary(v, 0x31000);
    v->monitor.vif = true; d->if_armed = false;
    cur.tf.user_esp -= 4;
    uint32_t image = t23_image(), lin = seg_base(v, (uint16_t)cur.tf.user_ss) + cur.tf.user_esp;
    CHECK(!dosvm_write(v, lin, &image, 4));
    cur.tf.eip = 0x31001; step(v, 13, 0);
    CHECK(!left && d->if_armed == 1 && !v->monitor.vif);
    step(v, 13, 0);
    CHECK(!left && cur.tf.eip == 0x31003 && v->monitor.vif && d->if_armed == 1);
    uint32_t armed_image;
    CHECK(!dosvm_read(v, lin, &armed_image, 4) && (armed_image & V86_TF));
    static const uint8_t load[] = {0x66, 0xBA, 0xC8, 0x03, 0xEE, 0x66, 0x42,
                                   0xEB, 0x00, 0x8A, 0x03, 0x43, 0xEE, 0x49, 0x75, 0xF7, 0x90};
    CHECK(!dosvm_write(v, base + 0x32000, load, sizeof(load)));
    uint8_t palette[768];
    for (unsigned i = 0; i < sizeof(palette); i++) palette[i] = (uint8_t)((i * 7) & 63);
    CHECK(!dosvm_write(v, 0x50000, palette, sizeof(palette)));
    cur.tf.eip = 0x32000; cur.tf.eax = 0; cur.tf.ebx = 0x50000; cur.tf.ecx = 768;
    unsigned dac0 = v->perf.dac_batched;
    unsigned exits = fx27_run(v, 0x32010, 100000, false);
    printf("t23-dosperf Warcraft II palette load with an armed image: exits=%u dac_batched=%u\n",
           exits, v->perf.dac_batched - dac0);
    fx27_budget("war2_palette_exits_with_armed_image", exits, 3, exits <= 3);
    CHECK(d->if_armed == 1 && cur.tf.ecx == 0 && cur.tf.ebx == 0x50000 + 768);
    CHECK(!dosvm_read(v, lin, &image, 4) && image == armed_image);
    for (unsigned i = 0; i < 256; i++)
        CHECK(v->vga.dac[i][0] == palette[i * 3] && v->vga.dac[i][1] == palette[i * 3 + 1] &&
              v->vga.dac[i][2] == palette[i * 3 + 2]);
    destroy(v, true); empty();
    puts("t23-dosperf Warcraft II palette: one DAC burst per load beside an armed flags image, image untouched, DAC exact PASS");
}

/* Warcraft II's music: OPL3 register writes through the Sound Blaster's
 * 220h-223h (trial: 0220/0222 at 12.8 us, never lean). Lean now, with the
 * register file the ordinary path writes (pass 0: scalar control). */
static void test_t23_sb_opl(void)
{
    uint8_t regs[2][4];
    for (unsigned pass = 0; pass < 2; pass++) {
        struct dosvm *v = fx27_vm(true);
        uint32_t base = seg_base(v, (uint16_t)cur.tf.cs);
        static const uint8_t out[] = {0xEE, 0x90};
        CHECK(!dosvm_write(v, base + 0x700, out, sizeof(out)));
        static const uint16_t port[8] = {0x220, 0x221, 0x220, 0x221, 0x222, 0x223, 0x388, 0x389};
        static const uint8_t value[8] = {0x20, 0x21, 0xA0, 0x44, 0x05, 0x01, 0x40, 0x3F};
        unsigned lean0 = v->perf.lean_exits;
        for (unsigned i = 0; i < 8; i++) {
            cur.tf.eip = 0x700; cur.tf.edx = port[i]; cur.tf.eax = value[i];
            g_need_resched = !pass; step(v, 13, 0); g_need_resched = false;
            CHECK(!left && cur.tf.eip == 0x701);
        }
        CHECK(v->perf.lean_exits == lean0 + 8 * pass);
        const struct dosvm_opl *o = &v->audio.opl;
        regs[pass][0] = o->regs[0x20]; regs[pass][1] = o->regs[0xA0]; regs[pass][2] = o->regs[0x105];
        regs[pass][3] = o->regs[0x40];
        CHECK(regs[pass][0] == 0x21 && regs[pass][1] == 0x44 && regs[pass][2] == 0x01 && regs[pass][3] == 0x3F);
        destroy(v, true); empty();
    }
    CHECK(!memcmp(regs[0], regs[1], sizeof(regs[0])));
    puts("t23-dosperf Sound Blaster OPL3 ports 220h-223h: lean exits with the ordinary register file PASS");
}

static void test_t23dosperf(void)
{
    test_t23_sb_opl();
    test_t23_unprovable_cli();
    test_t23_lean_flags();
    test_t23_war2_palette();
}
