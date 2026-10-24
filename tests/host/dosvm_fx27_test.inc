/* fx-27 windowed DOS graphics (docs/design/f3-acceptance.md "fx-27"):
 * lean port exits, DAC palette bursts, the chain-4 linear presenter, the
 * dirty-page VBE presenter and the surface writer, against the production
 * monitor with the fake trap transport. The replays use the production
 * decoder as the "native" CPU for the instructions that do not trap; a
 * port instruction is the #GP the hardware raises at IOPL 0. Each budget
 * below fails on the pre-fx-27 code (6b0a5481), whose results are quoted
 * beside it. SPDX-License-Identifier: GPL-2.0-only */

/* Budgets: fatal in the normal suite; with -DFX27_HEAD_BASELINE (the same
 * replays compiled against the pre-fx-27 production sources) each result
 * is printed and the run continues, so every budget shows its baseline. */
static unsigned fx27_budget_failures;
static void fx27_budget(const char *name, unsigned long long value, unsigned long long limit, bool ok)
{
    printf("fx-27 budget %s: value=%llu limit=%llu %s\n", name, value, limit, ok ? "PASS" : "FAIL");
#ifdef FX27_HEAD_BASELINE
    if (!ok) fx27_budget_failures++;
#else
    CHECK(ok);
#endif
}
#ifdef FX27_HEAD_BASELINE
#define FX27_LEAN(v) 0u
#define FX27_DAC(v) 0u
#else
#define FX27_LEAN(v) ((v)->perf.lean_exits)
#define FX27_DAC(v) ((v)->perf.dac_batched)
#endif
static unsigned fx27_rows;
static int fx27_count_row(void *arg, unsigned y, const void *pixels, unsigned bytes)
{ fx27_rows++; return capture_row(arg, y, pixels, bytes); }
struct fx27_native { struct dosvm *v; bool port; };
static int fx27_read(void *arg, uint32_t address, uint8_t *value)
{ struct fx27_native *n = arg; return dosvm_read(n->v, address, value, 1) ? -1 : 0; }
static int fx27_write(void *arg, uint32_t address, uint8_t value)
{ struct fx27_native *n = arg; return dosvm_write(n->v, address, &value, 1) ? -1 : 0; }
static int fx27_port(void *arg, uint16_t port, unsigned size, int write, uint32_t *value)
{ (void)port; (void)size; (void)write; (void)value; ((struct fx27_native *)arg)->port = true; return -1; }
static uint32_t fx27_base(void *arg, uint16_t sel)
{ struct dpmi_seg s; CHECK(dpmi_seg(((struct fx27_native *)arg)->v, sel, &s)); return s.base; }
/* Run until EIP reaches end; count the #GP exits the hardware would take. */
static unsigned fx27_run(struct dosvm *v, uint32_t end, unsigned budget, bool scalar)
{
    unsigned exits = 0;
    while (cur.tf.eip != end) {
        CHECK(budget--);
        bool pm = in_pm();
        struct fx27_native ctx = {v, false};
        cvx_bus bus = {.read = fx27_read, .write = fx27_write, .fetch = fx27_read, .context = &ctx,
                       .segment_base = pm ? fx27_base : 0, .port = fx27_port};
        cvx_cpu c = {0};
        if (pm) {
            struct dpmi_seg cs; CHECK(dpmi_seg(v, (uint16_t)cur.tf.cs, &cs));
            uint32_t sel[] = {cur.tf.es, cur.tf.cs, cur.tf.user_ss, cur.tf.ds, cur.tf.fs, cur.tf.gs};
            for (unsigned i = 0; i < 6; i++) c.sreg[i] = (uint16_t)sel[i];
            c.code32 = cs.big; c.seg_base[CVX_CS] = cs.base; c.base_valid = 1u << CVX_CS;
        } else {
            uint32_t seg[] = {cur.es, cur.tf.cs, cur.tf.user_ss, cur.ds, cur.fs, cur.gs};
            for (unsigned i = 0; i < 6; i++) { c.sreg[i] = (uint16_t)seg[i]; c.seg_base[i] = seg[i] << 4; }
        }
        c.gpr[0] = cur.tf.eax; c.gpr[1] = cur.tf.ecx; c.gpr[2] = cur.tf.edx; c.gpr[3] = cur.tf.ebx;
        c.gpr[4] = cur.tf.user_esp; c.gpr[5] = cur.tf.ebp; c.gpr[6] = cur.tf.esi; c.gpr[7] = cur.tf.edi;
        c.eip = cur.tf.eip; c.eflags = cur.tf.eflags;
        cvx_result r;
        int e = cvx_execute(&c, &bus, 4096, &r);
        if (ctx.port) {
            g_need_resched = scalar; /* the f3-14 scalar control: no lean exit, no burst */
            step(v, 13, 0); exits++;
            g_need_resched = false;
            CHECK(!left && v->info.state < DOSVM_STOPPING);
            continue;
        }
        CHECK(e == CVX_DONE);
        cur.tf.eax = c.gpr[0]; cur.tf.ecx = c.gpr[1]; cur.tf.edx = c.gpr[2]; cur.tf.ebx = c.gpr[3];
        cur.tf.user_esp = c.gpr[4]; cur.tf.ebp = c.gpr[5]; cur.tf.esi = c.gpr[6]; cur.tf.edi = c.gpr[7];
        cur.tf.eip = c.eip; cur.tf.eflags = c.eflags;
    }
    return exits;
}
/* A DOS VM ready for lean exits: booted, IRQs masked, CLI sites settled
 * (fx-16: no seed demand), PM flat 32-bit client or V86. */
