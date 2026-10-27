/* fx-doomwin (docs/directives/fx-doomwin.md, T23 trial 6c3b7800, DOOM in a
 * window): the fx-16 CLI seed demand restarts only for a CLI left without a
 * cached site, and a plane select (sequencer/graphics-controller OUT) retires
 * at trap entry (dosvm_trap_fast) with exactly the state fx-27's lean exit
 * leaves. Production monitor, fake trap transport. Included by dosvm_test.c.
 * SPDX-License-Identifier: GPL-2.0-only */

/* trap_dispatch's order for a VM #GP(0): dosvm_trap_fast, then dosvm_trap. */
static bool step_entry(struct dosvm *v)
{
    struct v86_frame hw = cur;
    hw.tf.vector = 13; hw.tf.err = 0;
    g_current = v->task; v->continuation = 0x12345678; cpu_flags = 0;
    left = false;
    bool fast = dosvm_trap_fast(&hw.tf);
    CHECK(!fast || !v->fast_retired);
    if (fast) cur = hw;
    else if (!setjmp(guest_return)) { CHECK(dosvm_trap(&hw.tf)); cur = hw; }
    else { left = true; cur = v->frame; }
    CHECK(!v->fast_retired);
    g_current = &caller; sti();
    cur.tf.vector = cur.tf.err = 0;
    return fast;
}

/* DOOM's learned CLI sites: one forward proof, then no tracker demand. Before
 * fx-doomwin every successful proof restarted DPMI_SEED_IDLE_MS of seeds
 * (12,282 of 17,940 #DB exits in the host DOOM replay's 30 s); a CLI that
 * stays without a site still restarts it (fx-16's rule). */
static void test_doomwin_seed_demand(void)
{
    if (g_ticks < 5000) g_ticks = 5000;     /* past the first DPMI_SEED_IDLE_MS */
    struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, true);
    uint32_t base = seg_base(v, (uint16_t)cur.tf.cs);
    struct dpmi *d = &v->dpmi;
    const uint8_t save[] = {0x9C, 0x58, 0xFA};                  /* PUSHF; POP AX; CLI (16-bit CS) */
    CHECK(!dosvm_write(v, base + 0x300, save, sizeof(save)));
    CHECK(dpmi_seed_wanted(v));                                 /* unsettled: seeds wanted */
    cur.tf.eip = 0x300; step(v, 0x20, 0);                       /* an observed boundary */
    v->monitor.vif = true; d->if_armed = false;
    cur.tf.eip = 0x302; cur.tf.eax = cur.tf.eflags = V86_IF | 2;
    unsigned proved = d->cli.proved;
    step(v, 13, 0);
    CHECK(!left && d->if_armed && (cur.tf.eax & V86_TF) && d->cli.proved == proved + 1);
    CHECK(!dpmi_seed_wanted(v));                                /* cached: no second of seeds */
    /* An IRQ boundary now acquires no seed, no stack snapshot, no tracker. */
    uint32_t seeds = d->cli.seeds, armed = v->perf.return_armed;
    v->monitor.vif = true; d->if_armed = false;
    cur.tf.eip = 0x500; step(v, 0x20, 0);
    CHECK(d->cli.seeds == seeds && !d->return_steps && v->perf.return_armed == armed && !d->decode_stack_valid);
    /* A rewritten site whose nearby boundary disproves it restarts the demand. */
    const uint8_t other[] = {0xBB, 0x9C, 0x58, 0xFA};            /* MOV BX,589Ch; CLI */
    CHECK(!dosvm_write(v, base + 0x2FF, other, sizeof(other)));
    v->monitor.vif = true; d->if_armed = false; review_boundary(v, 0x2FF);
    unsigned missed = d->cli.missed;
    cur.tf.eip = 0x302; cur.tf.eax = cur.tf.eflags = V86_IF | 2; step(v, 13, 0);
    CHECK(!d->if_armed && d->cli.missed == missed + 1 && dpmi_seed_wanted(v));
    destroy(v, true); empty();
    puts("fx-doomwin seed demand: a proven, cached CLI wants no seeds; IRQ boundaries then arm no tracker; a missed CLI restarts the demand PASS");
}

