/* Migrated PIC/PIT and keyboard-focus cases plus F3 corrected rules.
 * Included by the production lifecycle harness, with identical transports. */
static void device_out(struct dosvm *v, uint16_t p, uint8_t b)
{ uint32_t x=b; CHECK(!v86_io(&v->monitor,p,1,true,&x)); }
static uint8_t device_in(struct dosvm *v, uint16_t p)
{ uint32_t x=0; CHECK(!v86_io(&v->monitor,p,1,false,&x)); return x; }
static void device_pic_reset(struct dosvm *v, uint8_t flags)
{
    for (unsigned i=0;i<2;i++) {
        uint16_t p=i?0xA0:0x20;
        device_out(v,p,0x11); device_out(v,p+1,i?0x70:8);
        device_out(v,p+1,i?2:4); device_out(v,p+1,flags); device_out(v,p+1,0);
    }
}
static void test_device_pic(void)
{
    struct dosvm *v=create(), *other=create();
    device_pic_reset(v,1);
    CHECK(dosvm_irq_raise(dosvm_key(v),5)); CHECK(dosvm_pic_ack(&v->monitor)==13);
    CHECK(v->monitor.pic[0].isr==32 && !other->monitor.pic[0].isr);
    CHECK(dosvm_irq_raise(dosvm_key(v),6)); CHECK(v86_irq_pending(&v->monitor)<0);
    CHECK(dosvm_irq_raise(dosvm_key(v),1)); CHECK(dosvm_pic_ack(&v->monitor)==9);
    device_out(v,0x20,0x65); CHECK(v->monitor.pic[0].isr==2); /* specific, not top */
    device_out(v,0x20,0x20); CHECK(dosvm_pic_ack(&v->monitor)==14);
    device_out(v,0x20,0xA0); CHECK(v->monitor.pic[0].priority==6);
    CHECK(dosvm_irq_raise(dosvm_key(v),7)); CHECK(dosvm_irq_raise(dosvm_key(v),0));
    CHECK(dosvm_pic_ack(&v->monitor)==15); device_out(v,0x20,0x20);
    CHECK(dosvm_pic_ack(&v->monitor)==8); device_out(v,0x20,0x20);
    CHECK(dosvm_pic_ack(&v->monitor)==15 && !v->monitor.pic[0].isr); /* spurious 7 */
    device_pic_reset(v,1); CHECK(dosvm_irq_raise(dosvm_key(v),12));
    CHECK(dosvm_pic_ack(&v->monitor)==0x74 && v->monitor.pic[0].isr==4 && v->monitor.pic[1].isr==16);
    CHECK(dosvm_irq_raise(dosvm_key(v),13));
    device_out(v,0x20,0x62); CHECK(v86_irq_pending(&v->monitor)<0); /* slave still busy */
    device_out(v,0xA0,0x64); CHECK(dosvm_pic_ack(&v->monitor)==0x75);
    device_out(v,0xA0,0x65); device_out(v,0x20,0x62);
    CHECK(dosvm_irq_raise(dosvm_key(v),15)); device_out(v,0xA1,0x80);
    CHECK(dosvm_pic_ack(&v->monitor)==0x77 && !v->monitor.pic[1].isr && v->monitor.pic[0].isr==4);
    device_out(v,0x20,0x62); CHECK(!v->monitor.pic[0].isr);
    device_pic_reset(v,3); device_out(v,0x20,0x80);
    CHECK(dosvm_irq_raise(dosvm_key(v),3)); CHECK(dosvm_pic_ack(&v->monitor)==11);
    CHECK(!v->monitor.pic[0].isr && v->monitor.pic[0].priority==3);
    device_out(v,0x20,0x11); device_out(v,0x21,0x30); device_out(v,0x21,4); device_out(v,0x21,1);
    CHECK(dosvm_irq_raise(dosvm_key(v),1)); device_out(v,0x20,0x0C);
    CHECK(device_in(v,0x20)==0x81 && v->monitor.pic[0].isr==2);
    device_out(v,0x20,0x0B); CHECK(device_in(v,0x20)==2);
    device_out(v,0x21,2); device_out(v,0x20,0x68); /* special mask */
    CHECK(dosvm_irq_raise(dosvm_key(v),4) && v86_irq_pending(&v->monitor)==4);
    destroy(v,true); destroy(other,true); empty();
}
static void test_device_pit(void)
{
    struct dosvm *v=create();
    for (unsigned mode=0;mode<6;mode++) {
        dosvm_pit_init(v);
        device_out(v,0x43,0xB0|(mode<<1)); device_out(v,0x42,7); device_out(v,0x42,0);
        struct dosvm_pit_channel *c=&v->pit.channel[2];
        CHECK(c->null_count);
        pit_gate(c,true);
        CHECK(!pit_advance_channel(c,1) && !c->null_count && c->reload==7);
        CHECK(!pit_advance_channel(c,6));
        if (mode==2 || mode==3) CHECK(pit_advance_channel(c,1)==1);
        else if (mode==0 || mode==1) CHECK(pit_advance_channel(c,1)==1 && c->out);
        else { CHECK(!pit_advance_channel(c,1) && !c->out); CHECK(pit_advance_channel(c,1)==1 && c->out); }
    }
    dosvm_pit_init(v);
    device_out(v,0x43,0x34); device_out(v,0x40,0xE8); device_out(v,0x40,3);
    struct dosvm_pit_channel *c=&v->pit.channel[0];
    CHECK(!pit_advance_channel(c,1000)); CHECK(pit_advance_channel(c,1)==1);
    CHECK(pit_advance_channel(c,4000)==4 && pit_counter(c)==1000);
    device_out(v,0x43,0xC2); /* legacy read-back: count+status, channel 0 */
    CHECK((device_in(v,0x40)&0x3F)==0x34); CHECK(device_in(v,0x40)==0xE8);
    CHECK(!pit_advance_channel(c,100)); CHECK(device_in(v,0x40)==3); /* count frozen */
    device_out(v,0x43,0); uint16_t latched=c->latch;
    CHECK(!pit_advance_channel(c,1)); device_out(v,0x43,0); CHECK(c->latch==latched);
    device_in(v,0x40); device_in(v,0x40);
    device_out(v,0x43,0x35); device_out(v,0x40,0); device_out(v,0x40,0); /* BCD zero=10000 */
    CHECK(!pit_advance_channel(c,1) && c->reload==10000);
    CHECK(!pit_advance_channel(c,1) && pit_counter(c)==0x9999);
    uint32_t bad=0xAF; device_out(v,0x43,0x11);
    CHECK(v86_io(&v->monitor,0x40,1,true,&bad)==-V86_EPERM && c->null_count);
    dosvm_pit_init(v); CHECK(!dosvm_boot(v)); v->monitor.vif=false;
    uint64_t start=v->clock_ms; g_ticks+=1000; dosvm_tick();
    CHECK(v->clock_ms==start+1000 && v->pit.dropped_ticks==936 && v->pit.clocks==64*1193 && (v->monitor.pic[0].irr&1));
    destroy(v,true); empty();
}
static void test_device_input(void)
{
    struct dosvm *v=create(), *w=create(); struct dosvm_key key=dosvm_key(v);
    CHECK(dosvm_input_focus(key,true));
    struct ciuki_input_event e={.type=CIUKI_INPUT_KEY,.code=0x1D,.value=1};
    CHECK(dosvm_input_event(key,&e)); e.code=0x11D; CHECK(dosvm_input_event(key,&e));
    CHECK(device_in(v,0x60)==0x1D && device_in(v,0x60)==0xE0 && device_in(v,0x60)==0x1D);
    e.code=0x1E; CHECK(dosvm_input_event(key,&e)); e.value=2; CHECK(dosvm_input_event(key,&e));
    CHECK(v->kbd.count==2); /* host typematic, not duplicate makes */
    e.type=CIUKI_INPUT_TEXT; e.code='a'; CHECK(dosvm_input_event(key,&e) && v->kbd.count==2);
    e=(struct ciuki_input_event){.type=CIUKI_INPUT_BUTTON,.code=1,.value=1}; CHECK(dosvm_input_event(key,&e));
    CHECK(dosvm_input_focus(dosvm_key(w),true)); CHECK(!dosvm_input_event(key,&e));
    CHECK(!v->mouse.buttons && v->kbd.count==4);
    CHECK(device_in(v,0x60)==0x9D && device_in(v,0x60)==0x9E && device_in(v,0x60)==0xE0 && device_in(v,0x60)==0x9D);
    CHECK(dosvm_input_focus(key,true)); e=(struct ciuki_input_event){.type=CIUKI_INPUT_KEY,.code=0x200,.value=1};
    CHECK(dosvm_input_event(key,&e)); const uint8_t pause[]={0xE1,0x1D,0x45,0xE1,0x9D,0xC5};
    for(unsigned i=0;i<6;i++) CHECK(device_in(v,0x60)==pause[i]);
    for(unsigned i=0;i<100;i++) {
        e=(struct ciuki_input_event){.type=CIUKI_INPUT_KEY,.code=0x1E,.value=1}; CHECK(dosvm_input_event(key,&e));
        CHECK(!dosvm_kbd_bios(v) && v->keyboard[v->key_head]==0x1E61);
        struct v86_frame read={0}; CHECK(!dosvm_bios(v,&read,0x16) && (uint16_t)read.tf.eax==0x1E61);
        CHECK(dosvm_input_focus(key,false)); CHECK(device_in(v,0x60)==0x9E); CHECK(dosvm_input_focus(key,true));
    }
    /* Whole-sequence overflow leaves held state and byte count untouched. */
    v->kbd.count=DOSVM_KBD_BYTES-1; e.code=0x11D;
    CHECK(!dosvm_input_event(key,&e) && v->kbd.count==DOSVM_KBD_BYTES-1 && !(v->kbd.held[0x11D>>3]&(1u<<(0x11D&7))));
    CHECK(dosvm_input_focus(key,false)); CHECK(!v->kbd.count);
    device_out(v,0x64,0x20); CHECK(device_in(v,0x64)&1); CHECK(device_in(v,0x60)==0x41);
    device_out(v,0x60,0xED); CHECK(device_in(v,0x60)==0xFA); device_out(v,0x60,7); CHECK(device_in(v,0x60)==0xFA);
    device_out(v,0x64,0xD1); device_out(v,0x60,1); CHECK(!v->a20); device_out(v,0x64,0xD0); CHECK(device_in(v,0x60)==1);
    device_out(v,0x64,0xD1); device_out(v,0x60,3); CHECK(v->a20);
    destroy(v,true); CHECK(!dosvm_input_focus(key,true) && !dosvm_input_event(key,&e)); destroy(w,true); empty();
}
static void test_device_mouse_shadow(void)
{
    struct dosvm *v=create(); struct v86_frame f={0};
    CHECK(!dosvm_mouse_bios(v,&f) && (uint16_t)f.tf.eax==0xFFFF && f.tf.ebx==3);
    f.tf.eax=1; CHECK(!dosvm_mouse_bios(v,&f) && !v->mouse.visible);
    uint16_t before=v->text[1000]; dosvm_mouse_text(v); CHECK(v->text[1000]!=before);
    f.tf.eax=0xB; dosvm_mouse_event(v,17,-9,3); CHECK(!dosvm_mouse_bios(v,&f) && f.tf.ecx==17 && (uint16_t)f.tf.edx==(uint16_t)-9);
    f.tf.eax=3; CHECK(!dosvm_mouse_bios(v,&f) && f.tf.ebx==3 && !(f.tf.ecx&7) && !(f.tf.edx&7));
    f.tf.eax=0xC; f.tf.ecx=0x7F; f.es=0x200; f.tf.edx=0; CHECK(!dosvm_mouse_bios(v,&f));
    uint8_t retf=0xCB; CHECK(!dosvm_write(v,0x2000,&retf,1));
    dosvm_mouse_event(v,1,1,0); v->monitor.vif=false;
    CHECK(!dosvm_mouse_dispatch(v,&f)); v->monitor.vif=true; v->monitor.shadow=true;
    CHECK(!dosvm_mouse_dispatch(v,&f)); v->monitor.shadow=false;
    f.tf.cs=0x100; f.tf.eip=4; f.tf.user_ss=0x300; f.tf.user_esp=0x100; f.tf.eax=0xABCD;
    struct v86_frame saved=f;
    CHECK(dosvm_mouse_dispatch(v,&f)==1 && v->mouse.active && f.tf.cs==0x200 && f.tf.user_esp==0xFC);
    CHECK(!dosvm_mouse_dispatch(v,&f));
    f.tf.cs=0xF000; f.tf.eip=DOSVM_MOUSE_RETURN+2; f.tf.user_esp=0x100;
    CHECK(!dosvm_mouse_return(v,&f) && !memcmp(&f,&saved,sizeof(f)) && v->monitor.vif);
    CHECK(dosvm_mouse_return(v,&f)<0); /* forged continuation */
    const uint8_t code[]={0xFA,0xFB,0xF3,0x6E,0x8E,0xD0,0xE6,0x21};
    CHECK(!dosvm_write(v,0x2000,code,sizeof(code)));
    f=(struct v86_frame){0}; f.tf.cs=0x200; f.tf.user_ss=0x300; f.tf.user_esp=0x100; f.tf.eflags=V86_VM|V86_IF|2;
    CHECK(!v86_emulate(&v->monitor,&f) && !v->monitor.vif && (f.tf.eflags&V86_IF));
    CHECK(!v86_emulate(&v->monitor,&f) && v->monitor.shadow);
    f.tf.edx=0x300; f.tf.ecx=2; f.tf.esi=0; CHECK(!dosvm_write(v,0x3000,"AB",2)); f.ds=0x300;
    CHECK(!v86_emulate(&v->monitor,&f) && v->monitor.shadow && f.tf.eip==2);
    CHECK(!v86_emulate(&v->monitor,&f) && !v->monitor.shadow && f.tf.eip==4);
    v86_prepare_return(&v->monitor,&f); CHECK(v->monitor.shadow && (f.tf.eflags&V86_TF) && v->monitor.shadow_next==6); /* MOV SS */
    /* TCG raises #DB right after MOV SS: the successor keeps the shadow once. */
    f.tf.eip=6; CHECK(!v86_debug_step(&v->monitor,&f) && v->monitor.shadow && v->monitor.shadow_next==4);
    f.tf.eip=6; CHECK(!v86_debug_step(&v->monitor,&f) && !v->monitor.shadow);
    v->monitor.shadow=false; f.tf.eip=4; v86_prepare_return(&v->monitor,&f); CHECK(v->monitor.shadow);
    f.tf.eip=6; f.tf.eax=0xFF; CHECK(!v86_emulate(&v->monitor,&f) && !v->monitor.shadow && f.tf.eip==8);
    /* STI whose successor is MOV SS: the shadow extends over the successor's successor. */
    v->monitor.vif=false; f.tf.eip=1; CHECK(!v86_emulate(&v->monitor,&f) && v->monitor.shadow && f.tf.eip==2 && v->monitor.shadow_next==2);
    v->monitor.shadow=false; v->monitor.vif=false; const uint8_t sti_ss[]={0xFB,0x8E,0xD0,0xE6,0x21};
    CHECK(!dosvm_write(v,0x2100,sti_ss,sizeof(sti_ss))); f.tf.cs=0x210; f.tf.eip=0;
    CHECK(!v86_emulate(&v->monitor,&f) && v->monitor.shadow && f.tf.eip==1 && v->monitor.shadow_next==3);
    f.tf.eip=3; CHECK(!v86_debug_step(&v->monitor,&f) && v->monitor.shadow);
    f.tf.eax=0xFF; CHECK(!v86_emulate(&v->monitor,&f) && !v->monitor.shadow && f.tf.eip==5);
    destroy(v,true); empty();
}
/* f3-01 QEMU stage 5 (2026-10-11): the VMDEV.COM sequence through the
 * production trap handler, v86_emulate, v86_irq_deliver and the ROM stubs.
 * Only trapping guest instructions are executed; each MOV that precedes one is
 * folded into its AX. 8259A p.13: an OCW1 bit of 1 masks its IR line, so the
 * payload's FEh enabled IRQ0 only (the 1 kHz timer storm with IRQ1 queued). */
