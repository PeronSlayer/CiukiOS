/* Real full-screen protocol, real pool/PTEs; only the display device is fake. */
static struct dosvm *native_copy_vm;
static unsigned native_copy_preempts;
static bool native_transfer_entered;
static void native_transfer_enter(void)
{
    preempt_hook=0;
    CHECK(!dosvm_fullscreen_enter(dosvm_key(native_copy_vm)));
    native_transfer_entered=true;
}
static void native_copy_preempt(void)
{
    if (!native_copy_vm->vbe_direct.copying) return;
    CHECK(dosvm_fullscreen_pinned(native_copy_vm) && fullscreen_owner==native_copy_vm);
    CHECK(dosvm_fullscreen_leave(dosvm_key(native_copy_vm),false)==-EBUSY);
    native_copy_preempts++;
}
static void test_101_native(void)
{
    test_101_revoke_races();
    window_fixtures(); fs_vbe_available=true; fs_direct_available=true;
    (void)dosvm_fullscreen_input(INPUT_RESYNC,0,0);
    fs_direct_phys=0x300000; fs_direct_cache=0;
    const unsigned modes[]={0x101,0x103,0x111,0x114};
    for (unsigned cache=0;cache<2;cache++) for (unsigned mode=0;mode<4;mode++) {
        fs_direct_cache=cache ? PTE_PCD|PTE_PWT : 0;
        struct dosvm *v; CHECK(!dosvm_create(0,&v));
        struct dosvm_key key=dosvm_key(v); v->owner_pid=10;
        fs_expected_owner=v;
        CHECK(dosvm_input_focus(key,true)); v->fullscreen_parked=true;
        struct v86_frame f=vf(0x4F02,modes[mode]|0x4000,0,0);
        CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x4F);
        const struct dos_video_mode *m=dos_video_mode(modes[mode]);
        unsigned pages=(m->pitch*m->height+PAGE_SIZE-1)/PAGE_SIZE;
        uint32_t saved[256];
        uint8_t seed=0x27; CHECK(!dosvm_write(v,v->vbe_buffer.base,&seed,1));
        for (unsigned i=0;i<256;i++) CHECK(as_lookup(&v->as,v->vbe_buffer.base+i*PAGE_SIZE,&saved[i]));
        unsigned used=used_pages,mappings=g_user_mappings,resident=v->memory.pool.resident,pinned=v->memory.pool.pinned;
        uint32_t commitment=v->info.commitment;
        /* Every refusal must retain ordinary packed-copy presentation. */
        for (fs_direct_bad=1;fs_direct_bad<=5;fs_direct_bad++) {
            fs_linear_writes=0; CHECK(!dosvm_fullscreen_enter(key) && fs_linear_writes);
            CHECK((as_lookup(&v->as,v->vbe_buffer.base,0)&~4095u)==(saved[0]&~4095u));
            if (fs_direct_bad==1) {
                for (unsigned i=0;i<pages;i++) *vbe_pte(v,i)|=PTE_D;
                fs_linear_writes=fs_linear_bytes=0; dosvm_fullscreen_poll();
                CHECK(fs_linear_writes==pages && fs_linear_bytes==(unsigned)m->pitch*m->height);
                printf("101-d copy frame mode=%x cache=%s pages=%u bytes=%u PASS\n",modes[mode],cache ? "UC" : "WC",pages,fs_linear_bytes);
            }
            CHECK(!dosvm_fullscreen_leave(key,false));
        }
        fs_direct_bad=0;
        uint32_t invalid=*vbe_pte(v,pages/2); *vbe_pte(v,pages/2)&=~PTE_W;
        CHECK(!dosvm_fullscreen_enter(key));
        CHECK(as_lookup(&v->as,v->vbe_buffer.base,0)!=(uint32_t)fs_direct_phys);
        CHECK(!dosvm_fullscreen_leave(key,false)); *vbe_pte(v,pages/2)=invalid;
        native_copy_vm=v; native_transfer_entered=false; preempt_hook=native_transfer_enter;
        uint8_t racing=0xB8;
        CHECK(!dosvm_write(v,v->vbe_buffer.base,&racing,1) && native_transfer_entered);
        CHECK(*(uint8_t *)P2V(fs_direct_phys)==racing);
        unsigned initial_writes=fs_linear_writes;
        for (unsigned i=0;i<pages;i++) {
            uint32_t pte; CHECK(as_lookup(&v->as,v->vbe_buffer.base+i*PAGE_SIZE,&pte)==fs_direct_phys+i*PAGE_SIZE);
            CHECK((pte&(PTE_PCD|PTE_PWT|PTE_BORROWED))==(fs_direct_cache|PTE_BORROWED));
        }
        CHECK(!dosvm_pool_prefault(v,v->vbe_buffer.base,PAGE_SIZE,true));
        CHECK(as_lookup(&v->as,v->vbe_buffer.base,0)==fs_direct_phys);
        /* Private offscreen pages are not evidence of a scanout store. */
        (void)dosvm_vbe_dirty(v,true);
        *vbe_pte(v,255)|=PTE_D; dosvm_fullscreen_poll(); CHECK(!fullscreen_frame_seen);
        /* Real guest stores go through the translated PTE; monitor writes
         * must reach the same device, without a pool-frame/P2V shortcut. */
        *(uint8_t *)P2V(as_lookup(&v->as,v->vbe_buffer.base,0))=0x61;
        *vbe_pte(v,0)|=PTE_D;
        uint8_t value=0; CHECK(!dosvm_read(v,v->vbe_buffer.base,&value,1) && value==0x61);
        const uint8_t *fetch=monitor_memory(v,v->vbe_buffer.base,1,false);
        CHECK(fetch && *fetch==0x61 && fetch!=(uint8_t *)P2V(fs_direct_phys));
        value=0x7A; CHECK(!dosvm_write(v,v->vbe_buffer.base+1,&value,1));
        CHECK(((uint8_t *)P2V(fs_direct_phys))[1]==0x7A);
        CHECK(!aliases(v,(v->vbe_buffer.base-VM_POOL_BASE)/PAGE_SIZE,ALIAS_SAMPLE,0) && (*vbe_pte(v,0)&PTE_D));
        value=0x97; CHECK(!dosvm_write(v,v->vbe_buffer.base+pages*PAGE_SIZE-1,&value,1));
        value=0x9B; CHECK(!dosvm_write(v,v->vbe_buffer.base+DOS_VBE_BYTES-1,&value,1));
        dosvm_fullscreen_poll(); CHECK(fs_linear_writes==initial_writes && fullscreen_frame_seen);
        struct dosvm_frame shot; CHECK(!dosvm_frame_digest(v,g_ticks,&shot) && shot.mode==modes[mode]);
        CHECK(used_pages==used && g_user_mappings==mappings && v->memory.pool.resident==resident &&
              v->memory.pool.pinned==pinned && v->info.commitment==commitment);
        /* An actually running VM must park before copy-back; a delayed
         * checkpoint models preemption during guest execution. */
        v->started=true; v->task->state=T_READY; v->fullscreen_parked=false;
        fs_waiting_vm=v; fs_park_tick=g_ticks+5; sleep_hook=delayed_fullscreen_park;
        CHECK(!dosvm_fullscreen_leave(key,false));
        CHECK(g_ticks>=fs_park_tick);
        sleep_hook=0; fs_waiting_vm=0; v->started=false; v->task->state=T_BLOCKED;
        CHECK(!v->fullscreen_hold && !fs_display);
        for (unsigned i=0;i<256;i++) {
            uint32_t pte; CHECK(as_lookup(&v->as,v->vbe_buffer.base+i*PAGE_SIZE,&pte));
            CHECK((pte&~(PTE_A|PTE_D))==(saved[i]&~(PTE_A|PTE_D)));
        }
        CHECK(!dosvm_read(v,v->vbe_buffer.base,&value,1) && value==0x61);
        CHECK(!dosvm_read(v,v->vbe_buffer.base+1,&value,1) && value==0x7A);
        CHECK(!dosvm_read(v,v->vbe_buffer.base+pages*PAGE_SIZE-1,&value,1) && value==0x97);
        CHECK(!dosvm_read(v,v->vbe_buffer.base+DOS_VBE_BYTES-1,&value,1) && value==0x9B);
        struct dosvm_frame returned;
        CHECK(!dosvm_frame_digest(v,g_ticks,&returned) && !memcmp(shot.digest,returned.digest,sizeof(shot.digest)));
        value=0x19; CHECK(!dosvm_write(v,v->vbe_buffer.base,&value,1));
        CHECK(*(uint8_t *)P2V(fs_direct_phys)==0x61); /* no stale physical access */
        *(uint8_t *)P2V(fs_direct_phys)=0xED; CHECK(*fetch==0x61);
        v->fullscreen_parked=true; CHECK(!dosvm_fullscreen_enter(key));
        *(uint8_t *)P2V(fs_direct_phys)=0x29;
        CHECK(dosvm_input_focus(key,false)); dosvm_fullscreen_poll();
        CHECK(!fs_display && !dosvm_read(v,v->vbe_buffer.base,&value,1) && value==0x29);
        CHECK(dosvm_input_focus(key,true));
        v->fullscreen_parked=true; CHECK(!dosvm_fullscreen_enter(key));
        *(uint8_t *)P2V(fs_direct_phys)=0x53;
        v->started=true; v->task->state=T_READY; v->fullscreen_parked=false;
        CHECK(!dosvm_fullscreen_leave(key,false) && v->fullscreen_suspended);
        CHECK(!dosvm_read(v,v->vbe_buffer.base,&value,1) && value==0x53);
        v->started=false; v->task->state=T_BLOCKED;
        CHECK(dosvm_input_focus(key,true));
        v->fullscreen_parked=true; CHECK(!dosvm_fullscreen_enter(key));
        *(uint8_t *)P2V(fs_direct_phys)=0x5E;
        (void)dosvm_fullscreen_input(INPUT_KEY,0x38,1);
        CHECK(dosvm_fullscreen_input(INPUT_KEY,0x1C,1));
        dosvm_fullscreen_poll(); CHECK(!fs_display);
        CHECK(!dosvm_read(v,v->vbe_buffer.base,&value,1) && value==0x5E);
        (void)dosvm_fullscreen_input(INPUT_RESYNC,0,0);
        /* A mode-set clear must follow copy-back, never be overwritten by it. */
        v->fullscreen_parked=true; CHECK(!dosvm_fullscreen_enter(key));
        native_copy_vm=v; native_copy_preempts=0; preempt_hook=native_copy_preempt;
        f=vf(0x4F02,modes[mode]|0x4000,0,0); CHECK(!dosvm_vga_bios(v,&f));
        preempt_hook=0; CHECK(native_copy_preempts);
        dosvm_fullscreen_poll(); CHECK(!fs_display);
        CHECK(!dosvm_read(v,v->vbe_buffer.base,&value,1) && !value);
        /* A post-grant entry failure rolls all borrowed mappings back. */
        v->fullscreen_parked=true; fs_fail_checkpoint="watchdog-armed";
        CHECK(dosvm_fullscreen_enter(key)==DISPLAY_FULLSCREEN_WINDOWED);
        fs_fail_checkpoint=0; CHECK(!fs_display);
        CHECK(!(as_lookup(&v->as,v->vbe_buffer.base,0)>=fs_direct_phys &&
                as_lookup(&v->as,v->vbe_buffer.base,0)<fs_direct_phys+DOS_VBE_BYTES));
        /* Fault: permissions disappear immediately, before worker recovery. */
        CHECK(!dosvm_fullscreen_enter(key));
        *(uint8_t *)P2V(fs_direct_phys)=0xC3;
        CHECK(!dosvm_stop(key,DOSVM_FAULT));
        uint32_t pte; as_lookup(&v->as,v->vbe_buffer.base,&pte); CHECK(!(pte&(PTE_U|PTE_W)));
        dosvm_fullscreen_poll(); CHECK(!fs_display && !fullscreen_owner);
        CHECK(!dosvm_read(v,v->vbe_buffer.base,&value,1) && value==0xC3);
        CHECK(!dosvm_destroy(key,true));
        printf("101-d direct VBE mode=%x cache=%s pages=%u steady_frame_writes=0 PASS\n",modes[mode],cache ? "UC" : "WC",pages);
    }
    fs_direct_available=false; fs_vbe_available=false; fs_expected_owner=0;
    puts("PASS 101-d direct VBE: layout/cache refusals, guest/monitor stores, screenshot, running revoke, mode clear, rollback, fault, exact PTE/ledger restoration");
}
