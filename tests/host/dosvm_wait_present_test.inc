/* F3-52 production wait/presenter with fake clock, transport and scheduling.
 * Scheduling and donation themselves are exercised in proc/dosvm_wait_test.inc.
 * SPDX-License-Identifier: GPL-2.0-only */
static struct dosvm *present_test_vm;
static unsigned present_action;
static uint64_t present_action_at;
static struct ciuki_dosvm_command present_window;
static uint32_t protected_record;
static uint32_t protected_rights;
static void protect_wait_record(void)
{
    if (g_ticks<present_action_at) return;
    sleep_hook=0;
    CHECK(fake_current->memory && !fake_current->memory->pins);
    struct task *saved=g_current; g_current=present_test_vm->task;
    CHECK(!ua_mprotect(fake_current->memory,PAGE_ALIGN_DOWN(protected_record)+PAGE_SIZE,PAGE_SIZE,protected_rights));
    dosvm_console_byte(present_test_vm,'\n');
    g_current=saved;
}
static void test_wait_record_protection(void)
{
    window_fixtures(); CHECK(!create_window(0,"User record",&present_window));
    present_test_vm=dosvm_lookup((struct dosvm_key){present_window.id,present_window.generation});
    struct uaddr u; CHECK(!ua_init(&u,0));
    uint32_t base=CIUKI_MMAP_BASE;
    CHECK(!ua_map_at(&u,base,2*PAGE_SIZE,PROT_READ|PROT_WRITE,PROT_READ|PROT_WRITE,UA_ANON,0));
    protected_record=base+PAGE_SIZE-64; /* The 136-byte record crosses extents. */
    fake_current->memory=&u;
    const unsigned ops[]={CIUKI_DOSVM_WAIT_PRESENT,CIUKI_DOSVM_PRESENT,CIUKI_DOSVM_PRESENT_BANDS};
    for (unsigned op=0;op<ARRAY_SIZE(ops);op++) for (unsigned none=0;none<2;none++) {
        struct ciuki_dosvm_command c=command_for(present_window.id,present_window.generation), after;
        if (!op) c.wait.timeout_ms=1;
        CHECK(!ua_mprotect(&u,base,2*PAGE_SIZE,PROT_READ|PROT_WRITE));
        CHECK(!dosvm_model_write(&u,protected_record,&c,sizeof(c)));
        CHECK(!ua_mprotect(&u,base+PAGE_SIZE,PAGE_SIZE,none ? PROT_NONE : PROT_READ));
        unsigned calls=wait_calls;
        struct trap_frame tf={.ebx=ops[op],.ecx=protected_record,.edx=sizeof(c)};
        CHECK(dosvm_client_syscall(&tf)==-EFAULT && !u.pins);
        if (none) CHECK(wait_calls==calls); /* Unreadable input never enters WAIT. */
        CHECK(!dosvm_model_read(&u,&after,protected_record,sizeof(after)) && !memcmp(&c,&after,sizeof(c)));
    }
    for (unsigned none=0;none<2;none++) {
        CHECK(!ua_mprotect(&u,base,2*PAGE_SIZE,PROT_READ|PROT_WRITE));
        struct ciuki_dosvm_command c=command_for(present_window.id,present_window.generation);
        c.wait.timeout_ms=50;
        CHECK(!dosvm_model_write(&u,protected_record,&c,sizeof(c)));
        protected_rights=none ? PROT_NONE : PROT_READ;
        present_action_at=g_ticks+3; sleep_hook=protect_wait_record;
        struct trap_frame tf={.ebx=CIUKI_DOSVM_WAIT_PRESENT,.ecx=protected_record,.edx=sizeof(c)};
        CHECK(dosvm_client_syscall(&tf)==-EFAULT && !sleep_hook && g_ticks==present_action_at && !u.pins);
        struct ciuki_dosvm_command after;
        CHECK(!dosvm_model_read(&u,&after,protected_record,sizeof(after)) && !memcmp(&c,&after,sizeof(c)));
    }
    fake_current->memory=0; ua_destroy(&u);
    struct ciuki_dosvm_command c=command_for(present_window.id,present_window.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_CLOSE,&c,sizeof(c))); empty();
    puts("F3-52 real uaddr: WAIT/PRESENT/BANDS read-only and PROT_NONE records, cross-page all-or-error copy, protection changed during sleep, zero retained reservations PASS");
}

