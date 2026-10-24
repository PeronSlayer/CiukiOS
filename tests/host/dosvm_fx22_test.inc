/* fx-22: replays of the T23 f441c5bd full-screen return (Quake quit to the
 * DOS prompt in native full screen, Alt+Enter, BOOT.LOG ends at vga-capture).
 * The production state machine (dosvm_fullscreen.c, dosvm.c destroy, the
 * synthetic VGA BIOS) runs unmodified; the display worker, the VM worker and
 * the window owner are coroutines that switch where the kernel can switch
 * (kpreempt_point, task_sleep_ms), with g_current set to the running task.
 * The adapter is the cvga card of dosvm_fullscreen_seams.inc behind the
 * same task-local access rule as the production transport (fs_access).
 *
 *   transfer: Alt+Enter while the guest's own INT 10h AX=0003h (Quake's
 *     VID_Shutdown) is in its full-screen transfer.
 *   deadline: the capture on a stalled 10 us/byte aperture (2.6 s).
 *   closing:  the window owner closes the VM while the return runs.
 *   dirty:    only the pages the guest wrote are read back (fx-22 only).
 *
 * Built against the pre-fx-22 sources with -DFX22_HEAD_BASELINE, the first
 * three fail; the report gives the commands and the failure lines.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ucontext.h>
#define FX22_STACK (512u * 1024u)
static ucontext_t fx22_main_ctx, fx22_vm_ctx, fx22_display_ctx;
static char *fx22_vm_stack, *fx22_display_stack;
static struct dosvm *fx22_vm;
static struct dosvm_key fx22_key;
static struct task fx22_owner_task;          /* the window owner's thread */
static bool fx22_vm_done, fx22_display_started, fx22_display_done;
static unsigned fx22_preempts, fx22_preempt_at;
static bool fx22_slow;
static unsigned fx22_bytes;
static int fx22_destroy_result;
/* The production rule for the physical transport, over the cvga card. */
static void fx22_access(void) { fx22_bytes++; if (fx22_slow && !(fx22_bytes%100)) g_ticks++; }
static uint8_t fx22_in(void *a,uint16_t p) { return fs_access() ? card_in(a,p) : 0xFF; }
static void fx22_out(void *a,uint16_t p,uint8_t b) { if (fs_access()) card_out(a,p,b); }
static uint8_t fx22_read(void *a,unsigned o) { if (!fs_access()) return 0xFF; fx22_access(); return card_read(a,o); }
static void fx22_write(void *a,unsigned o,uint8_t b) { if (fs_access()) { fx22_access(); card_write(a,o,b); } }
static struct vga_hardware_bus fx22_bus={&fs_card,fx22_in,fx22_out,fx22_read,fx22_write};

static void fx22_switch(ucontext_t *from, ucontext_t *to, struct task *next)
{
    struct task *self=g_current;
    g_current=next; CHECK(!swapcontext(from,to)); g_current=self;
}
static struct task *fx22_display_task(void) { return &caller; }
static void fx22_preempt(void)
{
    /* The hotkey arrives at the Nth safe point of the VM's own transfer. */
    if (g_current==fx22_vm->task && !fx22_display_started && ++fx22_preempts==fx22_preempt_at) {
        fx22_display_started=true;
        fx22_switch(&fx22_vm_ctx,&fx22_display_ctx,fx22_display_task());
    }
}
static void fx22_sleep(void)
{
    if (g_current==fx22_display_task() && !fx22_vm_done)
        fx22_switch(&fx22_display_ctx,&fx22_vm_ctx,fx22_vm->task);
    else if (g_current==fx22_vm->task && fx22_display_started && !fx22_display_done)
        fx22_switch(&fx22_vm_ctx,&fx22_display_ctx,fx22_display_task());
}
static void fx22_hotkey(void)
{
    CHECK(!dosvm_fullscreen_input(INPUT_KEY,0x38,1));
    CHECK(dosvm_fullscreen_input(INPUT_KEY,0x1C,1) && fullscreen_return);
    (void)dosvm_fullscreen_input(INPUT_KEY,0x1C,0);
    (void)dosvm_fullscreen_input(INPUT_KEY,0x38,0);
}
static void fx22_display_main(void)
{
    fx22_hotkey();
    dosvm_fullscreen_poll();             /* the display worker's 10 ms poll */
    fx22_display_done=true;
    g_current=fx22_vm_done ? &caller : fx22_vm->task;
    setcontext(fx22_vm_done ? &fx22_main_ctx : &fx22_vm_ctx);
}
static void fx22_vm_main(void)
{
    /* Quake's VID_Shutdown, then the next trap boundary parks the VM. */
    struct v86_frame f=vf(0x0003,0,0,0);
    CHECK(!dosvm_bios(fx22_vm,&f,0x10));
    dosvm_fullscreen_checkpoint(fx22_vm);
    fx22_vm_done=true;
    bool resume=fx22_display_started && !fx22_display_done;
    g_current=&caller;                   /* the display worker, or the test */
    setcontext(resume ? &fx22_display_ctx : &fx22_main_ctx);
}
static void fx22_coroutine(ucontext_t *c, char **stack, void (*fn)(void))
{
    if (!*stack) { *stack=malloc(FX22_STACK); CHECK(*stack); }
    CHECK(!getcontext(c));
    c->uc_stack.ss_sp=*stack; c->uc_stack.ss_size=FX22_STACK; c->uc_link=0;
    makecontext(c,fn,0);
}
/* A full-screen session of a window VM on the card: mode 13h, a Quake frame
 * stored through all 16 graphics pages (their PTE D bits set). */
