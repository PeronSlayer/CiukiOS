/* fx-doswin3: production bursts against their original two-pass path and
 * the scalar DAC instruction oracle. No guest image or game payload.
 * SPDX-License-Identifier: GPL-2.0-only */
static void test_doswin3_stores(void)
{
    static const uint8_t programs[][32]={
        {0x8A,0x04,0x26,0x88,0x05,0x26,0xC6,0x45,1,0x5A,
         0x26,0x66,0xC7,0x45,2,0x44,0x33,0x22,0x11,0xCB},
        {0x26,0x8A,0x05,0x26,0x88,0x45,8,0xCB}, /* latch load then store */
        {0x26,0x80,0x05,1,0x26,0x86,0x45,1,0xCB}, /* RMW and XCHG */
        {0xF3,0xA4,0xCB}, /* bulk RAM-to-planar MOVS */
        {0xF3,0xAA,0xCB}, /* bulk planar STOS */
        {0x8A,0x04,0xC6,0x06,0,2,0x55,0xCB}, /* RAM store stops before mutation */
        {0x26,0x66,0xC7,0x05,0x44,0x33,0x22,0x11,0xCB},
    };
    static cvga_state expected;
    struct v86_frame reference={0};
    for (unsigned prog=0;prog<ARRAY_SIZE(programs);prog++) for (unsigned pass=0;pass<2;pass++) {
        struct dosvm *v=create();
        int10(v,0x13,0,0,0);
        port_out(v,0x3C4,2,0x0604); /* unchained, multiple plane mask */
        for (unsigned p=0;p<4;p++) for (unsigned at=0;at<CVGA_PLANE_SIZE;at++)
            v->vga.plane[p][at]=(uint8_t)(p*17+at);
        uint8_t source[64];for (unsigned i=0;i<sizeof(source);i++)source[i]=(uint8_t)(i*7+3);
        CHECK(!dosvm_write(v,0x30000,source,sizeof(source)));
        CHECK(!dosvm_write(v,0x20000,programs[prog],sizeof(programs[prog])));
        struct v86_frame f={.tf={.cs=0x2000,.eip=0,.eax=0xAABBCC5A,.ecx=32,
            .user_ss=0x9000,.user_esp=0xFFF0,.eflags=V86_VM|V86_IF|3},.ds=0x3000,.es=0xA000};
        if (prog==6) { f.es=0xBFFF;f.tf.edi=14; } /* dword crosses C0000: no partial commit */
        v->monitor.vif=false;
        v->fullscreen_hold=!pass; /* original, uncached two-pass interpreter */
        dosvm_vga_burst(v,&f);
        v->fullscreen_hold=false;
        if (!pass) { expected=v->vga;reference=f; }
        else {
            CHECK(!memcmp(&expected,&v->vga,sizeof(expected)));
            CHECK(!memcmp(&reference,&f,sizeof(f)));
        }
        uint8_t untouched;CHECK(!dosvm_read(v,0x30200,&untouched,1) && !untouched);
        destroy(v,true);empty();
    }
    puts("fx-doswin3 scalar stores: cached and original paths agree for byte/dword stores, latch reads, RMW, strings, refused RAM stores and cross-aperture rollback PASS");
}

