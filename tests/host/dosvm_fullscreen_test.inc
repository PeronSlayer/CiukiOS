/* Stateful hardware bus, plus the real ownership service and syscall adapter. */
static uint8_t card_read(void *arg,unsigned a) { return cvga_read_vram(arg,0xA0000+a); }
static void card_write(void *arg,unsigned a,uint8_t b) { cvga_write_vram(arg,0xA0000+a,b); }
static uint8_t card_in(void *arg,uint16_t p) { return cvga_read_port(arg,p,0); }
static void card_out(void *arg,uint16_t p,uint8_t b) { cvga_write_port(arg,p,b); }
static struct vga_hardware_bus card_bus={&fs_card,card_in,card_out,card_read,card_write};
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
    fs_refuse=true; CHECK(dosvm_fullscreen_enter(key)==-EBUSY && !v->fullscreen && !v->fullscreen_hold);
    fs_refuse=false;
    fs_test_checkpoint_pin=true;
    fs_fail_checkpoint="vm-park";
    unsigned before=fs_begins;
    CHECK(dosvm_fullscreen_enter(key)==-EIO && !v->fullscreen && !v->fullscreen_hold && fs_begins==before);
    fs_fail_checkpoint="guest-grant";
    CHECK(dosvm_fullscreen_enter(key)==-EIO && !fs_display && !v->fullscreen && !v->fullscreen_hold);
    for (unsigned i=0;i<32;i++) { uint32_t pte;as_lookup(&v->as,0xA0000+i*4096,&pte);CHECK((pte&~0x60u)==(saved[i]&~0x60u)); }
    fs_fail_checkpoint=0;
    fs_stuck_blank=true;
    CHECK(dosvm_fullscreen_enter(key)==-EIO && !fs_display && !v->fullscreen && !v->fullscreen_hold);
    fs_stuck_blank=false;
    CHECK(!dosvm_fullscreen_enter(key) && v->fullscreen && fs_display);
    hardware=card_bus; vga_hardware_load(&v->vga,&hardware);
    for (unsigned i=0;i<32;i++) {
        uint32_t pte; CHECK(as_lookup(&v->as,0xA0000+i*4096,&pte)==0xA0000+i*4096);
        CHECK((pte&(PTE_U|PTE_W|PTE_PCD|PTE_PWT))==(PTE_U|PTE_W|PTE_PCD|PTE_PWT));
    }
    dosvm_fullscreen_switch(v->task); CHECK(fs_ports);
    for (unsigned p=0;p<65536;p++) CHECK(!!(fs_bitmap[p/8]&(1u<<(p%8)))==!(p>=0x3B0 && p<0x3E0));
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
    dosvm_fullscreen_poll(); CHECK(!v->fullscreen && !fs_display && !fs_ports);
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
    v->fullscreen_parked=true; CHECK(!dosvm_fullscreen_enter(key)); hardware=card_bus;
    CHECK(!dosvm_stop(key,DOSVM_FAULT) && !fs_ports);
    dosvm_fullscreen_poll(); CHECK(!v->fullscreen && !fs_display);
    CHECK(!dosvm_destroy(key,false)); empty();
    fs_test_checkpoint_pin=false;
    g_boot.flags=old_flags; g_boot.test_request_len=old_length; memcpy(g_boot.test_request,old_request,sizeof(old_request));
    puts("PASS full-screen service: refusal/rollback, UC aperture grant/revoke, task-local ports, Alt+Enter/Tab consumed, PS/2/USB mouse path, pause/resume, watchdog and fault cleanup");
    puts("PASS f3-26 full-screen: pre-mutation checkpoint failure, rollback before guest grant, VGA register readback, first-write evidence and independent 5 s first-frame timeout");
}