static bool device_trap(struct dosvm *v, struct v86_frame *f, unsigned vector)
{
    struct task *self=g_current; bool running=true;
    g_current=v->task; v->continuation=0x12345678; f->tf.vector=vector;
    if (!setjmp(guest_return)) CHECK(dosvm_trap(&f->tf)); else running=false; /* VM stopped */
    sti(); g_current=self; v->continuation=0;
    return running;
}
static bool device_insn(struct dosvm *v, struct v86_frame *f, uint32_t ax, const uint8_t *code, unsigned n)
{ CHECK(!dosvm_write(v,(f->tf.cs<<4)+f->tf.eip,code,n)); f->tf.eax=ax; return device_trap(v,f,13); }
static void guest_out(struct dosvm *v, struct v86_frame *f, uint8_t port, uint8_t value)
{ const uint8_t c[]={0xE6,port}; CHECK(device_insn(v,f,value,c,2)); }
static uint8_t guest_in(struct dosvm *v, struct v86_frame *f, uint8_t port)
{ const uint8_t c[]={0xE4,port}; CHECK(device_insn(v,f,0,c,2)); return (uint8_t)f->tf.eax; }
static void guest_int33(struct dosvm *v, struct v86_frame *f, uint16_t ax)
{
    uint32_t cs=f->tf.cs, next=f->tf.eip+2; const uint8_t c[]={0xCD,0x33};
    CHECK(device_insn(v,f,ax,c,2) && f->tf.cs==0xF000 && f->tf.eip==0x330); /* guest IVT -> ROM stub */
    CHECK(device_trap(v,f,13) && f->tf.eip==0x332);                         /* INT FE service */
    CHECK(device_trap(v,f,13) && f->tf.cs==cs && f->tf.eip==next);           /* IRET */
}
struct device_guest { unsigned keys, last_key, isr1, callbacks; };
static void guest_keyboard(struct dosvm *v, struct v86_frame *f, struct device_guest *g)
{
    CHECK(f->tf.cs==0x280 && !f->tf.eip && !v->monitor.vif && v->monitor.stats.last_vector==9);
    g->last_key=guest_in(v,f,0x60); g->keys++;
    guest_out(v,f,0x20,0x0B); g->isr1=guest_in(v,f,0x20); guest_out(v,f,0x20,0x20);
    const uint8_t iret=0xCF; CHECK(device_insn(v,f,0,&iret,1));
}
static void guest_callback(struct dosvm *v, struct v86_frame *f, struct device_guest *g, uint32_t resume)
{
    /* The callback runs with IF clear and returns FAR to the private ROM stub. */
    CHECK(f->tf.cs==0x200 && f->tf.eip==0xF00 && v->mouse.active && !v->monitor.vif);
    uint16_t far[2]; CHECK(!dosvm_read(v,(f->tf.user_ss<<4)+(uint16_t)f->tf.user_esp,far,4) && far[0]==DOSVM_MOUSE_RETURN && far[1]==0xF000);
    g->callbacks++; f->tf.eip=far[0]; f->tf.cs=far[1]; f->tf.user_esp+=4;        /* RETF */
    CHECK(device_trap(v,f,13) && !v->mouse.active);                               /* INT FD continuation */
    /* Back at the interrupted HLT successor with IF restored, or already in a
     * keyboard IRQ that the restored IF made deliverable in the same trap. */
    CHECK((f->tf.cs==0x200 && f->tf.eip==resume && v->monitor.vif) || (f->tf.cs==0x280 && !v->monitor.vif));
}
static void device_payload_setup(struct dosvm *v, struct v86_frame *f)
{
    CHECK(!dosvm_boot(v)); v->info.dos_ready=true;
    const uint16_t ivt[4]={0,0x270,0,0x280}; CHECK(!dosvm_write(v,8*4,ivt,sizeof(ivt)));
    *f=(struct v86_frame){0}; f->tf.cs=0x200; f->tf.user_ss=0x300; f->tf.user_esp=0x100; f->tf.eflags=V86_VM|V86_IF|2;
    const uint8_t cli=0xFA; CHECK(device_insn(v,f,0,&cli,1) && !v->monitor.vif);
    static const uint8_t init[][2]={{0x20,0x11},{0xA0,0x11},{0x21,8},{0xA1,0x70},{0x21,4},{0xA1,2},
        {0x21,1},{0xA1,1},{0x21,0xFC},{0xA1,0xFF},{0x43,0x34},{0x40,1193&255},{0x40,1193>>8}};
    for (unsigned i=0;i<ARRAY_SIZE(init);i++) guest_out(v,f,init[i][0],init[i][1]);
    CHECK(v->monitor.pic[0].base==8 && v->monitor.pic[1].base==0x70 && v->monitor.pic[0].imr==0xFC && v->monitor.pic[1].imr==0xFF);
    guest_out(v,f,0x21,0xFF); task_sleep_ms(5); /* shadow stage: everything masked, IRQ0 pending */
    CHECK((v->monitor.pic[0].irr&1) && v->pit.edges==4 && !v->monitor.stats.reflected_irqs);
    struct dosvm_key key=dosvm_key(v); struct ciuki_input_event e={.type=CIUKI_INPUT_KEY,.code=0x1E,.value=1};
    CHECK(dosvm_input_focus(key,true) && dosvm_input_event(key,&e)); e.value=2; CHECK(dosvm_input_event(key,&e));
    CHECK(v->monitor.pic[0].irr==3 && v->kbd.count==2 && !v->monitor.pic[0].isr);
}
/* OCW1, STI, the 6-byte MOV publishing stage 5 and its single-step boundary. */
static uint32_t guest_unmask(struct dosvm *v, struct v86_frame *f, uint8_t ocw1)
{
    guest_out(v,f,0x21,ocw1);
    const uint8_t sti=0xFB; CHECK(device_insn(v,f,0,&sti,1) && v->monitor.vif && v->monitor.shadow && f->tf.cs==0x200);
    static const uint8_t publish[]={0xC7,0x06,0x02,0x06,0x05,0x00};
    CHECK(!dosvm_write(v,(f->tf.cs<<4)+f->tf.eip,publish,6)); f->tf.eip+=6;
    uint32_t resume=f->tf.eip; CHECK(device_trap(v,f,1)); return resume;
}
static struct dosvm *hook_vm; static unsigned hook_after; static void (*hook_action)(struct dosvm *);
static void device_hook(void) { if (--hook_after) return; sleep_hook=0; hook_action(hook_vm); }
static void device_hook_arm(struct dosvm *v, unsigned after, void (*action)(struct dosvm *))
{ hook_vm=v; hook_after=after; hook_action=action; sleep_hook=device_hook; }
static void device_pointer(struct dosvm *v)
{
    struct dosvm_key key=dosvm_key(v);
    struct ciuki_input_event motion={.type=CIUKI_INPUT_MOTION,.value=10,.value2=10}, button={.type=CIUKI_INPUT_BUTTON,.code=1,.value=1};
    CHECK(dosvm_input_event(key,&motion)); motion.value=27; motion.value2=18;
    CHECK(dosvm_input_event(key,&motion) && dosvm_input_event(key,&button));
}
static void guest_op(struct dosvm *v, struct v86_frame *f, uint8_t op) { CHECK(device_insn(v,f,f->tf.eax,&op,1)); }
static bool hang_halted;
static void device_hang_stop(struct dosvm *v)
{ hang_halted=v->monitor.state==V86_HALTED && v->info.state==DOSVM_BLOCKED; CHECK(!dosvm_stop(dosvm_key(v),DOSVM_TERMINATE)); }
/* .wait_focus as corrected: CLI; INT 33h AX=3; done when no button is down, the
 * 9Eh break was seen and a callback ran; otherwise STI; HLT; JMP .wait_focus. */
