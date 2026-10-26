/* T23 trial c42f556e (docs/validation/t23-win32-hang-shutdown-2026-10-25.md):
 * four Quake III Arena demo installers at once. The desktop's system dialogs
 * must open however many windows the programs hold, End Task must end a busy
 * program that keeps answering but does not close (Windows HungAppTimeout),
 * a second close request on a "Closing..." window offers End Task at once,
 * the end-of-session Win32 turn must not wait forever on a process with no
 * window, and the power sequence must end every Win32 process (Force with
 * SIGKILL). Cases 6-10 close the Codex review of 63c964019: no automatic
 * SIGTERM while WM_ENDSESSION may run (P1-2), broker errors are not
 * agreement (P1-3), the Win32 inventory keeps its errors (P2-4), Task
 * Manager and application prompts have reserved capacity (P2-5, P2-6), a
 * query pumping without a prompt is bounded (P2-7), and a call 73 that does
 * not return leads to the forced action (P1-1). Included by the production
 * desktop launcher harness after end_session_test.h.
 * SPDX-License-Identifier: MIT */
static unsigned hc_terms; static uint32_t hc_term_pid[32]; static int hc_term_sig[32];
static int hc_terminate(uint32_t pid, int sig)
{ if (hc_terms<32) { hc_term_pid[hc_terms]=pid; hc_term_sig[hc_terms]=sig; } hc_terms++; return 0; }
static unsigned hc_killed(uint32_t pid, int sig)
{ unsigned n=0; for (unsigned i=0;i<hc_terms && i<32;i++) n+=hc_term_pid[i]==pid && hc_term_sig[i]==sig; return n; }
/* A v2 program window; -1 when the desktop refuses the connection. */
static int hc_client_tier(uint32_t pid, const char *name, int tier)
{
    struct desktop *d=&shutdown_desktop;
    int i=0;
    while (i<DESK_CLIENTS && (d->clients[i].channel>=0 || d->clients[i].internal)) i++;
    if (i==DESK_CLIENTS) return -1;
    fake_channel(40+i);
    if (desk_add_client_tier(d,40+i,tier)<0) { fake_ops.close(40+i); return -1; }
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
static int hc_client(uint32_t pid, const char *name) { return hc_client_tier(pid,name,DESK_SLOT_PROGRAM); }
static void hc_key(int code, int value)
{ struct ciuki_input_event e={.type=CIUKI_INPUT_KEY,.code=code,.value=value}; desk_input(&shutdown_desktop,&e); }
static void hc_alt_f4(void) { hc_key(0x38,1); hc_key(0x3e,1); hc_key(0x3e,0); hc_key(0x38,0); }
static uint32_t hc_w32[128]; static unsigned hc_w32_count; static int hc_w32_error; static unsigned hc_w32_lists;
static int hc_w32_pids(struct desktop *d, uint32_t *pids, unsigned cap)
{
    (void)d; hc_w32_lists++;
    if (hc_w32_error) return hc_w32_error;
    if (hc_w32_count>cap) return -ENOSPC;   /* as w32_power_pids: never a silent cut */
    unsigned n=0; for (; n<hc_w32_count; n++) pids[n]=hc_w32[n]; return (int)n;
}
static unsigned hc_forces; static int hc_force_action;
static int hc_force_request(int action) { hc_forces++; hc_force_action=action; errno=EIO; return -1; }
/* Review 2, finding 8: processes below this PID have exited (confirmed gone). */
static uint32_t hc_gone_below;
static int hc_alive(struct desktop *d, uint32_t pid) { (void)d; return pid>=hc_gone_below; }
static void test_t23_hang_close(void)
{
    struct desktop *d=&shutdown_desktop;
    /* The close timeout and End Task deadline can both expire in the same
     * turn. Dismissing the first prompt must not expose the second deadline
     * immediately. The desktop still moves and closes the ghost/dialog. */
    es_setup(); d->terminate=hc_terminate; hc_terms=0;
    int frozen=hc_client(590,"Installer waiting for child");
    assert(!desk_task_end(d,d->clients[frozen].window_id,590));
    d->clients[frozen].unresponsive=1;
    shutdown_at(6000000000ull);
    assert(d->end_task.kind==DESK_HANG_NOT_RESPONDING);
    struct desk_box old_frame=d->clients[frozen].frame;
    desk_move(d,frozen,old_frame.x+17,old_frame.y+11);
    assert(d->clients[frozen].frame.x==old_frame.x+17 && d->clients[frozen].frame.y==old_frame.y+11);
    int retry_prompt=d->end_task.window;
    old_frame=d->clients[retry_prompt].frame;
    desk_move(d,retry_prompt,old_frame.x+9,old_frame.y+7);
    assert(d->clients[retry_prompt].frame.x==old_frame.x+9 && d->clients[retry_prompt].frame.y==old_frame.y+7);
    desk_end_task_choice(d,0);
    shutdown_at(6000000001ull);
    assert(!d->end_task.kind && !hc_terms);
    shutdown_at(10999999999ull);
    assert(!d->end_task.kind && d->clients[frozen].window);
    shutdown_at(11000000000ull);
    assert(d->end_task.kind==DESK_HANG_NOT_RESPONDING);
    desk_end_task_choice(d,1);
    assert(hc_killed(590,SIGKILL)==1);
    shutdown_cleanup();
    puts("PASS fx-q3hang Wait: no repeated prompt before five seconds, End Task still works");
    /* 1. A full client table: programs stop DESK_SYSTEM_SLOTS short of it.
     * The Task Manager the desktop launches on Ctrl+Alt+Del takes the first
     * reserved slot (review P2-5), End Task's "This program is not
     * responding" and Shut Down the last two; About and a second emergency
     * connection are refused. Before, sixteen windows filled the table and
     * both dialogs failed silently; then Task Manager was refused. */
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
    assert(desk_internal_open(d,DESK_INTERNAL_ABOUT)<0);           /* an ordinary built-in never takes a reserve */
    desk_shell_message(d,"Programs","A program did not start.",1);
    assert(d->sh.message_text[0] && desk_free_slots(d)==DESK_SYSTEM_SLOTS);   /* nor does a message box outside a session */
    int tasks=hc_client_tier(700,"Task Manager",DESK_SLOT_TASKMAN);
    assert(tasks>=0 && d->clients[tasks].slot_tier==DESK_SLOT_TASKMAN && desk_free_slots(d)==DESK_SYSTEM_SLOTS-1);
    assert(hc_client_tier(701,"Another",DESK_SLOT_TASKMAN)<0 && d->slots_refused==3);
    desk_request_close(d,slots[3]);
    assert(desk_task_end(d,d->clients[slots[3]].window_id,503)==0);   /* already closing: the dialog at once */
    assert(d->end_task.kind==DESK_HANG_NOT_RESPONDING && d->end_task.window>=0 && d->end_task.pid==503);
    int shut=desk_internal_open(d,DESK_INTERNAL_SHUTDOWN);
    assert(shut>=0 && d->clients[shut].internal==DESK_INTERNAL_SHUTDOWN && desk_free_slots(d)==1);   /* the prompt slot stays */
    assert(desk_internal_open(d,DESK_INTERNAL_RUN)<0);               /* the last slot is not for a built-in */
    desk_drop_client(d,shut);
    desk_end_task_choice(d,1);
    assert(hc_terms==1 && hc_killed(503,SIGKILL)==1);
    shutdown_cleanup();
    puts("PASS T23 slots: programs keep four slots free, Task Manager admitted from its tier, End Task dialog and Shut Down open with the table full, About, Run and a message box refused");

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

    /* 5. The power sequence lists every live Win32 process, also one with no
     * desktop connection (it kept files open: call 73 EBUSY). Started
     * without a session (never asked, review P1-3) they get no automatic
     * SIGTERM: at the close deadline the user is asked, and Force (SIGKILL)
     * is the consent. The list fails first and is fetched again (review
     * P2-4): its error is recorded, never taken as "no programs". Before, the
     * process was nobody's target, a failed list meant zero programs, and
     * SIGTERM went out automatically after 3 s. */
    es_setup(); d->terminate=hc_terminate; hc_terms=0;
    hc_w32[0]=11; hc_w32[1]=13; hc_w32_count=2; hc_w32_error=-EIO; hc_w32_lists=0; d->power_w32_pids=hc_w32_pids;
    hc_client(13,"Quake III Arena Demo Setup");
    shutdown_busy_left=-1;
    desk_power_begin(d,2);
    assert(hc_w32_lists==1 && d->power.w32_list_error==-EIO && d->power.w32_list_retry && d->power.count==1);
    hc_w32_error=0; shutdown_at(1250000000ull);
    assert(hc_w32_lists==2 && !d->power.w32_list_error && d->power.count==2);
    int named=0;
    for (unsigned k=0;k<d->power.count;k++) {
        if (d->power.targets[k].pid==11) { assert(!strcmp(d->power.targets[k].name,"Windows program (PID 11)") && d->power.targets[k].w32); named|=1; }
        if (d->power.targets[k].pid==13) { assert(!strcmp(d->power.targets[k].name,"Quake III Arena Demo Setup") && d->power.targets[k].w32); named|=2; }
    }
    assert(named==3);
    shutdown_at(3900000000ull); assert(!hc_terms && d->power.state==DESK_POWER_CLOSING);
    shutdown_at(4000000000ull);
    assert(!hc_terms && d->power.state==DESK_POWER_BUSY && !shutdown_calls);
    assert(strstr(d->power.text,"Windows program (PID 11)") && strstr(d->power.text,"could not be asked to save"));
    desk_power_force(d);
    assert(d->power.state==DESK_POWER_FORCE && hc_killed(11,SIGKILL)==1 && hc_killed(13,SIGKILL)==1 && hc_w32_lists==3);
    desk_power_cancel(d);
    hc_w32_count=0;
    shutdown_cleanup();
    puts("PASS T23 power: every live Win32 process is a target, a failed list is retried and recorded, unasked programs wait for the user's Force (SIGKILL)");

    /* 6. Review P1-2: after the session's COMMIT the Win32 programs run their
     * WM_ENDSESSION. They are not ended while the broker has not reported
     * every delivery done: a program that saves for 12 s gets its SIGTERM
     * only after ENDED. One that never finishes gets the "still running"
     * dialog at WaitToKillAppTimeout (20 s), Force kills it, a second Force
     * is the forced power action. Before, SIGTERM went out 3 s after COMMIT. */
    es_setup(); d->w32_session=es_w32; d->terminate=hc_terminate; hc_terms=0;
    hc_w32[0]=21; hc_w32_count=1; hc_w32_error=0; d->power_w32_pids=hc_w32_pids;
    es_w32_next=(struct desk_w32_status){DESK_W32_HELD,0,0,0};
    desk_session_begin(d,2);                        /* every window agreed: COMMIT at once */
    assert(es_w32_calls[DESK_W32_SESSION_COMMIT]==1 && !d->session.state && d->power.state==DESK_POWER_CLOSING);
    assert(d->power.w32_asked && !d->power.w32_ended && d->power.count==1 && d->power.targets[0].w32);
    es_w32_next=(struct desk_w32_status){DESK_W32_PENDING,21,0x2000,1};   /* WM_ENDSESSION being handled */
    for (uint64_t t=1250000000ull; t<=13000000000ull; t+=250000000ull) shutdown_at(t);
    assert(!hc_terms && d->power.state==DESK_POWER_CLOSING && !shutdown_calls);   /* 12 s of saving, no SIGTERM */
    es_w32_next.state=DESK_W32_ENDED; shutdown_at(13250000000ull);
    assert(d->power.w32_ended && hc_killed(21,SIGTERM)==1 && hc_terms==1);
    shutdown_external_live=0; shutdown_at(13500000000ull);
    assert(d->power.state==DESK_POWER_DONE && shutdown_calls==1);
    shutdown_cleanup();
    es_setup(); d->w32_session=es_w32; d->terminate=hc_terminate; hc_terms=0; d->power_force_request=hc_force_request; hc_forces=0;
    hc_w32[0]=22; hc_w32_count=1; d->power_w32_pids=hc_w32_pids;
    es_w32_next=(struct desk_w32_status){DESK_W32_HELD,0,0,0};
    desk_session_begin(d,1);
    assert(d->power.state==DESK_POWER_CLOSING && d->power.w32_asked);
    es_w32_next=(struct desk_w32_status){DESK_W32_PENDING,22,0x2200,1};
    for (uint64_t t=1250000000ull; t<=20900000000ull; t+=250000000ull) shutdown_at(t);
    assert(!hc_terms && d->power.state==DESK_POWER_CLOSING);
    shutdown_at(21000000000ull);                    /* WaitToKillAppTimeout from the COMMIT at 1 s */
    assert(!hc_terms && d->power.state==DESK_POWER_BUSY && strstr(d->power.text,"have not finished ending their session"));
    shutdown_busy_left=-1;
    desk_power_force(d);
    assert(hc_killed(22,SIGKILL)==1 && d->power.state==DESK_POWER_FORCE && !d->power.force_power);
    shutdown_at(21250000000ull);
    assert(shutdown_calls==1 && d->power.state==DESK_POWER_BUSY && strstr(d->power.text,"Force again"));
    shutdown_progress=CIUKI_POWER_UNCERTAIN;
    desk_power_force(d);
    assert(d->power.force_power && strstr(d->power.text,"Forcing the computer to restart"));
    shutdown_at(21500000000ull);
    assert(hc_forces==1 && hc_force_action==1 && shutdown_calls==1 && d->power.state==DESK_POWER_DONE && strstr(d->power.text,"could not be restarted automatically"));
    hc_w32_count=0;
    shutdown_cleanup();
    puts("PASS T23 end-session completion: no SIGTERM while WM_ENDSESSION runs, SIGTERM after ENDED, 20 s then the user, second Force is the forced restart");

    /* 7. Review P1-3: a broker error is not an agreement. BEGIN failing is
     * retried for 5 s; then the Win32 programs are unknown: the session goes
     * on without COMMIT, their windows get the ordinary close and the power
     * sequence asks the user instead of ending them. A STATUS failure that
     * recovers within the window changes nothing. Before, the first error
     * marked the Win32 part agreed and SIGTERM followed. */
    es_setup(); d->w32_session=es_w32; d->terminate=hc_terminate; hc_terms=0; es_w32_error=-EIO;
    hc_w32[0]=31; hc_w32_count=1; d->power_w32_pids=hc_w32_pids;
    desk_session_begin(d,2);
    assert(d->session.state && d->session.parts[0].state==DESK_PART_WAITING && es_w32_calls[DESK_W32_SESSION_BEGIN]==1);
    for (uint64_t t=1250000000ull; t<=5900000000ull; t+=250000000ull) shutdown_at(t);
    assert(d->session.state && es_w32_calls[DESK_W32_SESSION_BEGIN]>10 && !d->session.w32_unknown && !d->power.state);
    shutdown_at(6100000000ull);
    assert(!d->session.state && d->session.w32_unknown && !es_w32_calls[DESK_W32_SESSION_COMMIT]);
    assert(d->power.state==DESK_POWER_CLOSING && d->power.w32_unknown && !d->power.w32_asked);
    for (uint64_t t=6250000000ull; t<=9000000000ull; t+=250000000ull) shutdown_at(t);
    assert(!hc_terms && d->power.state==DESK_POWER_CLOSING);   /* the 3 s close grace still runs */
    shutdown_at(9250000000ull);
    assert(!hc_terms && d->power.state==DESK_POWER_BUSY && strstr(d->power.text,"could not be asked to save"));
    desk_power_cancel(d); es_w32_error=0; hc_w32_count=0;
    shutdown_cleanup();
    es_setup(); d->w32_session=es_w32; d->terminate=hc_terminate; hc_terms=0;
    es_w32_next=(struct desk_w32_status){DESK_W32_PENDING,32,0x3200,1};
    desk_session_begin(d,2);
    assert(d->session.state && d->session.parts[0].state==DESK_PART_QUERIED);
    es_w32_error=-EBUSY;                            /* an ExitWindowsEx restart owns the broker for a moment */
    for (uint64_t t=1250000000ull; t<=3000000000ull; t+=250000000ull) shutdown_at(t);
    assert(d->session.state && d->session.parts[0].state==DESK_PART_QUERIED && !d->session.w32_unknown);
    es_w32_error=0; es_w32_next.state=DESK_W32_HELD; shutdown_at(3250000000ull);
    /* committed; with no program left the power call completed in the same tick */
    assert(!d->session.state && es_w32_calls[DESK_W32_SESSION_COMMIT]==1 && d->power.w32_asked && d->power.state==DESK_POWER_DONE && shutdown_calls==1);
    shutdown_cleanup();
    puts("PASS T23 broker errors: BEGIN failing 5 s leaves the Win32 programs unknown (no COMMIT, no SIGTERM, the user decides), a STATUS error that recovers is harmless");

    /* 8. Review P2-7: a Win32 window that fetched its WM_QUERYENDSESSION and
     * keeps pumping messages (PONGs) without a prompt holds the session for
     * WaitToKillAppTimeout, then "not responding" asks the user; a prompt
     * opened after the query is waited for without a deadline. Before, such
     * a handler held the shutdown forever. */
    es_setup(); d->w32_session=es_w32; d->terminate=hc_terminate; hc_terms=0;
    int pumping=es_v3_window(41,"Quake III Arena Demo");   /* open before the session: not a prompt */
    es_w32_next=(struct desk_w32_status){DESK_W32_PENDING,41,0x4100,1};
    desk_session_begin(d,2);
    for (uint64_t t=1250000000ull; t<=20900000000ull; t+=250000000ull) { es_v3_pong(pumping); shutdown_at(t); }
    assert(d->session.state && !d->end_task.kind);
    es_v3_pong(pumping); shutdown_at(21100000000ull);
    assert(d->end_task.kind==DESK_HANG_NOT_RESPONDING && d->end_task.session && d->end_task.pid==41);
    desk_end_task_choice(d,0);                      /* Cancel: everything keeps running */
    assert(!d->session.state && !hc_terms && d->clients[pumping].window);
    desk_drop_client(d,(int)d->clients[pumping].v3_peer-1);
    shutdown_cleanup();
    es_setup(); d->w32_session=es_w32; d->terminate=hc_terminate; hc_terms=0;
    int asking=es_v3_window(42,"Quake III Arena Demo");
    es_w32_next=(struct desk_w32_status){DESK_W32_PENDING,42,0x4200,1};
    desk_session_begin(d,2);
    int save=es_v3_owned(asking,1,DESK_V3_OVERLAPPED|DESK_V3_MODAL,"Save changes?");  /* its prompt, opened after the query */
    assert(save>=0);
    for (uint64_t t=1250000000ull; t<=45000000000ull; t+=250000000ull) { es_v3_pong(asking); es_v3_pong(save); shutdown_at(t); }
    assert(d->session.state && !d->end_task.kind && !hc_terms);
    desk_session_cancel(d);
    desk_drop_client(d,(int)d->clients[asking].v3_peer-1);
    shutdown_cleanup();
    puts("PASS T23 query bound: pumping without a prompt is Not Responding after 20 s (Cancel keeps all), a prompt opened after the query waits without deadline");

    /* 9. Review P2-6: a program being closed may open its modal prompt
     * ("Save changes?", an owned window) from the emergency reserve when the
     * programs have filled their quota; an unowned window, or a program that
     * is not being closed, is still refused. Before, DialogBox returned -1
     * and the installer took it as Next. */
    es_setup(); d->terminate=hc_terminate; hc_terms=0;
    int editor=es_v3_window(51,"CiukWrite"), other=es_v3_window(52,"Other");
    while (desk_free_slots(d)>DESK_SYSTEM_SLOTS) assert(hc_client(600+(uint32_t)desk_free_slots(d),"Filler")>=0);
    assert(desk_free_slots(d)==DESK_SYSTEM_SLOTS);
    assert(es_v3_owned(other,1,DESK_V3_OVERLAPPED|DESK_V3_MODAL,"Prompt of an idle program")==-ENOSPC);
    desk_request_close(d,editor);
    assert(es_v3_owned(editor,0,DESK_V3_OVERLAPPED,"Unowned window")==-ENOSPC);
    assert(es_v3_owned(editor,1,DESK_V3_OVERLAPPED,"Owned but not modal")==-ENOSPC);
    int prompt=es_v3_owned(editor,1,DESK_V3_OVERLAPPED|DESK_V3_MODAL,"Save changes?");
    assert(prompt>=0 && d->clients[prompt].slot_tier==DESK_SLOT_PROMPT && desk_free_slots(d)==DESK_SYSTEM_SLOTS-1);
    assert(es_v3_owned(editor,1,DESK_V3_OVERLAPPED|DESK_V3_MODAL,"A second prompt")==-ENOSPC);   /* one prompt slot */
    int dialog=desk_internal_open(d,DESK_INTERNAL_END_TASK);
    assert(dialog>=0 && desk_free_slots(d)==DESK_SYSTEM_SLOTS-2);
    desk_drop_client(d,dialog);
    shutdown_cleanup();
    puts("PASS T23 prompts: a closing program's owned modal prompt takes its tier, unowned or non-modal windows and idle programs are refused, End Task still opens");

    /* 10. Review P1-1: an orderly call 73 that does not return (a disk
     * operation that never finishes) runs on another thread; the desktop
     * stays live and after 15 s tells the user; Force is the forced restart
     * (CIUKI_POWER_FORCE_REBOOT). Before, the desktop thread blocked inside
     * the kernel and Force was unreachable. */
    es_setup(); d->terminate=hc_terminate; hc_terms=0; d->power_force_request=hc_force_request; hc_forces=0;
    shutdown_in_progress=1;
    desk_power_begin(d,1);                          /* no program left: the call starts at the first tick */
    shutdown_at(1250000000ull);
    assert(shutdown_calls==1 && d->power.state==DESK_POWER_RETRY && d->power.call_started_ns==1250000000ull);
    for (uint64_t t=1500000000ull; t<=16000000000ull; t+=250000000ull) shutdown_at(t);
    assert(shutdown_calls>50 && d->power.state==DESK_POWER_RETRY);
    shutdown_at(16250000000ull);
    assert(d->power.state==DESK_POWER_BUSY && d->power.call_stuck && strstr(d->power.text,"disk operation that has not finished"));
    shutdown_progress=CIUKI_POWER_UNCERTAIN;
    desk_power_force(d);
    assert(d->power.force_power && d->power.state==DESK_POWER_FORCE);
    shutdown_at(16500000000ull);
    assert(hc_forces==1 && hc_force_action==1 && d->power.state==DESK_POWER_DONE && strstr(d->power.text,"could not be restarted automatically"));
    shutdown_cleanup();
    puts("PASS T23 stuck call 73: the desktop stays live, 15 s then the user, Force is the forced restart");

    /* 11. Review 2, finding 6: tier occupancy, not allocation order. With the
     * program quota full, End Task, Shut Down, Task Manager and a closing
     * program's prompt coexist in any order; a fifth taker of any tier is
     * refused. Before, with three shared reserve slots, End Task first
     * refused Task Manager and Task Manager first refused the prompt. */
    for (int order_case=0; order_case<2; order_case++) {
        es_setup(); d->terminate=hc_terminate; hc_terms=0;
        int ed=es_v3_window(81,"CiukWrite");
        while (desk_slot_available(d,DESK_SLOT_PROGRAM)) assert(hc_client(800+(uint32_t)desk_free_slots(d),"Filler")>=0);
        assert(desk_free_slots(d)==DESK_SYSTEM_SLOTS);
        desk_request_close(d,ed);
        int tm=-1,pr=-1,et=-1,sd=-1;
        if (order_case==0) {
            et=desk_internal_open(d,DESK_INTERNAL_END_TASK); sd=desk_internal_open(d,DESK_INTERNAL_SHUTDOWN);
            tm=hc_client_tier(880,"Task Manager",DESK_SLOT_TASKMAN);
            pr=es_v3_owned(ed,1,DESK_V3_OVERLAPPED|DESK_V3_MODAL,"Save changes?");
        } else {
            tm=hc_client_tier(880,"Task Manager",DESK_SLOT_TASKMAN);
            pr=es_v3_owned(ed,1,DESK_V3_OVERLAPPED|DESK_V3_MODAL,"Save changes?");
            et=desk_internal_open(d,DESK_INTERNAL_END_TASK); sd=desk_internal_open(d,DESK_INTERNAL_SHUTDOWN);
        }
        assert(tm>=0 && pr>=0 && et>=0 && sd>=0 && !desk_free_slots(d));
        assert(hc_client_tier(881,"Task Manager again",DESK_SLOT_TASKMAN)<0);
        assert(es_v3_owned(ed,1,DESK_V3_OVERLAPPED|DESK_V3_MODAL,"Another prompt")==-ENOSPC);
        assert(hc_client(882,"Program")<0);
        desk_drop_client(d,et); desk_drop_client(d,sd);
        shutdown_cleanup();
    }
    puts("PASS T23 tiers: Task Manager, the closing program's prompt, End Task and Shut Down coexist in either order; the quotas refuse a fifth");

    /* 12. Review 2, finding 1: End Task on the Win32 program in turn kills
     * it, but a thread in a kernel wait keeps the process and its broker
     * connection alive, so the broker keeps reporting it. After
     * DESK_KILL_WAIT_NS the session stops waiting: the Win32 part is
     * unknown, the power sequence follows and Force (then the forced
     * restart) is reachable. Before, the dialog came back every 5 s and the
     * session never ended. */
    es_setup(); d->w32_session=es_w32; d->terminate=hc_terminate; hc_terms=0; d->power_force_request=hc_force_request; hc_forces=0;
    hc_w32[0]=11; hc_w32_count=1; hc_w32_error=0; d->power_w32_pids=hc_w32_pids;
    es_w32_next=(struct desk_w32_status){DESK_W32_PENDING,11,0x10e4,1};
    desk_session_begin(d,1);
    shutdown_at(6100000000ull);
    assert(d->end_task.kind==DESK_HANG_NOT_RESPONDING && d->end_task.session && d->end_task.pid==11);
    desk_end_task_choice(d,1);
    assert(hc_killed(11,SIGKILL)==1 && d->session.state && d->session.w32_killed_pid==11);
    for (uint64_t t=6250000000ull; t<=11000000000ull; t+=250000000ull) shutdown_at(t);
    assert(d->session.state && !d->end_task.kind);   /* no second dialog while its exit is awaited */
    shutdown_at(11250000000ull);
    assert(!d->session.state && d->session.w32_unknown && d->power.state==DESK_POWER_CLOSING && !d->power.w32_asked);
    for (uint64_t t=11500000000ull; t<=14500000000ull; t+=250000000ull) shutdown_at(t);
    assert(d->power.state==DESK_POWER_BUSY && strstr(d->power.text,"Windows program (PID 11)"));
    shutdown_busy_left=-1;
    desk_power_force(d); shutdown_at(14750000000ull);
    assert(hc_killed(11,SIGKILL)==2 && d->power.state==DESK_POWER_BUSY && strstr(d->power.text,"Force again"));
    shutdown_progress=CIUKI_POWER_UNCERTAIN;
    desk_power_force(d); shutdown_at(15000000000ull);
    assert(hc_forces==1 && hc_force_action==1 && d->power.state==DESK_POWER_DONE);
    hc_w32_count=0;
    shutdown_cleanup();
    puts("PASS T23 End Task survivor: a killed process still reported by the broker is given up after 5 s, the power sequence and Force follow, the forced restart ends it");

    /* 13. Review 2, finding 3: while call 73 runs on its worker (EINPROGRESS)
     * Cancel is refused and not drawn; once the call returned (EBUSY: nothing
     * changed) Cancel works again. Before, Cancel restored the desktop while
     * the kernel could still shut it down. */
    es_setup(); d->terminate=hc_terminate; hc_terms=0;
    shutdown_in_progress=1;
    desk_power_begin(d,1); shutdown_at(1250000000ull);
    assert(shutdown_calls==1 && d->power.call_started_ns && !desk_power_cancelable(d));
    desk_power_cancel(d);
    assert(d->power.state==DESK_POWER_RETRY);         /* refused */
    shutdown_key(0x01);                               /* Esc: ignored */
    assert(d->power.state==DESK_POWER_RETRY);
    shutdown_in_progress=0; shutdown_busy_left=-1; shutdown_at(1500000000ull);
    assert(shutdown_calls==2 && !d->power.call_started_ns && desk_power_cancelable(d));
    desk_power_cancel(d);
    assert(d->power.state==DESK_POWER_IDLE);
    shutdown_cleanup();
    puts("PASS T23 cancel: refused while call 73 runs, possible again after an EBUSY result");

    /* 14. Review 2, finding 4: the broker's BEGIN and the Win32 inventory
     * both fail: no target is known, yet the Win32 programs may be running
     * (a windowless installer). Nothing proceeds on its own: the busy dialog
     * at the close deadline; Force is the consent. Before, the sequence
     * went straight to call 73 and the power-off. */
    es_setup(); d->w32_session=es_w32; d->terminate=hc_terminate; hc_terms=0; es_w32_error=-EIO;
    hc_w32_count=0; hc_w32_error=-EIO; d->power_w32_pids=hc_w32_pids;
    desk_session_begin(d,2);
    for (uint64_t t=1250000000ull; t<=6100000000ull; t+=250000000ull) shutdown_at(t);
    assert(!d->session.state && d->session.w32_unknown && d->power.state==DESK_POWER_CLOSING && !d->power.count);
    for (uint64_t t=6250000000ull; t<=8750000000ull; t+=250000000ull) shutdown_at(t);
    assert(d->power.state==DESK_POWER_CLOSING && !shutdown_calls);   /* the 3 s close grace from 6.0 s */
    shutdown_at(9000000000ull);
    assert(d->power.state==DESK_POWER_BUSY && !shutdown_calls && strstr(d->power.text,"could not be listed"));
    desk_power_force(d); shutdown_at(9500000000ull);
    assert(shutdown_calls==1 && d->power.state==DESK_POWER_DONE);
    es_w32_error=0; hc_w32_error=0;
    shutdown_cleanup();
    /* The list succeeds and shows no Win32 program: BEGIN failing is then harmless. */
    es_setup(); d->w32_session=es_w32; d->terminate=hc_terminate; es_w32_error=-EIO; hc_w32_count=0; d->power_w32_pids=hc_w32_pids;
    desk_session_begin(d,2);
    for (uint64_t t=1250000000ull; t<=6500000000ull; t+=250000000ull) shutdown_at(t);
    assert(d->power.state==DESK_POWER_DONE && shutdown_calls==1 && d->power.w32_listed);
    es_w32_error=0;
    shutdown_cleanup();
    puts("PASS T23 unresolved inventory: failed BEGIN plus failed list stops at the busy dialog (Force proceeds); an empty list lets the shutdown go on");

    /* 15. Review 2, finding 8: the target table reclaims entries whose
     * process is confirmed gone; a live process it cannot hold makes the
     * inventory unresolved (busy dialog), never a silent drop. Before, a
     * full table dropped new live PIDs. */
    es_setup(); d->terminate=hc_terminate; hc_terms=0; d->power_alive=hc_alive; hc_gone_below=0;
    for (unsigned k=0;k<DESK_POWER_TARGETS;k++) hc_w32[k]=1000+k;
    hc_w32_count=DESK_POWER_TARGETS; hc_w32_error=0; d->power_w32_pids=hc_w32_pids;
    desk_power_begin(d,2);
    assert(d->power.count==DESK_POWER_TARGETS && !d->power.targets_overflow && d->power.w32_listed);
    hc_w32_count++;                                   /* more live processes than the broker list carries: an error, not a cut */
    d->power.w32_list_retry=1; shutdown_at(1250000000ull);
    assert(d->power.w32_list_error==-ENOSPC && d->power.w32_list_retry);
    hc_gone_below=1032;                               /* 32 of them exited */
    hc_w32_count=0;
    for (unsigned k=32;k<DESK_POWER_TARGETS;k++) hc_w32[hc_w32_count++]=1000+k;
    for (unsigned k=0;k<32;k++) hc_w32[hc_w32_count++]=3000+k;
    shutdown_at(1500000000ull);                       /* the retry: the gone entries are reclaimed */
    assert(!d->power.w32_list_error && d->power.count==DESK_POWER_TARGETS && !d->power.targets_overflow);
    unsigned held=0; for (unsigned k=0;k<d->power.count;k++) held+=d->power.targets[k].pid>=3000;
    assert(held==32);
    assert(hc_client(4000,"Late program")>=0);        /* connects during the shutdown: no entry can be reclaimed */
    assert(d->power.targets_overflow);
    for (uint64_t t=1750000000ull; t<=4250000000ull; t+=250000000ull) shutdown_at(t);
    assert(d->power.state==DESK_POWER_BUSY && strstr(d->power.text,"could not be tracked") && !shutdown_calls);
    desk_power_cancel(d);
    d->power_alive=shutdown_alive; d->power_w32_pids=NULL; hc_w32_count=0; hc_gone_below=0;
    shutdown_cleanup();
    puts("PASS T23 target table: gone entries reclaimed, an unrepresentable inventory stops at the busy dialog");

    /* 16. Review 2, finding 2: the desktop's own worker (or no thread for
     * the call) refuses the call with EAGAIN: retried at the power cadence,
     * then the busy dialog names it and Force is the way out; the tick is
     * never skipped. Before, EAGAIN cancelled the sequence with an error
     * box and a busy auxiliary worker suppressed the tick. */
    es_setup(); d->terminate=hc_terminate; hc_terms=0; d->power_force_request=hc_force_request; hc_forces=0; d->power_w32_pids=NULL;
    shutdown_eagain=1;
    desk_power_begin(d,2);
    for (uint64_t t=1250000000ull; t<=6000000000ull; t+=250000000ull) shutdown_at(t);
    assert(shutdown_calls>15 && d->power.state==DESK_POWER_RETRY);   /* retried at the power cadence */
    shutdown_at(6250000000ull);                       /* the 5 s busy deadline from the first call */
    assert(d->power.state==DESK_POWER_BUSY && strstr(d->power.text,"desktop has not finished its own work"));
    desk_power_force(d); shutdown_at(6500000000ull);
    assert(d->power.state==DESK_POWER_BUSY && strstr(d->power.text,"Force again"));
    shutdown_progress=CIUKI_POWER_UNCERTAIN;
    desk_power_force(d); shutdown_at(6750000000ull);
    assert(hc_forces==1 && hc_force_action==2 && d->power.state==DESK_POWER_DONE);
    shutdown_cleanup();
    puts("PASS T23 EAGAIN: the desktop's own refusal is retried, named, and Force leads to the forced power off");

    /* 17. Review 2, finding 7: the explicit escalation route. The session
     * waits on a program's real prompt without a deadline; the user's
     * close (Alt+F4) or End Task on one of its windows opens "This program
     * is not responding" for the session at once; End Task ends it and
     * the session goes on. Before, nothing but the prompt's own answer
     * could move the session. */
    es_setup(); d->w32_session=es_w32; d->terminate=hc_terminate; hc_terms=0;
    int app=es_v3_window(91,"Quake III Arena Demo");
    es_w32_next=(struct desk_w32_status){DESK_W32_PENDING,91,0x9100,1};
    desk_session_begin(d,2);
    int ask=es_v3_owned(app,1,DESK_V3_OVERLAPPED|DESK_V3_MODAL,"Save changes?");
    assert(ask>=0);
    for (uint64_t t=1250000000ull; t<=30000000000ull; t+=250000000ull) { es_v3_pong(app); es_v3_pong(ask); shutdown_at(t); }
    assert(d->session.state && !d->end_task.kind);
    desk_focus(d,ask); hc_alt_f4();
    assert(d->end_task.kind==DESK_HANG_NOT_RESPONDING && d->end_task.session && d->end_task.pid==91);
    desk_end_task_choice(d,0);                        /* Cancel: the session is cancelled, everything stays */
    assert(!d->session.state && !hc_terms && d->clients[app].window);
    desk_session_begin(d,2);
    assert(desk_task_end(d,d->clients[app].window_id,91)==0);   /* Task Manager's End Task during the turn */
    assert(d->end_task.kind==DESK_HANG_NOT_RESPONDING && d->end_task.session && d->end_task.pid==91);
    desk_end_task_choice(d,1);
    assert(hc_killed(91,SIGKILL)==1 && d->session.state);
    desk_session_cancel(d);
    desk_drop_client(d,(int)d->clients[app].v3_peer-1);
    shutdown_cleanup();
    puts("PASS T23 escalation: Alt+F4 or End Task on the program the session waits for opens the session's End Task dialog at once");

    /* 18. Review 2, finding 7: an unowned newer window, a non-modal owned
     * window or a hidden modal one is not a prompt: the 20 s bound still
     * applies. Before, any newer window of the process disabled it. */
    es_setup(); d->w32_session=es_w32; d->terminate=hc_terminate; hc_terms=0;
    int prog=es_v3_window(92,"Installer");
    es_w32_next=(struct desk_w32_status){DESK_W32_PENDING,92,0x9200,1};
    desk_session_begin(d,2);
    int progress=es_v3_owned(prog,0,DESK_V3_OVERLAPPED,"Copying files...");
    int owned=es_v3_owned(prog,1,DESK_V3_OVERLAPPED,"Status");
    int hidden=es_v3_owned(prog,1,DESK_V3_OVERLAPPED|DESK_V3_MODAL,"Hidden dialog");
    assert(progress>=0 && owned>=0 && hidden>=0);
    d->clients[hidden].minimized=2;                   /* user32 PLACE flag 2: not visible */
    for (uint64_t t=1250000000ull; t<=20900000000ull; t+=250000000ull) { es_v3_pong(prog); es_v3_pong(progress); es_v3_pong(owned); es_v3_pong(hidden); shutdown_at(t); }
    assert(d->session.state && !d->end_task.kind);
    es_v3_pong(prog); es_v3_pong(progress); es_v3_pong(owned); es_v3_pong(hidden); shutdown_at(21100000000ull);
    assert(d->end_task.kind==DESK_HANG_NOT_RESPONDING && d->end_task.session && d->end_task.pid==92);
    desk_end_task_choice(d,0);
    desk_drop_client(d,(int)d->clients[prog].v3_peer-1);
    shutdown_cleanup();
    puts("PASS T23 prompt recognition: unowned, non-modal or hidden windows do not hold the session past 20 s");

    /* 19. Review 3 finding 1 and review 4 finding 1: the production readiness
     * check (shutdown_wait) with the media worker stuck in a device query
     * that never returns. After 15 s the busy dialog names the desktop's own
     * work; Force runs the production preparation (power_prepare) without
     * joining the stuck worker and the forced action follows. Before, the
     * sequence waited forever; then Force joined the worker and froze. */
    es_setup(); d->terminate=hc_terminate; hc_terms=0; d->power_force_request=hc_force_request; hc_forces=0; d->power_w32_pids=NULL;
    d->sound_shutdown=shutdown_wait;                  /* production: custom/media workers, settings, wallpaper, folder */
    media_available=1; __atomic_store_n(&media_block,1,__ATOMIC_RELEASE);
    observer_open(&d->media_fd,"/dev/media"); assert(media_read_production(d) >= 0 && media_worker.started);
    for (unsigned i=0;i<200 && !media_busy();i++) { struct timespec pause={0,1000000}; nanosleep(&pause,NULL); }
    assert(media_busy());                             /* the worker is inside the blocked query */
    desk_power_begin(d,2);
    for (uint64_t t=1250000000ull; t<=16000000000ull; t+=250000000ull) shutdown_at(t);
    assert(!shutdown_calls && !shutdown_prepares && d->power.state==DESK_POWER_RETRY);
    shutdown_at(16250000000ull);
    assert(d->power.state==DESK_POWER_BUSY && d->power.aux_stuck && strstr(d->power.text,"desktop has not finished its own work"));
    shutdown_progress=CIUKI_POWER_UNCERTAIN;
    desk_power_force(d);
    assert(d->power.force_power);
    shutdown_at(16500000000ull);                      /* power_prepare runs here: it must not join the worker */
    assert(shutdown_prepares==1 && media_worker.started && media_busy() && hc_forces==1 && hc_force_action==2 && d->power.state==DESK_POWER_DONE);
    __atomic_store_n(&media_block,0,__ATOMIC_RELEASE); media_stop(); media_available=0;
    d->sound_shutdown=NULL;
    shutdown_cleanup();
    puts("PASS T23 desktop work bound: a stuck media worker leads to the busy dialog at 15 s, Force prepares without joining it and the forced action runs");

    /* 20. Review 3, finding 2: the 15 s timeout of a call 73 in flight
     * never makes the sequence cancellable: the busy dialog has no Cancel,
     * Esc does nothing, and Force is the way out. Before, the timeout
     * cleared the timer and Cancel reappeared while the worker still ran. */
    es_setup(); d->terminate=hc_terminate; hc_terms=0; d->power_force_request=hc_force_request; hc_forces=0; d->power_w32_pids=NULL;
    shutdown_in_progress=1;
    desk_power_begin(d,1);
    for (uint64_t t=1250000000ull; t<=16250000000ull; t+=250000000ull) shutdown_at(t);
    assert(d->power.state==DESK_POWER_BUSY && d->power.call_stuck && d->power.worker_inflight && !desk_power_cancelable(d));
    desk_power_cancel(d); shutdown_key(0x01);
    assert(d->power.state==DESK_POWER_BUSY);
    shutdown_progress=CIUKI_POWER_UNCERTAIN;
    desk_power_force(d); shutdown_at(16500000000ull);
    assert(hc_forces==1 && d->power.state==DESK_POWER_DONE);
    shutdown_cleanup();
    puts("PASS T23 stuck call: never cancellable while the worker is in flight, Force is the way out");

    /* 21. Review 3, finding 6: the dialog the user asked for stays open
     * while the program's prompt keeps answering (ticks pass); it closes
     * when the turn moves to another window, and End Task ends the program.
     * Before, the next session tick dismissed it at once. */
    es_setup(); d->w32_session=es_w32; d->terminate=hc_terminate; hc_terms=0;
    int app2=es_v3_window(93,"Quake III Arena Demo");
    es_w32_next=(struct desk_w32_status){DESK_W32_PENDING,93,0x9300,1};
    desk_session_begin(d,2);
    int ask2=es_v3_owned(app2,1,DESK_V3_OVERLAPPED|DESK_V3_MODAL,"Save changes?");
    assert(ask2>=0);
    for (uint64_t t=1250000000ull; t<=10000000000ull; t+=250000000ull) { es_v3_pong(app2); es_v3_pong(ask2); shutdown_at(t); }
    desk_focus(d,ask2); hc_alt_f4();
    assert(d->end_task.kind==DESK_HANG_NOT_RESPONDING && d->end_task.session && d->session.escalated);
    for (uint64_t t=10250000000ull; t<=20000000000ull; t+=250000000ull) { es_v3_pong(app2); es_v3_pong(ask2); shutdown_at(t); }
    assert(d->end_task.kind==DESK_HANG_NOT_RESPONDING && d->end_task.session && d->end_task.pid==93);   /* still there */
    es_w32_next=(struct desk_w32_status){DESK_W32_PENDING,94,0x9400,1};   /* the program answered: the turn moved on */
    shutdown_at(20250000000ull);
    assert(!d->end_task.kind && !d->session.escalated && d->session.w32_pid==94);
    es_w32_next=(struct desk_w32_status){DESK_W32_PENDING,93,0x9300,1};
    shutdown_at(20500000000ull);
    desk_focus(d,ask2); hc_alt_f4();
    assert(d->end_task.kind==DESK_HANG_NOT_RESPONDING && d->session.escalated);
    es_v3_pong(app2); es_v3_pong(ask2); shutdown_at(20750000000ull);
    assert(d->end_task.kind==DESK_HANG_NOT_RESPONDING);
    desk_end_task_choice(d,1);
    assert(hc_killed(93,SIGKILL)==1 && !d->session.escalated && d->session.state);
    desk_session_cancel(d);
    desk_drop_client(d,(int)d->clients[app2].v3_peer-1);
    shutdown_cleanup();
    puts("PASS T23 escalation latch: the user's End Task dialog survives ticks with a responsive prompt, closes when the turn moves on, End Task ends the program");

    /* 22. Review 4, finding 2: the escalated dialog never outlives its
     * participant. The broker abandoning the Win32 turn dismisses it (the
     * native participant that follows gets its own query untouched), and an
     * answer whose participant is no longer the current one is only
     * dismissed. Before, the dialog survived and End Task marked the next
     * native participant gone. */
    es_setup(); d->w32_session=es_w32; d->terminate=hc_terminate; hc_terms=0;
    int app3=es_v3_window(95,"Quake III Arena Demo");
    { struct ciuki_message hw; desk_v3_message(&hw,DESK_V3_WIN32_HWND,4,++es_v3_token,d->clients[app3].v3_id,d->clients[app3].v3_generation);
      desk_put32(hw.data+16,0x9500); hw.sender_pid=95; assert(!desk_v3_receive(d,(int)d->clients[app3].v3_peer-1,&hw)); }
    int editor2=shutdown_client(96,"CiukNote",1,2); es_register(editor2,96);
    desk_focus(d,app3);                               /* the Win32 turn comes first, the native editor after it */
    es_w32_next=(struct desk_w32_status){DESK_W32_PENDING,95,0x9500,1};
    desk_session_begin(d,2);
    assert(d->session.count==2 && d->session.parts[0].kind==DESK_PART_WIN32 && d->session.parts[1].kind==DESK_PART_WINDOW);
    int ask3=es_v3_owned(app3,1,DESK_V3_OVERLAPPED|DESK_V3_MODAL,"Save changes?");
    assert(ask3>=0);
    shutdown_at(1250000000ull);
    desk_focus(d,ask3); hc_alt_f4();
    assert(d->end_task.kind==DESK_HANG_NOT_RESPONDING && d->session.escalated && d->end_task.pid==95);
    es_w32_error=-EIO;                                /* the broker goes away: retried 5 s, then abandoned */
    for (uint64_t t=1500000000ull; t<=6750000000ull; t+=250000000ull) { es_v3_pong(app3); es_v3_pong(ask3); shutdown_at(t); }
    assert(!d->end_task.kind && !d->session.escalated && !hc_terms);
    assert(d->session.state && d->session.at==1 && es_count(editor2,DESK_QUERY_END_SESSION)==1);   /* the editor's own query */
    es_w32_error=0;
    desk_session_cancel(d);
    /* An answer for a participant that is no longer current is dismissed. */
    es_w32_next=(struct desk_w32_status){DESK_W32_PENDING,95,0x9500,1};
    d->clients[app3].order=++d->order;                /* the editor's query had raised it: the Win32 window on top again */
    desk_session_begin(d,2);
    assert(d->session.parts[0].kind==DESK_PART_WIN32 && d->session.w32_pid==95);
    desk_focus(d,ask3); hc_alt_f4();
    assert(d->end_task.kind==DESK_HANG_NOT_RESPONDING && d->end_task.pid==95);
    d->end_task.pid=12345;                            /* a dialog that outlived its participant */
    desk_end_task_choice(d,1);
    assert(!hc_terms && !d->end_task.kind && d->session.state && d->session.at==0);
    desk_session_cancel(d);
    desk_drop_client(d,(int)d->clients[app3].v3_peer-1);
    shutdown_cleanup();
    puts("PASS T23 escalation lifetime: the dialog ends with its participant (broker abandonment), a stale answer is dismissed");
}
