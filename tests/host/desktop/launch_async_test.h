/* fx-8: the desktop keeps presenting frames and taking input while a program
 * is being created. The T23 trial of image 423b803a logged
 * `[desktop] stall stage=launch ... no_frame_ms=3891 input_pending=1 wait=launch`
 * while /Programs/netsurf (13.5 MB) was spawned synchronously on the input
 * thread. The fake spawn below takes those 3891 ms unless released.
 * CIUKI_TEST_SYNC_LAUNCH=1 runs the same scenario through the synchronous
 * (pre-fx-8, gate-only) path to show the failure it reproduces.
 * Included by shell_launch_test.c (spawn seam, fake channels).
 * SPDX-License-Identifier: MIT */
static uint64_t async_wall_ms(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000u+(uint64_t)t.tv_nsec/1000000u; }
static void async_release(void)
{ pthread_mutex_lock(&spawn_hold_lock); spawn_released=1; pthread_cond_broadcast(&spawn_hold_changed); pthread_mutex_unlock(&spawn_hold_lock); }
static void async_arm(unsigned ms)
{ pthread_mutex_lock(&spawn_hold_lock); spawn_hold_ms=ms; spawn_released=0; spawn_entered=0; pthread_mutex_unlock(&spawn_hold_lock); }
static void async_wait_entered(void)
{
    pthread_mutex_lock(&spawn_hold_lock);
    struct timespec until; clock_gettime(CLOCK_REALTIME,&until); until.tv_sec+=5;
    while (!spawn_entered) assert(pthread_cond_timedwait(&spawn_hold_changed,&spawn_hold_lock,&until)!=ETIMEDOUT);
    pthread_mutex_unlock(&spawn_hold_lock);
}
/* Service turns until n launches completed; bounded, never a busy spin. */
static void async_complete(struct desktop *d, unsigned n)
{
    uint64_t until=async_wall_ms()+5000;
    unsigned done=0;
    while (done<n) {
        done+=launch_service(d);
        if (done>=n) break;
        assert(async_wall_ms()<until);
        struct timespec pause={.tv_sec=0,.tv_nsec=1000000}; nanosleep(&pause,NULL);
    }
}
static int async_pending_slot(const struct desktop *d)
{
    int found=-1;
    for (int i=0;i<DESK_CLIENTS;i++) if (d->clients[i].spawn_pending) { assert(found<0); found=i; }
    return found;
}
static void test_launch_async(void)
{
    static struct desktop d;
    uint8_t elf[52]={0x7f,'E','L','F',1,1,1}; elf[16]=2; elf[18]=3; elf[20]=1; elf[40]=52;
    fake_reset(); desk_init(&d,&fake_ops,1024,768); desk_shell_enable(&d);
    assert(!d.sh.gate);
    d.launch=launch_program; d.authority_fd=-1;
    d.pixels=calloc(1024*768,4); assert(d.pixels);
    assert(!desk_redraw(&d) && d.presents);
    desk_program_add(&d,"netsurf",elf,sizeof(elf));
    desk_program_add(&d,"control",elf,sizeof(elf));
    const char *sync=getenv("CIUKI_TEST_SYNC_LAUNCH");
    if (!(sync && !strcmp(sync,"1"))) { assert(!launcher_start(&d) && d.launch_async); }

    /* 1. The trial's NetSurf launch from the Programs menu. */
    async_arm(3891);
    unsigned before=netsurf_spawns;
    startup_launch(&d,"netsurf"); assert(d.action>=0);
    uint64_t started=async_wall_ms();
    int r=launch_pending(&d);
    uint64_t blocked=async_wall_ms()-started;
    if (blocked>=250) {
        fprintf(stderr,"FAIL fx-8: the desktop's input thread was blocked %llu ms in the launch "
                "(T23 423b803a: stall stage=launch no_frame_ms=3891); no frame or input in that time\n",
                (unsigned long long)blocked);
        exit(1);
    }
    assert(r==0 && d.launch_deferred && netsurf_spawns==before); assert_launch_empty(&d);
    async_wait_entered();
    assert(!pthread_equal(spawn_thread,pthread_self()));      /* the worker spawns */
    int slot=async_pending_slot(&d); assert(slot>=0);
    struct desk_client *c=&d.clients[slot];
    assert(!c->pid && c->awaiting_frame && !c->window && !strcmp(c->application,"netsurf"));
    int pending_children=0;
    for (int i=0;i<DESK_CLIENTS;i++) pending_children+=d.children[i].pending && !d.children[i].pid;
    assert(pending_children==1);
    /* AppStarting at once, and the slot's channel is not polled for 1 ms pacing. */
    d.now_ns=monotonic(); desk_cursor_update(&d); assert(d.cursor_shape==DESK_CURSOR_ARROW_BUSY);
    assert(desk_launch_wait(&d,d.now_ns)==10000000);
    /* Twenty 16 ms turns of pointer input and frames during the spawn. */
    uint64_t presents=d.presents, inputs=d.inputs;
    int32_t x=d.mouse_x;
    for (unsigned turn=0;turn<20;turn++) {
        struct ciuki_input_event motion={.type=CIUKI_INPUT_MOTION,.value=3,.value2=1,.monotonic_ns=d.now_ns};
        desk_input(&d,&motion);
        d.now_ns+=16000000ull; desk_heartbeat(&d,d.now_ns);
        assert(!launch_service(&d));
        assert(!desk_redraw(&d));
        assert(d.cursor_shape==DESK_CURSOR_ARROW_BUSY);
    }
    assert(d.mouse_x==x+60 && d.presents>=presents+20 && d.inputs>=inputs+20);
    /* Keyboard: Ctrl+Esc opens the Start menu while NetSurf starts; Esc closes it. */
    input_edge(&d,0x1d,1); input_tap(&d,DU_KEY_ESC); input_edge(&d,0x1d,0);
    assert(d.menu_open && d.sh.levels==1);
    input_tap(&d,DU_KEY_ESC); assert(!d.sh.levels);
    assert(!desk_redraw(&d));
    assert(c->spawn_pending && async_wall_ms()-started<3891);
    async_release(); async_complete(&d,1);
    assert(netsurf_spawns==before+1 && !c->spawn_pending && c->pid==(uint32_t)pid_source-1);
    int bound=0;
    for (int i=0;i<DESK_CLIENTS;i++) { assert(!d.children[i].pending); bound+=d.children[i].pid==c->pid; }
    assert(bound==1);
    printf("PASS fx-8 launch: input thread returned in %llu ms; 20 frames and 20 input events, "
           "Start menu during a 3891 ms spawn; PID %u bound after completion\n",
           (unsigned long long)blocked,c->pid);

    /* 2. A failed start is reported afterwards, to the Run dialog's origin. */
    async_arm(3891); fail_spawn=1;
    char line[]="netsurf";
    started=async_wall_ms();
    assert(run_command(&d,line)==0 && async_wall_ms()-started<250);
    assert(strstr(d.notice,"Starting netsurf"));
    async_wait_entered(); slot=async_pending_slot(&d); assert(slot>=0);
    assert(!desk_redraw(&d));
    async_release(); async_complete(&d,1); fail_spawn=0;
    assert(async_pending_slot(&d)<0 && d.clients[slot].channel<0);
    assert(strstr(d.sh.message_text,"'netsurf' could not be started") && strstr(d.sh.message_text,"(8)"));
    for (int i=0;i<DESK_CLIENTS;i++) assert(!d.children[i].pending);
    puts("PASS fx-8 launch failure: Run reports ENOEXEC after the spawn, slot and channel released");

    /* 3. A program that exits before its PID is bound is not kept. */
    for (int i=0;i<DESK_CLIENTS;i++) if (d.clients[i].internal==DESK_INTERNAL_MESSAGE) desk_drop_client(&d,i);
    async_arm(3891);
    assert(desk_task_launch(&d,"control",NULL)==0);
    async_wait_entered(); slot=async_pending_slot(&d); assert(slot>=0);
    uint32_t exiting=(uint32_t)pid_source;
    launch_note_exit(&d,exiting,0);               /* reaped by waitpid(-1) before completion */
    async_release(); async_complete(&d,1);
    assert(d.clients[slot].channel<0);
    for (int i=0;i<DESK_CLIENTS;i++) assert(d.children[i].pid!=exiting && !d.children[i].pending);
    puts("PASS fx-8 early exit: a child reaped before its completion is dropped, not recorded");

    /* 4. LAUNCH from a client: the TASK_RESULT is sent when the spawn completes. */
    async_arm(3891);
    int files=-1;
    for (int i=0;i<DESK_CLIENTS && files<0;i++) if (d.clients[i].channel<0 && !d.clients[i].internal) files=i;
    assert(files>=0);
    fake_channel(90); assert(desk_add_client(&d,90)==files);
    d.clients[files].pid=77; d.clients[files].program=DESK_APP_FILES; d.clients[files].hello=1; d.clients[files].version=DESK_SHELL_VERSION;
    desk_launch_origin(&d,DESK_LAUNCH_CLIENT,NULL);
    d.launch_request.client=files; d.launch_request.channel=90; d.launch_request.pid=77;
    d.launch_request.serial=5; d.launch_request.opcode=DESK_LAUNCH;
    assert(desk_task_launch(&d,"control",NULL)==0 && !d.launch_request.origin);
    async_wait_entered();
    unsigned sent=fake_sent;
    desk_flush(&d,files); assert(fake_sent==sent);              /* nothing owed yet */
    async_release(); async_complete(&d,1);
    desk_flush(&d,files); assert(fake_sent==sent+1 && fake_destinations[sent]==90);
    struct desk_packet p; assert(desk_parse(&fake_messages[sent],0,NULL,&p));
    assert(p.opcode==DESK_TASK_RESULT && desk_get32(fake_messages[sent].data+4)==5 &&
           desk_get32(fake_messages[sent].data+12)==(uint32_t)pid_source-1);
    /* A requester whose slot was reused is not answered. */
    struct desk_launch_request stale={.origin=DESK_LAUNCH_CLIENT,.client=files,.channel=90,.pid=78,.serial=6,.opcode=DESK_LAUNCH};
    sent=fake_sent; desk_launch_completed(&d,&stale,"control",123); desk_flush(&d,files); assert(fake_sent==sent);
    puts("PASS fx-8 client LAUNCH: TASK_RESULT deferred to completion, same requester only");

    async_arm(0);
    desk_menu_close(&d); desk_render_release(&d); drop_all(&d); fake_reset();
    free(d.pixels); free(d.sh.wall);
}
