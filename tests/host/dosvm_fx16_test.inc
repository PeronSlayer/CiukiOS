/* fx-16: replays built from the T23 trial of preview e831e685 (private
 * evidence legacy/local/physical/e831e685: serial.log cadence records and
 * BOOT.LOG vm_perf banks), through the production trap, DPMI, PIC, PIT,
 * SB16 and clock paths. Each case fails on the code before fx-16 (source
 * e5d40038); FX16_HEAD_BASELINE compiles the behavioural checks alone
 * against that source for the report's before/after table.
 * Included by dosvm_test.c after dosvm_audio_test.inc (ms, dsp, dma8). */

/* DOOM/DMX timer service, as the host DOOM replay executed it at
 * 01df:00244264: `push ebx; push edx; pushfd; pop eax; cli`, entered from
 * DMX's dispatcher after its EOI, STI and an `lss esp`, so no forward
 * decode can reach it. Before fx-16 the CLI was never armed (1134 misses
 * in the host replay), DMX's POPFD left virtual IF clear for the rest of
 * the timer handler, where IF recovery is excluded, and the SB16 IRQ that
 * DMX re-enabled waited for the handler's IRET (T23: if_clear 380-605 ms per
 * 5 s window, SB16 IRQ latency up to 69-178 ms). Windows 95 armed it by the
 * code before the CLI (R. Chen, 2016-04-11). */
static void test_fx16_dmx_service_cli(void)
{
    for (unsigned bits = 0; bits < 2; bits++) {
        struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, bits != 0);
        uint16_t code = (uint16_t)cur.tf.cs;
        uint32_t base = seg_base(v, code);
        v->dpmi.vec[8] = (struct dpmi_vec){.sel=code, .off=0x700};
        v->dpmi.vec[15] = (struct dpmi_vec){.sel=code, .off=0x720};
        device_out(v, 0x21, 0x7E);                         /* IRQ0 and IRQ7 */
        const uint8_t service[] = {0x53, 0x52, 0x9C, 0x58, 0xFA, 0x50, 0x9D, 0x90};
        CHECK(!dosvm_write(v, base + 0x400, service, sizeof(service)));
        CHECK(dosvm_irq_raise(v->info.key, 0)); step(v, 0x20, 0);
        CHECK(!left && cur.tf.eip == 0x700 && v->dpmi.depth == 1 && !v->monitor.vif);
        /* Dispatcher: EOI first, then STI so the SB16 IRQ may nest. */
        cur.tf.eax = 0x20; const uint8_t eoi[] = {0xE6, 0x20}; exec(v, eoi, sizeof(eoi), 13, 0);
        op1(v, 0xFB);
        CHECK(v->monitor.vif && v->dpmi.decode_eip > 0x404);  /* the only boundary lies after the service */
        /* The service's PUSHFD; POP EAX; CLI. */
        cur.tf.eip = 0x404;
        cur.tf.eax = (cur.tf.eflags & 0x0CD5u) | V86_IF | 2u;
        uint32_t image = cur.tf.eax;
        step(v, 13, 0);
        CHECK(!left && !v->monitor.vif && cur.tf.eip == 0x405);
        CHECK(v->dpmi.if_armed && cur.tf.eax == (image | V86_TF));
        /* An SB16 block boundary while the service runs waits for its POPFD. */
        CHECK(dosvm_irq_raise(v->info.key, 7)); step(v, 0x20, 0);
        CHECK(!left && cur.tf.eip == 0x405 && !v->monitor.stats.delivered[7]);
        /* PUSH EAX; POPFD loads TF: BS one instruction later restores IF and
         * the SB16 IRQ nests inside the timer handler, as on the card. */
        cur.tf.eip = 0x407; debug_dr6 = 0xFFFF0FF0u | (1u << 14); step(v, 1, 0);
        CHECK(!left && cur.tf.eip == 0x720 && v->dpmi.depth == 2 && v->monitor.stats.delivered[7] == 1);
        destroy(v, true); empty();
    }
    puts("fx-16 DMX timer service: PUSHFD/POP EAX/CLI behind a stack switch armed, POPFD restores IF, SB16 IRQ nests in the timer handler PASS");
}

/* DMX programs the PIT for 140 Hz (count 8523; the host replay delivers 700
 * IRQ0 per 5 s). Trial records: cadence_clock gap_max_ms 8-9 (a late clock
 * service: two edges in one advance) and stretch_max_ms 7-8 for the VM
 * and desktop (IRQ0 requested but not taken for longer than a period).
 * An 8259A keeps one request per line: each such edge was lost, and with it
 * a DMX tick (DOOM's 35 Hz tic is every fourth). Linux KVM's i8254 keeps
 * such ticks pending and injects them after the ack (reinject mode). */
