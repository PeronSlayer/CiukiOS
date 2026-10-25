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
/* A second v3 window on the connection of `owner` (its process's prompt when
 * owned); -ENOSPC when the desktop refuses it. */
static int hc_v3_window(int owner, int owned, const char *title)
{
    struct desktop *d=&shutdown_desktop; struct desk_client *o=&d->clients[owner];
    int root=(int)o->v3_peer-1; uint32_t pid=o->pid;
    unsigned id=1; for (int k=0;k<DESK_CLIENTS;k++) if (d->clients[k].v3_peer==o->v3_peer && d->clients[k].v3_id>=id) id=d->clients[k].v3_id+1;
    fake_surface(90+(int)id,160,80,0x775533u);
    unsigned n=(unsigned)strlen(title); struct ciuki_message m;
    desk_v3_message(&m,DESK_CREATE_WINDOW,36+n,++es_v3_token,id,1); m.sender_pid=pid; m.fd_count=1; m.fds[0]=90+(int)id;
    desk_put32(m.data+16,DESK_V3_OVERLAPPED); desk_put32(m.data+20,owned ? o->v3_id : 0); desk_put32(m.data+24,owned ? o->v3_generation : 0);
    desk_put32(m.data+28,60); desk_put32(m.data+32,60); desk_put32(m.data+36,160); desk_put32(m.data+40,80);
    desk_put32(m.data+44,1); desk_put32(m.data+48,n); memcpy(m.data+52,title,n);
    int e=desk_v3_receive(d,root,&m);
    if (e) return e;   /* the desktop closed the refused surface */
    for (int k=0;k<DESK_CLIENTS;k++)
        if (d->clients[k].window && d->clients[k].v3_peer==o->v3_peer && d->clients[k].v3_id==id) return k;
    assert(0); return -1;
}
static void hc_key(int code, int value)
{ struct ciuki_input_event e={.type=CIUKI_INPUT_KEY,.code=code,.value=value}; desk_input(&shutdown_desktop,&e); }
static void hc_alt_f4(void) { hc_key(0x38,1); hc_key(0x3e,1); hc_key(0x3e,0); hc_key(0x38,0); }
static uint32_t hc_w32[4]; static unsigned hc_w32_count; static int hc_w32_error; static unsigned hc_w32_lists;
static int hc_w32_pids(struct desktop *d, uint32_t *pids, unsigned cap)
{
    (void)d; hc_w32_lists++;
    if (hc_w32_error) return hc_w32_error;
    unsigned n=0; for (; n<hc_w32_count && n<cap; n++) pids[n]=hc_w32[n]; return (int)n;
}
static unsigned hc_forces; static int hc_force_action;
static int hc_force_request(int action) { hc_forces++; hc_force_action=action; errno=EIO; return -1; }
static void test_t23_hang_close(void)
{
    struct desktop *d=&shutdown_desktop;
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
    int tasks=hc_client_tier(700,"Task Manager",DESK_SLOT_EMERGENCY);
    assert(tasks>=0 && desk_free_slots(d)==DESK_SYSTEM_SLOTS-1);
    assert(hc_client_tier(701,"Another",DESK_SLOT_EMERGENCY)<0 && d->slots_refused==3);
    desk_request_close(d,slots[3]);
    assert(desk_task_end(d,d->clients[slots[3]].window_id,503)==0);   /* already closing: the dialog at once */
    assert(d->end_task.kind==DESK_HANG_NOT_RESPONDING && d->end_task.window>=0 && d->end_task.pid==503);
    int shut=desk_internal_open(d,DESK_INTERNAL_SHUTDOWN);
    assert(shut>=0 && d->clients[shut].internal==DESK_INTERNAL_SHUTDOWN && !desk_free_slots(d));
    desk_drop_client(d,shut);
    desk_end_task_choice(d,1);
    assert(hc_terms==1 && hc_killed(503,SIGKILL)==1);
    shutdown_cleanup();
    puts("PASS T23 slots: programs keep three slots free, Task Manager admitted from the reserve, End Task dialog and Shut Down open with the table full, About and a message box refused");

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
    int save=hc_v3_window(asking,1,"Save changes?");  /* its prompt, opened after the query */
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
    assert(hc_v3_window(other,1,"Prompt of an idle program")==-ENOSPC);
    desk_request_close(d,editor);
    assert(hc_v3_window(editor,0,"Unowned window")==-ENOSPC);
    int prompt=hc_v3_window(editor,1,"Save changes?");
    assert(prompt>=0 && desk_free_slots(d)==DESK_SYSTEM_SLOTS-1);
    assert(hc_v3_window(editor,1,"A second prompt")==-ENOSPC);   /* the reserve for dialogs is kept */
    int dialog=desk_internal_open(d,DESK_INTERNAL_END_TASK);
    assert(dialog>=0 && desk_free_slots(d)==DESK_SYSTEM_SLOTS-2);
    desk_drop_client(d,dialog);
    shutdown_cleanup();
    puts("PASS T23 prompts: a closing program's owned prompt takes the emergency slot, unowned windows and idle programs are refused, End Task still opens");

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
}
