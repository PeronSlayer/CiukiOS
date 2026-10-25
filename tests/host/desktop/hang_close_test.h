/* T23 trial c42f556e (docs/validation/t23-win32-hang-shutdown-2026-10-25.md):
 * four Quake III Arena demo installers at once. The desktop's system dialogs
 * must open however many windows the programs hold, End Task must end a busy
 * program that keeps answering but does not close (Windows HungAppTimeout),
 * a second close request on a "Closing..." window offers End Task at once,
 * the end-of-session Win32 turn must not wait forever on a process with no
 * window, and the power sequence must end every Win32 process (Force with
 * SIGKILL). Included by the production desktop launcher harness after
 * end_session_test.h. SPDX-License-Identifier: MIT */
static unsigned hc_terms; static uint32_t hc_term_pid[16]; static int hc_term_sig[16];
static int hc_terminate(uint32_t pid, int sig)
{ if (hc_terms<16) { hc_term_pid[hc_terms]=pid; hc_term_sig[hc_terms]=sig; } hc_terms++; return 0; }
static unsigned hc_killed(uint32_t pid, int sig)
{ unsigned n=0; for (unsigned i=0;i<hc_terms && i<16;i++) n+=hc_term_pid[i]==pid && hc_term_sig[i]==sig; return n; }
/* A v2 program window; -1 when the desktop refuses the connection. */
static int hc_client(uint32_t pid, const char *name)
{
    struct desktop *d=&shutdown_desktop;
    int i=0;
    while (i<DESK_CLIENTS && (d->clients[i].channel>=0 || d->clients[i].internal)) i++;
    if (i==DESK_CLIENTS) return -1;
    fake_channel(40+i);
    if (desk_add_client(d,40+i)<0) { fake_ops.close(40+i); return -1; }
    struct ciuki_message m;
    desk_message(&m,DESK_HELLO,4); m.data[0]=2; m.sender_pid=pid; desk_receive(d,i,&m);
    fake_surface(60+i,160,80,0xabcdef);
    unsigned n=(unsigned)strlen(name);
    desk_message(&m,DESK_CREATE_WINDOW,16+n); m.data[0]=2; m.sender_pid=pid;
    m.fd_count=1; m.fds[0]=60+i;
    desk_put32(m.data+4,160); desk_put32(m.data+8,80); desk_put32(m.data+12,n); memcpy(m.data+16,name,n);
    desk_receive(d,i,&m); assert(d->clients[i].window);
    desk_flush(d,i);
    return i;
}
static void hc_key(int code, int value)
{ struct ciuki_input_event e={.type=CIUKI_INPUT_KEY,.code=code,.value=value}; desk_input(&shutdown_desktop,&e); }
static void hc_alt_f4(void) { hc_key(0x38,1); hc_key(0x3e,1); hc_key(0x3e,0); hc_key(0x38,0); }
static uint32_t hc_w32[4]; static unsigned hc_w32_count;
static unsigned hc_w32_pids(struct desktop *d, uint32_t *pids, unsigned cap)
{ (void)d; unsigned n=0; for (; n<hc_w32_count && n<cap; n++) pids[n]=hc_w32[n]; return n; }
static void test_t23_hang_close(void)
{
    struct desktop *d=&shutdown_desktop;
    /* 1. A full client table: programs stop DESK_SYSTEM_SLOTS short of it,
     * so End Task's "This program is not responding" and Shut Down still
     * open; another built-in window leaves one slot. Before, sixteen
     * windows filled the table and both dialogs failed silently. */
    es_setup(); d->terminate=hc_terminate; hc_terms=0;
    int slots[DESK_CLIENTS]; unsigned programs=0;
    for (unsigned k=0;k<DESK_CLIENTS;k++) {
        char name[32]; snprintf(name,sizeof(name),"Quake III Arena Demo %u",k);
        int i=hc_client(500+k,name);
        if (i<0) break;
        slots[programs++]=i;
    }
    assert(programs==DESK_CLIENTS-DESK_SYSTEM_SLOTS && desk_free_slots(d)==DESK_SYSTEM_SLOTS && d->slots_refused==1);
    assert(hc_client(600,"One too many")<0 && d->slots_refused==2);
    desk_request_close(d,slots[3]);
    assert(desk_task_end(d,d->clients[slots[3]].window_id,503)==0);   /* already closing: the dialog at once */
    assert(d->end_task.kind==DESK_HANG_NOT_RESPONDING && d->end_task.window>=0 && d->end_task.pid==503);
    assert(desk_internal_open(d,DESK_INTERNAL_ABOUT)<0);           /* leaves the last slot to a system dialog */
    int shut=desk_internal_open(d,DESK_INTERNAL_SHUTDOWN);
    assert(shut>=0 && d->clients[shut].internal==DESK_INTERNAL_SHUTDOWN && !desk_free_slots(d));
    desk_drop_client(d,shut);
    desk_end_task_choice(d,1);
    assert(hc_terms==1 && hc_killed(503,SIGKILL)==1);
    shutdown_cleanup();
    puts("PASS T23 slots: programs keep two slots free, End Task dialog and Shut Down open with the table full, About refused");

    /* 2. End Task on a busy program that answers PINGs but does not close
     * (the installer ignores SC_CLOSE and declines): after the 5 s
     * HungAppTimeout, "This program is not responding"; End Task kills it.
     * A program that closes in time gets no dialog. Before, End Task only
     * sent WM_CLOSE again and the program could never be ended. */
    es_setup(); d->terminate=hc_terminate; hc_terms=0;
    int busy=hc_client(511,"Quake III Arena Demo"), quick=hc_client(512,"Notepad");
    assert(desk_task_end(d,d->clients[busy].window_id,511)==0 && d->clients[busy].closing && !d->end_task.kind);
    es_send(busy,511,DESK_CLOSE_ACK,8,DESK_CLOSE_DECLINED,0);
    assert(!d->clients[busy].closing && d->clients[busy].end_task_ns);
    assert(desk_task_end(d,d->clients[quick].window_id,512)==0);
    { int s[]={busy,quick}; uint32_t p[]={511,512,0}; es_run(1250000000ull,5750000000ull,s,p); }
    assert(!d->end_task.kind && !hc_terms);
    desk_drop_client(d,quick);                      /* Notepad closed in time */
    { int s[]={busy}; uint32_t p[]={511,0}; es_run(6000000000ull,6250000000ull,s,p); }
    assert(d->end_task.kind==DESK_HANG_NOT_RESPONDING && !d->end_task.session && d->end_task.pid==511 &&
           !strcmp(d->end_task.name,"Quake III Arena Demo"));
    desk_end_task_choice(d,1);
    assert(hc_terms==1 && hc_killed(511,SIGKILL)==1);
    { int s[]={busy}; uint32_t p[]={511,0}; es_run(6500000000ull,20000000000ull,s,p); }
    assert(!d->end_task.kind && hc_terms==1);       /* one dialog per End Task */
    shutdown_cleanup();
    puts("PASS T23 End Task: a busy program that answers but does not close gets Not Responding after 5 s, End Task SIGKILL");

    /* 3. Alt+F4 (title button, task button Close) twice: the second request
     * on a window still "Closing..." opens the dialog at once; a program that
     * answered LATER and shows its own prompt is only asked once. Before,
     * the second request did nothing. */
    es_setup(); d->terminate=hc_terminate; hc_terms=0;
    int setup=hc_client(521,"Starting Setup"), note=hc_client(522,"Notepad");
    desk_focus(d,setup); hc_alt_f4();
    assert(d->clients[setup].closing && es_count(setup,DESK_CLOSE_REQUEST)==1 && !d->end_task.kind);
    es_pong(setup,521); shutdown_at(1500000000ull);
    desk_focus(d,setup); hc_alt_f4();
    assert(d->end_task.kind==DESK_HANG_NOT_RESPONDING && d->end_task.pid==521 && es_count(setup,DESK_CLOSE_REQUEST)==1);
    desk_end_task_choice(d,0);                      /* Cancel: the program stays */
    assert(!hc_terms && d->clients[setup].window);
    desk_focus(d,note); hc_alt_f4();
    es_send(note,522,DESK_CLOSE_ACK,8,DESK_CLOSE_LATER,0);
    desk_focus(d,note); hc_alt_f4();
    assert(!d->end_task.kind && d->clients[note].closing && es_count(note,DESK_CLOSE_REQUEST)==1);
    shutdown_cleanup();
    puts("PASS T23 close twice: a second Alt+F4 on a closing window opens Not Responding, a prompting program is asked once");

    /* 4. Restart with only Win32 programs (the log's "programs=1"): the
     * window in turn belongs to a process with no window on the desktop (its
     * connection ended) that fetched WM_QUERYENDSESSION and never answers.
     * After 5 s it is not responding; End Task ends it and the session goes
     * on. Before, the desktop waited forever with nothing on screen. */
    es_setup(); d->w32_session=es_w32; d->terminate=hc_terminate; hc_terms=0;
    es_w32_next=(struct desk_w32_status){DESK_W32_PENDING,11,0x10e4,1};
    desk_session_begin(d,1);                        /* the turn starts at 1 s */
    assert(d->session.count==1 && d->session.parts[0].kind==DESK_PART_WIN32 && d->session.w32_pid==11);
    shutdown_at(3000000000ull); shutdown_at(5900000000ull);
    assert(d->session.state && !d->end_task.kind);
    shutdown_at(6100000000ull);
    assert(d->end_task.kind==DESK_HANG_NOT_RESPONDING && d->end_task.session && d->end_task.pid==11 &&
           !strcmp(d->end_task.name,"Windows program"));
    desk_end_task_choice(d,1);
    assert(hc_terms==1 && hc_killed(11,SIGKILL)==1 && d->session.state);
    es_w32_next.state=DESK_W32_HELD; shutdown_at(6200000000ull);
    assert(!d->session.state && d->power.state==DESK_POWER_DONE && d->power.action==1 && shutdown_calls==1);
    shutdown_cleanup();
    puts("PASS T23 Win32 turn: a windowless process holding WM_QUERYENDSESSION is Not Responding after 5 s, End Task, restart goes on");

    /* 5. The power sequence ends every live Win32 process, also one with no
     * desktop connection (it kept files open: call 73 EBUSY); Force is
     * SIGKILL. Before, that process was nobody's target and the "Some
     * programs are still running" loop never ended. */
    es_setup(); d->terminate=hc_terminate; hc_terms=0;
    hc_w32[0]=11; hc_w32[1]=13; hc_w32_count=2; d->power_w32_pids=hc_w32_pids;
    hc_client(13,"Quake III Arena Demo Setup");
    shutdown_busy_left=-1;
    desk_power_begin(d,2);
    int named=0;
    for (unsigned k=0;k<d->power.count;k++) {
        if (d->power.targets[k].pid==11) { assert(!strcmp(d->power.targets[k].name,"Windows program (PID 11)")); named|=1; }
        if (d->power.targets[k].pid==13) { assert(!strcmp(d->power.targets[k].name,"Quake III Arena Demo Setup")); named|=2; }
    }
    assert(named==3);
    shutdown_at(3900000000ull); assert(!hc_terms);
    shutdown_at(4000000000ull);
    assert(hc_killed(11,SIGTERM)==1 && hc_killed(13,SIGTERM)==1);
    for (uint64_t t=4250000000ull; t<=9500000000ull && d->power.state!=DESK_POWER_BUSY; t+=250000000ull) shutdown_at(t);
    assert(d->power.state==DESK_POWER_BUSY && strstr(d->power.text,"Windows program (PID 11)"));
    desk_power_force(d);
    assert(d->power.state==DESK_POWER_FORCE && hc_killed(11,SIGKILL)==1 && hc_killed(13,SIGKILL)==1);
    desk_power_cancel(d);
    hc_w32_count=0;
    shutdown_cleanup();
    puts("PASS T23 power: every live Win32 process is a target (SIGTERM at the deadline, named in the busy list), Force is SIGKILL");
}
