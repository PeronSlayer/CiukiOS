/* G17 real owner-bound syscall, VGA text, background and wheel state. */
static struct dosvm *g17_held;
static void g17_unmark(void)
{
    CHECK(g17_held); g17_held->window_marking=false;
}
static void test_dos_properties(void)
{
    window_fixtures();
    struct ciuki_dosvm_command w,c;
    CHECK(!create_window(0,"Custom DOS program",&w));
    struct dosvm_key key={w.id,w.generation}; struct dosvm *v=dosvm_lookup(key); CHECK(v);
    CHECK(dosvm_input_focus(key,true));
    c=command_for(w.id,w.generation); c.policy.marking=2;
    CHECK(dosvm_call(CIUKI_DOSVM_POLICY,&c,sizeof(c))==-EINVAL);
    c=command_for(w.id,w.generation); c.policy.idle_sensitivity=101;
    CHECK(dosvm_call(CIUKI_DOSVM_POLICY,&c,sizeof(c))==-EINVAL);
    c=command_for(w.id,w.generation); c.policy.reserved[7]=1;
    CHECK(dosvm_call(CIUKI_DOSVM_POLICY,&c,sizeof(c))==-EINVAL);
    c=command_for(w.id,w.generation); fake_current=&fake_proc[1];
    CHECK(dosvm_call(CIUKI_DOSVM_POLICY,&c,sizeof(c))==-ESRCH);
    CHECK(dosvm_call(CIUKI_DOSVM_TEXT_CHUNK,&c,sizeof(c))==-ESRCH);
    fake_current=&fake_proc[0];
    c=command_for(w.id,w.generation); c.policy.marking=1; c.policy.idle_sensitivity=100;
    CHECK(!dosvm_call(CIUKI_DOSVM_POLICY,&c,sizeof(c)) && v->window_marking && v->window_idle==101 && !dosvm_task_focus());
    CHECK(!park_released(v)); g17_held=v; sleep_hook=g17_unmark;
    dosvm_fullscreen_checkpoint(v); CHECK(!v->window_marking && !v->fullscreen_parked); sleep_hook=NULL;
    CHECK(cvga_set_bios_mode(&v->vga,3,0));
    /* The CRTC start and pitch determine the copied cells, not B800:0000. */
    v->vga.crtc[12]=0; v->vga.crtc[13]=32;
    for (unsigned i=0;i<80;i++) v->vga.plane[0][(32+80+i)*2]=(uint8_t)('A'+i%26);
    c=command_for(w.id,w.generation); c.text_chunk.row=1; c.text_chunk.column=48;
    CHECK(!dosvm_call(CIUKI_DOSVM_TEXT_CHUNK,&c,sizeof(c)) && c.text_chunk.columns==80 && c.text_chunk.rows==25);
    CHECK(c.text_chunk.characters[0]=='A'+48%26 && c.text_chunk.characters[31]=='A'+79%26 && !c.text_chunk.characters[32]);
    c=command_for(w.id,w.generation); c.text_chunk.columns=80;
    CHECK(dosvm_call(CIUKI_DOSVM_TEXT_CHUNK,&c,sizeof(c))==-EINVAL);
    c=command_for(w.id,w.generation); c.text_chunk.row=100;
    CHECK(dosvm_call(CIUKI_DOSVM_TEXT_CHUNK,&c,sizeof(c))==-EINVAL);
    CHECK(cvga_set_bios_mode(&v->vga,0x13,0));
    c=command_for(w.id,w.generation); CHECK(dosvm_call(CIUKI_DOSVM_TEXT_CHUNK,&c,sizeof(c))==-EOPNOTSUPP);
    v->launch_returned=true;
    c=command_for(w.id,w.generation); CHECK(!dosvm_call(CIUKI_DOSVM_STATUS,&c,sizeof(c)) && (c.changed&CIUKI_DOSVM_PROGRAM_RETURNED));
    v->exec_pending=1;
    c=command_for(w.id,w.generation); CHECK(!dosvm_call(CIUKI_DOSVM_STATUS,&c,sizeof(c)) && !(c.changed&CIUKI_DOSVM_PROGRAM_RETURNED));
    v->exec_pending=0; v->launch_returned=false;
    c=command_for(w.id,w.generation); c.focused=1;
    CHECK(!dosvm_call(CIUKI_DOSVM_FOCUS,&c,sizeof(c)));
    c=command_for(w.id,w.generation); c.focused=1;
    CHECK(!dosvm_call(CIUKI_DOSVM_BACKGROUND,&c,sizeof(c)) && !v->fullscreen_suspended);
    c=command_for(w.id,w.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_FOCUS,&c,sizeof(c)) && v->fullscreen_suspended && !park_released(v));
    c=command_for(w.id,w.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_BACKGROUND,&c,sizeof(c)) && !v->fullscreen_suspended && park_released(v));
    c=command_for(w.id,w.generation); c.focused=1; CHECK(!dosvm_call(CIUKI_DOSVM_FOCUS,&c,sizeof(c)));
    c=command_for(w.id,w.generation); c.event.type=CIUKI_INPUT_WHEEL; c.event.value=-3;
    CHECK(!dosvm_call(CIUKI_DOSVM_INPUT,&c,sizeof(c)) && v->mouse.wheel==3);
    c=command_for(w.id,w.generation); c.event.type=CIUKI_INPUT_WHEEL; c.event.value=128;
    CHECK(dosvm_call(CIUKI_DOSVM_INPUT,&c,sizeof(c))==-EINVAL && v->mouse.wheel==3);
    struct v86_frame f=vf(0x11,0,0,0); CHECK(!dosvm_mouse_bios(v,&f) && (uint16_t)f.tf.eax==0x574D && (uint16_t)f.tf.ecx==1);
    f=vf(3,0,0,0); CHECK(!dosvm_mouse_bios(v,&f) && ((f.tf.ebx>>8)&255)==3 && !v->mouse.wheel);
    dosvm_mouse_wheel(v,-4); f=vf(5,0xFFFF,0,0);
    CHECK(!dosvm_mouse_bios(v,&f) && (int16_t)f.tf.ebx==-4 && (int8_t)(f.tf.eax>>8)==-4 && !v->mouse.wheel);
    dosvm_mouse_wheel(v,40000); f=vf(6,0xFFFF,0,0);
    CHECK(!dosvm_mouse_bios(v,&f) && (int16_t)f.tf.ebx==32767 && (int8_t)(f.tf.eax>>8)==127);
    f=vf(0xC,0,0x80,0); f.es=0x200;
    CHECK(!dosvm_mouse_bios(v,&f) && v->mouse.mask==0x80);
    dosvm_mouse_wheel(v,-2); CHECK(v->mouse.pending_mask==0x80 && v->mouse.callback_wheel==-2);
    f=vf(3,0,0,0); CHECK(!dosvm_mouse_bios(v,&f) && !v->mouse.wheel && v->mouse.callback_wheel==-2);
    /* A hardware callback is a guest continuation, with a separate counter. */
    f=vf(0,0,0,0); f.tf.user_ss=0x300; f.tf.user_esp=0x100; v->monitor.vif=true;
    CHECK(dosvm_mouse_dispatch(v,&f)==1 && (f.tf.eax&0x80) && (int8_t)(f.tf.ebx>>8)==-2 && !v->mouse.callback_wheel);
    v->mouse.active=false;
    f=vf(0x21,0,0,0); CHECK(!dosvm_mouse_bios(v,&f) && !v->mouse.wheel && !v->mouse.callback_wheel);
    fullscreen_owner=v;
    CHECK(dosvm_fullscreen_input(INPUT_REL,INPUT_WHEEL,-2) && v->mouse.wheel==2);
    fullscreen_owner=NULL; dosvm_mouse_init(v);
    c=command_for(w.id,w.generation); v->info.state=DOSVM_STOPPING;
    CHECK(!dosvm_call(CIUKI_DOSVM_CLOSE,&c,sizeof(c))); empty();
    const unsigned sensitivities[]={0,50,100},thresholds[]={16,8,1};
    for (unsigned n=0;n<3;n++) {
        v=idle_test_vm(); v86_start(); v->monitor.pic[0].imr=0xFF;
        v->window_idle=(uint8_t)(sensitivities[n]+1);
        for (unsigned i=0;i<thresholds[n];i++) {
            if (i+1==thresholds[n]) { idle_action=3; idle_at=g_ticks+2; sleep_hook=idle_test_event; }
            cur.tf.eax=0x0100; rom_int(v,0x16);
            CHECK(v->idle.blocks==(i+1==thresholds[n] ? 1u : 0u));
        }
        destroy(v,true); empty();
    }
    puts("PASS G17 VM policy/text: ownership, zero fields, bounds, safe-point pause, CRTC start/pitch, graphics refusal, window background suspend/resume, wheel polling/callback/reset/saturation");
    puts("PASS G17 idle sensitivity: 0/50/100 detect 16/8/1 empty INT 16h polls");
}
