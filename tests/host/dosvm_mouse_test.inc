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
static void mouse_callback_finish(struct dosvm *v, uint16_t code, uint16_t data)
{
    CHECK(in_pm() && cur.tf.cs==code && cur.tf.eip==0x600 && v->mouse.active && !v->monitor.vif);
    CHECK(cur.tf.es==data && cur.tf.edi==0x300 && cur.tf.user_ss==DPMI_HOST_SS);
    uint32_t at=(DATA_SEG<<4)+0x300;
    uint8_t s[0x32]; CHECK(!dosvm_read(v,at,s,sizeof(s)));
    uint16_t ret[2];
    CHECK(!dosvm_read(v,seg_base(v,(uint16_t)cur.tf.ds)+cur.tf.esi,ret,sizeof(ret)));
    CHECK(ret[0]==DOSVM_MOUSE_RETURN && ret[1]==0xF000);
    CHECK((vm_u32(s+0x1C)&1) && vm_u16(s+0x18)==v->mouse.x && vm_u16(s+0x14)==v->mouse.y);
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
    CHECK(dosvm_input_event(v->info.key,&e)); /* first sample places the pointer, no raw motion */
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
    mouse_callback_finish(v,code,data);
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
    step(v,13,0); mouse_callback_finish(v,code,data);
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
    CHECK(v->mouse.x==328 && v->mouse.y==94);
    mouse_int33_pm(v,data,3,0,0,0,0);
    CHECK(rmcs16(v,0x10)==1 && rmcs16(v,0x18)==328 && rmcs16(v,0x14)==94);
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
    puts("PASS fx-dosmouse: 16/32-bit DPMI 0300 INT33, 0303 callbacks, 0C/14 exchange, VBE precision, window/full-screen delivery, shadows, exhaustion, checked return");
}
