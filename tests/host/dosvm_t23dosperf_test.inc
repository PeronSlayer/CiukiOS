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

/* Codex xhigh review of t23-dosperf (2026-10-25), P2: the shared burst
 * decoder (vga_x86.c) executes MOV/INC/DEC/ADD/SUB/XCHG/shift/NEG/IMUL
 * forms whose destination is ESP. With an armed image at [ESP] and the
 * armed-image guard removed from the DAC/VGA bursts,
 *     out dx,al / add esp,4 / out dx,al / sub esp,4 / out dx,al    (DX=3C9h)
 * retired as one burst, so the monitor never saw ESP above the image (a
 * scalar exit's armed_observe marks it popped); armed_live then kept the
 * stale image as an enclosing owner at the next nested CLI (2 owners instead
 * of 1; 1 instead of 0 after the inner restore), if_recovery_prepare stayed
 * suppressed for an unplanted POPFD and a pending IRQ waited for the forced
 * recovery (reviewer's probe: 1/0/1/1 scalar against 2/1/0/0 batched).
 * Rule now: while an image is armed, the burst decoders refuse every
 * instruction that writes SP/ESP before executing it (cvx_cpu.lock_sp); the
 * CPU runs it natively and the next exit observes the stack. Without an
 * armed image the same sequences still batch, and a palette loop that
 * leaves ESP alone still batches beside the image (test_t23_war2_palette). */
