/* F3-52 production wait/presenter with fake clock, transport and scheduling.
 * Scheduling and donation themselves are exercised in proc/dosvm_wait_test.inc.
 * SPDX-License-Identifier: GPL-2.0-only */
static struct dosvm *present_test_vm;
static unsigned present_action;
static uint64_t present_action_at;
static struct ciuki_dosvm_command present_window;
static void present_action_hook(void)
{
    struct dosvm *v=present_test_vm;
    if (g_ticks < present_action_at) return;
    sleep_hook=wait_register_hook=0;
    unsigned wakes = host_present_wakes;
    CHECK(v->present_wait.owner==v->task && bindings[v->info.key.id-1].busy);
    switch (present_action) {
    case 0: { uint8_t pixel=27; CHECK(!dosvm_write(v,0xA0000,&pixel,1)); break; }
    case 1: {
        dosvm_vga_direct(v);
        uint32_t *pte=window_pte(v,0xA0000);
        CHECK(*pte & PTE_W);
        *(uint8_t *)P2V(*pte & ~0xFFFu)=41; *pte|=DOSVM_PTE_DIRTY;
        break;
    }
    case 2: port_out(v,0x3D4,2,0x2701); break; /* changed horizontal geometry */
    case 3: int10(v,0x12,0,0,0); break;
    case 4: dosvm_console_byte(v,'X'); dosvm_console_byte(v,'\n'); break;
    case 5: v->info.dos_ready=true; v->info.state=DOSVM_RUNNABLE; break;
    case 6: v->fullscreen=true; break;
    case 7: CHECK(!dosvm_stop(v->info.key,DOSVM_FAULT)); break;
    case 8: caught_signal=true; break;
    case 9: fake_current->state=PROC_STOPPING; break;
    case 10: v->fullscreen_error=-EIO; break;
    case 11: v->launch_ready=true; break;
    case 12: break; /* unchanged timeout */
    case 13: dosvm_console_byte(v,'\n'); break; /* enqueue/recheck race */
    case 14: {
        struct ciuki_dosvm_command c=command_for(present_window.id,present_window.generation);
        c.wait.timeout_ms=1;
        CHECK(dosvm_call(CIUKI_DOSVM_WAIT_PRESENT,&c,sizeof(c))==-EBUSY);
        dosvm_console_byte(v,'\n'); break;
    }
    case 15: v->fullscreen_suspended=true; break;
    case 16: v->launch_failed=true; break;
    case 17: CHECK(!dosvm_stop(v->info.key,DOSVM_TERMINATE)); break;
    case 18: user_fault=true; dosvm_console_byte(v,'\n'); break; /* copyout after sleep */
    case 19: {
        struct v86_frame f=vf(0x4F02,0x4111,0,0);
        CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x004F);
        break;
    }
    case 20: {
        uint32_t *pte=vbe_pte(v,0);
        *(uint8_t *)P2V(*pte & ~0xFFFu)=23; *pte|=DOSVM_PTE_DIRTY;
        break;
    }
    }
    struct task *saved = g_current;
    g_current = v->task; /* A VM/deferred worker evaluates the waiter predicate. */
    dosvm_present_checkpoint(v);
    dosvm_present_notify(v);
    if (present_action == 8 || present_action == 9) CHECK(host_present_wakes > wakes);
    g_current = saved;
}
static void test_wait_present(void)
{
    for (present_action=0;present_action<21;present_action++) {
        window_fixtures(); g_ticks=10000;
        if (present_action>=19) fs_vbe_available=true;
        CHECK(!create_window(0,"Wait test",&present_window));
        struct dosvm *v=dosvm_lookup((struct dosvm_key){present_window.id,present_window.generation});
        present_test_vm=v; int10(v,0x13,0,0,0);
        if (present_action==20) {
            struct v86_frame f=vf(0x4F02,0x4111,0,0);
            CHECK(!dosvm_vga_bios(v,&f) && (uint16_t)f.tf.eax==0x004F);
        }
        struct ciuki_dosvm_command c=command_for(present_window.id,present_window.generation);
        c.wait.timeout_ms=50;
        CHECK(!dosvm_call(CIUKI_DOSVM_WAIT_PRESENT,&c,sizeof(c)) && (c.wait.events&CIUKI_DOSVM_EVENT_VIDEO));
        CHECK(c.wait.frame_ms>0 && c.wait.frame_ms<=50 && !v->present_wait.owner);
        c=command_for(present_window.id,present_window.generation);
        CHECK(!dosvm_call(CIUKI_DOSVM_PRESENT_BANDS,&c,sizeof(c)));
        if (present_action==11) {
            v->launch_armed=true;
            c=command_for(present_window.id,present_window.generation); c.wait.timeout_ms=1;
            CHECK(!dosvm_call(CIUKI_DOSVM_WAIT_PRESENT,&c,sizeof(c)) && c.wait.events==CIUKI_DOSVM_EVENT_STATE);
        }
        g_ticks+=50; v->present_sample_due=g_ticks;
        uint64_t begin=g_ticks; unsigned wakes=host_present_wakes;
        present_action_at=g_ticks+(present_action==13 ? 0 : 3);
        if (present_action==13) wait_register_hook=present_action_hook;
        else sleep_hook=present_action_hook;
        c=command_for(present_window.id,present_window.generation); c.wait.timeout_ms=50;
        int e=dosvm_call(CIUKI_DOSVM_WAIT_PRESENT,&c,sizeof(c));
        CHECK(!sleep_hook && !wait_register_hook);
        if (present_action==18) { CHECK(e==-EFAULT); user_fault=false; }
        else if (present_action==8 || present_action==9) CHECK(e==-EINTR);
        else {
            CHECK(!e);
            unsigned expected=present_action<=3 || present_action>=19 ? CIUKI_DOSVM_EVENT_VIDEO :
                present_action==4 || present_action==13 || present_action==14 ? CIUKI_DOSVM_EVENT_CONSOLE :
                present_action==12 ? CIUKI_DOSVM_EVENT_TIMEOUT : CIUKI_DOSVM_EVENT_STATE;
            CHECK(c.wait.events&expected);
            if (present_action==1 || present_action==20) CHECK(g_ticks-begin>=3 && g_ticks-begin<=c.wait.frame_ms+3);
            else CHECK(g_ticks-begin==(present_action==12 ? 50u : present_action==13 ? 0u : 3u));
            if (present_action!=12) CHECK(host_present_wakes>wakes);
        }
        caught_signal=false;
        CHECK(!v->present_wait.head && !v->present_wait.owner);
        if (fake_current->state==PROC_LIVE) {
            v->fullscreen=false;
            c=command_for(present_window.id,present_window.generation);
            CHECK(!dosvm_call(CIUKI_DOSVM_CLOSE,&c,sizeof(c)));
        }
        empty(); CHECK(fake_surface[0].references==1);
    }
    window_fixtures(); CHECK(!create_window(0,"Pins",&present_window));
    struct dosvm *v=dosvm_lookup((struct dosvm_key){present_window.id,present_window.generation});
    struct ciuki_dosvm_command c=command_for(present_window.id,present_window.generation);
    c.wait.timeout_ms=1;
    CHECK(!dosvm_call(CIUKI_DOSVM_WAIT_PRESENT,&c,sizeof(c)));
    c=command_for(present_window.id,present_window.generation);
    /* Replay the former record reservation with the same copy seams: one
     * input-copy residency walk and one explicit writable pin per request. */
    unsigned before=record_pin_walks;
    for (unsigned n=0;n<100;n++) {
        struct ciuki_dosvm_command old; struct ua_pin pin;
        memcpy(user_mem+RECORD,&c,sizeof(c));
        CHECK(!copy_from_user(&old,RECORD,sizeof(old)));
        CHECK(!ua_pin(fake_current->memory,&pin,RECORD,sizeof(old),true));
        CHECK(!copy_to_user(RECORD,&old,sizeof(old))); ua_unpin(fake_current->memory,&pin);
    }
    unsigned old_walks=record_pin_walks-before;
    CHECK(old_walks==200);
    CHECK(!dosvm_call(CIUKI_DOSVM_PRESENT_BANDS,&c,sizeof(c)) && c.changed);
    unsigned rows=surface_rows; before=record_pin_walks;
    v->idle.sleeping=true; unsigned idle_events=v->idle.events;
    for (unsigned n=0;n<100;n++) {
        c=command_for(present_window.id,present_window.generation);
        CHECK(!dosvm_call(CIUKI_DOSVM_PRESENT_BANDS,&c,sizeof(c)) && !c.changed);
    }
    CHECK(record_pin_walks==before && surface_rows==rows && v->idle.sleeping && v->idle.events==idle_events);
    v->idle.sleeping=false;
    for (unsigned n=0;n<100;n++) {
        c=command_for(present_window.id,present_window.generation); c.wait.timeout_ms=1;
        CHECK(!dosvm_call(CIUKI_DOSVM_WAIT_PRESENT,&c,sizeof(c)));
    }
    CHECK(record_pin_walks==before && surface_rows==rows);
    uint16_t cell=0x1F5A;
    CHECK(!dosvm_write(v,0xB8000+2*80*3,&cell,sizeof(cell)));
    c=command_for(present_window.id,present_window.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_PRESENT_BANDS,&c,sizeof(c)) && c.changed);
    unsigned sparse_rows=surface_rows-rows;
    CHECK(sparse_rows>0 && sparse_rows<480 && record_pin_walks==before);
    c=command_for(present_window.id,present_window.generation); c.presentation.redraw=1;
    CHECK(!dosvm_call(CIUKI_DOSVM_PRESENT_BANDS,&c,sizeof(c)) && c.changed && record_pin_walks==before);
    c=command_for(present_window.id,present_window.generation); c.wait.timeout_ms=51;
    CHECK(dosvm_call(CIUKI_DOSVM_WAIT_PRESENT,&c,sizeof(c))==-EINVAL);
    c.wait.timeout_ms=0; CHECK(dosvm_call(CIUKI_DOSVM_WAIT_PRESENT,&c,sizeof(c))==-EINVAL);
    c.wait.timeout_ms=1; c.wait.events=1; CHECK(dosvm_call(CIUKI_DOSVM_WAIT_PRESENT,&c,sizeof(c))==-EINVAL);
    user_fault=true; c=command_for(present_window.id,present_window.generation); c.wait.timeout_ms=1;
    CHECK(dosvm_call(CIUKI_DOSVM_WAIT_PRESENT,&c,sizeof(c))==-EFAULT); user_fault=false;
    c=command_for(present_window.id,present_window.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_CLOSE,&c,sizeof(c))); empty(); fake_current=0;
    window_fixtures(); CHECK(!create_window(0,"Deferred frame",&present_window));
    v=dosvm_lookup((struct dosvm_key){present_window.id,present_window.generation}); int10(v,0x13,0,0,0);
    c=command_for(present_window.id,present_window.generation); c.wait.timeout_ms=50;
    CHECK(!dosvm_call(CIUKI_DOSVM_WAIT_PRESENT,&c,sizeof(c)) && (c.wait.events&CIUKI_DOSVM_EVENT_VIDEO));
    uint64_t observed_at=g_ticks;
    c=command_for(present_window.id,present_window.generation); c.wait.timeout_ms=50;
    CHECK(!dosvm_call(CIUKI_DOSVM_WAIT_PRESENT,&c,sizeof(c)) && (c.wait.events&CIUKI_DOSVM_EVENT_VIDEO));
    CHECK(g_ticks-observed_at>=c.wait.frame_ms); /* Overlay observations remain frame paced. */
    c=command_for(present_window.id,present_window.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_PRESENT_BANDS,&c,sizeof(c)) && c.changed);
    uint8_t pixel=63; CHECK(!dosvm_write(v,0xA0000,&pixel,1)); g_ticks+=50;
    v->video_due_us=(g_ticks+10)*1000u; /* Wait/sample clocks can differ after a delayed present. */
    c=command_for(present_window.id,present_window.generation); c.wait.timeout_ms=50;
    CHECK(!dosvm_call(CIUKI_DOSVM_WAIT_PRESENT,&c,sizeof(c)) && (c.wait.events&CIUKI_DOSVM_EVENT_VIDEO));
    uint32_t samples=v->present_samples;
    c=command_for(present_window.id,present_window.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_PRESENT_BANDS,&c,sizeof(c)) && !c.changed && v->present_samples==samples);
    c=command_for(present_window.id,present_window.generation); c.wait.timeout_ms=50;
    CHECK(!dosvm_call(CIUKI_DOSVM_WAIT_PRESENT,&c,sizeof(c)) && (c.wait.events&CIUKI_DOSVM_EVENT_VIDEO));
    c=command_for(present_window.id,present_window.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_PRESENT_BANDS,&c,sizeof(c)) && c.changed && v->present_samples!=samples);
    CHECK(!bindings[present_window.id-1].video_pending);
    c=command_for(present_window.id,present_window.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_CLOSE,&c,sizeof(c))); empty(); fake_current=0;
    printf("F3-52 wait: dirty/trapped/direct/VBE, geometry/mode, console, ready/launch/failure/fullscreen/suspend/refusal/stop/fault, timeout, signal/exit, copyout fault, enqueue race, single waiter, deferred final frame and overlay pacing PASS\n");
    printf("F3-52 pins: 100 requests former_record_walks=%u unchanged_present_walks=0 unchanged_wait_walks=0 surface_pin_walks=0 unchanged_rows=0 sparse_rows=%u sparse_walks=0; forced frame writes only rows, no pin walk PASS\n",old_walks,sparse_rows);
}