static struct dosvm *fx27_vm(bool pm)
{
    struct dosvm *v = dpmi_vm(true);
    v86_start();
    if (pm) {
        enter(v, true); CHECK(in_pm());
        client_desc(v, 900, 0, UINT32_MAX, 0xFA, 0x40);
        client_desc(v, 901, 0, UINT32_MAX, 0xF2, 0x40);
        cur.tf.cs = DPMI_SEL(900); cur.tf.ds = cur.tf.es = cur.tf.user_ss = DPMI_SEL(901);
        cur.tf.user_esp = 0x2F000;
        v->dpmi.cli_settled = true; v->dpmi.cli_miss_tick = 0;
        if (g_ticks < 5000) g_ticks = 5000;
    } else {
        /* Plain V86 program: no DPMI client (dpmi_vm's host stays idle). */
        cur.tf.cs = 0x3000; cur.ds = cur.es = 0x2000;
    }
    v->info.dos_ready = true;
    v->monitor.pic[0].imr = v->monitor.pic[1].imr = 0xFF;
    v->monitor.state = V86_RUNNING; v->monitor.vif = true;
    int10(v, 0x13, 0, 0, 0);
    return v;
}
static uint32_t fx27_code(bool pm) { return pm ? 0x30000u : 0u; }
static uint32_t fx27_linear(bool pm) { return pm ? 0x30000u : 0x30000u; }

/* Quake 1.06 VGA_SetPalette (vid_vga.c) as compiled in QUAKE.EXE: per byte
 * movzx eax,[edi+esi]; mov ecx,ebx; sar eax,cl; mov edx,3C9h; out dx,al;
 * inc edi; cmp edi,2FFh; jng. The index OUT (3C8h) precedes it. */
static unsigned fx27_quake_palette(bool scalar, uint8_t dac[256][3], unsigned *lean, unsigned *dac_batched)
{
    struct dosvm *v = fx27_vm(true);
    uint8_t code[64]; unsigned n = 0;
    code[n++] = 0xBA; code[n++] = 0xC8; code[n++] = 3; code[n++] = 0; code[n++] = 0;   /* mov edx,3C8h */
    code[n++] = 0x31; code[n++] = 0xC0;                                               /* xor eax,eax */
    code[n++] = 0xEE;                                                                 /* out dx,al */
    code[n++] = 0x31; code[n++] = 0xFF;                                               /* xor edi,edi */
    unsigned loop = n;
    code[n++] = 0x0F; code[n++] = 0xB6; code[n++] = 0x04; code[n++] = 0x37;           /* movzx eax,[edi+esi] */
    code[n++] = 0x89; code[n++] = 0xD9;                                               /* mov ecx,ebx */
    code[n++] = 0xD3; code[n++] = 0xF8;                                               /* sar eax,cl */
    code[n++] = 0xBA; code[n++] = 0xC9; code[n++] = 3; code[n++] = 0; code[n++] = 0;   /* mov edx,3C9h */
    code[n++] = 0xEE;                                                                 /* out dx,al */
    code[n++] = 0x47;                                                                 /* inc edi */
    code[n++] = 0x81; code[n++] = 0xFF; code[n++] = 0xFF; code[n++] = 2; code[n++] = 0; code[n++] = 0;
    code[n++] = 0x7E; code[n] = (uint8_t)(loop - (n + 1)); n++;                       /* jng loop */
    code[n++] = 0x90;
    CHECK(!dosvm_write(v, fx27_linear(true), code, n));
    uint8_t source[768];
    for (unsigned i = 0; i < 768; i++) source[i] = (uint8_t)(i * 5 + 3);
    CHECK(!dosvm_write(v, 0x40000, source, sizeof(source)));
    cur.tf.eip = fx27_code(true); cur.tf.esi = 0x40000; cur.tf.ebx = 2;
    unsigned lean0 = FX27_LEAN(v), dac0 = FX27_DAC(v);
    unsigned exits = fx27_run(v, fx27_code(true) + n - 1, 100000, scalar);
    CHECK(cur.tf.edi == 0x300 && (uint8_t)cur.tf.eax == (uint8_t)(source[767] >> 2) && cur.tf.edx == 0x3C9);
    for (unsigned i = 0; i < 768; i++) CHECK(v->vga.dac[i / 3][i % 3] == (source[i] >> 2));
    memcpy(dac, v->vga.dac, sizeof(v->vga.dac));
    *lean = FX27_LEAN(v) - lean0; *dac_batched = FX27_DAC(v) - dac0;
    destroy(v, true); empty();
    return exits;
}
/* DOOM 1.9 I_SetPalette (i_ibm.c, Watcom, 16-bit V86 form here): lodsb from
 * the gamma-mapped table, shr al,2, out dx,al, loop. */
