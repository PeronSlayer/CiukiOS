/* f3-33: scalar/differential controls for virtual byte I/O and INT frames.
 * SPDX-License-Identifier: GPL-2.0-only */
static int exit_reference_reflect(struct v86 *v, struct v86_frame *f, uint8_t vector)
{
    uint8_t ivt[4], slot[2];
    uint16_t sp=(uint16_t)(f->tf.user_esp-6);
    void *a=segmem(v,f->tf.user_ss,sp,2,true);
    void *b=segmem(v,f->tf.user_ss,(uint16_t)(sp+2),2,true);
    void *c=segmem(v,f->tf.user_ss,(uint16_t)(sp+4),2,true);
    if (copyseg(v,0,vector*4u,ivt,4,false) || !a || !b || !c) return v86_abort(v,f,-EFAULT);
    uint32_t ip=getword(ivt,2), cs=getword(ivt+2,2);
    if (!segmem(v,cs,ip,1,false)) return v86_abort(v,f,-EFAULT);
    putword(slot,2,f->tf.eip);
    if (copyseg(v,f->tf.user_ss,sp,slot,2,true)) return v86_abort(v,f,-EFAULT);
    putword(slot,2,f->tf.cs);
    if (copyseg(v,f->tf.user_ss,(uint16_t)(sp+2),slot,2,true)) return v86_abort(v,f,-EFAULT);
    putword(slot,2,guest_flags(v,f));
    if (copyseg(v,f->tf.user_ss,(uint16_t)(sp+4),slot,2,true)) return v86_abort(v,f,-EFAULT);
    setlow(&f->tf.user_esp,sp); f->tf.eip=ip; f->tf.cs=cs;
    v->vif=v->shadow=false; f->tf.eflags&=~V86_TF; v->state=V86_RUNNING;
    return 0;
}
static void test_exit_reflection(void)
{
    struct dosvm *v=create(); CHECK(!dosvm_boot(v));
    uint16_t vec[]={0x20,0x500}; CHECK(!dosvm_write(v,0x84,vec,4));
    const uint16_t stacks[]={0x100,0x1002,0,2,4,5,0xFFFE};
    for (unsigned hook=0;hook<2;hook++) {
        vec[0]+=2; CHECK(!dosvm_write(v,0x84,vec,4));
        for (unsigned i=0;i<ARRAY_SIZE(stacks);i++) {
            struct v86_frame initial={.tf={.cs=0x3000,.eip=0x1234,.user_ss=0x2000,
                .user_esp=0xABCD0000u|stacks[i],.eflags=V86_VM|V86_IF|V86_TF|0x47}};
            struct v86_frame a=initial,b=initial;
            struct v86 monitor=v->monitor;
            uint8_t old[6], expected[6], actual[6];
            uint16_t sp=(uint16_t)(stacks[i]-6);
            for (unsigned k=0;k<3;k++) CHECK(!dosvm_read(v,0x20000+(uint16_t)(sp+k*2),old+k*2,2));
            int ea=exit_reference_reflect(&v->monitor,&a,0x21);
            struct v86 expected_monitor=v->monitor;
            for (unsigned k=0;k<3;k++) {
                CHECK(!dosvm_read(v,0x20000+(uint16_t)(sp+k*2),expected+k*2,2));
                CHECK(!dosvm_write(v,0x20000+(uint16_t)(sp+k*2),old+k*2,2));
            }
            v->monitor=monitor;
            int eb=v86_reflect(&v->monitor,&b,0x21);
            for (unsigned k=0;k<3;k++) CHECK(!dosvm_read(v,0x20000+(uint16_t)(sp+k*2),actual+k*2,2));
            CHECK(ea==eb && !memcmp(&a,&b,sizeof(a)) && !memcmp(expected,actual,6));
            CHECK(!memcmp(&v->monitor,&expected_monitor,sizeof(monitor)));
            if (!eb) {
                uint8_t iret=0xCF; CHECK(!dosvm_write(v,(b.tf.cs<<4)+b.tf.eip,&iret,1));
                CHECK(!v86_emulate(&v->monitor,&b));
                CHECK(b.tf.cs==initial.tf.cs && b.tf.eip==initial.tf.eip && b.tf.user_esp==initial.tf.user_esp);
            }
        }
    }
    /* Complete destination preflight: a ROM slot prevents all frame writes. */
    struct v86_frame f={.tf={.cs=0x3000,.eip=0,.user_ss=0xEFFF,.user_esp=0x16,.eflags=V86_VM|V86_IF|2}};
    uint8_t old[6], now[6]; CHECK(!dosvm_read(v,0xF0000,old,6));
    CHECK(v86_reflect(&v->monitor,&f,0x21)==-EFAULT);
    CHECK(!dosvm_read(v,0xF0000,now,6) && !memcmp(old,now,6));
    destroy(v,true); empty();
}
static void test_exit_fast_io(void)
{
    struct dosvm *v=create(); CHECK(!dosvm_boot(v));
    static const uint16_t ports[]={0x20,0x21,0xA0,0xA1,0x40,0x41,0x42,0x43};
    static const uint8_t ops[]={0xE4,0xE6,0xEC,0xEE};
    for (unsigned p=0;p<ARRAY_SIZE(ports);p++) for (unsigned o=0;o<ARRAY_SIZE(ops);o++)
    for (unsigned value=0;value<256;value++) {
        uint8_t code[]={ops[o],(uint8_t)ports[p],0x90};
        if (ports[p]>255 && ops[o]<0xE8) continue;
        CHECK(!dosvm_write(v,0x30000,code,sizeof(code)));
        struct v86 saved=v->monitor;
        struct dosvm_pit pit=v->pit;
        struct v86_frame a={.tf={.cs=0x3000,.eip=0,.eax=0xBEEF0000u|value,.edx=ports[p],.eflags=V86_VM|V86_IF|V86_TF|2}};
        struct v86_frame b=a;
        v->frame_pm=false;
        int ea=v86_emulate(&v->monitor,&a);
        struct v86 expected=v->monitor;
        struct dosvm_pit expected_pit=v->pit;
        v->monitor=saved; v->pit=pit;
        int eb=123;
        CHECK(dosvm_io_fast(v,&b,0,&eb));
        CHECK(ea==eb && !memcmp(&a,&b,sizeof(a)));
        CHECK(!memcmp(&expected,&v->monitor,sizeof(expected)) && !memcmp(&expected_pit,&v->pit,sizeof(pit)));
        v->monitor=saved; v->pit=pit;
    }
    destroy(v,true); empty();
}
static void test_exit_burst(bool pm, bool big)
{
    struct dosvm *v=pm ? dpmi_vm(true) : create();
    if (pm) { v86_start(); enter(v,big); }
    else { CHECK(!dosvm_boot(v)); v86_start(); }
    struct v86_frame initial=cur;
    if (pm) {
        client_desc(v,900,0,UINT32_MAX,0xFA,big ? 0x40 : 0);
        client_desc(v,901,0,UINT32_MAX,0xF2,big ? 0x40 : 0);
        initial.tf.cs=DPMI_SEL(900); initial.tf.user_ss=initial.tf.ds=initial.tf.es=DPMI_SEL(901);
        initial.tf.user_esp=0x28000; initial.tf.eip=big ? 0x30000 : 0x3000;
    } else { initial.tf.cs=0x3000; initial.tf.eip=0; }
    uint32_t address=pm ? initial.tf.eip : 0x30000;
    const uint8_t code[]={0xE6,0x43,0xE4,0x40,0x88,0xC4,0xE4,0x40,0x90};
    CHECK(!dosvm_write(v,address,code,sizeof(code)));
    v->info.dos_ready=true; v->monitor.pic[0].imr=v->monitor.pic[1].imr=255;
    v->monitor.state=V86_RUNNING; v->frame_pm=pm;
    initial.tf.eax=0xBAAD0000; initial.tf.eflags|=0x845;
    struct v86 monitor=v->monitor;
    struct dosvm_pit pit=v->pit;
    struct v86_frame scalar={0};
    struct dosvm_pit scalar_pit={0};
    for (unsigned batch=0;batch<2;batch++) {
        cur=initial; v->monitor=monitor; v->pit=pit;
        uint32_t gp=v->perf.exits[0];
        unsigned count=0;
        while (cur.tf.eip<initial.tf.eip+8) {
            if (cur.tf.eip==initial.tf.eip+4) {
                cur.tf.eax=(cur.tf.eax&~0xFF00u)|((cur.tf.eax&255)<<8); cur.tf.eip+=2;
            } else { g_need_resched=!batch; step(v,13,0); count++; CHECK(!left); }
        }
        CHECK(count==(batch ? 1u : 3u) && v->perf.exits[0]-gp==count);
        if (!batch) { scalar=cur; scalar_pit=v->pit; }
        else CHECK(!memcmp(&scalar,&cur,sizeof(cur)) && !memcmp(&scalar_pit,&v->pit,sizeof(pit)));
    }
    /* All established stop conditions leave speculative MOVs unretired. */
    struct v86_frame next=initial; next.tf.eip+=2;
    for (unsigned guard=0;guard<8;guard++) {
        cur=next; v->monitor=monitor; v->pit=pit;
        v->dpmi.if_armed=v->dpmi.shadow=false; v->dpmi.return_steps=v->dpmi.dr7=0;
        g_need_resched=false;
        switch (guard) {
        case 0:g_need_resched=true;break;
        case 1:cur.tf.eflags|=V86_TF;break;
        case 2:v->dpmi.dr7=1;break;
        case 3:v->dpmi.if_armed=true;break;
        case 4:v->dpmi.return_steps=1;break;
        case 5:v->monitor.shadow=true;break;
        case 6:v->dpmi.shadow=true;break;
        case 7:v->monitor.vif=true;v->monitor.pic[0].imr=0;v->monitor.pic[0].irr=1;break;
        }
        struct v86_frame before=cur;
        CHECK(!dosvm_io_burst(v,&cur) && !memcmp(&before,&cur,sizeof(cur)));
    }
    g_need_resched=false;
    v->dpmi.if_armed=v->dpmi.shadow=false; v->dpmi.return_steps=v->dpmi.dr7=0;
    destroy(v,true); empty();
}
static bool exit_scalar_preceding(struct dosvm *v, struct v86_frame *f, const struct dpmi_seg *cs,
                            uint8_t *pop, unsigned *width, bool boundary_only)
{
    struct dpmi *d = &v->dpmi;
    uint32_t at = d->decode_eip, end = f->tf.eip;
    if (!d->decode_valid || d->decode_cs != (uint16_t)f->tf.cs || at > end ||
        end > cs->limit || end - at > 4096) return false;
    uint8_t previous = 0, last = 0;
    unsigned previous_w = 0, last_w = 0;
    while (at < end) {
        uint8_t bytes[15], op;
        unsigned n = end - at < 15 ? end - at : 15, w;
        if (dosvm_read(v, cs->base + at, bytes, n)) return false;
        unsigned len = flags_instruction(bytes, n, cs->big, &op, &w);
        if (!len) return false;
        previous = last; previous_w = last_w; last = op; last_w = w;
        at += len;
    }
    if (boundary_only) return true;
    if (last == 0x9C) { *pop = 0; *width = last_w; return true; }
    if (previous != 0x9C || previous_w != last_w || !pop_register(f, last)) return false;
    *pop = last; *width = last_w; return true;
}

