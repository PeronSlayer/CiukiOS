static unsigned es_logoffs;
static void es_logoff(struct desktop *desktop) { es_logoffs++; desk_logon_open(desktop,1); }
/* G3 end of session and window close without data loss (Windows 9x
 * WM_QUERYENDSESSION / WM_ENDSESSION, "This program is not responding",
 * the MS-DOS "cannot shut down this program automatically" warning).
 * Included by the production desktop launcher harness after
 * shutdown_sequence_test.h. SPDX-License-Identifier: MIT */
static unsigned es_terms; static uint32_t es_term_pid; static int es_term_sig;
static int es_terminate(uint32_t pid, int sig) { es_terms++; es_term_pid=pid; es_term_sig=sig; return 0; }
static unsigned es_w32_calls[8]; static uint32_t es_w32_flags; static struct desk_w32_status es_w32_next;
static int es_w32_error;   /* review P1-3: BEGIN/STATUS fail with this -errno while set */
static int es_w32(struct desktop *d, unsigned action, uint32_t flags, struct desk_w32_status *st)
{
    (void)d; assert(action<8 && st); es_w32_calls[action]++;
    if (es_w32_error && (action==DESK_W32_SESSION_BEGIN || action==DESK_W32_SESSION_STATUS)) return es_w32_error;
    if (action==DESK_W32_SESSION_BEGIN) es_w32_flags=flags;
    *st=action==DESK_W32_SESSION_STATUS ? es_w32_next : (struct desk_w32_status){0};
    return 0;
}
static void es_setup(void)
{
    shutdown_setup();
    shutdown_desktop.terminate=es_terminate; es_terms=es_term_pid=0; es_term_sig=0;
    memset(es_w32_calls,0,sizeof(es_w32_calls)); es_w32_flags=0; memset(&es_w32_next,0,sizeof(es_w32_next)); es_w32_error=0;
}
static void es_send(int i, uint32_t pid, unsigned op, unsigned length, uint32_t a, uint32_t b)
{
    struct ciuki_message m; desk_message(&m,op,length);
    m.data[0]=(uint8_t)shutdown_desktop.clients[i].version; m.sender_pid=pid;
    if (length>=8) desk_put32(m.data+4,a);
    if (length>=12) desk_put32(m.data+8,b);
    desk_receive(&shutdown_desktop,i,&m);
}
static void es_register(int i, uint32_t pid)
{ es_send(i,pid,DESK_SESSION_ANSWER,12,0,DESK_SESSION_AGREE); assert(shutdown_desktop.clients[i].session_aware); }
static unsigned es_count(int i, unsigned op) { return queued(&shutdown_desktop,i,op,NULL); }
static struct desk_packet es_last(int i, unsigned op)
{
    static struct desk_packet p[DESK_TX_MAX]; unsigned n=queued(&shutdown_desktop,i,op,p);
    assert(n); return p[n-1];
}
/* The answer to the query in turn (its serial survives drained rings). */
static void es_answer(int i, uint32_t pid, uint32_t answer)
{
    const struct desk_session *s=&shutdown_desktop.session;
    assert(s->state==DESK_SESSION_QUERY && s->at<s->count && s->parts[s->at].window_id==shutdown_desktop.clients[i].window_id);
    es_send(i,pid,DESK_SESSION_ANSWER,12,s->parts[s->at].serial,answer);
}
static void es_pong(int i, uint32_t pid)
{
    struct desk_client *c=&shutdown_desktop.clients[i];
    if (c->window && c->ping_pending) es_send(i,pid,DESK_PONG,8,c->serial,0);
}
/* Time passes with the listed programs answering PINGs (pid 0 ends the list). */
static void es_run(uint64_t from, uint64_t to, const int *slots, const uint32_t *pids)
{
    for (uint64_t t=from; t<=to; t+=250000000ull) {
        for (unsigned k=0; pids[k]; k++) { drain(&shutdown_desktop,slots[k]); es_pong(slots[k],pids[k]); }
        shutdown_at(t);
    }
}
/* T23 c42f556e: a Win32 program's own prompt is a window of its process
 * (a v3 window on its own connection); it answers PINGs while it is shown. */