static unsigned fx27_v86_palette(bool scalar, unsigned *lean)
{
    struct dosvm *v = fx27_vm(false);
    const uint8_t code[] = {0xBA, 0xC8, 0x03, 0x30, 0xC0, 0xEE,  /* mov dx,3C8h; xor al,al; out dx,al */
        0x42, 0xBE, 0x00, 0x01, 0xB9, 0x00, 0x03,                  /* inc dx; mov si,100h; mov cx,300h */
        0xAC, 0xC0, 0xE8, 0x02, 0xEE, 0xE2, 0xF9,                  /* lodsb; shr al,2; out dx,al; loop */
        0x90};
    CHECK(!dosvm_write(v, 0x30000, code, sizeof(code)));
    uint8_t source[768];
    for (unsigned i = 0; i < 768; i++) source[i] = (uint8_t)(255 - i);
    CHECK(!dosvm_write(v, 0x20100, source, sizeof(source)));
    cur.tf.eip = 0;
    unsigned lean0 = FX27_LEAN(v);
    unsigned exits = fx27_run(v, sizeof(code) - 1, 100000, scalar);
    for (unsigned i = 0; i < 768; i++) CHECK(v->vga.dac[i / 3][i % 3] == (uint8_t)(source[i] >> 2));
    CHECK((uint16_t)cur.tf.ecx == 0 && (uint16_t)cur.tf.esi == 0x400);
    *lean = FX27_LEAN(v) - lean0;
    destroy(v, true); empty();
    return exits;
}
static void test_fx27_palette(void)
{
    static uint8_t scalar_dac[256][3], batch_dac[256][3];
    unsigned lean, batched;
    unsigned scalar = fx27_quake_palette(true, scalar_dac, &lean, &batched);
    CHECK(scalar == 769 && !lean && !batched);
    /* Pre-fx-27: 769 exits, one per OUT (vga_io 21.8 us each on the T23). */
    unsigned exits = fx27_quake_palette(false, batch_dac, &lean, &batched);
    CHECK(!memcmp(scalar_dac, batch_dac, sizeof(batch_dac)));
    fx27_budget("quake_palette_exits", exits, 2, exits <= 2);
    unsigned v86_scalar = fx27_v86_palette(true, &lean);
    CHECK(v86_scalar == 769 && !lean);
    unsigned v86 = fx27_v86_palette(false, &lean);
    fx27_budget("doom_v86_palette_exits", v86, 2, v86 <= 2);
    printf("fx-27 palette loads: Quake PM32 %u -> %u exits (dac_batched=%u), DOOM-style V86 %u -> %u exits, DAC exact\n",
           scalar, exits, batched, v86_scalar, v86);
}
/* Per-exit work and per-frame presentation budgets on observables that
 * exist before and after fx-27 (fake CR3/INVLPG/preemption counters,
 * presenter and surface counters). */
