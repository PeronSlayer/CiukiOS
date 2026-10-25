/* Stateful hardware bus, plus the real ownership service and syscall adapter. */
static bool fs_slow_aperture;
static unsigned fs_aperture_bytes;
static void card_access(void) {
    if (fs_slow_aperture && !(++fs_aperture_bytes%500)) g_ticks++; /* 2 us/byte */
}
static uint8_t card_read(void *arg,unsigned a) { card_access();return cvga_read_vram(arg,0xA0000+a); }
static void card_write(void *arg,unsigned a,uint8_t b) { card_access();cvga_write_vram(arg,0xA0000+a,b); }
static uint8_t card_in(void *arg,uint16_t p) { return cvga_read_port(arg,p,0); }
static void card_out(void *arg,uint16_t p,uint8_t b) { cvga_write_port(arg,p,b); }
static struct vga_hardware_bus card_bus={&fs_card,card_in,card_out,card_read,card_write};
static struct dosvm *fs_waiting_vm;
static uint64_t fs_park_tick;
static void delayed_fullscreen_park(void) {
    if (g_ticks>=fs_park_tick) fs_waiting_vm->fullscreen_parked=true;
}
/* Guest register tweaks, not provider presets. 13h has no unique BIOS
 * number for these rasters; preserve the entire programmed state. */