static bool exit_reference_arm(struct dosvm *v, struct v86_frame *f, const struct dpmi_seg *cs)
{
    uint8_t pop;
    unsigned width;
    if (!exit_scalar_preceding(v, f, cs, &pop, &width, false) &&
        !flags_control_preceding(v, f, cs, &pop, &width)) return false;
    /* PUSHF's image of the real flags: IF set (the host never runs a client
     * disabled), IOPL 0, NT and TF clear, status flags as now (POP and the
     * trapped CLI change none). */
    /* PUSHFD also preserves AC/ID. RF/VM are cleared by PUSHFD. Comparing
     * against a truncated uint16_t rejected legitimate high flags. */
    uint32_t want = (f->tf.eflags & (width == 4 ? 0x00240CD5u : 0x0CD5u)) | V86_IF | 2u;
    if (!pop) {                             /* decoded PUSHF; CLI */
        struct dpmi_seg ss;
        uint16_t sel = (uint16_t)f->tf.user_ss;
        uint32_t lin, image = 0;
        if (!dpmi_seg(v, sel, &ss)) return false;
        uint32_t esp = ss.big ? f->tf.user_esp : (uint16_t)f->tf.user_esp;
        if (!dpmi_ptr(v, sel, esp, width, true, &lin) || dosvm_read(v, lin, &image, width) || image != want) return false;
        image |= V86_TF;
        return !dosvm_write(v, lin, &image, width);
    }
    uint32_t *r = pop_register(f, pop);
    if (!r || (width == 4 ? *r : (uint16_t)*r) != want) return false;
    *r |= V86_TF;
    return true;
}