static void test_fx27_frame_budgets(void)
{
    /* DOOM Mode Y: 1279 map-mask OUTs per frame (planar.asm R_DrawColumn /
     * R_DrawSpan, the f3-19 replay). No whole-TLB CR3 reload per plane
     * swap, and no per-exit housekeeping (kpreempt_point) on these exits. */
    struct dosvm *v = fx27_vm(true);
    port_out(v, 0x3C4, 2, 0x0604); port_out(v, 0x3D4, 2, 0x0014);
    port_out(v, 0x3D4, 2, 0xE317); port_out(v, 0x3C4, 2, 0x0102);
    const uint8_t out[] = {0xEE, 0x90};
    CHECK(!dosvm_write(v, fx27_linear(true), out, sizeof(out)));
    cpu_cr3 = v->as.pd_phys;
    unsigned reloads = cr3_reloads, points = preempt_points;
    for (unsigned n = 0; n < 1279; n++) {
        unsigned p = n < 320 ? n & 3 : (n - 320) & 3;
        cur.tf.eip = fx27_code(true); cur.tf.edx = 0x3C5; cur.tf.eax = 1u << p;
        step(v, 13, 0); CHECK(!left && v->vga.seq[2] == (1u << p));
    }
    fx27_budget("doom_frame_cr3_reloads", cr3_reloads - reloads, 0, cr3_reloads == reloads);
    fx27_budget("doom_frame_exit_preemption_points", preempt_points - points, 0, preempt_points == points);
    cpu_cr3 = 0; destroy(v, true); empty();
    /* Quake mode 13h: a whole 320x200 frame stored through the chain-4
     * window, then presented: no 64 KiB deinterleave into the planes. */
    v = create(); int10(v, 0x13, 0, 0, 0);
    struct ciuki_rect d;
    CHECK(dosvm_vga_present(v, capture_row, frame_buffer, 0, &d) == 1);
    for (unsigned at = 0; at < 64000; at++) linear_store(v, at, (uint8_t)(at * 7));
    unsigned pages = v->perf.direct_pages;
    CHECK(dosvm_vga_present(v, capture_row, frame_buffer, 40, &d) == 1 && d.height == 400);
    fx27_budget("quake_13h_frame_deinterleaved_pages", v->perf.direct_pages - pages, 0, v->perf.direct_pages == pages);
    destroy(v, true); empty();
    /* A changed row proven by the index comparison is written without
     * reading the surface back. */
    v = create(); int10(v, 0x13, 0, 0, 0);
    static uint8_t pixels[CIUKI_DOSVM_WIDTH * CIUKI_DOSVM_HEIGHT * 4];
    struct surface s = {.info = {.allocation_bytes = sizeof(pixels), .width = CIUKI_DOSVM_WIDTH,
        .height = CIUKI_DOSVM_HEIGHT, .stride = CIUKI_DOSVM_WIDTH * 4}, .pixels = pixels, .references = 2};
#ifdef FX27_HEAD_BASELINE
    struct row_target target = {&s, true};
#else
    struct row_target target = {&s, true, v};
#endif
    CHECK(dosvm_vga_present(v, put_row, &target, 0, &d) == 1);
    target.force = false;
    for (unsigned x = 0; x < 320; x++) linear_store(v, 50 * 320 + x, (uint8_t)(x + 1));
    unsigned reads = surface_reads;
    CHECK(dosvm_vga_present(v, put_row, &target, 40, &d) == 1 && d.height == 2);
    fx27_budget("changed_row_surface_readbacks", surface_reads - reads, 0, surface_reads == reads);
    destroy(v, true); empty();
    /* VESA 640x480x8: 32 lines written (a status bar) convert at most the
     * rows of the dirty pages, not the whole frame. */
    CHECK(!dosvm_create(0, &v));
    bool available = fs_vbe_available; fs_vbe_available = true;
    int10(v, 0x4F02, 0x4101, 0, 0);
    CHECK(v->vbe_mode == 0x101 && v->vbe_buffer.used);
    CHECK(dosvm_vga_present(v, capture_row, frame_buffer, 0, &d) == 1);
    fx27_rows = 0;
    for (unsigned y = 448; y < 480; y++) {
        uint8_t row[640]; memset(row, (int)y, sizeof(row));
        CHECK(!dosvm_write(v, v->vbe_buffer.base + y * 640, row, sizeof(row)));
    }
    for (unsigned page = 448 * 640 / PAGE_SIZE; page <= (480 * 640 - 1) / PAGE_SIZE; page++) *vbe_pte(v, page) |= 0x40;
    CHECK(dosvm_vga_present(v, fx27_count_row, frame_buffer, 40, &d) == 1);
    fx27_budget("vbe_status_bar_rows_converted", fx27_rows, 40, fx27_rows <= 40);
    fs_vbe_available = available; destroy(v, true); empty();
}
#ifndef FX27_HEAD_BASELINE
/* The burst never commits a store, a VGA memory read or an instruction it
 * cannot decode; a deliverable IRQ ends it before the next instruction. */