static unsigned t23_sp_bytes(unsigned kind, bool up, uint32_t x, uint8_t *b)
{
    uint32_t val = up ? x + 4 : x;
    switch (kind) {
    case 0: b[0] = 0x43; return 1;                                     /* control: inc ebx */
    case 1: b[0] = 0x83; b[1] = up ? 0xC4 : 0xEC; b[2] = 4; return 3;   /* add/sub esp,4 */
    case 2: b[0] = up ? 0x44 : 0x4C; return 1;                         /* inc/dec esp */
    case 3: b[0] = 0xBC; memcpy(b + 1, &val, 4); return 5;             /* mov esp,imm32 */
    case 4: b[0] = 0xB8; memcpy(b + 1, &val, 4); b[5] = 0x89; b[6] = 0xC4; return 7; /* mov eax,imm32; mov esp,eax */
    case 5: b[0] = 0x66; b[1] = 0x83; b[2] = up ? 0xC4 : 0xEC; b[3] = 4; return 4;   /* add/sub sp,4 */
    case 6: b[0] = 0xC1; b[1] = up ? 0xE4 : 0xEC; b[2] = 1; return 3;   /* shl/shr esp,1 */
    case 7: b[0] = 0xF7; b[1] = 0xDC; return 2;                        /* neg esp, twice */
    case 8: b[0] = 0xFF; b[1] = up ? 0xC4 : 0xCC; return 2;            /* inc/dec esp (FF /0, FF /1) */
    case 9:                                                            /* xchg esp,eax */
        if (up) { b[0] = 0xB8; memcpy(b + 1, &val, 4); b[5] = 0x87; b[6] = 0xE0; return 7; }
        b[0] = 0x87; b[1] = 0xE0; return 2;
    case 10: b[0] = 0x6B; b[1] = 0xE4; b[2] = 1; return 3;             /* imul esp,esp,1: ESP unchanged */
    case 11:                                                           /* mov esp,ss: below the image */
        if (up) { b[0] = 0x8C; b[1] = 0xD4; return 2; }
        b[0] = 0xBC; memcpy(b + 1, &val, 4); return 5;
    }
    return 0;
}
static void test_t23_armed_stack_burst(void)
{
    static const char *const names[] = {"inc ebx", "add/sub esp", "inc/dec esp", "mov esp,imm", "mov esp,eax",
        "add/sub sp", "shl/shr esp", "neg esp", "inc/dec esp (FF)", "xchg esp,eax", "imul esp", "mov esp,ss"};
    for (unsigned pass = 0; pass < 2; pass++)          /* 0: no armed image (control); 1: armed */
    for (unsigned kind = 0; kind < 12; kind++) {
        bool armed = pass, writes_sp = kind != 0, pops = kind >= 1 && kind <= 9;
        struct dosvm *v = fx27_vm(true);
        struct dpmi *d = &v->dpmi;
        uint32_t base = seg_base(v, (uint16_t)cur.tf.cs);
        /* The outer region: PUSHF; CLI; STI; POPFD; NOP; HLT. */
        static const uint8_t region[] = {0x9C, 0xFA, 0xFB, 0x9D, 0x90, 0xF4};
        CHECK(!dosvm_write(v, base + 0x33000, region, sizeof(region)));
        cur.tf.user_esp -= 4;
        uint32_t x = cur.tf.user_esp, lin = seg_base(v, (uint16_t)cur.tf.user_ss) + x;
        uint32_t image = t23_image();
        CHECK(!dosvm_write(v, lin, &image, 4));
        if (armed) {
            review_boundary(v, 0x33000);
            v->monitor.vif = true; d->if_armed = false;
            cur.tf.eip = 0x33001; step(v, 13, 0);
            CHECK(!left && d->if_armed == 1 && !d->armed[0].popped && !v->monitor.vif);
            step(v, 13, 0);                                   /* STI */
            CHECK(!left && cur.tf.eip == 0x33003 && v->monitor.vif && d->if_armed == 1);
        } else { v->monitor.vif = true; d->if_armed = 0; }
        /* mov edx,3C9h / out dx,al / <up> / out dx,al / <down> / out dx,al / hlt */
        uint8_t code[40]; unsigned n = 0;
        code[n++] = 0xBA; code[n++] = 0xC9; code[n++] = 3; code[n++] = 0; code[n++] = 0;
        code[n++] = 0xEE;
        n += t23_sp_bytes(kind, true, x, code + n);
        code[n++] = 0xEE;
        n += t23_sp_bytes(kind, false, x, code + n);
        code[n++] = 0xEE;
        uint32_t end = 0x33100 + n;
        code[n++] = 0xF4;
        CHECK(!dosvm_write(v, base + 0x33100, code, n));
        cur.tf.eip = 0x33100; cur.tf.eax = 0;
        unsigned dac0 = v->perf.dac_batched;
        unsigned exits = fx27_run(v, end, 1000, false);
        unsigned expect = armed && writes_sp ? 3 : 1;
        printf("t23-dosperf review stack burst %s, %s: exits=%u (expected %u) dac_batched=%u popped=%u\n",
               names[kind], armed ? "armed image" : "no image", exits, expect,
               v->perf.dac_batched - dac0, armed ? d->armed[0].popped : 0);
        CHECK(cur.tf.user_esp == x && cur.tf.eip == end);
        CHECK(exits == expect && v->perf.dac_batched - dac0 == (expect == 1 ? 2u : 0u));
        if (armed) {
            CHECK(d->if_armed == 1 && d->armed[0].popped == pops);
            /* The nested region at the same site: a popped image is no owner. */
            review_boundary(v, 0x33000);
            cur.tf.user_esp -= 4;
            uint32_t lin1 = seg_base(v, (uint16_t)cur.tf.user_ss) + cur.tf.user_esp, image1 = t23_image();
            CHECK(!dosvm_write(v, lin1, &image1, 4));
            cur.tf.eip = 0x33001; step(v, 13, 0);
            CHECK(!left && !v->monitor.vif && d->if_armed == (pops ? 1 : 2));
            uint32_t planted;
            CHECK(!dosvm_read(v, lin1, &planted, 4) && (planted & V86_TF));
            /* The inner restore: POPFD loads the planted image, the NOP
             * retires, the CPU reports BS. */
            cur.tf.user_esp += 4;
            cur.tf.eflags = (cur.tf.eflags & ~0x0CD5u) | (planted & 0x0CD5u) | V86_TF;
            cur.tf.eip = 0x33005;
            debug_dr6 = 1u << 14; step(v, 1, 0);
            CHECK(!left && v->monitor.vif && !(cur.tf.eflags & V86_TF) && d->if_armed == (pops ? 0 : 1));
            if (pops) {
                /* An unplanted POPFD under a pending IRQ (NOP; CLI; POPFD;
                 * NOP; HLT): with no stale owner the recovery step runs
                 * and the IRQ is delivered after the restore. */
                static const uint8_t plain[] = {0x90, 0xFA, 0x9D, 0x90, 0xF4};
                CHECK(!dosvm_write(v, base + 0x33200, plain, sizeof(plain)));
                cur.tf.eip = 0x33201; step(v, 13, 0);
                CHECK(!left && !v->monitor.vif && !d->if_armed && cur.tf.eip == 0x33202);
                cur.tf.user_esp -= 4;
                uint32_t lin2 = seg_base(v, (uint16_t)cur.tf.user_ss) + cur.tf.user_esp, image2 = t23_image();
                CHECK(!dosvm_write(v, lin2, &image2, 4));
                v->monitor.pic[0].imr = 0xFD; v->monitor.pic[0].irr |= 2;
                uint32_t irqs = v->monitor.stats.delivered[1];
                step(v, 0x20, 0);                             /* a timer exit at the POPFD */
                CHECK(!left && v->monitor.stats.delivered[1] == irqs);
                CHECK(d->if_step && d->if_restore_step && (cur.tf.eflags & V86_TF));
                cur.tf.user_esp += 4;
                cur.tf.eflags = (cur.tf.eflags & ~(0x0CD5u | V86_TF)) | (image2 & 0x0CD5u);
                cur.tf.eip = 0x33203;
                debug_dr6 = 1u << 14; step(v, 1, 0);
                /* Delivered at this exit (reflected to the real-mode IRQ1
                 * handler: the frame is the handler's, IF clear). */
                CHECK(v->monitor.stats.delivered[1] == irqs + 1 && !d->if_step && !(cur.tf.eflags & V86_TF));
            }
        }
        destroy(v, true); empty();
    }
    puts("t23-dosperf review: a DAC burst beside an armed image stops before every SP/ESP write (12 encodings), the next exit observes the popped image, the nested CLI keeps one owner, the inner restore none, and the unplanted POPFD's recovery step delivers the pending IRQ; the same sequences batch without an image PASS");
}