static void test_wait_cadence(void)
{
    for (unsigned fast=0;fast<2;fast++) for (unsigned direct=0;direct<2;direct++) {
        window_fixtures(); g_ticks=10000;
        CHECK(!create_window(0,"Cadence",&present_window));
        struct dosvm *v=dosvm_lookup((struct dosvm_key){present_window.id,present_window.generation});
        int10(v,0x13,0,0,0);
        if (direct) dosvm_vga_direct(v);
        uint64_t period=14268u*(fast ? 1u : 2u), phase=g_ticks*1000u;
        unsigned frames=0, deferred=0, short_intervals=0, long_intervals=0;
        uint64_t last=0;
        while (frames<140) {
            v->video_flip_run=fast ? 2 : 0; v->video_flip_us=g_ticks*1000u;
            uint8_t pixel=(uint8_t)(frames+1);
            if (direct) {
                uint32_t *p=window_pte(v,0xA0000); CHECK(*p&PTE_W);
                *(uint8_t *)P2V(*p&~0xFFFu)=pixel; *p|=DOSVM_PTE_DIRTY;
            } else CHECK(!dosvm_write(v,0xA0000,&pixel,1));
            struct ciuki_dosvm_command c=command_for(present_window.id,present_window.generation);
            c.wait.timeout_ms=50;
            CHECK(!dosvm_call(CIUKI_DOSVM_WAIT_PRESENT,&c,sizeof(c)) && (c.wait.events&CIUKI_DOSVM_EVENT_VIDEO));
            CHECK(g_ticks==phase/1000u); /* Fractional phase survives every wait. */
            if (frames>1) {
                uint64_t gap=g_ticks-last;
                if (gap==period/1000u) short_intervals++;
                else { CHECK(gap==period/1000u+1); long_intervals++; }
            }
            last=g_ticks;
            /* Rendering can start 0..3 ms after the notification. This must
             * not reset phase or defer alternating samples. */
            g_ticks+=(frames+2)%4;
            if (!frames) phase=g_ticks*1000u; /* Delayed first sample sets the phase. */
            v->video_flip_us=g_ticks*1000u;
            uint32_t samples=v->present_samples;
            c=command_for(present_window.id,present_window.generation);
            CHECK(!dosvm_call(CIUKI_DOSVM_PRESENT_BANDS,&c,sizeof(c)));
            deferred+=v->present_samples==samples;
            CHECK(c.changed && v->video_due_us==phase+period && v->present_due_us==v->video_due_us);
            frames++; phase+=period;
        }
        CHECK(!deferred && short_intervals && long_intervals);
        printf("F3-52 WAIT->PRESENT cadence direct=%u period_us=%llu frames=%u short=%u long=%u deferred=%u delayed_samples=0..3ms PASS\n",
               direct,(unsigned long long)period,frames,short_intervals,long_intervals,deferred);
        struct ciuki_dosvm_command c=command_for(present_window.id,present_window.generation);
        CHECK(!dosvm_call(CIUKI_DOSVM_CLOSE,&c,sizeof(c))); empty();
    }
}
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
    case 5: line(v,"DOSREADY 05.00 free=8000 C\r"); CHECK(v->info.dos_ready); break;
    case 6: v->fullscreen_parked=true; CHECK(!dosvm_fullscreen_enter(v->info.key)); break;
    case 7: CHECK(!dosvm_stop(v->info.key,DOSVM_FAULT)); break;
    /* Signal/process transport makes the blocked native task runnable,
     * independently of VM frame notification (scheduler tests cover unlink). */
    case 8: caught_signal=true; host_wait_notified=true; break;
    case 9: fake_current->state=PROC_STOPPING; host_wait_notified=true; break;
    case 10: entry_error(v->info.key,-EIO); break;
    case 11: v->launch_exec=true; dosvm_launch_video(v); break;
    case 12: break; /* unchanged timeout */
    case 13: dosvm_console_byte(v,'\n'); break; /* enqueue/recheck race */
    case 14: {
        struct ciuki_dosvm_command c=command_for(present_window.id,present_window.generation);
        c.wait.timeout_ms=1;
        CHECK(dosvm_call(CIUKI_DOSVM_WAIT_PRESENT,&c,sizeof(c))==-EBUSY);
        dosvm_console_byte(v,'\n'); break;
    }
    case 15: CHECK(!dosvm_fullscreen_leave(v->info.key,true)); break;
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
    /* No test-side frame checkpoint or notification. The mutation either
     * notifies in its production path or is observed by dosvm_clock_work. */
    if (present_action==4 || present_action==5 || present_action==7 || present_action==10 ||
        present_action==11 || present_action==13 || present_action==14 || present_action==17 || present_action==18)
        CHECK(host_present_wakes>wakes);
}
static void test_wait_present(void)
{
    window_fixtures(); unsigned saved_base=g_current->base_priority;
    g_current->base_priority=9;
    CHECK(!create_window(0,"Producer base",&present_window));
    struct dosvm *producer=dosvm_lookup((struct dosvm_key){present_window.id,present_window.generation});
    CHECK(producer->task->base_priority==7 && !producer->present_wait.head);
    struct ciuki_dosvm_command close=command_for(present_window.id,present_window.generation);
    unsigned forwards=input_forwards;
    close.focused=1; CHECK(!dosvm_call(CIUKI_DOSVM_FOCUS,&close,sizeof(close)));
    CHECK(input_forwards==forwards); /* focus alone is not delivered input */
    close=command_for(present_window.id,present_window.generation);
    close.event.type=INPUT_KEY; close.event.code=0x1e; close.event.value=1;
    CHECK(!dosvm_call(CIUKI_DOSVM_INPUT,&close,sizeof(close)));
    CHECK(input_forwards==forwards+1 && forward_source==g_current && forward_target==producer->task);
    CHECK(producer->task->input_priority==9 && producer->task->input_expiry==g_ticks+20);
    /* The second owner's acquisition is authoritative even before the
     * desktop delivers the old owner's loss message. No second input yet. */
    struct ciuki_dosvm_command second;
    fake_current=&fake_proc[1];
    CHECK(!create_window(3,"Second focus",&second));
    struct dosvm *other=dosvm_lookup((struct dosvm_key){second.id,second.generation});
    second=command_for(second.id,second.generation); second.focused=1;
    CHECK(!dosvm_call(CIUKI_DOSVM_FOCUS,&second,sizeof(second)));
    CHECK(!producer->task->input_priority && !producer->task->input_expiry && !other->task->input_priority);
    second=command_for(second.id,second.generation);
    second.event.type=INPUT_KEY; second.event.code=0x1e; second.event.value=1;
    CHECK(!dosvm_call(CIUKI_DOSVM_INPUT,&second,sizeof(second)) && other->task->input_priority==9);
    fake_current=&fake_proc[0];
    close=command_for(present_window.id,present_window.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_FOCUS,&close,sizeof(close)) && !producer->task->input_priority);
    CHECK(other->task->input_priority==9); /* delayed old loss cannot revoke the new grant */
    fake_current=&fake_proc[1];
    second=command_for(second.id,second.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_CLOSE,&second,sizeof(second)));
    fake_current=&fake_proc[0];
    close.event.type=INPUT_KEY; close.event.code=0x1e; close.event.value=1;
    CHECK(dosvm_call(CIUKI_DOSVM_INPUT,&close,sizeof(close))==-EACCES);
    close=command_for(present_window.id,present_window.generation); close.focused=1;
    CHECK(!dosvm_call(CIUKI_DOSVM_FOCUS,&close,sizeof(close)));
    /* Distinct configured bases make missed asynchronous restoration visible. */
    struct binding *binding=&bindings[producer->info.key.id-1]; binding->vm_base=5;
    priority_transition_task=producer->task; priority_transition_hold=&producer->fullscreen_hold;
    int10(producer,3,0,0,0);
    producer->fullscreen_parked=true;
    CHECK(!dosvm_fullscreen_enter(producer->info.key) && producer->task->base_priority==5);
    CHECK(!dosvm_fullscreen_leave(producer->info.key,false) && producer->task->base_priority==7);
    dosvm_fullscreen_input(INPUT_KEY,0x38,1); dosvm_fullscreen_input(INPUT_KEY,0x1c,1);
    producer->fullscreen_parked=true; dosvm_fullscreen_poll();
    CHECK(producer->fullscreen && producer->task->base_priority==5);
    dosvm_fullscreen_input(INPUT_KEY,0x1c,0); dosvm_fullscreen_input(INPUT_KEY,0x38,0);
    producer->idle.sleeping=producer->idle.seen=false; producer->monitor.state=V86_RUNNING;
    g_ticks+=15000; producer->fullscreen_parked=true; dosvm_fullscreen_poll();
    CHECK(!producer->fullscreen && producer->task->base_priority==7 && producer->fullscreen_error==-ETIMEDOUT);
    priority_transition_task=0; priority_transition_hold=0;
    close=command_for(present_window.id,present_window.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_CLOSE,&close,sizeof(close))); empty();
    puts("F1-87 D binding: focused client base9 leaves VM base7, input forwarding/withdrawal, syscall/hotkey/worker fullscreen bases PASS");
    g_current->base_priority=saved_base;
    for (present_action=0;present_action<21;present_action++) {
        window_fixtures(); g_ticks=10000;
        if (present_action>=19) fs_vbe_available=true;
        CHECK(!create_window(0,"Wait test",&present_window));
        struct dosvm *v=dosvm_lookup((struct dosvm_key){present_window.id,present_window.generation});
        present_test_vm=v; int10(v,0x13,0,0,0);
        if (present_action==6 || present_action==15) {
            memset(fs_bitmap,0xFF,sizeof(fs_bitmap)); hardware=card_bus;
            CHECK(dosvm_input_focus(v->info.key,true));
        }
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
        if (present_action==15) {
            v->fullscreen_parked=true; CHECK(!dosvm_fullscreen_enter(v->info.key));
            v->fullscreen_pause=true;
            c=command_for(present_window.id,present_window.generation); c.wait.timeout_ms=1;
            CHECK(!dosvm_call(CIUKI_DOSVM_WAIT_PRESENT,&c,sizeof(c)));
        }
        g_ticks+=50; v->present_sample_due_us=g_ticks*1000u;
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
            else if (present_action==12 || present_action==13) CHECK(g_ticks-begin==(present_action==12 ? 33u : 0u));
            else CHECK(g_ticks-begin>=3 && g_ticks-begin<=4);
            if (present_action!=12) CHECK(host_present_wakes>wakes);
        }
        caught_signal=false;
        CHECK(!v->present_wait.head && !v->present_wait.owner);
        if (fake_current->state==PROC_LIVE) {
            CHECK(!dosvm_fullscreen_leave(v->info.key,false));
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
    CHECK(g_ticks-observed_at+1>=c.wait.frame_ms); /* Quantize the absolute deadline once. */
    c=command_for(present_window.id,present_window.generation);
    CHECK(!dosvm_call(CIUKI_DOSVM_PRESENT_BANDS,&c,sizeof(c)) && c.changed);
    uint8_t pixel=63; CHECK(!dosvm_write(v,0xA0000,&pixel,1)); g_ticks+=50;
    c=command_for(present_window.id,present_window.generation); c.wait.timeout_ms=50;
    CHECK(!dosvm_call(CIUKI_DOSVM_WAIT_PRESENT,&c,sizeof(c)) && (c.wait.events&CIUKI_DOSVM_EVENT_VIDEO));
    v->video_due_us=(g_ticks+10)*1000u; /* A cadence change between WAIT and PRESENT. */
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
    test_wait_record_protection();
    test_wait_cadence();
}