static void tweak_mode(cvga_state *g,unsigned width,unsigned height,bool chain)
{
    CHECK(cvga_set_bios_mode(g,0x13,0));
    if (height==240 || height==480) {
        const cvga_mode_params *v=cvga_find_mode(0x12);
        const unsigned vertical[]={6,7,9,0x10,0x11,0x12,0x15,0x16};
        for (unsigned i=0;i<ARRAY_SIZE(vertical);i++) g->crtc[vertical[i]]=v->crtc[vertical[i]];
    }
    g->crtc[9]=(g->crtc[9]&0xE0)|(height<=240 ? 1 : 0);
    if (width==360) {
        g->misc=0x67;
        g->crtc[0]=0x6B;g->crtc[1]=0x59;g->crtc[2]=0x5A;
        g->crtc[3]=0x8E;g->crtc[4]=0x5E;g->crtc[5]=0x8A;
        g->crtc[0x13]=45;
    }
    g->seq[4]=chain ? 0x0E : 6;
    g->crtc[0x14]=chain ? 0x40 : 0;
    g->crtc[0x17]=chain ? 0xA3 : 0xE3;
    g->display_changes++;
}
static cvga_state fs_saved,fs_captured;
static void test_fullscreen_transport(void)
{
    const unsigned modes[]={1,3,0x12,0x13};
    for (unsigned n=0;n<ARRAY_SIZE(modes);n++) {
        cvga_init(&fs_saved); CHECK(cvga_set_bios_mode(&fs_saved,modes[n],0));
        for (unsigned p=0;p<4;p++) for (unsigned a=0;a<CVGA_PLANE_SIZE;a++)
            fs_saved.plane[p][a]=(a*17u+(a>>8)*29u+p*37u)&255;
        for (unsigned i=0;i<256;i++) for (unsigned k=0;k<3;k++) fs_saved.dac[i][k]=(i*3+k*19)&63;
        fs_saved.latch[0]=0x12; fs_saved.latch[1]=0x67; fs_saved.latch[2]=0x91; fs_saved.latch[3]=0xAB;
        fs_saved.dac_index=37; fs_saved.dac_component=2;
        fs_saved.seq_index=2; fs_saved.gc_index=4; fs_saved.crtc_index=12;
        fs_saved.attr_index=0x32; fs_saved.attr_data_phase=1;
        cvga_init(&fs_card);
        vga_hardware_load(&fs_saved,&card_bus);
        CHECK(!vga_hardware_verify(&fs_saved,&card_bus));
        fs_card.seq[1]^=0x20;
        CHECK(vga_hardware_verify(&fs_saved,&card_bus)==2);
        fs_card.seq[1]^=0x20;
        fs_card.gc[6]^=1;
        CHECK(vga_hardware_verify(&fs_saved,&card_bus)==4);
        fs_card.gc[6]^=1;
        CHECK(!memcmp(fs_saved.plane,fs_card.plane,sizeof(fs_card.plane)));
        CHECK(!memcmp(fs_saved.seq,fs_card.seq,sizeof(fs_card.seq)));
        CHECK(!memcmp(fs_saved.gc,fs_card.gc,sizeof(fs_card.gc)));
        CHECK(!memcmp(fs_saved.crtc,fs_card.crtc,sizeof(fs_card.crtc)));
        CHECK(!memcmp(fs_saved.attr,fs_card.attr,sizeof(fs_card.attr)));
        CHECK(!memcmp(fs_saved.dac,fs_card.dac,sizeof(fs_card.dac)));
        CHECK(!memcmp(fs_saved.latch,fs_card.latch,4));
        CHECK(fs_card.dac_component==2 && fs_card.dac_index==37);
        vga_hardware_capture(&fs_captured,&card_bus);
        CHECK(!memcmp(fs_saved.plane,fs_captured.plane,sizeof(fs_card.plane)));
        CHECK(!memcmp(fs_saved.plane,fs_card.plane,sizeof(fs_card.plane)));
        CHECK(!memcmp(fs_saved.seq,fs_captured.seq,sizeof(fs_card.seq)));
        CHECK(!memcmp(fs_saved.gc,fs_captured.gc,sizeof(fs_card.gc)));
        CHECK(!memcmp(fs_saved.crtc,fs_captured.crtc,sizeof(fs_card.crtc)));
        CHECK(!memcmp(fs_saved.attr,fs_captured.attr,sizeof(fs_card.attr)));
        CHECK(!memcmp(fs_saved.dac,fs_captured.dac,sizeof(fs_card.dac)));
        CHECK(fs_captured.seq_index==fs_saved.seq_index && fs_captured.gc_index==fs_saved.gc_index &&
              fs_captured.crtc_index==fs_saved.crtc_index && fs_captured.attr_index==fs_saved.attr_index);
    }
    puts("PASS full-screen VGA transport: text 01/03, planar 12, chain-4 13; all registers, DAC and 256 KiB planes; load latches/partial DAC; non-destructive capture");
}
static void test_fullscreen_service(void)
{
    window_fixtures();
    memset(fs_bitmap,0xFF,sizeof(fs_bitmap));
    uint32_t old_flags=g_boot.flags,old_length=g_boot.test_request_len;
    char old_request[sizeof(g_boot.test_request)]; memcpy(old_request,g_boot.test_request,sizeof(old_request));
    static const char request[]="f3:vm-fullscreen run=12345678";
    g_boot.flags|=CBI_F_TEST_REQUEST; g_boot.test_request_len=sizeof(request)-1;
    memcpy(g_boot.test_request,request,sizeof(request));
    struct ciuki_dosvm_command w;
    CHECK(!create_window(0,"Fullscreen",&w));
    struct dosvm_key key={w.id,w.generation}; struct dosvm *v=dosvm_lookup(key);
    CHECK(!strcmp(v->probe,"vm-fullscreen") && strstr(last_record,"probe=vm-fullscreen "));
    CHECK(dosvm_input_focus(key,true));
    /* The fake CPU is parked at its host boundary, rather than executing
     * hardware VGA cycles. All service/PTE/owner operations are production. */
    v->fullscreen_parked=true;
    int10(v,0x0013,0,0,0);
    dosvm_vga_sync(v,true);
    uint32_t saved[32]; for (unsigned i=0;i<32;i++) as_lookup(&v->as,0xA0000+i*4096,&saved[i]);
    fs_refuse=true; CHECK(dosvm_fullscreen_enter(key)==DISPLAY_FULLSCREEN_WINDOWED && !v->fullscreen && !v->fullscreen_hold && !v->fullscreen_error);
    struct ciuki_dosvm_command attempt=command_for(key.id,key.generation);attempt.focused=1;
    CHECK(!dosvm_call(CIUKI_DOSVM_FOCUS,&attempt,sizeof(attempt)));
    attempt=command_for(key.id,key.generation);attempt.focused=1;
    CHECK(!dosvm_call(CIUKI_DOSVM_FULLSCREEN,&attempt,sizeof(attempt)) && !v->fullscreen && !v->fullscreen_error);
    struct ciuki_dosvm_command refused=command_for(key.id,key.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_STATUS,&refused,sizeof(refused)) &&
          !(refused.changed & CIUKI_DOSVM_FULLSCREEN_REFUSED));
    fs_refuse=false;
    uint64_t retry_tick=g_ticks;
    g_ticks+=50;
    CHECK(!dosvm_fullscreen_enter(key) && v->fullscreen && !v->fullscreen_error);
    CHECK(g_ticks-retry_tick<100);
    CHECK(!dosvm_fullscreen_leave(key,false) && !v->fullscreen && !v->fullscreen_hold);
    CHECK(!v->fullscreen_error && !fs_ports);
    fs_test_checkpoint_pin=true;
    fs_fail_checkpoint="vm-park";
    unsigned before=fs_begins;
    CHECK(dosvm_fullscreen_enter(key)==DISPLAY_FULLSCREEN_WINDOWED && !v->fullscreen && !v->fullscreen_hold && fs_begins==before);
    fs_fail_checkpoint="guest-grant";
    CHECK(dosvm_fullscreen_enter(key)==DISPLAY_FULLSCREEN_WINDOWED && !fs_display && !v->fullscreen && !v->fullscreen_hold);
    for (unsigned i=0;i<32;i++) { uint32_t pte;as_lookup(&v->as,0xA0000+i*4096,&pte);CHECK((pte&~0x60u)==(saved[i]&~0x60u)); }
    fs_fail_checkpoint=0;
    fs_stuck_blank=true;
    CHECK(dosvm_fullscreen_enter(key)==DISPLAY_FULLSCREEN_WINDOWED && !fs_display && !v->fullscreen && !v->fullscreen_hold);
    fs_stuck_blank=false;
    CHECK(!v->fullscreen_error && !fullscreen_owner && !fs_ports);
    /* A failure to restore after partial entry must still reach the owner. */
    fs_fail_checkpoint="guest-grant";fs_end_result=-EIO;
    CHECK(dosvm_fullscreen_enter(key)==-EIO && v->fullscreen_error==-EIO && !v->fullscreen_hold);
    fs_fail_checkpoint=0;fs_end_result=0;
    CHECK(!dosvm_fullscreen_enter(key) && v->fullscreen && fs_display);
    hardware=card_bus; vga_hardware_load(&v->vga,&hardware);
    for (unsigned i=0;i<32;i++) {
        uint32_t pte; CHECK(as_lookup(&v->as,0xA0000+i*4096,&pte)==0xA0000+i*4096);
        CHECK((pte&(PTE_U|PTE_W|PTE_PCD|PTE_PWT))==(PTE_U|PTE_W|PTE_PCD|PTE_PWT));
    }
    dosvm_fullscreen_switch(v->task); CHECK(fs_ports);
    for (unsigned p=0;p<65536;p++) CHECK(!!(fs_bitmap[p/8]&(1u<<(p%8)))==!((p>=0x3B0 && p<0x3BC) || (p>=0x3C0 && p<0x3E0)));
    CHECK((fs_bitmap[0x3BC/8]&0xF0)==0xF0); /* 3BCh-3BFh: parallel port, denied */
    CHECK(fs_bitmap[8192]==0xFF);
    dosvm_fullscreen_switch(&caller); CHECK(!fs_ports);
    CHECK(!dosvm_fullscreen_input(INPUT_KEY,0x1D,1));
    CHECK(!dosvm_fullscreen_input(INPUT_KEY,0x38,1));
    CHECK(!dosvm_fullscreen_input(INPUT_KEY,0x153,1));
    CHECK(!dosvm_fullscreen_input(INPUT_KEY,0x153,0));
    CHECK(!dosvm_fullscreen_input(INPUT_KEY,0x53,1));
    CHECK(!dosvm_fullscreen_input(INPUT_KEY,0x53,0));
    CHECK(!dosvm_fullscreen_input(INPUT_KEY,0x1D,0));
    CHECK(!dosvm_fullscreen_input(INPUT_KEY,0x38,0));
    CHECK(dosvm_fullscreen_input(INPUT_KEY,0x38,1)==false);
    CHECK(dosvm_fullscreen_input(INPUT_KEY,0x1C,1) && fullscreen_return);
    CHECK(!(v->kbd.held[0x1C>>3]&(1u<<(0x1C&7))));
    CHECK(dosvm_fullscreen_input(INPUT_KEY,0x1C,2) && dosvm_fullscreen_input(INPUT_KEY,0x1C,0));
    CHECK(dosvm_fullscreen_input(INPUT_REL,INPUT_X,10));
    fs_return_order=true;fs_exit_stage=0;fs_expected_owner=v;
    memcpy(fs_expected_ptes,saved,sizeof(saved));
    unsigned exits=fs_ends,returns=fs_watchdog_returns;
    dosvm_fullscreen_poll(); CHECK(!v->fullscreen && !fs_display && !fs_ports);
    CHECK(fs_exit_stage==3 && fs_ends==exits+1 && fs_watchdog_returns==returns+1);
    CHECK(strstr(last_dosvm_log,"revoke_result=0 restore_result=0"));
    fs_return_order=false;
    for (unsigned i=0;i<32;i++) { uint32_t pte; as_lookup(&v->as,0xA0000+i*4096,&pte); CHECK((pte&~0x60u)==(saved[i]&~0x60u)); }
    struct ciuki_input_event out;
    CHECK(dosvm_fullscreen_desktop_event(&out) && out.type==CIUKI_INPUT_RESYNC);
    CHECK(!dosvm_fullscreen_desktop_event(&out));
    dosvm_fullscreen_input(INPUT_KEY,0x38,0);
    v->fullscreen_parked=true;
    CHECK(!dosvm_fullscreen_enter(key)); hardware=card_bus;
    int32_t old_x=v->mouse.x;
    CHECK(dosvm_fullscreen_input(INPUT_REL,INPUT_X,10) && v->mouse.x>=old_x);
    CHECK(dosvm_fullscreen_input(INPUT_BTN,INPUT_LEFT,1) && (v->mouse.buttons&1));
    CHECK(dosvm_fullscreen_input(INPUT_BTN,INPUT_LEFT,0) && !(v->mouse.buttons&1));
    v->fullscreen_pause=true;
    dosvm_fullscreen_input(INPUT_KEY,0x38,1);
    CHECK(dosvm_fullscreen_input(INPUT_KEY,0x0F,1));
    dosvm_fullscreen_poll(); CHECK(!v->fullscreen && v->fullscreen_suspended);
    const uint16_t codes[]={0,0x38,0x0F,0x0F,0x38};
    for (unsigned i=0;i<5;i++) { CHECK(dosvm_fullscreen_desktop_event(&out)); CHECK(out.code==codes[i]); }
    CHECK(!dosvm_fullscreen_desktop_event(&out));
    CHECK(dosvm_input_focus(key,true) && !v->fullscreen_suspended);
    dosvm_fullscreen_input(INPUT_KEY,0x0F,0); dosvm_fullscreen_input(INPUT_KEY,0x38,0);
    v->fullscreen_parked=true; CHECK(!dosvm_fullscreen_enter(key)); hardware=card_bus;
    uint32_t *pd=P2V(v->as.pd_phys),*pt=P2V(pd[0]&~0xFFFu);
    pt[0xA0]|=0x40;
    dosvm_fullscreen_poll(); CHECK(fullscreen_frame_seen && strstr(last_dosvm_log,"fullscreen_first_frame"));
    v->idle.sleeping=true;
    g_ticks+=15000; dosvm_fullscreen_poll(); CHECK(v->fullscreen && fs_display);
    v->idle.sleeping=false; v->fullscreen_progress=g_ticks;
    g_ticks+=15000; dosvm_fullscreen_poll(); CHECK(!v->fullscreen && !fs_display);
    /* Changing instructions and audio/idle hints cannot hide no guest frame. */
    v->fullscreen_parked=true; CHECK(!dosvm_fullscreen_enter(key)); hardware=card_bus;
    v->idle.sleeping=true;
    g_ticks+=4999; dosvm_fullscreen_progress(v,8,0x1234);dosvm_fullscreen_poll();CHECK(v->fullscreen);
    g_ticks++;dosvm_fullscreen_progress(v,8,0x1235);dosvm_fullscreen_poll();
    CHECK(!v->fullscreen && !fs_display && v->fullscreen_error==-ETIMEDOUT);
    v->idle.sleeping=false;
    /* The same ordered leave handles a synchronous clean exit, a watchdog
     * return after successful recovery, and a reported restoration failure. */
    for (unsigned failure=0;failure<2;failure++) {
        v->fullscreen_parked=true;CHECK(!dosvm_fullscreen_enter(key));hardware=card_bus;
        fs_return_order=true;fs_exit_stage=0;fs_end_result=failure ? -EIO : 0;
        exits=fs_ends;
        if (!failure) CHECK(!dosvm_fullscreen_leave(key,false));
        else { fullscreen_return=true;dosvm_fullscreen_poll(); }
        CHECK(fs_exit_stage==3 && fs_ends==exits+1 && !fs_display && !v->fullscreen && !fs_ports);
        CHECK(!fullscreen_owner && !fullscreen_busy && !v->fullscreen_hold);
        CHECK(v->fullscreen_error==fs_end_result);
        CHECK(strstr(last_dosvm_log,failure ? "revoke_result=0 restore_result=-5" : "revoke_result=0 restore_result=0"));
        fs_return_order=false;fs_end_result=0;
    }
    /* Slow UC capture plus native recovery; a stuck VM cannot consume the
     * recovery budget or retain its physical aperture/port lease. */
    for (unsigned delay=0;delay<3;delay++) {
        bool stuck=delay==2;
        v->fullscreen_parked=true;v->fullscreen_suspended=false;
        CHECK(!dosvm_fullscreen_enter(key));hardware=card_bus;
        v->started=true;v->fullscreen_parked=delay==0;
        if (stuck) {
            uint64_t began=g_ticks;unsigned before=fs_ends;
            /* Negative control: the old 2 s handshake expires while leaving
             * the physical display/owner in place, without desktop restore. */
            CHECK(park(v,2000)==-EBUSY && g_ticks-began==2000);
            unhold(v);
            CHECK(v->fullscreen && fs_display && fullscreen_owner==v && fs_ends==before);
            puts("F1-80 old park policy elapsed_ms=2000 result=-16 desktop_restored=0 PASS");
        }
        if (delay==1) {
            fs_waiting_vm=v;fs_park_tick=g_ticks+100;sleep_hook=delayed_fullscreen_park;
        }
        fs_slow_aperture=true;fs_aperture_bytes=0;fs_recovery_ms=160;
        /* fx-22: the guest stored through every aperture page (a game), so
         * the dirty-page capture reads back all 256 KiB as before. */
        { uint32_t *gpd=P2V(v->as.pd_phys),*gpt=P2V(gpd[0]&~0xFFFu); for (unsigned i=0xA0;i<0xC0;i++) gpt[i]|=0x40u; }
        uint64_t began=g_ticks;unsigned before=fs_ends;
        fullscreen_return=true;dosvm_fullscreen_poll();
        sleep_hook=0;fs_waiting_vm=0;
        CHECK(!v->fullscreen && !fs_display && !fs_ports && fs_ends==before+1);
        CHECK(g_ticks-began<=2000 && !fullscreen_owner && !v->fullscreen_hold);
        CHECK(v->fullscreen_suspended==!!stuck);
        CHECK(!stuck || v->fullscreen_error==-ETIMEDOUT);
        for (unsigned i=0;i<32;i++) {
            uint32_t pte;as_lookup(&v->as,0xA0000+i*4096,&pte);
            CHECK((pte&~0x60u)==(saved[i]&~0x60u));
        }
        /* Resume a C transport continuation after revocation: no physical
         * port/aperture access may escape, even though the task stays alive. */
        struct task *prior=g_current;g_current=v->task;
        uint8_t byte=0x5a;fs_write(&byte,0,0);CHECK(byte==0x5a && fs_read(&byte,0)==0xff);
        CHECK(fs_in(0,0x3c4)==0xff);fs_out(0,0x3c4,4);
        /* A later lease must not reauthorize this old continuation. */
        static struct dosvm later_owner;
        later_owner.task=prior;later_owner.fullscreen=true;
        fullscreen_owner=&later_owner;fullscreen_service_task=prior;
        fs_write(&byte,0,0);CHECK(byte==0x5a && fs_read(&byte,0)==0xff);
        CHECK(fs_in(0,0x3c4)==0xff);fs_out(0,0x3c4,4);
        fullscreen_owner=0;
        g_current=prior;
        printf("F1-80 VM park_delay=%u stuck=%u watchdog-return->desktop-restored=%llums aperture_bytes=%u PASS\n",
               delay==1 ? 100u : 0u,stuck,(unsigned long long)(g_ticks-began),fs_aperture_bytes);
        fs_slow_aperture=false;fs_recovery_ms=0;v->fullscreen_suspended=false;
    }
    v->fullscreen_parked=true; CHECK(!dosvm_fullscreen_enter(key)); hardware=card_bus;
    CHECK(!dosvm_stop(key,DOSVM_FAULT) && !fs_ports);
    dosvm_fullscreen_poll(); CHECK(!v->fullscreen && !fs_display);
    struct ciuki_dosvm_command close=command_for(key.id,key.generation);
    CHECK(!finish_test_vm(&close,sizeof(close))); empty();
    fs_test_checkpoint_pin=false;
    g_boot.flags=old_flags; g_boot.test_request_len=old_length; memcpy(g_boot.test_request,old_request,sizeof(old_request));
    puts("PASS full-screen service: refusal/rollback, UC aperture grant/revoke, task-local ports, Alt+Enter/Tab consumed, PS/2/USB mouse path, pause/resume, watchdog and fault cleanup");
    puts("PASS f3-39 VM refusal: windowed continuation has no owner error/port/PTE grant; actual rollback failure remains EIO");
    puts("PASS f3-26 full-screen: pre-mutation checkpoint failure, rollback before guest grant, VGA register readback, first-write evidence and independent 5 s first-frame timeout");
    puts("PASS f3-36 VM exit: clean and return-or-fault watchdog share revoke -> capture -> restore, one lease return, exact PTE/port cleanup, restore result and failure surfaced");
}
static void test_fullscreen_more_modes(void)
{
    window_fixtures();
    struct ciuki_dosvm_command w;CHECK(!create_window(0,"More modes",&w));
    struct dosvm_key key={w.id,w.generation};struct dosvm *v=dosvm_lookup(key);
    CHECK(dosvm_input_focus(key,true));v->fullscreen_parked=true;
    const unsigned heights[]={200,240,400,480};
    for (unsigned wide=0;wide<2;wide++) for (unsigned h=0;h<4;h++) for (unsigned chain=0;chain<2;chain++) {
        int10(v,0x13,0,0,0);dosvm_vga_sync(v,true);
        unsigned width=wide ? 360 : 320;tweak_mode(&v->vga,width,heights[h],chain);
        for (unsigned p=0;p<4;p++) memset(v->vga.plane[p],p+1,CVGA_PLANE_SIZE);
        cvga_geometry g;CHECK(cvga_get_geometry(&v->vga,&g) && g.width==width && g.height==heights[h]);
        CHECK(!dosvm_fullscreen_enter(key));hardware=card_bus;vga_hardware_load(&v->vga,&hardware);
        uint8_t pixels[640];CHECK(cvga_render_row8(&fs_card,0,pixels,sizeof(pixels),0)==width && pixels[0]);
        uint32_t *pd=P2V(v->as.pd_phys),*pt=P2V(pd[0]&~0xFFFu);pt[0xA0]|=0x40;
        dosvm_fullscreen_poll();CHECK(fullscreen_frame_seen);
        CHECK(!dosvm_fullscreen_leave(key,false) && !fs_display && !fs_ports && !v->fullscreen_hold);
        CHECK(cvga_get_geometry(&v->vga,&g) && g.width==width && g.height==heights[h]);
    }
    const unsigned planar[]={0x0D,0x0E,0x10,0x11,0x12,0x12};
    for (unsigned i=0;i<ARRAY_SIZE(planar);i++) {
        int10(v,planar[i],0,0,0);dosvm_vga_sync(v,true);
        if (i==5) {
            const cvga_mode_params *p=cvga_find_mode(0x13);
            const unsigned vertical[]={6,7,9,0x10,0x11,0x12,0x15,0x16};
            for (unsigned k=0;k<ARRAY_SIZE(vertical);k++) v->vga.crtc[vertical[k]]=p->crtc[vertical[k]];
            v->vga.crtc[9]&=0xE0;
        }
        memset(v->vga.plane[0],255,CVGA_PLANE_SIZE);
        CHECK(!dosvm_fullscreen_enter(key));hardware=card_bus;vga_hardware_load(&v->vga,&hardware);
        uint8_t pixels[640];cvga_geometry g;CHECK(cvga_get_geometry(&v->vga,&g));
        CHECK(cvga_render_row8(&fs_card,0,pixels,sizeof(pixels),0)==g.width && pixels[0]);
        uint32_t *pd=P2V(v->as.pd_phys),*pt=P2V(pd[0]&~0xFFFu);pt[0xA0]|=0x40;
        dosvm_fullscreen_poll();CHECK(fullscreen_frame_seen);CHECK(!dosvm_fullscreen_leave(key,false));
    }
    struct ciuki_dosvm_command close=command_for(key.id,key.generation);
    CHECK(!finish_test_vm(&close,sizeof(close)));empty();
    puts("PASS f3-37 VGA: 16 chain-4/unchained 320/360 x200/240/400/480 variants and planar 0d/0e/10/11/12 plus 640x400, enter/nonblack frame/dirty-store proof/capture/return");
}
static int vbe_changing_row(void *arg,unsigned y,const void *pixels,unsigned bytes)
{
    struct dosvm *v=arg;
    if (!y) v->vga.display_changes++;
    return capture_row(frame_buffer,y,pixels,bytes);
}
static void vbe_saved_vga_restore(struct dosvm *v,struct dosvm_key key,bool fullscreen)
{
    int10(v,3,0,0,0);
    struct v86_frame f=vf(0x4F03,0,0,0);
    CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x004F && (uint16_t)f.tf.ebx==3);
    unsigned saved=(uint16_t)f.tf.ebx;
    f=vf(0x4F02,0x4101,0,0);
    CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x004F && v->vbe_mode==0x101);
    if (fullscreen) CHECK(!dosvm_fullscreen_enter(key));
    f=vf(0x4F02,saved,0,0);
    CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x004F && !v->vbe_mode);
    if (fullscreen) {
        CHECK(fullscreen_return && v->fullscreen_hold && fs_display);
        dosvm_fullscreen_poll();
    }
    CHECK(!v->fullscreen && !v->fullscreen_hold && !fs_display);
    f=vf(0x4F03,0,0,0);
    CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.ebx==saved);
    /* DOS text writes must reach the VGA presenter after the restore. */
    int10(v,0x0E43,7,0,0);int10(v,0x0E3E,7,0,0);
    CHECK(cvga_peek_vram(&v->vga,0xB8000)=='C' && cvga_peek_vram(&v->vga,0xB8002)=='>');
    compare_with_reference(v,g_ticks);
    bool visible=false;
    for (unsigned i=0;i<sizeof(frame_buffer);i++) visible|=frame_buffer[i]!=0;
    CHECK(visible);
}
static void test_fullscreen_vbe(void)
{
    window_fixtures();fs_vbe_available=true;
    struct ciuki_dosvm_command w;CHECK(!create_window(0,"VBE modes",&w));
    struct dosvm_key key={w.id,w.generation};struct dosvm *v=dosvm_lookup(key);
    CHECK(dosvm_input_focus(key,true));v->fullscreen_parked=true;
    struct v86_frame f=vf(0x4F00,0,0,0);f.es=0x2000;
    CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x004F);
    uint8_t info[512];CHECK(!dosvm_read(v,0x20000,info,256));
    CHECK(!memcmp(info,"VESA",4) && vm_u16(info+4)==0x200 && vm_u16(info+18)==16);
    const unsigned modes[]={0x101,0x103,0x111,0x114};
    const unsigned vga_modes[]={0,1,2,3,0x0D,0x0E,0x10,0x11,0x12,0x13};
    uint8_t list[30];CHECK(!dosvm_read(v,vm_far(vm_u32(info+14)),list,sizeof(list)));
    for (unsigned i=0;i<4;i++) CHECK(vm_u16(list+2*i)==modes[i]);
    for (unsigned i=0;i<ARRAY_SIZE(vga_modes);i++) CHECK(vm_u16(list+8+2*i)==vga_modes[i]);
    CHECK(vm_u16(list+28)==0xFFFF && !dosvm_range(v,0xF3880,sizeof(list),true));
    for (unsigned i=34;i<256;i++) CHECK(!info[i]);
    CHECK(!dosvm_write(v,0x20000,"VBE2",4));f=vf(0x4F00,0,0,0);f.es=0x2000;
    CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x004F && !dosvm_read(v,0x20000,info,512));
    CHECK(vm_u16(info+20)==1 && vm_u32(info+22)==0xF0003810);
    /* Complete-buffer rejection, no ROM or wrapping segment output. */
    f=vf(0x4F01,0,0x111,0);f.es=0x2000;f.tf.edi=0xFF80;
    CHECK(dosvm_vga_bios(v,&f)==-EFAULT && !v->vbe_mode);
    f.tf.edi=0;f.es=0xF000;CHECK(dosvm_vga_bios(v,&f)==-EFAULT);
    for (unsigned i=0;i<ARRAY_SIZE(vga_modes);i++) {
        unsigned mode=vga_modes[i];const cvga_mode_params *m=cvga_find_mode(mode);
        f=vf(0x4F01,0,mode,0);f.es=0x2000;
        CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x004F && !dosvm_read(v,0x20000,info,256));
        CHECK(vm_u16(info)==(m->text ? 0xF : 0x1F) && info[2]==6 && !info[3]);
        CHECK(vm_u16(info+8)==m->segment && !vm_u32(info+40));
        CHECK(vm_u16(info+18)==(m->text ? m->columns : m->width) &&
              vm_u16(info+20)==(m->text ? m->rows : m->height));
        CHECK(info[24]==(m->text || mode==0x13 ? 1 : 4) && info[25]==(mode==0x13 ? 8 : 4));
        f=vf(0x4F02,mode,0,0);CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x004F && !v->vbe_mode);
        f=vf(0x4F03,0,0,0);CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.ebx==mode);
        uint8_t ctl;CHECK(!dosvm_read(v,0x487,&ctl,1) && !(ctl&0x80));
        cvga_store_plane(&v->vga,0,0,0x5A);
        f=vf(0x4F02,mode|0x8000,0,0);CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x004F);
        CHECK(v->vga.plane[0][0]==0x5A && !dosvm_read(v,0x487,&ctl,1) && (ctl&0x80));
        f=vf(0x4F03,0,0,0);CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.ebx==(mode|0x8000));
    }
    vbe_saved_vga_restore(v,key,false);
    vbe_saved_vga_restore(v,key,true);
    for (unsigned i=0;i<4;i++) {
        const struct dos_video_mode *m=dos_video_mode(modes[i]);
        f=vf(0x4F01,0,m->number,0);f.es=0x2000;
        CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x004F);
        /* war2vbe: window A (D6 clear, attributes 7) beside the linear buffer (D7). */
        CHECK(!dosvm_read(v,0x20000,info,256) && (vm_u16(info)&0xD1)==0x91);
        CHECK(vm_u16(info+16)==m->pitch && vm_u16(info+18)==m->width && vm_u16(info+20)==m->height);
        CHECK(info[25]==m->bpp && vm_u32(info+40)==DOS_VBE_PHYS && info[2]==7 && !info[3]);
        f=vf(0x4F02,m->number|0x4000,0,0);
        CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x004F && v->vbe_mode==m->number);
        v->video_vblank_us=1000000;v->video_blank=0;
        for (unsigned k=0;k<37;k++) (void)dosvm_vga_status(v,1000);
        CHECK(v->video_vblank_us==1000000+(m->width==640 ? 16683u : 16579u));
        f=vf(0x4F03,0,0,0);CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.ebx==(m->number|0x4000));
        f=vf(0x4F06,0xA501,0,0);CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x004F &&
            (uint16_t)f.tf.ebx==m->pitch && (uint16_t)f.tf.ecx==m->width && (uint16_t)f.tf.edx==m->height);
        /* Bit 15 and the BDA agree; replaying 4F03 must retain RAM. */
        uint8_t ctl=0x65,byte=0x5A;
        CHECK(!dosvm_write(v,0x487,&ctl,1) && !dosvm_write(v,v->vbe_buffer.base,&byte,1));
        f=vf(0x4F02,m->number|0xC000,0,0);
        CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x004F && !dosvm_read(v,0x487,&ctl,1) && ctl==0xE5);
        f=vf(0x4F03,0,0,0);CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.ebx==(m->number|0xC000));
        f.tf.eax=0x4F02;CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x004F);
        CHECK(!dosvm_read(v,v->vbe_buffer.base,&byte,1) && byte==0x5A);
        f=vf(0x4F02,m->number|0x4000,0,0);
        CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x004F && !dosvm_read(v,0x487,&ctl,1) && ctl==0x65);
        CHECK(!dosvm_read(v,v->vbe_buffer.base,&byte,1) && !byte);
        f=vf(0x4F03,0,0,0);CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.ebx==(m->number|0x4000));
        uint8_t pattern[4]={0x1F,0xF8,0xE0,0x07};CHECK(!dosvm_write(v,v->vbe_buffer.base,pattern,4));
        struct ciuki_rect damage;v->video_presented=false;
        CHECK(dosvm_vga_present(v,capture_row,frame_buffer,g_ticks,&damage)==1);
        CHECK(*(uint32_t *)frame_buffer!=0 && damage.width==640 && damage.height==480);
        unsigned pages=v->vbe_buffer.pages;uint32_t saved[256];CHECK(pages==256);
        for (unsigned p=0;p<pages;p++) as_lookup(&v->as,v->vbe_buffer.base+p*PAGE_SIZE,&saved[p]);
        fs_linear_writes=0;CHECK(!dosvm_fullscreen_enter(key) && fs_linear_writes && !memcmp(fs_linear,pattern,4));
        dosvm_fullscreen_switch(v->task);CHECK(!fs_ports); /* VBE is always trapped */
        uint8_t physical=*(uint8_t *)P2V(0xB8000),virtual_byte=physical^0x80;
        CHECK(!dosvm_write(v,0xB8000,&virtual_byte,1) && *(uint8_t *)P2V(0xB8000)==physical);
        uint8_t pixel=0x42;CHECK(!dosvm_write(v,v->vbe_buffer.base,&pixel,1));
        *vbe_pte(v,0)|=0x40;dosvm_fullscreen_poll();CHECK(fullscreen_frame_seen && fs_linear[0]==pixel);
        CHECK(!dosvm_fullscreen_leave(key,false) && !fs_display && !v->fullscreen_hold);
        for (unsigned p=0;p<pages;p++) {
            uint32_t pte;as_lookup(&v->as,v->vbe_buffer.base+p*PAGE_SIZE,&pte);CHECK((pte&~0x60u)==(saved[p]&~0x60u));
        }
        CHECK(!dosvm_read(v,v->vbe_buffer.base,&pixel,1) && pixel==0x42);
        v->video_presented=false;CHECK(dosvm_vga_present(v,capture_row,frame_buffer,g_ticks,&damage)==1);
        /* Failed restore is surfaced for each mode; framebuffer RAM survives. */
        CHECK(!dosvm_fullscreen_enter(key));fs_end_result=-EIO;
        CHECK(dosvm_fullscreen_leave(key,false)==-EIO && !fullscreen_owner && !v->fullscreen_hold);
        fs_end_result=0;
    }
    struct ciuki_rect race_damage;v->video_presented=false;
    CHECK(dosvm_vga_present(v,vbe_changing_row,v,g_ticks,&race_damage)==1 && !v->video_presented);
    CHECK(dosvm_vga_present(v,capture_row,frame_buffer,g_ticks,&race_damage)==1 && v->video_presented);
    /* A whole failed palette buffer cannot partially alter guest colours. */
    uint8_t saved_dac[3];memcpy(saved_dac,v->vga.dac[255],3);
    f=vf(0x4F09,0,2,255);f.es=0x2000;
    CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x014F && !memcmp(saved_dac,v->vga.dac[255],3));
    f=vf(0x4F09,0,2,0);f.es=0x2000;f.tf.edi=0xFFFC;
    CHECK(dosvm_vga_bios(v,&f)==-EFAULT);
    uint8_t palette[8]={1,2,3,0,4,5,6,0};CHECK(!dosvm_write(v,0x20000,palette,8));
    f=vf(0x4F08,0xA501,0,0);CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x004F && (uint16_t)f.tf.ebx==0x0600);
    /* Reuse BX=0600h from the DAC query, changing only AX/CX/ES. */
    f.tf.eax=0x4F09;f.tf.ecx=2;f.es=0x2000;
    CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x004F);
    CHECK(v->vga.dac[0][0]==3 && v->vga.dac[1][2]==4);
    CHECK(!dosvm_write(v,0x20000,info,8));
    f=vf(0x4F09,0xA501,2,0);f.es=0x2000;CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x004F && !dosvm_read(v,0x20000,info,8) && !memcmp(info,palette,8));
    f=vf(0x4F08,0x0600,0,0);CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x004F);
    f=vf(0x4F08,0x0800,0,0);CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x014F);
    f=vf(0x4F07,0x0601,0,0);CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x014F);
    f=vf(0x4F07,0x0600,0,0);CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x014F);
    /* Unsupported/custom-refresh modes leave the current state intact (the
     * windowed model of a listed mode, e.g. 101h, is valid since war2vbe). */
    const unsigned bad[]={0x110|0x4000,0x113|0x4000,0x105,0x114|0x4800,0x118|0x4000,0x83,0x4003,4};
    for (unsigned i=0;i<ARRAY_SIZE(bad);i++) {
        uint8_t ctl=0xE5;CHECK(!dosvm_write(v,0x487,&ctl,1));
        f=vf(0x4F02,bad[i],0,0);CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x014F && v->vbe_mode==0x114);
        CHECK(!dosvm_read(v,0x487,&ctl,1) && ctl==0xE5);
        f=vf(0x4F03,0,0,0);CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.ebx==0xC114);
    }
    CHECK(!dosvm_fullscreen_enter(key));fs_linear_error=-EIO;
    *vbe_pte(v,0)|=0x40;dosvm_fullscreen_poll();
    CHECK(!v->fullscreen && !v->fullscreen_hold && v->fullscreen_error==-EIO);fs_linear_error=0;
    /* DPMI maps only the private token, never arbitrary physical memory. */
    v->dpmi.clients=1;
    f=vf(0x0800,DOS_VBE_PHYS>>16,0,0);CHECK(dosvm_vbe_map(v,&f,0x0800)==1);
    f.tf.esi=17;CHECK(dosvm_vbe_map(v,&f,0x0800)==1);
    f=vf(0x0800,DOS_VBE_PHYS>>16,0,0);f.tf.esi=16;f.tf.edi=0;
    CHECK(!dosvm_vbe_map(v,&f,0x0800) && (((f.tf.ebx&65535)<<16)|(f.tf.ecx&65535))==v->vbe_buffer.base);
    CHECK(dpmi_linear_owned(v,v->vbe_buffer.base,DOS_VBE_BYTES));
    f=vf(0x0800,0xFEC0,0,0);f.tf.edi=4096;CHECK(dosvm_vbe_map(v,&f,0x0800)==1);
    f=vf(0x0801,v->vbe_buffer.base>>16,v->vbe_buffer.base&65535,0);
    CHECK(!dosvm_vbe_map(v,&f,0x0801) && dosvm_vbe_map(v,&f,0x0801)==1);v->dpmi.clients=0;
    /* Mode changes while full-screen park and return through the same worker. */
    CHECK(!dosvm_fullscreen_enter(key));f=vf(0x4F02,0x4101,0,0);
    CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x004F && v->fullscreen_hold);
    dosvm_fullscreen_poll();CHECK(!v->fullscreen && !v->fullscreen_hold && v->vbe_mode==0x101);
    CHECK(!dosvm_fullscreen_enter(key));int10(v,3,0,0,0);
    CHECK(fullscreen_return && v->fullscreen_hold && !v->vbe_mode);
    dosvm_fullscreen_poll();CHECK(!v->fullscreen && !v->fullscreen_hold && !fs_display);
    int10(v,0x13,0,0,0);CHECK(!dosvm_fullscreen_enter(key));hardware=card_bus;
    vga_hardware_load(&v->vga,&hardware);f=vf(0x4F02,0x4114,0,0);
    CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x004F && v->fullscreen_hold);
    dosvm_fullscreen_poll();CHECK(!v->fullscreen && !v->fullscreen_hold && v->vbe_mode==0x114);
    fs_linear_error=-EIO;CHECK(dosvm_fullscreen_enter(key)==DISPLAY_FULLSCREEN_WINDOWED && !fs_display && !v->fullscreen_hold);
    fs_linear_error=0;
    struct ciuki_dosvm_command close=command_for(key.id,key.generation);
    CHECK(!finish_test_vm(&close,sizeof(close)));empty();
    v=create();f=vf(0x4F02,0x4101,0,0);
    CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x014F && !v->vbe_buffer.used && !v->vbe_mode);
    destroy(v,true);empty();fs_vbe_available=false;
    puts("PASS f3-37 review fixes: saved VGA restore windowed/fullscreen with visible text, modeled VGA enumeration/info, nonzero-BH palette/scanline calls, preserve-memory/BDA round trips and refused-set retention");
    puts("PASS f3-37 guest VBE: four modes query/set/get, bounded buffers and refusals, private DPMI mapping, window frames, enter/first linear write/exact RAM PTEs/restore failure, ordered VGA return and cleanup");
}
