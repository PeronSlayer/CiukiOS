/* Included by the production desktop launcher harness. SPDX-License-Identifier: MIT */
static struct desktop shutdown_desktop;
static unsigned shutdown_calls, shutdown_prepares, shutdown_terms;
static int shutdown_error, shutdown_busy_left, shutdown_vm_live;
static int shutdown_progress;
static int shutdown_state(void) { return shutdown_progress; }
static uint64_t shutdown_call_times[32];
static int shutdown_external_live=1;
static int shutdown_alive(struct desktop *d, uint32_t pid)
{ (void)d; (void)pid; return shutdown_external_live; }
static void shutdown_prepare(struct desktop *d)
{
    shutdown_prepares++;
    assert(!d->sh.gate);
    power_prepare(d); /* production descriptor cleanup, fstat/close boundary faked */
}
static int shutdown_request(int action)
{
    assert(action==shutdown_desktop.power.action && shutdown_prepares==1);
    assert(shutdown_calls<32);
    shutdown_call_times[shutdown_calls++]=shutdown_desktop.now_ns;
    assert(shutdown_desktop.mixer_fd==-1);
    if (shutdown_vm_live || shutdown_busy_left) {
        if (shutdown_busy_left>0) shutdown_busy_left--;
        errno=EBUSY; return -1;
    }
    if (shutdown_error) { errno=shutdown_error; return -1; }
    return 0;
}
static int shutdown_terminate(uint32_t pid, int sig)
{
    shutdown_terms++;
    assert(sig==SIGTERM);
    int r=terminate_child(pid,sig); /* production call 71 wrapper and rights */
    assert(last_authority==66 && terminated_pid==pid && terminated_signal==SIGTERM);
    return r;
}
static void shutdown_setup(void)
{
    fake_reset(); desk_init(&shutdown_desktop,&fake_ops,640,480);
    desk_shell_enable(&shutdown_desktop);
    shutdown_desktop.self_pid=99; shutdown_desktop.now_ns=1000000000ull;
    shutdown_desktop.terminate=shutdown_terminate; terminate_authority=66;
    shutdown_desktop.power_request=shutdown_request;
    shutdown_desktop.power_state=shutdown_state;
    shutdown_desktop.power_prepare=shutdown_prepare;
    shutdown_desktop.power_alive=shutdown_alive;
    shutdown_desktop.pixels=calloc(640*480,4); assert(shutdown_desktop.pixels);
    shutdown_calls=shutdown_terms=shutdown_prepares=0;
    shutdown_error=shutdown_busy_left=shutdown_vm_live=0;
    shutdown_progress=0;
    shutdown_external_live=1;
    shell_regular_fd=shell_directory_fd=-1;
}
static void shutdown_cleanup(void)
{
    shutdown_desktop.power.state=DESK_POWER_IDLE;
    for (int i=0; i<DESK_CLIENTS; i++) desk_drop_client(&shutdown_desktop,i);
    int keep[]={shutdown_desktop.input,shutdown_desktop.display,shutdown_desktop.output,shutdown_desktop.authority_fd};
    for (unsigned i=0; i<sizeof(keep)/sizeof(keep[0]); i++) if (keep[i]>=0) fake_ops.close(keep[i]);
    free(shutdown_desktop.pixels); shutdown_desktop.pixels=NULL;
    assert(!fake_live());
}
static int shutdown_client(uint32_t pid, const char *name, int owned, int version)
{
    struct desktop *d=&shutdown_desktop;
    int i=0;
    while (i<DESK_CLIENTS && d->clients[i].channel>=0) i++;
    assert(i<DESK_CLIENTS);
    fake_channel(40+i); assert(desk_add_client(d,40+i)==i);
    struct ciuki_message m;
    desk_message(&m,DESK_HELLO,4); m.data[0]=(uint8_t)version; m.sender_pid=pid; desk_receive(d,i,&m);
    fake_surface(60+i,160,80,0xabcdef);
    unsigned n=(unsigned)strlen(name);
    desk_message(&m,DESK_CREATE_WINDOW,16+n); m.data[0]=(uint8_t)version; m.sender_pid=pid;
    m.fd_count=1; m.fds[0]=60+i;
    desk_put32(m.data+4,160); desk_put32(m.data+8,80); desk_put32(m.data+12,n); memcpy(m.data+16,name,n);
    desk_receive(d,i,&m); assert(d->clients[i].window);
    if (owned) d->children[i].pid=pid;
    desk_flush(d,i);
    return i;
}
static void shutdown_at(uint64_t ns)
{
    /* A PING's 5 s HungAppTimeout starts when it is sent, not when it is queued (fx-28). The tests inspect the
     * queued messages, so the transport "sends" each PING at the turn it was queued without draining the ring. */
    for (int i=0;i<DESK_CLIENTS;i++) if (shutdown_desktop.clients[i].ping_pending) shutdown_desktop.clients[i].ping_queued=0;
    desk_heartbeat(&shutdown_desktop,ns);
    if (shutdown_desktop.power.state) shell_service(&shutdown_desktop);
    else desk_power_tick(&shutdown_desktop);
}
static void shutdown_reap(uint32_t pid, int vm)
{
    desk_child_exited(&shutdown_desktop,pid,0);
    if (vm) shutdown_vm_live=0; /* kernel VM teardown is modeled at process exit */
}
static void shutdown_key(int code)
{
    struct ciuki_input_event e={.type=CIUKI_INPUT_KEY,.code=code,.value=1};
    desk_input(&shutdown_desktop,&e);
}
static uint64_t shutdown_sound_deadline;
static unsigned shutdown_sound_checks;
static int shutdown_sound(struct desktop *d) { shutdown_sound_checks++; return d->now_ns>=shutdown_sound_deadline; }
static unsigned shutdown_mode_sets;
static int shutdown_display_set(struct desktop *d, uint16_t id)
{
    assert(d == &shutdown_desktop && id == 0x111);
    shutdown_mode_sets++;
    return 0;
}
static void test_shutdown_sequence(void)
{
    struct desktop *d=&shutdown_desktop;
    /* The audio completion seam precedes descriptor cleanup and the power syscall. */
    shutdown_setup(); d->sound_shutdown=shutdown_sound; shutdown_sound_checks=0;
    shutdown_client(210,"Files",1,2);
    uint64_t sound_start=d->now_ns; shutdown_sound_deadline=sound_start+700000000ull;
    desk_power_begin(d,2); shutdown_at(sound_start);
    assert(!shutdown_prepares && !shutdown_calls && !shutdown_sound_checks);
    shutdown_reap(210,0); shutdown_at(sound_start+100000000ull);
    assert(!shutdown_prepares && !shutdown_calls && shutdown_sound_checks==1);
    shutdown_at(sound_start+699999999ull); assert(!shutdown_prepares && !shutdown_calls);
    shutdown_at(shutdown_sound_deadline); assert(shutdown_prepares==1 && shutdown_calls==1);
    shutdown_cleanup();
    puts("PASS shutdown sound: audio completion precedes descriptor cleanup and the power request");
    /* The merged settings path applies display requests only while idle. */
    shutdown_setup();
    int control=shutdown_client(200,"Control Panel",1,2);
    d->clients[control].program=DESK_APP_CONTROL;
    d->display_set=shutdown_display_set; shutdown_mode_sets=0;
    struct ciuki_message settings;
    desk_message(&settings,DESK_SETTINGS,24);
    settings.data[0]=2; settings.sender_pid=200;
    desk_put32(settings.data+4,1); desk_put32(settings.data+8,DESK_SETTINGS_DISPLAY);
    desk_put32(settings.data+12,0x111);
    desk_receive(d,control,&settings); desk_flush(d,control);
    assert(shutdown_mode_sets==1 && fake_messages[fake_sent-1].data[1]==DESK_TASK_RESULT);
    assert((int32_t)desk_get32(fake_messages[fake_sent-1].data+12)==0);
    desk_power_begin(d,2); desk_flush(d,control);
    assert(d->power.state==DESK_POWER_CLOSING);
    desk_receive(d,control,&settings); desk_flush(d,control);
    assert(shutdown_mode_sets==1 && fake_messages[fake_sent-1].data[1]==DESK_TASK_RESULT);
    assert((int32_t)desk_get32(fake_messages[fake_sent-1].data+12)==-EBUSY);
    shutdown_cleanup();
    puts("PASS display/shutdown settings: idle request applies, closing request returns EBUSY without mode switch");
    /* Two ordinary clients plus a DOS/VM window. All CLOSE requests precede
     * termination/descriptor cleanup/power; EOF is not process exit. */
    shutdown_setup();
    shutdown_client(201,"Files",1,2); shutdown_client(202,"CiukNote",1,1);
    int dos=shutdown_client(203,"DOOM",1,2); shutdown_vm_live=1;
    fake_channel(100); d->mixer_fd=100;
    fake_channel(101); shell_regular_fd=101;
    fake_channel(102); shell_directory_fd=102;
    fake_channel(103); d->display=103; fake_channel(104); d->input=104;
    fake_channel(105); d->output=105; fake_channel(106); d->authority_fd=106;
    unsigned before=fake_sent;
    desk_power_begin(d,2);
    assert(d->power.count==3 && d->power.state==DESK_POWER_CLOSING && !shutdown_calls);
    assert(d->clients[d->power.window].internal==DESK_INTERNAL_POWER);
    assert(!strcmp(d->clients[d->power.window].title,"Shutting down..."));
    for (int i=0; i<3; i++) { assert(d->clients[i].closing); desk_flush(d,i); }
    unsigned closes=0;
    for (unsigned i=before; i<fake_sent; i++) if (fake_messages[i].data[1]==DESK_CLOSE_REQUEST || fake_messages[i].data[1]==DESK_CLOSED) {
        assert(fake_destinations[i]==40+(int)closes);
        assert(fake_messages[i].data[1]==(closes==1 ? DESK_CLOSED : DESK_CLOSE_REQUEST));
        closes++;
    }
    assert(closes==3);
    desk_redraw(d); assert(fake_presents);
    shutdown_at(2000000000ull); assert(!shutdown_calls && !shutdown_terms);
    shutdown_reap(201,0); shutdown_reap(202,0);
    struct ciuki_message m;
    desk_message(&m,DESK_CLOSE,4); m.data[0]=2; m.sender_pid=203; desk_receive(d,dos,&m);
    assert(!d->clients[dos].window && d->children[dos].pid==203 && shutdown_vm_live);
    shutdown_at(3999999999ull); assert(!shutdown_calls && !shutdown_terms);
    shutdown_at(4000000000ull);
    assert(shutdown_terms==1 && shutdown_calls==1 && shutdown_prepares==1 && fake_closed==9 && fake_live()==4);
    shutdown_reap(203,1);
    shutdown_at(4249999999ull); assert(shutdown_calls==1);
    shutdown_at(4250000000ull); assert(shutdown_calls==2 && d->power.state==DESK_POWER_DONE);
    shutdown_at(5000000000ull); assert(shutdown_calls==2);
    shutdown_cleanup();

    /* Production call 70 recognizes stopping versus fully cleaned zombies,
     * and caches one validated snapshot for all targets in the loop. */
    shutdown_setup(); d->now_ns=12000000000ull;
    fake_record_count=1; fake_records[0]=(struct ciuki_proc_record){.pid=207,.state=CIUKI_PROC_STOPPING};
    assert(power_alive(d,207)==1 && power_alive(d,208)==0);
    fake_records[0].state=CIUKI_PROC_ZOMBIE;
    assert(power_alive(d,207)==1); /* same loop: cached */
    d->now_ns++; assert(power_alive(d,207)==0);
    fake_record_count=0; shutdown_cleanup();

    /* Graceful exits take the early path, with no terminate calls. */
    shutdown_setup(); shutdown_client(209,"Files",1,2); shutdown_client(210,"CiukNote",1,2);
    desk_power_begin(d,2); shutdown_at(1500000000ull); assert(!shutdown_calls);
    shutdown_reap(209,0); shutdown_reap(210,0); shutdown_at(2000000000ull);
    assert(shutdown_calls==1 && !shutdown_terms && d->power.state==DESK_POWER_DONE);
    shutdown_cleanup();

    /* Persistent EBUSY names survivors, retry cadence, modal input, Force's
     * one retry after teardown time, Cancel restores focus and ordinary input. */
    shutdown_setup(); int stubborn=shutdown_client(204,"Stubborn",1,2);
    shutdown_busy_left=-1; desk_power_begin(d,1);
    int progress=d->power.window;
    struct desk_box frame=d->clients[progress].frame;
    shutdown_key(0x15b); shutdown_key(0x38); shutdown_key(0x0f);
    assert(!d->sh.levels && !d->sh.switcher && d->focus==progress);
    assert(desk_task_launch(d,"demo",NULL)==-EBUSY && desk_task_switch(d,d->clients[stubborn].window_id)==-EBUSY);
    shutdown_at(4000000000ull);
    for (unsigned i=1; i<=20; i++) shutdown_at(4000000000ull+i*DESK_POWER_RETRY_NS);
    assert(shutdown_calls==21 && shutdown_terms==1 && d->power.state==DESK_POWER_BUSY);
    for (unsigned i=1; i<shutdown_calls; i++) assert(shutdown_call_times[i]-shutdown_call_times[i-1]==DESK_POWER_RETRY_NS);
    assert(strstr(d->power.text,"Some programs are still running:") && strstr(d->power.text,"Stubborn"));
    shutdown_at(10000000000ull); assert(shutdown_calls==21);
    progress=d->power.window;
    frame=d->clients[progress].frame;
    d->mouse_x=frame.x+8; d->mouse_y=frame.y+8;
    struct ciuki_input_event e={.type=CIUKI_INPUT_BUTTON,.code=1,.value=1}; desk_input(d,&e);
    e.type=CIUKI_INPUT_MOTION; e.value=20; e.value2=20; desk_input(d,&e);
    assert(d->drag==-1 && d->clients[progress].frame.x==frame.x && d->clients[progress].frame.y==frame.y);
    shutdown_key(0x21); assert(d->power.state==DESK_POWER_FORCE && shutdown_terms==2 && shutdown_calls==21);
    shutdown_at(10249999999ull); assert(shutdown_calls==21);
    shutdown_at(10250000000ull); assert(shutdown_calls==22 && d->power.state==DESK_POWER_BUSY);
    shutdown_at(11000000000ull); assert(shutdown_calls==22);
    shutdown_key(0x01); assert(d->power.state==DESK_POWER_IDLE && d->focus==stubborn);
    assert(!d->children[stubborn].close_ns && !d->children[stubborn].term_sent && !d->clients[stubborn].closing && !d->sh.modifiers);
    shutdown_key(0x1e);
    assert(queued(d,stubborn,DESK_KEY,NULL)>0);
    shutdown_cleanup();

    /* Cancel during the grace period prevents automatic termination. */
    shutdown_setup(); stubborn=shutdown_client(205,"Unsaved document",1,2);
    desk_power_begin(d,2); shutdown_at(2000000000ull);
    struct desk_box content=desk_content(&d->clients[d->power.window]);
    d->mouse_x=content.x+content.w-81; d->mouse_y=content.y+content.h-29;
    e.type=CIUKI_INPUT_BUTTON; e.code=1; e.value=1; desk_input(d,&e);
    shutdown_at(6000000000ull); assert(!shutdown_terms && !shutdown_calls && d->focus==stubborn);
    shutdown_cleanup();

    /* No-window child (not yet through HELLO) gets the same bounded wait. */
    shutdown_setup(); d->children[0].pid=206; shutdown_busy_left=1;
    desk_power_begin(d,2); shutdown_at(3999999999ull); assert(!shutdown_calls);
    shutdown_at(4000000000ull); assert(shutdown_terms==1 && shutdown_calls==1);
    shutdown_reap(206,0); shutdown_at(4250000000ull); assert(d->power.state==DESK_POWER_DONE);
    shutdown_cleanup();

    /* An external client disconnect must wait for the process-list seam. */
    shutdown_setup(); int external=shutdown_client(207,"External",0,2);
    desk_power_begin(d,2); desk_drop_client(d,external);
    shutdown_at(2000000000ull); assert(!shutdown_calls);
    shutdown_external_live=0; shutdown_at(2100000000ull);
    assert(shutdown_calls==1 && !shutdown_terms);
    shutdown_cleanup();

    /* Successful Force performs exactly one call, even with repeated ticks. */
    shutdown_setup(); shutdown_client(208,"Late exit",1,2); shutdown_busy_left=-1;
    desk_power_begin(d,2); shutdown_at(4000000000ull); shutdown_at(9000000000ull);
    assert(d->power.state==DESK_POWER_BUSY);
    desk_power_force(d); shutdown_reap(208,0); shutdown_busy_left=0;
    shutdown_at(9250000000ull); assert(shutdown_calls==3 && d->power.state==DESK_POWER_DONE);
    shutdown_at(9500000000ull); assert(shutdown_calls==3);
    shutdown_cleanup();

    /* Unknown storage refs are labeled honestly; Cancel is available. */
    shutdown_setup(); shutdown_busy_left=-1;
    desk_power_begin(d,2); shutdown_at(1000000000ull); shutdown_at(6000000000ull);
    assert(d->power.state==DESK_POWER_BUSY && strstr(d->power.text,"Unknown storage holder"));
    desk_power_cancel(d); shutdown_cleanup();

    /* Both actions use production mapping; errors restore input and include
     * errno text. The original ENOSYS help stays present. */
    for (int action=1; action<=2; action++) {
        shutdown_setup(); d->power_request=power_request;
        desk_power_begin(d,action); shutdown_at(1000000000ull);
        assert(last_power_action==(uint32_t)(action==1 ? CIUKI_POWER_REBOOT : CIUKI_POWER_OFF));
        assert(!d->power.state && strstr(d->sh.message_text,"power interface (call 73)") && strstr(d->sh.message_text,strerror(ENOSYS)));
        shutdown_cleanup();
    }
    shutdown_setup(); shutdown_error=EIO; desk_power_begin(d,2); shutdown_at(1000000000ull);
    assert(!d->power.state && strstr(d->sh.message_text,"Shut down failed") && strstr(d->sh.message_text,strerror(EIO)));
    shutdown_cleanup();
    for (unsigned uncertain=0;uncertain<2;uncertain++) {
        shutdown_setup(); shutdown_error=uncertain ? EBUSY : EIO;
        shutdown_progress=uncertain ? CIUKI_POWER_UNCERTAIN : CIUKI_POWER_STOPPED;
        desk_power_begin(d,2); shutdown_at(1000000000ull);
        assert(d->power.state==DESK_POWER_DONE && shutdown_calls==1);
        assert(strstr(d->power.text,uncertain ? "reset button" : "safe to turn off"));
        if (uncertain) assert(!strstr(d->power.text,"safe to turn off"));
        desk_power_cancel(d); assert(d->power.state==DESK_POWER_DONE);
        shutdown_at(2000000000ull); assert(shutdown_calls==1);
        shutdown_cleanup();
    }

    /* Runtime gates (including the crash-isolation desktop) cannot begin. */
    shutdown_setup(); d->sh.gate=1;
    desk_power_begin(d,2); assert(!d->power.state && !shutdown_calls && !shutdown_prepares);
    d->sh.want_power=1; shell_service(d); assert(!d->sh.want_power && !shutdown_calls);
    shutdown_cleanup();
    puts("PASS shutdown sequence: two clients/DOS VM, protocol/exit ordering, 3s timeout/call71 rights, own descriptors, EBUSY 250ms/5s, named survivors, Force once, Cancel/modal input, errors, gates");
}