static unsigned es_v3_token;
static int es_v3_window(uint32_t pid, const char *title)
{
    struct desktop *d=&shutdown_desktop;
    int root=0;
    while (root<DESK_CLIENTS && (d->clients[root].channel>=0 || d->clients[root].internal)) root++;
    assert(root<DESK_CLIENTS);
    fake_channel(40+root); assert(desk_add_client(d,40+root)==root);
    struct ciuki_message m; desk_v3_message(&m,DESK_HELLO,0,0,0,0); m.sender_pid=pid;
    assert(!desk_v3_receive(d,root,&m));
    fake_surface(60+root,160,80,0x335577u);
    unsigned n=(unsigned)strlen(title);
    desk_v3_message(&m,DESK_CREATE_WINDOW,36+n,++es_v3_token,1,1); m.sender_pid=pid; m.fd_count=1; m.fds[0]=60+root;
    desk_put32(m.data+16,DESK_V3_OVERLAPPED); desk_put32(m.data+20,0); desk_put32(m.data+24,0);
    desk_put32(m.data+28,40); desk_put32(m.data+32,40); desk_put32(m.data+36,160); desk_put32(m.data+40,80);
    desk_put32(m.data+44,1); desk_put32(m.data+48,n); memcpy(m.data+52,title,n);
    assert(!desk_v3_receive(d,root,&m));
    for (int k=0;k<DESK_CLIENTS;k++)
        if (d->clients[k].window && d->clients[k].version==DESK_V3_VERSION && d->clients[k].v3_peer==(unsigned)root+1) return k;
    assert(0); return -1;
}
/* A second v3 window on the connection of `owner`: owned (its process's
 * prompt when DESK_V3_MODAL) or not; -ENOSPC when the desktop refuses it. */