static void test_fx16_pit_reinject(void)
{
    struct dosvm *v = audio_vm_create();
    device_out(v, 0x21, 0x7E);
    device_out(v, 0x43, 0x34); device_out(v, 0x40, 8523 & 255); device_out(v, 0x40, 8523 >> 8);
    v->monitor.pic[0].irr = 0;
    uint8_t vector0 = v->monitor.pic[0].base;
    uint64_t edges0 = v->pit.edges;
    unsigned taken = 0, late = 0;
    for (unsigned t = 0; t < 5000;) {
        /* 97 and 89 ms are co-prime with the 50/7 ms period: every phase occurs. */
        if (t % 97 == 40) { g_ticks += 9; host_clock_tick(); t += 9; late++; }   /* gap_max_ms=9 */
        else { ms(1); t++; }
        bool away = t % 89 >= 60 && t % 89 < 68;                                /* 8 ms stretch */
        if (!away && (v->monitor.pic[0].irr & 1)) {
            CHECK(dosvm_pic_ack(&v->monitor) == vector0); device_out(v, 0x20, 0x20); taken++;
        }
    }
    for (unsigned t = 0; t < 32; t++) {                     /* drain what is still owed */
        ms(1);
        if (v->monitor.pic[0].irr & 1) { CHECK(dosvm_pic_ack(&v->monitor) == vector0); device_out(v, 0x20, 0x20); taken++; }
    }
    uint64_t edges = v->pit.edges - edges0;
    printf("fx-16 PIT 140 Hz: edges=%llu taken=%u late_services=%u", (unsigned long long)edges, taken, late);
#ifndef FX16_HEAD_BASELINE
    printf(" reinjected=%llu lost=%llu owed_max=%u", (unsigned long long)v->pit.reinjected, (unsigned long long)v->pit.lost_edges, v->pit.owed_max);
#endif
    printf("\n");
    CHECK(edges >= 700 && late >= 50);
    CHECK(taken == edges);                                  /* every DMX tick reaches the guest */
#ifndef FX16_HEAD_BASELINE
    CHECK(v->pit.reinjected >= 20 && !v->pit.lost_edges && !v->pit.owed && v->pit.owed_max <= 2);
    /* A reprogrammed timer owes nothing; a masked line latches one request. */
    v->pit.owed = 3; device_out(v, 0x43, 0x34); CHECK(!v->pit.owed);
    device_out(v, 0x40, 8523 & 255); device_out(v, 0x40, 8523 >> 8);
    device_out(v, 0x21, 0x7F); v->monitor.pic[0].irr = 0;
    g_ticks += 20; host_clock_tick();
    CHECK(!v->pit.owed && (v->monitor.pic[0].irr & 1));
#endif
    destroy(v, true); empty();
    puts("fx-16 PIT reinjection: 9 ms late services and 8 ms stretches at 140 Hz, every edge delivered once, owed ticks bounded PASS");
}

/* Full-screen switch (trial W2: parked=808 ms, merged=102, underruns=94;
 * vm-park tick 128852 to first-guest-frame tick 129777). DOOM's DMX plays
 * 8-bit mono 11025 Hz auto-init on DMA 1 in 128-byte blocks (86 IRQs/s,
 * blocks=430 per 5 s window). The guest services every IRQ within 1 ms
 * except while parked. Before fx-16 the parked VM's DSP and PIT kept
 * running: the unrefilled buffer replayed (a buzz) and every boundary of
 * the park merged into one latched IRQ. */