static void test_exit_flags(void)
{
    for (unsigned big=0;big<2;big++) {
        struct dosvm *v=dpmi_vm(true); v86_start(); enter(v,big);
        client_desc(v,900,0,UINT32_MAX,0xFA,big ? 0x40 : 0);
        client_desc(v,901,0,UINT32_MAX,0xF2,big ? 0x40 : 0);
        struct v86_frame initial={.tf={.cs=DPMI_SEL(900),.user_ss=DPMI_SEL(901),
            .eip=0x8002,.user_esp=0x2800,.eflags=V86_IF|2}};
        struct dpmi_seg cs; CHECK(dpmi_seg(v,initial.tf.cs,&cs));
        for (unsigned pop=0x57;pop<=0x5F;pop++) for (unsigned match=0;match<2;match++) {
            if (pop==0x5C) continue;
            uint8_t code[]={0x9C,(uint8_t)(pop==0x57 ? 0xFA : pop),0xFA};
            initial.tf.eip=0x8000+(pop==0x57 ? 1 : 2);
            CHECK(!dosvm_write(v,0x8000,code,sizeof(code)));
            struct v86_frame a=initial,b=initial;
            uint32_t image=match ? V86_IF|2 : 0x40, zero=0;
            if (pop!=0x57) *pop_register(&a,pop)=*pop_register(&b,pop)=image;
            CHECK(!dosvm_write(v,0x2800,pop==0x57 ? &image : &zero,4));
            v->dpmi.decode_valid=true; v->dpmi.decode_cs=initial.tf.cs; v->dpmi.decode_eip=0x8000;
            bool ea=exit_reference_arm(v,&a,&cs);
            uint32_t expected; CHECK(!dosvm_read(v,0x2800,&expected,4));
            CHECK(!dosvm_write(v,0x2800,pop==0x57 ? &image : &zero,4));
            bool eb=arm_flags_image(v,&b,&cs);
            uint32_t actual; CHECK(!dosvm_read(v,0x2800,&actual,4));
            if (!(ea==eb && ea==!!match && expected==actual && !memcmp(&a,&b,sizeof(a))))
                fprintf(stderr,"flags differential big=%u pop=%x match=%u old=%u new=%u expected=%x actual=%x\n",big,pop,match,ea,eb,expected,actual);
            CHECK(ea==eb && ea==!!match && expected==actual && !memcmp(&a,&b,sizeof(a)));
        }
        /* Window refills, a page-crossing instruction and changed bytes on
         * the next proof must have exactly the scalar walk's result. */
        uint8_t bytes[600]; memset(bytes,0x90,sizeof(bytes)); bytes[598]=0x9C; bytes[599]=0x58;
        const uint32_t base=0x8F80;
        CHECK(!dosvm_write(v,base,bytes,sizeof(bytes)));
        initial.tf.eip=base+sizeof(bytes); v->dpmi.decode_eip=base;
        for (unsigned change=0;change<2;change++) {
            uint8_t pa=0,pb=0; unsigned wa=0,wb=0;
            bool ea=exit_scalar_preceding(v,&initial,&cs,&pa,&wa,false);
            bool eb=flags_preceding(v,&initial,&cs,&pb,&wb,false);
            CHECK(ea==eb && (ea ? pa==pb && wa==wb : true) && ea==!change);
            uint8_t nop=0x90; CHECK(!dosvm_write(v,base+598,&nop,1));
        }
        destroy(v,true); empty();
    }
}
static void test_exit_cost(void)
{
    test_exit_reflection(); test_exit_fast_io(); test_exit_flags();
    test_exit_burst(false,false); test_exit_burst(true,false); test_exit_burst(true,true);
    struct dosvm *v=dpmi_vm(true); v86_start(); enter(v,false);
    client_desc(v,900,0,UINT32_MAX,0xFA,0);
    struct dpmi_seg cs; CHECK(dpmi_seg(v,DPMI_SEL(900),&cs));
    uint8_t prefix=0x67, in=0xE4, port=0x21;
    CHECK(!dosvm_write(v,0xFFFE,&prefix,1) && !dosvm_write(v,0xFFFF,&in,1) && !dosvm_write(v,0,&port,1));
    struct v86_frame a=cur,b=cur; a.tf.cs=b.tf.cs=DPMI_SEL(900);
    a.tf.eip=0xFFFE; b.tf.eip=0xFFFF;
    CHECK(!emulate(v,&a)); /* prefix forces the complete scalar decoder */
    int error=123; CHECK(dosvm_io_fast(v,&b,&cs,&error) && !error && !memcmp(&a,&b,sizeof(a)));
    destroy(v,true); empty();
    puts("f3-33: scalar hot-port differential, live IVT, wrapped/guarded INT/IRET stacks, RM/PM16/PM32 PIT bursts and stop boundaries PASS");
}