static bool guest_focus_check(struct dosvm *v, struct v86_frame *f, struct device_guest *g, uint32_t loop)
{
    f->tf.eip=loop; guest_op(v,f,0xFA); CHECK(!v->monitor.vif);
    f->tf.eip+=3; guest_int33(v,f,3); CHECK(!v->monitor.vif);
    return !(uint16_t)f->tf.ebx && g->last_key==0x9E && g->callbacks;
}
static void test_device_irq_delivery(void)
{
    /* The payload as run on QEMU: IRQ0 is the enabled line, IRQ1 stays queued. */
    struct dosvm *v=create(); struct v86_frame f; device_payload_setup(v,&f);
    guest_unmask(v,&f,0xFE);
    CHECK(f.tf.cs==0x270 && v->monitor.stats.last_vector==8 && v->monitor.stats.delivered[0]==1 && !v->monitor.stats.delivered[1]);
    CHECK(v->monitor.pic[0].isr==1 && (v->monitor.pic[0].irr&2) && v->kbd.count==2);
    destroy(v,true); empty();

    /* FDh with the old ".wait_keys: HLT; CMP; JB" loop, as run on QEMU (a9e3845):
     * both IRQ1s are taken at the boundary after the stage-5 MOV (the STI shadow
     * covers that MOV, not the HLT) and return to the HLT, which then waits for
     * an interrupt that never comes. Same record: AX=FDh, keys=2, IRR 01h, ISR 0. */
    v=create(); device_payload_setup(v,&f); struct device_guest g={0};
    uint32_t resume=guest_unmask(v,&f,0xFD);
    guest_keyboard(v,&f,&g); guest_keyboard(v,&f,&g);
    CHECK(g.keys==2 && f.tf.cs==0x200 && f.tf.eip==resume && v->monitor.vif);
    const uint8_t hlt=0xF4; device_hook_arm(v,200,device_hang_stop);
    CHECK(!device_insn(v,&f,0xFD,&hlt,1) && hang_halted && v->info.reason==DOSVM_TERMINATE);
    CHECK(v->frame.tf.cs==0x200 && v->frame.tf.eip==resume+1 && (uint16_t)v->frame.tf.eax==0xFD && v->frame.tf.user_esp==0x100);
    CHECK(v->monitor.pic[0].irr==1 && !v->monitor.pic[0].isr && v->monitor.pic[0].imr==0xFD &&
          v->monitor.stats.delivered[1]==2 && !v->monitor.stats.delivered[0] && !v->kbd.count);
    destroy(v,true); empty();

    /* Corrected payload, stages 5-7 and the locality fault. */
    struct dosvm *w=create(); v=create(); device_payload_setup(v,&f);
    /* The EMS entry/name and the INT 33h continuation share ROM page F1 without overlap. */
    uint8_t rom[3]; CHECK(!dosvm_read(v,0xF0000+DOSVM_MOUSE_RETURN,rom,3) && rom[0]==0xCD && rom[1]==0xFD && rom[2]==0xF4);
    char ems[8]; CHECK(!dosvm_read(v,0xF100A,ems,8) && !memcmp(ems,"EMMXXXX0",8) && !dosvm_read(v,0xF1000,rom,3) && rom[1]==0xFE);
    g=(struct device_guest){0}; struct dosvm_key key=dosvm_key(v);
    resume=guest_unmask(v,&f,0xFD);
    guest_keyboard(v,&f,&g); /* its IRET delivers the host typematic make at once */
    CHECK(g.last_key==0x1E && g.isr1==2 && f.tf.cs==0x280 && v->monitor.stats.delivered[1]==2);
    guest_keyboard(v,&f,&g);
    CHECK(g.keys==2 && g.last_key==0x1E && g.isr1==2 && f.tf.cs==0x200 && f.tf.eip==resume && v->monitor.vif);
    CHECK(!v->kbd.count && v->monitor.pic[0].irr==1 && !v->monitor.pic[0].isr && !v->monitor.stats.delivered[0]);
    /* .wait_keys: CLI; CMP keys,2; JAE .keys_done; .keys_done: STI; XOR AX,AX. */
    guest_op(v,&f,0xFA); CHECK(!v->monitor.vif && f.tf.eip==resume+1);
    f.tf.eip+=7; guest_op(v,&f,0xFB); CHECK(v->monitor.vif && v->monitor.shadow);
    f.tf.eip+=2; CHECK(device_trap(v,&f,1) && !v->monitor.shadow);
    guest_int33(v,&f,0); CHECK((uint16_t)f.tf.eax==0xFFFF && (uint16_t)f.tf.ebx==3);
    const uint8_t retf=0xCB; CHECK(!dosvm_write(v,0x2F00,&retf,1));
    f.es=0x200; f.tf.ecx=0x7F; f.tf.edx=0xF00; guest_int33(v,&f,0x0C);
    CHECK(v->mouse.mask==0x7F && v->mouse.callback_cs==0x200 && v->mouse.callback_ip==0xF00);
    /* .wait_focus pass 1: nothing yet; STI;HLT blocks across ~50 masked timer
     * edges until the pointer events wake the callback. */
    uint32_t loop=f.tf.eip+6; CHECK(!guest_focus_check(v,&f,&g,loop));
    guest_op(v,&f,0xFB); CHECK(v->monitor.vif && v->monitor.shadow);
    uint64_t edges=v->pit.edges; uint32_t halt=f.tf.eip;
    device_hook_arm(v,50,device_pointer); CHECK(device_insn(v,&f,0,&hlt,1));
    CHECK(!sleep_hook && v->pit.edges>=edges+48 && (v->monitor.pic[0].irr&1) && !v->monitor.stats.delivered[0]);
    CHECK((uint16_t)f.tf.eax==3 && (uint16_t)f.tf.ebx==1 && (uint16_t)f.tf.ecx==336 && (uint16_t)f.tf.edx==96 &&
          (uint16_t)f.tf.esi==17 && (uint16_t)f.tf.edi==8);
    guest_callback(v,&f,&g,halt+1); CHECK(f.tf.cs==0x200);
    /* Pass 2: button held. The focus release becomes pending while IF is clear,
     * before STI;HLT: the STI shadow covers HLT, so the HLT still wakes (the
     * race the old loop lost). The release callback runs first, then IRQ1. */
    CHECK(!guest_focus_check(v,&f,&g,loop) && (uint16_t)f.tf.ebx==1);
    struct dosvm_device_info d; CHECK(dosvm_device_info(key,&d) && d.held_keys==1 && d.buttons==1);
    CHECK(dosvm_input_focus(key,false) && dosvm_device_info(key,&d) && !d.held_keys && !d.buttons && d.queued_bytes==1);
    guest_op(v,&f,0xFB); CHECK(v->monitor.shadow && !v->mouse.active && f.tf.cs==0x200);
    halt=f.tf.eip; CHECK(device_insn(v,&f,0,&hlt,1) && (uint16_t)f.tf.eax==4 && !(uint16_t)f.tf.ebx);
    guest_callback(v,&f,&g,halt+1); CHECK(f.tf.cs==0x280 && v->monitor.stats.delivered[1]==3);
    guest_keyboard(v,&f,&g); CHECK(g.last_key==0x9E && g.keys==3 && f.tf.cs==0x200 && f.tf.eip==halt+1);
    /* Pass 3: done; STI, then stage 7. */
    CHECK(guest_focus_check(v,&f,&g,loop) && g.callbacks==2 && !v->monitor.stats.delivered[0]);
    guest_op(v,&f,0xFB); CHECK(v->monitor.vif);
    /* Malformed wide PIT I/O stops only this VM, with #GP recorded. */
    f.tf.edx=0x40; const uint8_t wide[]={0xEF}; CHECK(!device_insn(v,&f,0x1234,wide,1));
    CHECK(v->info.state==DOSVM_STOPPING && v->info.reason==DOSVM_FAULT && v->info.fault_vector==13);
    CHECK(dosvm_valid(dosvm_key(w)) && w->monitor.pic[0].imr==0xF8 && !w->monitor.pic[0].irr && !w->kbd.count);
    destroy(v,true); destroy(w,true); empty();
}
static void test_devices(void)
{
    test_device_pic(); test_device_pit(); test_device_input(); test_device_mouse_shadow(); test_device_irq_delivery();
    puts("DOS devices: PASS (PIC priorities/cascade/spurious, six PIT modes/BCD/latches/cap, focus/8042/IRQ1, INT33 callback, STI/MOV-SS/string shadow, VMDEV stages 5-7 through dosvm_trap)");
}