static void test_fx16_parked_switch(void)
{
    struct dosvm *v = audio_vm_create(); dsp_reset(v);
    dma8(v, 1, 0x30000, 255, true);
    dsp(v, 0x41); dsp(v, 0x2B); dsp(v, 0x11);              /* 11025 Hz */
    dsp(v, 0x48); dsp(v, 127); dsp(v, 0);                  /* 128-byte blocks */
    dsp(v, 0x1C);                                          /* 8-bit auto-init */
    v->monitor.vif = true;
    uint8_t vector7 = (uint8_t)(v->monitor.pic[0].base + 7);
    ms(20);
    uint32_t boundaries = v->audio.sb.boundaries, parked_blocks = 0;
    uint32_t merged0 = v->audio.sb.irq_merged, late0 = v->audio.cadence_late_acks;
    v->audio.sb.max_service = 0;          /* lifetime counters: the 5 s bank rolls over */
    uint64_t pos_parked = 0;
    for (unsigned t = 0; t < 5000; t++) {
        bool parked = t >= 1000 && t < 1808;
        v->fullscreen_parked = parked;
        if (t == 1000) pos_parked = v->audio.sb.pos;
        uint32_t before = v->audio.sb.boundaries;
        ms(1);
        if (parked) parked_blocks += v->audio.sb.boundaries - before;
        if (!parked && (v->monitor.pic[0].irr & 0x80)) {
            CHECK(dosvm_pic_ack(&v->monitor) == vector7);
            (void)a_in(v, SB + 0xE); device_out(v, 0x20, 0x20);
        }
    }
    v->fullscreen_parked = false;
    uint32_t blocks = v->audio.sb.boundaries - boundaries;
    uint32_t merged = v->audio.sb.irq_merged - merged0, late = v->audio.cadence_late_acks - late0;
    unsigned ack_ms = v->audio.sb.max_service / DOSVM_AUDIO_PER_MS;
    printf("fx-16 park 808 ms: blocks=%u parked_blocks=%u merged=%u late_acks=%u ack_max_ms=%u\n",
           blocks, parked_blocks, merged, late, ack_ms);
    CHECK(!parked_blocks && !merged && !late && ack_ms <= 11);  /* within one 11.6 ms block */
    CHECK(blocks >= 360 && blocks <= 362);                 /* 4192 ms of DSP time */
#ifndef FX16_HEAD_BASELINE
    CHECK(v->parked_ms == 808 && v->audio.sb.pos > pos_parked);
#endif
    (void)pos_parked;
    dsp_reset(v); destroy(v, true); empty();
    puts("fx-16 parked switch: an 808 ms park stops the VM's DSP and PIT, no stale block replayed, no merged IRQ PASS");
}

#ifndef FX16_HEAD_BASELINE
/* The proof cache and the seed demand: one hot flags-saving CLI site (DMX
 * executes its few sites thousands of times per second) is proven once;
 * after DPMI_SEED_IDLE_MS without a CLI that needed a proof no return
 * tracker breakpoint, IRQ-boundary proof or stack snapshot is acquired.
 * A rewritten site is proven again. */
static void test_fx16_cli_site_cache(void)
{
    struct dosvm *v = dpmi_vm(true); v86_start(); enter(v, true);
    uint32_t base = seg_base(v, (uint16_t)cur.tf.cs);
    const uint8_t save[] = {0x9C, 0x58, 0xFA};
    CHECK(!dosvm_write(v, base + 0x300, save, sizeof(save)));
    struct dpmi *d = &v->dpmi;
    for (unsigned i = 0; i < 1000; i++) {
        if (!i) { cur.tf.eip = 0x300; step(v, 0x20, 0); }   /* one observed boundary */
        v->monitor.vif = true; d->if_armed = false;
        cur.tf.eip = 0x302; cur.tf.eax = cur.tf.eflags = V86_IF | 2;
        step(v, 13, 0);
        CHECK(!left && d->if_armed && (cur.tf.eax & V86_TF));
        if (!(i % 100)) g_ticks += 300;
    }
    CHECK(d->cli.proved == 1 && d->cli.hits == 999 && !d->cli.missed && !d->cli.pattern);
    CHECK(!dpmi_seed_wanted(v));
    uint32_t seeds = d->cli.seeds, skipped = d->cli.seeds_skipped;
    v->monitor.vif = true; d->if_armed = false; cur.tf.eip = 0x500; step(v, 0x20, 0);
    CHECK(d->cli.seeds == seeds && d->cli.irq_proofs_skipped && !d->decode_stack_valid);
    CHECK(d->cli.seeds_skipped > skipped);
    /* Rewritten code is proven (or refused) again, and seeds resume. */
    const uint8_t other[] = {0xBB, 0x9C, 0x58, 0xFA};    /* MOV BX,589Ch; CLI */
    CHECK(!dosvm_write(v, base + 0x2FF, other, sizeof(other)));
    v->monitor.vif = true; d->if_armed = false; review_boundary(v, 0x2FF);
    cur.tf.eip = 0x302; cur.tf.eax = cur.tf.eflags = V86_IF | 2; step(v, 13, 0);
    CHECK(!d->if_armed && cur.tf.eax == (V86_IF | 2) && d->cli.missed == 1 && dpmi_seed_wanted(v));
    destroy(v, true); empty();
    puts("fx-16 CLI proof cache: one proof for 1000 executions, no seed while settled, rewritten site disproved and seeding resumed PASS");
}
#endif

/* CIUKI_VM_FX16=dmx|pit|park|cache selects one case (the report's
 * before/after runs); unset runs all of them. */
static void test_fx16(void)
{
    const char *only = getenv("CIUKI_VM_FX16");
    if (only && !*only) only = 0;
    if (!only || !strcmp(only, "dmx")) test_fx16_dmx_service_cli();
    if (!only || !strcmp(only, "pit")) test_fx16_pit_reinject();
    if (!only || !strcmp(only, "park")) test_fx16_parked_switch();
#ifndef FX16_HEAD_BASELINE
    if (!only || !strcmp(only, "cache")) test_fx16_cli_site_cache();
#endif
}
