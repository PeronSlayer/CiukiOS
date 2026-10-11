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
    f.tf.cs=0xF000; f.tf.eip=0x1002; f.tf.user_esp=0x100;
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
static void test_devices(void)
{
    test_device_pic(); test_device_pit(); test_device_input(); test_device_mouse_shadow();
    puts("DOS devices: PASS (PIC priorities/cascade/spurious, six PIT modes/BCD/latches/cap, focus/8042/IRQ1, INT33 callback, STI/MOV-SS/string shadow)");
}
