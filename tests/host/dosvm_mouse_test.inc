/* fx-dosmouse: production monitor, INT33 ROM, DPMI translation and callbacks.
 * The test plays non-trapping RETF/IRET instructions, like the DPMI suite.
 * No host callback pointer or shortcut to the INT33 implementation.
 * SPDX-License-Identifier: GPL-2.0-only */
static void mouse_int33_pm(struct dosvm *v, uint16_t data, uint16_t ax,
                           uint16_t bx, uint16_t cx, uint16_t dx, uint16_t es)
{
    rmcs_set(v,ax,bx,0x202,es,0,0,0,0);
    uint8_t s[0x32]; CHECK(!dosvm_read(v,rmcs(),s,sizeof(s)));
    vm_w32(s+0x18,cx); vm_w32(s+0x14,dx);
    CHECK(!dosvm_write(v,rmcs(),s,sizeof(s)));
    cur.tf.es=data; cur.tf.edi=0x200; cur.tf.ebx=0x33; cur.tf.ecx=0;
    int31(v,0x300);
    CHECK(!in_pm() && cur.tf.cs==0xF000 && cur.tf.eip==0x330);
    step(v,13,0); /* INT FE, production INT33 */
    CHECK(!in_pm() && cur.tf.cs==0xF000 && cur.tf.eip==0x332);
    step(v,13,0); /* IRET */
    CHECK(cur.tf.eip==DPMI_RM_RETURN);
    step(v,13,0);
    CHECK(in_pm() && !cf());
}
static void mouse_callback_finish(struct dosvm *v, uint16_t code, uint16_t data, unsigned x, unsigned y)
{
    CHECK(in_pm() && cur.tf.cs==code && cur.tf.eip==0x600 && v->mouse.active && !v->monitor.vif);
    CHECK(cur.tf.es==data && cur.tf.edi==0x300 && cur.tf.user_ss==DPMI_HOST_SS);
    uint32_t at=(DATA_SEG<<4)+0x300;
    uint8_t s[0x32]; CHECK(!dosvm_read(v,at,s,sizeof(s)));
    uint16_t ret[2];
    CHECK(!dosvm_read(v,seg_base(v,(uint16_t)cur.tf.ds)+cur.tf.esi,ret,sizeof(ret)));
    CHECK(ret[0]==DOSVM_MOUSE_RETURN && ret[1]==0xF000);
    CHECK((vm_u32(s+0x1C)&1) && vm_u16(s+0x18)==x && vm_u16(s+0x14)==y);
    /* The client's callback copies the FAR return from DS:ESI into the
     * register structure and pops it, then IRETs (DPMI 0.9 section 11). */
    vm_w16(s+0x2A,ret[0]); vm_w16(s+0x2C,ret[1]); vm_w16(s+0x2E,vm_u16(s+0x2E)+4);
    CHECK(!dosvm_write(v,at,s,sizeof(s)));
    pm_iret(v); at_stub(v,DPMI_PM_CBRET);
    CHECK(!in_pm() && cur.tf.eip==DOSVM_MOUSE_RETURN && v->mouse.active);
    step(v,13,0); /* INT FD, checked mouse return -> suspended PM frame */
    CHECK(in_pm() && !v->mouse.active && !v->mouse.callback_pm && v->monitor.vif);
}
static void test_mouse_dpmi(bool bits32)
{
    struct dosvm *v=dpmi_vm(true);
    v86_start(); enter(v,bits32);
    uint16_t code=cur.tf.cs, data=cur.tf.ds;
    mouse_int33_pm(v,data,0,0,0,0,0);
    CHECK(rmcs16(v,0x1C)==0xFFFF && rmcs16(v,0x10)==3);
    v->vbe_mode=0x101;
    mouse_int33_pm(v,data,7,0,0,639,0);
    mouse_int33_pm(v,data,8,0,0,479,0);
    mouse_int33_pm(v,data,0xF,0,8,8,0);
    mouse_int33_pm(v,data,4,0,317,233,0);
    mouse_int33_pm(v,data,3,0,0,0,0);
    CHECK(rmcs16(v,0x18)==317 && rmcs16(v,0x14)==233);
    cur.tf.ds=code; cur.tf.esi=0x600; cur.tf.es=data; cur.tf.edi=0x300;
    int31(v,0x303); CHECK(!cf());
    uint16_t cbcs=cur.tf.ecx, cbip=cur.tf.edx;
    mouse_int33_pm(v,data,0xC,0,0x7F,cbip,cbcs);
    mouse_int33_pm(v,data,0x14,0,0x7F,cbip,cbcs);
    CHECK(rmcs16(v,0x18)==0x7F && rmcs16(v,0x22)==cbcs && rmcs16(v,0x14)==cbip);
    CHECK(dosvm_input_focus(v->info.key,true));
    struct ciuki_input_event e={.type=CIUKI_INPUT_MOTION,.value=10,.value2=20};
    CHECK(dosvm_input_event(v->info.key,&e) && !v->mouse_captured && v->mouse.x==317);
    struct ciuki_input_event click={.type=CIUKI_INPUT_BUTTON,.code=1,.value=1};
    CHECK(dosvm_input_event(v->info.key,&click) && v->mouse_captured);
    click.value=0; CHECK(dosvm_input_event(v->info.key,&click));
    CHECK(v->mouse.x==10 && v->mouse.y==20 && !v->mouse.motion_x && !v->mouse.motion_y);
    v->mouse.pending_mask=0;
    CHECK(!v->mouse.pending_mask);
    e.value=19; e.value2=25; CHECK(dosvm_input_event(v->info.key,&e));
    CHECK(v->mouse.x==19 && v->mouse.y==25 && v->mouse.motion_x==9 && v->mouse.motion_y==5 && v->mouse.pending_mask==1);
    CHECK(!lean_before(v,&cur) && !lean_after(v,&cur));
    v->monitor.vif=false; CHECK(!dosvm_mouse_dispatch(v,&cur));
    v->monitor.pic[0].imr=v->monitor.pic[1].imr=0xFF;
    v->dpmi.dr7=1; /* leave the client's debugger in control of TF */
    if_recovery_prepare(v,&cur);
    CHECK(v->dpmi.if_waiting && !v->monitor.vif); /* mouse alone is asynchronous work */
    if_recovery_end(v,&cur); v->dpmi.dr7=0;
    v->monitor.vif=true; v->dpmi.shadow=true; CHECK(!dosvm_mouse_dispatch(v,&cur));
    v->dpmi.shadow=false; v->monitor.shadow=true; CHECK(!dosvm_mouse_dispatch(v,&cur));
    v->monitor.shadow=false;
    uint32_t top=v->dpmi.rstack_top; v->dpmi.rstack_top=0;
    CHECK(!dosvm_mouse_dispatch(v,&cur) && v->mouse.pending_mask==1 && !v->mouse.active);
    v->dpmi.rstack_top=top;
    CHECK(wake_condition(v));
    struct v86_frame saved=cur;
    CHECK(dosvm_deliver_irq(v,&cur)==1 && !in_pm() && v->mouse.callback_pm && v->dpmi.depth==1);
    CHECK(cur.tf.cs==cbcs && cur.tf.eip==cbip && cur.tf.ecx==19 && cur.tf.edx==25);
    CHECK(!dosvm_mouse_dispatch(v,&cur)); /* no recursion, even if IF gets set */
    step(v,13,0); /* DPMI real callback stub -> PM user procedure */
    mouse_callback_finish(v,code,data,19,25);
    CHECK(cur.tf.cs==saved.tf.cs && cur.tf.eip==saved.tf.eip && cur.tf.user_ss==saved.tf.user_ss &&
          cur.tf.user_esp==saved.tf.user_esp && cur.tf.eax==saved.tf.eax && cur.tf.ds==saved.tf.ds &&
          cur.tf.es==saved.tf.es && !v->dpmi.depth && v->dpmi.rstack_top==top);
    /* Exclusive full-screen input uses the same INT33/DPMI continuation. */
    fullscreen_owner=v;
    CHECK(dosvm_fullscreen_input(INPUT_REL,INPUT_X,7));
    CHECK(dosvm_fullscreen_input(INPUT_REL,INPUT_Y,3));
    CHECK(dosvm_fullscreen_input(INPUT_BTN,INPUT_RIGHT,1));
    CHECK(v->mouse.x==26 && v->mouse.y==28 && v->mouse.buttons==2);
    CHECK(dosvm_deliver_irq(v,&cur)==1);
    /* A forged depth/SP must not release the suspended protected frame. */
    struct v86_frame fake=cur; fake.tf.eip=DOSVM_MOUSE_RETURN+2; fake.tf.user_esp+=6;
    CHECK(dosvm_mouse_return(v,&fake)==-V86_EPERM && v->mouse.active && v->dpmi.depth==1);
    step(v,13,0); mouse_callback_finish(v,code,data,26,28);
    fullscreen_owner=NULL;
    mouse_int33_pm(v,data,0xC,0,0,0,0);
    /* Warcraft II's 0..1278 virtual columns must reach both window edges. */
    mouse_int33_pm(v,data,7,0,0,1278,0);
    e=(struct ciuki_input_event){.type=CIUKI_INPUT_MOTION,.value=639,.value2=479};
    CHECK(dosvm_input_event(v->info.key,&e) && v->mouse.x==1278 && v->mouse.y==479);
    e.value=0; e.value2=0;
    CHECK(dosvm_input_event(v->info.key,&e) && !v->mouse.x && !v->mouse.y);
    /* DOOM/Wolf/Quake polling profile: range and scaling never clip or
     * consume raw mickeys; a second read clears the counters. */
    mouse_int33_pm(v,data,0,0,0,0,0);
    mouse_int33_pm(v,data,0xF,0,16,32,0);
    dosvm_mouse_event(v,16,-8,1);
    CHECK(v->mouse.x==328 && v->mouse.y==238);
    mouse_int33_pm(v,data,3,0,0,0,0);
    CHECK(rmcs16(v,0x10)==1 && rmcs16(v,0x18)==328 && rmcs16(v,0x14)==238);
    mouse_int33_pm(v,data,0xB,0,0,0,0);
    CHECK(rmcs16(v,0x18)==16 && rmcs16(v,0x14)==(uint16_t)-8);
    mouse_int33_pm(v,data,0xB,0,0,0,0);
    CHECK(!rmcs16(v,0x18) && !rmcs16(v,0x14));
    dosvm_mouse_event(v,INT32_MAX,INT32_MIN,0);
    mouse_int33_pm(v,data,0xB,0,0,0,0);
    CHECK(rmcs16(v,0x18)==32767 && rmcs16(v,0x14)==0x8000);
    CHECK(v->mouse.x==v->mouse.max_x && v->mouse.y==v->mouse.min_y);
    mouse_int33_pm(v,data,1,0,0,0,0);
    mouse_int33_pm(v,data,2,0,0,0,0);
    mouse_int33_pm(v,data,2,0,0,0,0); CHECK(v->mouse.visible==-2);
    mouse_int33_pm(v,data,1,0,0,0,0); CHECK(v->mouse.visible==-1);
    mouse_int33_pm(v,data,1,0,0,0,0); CHECK(v->mouse.visible==0);
    cur.tf.ecx=cbcs; cur.tf.edx=cbip; int31(v,0x304); CHECK(!cf());
    v->vbe_mode=0;
    destroy(v,true); empty();
}
static void test_mouse_geometry(void)
{
    struct dosvm *v=create(); CHECK(!dosvm_boot(v));
    const unsigned modes[]={3,0x13,0x12};
    for (unsigned i=0;i<3;i++) {
        int10(v,modes[i],0,0,0);
        struct v86_frame f=vf(0,0,0,0); CHECK(!dosvm_mouse_bios(v,&f));
        CHECK(v->mouse.max_x==639 && v->mouse.max_y==(i==2 ? 479 : 199));
        dosvm_mouse_absolute(v,0,i==2 ? 0 : 40);
        f=vf(3,0,0,0); CHECK(!dosvm_mouse_bios(v,&f) && !f.tf.ecx && !f.tf.edx);
        dosvm_mouse_absolute(v,639,i==2 ? 479 : 439);
        f=vf(3,0,0,0); CHECK(!dosvm_mouse_bios(v,&f));
        CHECK(f.tf.ecx==(i==0 ? 632 : 639) && f.tf.edx==(i==0 ? 192 : i==1 ? 199 : 479));
        /* Both letterbox borders clamp to the visible image's endpoints. */
        dosvm_mouse_absolute(v,INT32_MIN,INT32_MIN); CHECK(!v->mouse.x && !v->mouse.y);
        dosvm_mouse_absolute(v,INT32_MAX,INT32_MAX);
        CHECK(v->mouse.x==639 && v->mouse.y==v->mouse.max_y);
    }
    int10(v,3,0,0,0); int10(v,0x1112,0,0,0); /* 8x8 ROM font: 80x50 */
    cvga_geometry g; CHECK(cvga_get_geometry(&v->vga,&g) && g.text && g.height/g.char_height==50);
    struct v86_frame f=vf(0x21,0,0,0); CHECK(!dosvm_mouse_bios(v,&f));
    CHECK(v->mouse.max_x==639 && v->mouse.max_y==399 && v->mouse.y==200);
    dosvm_mouse_absolute(v,639,439); f=vf(3,0,0,0);
    CHECK(!dosvm_mouse_bios(v,&f) && f.tf.edx==392);
    v->vbe_mode=0x103; f=vf(0,0,0,0); CHECK(!dosvm_mouse_bios(v,&f));
    CHECK(v->mouse.max_x==799 && v->mouse.max_y==599 && v->mouse.x==400 && v->mouse.y==300);
    dosvm_mouse_absolute(v,639,479); CHECK(v->mouse.x==799 && v->mouse.y==599);
    v->vbe_mode=0; destroy(v,true); empty();
}
static void test_mouse_arguments_state(bool bits32)
{
    struct dosvm *v=dpmi_vm(true); v86_start(); enter(v,bits32);
    uint16_t data=cur.tf.ds;
    v->vbe_mode=0x101; mouse_int33_pm(v,data,0,0,0,0,0);
    CHECK(v->mouse.max_y==479 && v->mouse.y==240);
    mouse_int33_pm(v,data,7,0,600,100,0);
    mouse_int33_pm(v,data,8,0,400,40,0);
    CHECK(v->mouse.min_x==100 && v->mouse.max_x==600 && v->mouse.min_y==40 && v->mouse.max_y==400);
    mouse_int33_pm(v,data,4,0,0xFFFF,0,0); CHECK(v->mouse.x==600 && v->mouse.y==40);
    mouse_int33_pm(v,data,0xC,0,1,0,0x200);
    mouse_int33_pm(v,data,0xC,0,1,DPMI_RM_CALLBACK+4,0xF000); /* unallocated thunk */
    mouse_int33_pm(v,data,0x14,0,0x100,0,0x200); /* invalid event mask */
    CHECK(v->mouse.mask==1 && v->mouse.callback_cs==0x200 && !v->mouse.callback_ip);
    mouse_int33_pm(v,data,0xA,2,0,0,0); CHECK(!v->mouse.text_type && v->mouse.screen_mask==0xFFFF);
    mouse_int33_pm(v,data,0xF,0,0,5,0); CHECK(v->mouse.ratio_x==8 && v->mouse.ratio_y==16);
    mouse_int33_pm(v,data,0xF,0,8,0,0); CHECK(v->mouse.ratio_x==8 && v->mouse.ratio_y==16);
    mouse_int33_pm(v,data,0x15,0x2DC4,0,0,0);
    unsigned bytes=rmcs16(v,0x10); CHECK(bytes==sizeof(struct mouse_state) && bytes<1024);
    mouse_int33_pm(v,data,0xF,0,12,24,0);
    mouse_int33_pm(v,data,1,0,0,0,0);
    dosvm_mouse_event(v,-12,24,1); dosvm_mouse_wheel(v,2);
    v->mouse.pending_mask=0; /* inspect/save settings without running the dummy handler */
    struct dosvm_mouse saved=v->mouse;
    uint8_t guards[1026]; memset(guards,0xA5,sizeof(guards));
    CHECK(!dosvm_write(v,(DATA_SEG<<4)+0x7FF,guards,bytes+2));
    mouse_int33_pm(v,data,0x16,0,0,0x800,DATA_SEG); /* Microsoft ignores BX */
    CHECK(!dosvm_read(v,(DATA_SEG<<4)+0x7FF,guards,bytes+2) && guards[0]==0xA5 && guards[bytes+1]==0xA5);
    mouse_int33_pm(v,data,0,0,0,0,0);
    CHECK(!v->mouse.mask && v->mouse.visible==-1 && v->mouse.max_y==479);
    mouse_int33_pm(v,data,0x17,0,0,0x800,DATA_SEG);
    CHECK(v->mouse.x==saved.x && v->mouse.y==saved.y && v->mouse.ratio_x==12 && v->mouse.ratio_y==24 &&
          v->mouse.max_x==600 && v->mouse.min_y==40 && v->mouse.visible==0 && v->mouse.mask==1 &&
          v->mouse.pressed[0]==1 && v->mouse.wheel==2 && !v->mouse.pending_mask && !v->mouse.active);
    struct dosvm_mouse before=v->mouse;
    mouse_int33_pm(v,data,0x16,0,0,0,0xF000); /* ROM cannot be written */
    mouse_int33_pm(v,data,0x17,0,0,0xFFFF,0xFFFF); /* buffer crosses the end of HMA */
    uint8_t bad[sizeof(struct mouse_state)]; memset(bad,0xFF,sizeof(bad));
    CHECK(!dosvm_write(v,(DATA_SEG<<4)+0x900,bad,sizeof(bad)));
    mouse_int33_pm(v,data,0x17,0,0,0x900,DATA_SEG); /* forged header */
    CHECK(!memcmp(&v->mouse,&before,sizeof(before)));
    struct mouse_state corrupt; CHECK(!dosvm_read(v,(DATA_SEG<<4)+0x800,&corrupt,sizeof(corrupt)));
    struct dosvm_mouse invalid={0}; memcpy(&invalid,corrupt.settings,MOUSE_SETTINGS_BYTES);
    invalid.ratio_x=0; memcpy(corrupt.settings,&invalid,MOUSE_SETTINGS_BYTES);
    CHECK(!dosvm_write(v,(DATA_SEG<<4)+0x900,&corrupt,sizeof(corrupt)));
    mouse_int33_pm(v,data,0x17,0,0,0x900,DATA_SEG); /* valid header, invalid ratio */
    CHECK(!memcmp(&v->mouse,&before,sizeof(before)) && v->info.state!=DOSVM_STOPPING);
    v->vbe_mode=0; destroy(v,true); empty();
}
static void test_mouse_lifetime(bool bits32, unsigned action)
{
    struct dosvm *v=dpmi_vm(true); v86_start(); enter(v,bits32);
    uint16_t code=cur.tf.cs, data=cur.tf.ds;
    v->vbe_mode=0x101;
    mouse_int33_pm(v,data,0,0,0,0,0);
    cur.tf.ds=code; cur.tf.esi=0x600; cur.tf.es=data; cur.tf.edi=0x300;
    int31(v,0x303); CHECK(!cf());
    uint16_t cs=cur.tf.ecx, ip=cur.tf.edx;
    mouse_int33_pm(v,data,0xC,0,1,ip,cs);
    mouse_int33_pm(v,data,0x16,0,0,0x800,DATA_SEG);
    dosvm_mouse_event(v,1,0,0);
    if (action>=2) {
        CHECK(dosvm_deliver_irq(v,&cur)==1 && v->mouse.active);
        step(v,13,0); CHECK(in_pm() && v->dpmi.depth==2);
    }
    if (!action) {
        cur.tf.ecx=cs; cur.tf.edx=ip; int31(v,0x304); CHECK(!cf());
        CHECK(!v->mouse.mask && !v->mouse.pending_mask);
        /* Even a valid old state blob cannot resurrect a freed callback. */
        mouse_int33_pm(v,data,0x17,0,0,0x800,DATA_SEG); CHECK(!v->mouse.mask);
        dosvm_mouse_event(v,1,1,0); CHECK(!dosvm_mouse_dispatch(v,&cur));
    } else if (action==3) {
        /* Reset called by the callback, through nested production 0300h. */
        mouse_int33_pm(v,data,0,0,0,0,0);
        CHECK(!v->mouse.mask && !v->mouse.pending_mask && v->mouse.active && v->mouse.callback_pm);
        /* Play the handler restoring its structure pointer and jumping to
         * its IRET epilogue after the nested service call. */
        cur.tf.es=data; cur.tf.edi=0x300; cur.tf.eip=0x600;
        mouse_callback_finish(v,code,data,321,240);
        CHECK(!v->dpmi.depth && !v->mouse.active);
    } else {
        /* Ordinary exit, and exit from a stuck callback, both reach the
         * production client cleanup. DOS then returns to its prompt. */
        cur.tf.eax=0x4C00; pm_int(v,0x21);
        CHECK(!in_pm() && !v->dpmi.clients && !v->mouse.mask && !v->mouse.pending_mask &&
              !v->mouse.active && !v->mouse.callback_pm);
        dos_service(v); v86_start();
        rom_int(v,0x33); /* reset at the prompt must not fault */
        dosvm_mouse_event(v,1,1,0); v->monitor.vif=true;
        CHECK(!dosvm_mouse_dispatch(v,&cur) && v->info.state!=DOSVM_STOPPING);
    }
    v->vbe_mode=0; destroy(v,true); empty();
}
static void test_mouse_capture(void)
{
    struct dosvm *v=create(); struct dosvm_key key=dosvm_key(v);
    CHECK(dosvm_input_focus(key,true));
    struct ciuki_input_event move={.type=CIUKI_INPUT_MOTION,.value=100,.value2=100};
    struct ciuki_input_event click={.type=CIUKI_INPUT_BUTTON,.code=1,.value=1};
    struct ciuki_input_event wheel={.type=CIUKI_INPUT_WHEEL,.value=1};
    for (unsigned round=0;round<5;round++) {
        int32_t x=v->mouse.x, y=v->mouse.y;
        CHECK(dosvm_input_event(key,&move) && dosvm_input_event(key,&wheel));
        CHECK(!v->mouse_captured && v->mouse.x==x && v->mouse.y==y && !v->mouse.motion_x && !v->mouse.wheel);
        CHECK(dosvm_input_event(key,&click) && v->mouse_captured && v->mouse.buttons==1);
        CHECK(dosvm_input_focus(key,true) && v->mouse_captured && v->pointer_known);
        move.value+=10; CHECK(dosvm_input_event(key,&move) && v->mouse.motion_x==10);
        struct v86_frame f=vf(0xB,0,0,0); CHECK(!dosvm_mouse_bios(v,&f));
        if (round<3) {
            /* Both Ctrl+Alt orders, then Alt+Tab, including right modifiers. */
            struct ciuki_input_event k={.type=CIUKI_INPUT_KEY,.code=round==0 ? 0x1D : round==1 ? 0x138 : 0x38,.value=1};
            CHECK(dosvm_input_event(key,&k));
            k.code=round==0 ? 0x38 : round==1 ? 0x11D : 0x0F;
            CHECK(dosvm_input_event(key,&k));
        } else if (round==3) {
            CHECK(dosvm_input_focus(key,false)); CHECK(dosvm_input_focus(key,true));
        } else {
            struct ciuki_input_event sync={.type=CIUKI_INPUT_RESYNC}; CHECK(dosvm_input_event(key,&sync));
        }
        CHECK(!v->mouse_captured && !v->mouse.buttons && !v->pointer_known);
        dosvm_kbd_release(v);
    }
    /* A click outside the source, or without a known position, cannot capture. */
    CHECK(dosvm_input_event(key,&click) && !v->mouse_captured);
    move.value=-1; CHECK(dosvm_input_event(key,&move) && dosvm_input_event(key,&click) && !v->mouse_captured);
    fullscreen_owner=v;
    CHECK(dosvm_fullscreen_input(INPUT_REL,INPUT_X,12) && v->mouse.motion_x==12);
    CHECK(dosvm_fullscreen_input(INPUT_BTN,INPUT_LEFT,1) && v->mouse.buttons==1 && !v->mouse_captured);
    fullscreen_owner=NULL; destroy(v,true); empty();
}
static void test_dos_mouse(void)
{
    struct dosvm *v=create();
    /* Model DOS's setup_int_vectors, then exercise the real ready record. */
    uint16_t entry[2]={0x80,0x200}; uint8_t op=0xCF;
    CHECK(!dosvm_write(v,0x2080,&op,1) && !dosvm_write(v,0x33*4,entry,4));
    line(v,"DOSREADY 07.0A free=8E95 C\r");
    CHECK(v->info.dos_ready && !dosvm_read(v,0x33*4,entry,4) && entry[0]==0x330 && entry[1]==0xF000);
    /* Neither a later ready record nor the installer replaces a real TSR. */
    op=0x90; entry[0]=0x80; entry[1]=0x200;
    CHECK(!dosvm_write(v,0x2080,&op,1) && !dosvm_write(v,0x33*4,entry,4));
    line(v,"DOSREADY 07.0A free=8E95 C\r");
    CHECK(!dosvm_mouse_install(v) && !dosvm_read(v,0x33*4,entry,4) && entry[0]==0x80 && entry[1]==0x200);
    memset(entry,0,sizeof(entry)); CHECK(!dosvm_write(v,0x33*4,entry,4));
    CHECK(!dosvm_mouse_install(v) && !dosvm_read(v,0x33*4,entry,4) && entry[1]==0xF000);
    destroy(v,true); empty();
    test_mouse_dpmi(false); test_mouse_dpmi(true);
    test_mouse_geometry(); test_mouse_capture();
    for (unsigned bits32=0;bits32<2;bits32++) {
        test_mouse_arguments_state(bits32);
        for (unsigned action=0;action<4;action++) test_mouse_lifetime(bits32,action);
    }
    puts("PASS fx-dosmouse: 16/32-bit DPMI 0300 INT33, 0303 callbacks, 0C/14 exchange, VBE precision, window/full-screen delivery, shadows, exhaustion, checked return");
}