static struct dosvm *fx22_session(void)
{
    window_fixtures();
    memset(fs_bitmap,0xFF,sizeof(fs_bitmap));
    struct ciuki_dosvm_command w;
    CHECK(!create_window(0,"fx-22",&w));
    fx22_key=(struct dosvm_key){w.id,w.generation};
    struct dosvm *v=dosvm_lookup(fx22_key);
    CHECK(v && dosvm_input_focus(fx22_key,true));
    v->fullscreen_parked=true;
    int10(v,0x0013,0,0,0); dosvm_vga_sync(v,true);
    for (unsigned p=0;p<4;p++) for (unsigned a=0;a<CVGA_PLANE_SIZE;a++) v->vga.plane[p][a]=(uint8_t)(a*7u+p*61u+1u);
    cvga_init(&fs_card);
    CHECK(!dosvm_fullscreen_enter(fx22_key) && v->fullscreen && fs_display);
    hardware=fx22_bus;
    vga_hardware_load(&v->vga,&hardware);            /* as the entry's transfer did */
    uint32_t *pd=P2V(v->as.pd_phys),*pt=P2V(pd[0]&~0xFFFu);
    for (unsigned i=0xA0;i<0xB0;i++) pt[i]|=0x40u;   /* the guest drew its frames */
    v->started=true; v->fullscreen_parked=false;
    return v;
}
static void fx22_close(struct dosvm *v)
{
    (void)v;
    struct ciuki_dosvm_command close=command_for(fx22_key.id,fx22_key.generation);
    CHECK(!finish_test_vm(&close,sizeof(close))); empty();
    fs_slow_aperture=false; fx22_slow=false; fs_end_sleeps=0; fs_recovery_ms=0;
    preempt_hook=0; sleep_hook=0; g_current=&caller;
}