static void test_fx27_dac_limits(void)
{
    struct dosvm *v = fx27_vm(true);
    /* out dx,al; mov [ebx],al (a store); out dx,al ... */
    const uint8_t store[] = {0xEE, 0x88, 0x03, 0xEE, 0x90};
    CHECK(!dosvm_write(v, 0x30000, store, sizeof(store)));
    cur.tf.eip = 0x30000; cur.tf.edx = 0x3C9; cur.tf.ebx = 0x41000; cur.tf.eax = 7;
    unsigned dac0 = v->perf.dac_batched;
    CHECK(fx27_run(v, 0x30000 + sizeof(store) - 1, 100, false) == 2 && v->perf.dac_batched == dac0);
    uint8_t stored; CHECK(!dosvm_read(v, 0x41000, &stored, 1) && stored == 7);
    /* A VGA memory load (latch side effect) ends the burst. */
    const uint8_t vram[] = {0xEE, 0x8A, 0x03, 0xEE, 0x90};
    CHECK(!dosvm_write(v, 0x30000, vram, sizeof(vram)));
    cur.tf.eip = 0x30000; cur.tf.ebx = 0xA0000;
    dosvm_vga_model(v); /* closed window: the native load would trap as a #PF */
    g_need_resched = false; step(v, 13, 0);
    CHECK(cur.tf.eip == 0x30001 && v->perf.dac_batched == dac0);
    /* A deliverable IRQ stops the burst after the trapped OUT: IRQ1 pending,
     * unmasked, virtual IF set. The ordinary tail then delivers it. */
    const uint8_t loop[] = {0xEE, 0x90, 0xEE, 0x90};
    CHECK(!dosvm_write(v, 0x30000, loop, sizeof(loop)));
    cur.tf.eip = 0x30000;
    v->monitor.pic[0].imr = 0xFD; v->monitor.pic[0].irr |= 2;
    unsigned lean0 = v->perf.lean_exits, irqs = v->monitor.stats.delivered[1];
    step(v, 13, 0);
    CHECK(v->perf.dac_batched == dac0 && v->perf.lean_exits == lean0 && v->monitor.stats.delivered[1] == irqs + 1);
    destroy(v, true); empty();
    puts("fx-27 DAC burst limits: no store, no VGA memory load, deliverable IRQ ends the burst and is delivered PASS");
}
/* Lean exits: eligibility and equivalence with the ordinary path. */
static void test_fx27_lean(void)
{
    for (unsigned pm = 0; pm < 2; pm++) {
        struct dosvm *v = fx27_vm(pm);
        port_out(v, 0x3C4, 2, 0x0604); port_out(v, 0x3D4, 2, 0x0014);
        port_out(v, 0x3D4, 2, 0xE317); port_out(v, 0x3C4, 2, 0x0102);   /* Mode Y */
        const uint8_t out[] = {0xEE, 0x90};
        CHECK(!dosvm_write(v, fx27_linear(pm), out, sizeof(out)));
        unsigned lean0 = v->perf.lean_exits, gp0 = v->perf.exits[0];
        for (unsigned i = 0; i < 64; i++) {
            cur.tf.eip = fx27_code(pm); cur.tf.edx = 0x3C5; cur.tf.eax = 1u << (i & 3);
            step(v, 13, 0);
            CHECK(!left && cur.tf.eip == fx27_code(pm) + 1 && v->vga.seq[2] == (1u << (i & 3)) &&
                  v->video_direct == (i & 3) + 1);
        }
        CHECK(v->perf.lean_exits == lean0 + 64 && v->perf.exits[0] == gp0 + 64);
        CHECK(v->perf.cost_count[DOSVM_COST_VGA_IO] >= 64);
        /* Ineligible states keep the ordinary path, with identical results. */
        /* Ineligible states keep the ordinary path, with identical results:
         * TF, an IPX VM, an idle poll, CLI-seed demand (PM), an STI or
         * MOV SS shadow, and the saved return point of an open hardware
         * interrupt record (a late native return, dpmi_irq_resync). */
        for (unsigned k = 0; k < 6; k++) {
            if ((k == 3 || k == 5) && !pm) continue;
            uint32_t eflags = cur.tf.eflags;
            if (k == 0) cur.tf.eflags |= V86_TF;
            if (k == 1) v->network_attached = true;
            if (k == 2) v->idle.depth = 1;
            if (k == 3) v->dpmi.cli_settled = false;
            if (k == 4) { if (pm) { v->dpmi.shadow = true; v->dpmi.shadow_cs = 1; } else v->monitor.shadow = true; }
            cur.tf.eip = fx27_code(pm); cur.tf.edx = 0x3C5; cur.tf.eax = 2;
            if (k == 5) {
                struct dpmi_record *r = &v->dpmi.rec[0];
                *r = (struct dpmi_record){0};
                r->flags = DPMI_RF_HWINT; r->isr_measured = true; r->client = v->dpmi.clients;
                r->saved_pm = true; r->saved = cur; v->dpmi.depth = 1;
                CHECK(!dpmi_lean_eligible(v, &cur));
                cur.tf.eip++; CHECK(dpmi_lean_eligible(v, &cur)); cur.tf.eip--;   /* inside the handler: lean */
                v->dpmi.depth = 0;
                CHECK(dpmi_lean_eligible(v, &cur));
                continue;
            }
            lean0 = v->perf.lean_exits;
            step(v, 13, 0);
            CHECK(v->perf.lean_exits == lean0 && v->vga.seq[2] == 2 && cur.tf.eip == fx27_code(pm) + 1);
            cur.tf.eflags = eflags & ~V86_TF;
            v->network_attached = false; v->idle.depth = 0; v->dpmi.cli_settled = true;
            v->dpmi.shadow = false; v->monitor.shadow = false;
        }
        /* An armed flags image (fx-16: PUSHF; CLI ... POPF, the image
         * carries TF) is lean: at an I/O exit with no deliverable IRQ the
         * ordinary path does nothing for it (no seed, no skipped-seed
         * count, I/O bursts refused), and the image keeps its owner until
         * its POPF reports the restore. Ordinary control first, then lean,
         * with virtual IF set (after STI) and clear (inside the CLI). */
        if (pm) {
            for (unsigned pass = 0; pass < 4; pass++) {
                v->dpmi.if_armed = true; v->monitor.vif = pass < 2;
                unsigned skipped = v->dpmi.cli.seeds_skipped, seeds = v->dpmi.cli.seeds;
                lean0 = v->perf.lean_exits;
                cur.tf.eip = fx27_code(pm); cur.tf.edx = 0x3C5; cur.tf.eax = 8;
                g_need_resched = !(pass & 1); step(v, 13, 0); g_need_resched = false;
                CHECK(!left && v->vga.seq[2] == 8 && cur.tf.eip == fx27_code(pm) + 1 && v->dpmi.if_armed &&
                      v->monitor.vif == (pass < 2) && !(cur.tf.eflags & V86_TF) &&
                      v->dpmi.cli.seeds_skipped == skipped && v->dpmi.cli.seeds == seeds &&
                      v->perf.lean_exits == lean0 + (pass & 1));
            }
            v->dpmi.if_armed = false; v->monitor.vif = true;
        }
        /* Quake's Sys_FloatTime (sys_dos.c): latch counter 0 at 43h, read
         * 40h twice. Lean, with the values the ordinary path returns. */
        static const uint8_t pit[] = {0xE6, 0x43, 0xE4, 0x40, 0xE4, 0x40, 0x90};
        CHECK(!dosvm_write(v, fx27_linear(pm), pit, sizeof(pit)));
        uint32_t al[2];
        for (unsigned pass = 0; pass < 2; pass++) {
            /* Pass 0: the scalar control (three ordinary exits); pass 1: one
             * lean exit whose I/O burst retires both reads. */
            lean0 = v->perf.lean_exits;
            unsigned exits = 0;
            cur.tf.eip = fx27_code(pm); cur.tf.eax = 0;
            while (cur.tf.eip != fx27_code(pm) + 6) {
                CHECK(exits++ < 3);
                g_need_resched = !pass; step(v, 13, 0); g_need_resched = false;
            }
            al[pass] = cur.tf.eax & 255;
            CHECK(exits == (pass ? 1u : 3u) && v->perf.lean_exits == lean0 + pass);
        }
        CHECK(al[0] == al[1]);
        /* A DMA register write is pinned at the ordinary settle point. */
        static const uint8_t dma[] = {0xE6, 0x0A, 0x90};
        CHECK(!dosvm_write(v, fx27_linear(pm), dma, sizeof(dma)));
        lean0 = v->perf.lean_exits;
        cur.tf.eip = fx27_code(pm); cur.tf.eax = 0x05;   /* mask channel 1 */
        step(v, 13, 0);
        CHECK(!v->memory.pool.dma_dirty && v->perf.lean_exits == lean0 + (v->memory.pool.pages ? 0u : 1u));
        /* A pending deliverable IRQ: emulated, then the ordinary tail delivers. */
        CHECK(!dosvm_write(v, fx27_linear(pm), out, sizeof(out)));
        v->monitor.pic[0].imr = 0xFD; v->monitor.pic[0].irr |= 2;
        unsigned irqs = v->monitor.stats.delivered[1];
        lean0 = v->perf.lean_exits;
        cur.tf.eip = fx27_code(pm); cur.tf.edx = 0x3C5; cur.tf.eax = 4;
        step(v, 13, 0);
        CHECK(v->vga.seq[2] == 4 && v->monitor.stats.delivered[1] == irqs + 1 && v->perf.lean_exits == lean0);
        destroy(v, true); empty();
    }
    puts("fx-27 lean exits: Mode Y map-mask OUTs and Quake's PIT latch/reads retire without the per-exit housekeeping (V86 and PM32, same values); an armed flags image is lean with the ordinary bookkeeping; TF, IPX, idle poll, seed demand, shadows, an interrupted context's return point, DMA pinning and pending IRQs keep the ordinary path PASS");
}
/* Chain-4: the presenter converts the open window's linear backing; the
 * planes are deinterleaved only for a model consumer. */
