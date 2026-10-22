/* f3-14 production trap replay. Scalar control requests host rescheduling
 * before each OUT, preventing coalescing at that boundary. Identical VGA
 * cycles and guest register results are compared with the unblocked run.
 * This is a startup instruction replay, not a DOOM executable or FPS test.
 * SPDX-License-Identifier: GPL-2.0-only */
static unsigned cost_startup(bool pm, bool batch, uint8_t palette[256][3])
{
    struct dosvm *v = pm ? dpmi_vm(true) : create();
    if (pm) {
        v86_start(); enter(v, false);
        client_desc(v, 900, 0, UINT32_MAX, 0xFA, 0x40);
        client_desc(v, 901, 0, UINT32_MAX, 0xF2, 0x40);
        cur.tf.cs = DPMI_SEL(900); cur.tf.ds = cur.tf.es = DPMI_SEL(901); cur.tf.eip = 0x30000;
    } else {
        CHECK(!dosvm_boot(v));
        cur = (struct v86_frame){0};
        cur.tf.cs = 0x3000; cur.ds = 0x2000; cur.tf.user_ss = 0x2000; cur.tf.user_esp = 0x100;
        cur.tf.eflags = V86_VM | V86_IF | 2;
        v->monitor.state = V86_RUNNING;
    }
    v->info.dos_ready = true;
    v->monitor.pic[0].imr = v->monitor.pic[1].imr = 0xFF;
    int10(v, 0x13, 0, 0, 0);
    v->monitor.vif = true;
    uint8_t code[4096]; unsigned size = 0;
    code[size++] = 0xEE; /* DAC index 0: initial DX=3C8 */
    code[size++] = 0xBA; code[size++] = 0xC9; code[size++] = 3;
    if (pm) { code[size++] = 0; code[size++] = 0; }
    for (unsigned i = 0; i < 768; i++) {
        code[size++] = 0xB0; code[size++] = (uint8_t)(i & 63); code[size++] = 0xEE;
    }
    code[size++] = 0xBA; code[size++] = 0xC4; code[size++] = 3;
    if (pm) { code[size++] = 0; code[size++] = 0; }
    for (unsigned i = 0; i < 5; i++) {
        code[size++] = 0xB8; code[size++] = (uint8_t)i; code[size++] = v->vga.seq[i];
        if (pm) { code[size++] = 0; code[size++] = 0; code[size++] = 0x66; }
        code[size++] = 0xEF; /* indexed OUTW, low byte first */
    }
    code[size++] = 0x90; /* ordinary CPU instruction ends the burst */
    CHECK(!dosvm_write(v, 0x30000, code, size));
    cur.tf.eax = 0; cur.tf.edx = 0x3C8;
    unsigned gp = v->perf.exits[0], pf = v->perf.exits[1], maps = v->video_linear_maps;
    while (cur.tf.eip < (pm ? 0x30000u : 0u) + size - 1) {
        unsigned at = cur.tf.eip - (pm ? 0x30000u : 0u);
        CHECK(at < size - 1);
        if (code[at] == 0xB0) {
            cur.tf.eax = (cur.tf.eax & ~255u) | code[at + 1]; cur.tf.eip += 2;
        } else if (code[at] == 0xBA) {
            cur.tf.edx = code[at + 1] | (uint32_t)code[at + 2] << 8; cur.tf.eip += pm ? 5 : 3;
        } else if (code[at] == 0xB8) {
            cur.tf.eax = code[at + 1] | (uint32_t)code[at + 2] << 8; cur.tf.eip += pm ? 5 : 3;
        } else {
            CHECK(code[at] == 0xEE || code[at] == 0xEF || (code[at] == 0x66 && code[at + 1] == 0xEF));
            g_need_resched = !batch; step(v, 13, 0);
            CHECK(!left && v->info.state < DOSVM_STOPPING);
        }
    }
    unsigned exits = v->perf.exits[0] - gp;
    CHECK(v->perf.exits[1] == pf && v->video_direct == VIDEO_LINEAR && v->video_linear_maps == maps);
    CHECK(exits == (batch ? 13u : 774u) && v->perf.batched == (batch ? 761u : 0u));
    CHECK(cur.tf.eax == 0x0E04 && cur.tf.edx == 0x3C4 && cur.tf.eip == (pm ? 0x30000u : 0u) + size - 1);
    memcpy(palette, v->vga.dac, sizeof(v->vga.dac));
    printf("f3-14 DOOM-style %s DAC/sequencer startup %s: vga_io GP=%u vga_pf=0 batched=%u maps_delta=0\n",
           pm ? "PM32" : "V86", batch ? "batched" : "scalar", exits, v->perf.batched);
    destroy(v, true);
    empty();
    return exits;
}
static int hot_override(struct dosvm *v,uint16_t p,unsigned w,bool wr,uint32_t *value,void *arg)
{ (void)v;(void)p;(void)w;(void)wr;(void)arg; *value=0x87; return 0; }
static void test_hot_io(void)
{
    struct dosvm *v=create(); CHECK(!dosvm_boot(v)); v->info.dos_ready=true;
    const uint8_t code[]={0xe6,0x43,0xe4,0x40,0xe4,0x40,0xe4,0x43,0xee,0xee};
    CHECK(!dosvm_write(v,0x30000,code,sizeof(code)));
    port_out(v,0x388,1,0x20); /* populate the authoritative audio hint */
    unsigned dispatch=v->perf.io_dispatches,fast=v->perf.io_fast_cycles;
    for (unsigned i=0;i<1000;i++) {
        struct v86_frame f=vf(0,0,0,0); f.tf.cs=0x3000; int e;
        for (unsigned cycle=0;cycle<4;cycle++) CHECK(dosvm_io_fast(v,&f,0,&e) && !e);
        CHECK((uint8_t)f.tf.eax==0xff); /* 43h is write-only */
        f.tf.edx=0x388; f.tf.eax=0x20; CHECK(dosvm_io_fast(v,&f,0,&e) && !e);
        f.tf.edx=0x389; f.tf.eax=i; CHECK(dosvm_io_fast(v,&f,0,&e) && !e);
    }
    CHECK(v->perf.io_dispatches==dispatch && v->perf.io_fast_cycles==fast+6000);
    CHECK(!dosvm_port_unregister(v->info.key,0x40));
    CHECK(!dosvm_port_register(v->info.key,0x40,0x43,hot_override,0));
    struct v86_frame f=vf(0,0,0,0); f.tf.cs=0x3000; f.tf.eip=2; int e;
    CHECK(dosvm_io_fast(v,&f,0,&e) && !e && (uint8_t)f.tf.eax==0x87);
    CHECK(v->perf.io_dispatches==dispatch+1);
    destroy(v,true); empty();
    puts("F1-87 PIT latch/read + OPL writes: 6000 fast cycles, zero dispatcher entries; custom registration uses dispatcher PASS");
}
static void test_cost_bursts(void)
{
    test_hot_io();
    uint8_t scalar[256][3], batched[256][3];
    for (unsigned pm = 0; pm < 2; pm++) {
        CHECK(cost_startup(pm, false, scalar) == 774);
        CHECK(cost_startup(pm, true, batched) == 13);
        CHECK(!memcmp(scalar, batched, sizeof(scalar)));
        for (unsigned i = 0; i < 768; i++) CHECK(scalar[i / 3][i % 3] == (i & 63));
    }
    struct dosvm *v = create();
    CHECK(!dosvm_boot(v)); v->info.dos_ready = true;
    int10(v, 0x13, 0, 0, 0);
    const uint8_t rep[] = {0xF3, 0x6E, 0x90}, out[] = {0xEE, 0xEE, 0x90};
    uint8_t bytes[128]; for (unsigned i = 0; i < sizeof(bytes); i++) bytes[i] = i & 63;
    CHECK(!dosvm_write(v, 0x30000, rep, sizeof(rep)) && !dosvm_write(v, 0x20000, bytes, sizeof(bytes)));
    cur = (struct v86_frame){.tf={.cs=0x3000, .user_ss=0x2000, .user_esp=0x100,
        .eflags=V86_VM|V86_IF|2, .edx=0x3C9, .esi=0xA0000000, .ecx=0xB0000080}, .ds=0x2000};
    v->monitor.pic[0].imr = v->monitor.pic[1].imr = 0xFF;
    v->monitor.vif = true; v->monitor.state = V86_RUNNING;
    step(v, 13, 0);
    CHECK(!left && cur.tf.eip == 0 && cur.tf.ecx == 0xB0000040 && cur.tf.esi == 0xA0000040);
    step(v, 13, 0);
    CHECK(!left && cur.tf.eip == 2 && cur.tf.ecx == 0xB0000000 && cur.tf.esi == 0xA0000080);
    CHECK(v->perf.batched == 126);
    /* The REP cursor also progresses backwards and respects the 16-bit high halves. */
    cur.tf.eip = 0; cur.tf.eflags |= 0x400; cur.tf.ecx = 0xB0000080; cur.tf.esi = 0xA000007F;
    step(v, 13, 0); step(v, 13, 0);
    CHECK(!left && cur.tf.eip == 2 && cur.tf.esi == 0xA000FFFF && cur.tf.ecx == 0xB0000000);
    CHECK(!dosvm_write(v, 0x30000, out, sizeof(out)));
    cur.tf.eip = 0; cur.tf.eflags &= ~0x400;
    for (unsigned guard = 0; guard < 6; guard++) {
        struct v86_frame f = cur;
        f.tf.eip = 0;
        v->frame_pm = false; v->monitor.vif = true;
        if (guard == 0) f.tf.eflags |= V86_TF;
        if (guard == 1) v->monitor.shadow = true;
        if (guard == 2) v->dpmi.if_armed = true;
        if (guard == 3) v->dpmi.dr7 = 1;
        if (guard == 4) g_need_resched = true;
        if (guard == 5) { v->monitor.pic[0].imr = 0; v86_irq_raise(&v->monitor, 1); }
        uint32_t before = v->perf.batched;
        CHECK(!dosvm_io_burst(v, &f) && !f.tf.eip && v->perf.batched == before);
        v->monitor.shadow = v->dpmi.if_armed = false; v->dpmi.dr7 = 0; g_need_resched = false;
        v->monitor.pic[0].irr = 0; v->monitor.pic[0].imr = 0xFF;
    }
    /* A redundant sequencer write leaves the native linear stores mapped. */
    linear_store(v, 0x1237, 0xA5);
    unsigned maps = v->video_linear_maps, harvests = v->perf.harvests;
    port_out(v, 0x3C4, 2, (uint16_t)v->vga.seq[2] << 8 | 2);
    CHECK(v->video_direct == VIDEO_LINEAR && v->video_linear_maps == maps && v->perf.harvests == harvests);
    for (unsigned i = 0; i < 64000; i++) linear_store(v, i, (uint8_t)i);
    CHECK(!v->perf.exits[1]); /* CPU stores through the open window have no #PF */
    destroy(v, true); empty();
    /* PM REP uses the same checked element emulator, including partial
     * progress when the next source page is inaccessible. */
    v = dpmi_vm(true); v86_start(); enter(v, true);
    client_desc(v, 900, 0, UINT32_MAX, 0xFA, 0x40);
    client_desc(v, 901, 0, UINT32_MAX, 0xF2, 0x40);
    int10(v, 0x13, 0, 0, 0);
    CHECK(!dosvm_write(v, 0x30000, rep, sizeof(rep)) && !dosvm_write(v, 0x20FC0, bytes, 64));
    cur.tf.cs = DPMI_SEL(900); cur.tf.ds = cur.tf.es = DPMI_SEL(901); cur.tf.eip = 0x30000;
    cur.tf.edx = 0x3C9; cur.tf.esi = 0x20FC0; cur.tf.ecx = 65;
    cur.tf.eflags = V86_IF | 2; v->monitor.vif = true;
    v->monitor.pic[0].imr = v->monitor.pic[1].imr = 0xFF;
    CHECK(!as_map(&v->as, 0x21000, v->frames[0x21], PTE_W));
    unsigned gp = v->perf.exits[0];
    step(v, 13, 0);
    CHECK(!left && cur.tf.eip == 0x30000 && cur.tf.esi == 0x21000 && cur.tf.ecx == 1 &&
          v->perf.exits[0] == gp + 1 && v->perf.batched == 63);
    for (unsigned i = 0; i < 64; i++) CHECK(v->vga.dac[i / 3][i % 3] == bytes[i]);
    step(v, 13, 0);
    CHECK(left && v->info.state == DOSVM_STOPPING && v->info.reason == DOSVM_FAULT &&
          cur.tf.eip == 0x30000 && cur.tf.esi == 0x21000 && cur.tf.ecx == 1 && v->perf.batched == 63);
    CHECK(!as_map(&v->as, 0x21000, v->frames[0x21], PTE_U | PTE_W));
    destroy(v, true); empty();
    puts("f3-14 VGA: exact scalar/batched palette, bounded REP/DF/high halves, IRQ/TF/watchpoint/IF guards, redundant sequencer and zero-fault mode13h PASS");
}