/* Codex review, inherited P2: pm_trap keeps a PM STI shadow across a
 * physical IRQ at the shadowed CS:EIP, because the IRQ may have landed
 * before that instruction. STI; JMP $ retires at the same address, so
 * address equality is no retirement evidence: with IRQ1 pending, 200 timer
 * exits left shadow=1, vif=1, IRR=2 and 0 deliveries (dpmi_deliver refuses
 * under the shadow). Now such an IRQ exit, with a virtual IRQ deliverable,
 * single-steps the shadowed instruction once with the monitor's own TF
 * (shadow_step): the BS trap after it is the evidence, ends the shadow and
 * that exit delivers. The step is planted only for an instruction
 * return_step_safe admits: never across PUSHF (it would save the TF),
 * POPF/IRET or an SS load (its #DB inhibition). */
static void test_t23_shadow_self_jump(void)
{
    /* 1. STI; L: JMP L with IRQ1 pending: timer exits at L. */
    struct dosvm *v = fx27_vm(true);
    struct dpmi *d = &v->dpmi;
    uint32_t base = seg_base(v, (uint16_t)cur.tf.cs);
    static const uint8_t loop[] = {0xFB, 0xEB, 0xFE, 0xF4};
    CHECK(!dosvm_write(v, base + 0x34000, loop, sizeof(loop)));
    v->monitor.vif = false;
    v->monitor.pic[0].imr = 0xFD; v->monitor.pic[0].irr |= 2;
    uint32_t irqs = v->monitor.stats.delivered[1];
    cur.tf.eip = 0x34000; step(v, 13, 0);                      /* STI: shadow over JMP $ */
    CHECK(!left && v->monitor.vif && d->shadow && d->shadow_eip == 0x34001 &&
          v->monitor.stats.delivered[1] == irqs && !(cur.tf.eflags & V86_TF));
    for (unsigned i = 0; i < 2; i++) {
        cur.tf.eip = 0x34001; step(v, 0x20, 0);
        CHECK(!left && d->shadow && v->monitor.vif && v->monitor.stats.delivered[1] == irqs);
    }
    /* The first IRQ exit planted the owned step; the second, before the
     * stepped instruction retired, kept it. */
    CHECK(d->shadow_step && (cur.tf.eflags & V86_TF) && !d->if_step && d->shadow_steps == 1);
    /* JMP $ retires under TF: BS at the same address. The shadow ends and
     * the exit delivers IRQ1; no TF is left in the frame or the handler. */
    debug_dr6 = 1u << 14; step(v, 1, 0);
    CHECK(!d->shadow && !d->shadow_step && !(cur.tf.eflags & V86_TF));
    CHECK(v->monitor.stats.delivered[1] == irqs + 1);          /* the handler's frame now */
    destroy(v, true); empty();

    /* 2. STI; PUSHF: no step across a flags save; the shadow ends at the
     * next trap elsewhere, which delivers. */
    v = fx27_vm(true); d = &v->dpmi;
    base = seg_base(v, (uint16_t)cur.tf.cs);
    static const uint8_t save[] = {0xFB, 0x9C, 0x90, 0xF4};
    static const uint8_t out[] = {0xEE, 0x90};
    CHECK(!dosvm_write(v, base + 0x34100, save, sizeof(save)));
    CHECK(!dosvm_write(v, base + 0x34200, out, sizeof(out)));
    v->monitor.vif = false;
    v->monitor.pic[0].imr = 0xFD; v->monitor.pic[0].irr |= 2;
    irqs = v->monitor.stats.delivered[1];
    cur.tf.eip = 0x34100; step(v, 13, 0);
    CHECK(!left && d->shadow && d->shadow_eip == 0x34101);
    cur.tf.eip = 0x34101; step(v, 0x20, 0);
    CHECK(!left && d->shadow && !d->shadow_step && !(cur.tf.eflags & V86_TF) &&
          v->monitor.stats.delivered[1] == irqs && d->shadow_steps == 0);
    cur.tf.eip = 0x34200; cur.tf.edx = 0x3C9; step(v, 13, 0);  /* PUSHF; NOP ran natively */
    CHECK(!d->shadow && v->monitor.stats.delivered[1] == irqs + 1 && !(cur.tf.eflags & V86_TF));
    destroy(v, true); empty();

    /* 3. STI; OUT: the step is planted, then the shadowed instruction
     * traps itself (#GP(0)): TF is taken back, the shadow ends as before,
     * the OUT is emulated and the exit delivers. */
    v = fx27_vm(true); d = &v->dpmi;
    base = seg_base(v, (uint16_t)cur.tf.cs);
    static const uint8_t trap[] = {0xFB, 0xEE, 0xF4};
    CHECK(!dosvm_write(v, base + 0x34300, trap, sizeof(trap)));
    v->monitor.vif = false;
    v->monitor.pic[0].imr = 0xFD; v->monitor.pic[0].irr |= 2;
    irqs = v->monitor.stats.delivered[1];
    cur.tf.eip = 0x34300; cur.tf.edx = 0x3C9; cur.tf.eax = 0x15; step(v, 13, 0);
    CHECK(!left && d->shadow && d->shadow_eip == 0x34301);
    cur.tf.eip = 0x34301; step(v, 0x20, 0);
    CHECK(!left && d->shadow && d->shadow_step && (cur.tf.eflags & V86_TF) && v->monitor.stats.delivered[1] == irqs);
    step(v, 13, 0);                                             /* the OUT's #GP(0) */
    CHECK(!d->shadow && !d->shadow_step && !(cur.tf.eflags & V86_TF));
    CHECK(v->monitor.stats.delivered[1] == irqs + 1 && d->shadow_steps == 1);
    destroy(v, true); empty();
    puts("t23-dosperf review: an IRQ at a shadowed self-jump single-steps the shadowed instruction with the monitor's TF and delivers at its BS; no step across PUSHF; a trapping shadowed instruction gives the TF back PASS");
}

static void test_t23dosperf(void)
{
    test_t23_sb_opl();
    test_t23_unprovable_cli();
    test_t23_lean_flags();
    test_t23_war2_palette();
    test_t23_armed_stack_burst();
    test_t23_shadow_self_jump();
}