static void test_doswin3_palette(void)
{
    /* MOV EDX,EDX in place of JMP $+2 disables the recognizer while retaining
     * the same architectural results through the general decoder. */
    static uint8_t expected[256][3];
    static const unsigned counts[]={1,2,257,768,769};
    struct trap_frame reference={0};
    for (unsigned reg=3;reg<8;reg++) {
        if (reg!=3 && reg!=6 && reg!=7)continue;
        for (unsigned sample=0;sample<ARRAY_SIZE(counts);sample++)
            for (unsigned pass=0;pass<2;pass++) {
                unsigned count=counts[sample];
                struct dosvm *v=fx27_vm(true);
                uint8_t code[]={0x66,0xBA,0xC8,0x03,0xEE,0x66,0x42,
                    0xEB,0,0x8A,(uint8_t)reg,(uint8_t)(0x40+reg),0xEE,0x49,0x75,0xF7,0x90};
                if (!pass) { code[7]=0x89;code[8]=0xD2; }
                CHECK(!dosvm_write(v,0x32000,code,sizeof(code)));
                uint8_t palette[769];for (unsigned i=0;i<sizeof(palette);i++)palette[i]=(uint8_t)(i*17+3);
                /* Cross a data page; physically separated RAM frames. */
                CHECK(!dosvm_write(v,0x50F00,palette,sizeof(palette)));
                cur.tf.eip=0x32000;cur.tf.eax=0x12345600;cur.tf.ecx=count;
                cur.tf.ebx=cur.tf.esi=cur.tf.edi=0x50F00;
                cur.tf.eflags|=1;
                unsigned fast=v->perf.dac_counted;
                fx27_run(v,0x32010,100000,false);
                CHECK(v->perf.dac_counted-fast==(pass ? count-1 : 0));
                if (!pass) { memcpy(expected,v->vga.dac,sizeof(expected));reference=cur.tf; }
                else { CHECK(!memcmp(expected,v->vga.dac,sizeof(expected)));CHECK(!memcmp(&reference,&cur.tf,sizeof(reference))); }
                destroy(v,true);empty();
            }
    }
    /* A missing or borrowed source page refuses the next component before
     * decrementing the count, changing a register or touching the DAC. */
    for (unsigned borrowed=0;borrowed<2;borrowed++) {
        struct dosvm *v=fx27_vm(true);
        const uint8_t loop[]={0xEB,0,0x8A,3,0x43,0xEE,0x49,0x75,0xF7};
        CHECK(!dosvm_write(v,0x32000,loop,sizeof(loop)));
        cvx_cpu c={.code32=1,.eip=0x32006,.base_valid=1u<<CVX_CS};
        c.sreg[CVX_CS]=(uint16_t)cur.tf.cs;c.sreg[CVX_DS]=(uint16_t)cur.tf.ds;
        c.gpr[CVX_ECX]=10;c.gpr[CVX_EBX]=0x50000;c.gpr[CVX_EDX]=0x3C9;
        struct dac_bus ctx={.cache={.v=v}};
        cvx_bus bus={.read=dac_read,.write=dac_write,.fetch=dac_fetch,.context=&ctx,
            .segment_base=dac_base,.validate=dac_validate,.port=dac_port};
        uint32_t *pte=window_pte(v,0x50000),saved=*pte;
        *pte=borrowed ? saved|PTE_BORROWED : 0;
        cvx_cpu before=c;
        CHECK(!dac_counted_run(v,&cur,&c,&ctx,&bus,100,g_ticks));
        CHECK(!memcmp(&before,&c,sizeof(c)));
        *pte=saved;
        destroy(v,true);empty();
    }
    puts("fx-doswin3 counted DAC loop: scalar oracle, EBX/ESI/EDI, short counts, page crossing, complete registers/flags and inaccessible/borrowed source stops PASS");
}
static void test_doswin3(void)
{
    test_doswin3_stores();test_doswin3_palette();
    /* A recognized loop must still yield to every safety guard. Exercise
     * the recognizer itself, so the ordinary decoder cannot mask a bypass. */
    for (unsigned guard=0;guard<15;guard++) {
        struct dosvm *v=fx27_vm(true);
        const uint8_t loop[]={0xEB,0,0x8A,3,0x43,0xEE,0x49,0x75,0xF7};
        uint8_t value=0x2A;
        CHECK(!dosvm_write(v,0x32000,loop,sizeof(loop)));
        CHECK(!dosvm_write(v,0x50000,&value,1));
        cvx_cpu c={.code32=1,.eip=0x32006,.base_valid=1u<<CVX_CS};
        c.sreg[CVX_CS]=(uint16_t)cur.tf.cs;c.sreg[CVX_DS]=(uint16_t)cur.tf.ds;
        c.gpr[CVX_ECX]=10;c.gpr[CVX_EBX]=0x50000;c.gpr[CVX_EDX]=0x3C9;
        struct dac_bus ctx={.cache={.v=v}};
        cvx_bus bus={.read=dac_read,.write=dac_write,.fetch=dac_fetch,.context=&ctx,
            .segment_base=dac_base,.validate=dac_validate,.port=dac_port};
        unsigned budget=1;
        uint64_t tick=g_ticks;
        switch (guard) {
        case 1: budget=0;break;
        case 2: tick--;break;
        case 3: g_need_resched=true;break;
        case 4: v->monitor.shadow=true;break;
        case 5: v->dpmi.shadow=true;break;
        case 6: v->dpmi.dr7=1;break;
        case 7: v->dpmi.return_steps=1;break;
        case 8: v->dpmi.if_step=true;break;
        case 9: v->dpmi.if_restore_step=true;break;
        case 10: cur.tf.eflags|=V86_TF;break;
        case 11: v->fullscreen=true;break;
        case 12: v->fullscreen_hold=true;break;
        case 13: c.code32=0;break;
        case 14: v->monitor.vif=true;v->monitor.pic[0].imr=0xFD;v->monitor.pic[0].irr|=2;break;
        }
        cvx_cpu before=c;
        unsigned writes=v->perf.port_count[DOSVM_VGA_DAC];
        CHECK(dac_counted_run(v,&cur,&c,&ctx,&bus,budget,tick)==(guard ? 0u : 1u));
        if (guard) {
            CHECK(!memcmp(&before,&c,sizeof(c)));
            CHECK(v->perf.port_count[DOSVM_VGA_DAC]==writes);
        } else {
            CHECK(c.gpr[CVX_ECX]==9 && c.gpr[CVX_EBX]==0x50001 && c.eip==before.eip);
            CHECK(v->perf.port_count[DOSVM_VGA_DAC]==writes+1);
        }
        g_need_resched=false;v->fullscreen=false;v->fullscreen_hold=false;
        destroy(v,true);empty();
    }
    puts("fx-doswin3 counted DAC guards: one-component budget, tick/reschedule, debug/IF/shadow, pending IRQ, full-screen/hold and 16-bit refusal preserve registers and DAC PASS");
    test_doswin3_r2();
}