/* The DOOM Mode Y plane select and its neighbours, once through trap entry
 * and once through dosvm_trap (fx-27's lean path): same registers, window,
 * PTEs, pending dirty pages, DPMI boundary and counts; refused before any
 * change where the lean path would not retire the exit; an obligation after
 * it hands the retired OUT to the ordinary tail (emulated once). */
struct doomwin_state {
    uint8_t seq[5], gc[9], seq_index, gc_index;
    unsigned direct; bool read_window, writable;
    uint16_t pending[4];
    uint32_t window[16], eip, port_count[DOSVM_VGA_REGS], planes;
    bool decode_valid; uint16_t decode_cs; uint32_t decode_eip;
};
static void doomwin_capture(struct dosvm *v, struct doomwin_state *s)
{
    memset(s, 0, sizeof(*s));
    memcpy(s->seq, v->vga.seq, sizeof(s->seq)); memcpy(s->gc, v->vga.gc, sizeof(s->gc));
    s->seq_index = v->vga.seq_index; s->gc_index = v->vga.gc_index;
    s->direct = v->video_direct; s->read_window = v->video_read_window; s->writable = v->video_writable;
    memcpy(s->pending, v->video_pending, sizeof(s->pending));
    for (unsigned i = 0; i < 16; i++) {
        /* Which memory each window page maps (the two VMs differ in host
         * addresses), with its U/W/P bits. */
        uint32_t pte = *window_pte(v, 0xA0000 + i * PAGE_SIZE), frame = pte & ~0xFFFu, kind = 0xFF;
        for (unsigned plane = 0; plane < 4; plane++)
            if (frame == (plane_frame(v, plane, i) & ~0xFFFu)) kind = plane + 1;
        if (frame == v->frames[0xA0 + i]) kind = 0x10;
        s->window[i] = kind << 8 | (pte & (PTE_U | PTE_W | PTE_P));
    }
    for (unsigned plane = 0; plane < 4; plane++)
        for (unsigned i = 0; i < CVGA_PLANE_SIZE; i++) s->planes = s->planes * 31u + v->vga.plane[plane][i];
    s->eip = cur.tf.eip;
    memcpy(s->port_count, v->perf.port_count, sizeof(s->port_count));
    s->decode_valid = v->dpmi.decode_valid; s->decode_cs = v->dpmi.decode_cs; s->decode_eip = v->dpmi.decode_eip;
}
static void doomwin_replay(bool pm, bool entry, struct doomwin_state *out, unsigned *fast)
{
    struct dosvm *v = fx27_vm(pm);
    port_out(v, 0x3C4, 2, 0x0604); port_out(v, 0x3D4, 2, 0x0014);
    port_out(v, 0x3D4, 2, 0xE317); port_out(v, 0x3C4, 2, 0x0102);   /* Mode Y */
    /* OUT DX,AL; OUT DX,AX (16-bit code: EF; 32-bit code: 66 EF). */
    const uint8_t code[] = {0xEE, 0x90, 0x66, 0xEF, 0x90, 0xEF, 0x90};
    CHECK(!dosvm_write(v, fx27_linear(pm), code, sizeof(code)));
    uint32_t at_al = fx27_code(pm), at_ax = fx27_code(pm) + (pm ? 2 : 5);
    unsigned len_ax = pm ? 2 : 1;
    *fast = 0;
    for (unsigned i = 0; i < 96; i++) {
        uint32_t pages = 0;
        if (i % 8 == 3) {                     /* OUT DX,AX: index 2 and a mask */
            cur.tf.eip = at_ax; cur.tf.edx = 0x3C4; cur.tf.eax = 0x0002u | (1u << (8 + (i & 3)));
        } else if (i % 8 == 5) {              /* GR04 read map select (Wolf3D's VGAREADMAP) */
            cur.tf.eip = at_ax; cur.tf.edx = 0x3CE; cur.tf.eax = 0x0004u | ((i & 3) << 8);
        } else if (i % 8 == 6) {              /* back to the sequencer index */
            cur.tf.eip = at_al; cur.tf.edx = 0x3C4; cur.tf.eax = 2;
        } else {
            cur.tf.eip = at_al; cur.tf.edx = 0x3C5; cur.tf.eax = 1u << (i & 3);
        }
        uint32_t next = cur.tf.eip + (cur.tf.eip == at_ax ? len_ax : 1);
        if (entry) *fast += step_entry(v); else step(v, 13, 0);
        CHECK(!left && cur.tf.eip == next);
        /* A guest store through the open plane window: the byte lands in
         * the mapped plane and the CPU sets A and D in its PTE. */
        if (v->video_direct && v->video_direct <= 4 && v->video_writable) {
            uint32_t offset = (i % 16) * PAGE_SIZE + i;
            CHECK((*window_pte(v, 0xA0000 + offset) & ~0xFFFu) ==
                  (plane_frame(v, v->video_direct - 1, offset / PAGE_SIZE) & ~0xFFFu));
            v->vga.plane[v->video_direct - 1][offset] = (uint8_t)(i * 7 + 1);
            *window_pte(v, 0xA0000 + offset) |= DOSVM_PTE_DIRTY | 0x20u;
            pages++;
        }
        CHECK(pages || !v->video_direct || !v->video_writable);
    }
    doomwin_capture(v, out);
    /* Counts: every select an exit of the lean kind. */
    CHECK(v->perf.cost_count[DOSVM_COST_VGA_IO] >= 96);
    destroy(v, true); empty();
}
/* V86: the OUT is followed by MOV SS,AX. fx-27's lean return arms the
 * stack-switch shadow (v86_prepare_return); the trap-entry return must too. */