/* transfer: the return must not tear the guest's own mode set. */
static void fx22_transfer(void)
{
    struct dosvm *v=fx22_vm=fx22_session();
    fx22_vm_done=fx22_display_started=fx22_display_done=false;
    fx22_preempts=0; fx22_preempt_at=40;            /* 10 KiB into its read-back */
    preempt_hook=fx22_preempt; sleep_hook=fx22_sleep;
    fx22_coroutine(&fx22_vm_ctx,&fx22_vm_stack,fx22_vm_main);
    fx22_coroutine(&fx22_display_ctx,&fx22_display_stack,fx22_display_main);
    uint64_t began=g_ticks;
    fx22_switch(&fx22_main_ctx,&fx22_vm_ctx,v->task);
    preempt_hook=0; sleep_hook=0; g_current=&caller;
    CHECK(fx22_vm_done && fx22_display_started && fx22_display_done);
    CHECK(!v->fullscreen && !fs_display && !v->fullscreen_hold && !fullscreen_owner && !fullscreen_busy);
    uint8_t mode=0; CHECK(!dosvm_read(v,0x449,&mode,1) && mode==3);
    cvga_geometry g;
    bool text=cvga_get_geometry(&v->vga,&g) && g.text;
    bool card_text=cvga_get_geometry(&fs_card,&g) && g.text;
    bool blank=(v->vga.seq[1]&0x20)!=0;
    printf("fx-22 transfer: guest mode=%u model_text=%u adapter_text=%u model_blank=%u gc6=%02x return_ms=%llu\n",
           mode,(unsigned)text,(unsigned)card_text,(unsigned)blank,v->vga.gc[6],(unsigned long long)(g_ticks-began));
    /* The window shows the DOS prompt's text mode the guest set, unblanked. */
    CHECK(text && card_text && !blank);
    CHECK(v->vga.plane[0][0]==' ' && v->vga.plane[1][0]==0x07);
#ifndef FX22_HEAD_BASELINE
    CHECK(strstr(last_return_log,"transfer=1") && strstr(last_return_log,"park=parked") &&
          strstr(last_return_log,"capture=complete"));
#endif
    fx22_close(v);
    puts("PASS fx-22 transfer: Alt+Enter during the guest's INT 10h mode set keeps its text mode");
}

/* deadline: a stalled aperture cannot hold the desktop. */
static void fx22_deadline(void)
{
    struct dosvm *v=fx22_session();
    v->fullscreen_parked=true;                      /* already at its boundary */
    uint32_t *pd=P2V(v->as.pd_phys),*pt=P2V(pd[0]&~0xFFFu);
    for (unsigned i=0xA0;i<0xC0;i++) pt[i]|=0x40u;
    fx22_slow=true; fx22_bytes=0; fs_recovery_ms=160;
    uint64_t began=g_ticks;
    fx22_hotkey(); dosvm_fullscreen_poll();
    uint64_t elapsed=g_ticks-began;
    printf("fx-22 deadline: watchdog-return->desktop-restored=%llums bytes=%u suspended=%u error=%d\n",
           (unsigned long long)elapsed,fx22_bytes,(unsigned)v->fullscreen_suspended,v->fullscreen_error);
    CHECK(!fs_display && !v->fullscreen && !fullscreen_owner && !v->fullscreen_hold);
    CHECK(elapsed<=1200);                           /* 750 ms capture bound + restore */
    CHECK(v->fullscreen_suspended && v->fullscreen_error==-ETIMEDOUT);
#ifndef FX22_HEAD_BASELINE
    CHECK(strstr(last_return_log,"capture=partial"));
#endif
    v->fullscreen_suspended=false;
    fx22_close(v);
    puts("PASS fx-22 deadline: a 2.6 s capture stops at its bound, the desktop returns, the VM stays suspended");
}

/* closing: the owner's CLOSE while the return runs. */
static void fx22_closing_display(void)
{
    dosvm_fullscreen_poll();
    fx22_display_done=true;
    g_current=&fx22_owner_task;
    setcontext(&fx22_main_ctx);
}
static void fx22_closing_sleep(void)
{
    if (g_current==&fx22_owner_task && !fx22_display_done) {
        fx22_display_started=true;
        fx22_switch(&fx22_main_ctx,&fx22_display_ctx,fx22_display_task());
    } else if (g_current==fx22_display_task() && !fx22_display_done)
        fx22_switch(&fx22_display_ctx,&fx22_main_ctx,&fx22_owner_task);
}
static void fx22_closing(void)
{
    struct dosvm *v=fx22_session();
    v->fullscreen_parked=false;
    fx22_display_started=fx22_display_done=false;
    fs_end_sleeps=40;                               /* the native restore's safe points */
    fx22_coroutine(&fx22_display_ctx,&fx22_display_stack,fx22_closing_display);
    sleep_hook=fx22_closing_sleep;
    destroy_display_failures=0;
    struct task *prior=g_current; g_current=&fx22_owner_task;
    fx22_destroy_result=dosvm_destroy(fx22_key,false);
    g_current=prior;
    while (fx22_display_started && !fx22_display_done) fx22_switch(&fx22_main_ctx,&fx22_display_ctx,fx22_display_task());
    sleep_hook=0;
    printf("fx-22 closing: destroy=%d destroy_display_failures=%u display_restored=%u vm_state=%u\n",
           fx22_destroy_result,destroy_display_failures,(unsigned)!fs_display,(unsigned)v->info.state);
    CHECK(!fs_display && !fullscreen_owner && !fullscreen_busy);
    CHECK(fx22_destroy_result==0 && !destroy_display_failures && v->info.state==DOSVM_DEAD);
    struct ciuki_dosvm_command close=command_for(fx22_key.id,fx22_key.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_CLOSE,&close,sizeof(close))); empty();
    fs_end_sleeps=0; g_current=&caller;
    puts("PASS fx-22 closing: the VM closed during the return is destroyed after it, devices never closed under it");
}

