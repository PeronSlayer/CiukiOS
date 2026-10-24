/* SPDX-License-Identifier: GPL-2.0-only. Real V86/DPMI handler and owner-only
 * syscall validation. The native/desktop half has its own store tests. */
static struct dosvm *clipboard_vm;
static unsigned clipboard_calls,clipboard_last_op;
static uint32_t clipboard_last_serial;
static void clipboard_complete_hook(void)
{
    wait_register_hook=0;
    struct dosvm *v=clipboard_vm;
    struct binding *b=&bindings[v->info.key.id-1];
    struct present_request r={b,v,fake_current,&window_thread};
    CHECK(present_events(&r)&CIUKI_DOSVM_EVENT_CLIPBOARD);
    struct ciuki_dosvm_command c=command_for(v->info.key.id,v->info.key.generation);
    c.clip.buffer=1024; c.clip.capacity=3072;
    int result=dosvm_call(CIUKI_DOSVM_CLIP_REQUEST,&c,sizeof(c));
    if (result) fprintf(stderr,"clipboard request failed: %d op=%u pending=%u done=%u pid=%u owner=%u\n",result,v->clipboard.op,v->clipboard.pending,v->clipboard.done,fake_current->pid,v->owner_pid);
    CHECK(!result);
    clipboard_calls++; clipboard_last_op=c.clip.op; clipboard_last_serial=c.clip.serial;
    unsigned op=c.clip.op;
    if (op==3) CHECK(c.clip.bytes==5 && !memcmp(user_mem+1024,"DOS!",5));
    if (op==CIUKI_DOSVM_CLIP_SCREEN) CHECK(c.clip.bytes>=5 && !memcmp(user_mem+1024,"DOG!",4));
    uint32_t serial=c.clip.serial;
    c=command_for(v->info.key.id,v->info.key.generation); c.clip.serial=serial-1;
    CHECK(dosvm_call(CIUKI_DOSVM_CLIP_COMPLETE,&c,sizeof(c))==-ESRCH);
    c.clip.serial=serial;
    if (op==4 || op==5) c.clip.bytes=6;
    if (op==5) { memcpy(user_mem+1024,"native",6); c.clip.buffer=1024; c.clip.capacity=6; }
    if (op==9) c.clip.bytes=CIUKI_DOSVM_CLIP_MAX;
    CHECK(!dosvm_call(CIUKI_DOSVM_CLIP_COMPLETE,&c,sizeof(c)));
    CHECK(dosvm_call(CIUKI_DOSVM_CLIP_COMPLETE,&c,sizeof(c))==-ESRCH);
}
static void test_programmatic_clipboard(void)
{
    window_fixtures(); struct ciuki_dosvm_command c;
    CHECK(!create_window(0,"Clipboard test",&c));
    struct dosvm *v=dosvm_lookup((struct dosvm_key){c.id,c.generation}); CHECK(v); clipboard_vm=v;
    struct v86_frame f=vf(0x1700,0,0,0); CHECK(!interrupt_dispatch(v,&f,0x2f) && (uint16_t)f.tf.eax==0x0104);
    for (unsigned op=1;op<=9;op++) {
        if (op==6 || op==7) continue;
        f=vf(0x1700+op,0x200,op==3 ? 5 : 0,7); f.es=0x1000;
        CHECK(!dosvm_write(v,0x10200,"DOS!",5));
        wait_register_hook=clipboard_complete_hook;
        CHECK(!interrupt_dispatch(v,&f,0x2f) && !wait_register_hook && !v->clipboard.pending);
        CHECK(clipboard_last_op==op);
        if (op==4) CHECK((uint16_t)f.tf.eax==6 && !(uint16_t)f.tf.edx);
        else if (op==9) CHECK(!(uint16_t)f.tf.eax && (uint16_t)f.tf.edx==1);
        else CHECK((uint16_t)f.tf.eax==1);
        if (op==5) { char bytes[6]; CHECK(!dosvm_read(v,0x10200,bytes,6) && !memcmp(bytes,"native",6)); }
    }
    unsigned before=clipboard_calls;
    f=vf(0x1703,0,5,8); CHECK(!dosvm_clipboard_interrupt(v,&f) && !(uint16_t)f.tf.eax && clipboard_calls==before);
    f=vf(0x1703,0xffff,5,7); f.es=0xffff;
    /* A guest-controlled range is checked before a desktop request. */
    if (!dosvm_range(v,0x10ffef,5,false)) CHECK(!dosvm_clipboard_interrupt(v,&f) && !(uint16_t)f.tf.eax);
    f=vf(0x1703,0,1,7); f.tf.esi=2; CHECK(!dosvm_clipboard_interrupt(v,&f) && !(uint16_t)f.tf.eax);
    f=vf(0x1706,0,0,0); CHECK(dosvm_clipboard_interrupt(v,&f)==-ENOSYS);
    f=vf(0x1701,0,0,7); wait_register_hook=clipboard_complete_hook;
    f.tf.user_esp=0x100; CHECK(!dosvm_clipboard_reflect(v,&f,0x202));
    CHECK(f.tf.cs==0xf000 && f.tf.eip==DPMI_RM_RETURN && (uint16_t)f.tf.user_esp==0x106 && (uint16_t)f.tf.eax==1);
    f=vf(0x1701,0,0,7); uint64_t start=g_ticks;
    CHECK(!dosvm_clipboard_interrupt(v,&f) && !(uint16_t)f.tf.eax && g_ticks-start==2000 && !v->clipboard.pending);
    c=command_for(c.id,c.generation); c.clip.serial=clipboard_last_serial;
    CHECK(dosvm_call(CIUKI_DOSVM_CLIP_COMPLETE,&c,sizeof(c))==-ESRCH);
    c=command_for(c.id,c.generation);
    fake_current=&fake_proc[1]; CHECK(dosvm_call(CIUKI_DOSVM_CLIP_REQUEST,&c,sizeof(c))==-ESRCH);
    fake_current=&fake_proc[0]; c.clip.capacity=CIUKI_DOSVM_CLIP_MAX+1;
    CHECK(dosvm_call(CIUKI_DOSVM_CLIP_REQUEST,&c,sizeof(c))==-EINVAL);
    c=command_for(c.id,c.generation);
    CHECK(dosvm_input_focus(v->info.key,true)); v->fullscreen_parked=true;
    int10(v,3,0,0,0); uint8_t cells[]={'D',7,'O',7,'G',7,'!',7};
    CHECK(!dosvm_write(v,0xb8000,cells,sizeof(cells)));
    CHECK(!dosvm_fullscreen_enter(v->info.key)); hardware=card_bus; vga_hardware_load(&v->vga,&hardware);
    before=clipboard_calls;
    CHECK(dosvm_fullscreen_input(INPUT_KEY,0x137,1) && v->clipboard_screen);
    wait_register_hook=clipboard_complete_hook; dosvm_clipboard_checkpoint(v);
    CHECK(clipboard_calls==before+1 && clipboard_last_op==CIUKI_DOSVM_CLIP_SCREEN && !v->clipboard_screen);
    CHECK(dosvm_fullscreen_input(INPUT_KEY,0x137,0));
    CHECK(!dosvm_fullscreen_leave(v->info.key,false));
    struct ciuki_input_event pending; while (dosvm_fullscreen_desktop_event(&pending)) {}
    /* Graphics Print Screen consumes no request and leaves the store alone. */
    v->fullscreen=true; v->vbe_mode=0x101; v->clipboard_screen=true;
    before=clipboard_calls; dosvm_clipboard_checkpoint(v); CHECK(clipboard_calls==before && !v->clipboard_screen);
    v->vbe_mode=0; v->fullscreen=false;
    CHECK(!release_binding(&bindings[v->info.key.id-1],true)); clipboard_vm=0;
    printf("PASS G5b WINOLDAP registers, bounded V86 buffer, DPMI return, event wake/completion serial, timeout, owner isolation, full-screen text Print Screen and graphics no-op\n");
}