static void test_fx27_linear_presenter(void)
{
    struct dosvm *v = create(); int10(v, 0x13, 0, 0, 0);
    CHECK(v->video_direct == VIDEO_LINEAR);
    struct ciuki_rect d;
    CHECK(dosvm_vga_present(v, capture_row, frame_buffer, 0, &d) == 1);
    for (unsigned y = 0; y < 200; y++) for (unsigned x = 0; x < 320; x++)
        linear_store(v, y * 320 + x, (uint8_t)(x * 3 + y));
    uint64_t tick = 40;
    unsigned harvests = v->video_linear_harvests, pages = v->perf.direct_pages, rows = v->perf.rows;
    CHECK(dosvm_vga_present(v, capture_row, frame_buffer, tick, &d) == 1 && d.height == 400);
    /* Pre-fx-27: 16 pages (64 KiB) deinterleaved into the planes per frame. */
    CHECK(v->video_linear_harvests == harvests && v->perf.direct_pages == pages);
    CHECK(v->perf.rows == rows + 400 && v->video_linear_stale == 0xFFFF && v->perf.linear_direct >= 200);
    speed_compare_scanout(v);                        /* flushes, then compares with the planes */
    CHECK(v->video_linear_harvests == harvests + 16 && !v->video_linear_stale);
    /* One changed byte: one source row converted, two destination rows. */
    rows = v->perf.rows; unsigned converted = v->perf.converted_bytes;
    linear_store(v, 117 * 320 + 9, 0xEE); tick += 40;
    CHECK(dosvm_vga_present(v, capture_row, frame_buffer, tick, &d) == 1 && d.dst_y == 40 + 117 * 2 && d.height == 2);
    CHECK(v->perf.rows == rows + 2 && v->perf.converted_bytes == converted + 320);
    /* Same bytes rewritten: a D page, but no changed row. */
    linear_store(v, 117 * 320 + 9, 0xEE); tick += 40;
    CHECK(!dosvm_vga_present(v, capture_row, frame_buffer, tick, &d) && v->perf.rows == rows + 2);
    /* Pan and start address while mapped: rows from the backing, exact. */
    port_out(v, 0x3D4, 2, 0x100C); port_out(v, 0x3D4, 2, 0x000D);   /* start 1000h words */
    (void)port_in(v, 0x3DA, 1); port_out(v, 0x3C0, 1, 0x33); port_out(v, 0x3C0, 1, 2);
    tick += 40;
    CHECK(dosvm_vga_present(v, capture_row, frame_buffer, tick, &d) == 1 && v->video_direct == VIDEO_LINEAR);
    speed_compare_scanout(v);
    /* Closing the window (model consumer): planes current, planar source. */
    linear_store(v, 5, 0x77);
    dosvm_vga_model(v);
    CHECK(v->video_direct != VIDEO_LINEAR && v->vga.plane[1][4] == 0x77 && !v->video_linear_stale);
    tick += 40;
    CHECK(dosvm_vga_present(v, capture_row, frame_buffer, tick, &d) == 1 && !v->video_index_linear);
    speed_compare_scanout(v);
    destroy(v, true); empty();
    puts("fx-27 chain-4 linear presenter: no per-frame deinterleave, exact rows from the backing (start/pan), single-row damage, lazy planes for model consumers PASS");
}
/* VBE: only rows in written pages are converted; palette redraws all. */
static void test_fx27_vbe(void)
{
    struct dosvm *v; CHECK(!dosvm_create(0, &v));
    bool available = fs_vbe_available; fs_vbe_available = true;
    for (unsigned m = 0; m < 2; m++) {
        int10(v, 0x4F02, m ? 0x4114 : 0x4101, 0, 0);
        const struct dos_video_mode *mode = dos_video_mode(v->vbe_mode);
        CHECK(mode && v->vbe_buffer.used);
        struct ciuki_rect d;
        CHECK(dosvm_vga_present(v, capture_row, frame_buffer, 0, &d) == 1 && d.height == 480);
        unsigned rows = v->perf.rows, converted = v->perf.converted_bytes;
        uint32_t at = 300 * mode->pitch + 8;
        uint8_t byte = 0x5A;
        CHECK(!dosvm_write(v, v->vbe_buffer.base + at, &byte, 1));
        *vbe_pte(v, at / PAGE_SIZE) |= 0x40;            /* the CPU's D bit for that store */
        CHECK(dosvm_vga_present(v, capture_row, frame_buffer, 40, &d) == 1);
        unsigned page_rows = PAGE_SIZE / mode->pitch + 2;
        /* Pre-fx-27: 480 rows converted for any dirty page. */
        CHECK(v->perf.rows - rows <= page_rows * 480 / mode->height + 1 &&
              v->perf.converted_bytes - converted <= (page_rows + 1) * mode->width * (mode->bpp / 8));
        /* The changed source row is presented, exactly as a full redraw. */
        memcpy(reference_buffer, frame_buffer, sizeof(frame_buffer));
        v->video_presented = false;
        CHECK(dosvm_vga_present(v, capture_row, frame_buffer, 80, &d) == 1 && d.height == 480);
        CHECK(!memcmp(reference_buffer, frame_buffer, sizeof(frame_buffer)));
        if (!m) {
            rows = v->perf.rows;
            port_out(v, 0x3C8, 1, 0); port_out(v, 0x3C9, 1, 63); port_out(v, 0x3C9, 1, 0); port_out(v, 0x3C9, 1, 0);
            CHECK(dosvm_vga_present(v, capture_row, frame_buffer, 120, &d) == 1 && v->perf.rows == rows + 480);
        }
    }
    fs_vbe_available = available; destroy(v, true); empty();
    puts("fx-27 VBE: 640x480x8 and 800x600x16 convert only rows in dirty pages, identical to a full redraw; a palette change redraws every row PASS");
}
/* The surface writer reads nothing back for a row proven changed. */
static void test_fx27_surface(void)
{
    struct dosvm *v = create(); int10(v, 0x13, 0, 0, 0);
    static uint8_t pixels[CIUKI_DOSVM_WIDTH * CIUKI_DOSVM_HEIGHT * 4];
    struct surface s = {.info = {.allocation_bytes = sizeof(pixels), .width = CIUKI_DOSVM_WIDTH,
        .height = CIUKI_DOSVM_HEIGHT, .stride = CIUKI_DOSVM_WIDTH * 4}, .pixels = pixels, .references = 2};
    struct row_target target = {&s, true, v};
    struct ciuki_rect d;
    CHECK(dosvm_vga_present(v, put_row, &target, 0, &d) == 1);
    target.force = false;
    for (unsigned x = 0; x < 320; x++) linear_store(v, 50 * 320 + x, (uint8_t)(x + 1));
    unsigned reads = surface_reads, writes = surface_rows;
    CHECK(dosvm_vga_present(v, put_row, &target, 40, &d) == 1 && d.height == 2);
    /* Pre-fx-27: each changed row first read the surface back (256-byte chunks). */
    CHECK(surface_reads == reads && surface_rows == writes + 2);
    /* A palette change forces every row; unchanged colours are still compared. */
    port_out(v, 0x3C8, 1, 200); port_out(v, 0x3C9, 1, 1); port_out(v, 0x3C9, 1, 2); port_out(v, 0x3C9, 1, 3);
    reads = surface_reads;
    CHECK(dosvm_vga_present(v, put_row, &target, 80, &d) <= 1 && surface_reads > reads);
    destroy(v, true); empty();
    puts("fx-27 surface writer: index-proven rows written without read-back, forced rows still compared PASS");
}
#endif
static void test_fx27(void)
{
    test_fx27_palette(); test_fx27_frame_budgets();
#ifdef FX27_HEAD_BASELINE
    printf("fx-27 baseline: %u budget(s) failed\n", fx27_budget_failures);
#else
    test_fx27_dac_limits(); test_fx27_lean();
    test_fx27_linear_presenter(); test_fx27_vbe(); test_fx27_surface();
#endif
}