#ifndef FX22_HEAD_BASELINE
/* dirty: what the guest wrote, nothing else. */
static void fx22_dirty(void)
{
    struct dosvm *v=fx22_session();
    v->fullscreen_parked=true;
    uint32_t *pd=P2V(v->as.pd_phys),*pt=P2V(pd[0]&~0xFFFu);
    for (unsigned i=0xA0;i<0xC0;i++) pt[i]&=~0x40u;
    fullscreen_dirty=0;
    /* The guest stores one byte at B800:0000 (page B8h -> plane pages 8, 0). */
    pt[0xB8]|=0x40u;
    fs_card.plane[0][0]=0x41; fs_card.plane[0][0x8000]=0x42;
    fs_card.plane[0][0x4000]=0x43;                   /* not written by the guest */
    fx22_bytes=0;
    fx22_hotkey(); dosvm_fullscreen_poll();
    printf("fx-22 dirty: bytes=%u %s\n",fx22_bytes,last_return_log);
    CHECK(strstr(last_return_log,"capture=complete capture_pages=2/2"));
    CHECK(fx22_bytes<=2*4*4096+1);
    CHECK(v->vga.plane[0][0]==0x41 && v->vga.plane[0][0x8000]==0x42 && v->vga.plane[0][0x4000]!=0x43);
    /* A kernel copy by the owner marks its pages; another task gets the model. */
    v->fullscreen_parked=true;
    CHECK(!dosvm_fullscreen_enter(fx22_key)); hardware=fx22_bus;
    fullscreen_dirty=0;
    struct task *prior=g_current; g_current=v->task;
    CHECK(dosvm_fullscreen_aperture(v,0xA3000,16,true) && (fullscreen_dirty&(1u<<3)));
    static struct task other; g_current=&other;
    CHECK(!dosvm_fullscreen_aperture(v,0xA4000,16,true) && !(fullscreen_dirty&(1u<<4)));
    g_current=prior;
    /* A completed BIOS transfer resynchronises: no stale page is read back. */
    pt=P2V(((uint32_t *)P2V(v->as.pd_phys))[0]&~0xFFFu);
    for (unsigned i=0xA0;i<0xC0;i++) pt[i]|=0x40u;
    g_current=v->task; int10(v,0x0200,0,0,0x0101); g_current=prior; /* AH=02h cursor */
    CHECK(!fullscreen_dirty);
    for (unsigned i=0xA0;i<0xC0;i++) CHECK(!(pt[i]&0x40u));
    fx22_bytes=0; fx22_hotkey(); dosvm_fullscreen_poll();
    CHECK(strstr(last_return_log,"capture=complete capture_pages=0/0"));
    fx22_close(v);
    puts("PASS fx-22 dirty: return reads back only guest-written pages; owner copies are recorded, a BIOS transfer resynchronises");
}
#endif

static void test_fx22(void)
{
    /* CIUKI_VM_FX22=transfer|deadline|closing|dirty selects one replay. */
    const char *only=getenv("CIUKI_VM_FX22");
    bool all=!only || !strcmp(only,"1");
    if (all || !strcmp(only,"transfer")) fx22_transfer();
    if (all || !strcmp(only,"deadline")) fx22_deadline();
    if (all || !strcmp(only,"closing")) fx22_closing();
#ifndef FX22_HEAD_BASELINE
    if (all || !strcmp(only,"dirty")) fx22_dirty();
#endif
    free(fx22_vm_stack); free(fx22_display_stack); fx22_vm_stack=fx22_display_stack=0;
}