static int es_v3_owned(int owner, int owned, uint32_t style, const char *title)
{
    struct desktop *d=&shutdown_desktop; struct desk_client *o=&d->clients[owner];
    int root=(int)o->v3_peer-1; uint32_t pid=o->pid;
    unsigned id=1; for (int k=0;k<DESK_CLIENTS;k++) if (d->clients[k].v3_peer==o->v3_peer && d->clients[k].v3_id>=id) id=d->clients[k].v3_id+1;
    fake_surface(90+(int)id,160,80,0x775533u);
    unsigned n=(unsigned)strlen(title); struct ciuki_message m;
    desk_v3_message(&m,DESK_CREATE_WINDOW,36+n,++es_v3_token,id,1); m.sender_pid=pid; m.fd_count=1; m.fds[0]=90+(int)id;
    desk_put32(m.data+16,style); desk_put32(m.data+20,owned ? o->v3_id : 0); desk_put32(m.data+24,owned ? o->v3_generation : 0);
    desk_put32(m.data+28,60); desk_put32(m.data+32,60); desk_put32(m.data+36,160); desk_put32(m.data+40,80);
    desk_put32(m.data+44,1); desk_put32(m.data+48,n); memcpy(m.data+52,title,n);
    int e=desk_v3_receive(d,root,&m);
    if (e) return e;   /* the desktop closed the refused surface */
    for (int k=0;k<DESK_CLIENTS;k++)
        if (d->clients[k].window && d->clients[k].v3_peer==o->v3_peer && d->clients[k].v3_id==id) return k;
    assert(0); return -1;
}
static void es_v3_pong(int i)
{
    struct desktop *d=&shutdown_desktop; struct desk_client *c=&d->clients[i];
    int root=(int)c->v3_peer-1;
    drain(d,root);
    if (!c->window || !c->ping_pending) return;
    struct ciuki_message m; desk_v3_message(&m,DESK_PONG,4,0,c->v3_id,c->v3_generation);
    desk_put32(m.data+16,c->serial); m.sender_pid=c->pid;
    assert(!desk_v3_receive(d,root,&m));
}
static int es_intact(const int *slots, unsigned n)
{
    const struct desktop *d=&shutdown_desktop;
    if (d->session.state || d->power.state || d->end_task.kind) return 0;
    for (unsigned k=0; k<n; k++) if (!d->clients[slots[k]].window || d->clients[slots[k]].closing) return 0;
    return 1;
}
static void test_end_session(void)
{
    struct desktop *d=&shutdown_desktop;
    /* 1. Topmost first; an unregistered v2 and a v1 window agree as DefWindowProc
     * does; a program shows its save prompt and the session waits a minute for
     * the user with no termination; then all agreed: END_SESSION(1), the power
     * call once. */
    es_setup();
    int a=shutdown_client(301,"CiukNote",1,2), b=shutdown_client(302,"Files",1,2);
    int legacy=shutdown_client(303,"Clock",1,1), plain=shutdown_client(304,"Demo",1,2);
    es_register(a,301); es_register(b,302);
    desk_session_begin(d,2);
    assert(d->session.state==DESK_SESSION_QUERY && !d->power.state && d->session.count==4);
    assert(es_count(b,DESK_QUERY_END_SESSION)==1 && !es_count(a,DESK_QUERY_END_SESSION) && d->focus==b);
    assert(!es_count(plain,DESK_QUERY_END_SESSION) && !es_count(legacy,DESK_QUERY_END_SESSION));
    assert(es_last(b,DESK_QUERY_END_SESSION).flags==0);
    es_answer(b,302,DESK_SESSION_AGREE);
    assert(es_count(a,DESK_QUERY_END_SESSION)==1 && d->focus==a);
    es_answer(a,301,DESK_SESSION_ASKING);
    { int s[]={a,b,legacy,plain}; uint32_t p[]={301,302,303,304,0};
      es_run(1250000000ull,61000000000ull,s,p); }
    assert(d->session.state && !d->power.state && !es_terms && !shutdown_calls && !d->end_task.kind && d->clients[a].window);
    es_answer(a,301,DESK_SESSION_AGREE);           /* the user saved */
    assert(!d->session.state && d->power.state==DESK_POWER_CLOSING);
    static struct desk_packet end;end=es_last(a,DESK_END_SESSION); assert(end.focused==1 && !end.flags);
    assert(es_count(b,DESK_END_SESSION)==1 && es_last(b,DESK_END_SESSION).focused==1);
    assert(es_count(plain,DESK_CLOSE_REQUEST)==1 && !es_count(plain,DESK_END_SESSION));
    assert(es_count(legacy,DESK_CLOSED)==1 && !es_count(a,DESK_CLOSE_REQUEST));
    for (uint32_t pid=301; pid<=304; pid++) shutdown_reap(pid,0);
    shutdown_at(61500000000ull); assert(shutdown_calls==1 && d->power.state==DESK_POWER_DONE && !es_terms);
    shutdown_at(62000000000ull); assert(shutdown_calls==1);
    shutdown_cleanup();
    puts("PASS G3 native: topmost first, default agreement, save prompt waited 60 s with no kill, END_SESSION(1), power call once");

    /* 2. A veto after another program agreed: WM_ENDSESSION(FALSE) to the one
     * that agreed, none to the vetoer, every program still running, no power. */
    es_setup();
    a=shutdown_client(311,"CiukNote",1,2); b=shutdown_client(312,"Paint",1,2);
    es_register(a,311); es_register(b,312);
    desk_session_begin(d,1);
    es_answer(b,312,DESK_SESSION_AGREE); es_answer(a,311,DESK_SESSION_ASKING);
    es_answer(a,311,DESK_SESSION_VETO);            /* Cancel in the program's prompt */
    { int s[]={a,b}; assert(es_intact(s,2)); }
    assert(es_count(b,DESK_END_SESSION)==1 && !es_last(b,DESK_END_SESSION).focused && !es_count(a,DESK_END_SESSION));
    { int s[]={a,b}; uint32_t p[]={311,312,0}; es_run(1250000000ull,20000000000ull,s,p); assert(es_intact(s,2)); }
    assert(!es_terms && !shutdown_calls && !shutdown_prepares);
    desk_session_begin(d,2); assert(d->session.state==DESK_SESSION_QUERY);   /* a new session may start */
    desk_session_cancel(d); assert(!d->session.state);
    shutdown_cleanup();
    puts("PASS G3 veto: END_SESSION(0) to the agreed program, none to the vetoer, programs intact, no power");

    /* 3. A hung program: no answer and no PONG for the 5 s HungAppTimeout ->
     * "This program is not responding"; Cancel keeps everything, End Task
     * terminates it (SIGKILL) and the session goes on. A recovered program's
     * PONG dismisses the dialog and counts as DefWindowProc's TRUE. */
    es_setup();
    a=shutdown_client(321,"Hung",1,2); es_register(a,321);
    desk_session_begin(d,2);
    shutdown_at(5900000000ull); assert(!d->end_task.kind);
    shutdown_at(6000000000ull);
    assert(d->end_task.kind==DESK_HANG_NOT_RESPONDING && d->end_task.session && d->end_task.pid==321);
    int dialog=d->end_task.window;
    assert(d->clients[dialog].internal==DESK_INTERNAL_END_TASK && !strcmp(d->clients[dialog].title,"Hung") && d->focus==dialog);
    desk_redraw(d);
    shutdown_key(DU_KEY_ENTER);                     /* the default button is Cancel */
    { int s[]={a}; assert(es_intact(s,1) && !es_terms); }
    desk_session_begin(d,2); shutdown_at(13000000000ull);
    assert(d->end_task.kind==DESK_HANG_NOT_RESPONDING && d->end_task.session);
    shutdown_key(0x12);                             /* E: End Task */
    assert(es_terms==1 && es_term_pid==321 && es_term_sig==SIGKILL && !d->end_task.kind);
    assert(!d->session.state && d->power.state==DESK_POWER_CLOSING);
    shutdown_reap(321,0); shutdown_at(13500000000ull); assert(shutdown_calls==1);
    shutdown_cleanup();
    es_setup();
    a=shutdown_client(322,"Busy",1,2); es_register(a,322);
    desk_session_begin(d,2); shutdown_at(6000000000ull);
    assert(d->end_task.kind==DESK_HANG_NOT_RESPONDING);
    es_pong(a,322);                                  /* it recovers and processed the query */
    shutdown_at(6100000000ull);
    assert(!d->end_task.kind && !d->session.state && d->power.state==DESK_POWER_CLOSING && !es_terms);
    assert(es_count(a,DESK_END_SESSION)==1);
    shutdown_cleanup();
    puts("PASS G3 hung: Not Responding after 5 s, Cancel keeps all, End Task SIGKILL then power, recovery dismisses");
    /* G11 battery alarm with "Force standby or shutdown even if a program
     * stops responding": the hung program is ended as End Task would. */
    es_setup();
    a=shutdown_client(323,"Hung",1,2); es_register(a,323);
    desk_session_begin(d,2); d->session.force=1;
    shutdown_at(5900000000ull); assert(!d->end_task.kind && !es_terms);
    shutdown_at(6000000000ull);
    assert(es_terms==1 && es_term_pid==323 && es_term_sig==SIGKILL && !d->end_task.kind);
    assert(!d->session.state && d->power.state==DESK_POWER_CLOSING);
    shutdown_reap(323,0); shutdown_at(6500000000ull); assert(shutdown_calls==1);
    shutdown_cleanup();
    puts("PASS G11 alarm Force: a program that stops responding during the alarm's shutdown is ended, power follows");

    /* 4. A DOS window whose program runs: the Windows 98 warning. No cancels
     * the shutdown; Yes agrees and the window gets END_SESSION(1). */
    es_setup();
    int dos=shutdown_client(331,"DOOM",1,2); es_register(dos,331);
    desk_session_begin(d,2); es_answer(dos,331,DESK_SESSION_DOS_ACTIVE);
    assert(d->end_task.kind==DESK_HANG_DOS && d->end_task.session && !strcmp(d->clients[d->end_task.window].title,"DOOM"));
    desk_redraw(d);
    shutdown_key(0x31);                              /* N: No */
    { int s[]={dos}; assert(es_intact(s,1) && !es_terms); }
    desk_session_begin(d,2); es_answer(dos,331,DESK_SESSION_DOS_ACTIVE);
    { int s[]={dos}; uint32_t p[]={331,0}; es_run(1000000000ull,30000000000ull,s,p); }
    assert(d->end_task.kind==DESK_HANG_DOS && !es_terms);   /* the warning waits for the user */
    shutdown_key(0x15);                              /* Y: Yes */
    assert(!d->end_task.kind && d->power.state==DESK_POWER_CLOSING && es_last(dos,DESK_END_SESSION).focused==1 && !es_terms);
    shutdown_cleanup();
    /* Closing the DOS window itself: CLOSE_ACK DOS_ACTIVE -> warning; No keeps it, Yes ends it. */
    es_setup();
    dos=shutdown_client(332,"Duke",1,2);
    desk_request_close(d,dos); assert(d->clients[dos].closing);
    es_send(dos,332,DESK_CLOSE_ACK,8,DESK_CLOSE_DOS_ACTIVE,0);
    assert(d->end_task.kind==DESK_HANG_DOS && !d->end_task.session);
    desk_end_task_choice(d,0); assert(!d->end_task.kind && !d->clients[dos].closing && !es_terms);
    desk_request_close(d,dos); es_send(dos,332,DESK_CLOSE_ACK,8,DESK_CLOSE_DOS_ACTIVE,0);
    desk_end_task_choice(d,1); assert(es_terms==1 && es_term_pid==332 && es_term_sig==SIGKILL);
    shutdown_cleanup();
    puts("PASS G3 DOS: shutdown warning No cancels and Yes agrees (no deadline), window close warning No/Yes");

    /* 5. Win32 programs through the broker turn: BEGIN with EWX and HOLD; a
     * WM_QUERYENDSESSION FALSE cancels (the agreed native gets END_SESSION(0));
     * a USER thread that never fetches its message is hung after 5 s and End
     * Task kills its process; all agreed: COMMIT once, then power. */
    es_setup(); d->w32_session=es_w32;
    b=shutdown_client(341,"Files",1,2); es_register(b,341);
    desk_session_begin(d,2); assert(d->session.count==2 && d->session.parts[1].kind==DESK_PART_WIN32);
    es_answer(b,341,DESK_SESSION_AGREE);
    assert(es_w32_calls[DESK_W32_SESSION_BEGIN]==1 && es_w32_flags==(DESK_EWX_SHUTDOWN|DESK_EWX_POWEROFF|DESK_W32_SESSION_HOLD));
    es_w32_next=(struct desk_w32_status){DESK_W32_PENDING,500,0x1234,1};   /* its save prompt is open */
    /* T23 c42f556e: the prompt is a modal dialog owned by a window of pid 500 (review 2, finding 7) */
    int program=es_v3_window(500,"Program"), prompt=es_v3_owned(program,1,DESK_V3_OVERLAPPED|DESK_V3_MODAL,"Save changes?");
    assert(prompt>=0);
    for (uint64_t t=1000000000ull; t<=40000000000ull; t+=250000000ull) {
        drain(d,b); es_pong(b,341); es_v3_pong(program); es_v3_pong(prompt); shutdown_at(t);
    }
    assert(d->session.state && !d->end_task.kind && !es_terms && es_w32_calls[DESK_W32_SESSION_STATUS]>100);
    es_w32_next.state=DESK_W32_VETOED; shutdown_at(40100000000ull);
    { int s[]={b}; assert(es_intact(s,1)); }
    assert(es_last(b,DESK_END_SESSION).focused==0 && es_w32_calls[DESK_W32_SESSION_CANCEL]==1 && !es_w32_calls[DESK_W32_SESSION_COMMIT]);
    assert(!shutdown_calls && !es_terms);
    desk_drop_client(d,(int)d->clients[program].v3_peer-1);   /* Cancel closed the program's prompt */
    memset(es_w32_calls,0,sizeof(es_w32_calls));
    es_w32_next=(struct desk_w32_status){DESK_W32_PENDING,501,0x1240,0};
    desk_session_begin(d,1); es_answer(b,341,DESK_SESSION_AGREE);
    assert(es_w32_flags==(DESK_EWX_REBOOT|DESK_W32_SESSION_HOLD));
    shutdown_at(41000000000ull); assert(!d->end_task.kind);
    shutdown_at(46100000000ull);
    assert(d->end_task.kind==DESK_HANG_NOT_RESPONDING && d->end_task.pid==501 && !strcmp(d->end_task.name,"Windows program"));
    desk_end_task_choice(d,1); assert(es_terms==1 && es_term_pid==501 && es_term_sig==SIGKILL && d->session.state);
    es_w32_next.state=DESK_W32_HELD; shutdown_at(46200000000ull);
    assert(es_w32_calls[DESK_W32_SESSION_COMMIT]==1 && !es_w32_calls[DESK_W32_SESSION_CANCEL]);
    assert(!d->session.state && d->power.state==DESK_POWER_CLOSING && d->power.action==1);
    shutdown_reap(341,0); shutdown_at(46500000000ull); assert(shutdown_calls==1);
    shutdown_cleanup();
    puts("PASS G3 Win32: broker turn with EWX|HOLD, FALSE cancels with END_SESSION(0), unfetched query hung -> End Task, COMMIT once then power");

    /* 6. Closing one window (WM_CLOSE) and End Task from Task Manager: no
     * deadline while the program answers PINGs (its prompt may stay open);
     * a hung program gets Not Responding; a second End Task offers it at once. */
    es_setup();
    a=shutdown_client(351,"CiukNote",1,2); b=shutdown_client(352,"Frozen",1,2);
    desk_request_close(d,a); es_send(a,351,DESK_CLOSE_ACK,8,DESK_CLOSE_LATER,0);
    { int s[]={a,b}; uint32_t p[]={351,352,0}; es_run(1000000000ull,90000000000ull,s,p); }
    assert(d->clients[a].closing && d->clients[a].window && !d->end_task.kind && !es_terms);
    es_send(a,351,DESK_CLOSE_ACK,8,DESK_CLOSE_DECLINED,0);   /* Cancel in its prompt */
    assert(!d->clients[a].closing && d->clients[a].window);
    desk_request_close(d,b);
    for (uint64_t t=90250000000ull; t<=98000000000ull && !d->end_task.kind; t+=250000000ull) { es_pong(a,351); shutdown_at(t); }
    assert(d->end_task.kind==DESK_HANG_NOT_RESPONDING && !d->end_task.session && d->end_task.pid==352);
    desk_end_task_choice(d,0);                      /* Cancel: no second dialog for this request */
    { int s[]={a}; uint32_t p[]={351,0}; es_run(96250000000ull,110000000000ull,s,p); }
    assert(!d->end_task.kind && !es_terms && d->clients[b].closing);
    assert(desk_task_end(d,d->clients[b].window_id,352)==0);   /* End Task again: the dialog at once */
    assert(d->end_task.kind==DESK_HANG_NOT_RESPONDING && d->end_task.pid==352);
    desk_end_task_choice(d,1); assert(es_terms==1 && es_term_pid==352 && es_term_sig==SIGKILL);
    unsigned closes=es_count(a,DESK_CLOSE_REQUEST);
    assert(desk_task_end(d,d->clients[a].window_id,351)==0 && d->clients[a].closing && es_count(a,DESK_CLOSE_REQUEST)==closes+1);
    assert(!d->end_task.kind);                      /* a responsive program is only asked to close */
    shutdown_cleanup();
    puts("PASS G3 close/End Task: deferred prompt waited 90 s, decline restores, hung -> Not Responding, Cancel, End Task again kills");

    /* 7. Log off asks the same way and closes every program without power;
     * Task Manager's Shut Down and a second Ctrl+Alt+Del open Shut Down
     * (Restart the default). */
    es_setup();
    a=shutdown_client(361,"CiukNote",1,2); es_register(a,361);
    d->logoff=es_logoff; es_logoffs=0;
    desk_session_begin(d,3);
    assert(es_last(a,DESK_QUERY_END_SESSION).flags==DESK_ENDSESSION_LOGOFF);
    es_answer(a,361,DESK_SESSION_AGREE);
    assert(d->power.state==DESK_POWER_CLOSING && es_last(a,DESK_END_SESSION).flags==DESK_ENDSESSION_LOGOFF);
    shutdown_reap(361,0); shutdown_at(2000000000ull);
    assert(!d->power.state && !shutdown_calls && !shutdown_prepares);
    assert(es_logoffs==1 && d->sh.user_gate==1);
    shutdown_cleanup();
    es_setup();
    int tasks=shutdown_client(371,"Task Manager",1,2); d->clients[tasks].taskman=1;
    struct ciuki_message settings; desk_message(&settings,DESK_SETTINGS,24); settings.data[0]=2; settings.sender_pid=371;
    desk_put32(settings.data+4,9); desk_put32(settings.data+8,DESK_SETTINGS_OPEN_POWER);
    desk_receive(d,tasks,&settings);
    assert(d->focus>=0 && d->clients[d->focus].internal==DESK_INTERNAL_SHUTDOWN);
    desk_put32(settings.data+8,DESK_SETTINGS_RELOAD); desk_receive(d,tasks,&settings);
    assert(!d->sh.want_reload);                     /* only Shut Down is Task Manager's */
    desk_request_close(d,d->focus); desk_focus(d,tasks);
    shutdown_key(0x1d); shutdown_key(0x38); shutdown_key(0x153);
    int shut=d->focus; assert(shut>=0 && d->clients[shut].internal==DESK_INTERNAL_SHUTDOWN && d->sh.shutdown_choice==1);
    desk_redraw(d);
    struct ciuki_input_event up={.type=CIUKI_INPUT_KEY,.code=0x38,.value=0}; desk_input(d,&up);
    up.code=0x1d; desk_input(d,&up);
    shutdown_key(DU_KEY_ENTER); assert(d->sh.want_power==1);
    shell_service(d);   /* Task Manager is unregistered: it agrees, the restart proceeds */
    assert(!d->session.state && d->power.state==DESK_POWER_CLOSING && d->power.action==1);
    desk_power_cancel(d);
    shutdown_cleanup();
    puts("PASS G3 log off and Task Manager: LOGOFF flag, programs closed, no power; Shut Down request; second Ctrl+Alt+Del offers Restart");
}