static void doomwin_v86_shadow(bool entry, bool *shadow, uint32_t *eflags, uint16_t *ip)
{
    struct dosvm *v = fx27_vm(false);
    port_out(v, 0x3C4, 2, 0x0604); port_out(v, 0x3C4, 2, 0x0102);
    const uint8_t code[] = {0xEE, 0x8E, 0xD0, 0x90};
    CHECK(!dosvm_write(v, fx27_linear(false) + 0x10, code, sizeof(code)));
    cur.tf.eip = fx27_code(false) + 0x10; cur.tf.edx = 0x3C5; cur.tf.eax = 4;
    CHECK(v->monitor.track_boundaries && !v->monitor.shadow);
    if (entry) CHECK(step_entry(v)); else step(v, 13, 0);
    CHECK(!left && cur.tf.eip == fx27_code(false) + 0x11 && v->vga.seq[2] == 4);
    *shadow = v->monitor.shadow; *eflags = cur.tf.eflags; *ip = v->monitor.shadow_ip;
    destroy(v, true); empty();
}
static void test_doomwin_fast_select(void)
{
    bool shadow[2]; uint32_t eflags[2]; uint16_t ip[2];
    doomwin_v86_shadow(false, &shadow[0], &eflags[0], &ip[0]);
    doomwin_v86_shadow(true, &shadow[1], &eflags[1], &ip[1]);
    CHECK(shadow[0] && shadow[1] && ip[0] == ip[1] && eflags[0] == eflags[1]);
    for (unsigned pm = 0; pm < 2; pm++) {
        struct doomwin_state lean, fast;
        unsigned none = 0, taken = 0;
        doomwin_replay(pm, false, &lean, &none);
        doomwin_replay(pm, true, &fast, &taken);
        CHECK(!none && taken == 96);
        CHECK(!memcmp(&lean, &fast, sizeof(lean)));
        /* Refusals, nothing changed, and the lean/ordinary path decides. */
        struct dosvm *v = fx27_vm(pm);
        const uint8_t code[] = {0xEE, 0x90, 0x66, 0xEF, 0x90, 0xEF, 0x90};
        CHECK(!dosvm_write(v, fx27_linear(pm), code, sizeof(code)));
        for (unsigned k = 0; k < 8; k++) {
            /* 0: chain-4 (mode 13h); 1: SR04 data; 2: OUT DX,EAX; 3: TF;
             * 4: STI/MOV SS shadow; 5: IPX VM (no lean exit); 6: a launch
             * before its video; 7: OUT DX,AX at 3C5 (DAC mask next). */
            if (k) { port_out(v, 0x3C4, 2, 0x0604); port_out(v, 0x3C4, 2, 0x0102); }
            uint32_t eflags = cur.tf.eflags;
            cur.tf.eip = fx27_code(pm); cur.tf.edx = 0x3C5; cur.tf.eax = 4;
            if (k == 1) port_out(v, 0x3C4, 1, 4);                 /* index 4: memory mode */
            if (k == 2) { cur.tf.eip = fx27_code(pm) + (pm ? 5 : 2); cur.tf.edx = 0x3C4; cur.tf.eax = 0x0402; }
            if (k == 7) { cur.tf.eip = fx27_code(pm) + (pm ? 2 : 5); cur.tf.eax = 0x0204; }
            if (k == 3) cur.tf.eflags |= V86_TF;
            if (k == 4) {
                if (pm) { v->dpmi.shadow = true; v->dpmi.shadow_cs = 1; }
                else { v->monitor.shadow = true; v->monitor.shadow_cs = (uint16_t)(cur.tf.cs - 1);
                       v->monitor.shadow_ip = v->monitor.shadow_next = 0x10; }
            }
            if (k == 5) v->network_attached = true;
            if (k == 6) { v->launch_exec = true; v->launch_ready = false; }
            struct v86_frame hw = cur; hw.tf.vector = 13; hw.tf.err = 0;
            g_current = v->task; v->continuation = 0x12345678; cpu_flags = 0;
            struct doomwin_state before, after;
            doomwin_capture(v, &before);
            unsigned fast0 = v->perf.fast_exits, gp0 = v->perf.exits[0];
            CHECK(!dosvm_trap_fast(&hw.tf) && !v->fast_retired);
            CHECK(hw.tf.eip == cur.tf.eip && hw.tf.eax == cur.tf.eax && hw.tf.eflags == cur.tf.eflags);
            doomwin_capture(v, &after);
            CHECK(!memcmp(&before, &after, sizeof(before)) && v->perf.fast_exits == fast0 && v->perf.exits[0] == gp0);
            g_current = &caller; sti();
            cur.tf.eflags = eflags & ~V86_TF;
            v->dpmi.shadow = false; v->monitor.shadow = false; v->network_attached = false;
            v->launch_exec = false;
        }
        /* A deliverable IRQ after the select: retired once at entry, then the
         * ordinary tail delivers it (lean_io's result 2). */
        port_out(v, 0x3C4, 2, 0x0604); port_out(v, 0x3C4, 2, 0x0102);
        v->monitor.pic[0].imr = 0xFD; v->monitor.pic[0].irr |= 2;
        unsigned irqs = v->monitor.stats.delivered[1], lean0 = v->perf.lean_exits;
        unsigned masks = v->perf.port_count[DOSVM_VGA_SR_MASK], fast0 = v->perf.fast_exits;
        cur.tf.eip = fx27_code(pm); cur.tf.edx = 0x3C5; cur.tf.eax = 8;
        CHECK(!step_entry(v));
        CHECK(v->vga.seq[2] == 8 && v->monitor.stats.delivered[1] == irqs + 1 && v->perf.lean_exits == lean0 &&
              v->perf.port_count[DOSVM_VGA_SR_MASK] == masks + 1 && v->perf.fast_exits == fast0);
        destroy(v, true); empty();
    }
    puts("fx-doomwin trap-entry plane selects: 96 Mode Y/GR04 selects (V86 and PM32) leave the lean path's registers, window PTEs, dirty pages and DPMI boundary; chain-4, other indexes, OUT DX,EAX, TF, shadows, IPX and an unready launch refused unchanged; a pending IRQ gets the ordinary tail after one emulation; a V86 MOV SS after the OUT gets its shadow PASS");
}
static void test_doomwin(void)
{
    test_doomwin_seed_demand();
    test_doomwin_fast_select();
}
