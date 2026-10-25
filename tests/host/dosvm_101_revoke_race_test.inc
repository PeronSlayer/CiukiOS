/* Real leave/poll and guest mode-set continuations, switched only at kernel
 * service points. No shared busy/copying flag is fabricated by the fixture.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ucontext.h>
static ucontext_t native_race_main, native_race_display, native_race_guest;
static struct dosvm *native_race_vm;
static bool native_race_poll, native_race_worker_copy, native_race_paused, native_race_guest_done;
static unsigned native_race_ends;
static int native_race_leave_result;

static void native_race_switch(ucontext_t *from,ucontext_t *to,struct task *next)
{
    struct task *self=g_current;
    g_current=next; CHECK(!swapcontext(from,to)); g_current=self;
}
static void native_race_checkpoint(const char *step)
{
    if (strcmp(step,native_race_poll ? "first-guest-frame" : "guest-revoke")) return;
    fs_checkpoint_hook=0;
    CHECK(fullscreen_busy && !native_race_vm->fullscreen_hold);
    native_race_switch(&native_race_display,&native_race_guest,native_race_vm->task);
}
static void native_race_preempt(void)
{
    struct dosvm *v=native_race_vm;
    if (!v->vbe_direct.copying || native_race_paused) return;
    native_race_paused=true;
    CHECK(dosvm_fullscreen_pinned(v) && fullscreen_owner==v && fs_display);
    CHECK(v->vbe_direct.pages && v->vbe_direct.lfb.mapped);
    CHECK(!(*vbe_pte(v,0)&(PTE_U|PTE_W)));
    if (native_race_worker_copy) {
        CHECK(g_current==&caller);
        native_race_switch(&native_race_display,&native_race_guest,v->task);
        return;
    }
    native_race_switch(&native_race_guest,&native_race_display,&caller);
    CHECK(fs_display && fs_ends==native_race_ends && v->vbe_direct.lfb.mapped);
}
static void native_race_sleep(void)
{
    if (!native_race_worker_copy || g_current!=native_race_vm->task) return;
    CHECK(native_race_paused && native_race_vm->vbe_direct.copying && fs_display);
    native_race_switch(&native_race_guest,&native_race_display,&caller);
    CHECK(fs_display && fs_ends==native_race_ends && !native_race_vm->vbe_direct.pages);
}
static void native_race_guest_main(void)
{
    struct v86_frame f=vf(0x4F02,native_race_vm->vbe_mode|0x4000,0,0);
    CHECK(!dosvm_vga_bios(native_race_vm,&f) && (uint16_t)f.tf.eax==0x4F);
    native_race_guest_done=true;
    g_current=&caller; CHECK(!setcontext(&native_race_main));
}
static void native_race_display_main(void)
{
    if (native_race_poll) dosvm_fullscreen_poll();
    else native_race_leave_result=dosvm_fullscreen_leave(dosvm_key(native_race_vm),false);
    g_current=&caller; CHECK(!setcontext(&native_race_main));
}
static void native_race_context(ucontext_t *ctx,void *stack,void (*entry)(void))
{
    CHECK(!getcontext(ctx));
    ctx->uc_stack.ss_sp=stack; ctx->uc_stack.ss_size=512u*1024u; ctx->uc_link=0;
    makecontext(ctx,entry,0);
}
static void test_101_revoke_races(void)
{
    const char *only=getenv("CIUKI_VM_101_REVOKE_RACE");
    const char *const names[]={"leave","poll","copy-owner"};
    for (unsigned race=0;race<3;race++) {
        bool poll=race==1;
        if (only && strcmp(only,"all") && strcmp(only,names[race])) continue;
        for (unsigned cache=0;cache<2;cache++) {
            window_fixtures(); fs_vbe_available=fs_direct_available=true;
            fs_direct_phys=0x300000; fs_direct_cache=cache ? PTE_PCD|PTE_PWT : 0;
            struct dosvm *v; CHECK(!dosvm_create(0,&v));
            struct dosvm_key key=dosvm_key(v); v->owner_pid=10; fs_expected_owner=v;
            CHECK(dosvm_input_focus(key,true)); v->fullscreen_parked=true;
            struct v86_frame f=vf(0x4F02,0x4101,0,0);
            CHECK(!dosvm_vga_bios(v,&f));
            uint32_t saved[256];
            for (unsigned i=0;i<256;i++) CHECK(as_lookup(&v->as,v->vbe_buffer.base+i*PAGE_SIZE,&saved[i]));
            unsigned used=used_pages,mappings=g_user_mappings,resident=v->memory.pool.resident,pinned=v->memory.pool.pinned;
            CHECK(!dosvm_fullscreen_enter(key) && v->vbe_direct.pages);
            memset(P2V(fs_direct_phys),0xA7,v->vbe_direct.pages*PAGE_SIZE);
            *vbe_pte(v,0)|=PTE_D;
            v->started=true; v->task->state=T_READY; v->fullscreen_parked=false;
            native_race_vm=v; native_race_poll=poll; native_race_worker_copy=race==2;
            native_race_paused=native_race_guest_done=false;
            native_race_ends=fs_ends; native_race_leave_result=0;
            void *guest_stack=malloc(512u*1024u),*display_stack=malloc(512u*1024u);
            CHECK(guest_stack && display_stack);
            native_race_context(&native_race_guest,guest_stack,native_race_guest_main);
            native_race_context(&native_race_display,display_stack,native_race_display_main);
            fs_checkpoint_hook=native_race_worker_copy ? 0 : native_race_checkpoint;
            preempt_hook=native_race_preempt; sleep_hook=native_race_sleep;
            uint64_t began=g_ticks;
            native_race_switch(&native_race_main,&native_race_display,&caller);
            CHECK(native_race_paused && !native_race_guest_done);
            if (!poll) CHECK(native_race_leave_result==-EBUSY && g_ticks-began>=FS_PARK_MS);
            CHECK(!fullscreen_busy && fullscreen_owner==v && v->fullscreen && fs_display);
            CHECK(fs_ends==native_race_ends && v->fullscreen_hold && dosvm_fullscreen_pinned(v));
            if (native_race_worker_copy)
                CHECK(!v->vbe_direct.pages && !v->vbe_direct.copying && !v->vbe_direct.lfb.mapped);
            else CHECK(v->vbe_direct.pages && v->vbe_direct.copying && v->vbe_direct.lfb.mapped);
            dosvm_fullscreen_poll();
            CHECK(!fullscreen_busy && fs_ends==native_race_ends && fs_display);
            CHECK(dosvm_fullscreen_leave(key,false)==-EBUSY);
            CHECK(dosvm_destroy(key,true)==-EBUSY && dosvm_valid(key));
            native_race_switch(&native_race_main,&native_race_guest,v->task);
            fs_checkpoint_hook=0; preempt_hook=0; sleep_hook=0;
            CHECK(native_race_guest_done && !fullscreen_busy);
            CHECK(!v->vbe_direct.pages && !v->vbe_direct.copying && !v->vbe_direct.lfb.mapped);
            CHECK(fs_display && fs_ends==native_race_ends && fullscreen_return && v->fullscreen_hold);
            for (unsigned i=0;i<256;i++) {
                uint32_t pte; CHECK(as_lookup(&v->as,v->vbe_buffer.base+i*PAGE_SIZE,&pte));
                CHECK((pte&~(PTE_A|PTE_D))==(saved[i]&~(PTE_A|PTE_D)));
            }
            uint8_t value=0xFF;
            CHECK(!dosvm_read(v,v->vbe_buffer.base,&value,1) && !value);
            CHECK(*(uint8_t *)P2V(fs_direct_phys)==0xA7); /* mode clear only touched RAM */
            CHECK(used_pages==used && g_user_mappings==mappings &&
                  v->memory.pool.resident==resident && v->memory.pool.pinned==pinned);
            v->fullscreen_parked=true;
            dosvm_fullscreen_poll();
            CHECK(!fs_display && fs_ends==native_race_ends+1 && !fullscreen_owner && !fullscreen_busy);
            CHECK(!v->fullscreen_hold && !dosvm_fullscreen_pinned(v));
            CHECK(v->fullscreen_suspended==!poll);
            if (!poll) CHECK(v->fullscreen_error==-ETIMEDOUT);
            CHECK(!dosvm_read(v,v->vbe_buffer.base,&value,1) && !value);
            v->started=false; v->task->state=T_BLOCKED;
            CHECK(!dosvm_destroy(key,true));
            free(guest_stack); free(display_stack);
            printf("PASS 101-d revoke race: %s cache=%s\n",names[race],cache ? "UC" : "WC");
        }
    }
    fs_direct_available=fs_vbe_available=false; fs_expected_owner=0; native_race_vm=0;
}
