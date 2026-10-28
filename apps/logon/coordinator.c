/* SPDX-License-Identifier: MIT
 * LOGON coordinator (logon.h): trusted gate, authenticated session start,
 * lock/unlock, secure attention, Log Off collection and the logon screen's
 * power requests. Contract: docs/design/user-security-contract.md 4.1-4.4,
 * user-security-kernel.md "Sessions and logoff", A1-2 and the A2-3 table.
 *
 * Research (recorded with links in docs/validation/sec-user03.md): Windows XP
 * Winlogon owns the secure desktop, starts the shell only after the user's
 * profile is loaded (MSGINA WlxActivateUserShell), returns to the Welcome
 * screen after logoff, and shows the Windows Security options for
 * Ctrl+Alt+Del while logged on (WlxLoggedOnSAS) and only the unlock dialog
 * while locked (WlxWkstaLockedSAS). Log Off asks the user's programs first
 * (ExitWindowsEx EWX_LOGOFF, WM_QUERYENDSESSION/ENDSESSION_LOGOFF): here the
 * session's desktop runs that query/veto and then tells LOGON.
 */
#include "logon.h"
#include "../accounts/wire.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define NO_LINK 0xffffffffu
#define WATCH_MS 100u

void lgn_note(struct lgn *g, const char *fmt, ...)
{
    va_list ap; va_start(ap,fmt);
    vsnprintf(g->log_line,sizeof(g->log_line),fmt,ap);
    va_end(ap);
    if (g->sys->log) g->sys->log(g->sys->ctx,g->log_line);
}
static uint64_t now_of(struct lgn *g) { return g->sys->now_ms(g->sys->ctx); }
static void changed(struct lgn *g) { g->state_generation++; }
static void set_message(struct lgn *g, uint32_t message, int error)
{ g->message=message; g->error=error>0 ? -error : error; changed(g); }
static void set_screen(struct lgn *g, uint32_t mode) { if (g->mode!=mode) { g->mode=mode; changed(g); } }

/* ---- kernel calls --------------------------------------------------- */
static int session_query(struct lgn *g, uint32_t id, uint32_t *state, uint32_t *generation)
{
    struct ciuki_security_args a;
    int e=lgn_security(g,CIUKI_SECURITY_SESSION_QUERY,id,0,NULL,0,NULL,0,&a);
    if (!e) { *state=a.state; *generation=a.result; }
    return e;
}
/* SECURE_ENTER. Session 0 is the boot gate (ATTENTION only). EBUSY means
 * the kernel concealed but a fence is still draining: retry later. */
static int console_enter(struct lgn *g, uint32_t session, uint32_t reason)
{
    struct ciuki_security_console in, out;
    memset(&in,0,sizeof(in)); memset(&out,0,sizeof(out));
    in.version=1; in.size=64; in.session_id=session; in.reason=session ? reason : CIUKI_SECURITY_SECURE_ATTENTION;
    if (session) {
        uint32_t state=0, generation=0;
        int e=session_query(g,session,&state,&generation);
        if (e) return e;
        if (state!=CIUKI_SECURITY_SESSION_ACTIVE) return -ESRCH;
        in.session_generation=generation;
    }
    int e=lgn_security(g,CIUKI_SECURITY_SECURE_ENTER,session,0,&in,64,&out,64,NULL);
    if (e) {
        if (e==-EBUSY && session) { g->enter_reason=reason; g->next_enter=now_of(g)+LGN_POLL_MS; }
        return e;
    }
    if (session) {
        g->session.console=out; g->session.console_valid=1; g->session.generation=out.session_generation;
        if (g->enter_reason==reason) g->enter_reason=0;
    } else { g->boot=out; g->boot_valid=1; }
    changed(g);
    return 0;
}
/* Idempotent refresh of the current binding before a correlation check. */
static int gate_refresh(struct lgn *g)
{
    struct lgn_session *s=&g->session;
    if (!s->phase) return console_enter(g,0,CIUKI_SECURITY_SECURE_ATTENTION);
    if (!s->console_valid || s->console.state!=CIUKI_SECURITY_CONSOLE_TRUSTED) return 0;
    return console_enter(g,s->id,s->console.reason);
}
/* SECURE_LEAVE with the exact cached tuple. The caller decides whether a
 * stale tuple may be refreshed (menu return) or not (an unlock proof). */
static int console_leave(struct lgn *g)
{
    struct lgn_session *s=&g->session;
    if (!s->console_valid || s->console.state!=CIUKI_SECURITY_CONSOLE_TRUSTED) return -EAGAIN;
    struct ciuki_security_console in=s->console, out;
    in.state=0; memset(in.reserved,0,sizeof(in.reserved)); memset(&out,0,sizeof(out));
    int e=lgn_security(g,CIUKI_SECURITY_SECURE_LEAVE,s->id,0,&in,64,&out,64,NULL);
    lgn_wipe(&in,sizeof(in));
    if (e) return e;
    s->console=out; s->generation=out.session_generation; s->console_valid=1;
    changed(g);
    return 0;
}
static int menu_return(struct lgn *g)
{
    int e=console_leave(g);
    if (e==-EAGAIN && !gate_refresh(g)) e=console_leave(g);
    if (!e) { set_screen(g,LGN_MODE_HIDDEN); if (g->message==LGN_MSG_BLANK_LOCK) set_message(g,LGN_MSG_NONE,0); }
    return e;
}
static int session_end(struct lgn *g, uint32_t flags, uint32_t *state)
{
    struct ciuki_security_args a;
    int e=lgn_security(g,CIUKI_SECURITY_SESSION_END,g->session.id,flags,NULL,0,NULL,0,&a);
    if (!e) *state=a.state;
    return e;
}
static void begin_ending(struct lgn *g, uint32_t reason)
{
    struct lgn_session *s=&g->session;
    if (s->phase==LGN_ENDING) return;
    if (reason!=LGN_END_CRASH || s->end==LGN_END_CRASH) s->end=reason;
    s->phase=LGN_ENDING; s->next_poll=0; g->enter_reason=0;
    set_screen(g,LGN_MODE_ENDING);
    lgn_note(g,"[logon] session %u ending reason=%u",s->id,s->end);
}

/* ---- session start ---------------------------------------------------- */
static void session_clear(struct lgn *g)
{
    lgn_wipe(&g->session,sizeof(g->session));
    g->enter_reason=0;
    changed(g);
}
static void start_session(struct lgn *g, const struct acct_auth_outcome *o, int blank)
{
    struct lgn_session *s=&g->session;
    lgn_wipe(s,sizeof(*s));
    s->phase=LGN_CREATE; s->blank=blank; s->uid=o->challenge.uid;
    memcpy(s->credential,o->credential,64); memcpy(s->profile,o->profile,112); memcpy(s->name,o->name,32);
    s->recovery=!!(a32(o->credential+56)&CIUKI_CRED_SAFE_RECOVERY);
    s->started=now_of(g); s->create_until=s->started+LGN_CREATE_BUSY_MS;
    set_message(g,LGN_MSG_NONE,0);
    set_screen(g,LGN_MODE_LOADING);
    lgn_note(g,"[logon] authenticated uid=%u; starting session",s->uid);
}
static int launch(struct lgn *g, uint32_t purpose)
{
    struct lgn_session *s=&g->session;
    uint32_t state=0, generation=0;
    int e=session_query(g,s->id,&state,&generation);
    if (e) return e;
    struct ciuki_security_launch in, out;
    memset(&in,0,sizeof(in)); memset(&out,0,sizeof(out));
    in.version=1; in.size=96; in.purpose=purpose; in.session_id=s->id; in.session_generation=generation;
    e=lgn_security(g,CIUKI_SECURITY_SESSION_LAUNCH,s->id,0,&in,96,&out,96,NULL);
    if (!e) { s->launch[purpose-1]=out; s->generation=out.session_generation; }
    return e;
}
static int launch_poll(struct lgn *g, uint32_t purpose)
{
    struct lgn_session *s=&g->session;
    struct ciuki_security_launch *l=&s->launch[purpose-1], in, out;
    for (unsigned attempt=0;;attempt++) {
        memset(&in,0,sizeof(in)); memset(&out,0,sizeof(out));
        in.version=1; in.size=96; in.purpose=purpose; in.session_id=s->id; in.session_generation=s->generation;
        in.launch_id=l->launch_id; in.launch_generation=l->launch_generation;
        int e=lgn_security(g,CIUKI_SECURITY_LAUNCH_POLL,s->id,0,&in,96,&out,96,NULL);
        if (e==-EAGAIN && !attempt) {
            uint32_t state=0; int q=session_query(g,s->id,&state,&s->generation);
            if (q) return q;
            continue;
        }
        if (!e) { *l=out; s->generation=out.session_generation; }
        return e;
    }
}
static void session_failed(struct lgn *g, int error, uint32_t message)
{
    lgn_note(g,"[logon] session start failed: %d",error);
    set_message(g,message,error);
    if (g->session.id) begin_ending(g,LGN_END_FAILED);
    else { session_clear(g); set_screen(g,LGN_MODE_WELCOME); }
}
static void session_create(struct lgn *g)
{
    struct lgn_session *s=&g->session;
    uint8_t records[176];
    memcpy(records,s->credential,64); memcpy(records+64,s->profile,112);
    struct ciuki_security_args a;
    int e=lgn_security(g,CIUKI_SECURITY_SESSION_CREATE,0,0,records,sizeof(records),NULL,0,&a);
    lgn_wipe(records,sizeof(records));
    if (e==-EBUSY && now_of(g)<s->create_until) { s->next_poll=now_of(g)+LGN_COLLECT_MS; return; }
    if (e) { session_failed(g,e,LGN_MSG_PROFILE_FAILED); return; }
    s->id=a.result; s->generation=1;
    /* The new session inherits the boot gate's console (SESSION_CREATE). */
    s->console=g->boot; s->console.session_id=s->id; s->console.session_generation=1;
    s->console.authentication_required=0; s->console_valid=g->boot_valid;
    lgn_wipe(s->credential,64);
    lgn_note(g,"[logon] session %u created uid=%u",s->id,s->uid);
    s->phase=LGN_LAUNCH_W32;
}
static void session_activate(struct lgn *g)
{
    struct lgn_session *s=&g->session;
    s->phase=LGN_ACTIVE; s->next_poll=now_of(g)+WATCH_MS;
    set_screen(g,LGN_MODE_HIDDEN);
    lgn_note(g,"[logon] session %u running uid=%u start_ms=%llu",s->id,s->uid,(unsigned long long)(now_of(g)-s->started));
}
static void session_poll(struct lgn *g)
{
    struct lgn_session *s=&g->session;
    int e=launch_poll(g,CIUKI_SECURITY_LAUNCH_W32SRV);
    if (!e) e=launch_poll(g,CIUKI_SECURITY_LAUNCH_DESKTOP);
    if (e) { session_failed(g,e,LGN_MSG_START_FAILED); return; }
    const struct ciuki_security_launch *d=&s->launch[0], *w=&s->launch[1];
    if (d->state==CIUKI_SECURITY_LAUNCH_FAILED || w->state==CIUKI_SECURITY_LAUNCH_FAILED) {
        session_failed(g,d->state==CIUKI_SECURITY_LAUNCH_FAILED ? d->error : w->error,LGN_MSG_START_FAILED);
        return;
    }
    if (d->state==CIUKI_SECURITY_LAUNCH_RUNNING) { s->console_valid=0; session_activate(g); return; }
    if (d->state!=CIUKI_SECURITY_LAUNCH_READY || w->state<CIUKI_SECURITY_LAUNCH_READY) return;
    /* Both ready but the initial activation was superseded (secure attention
     * or a service restart during the start): the current gate decides. */
    if (g->enter_reason) return;
    e=console_enter(g,s->id,s->console_valid ? s->console.reason : CIUKI_SECURITY_SECURE_ATTENTION);
    if (e) { if (e!=-EBUSY) session_failed(g,e,LGN_MSG_START_FAILED); return; }
    if (s->console.authentication_required) { s->phase=LGN_ACTIVE; s->next_poll=now_of(g)+WATCH_MS; set_screen(g,LGN_MODE_LOCKED); return; }
    e=console_leave(g);
    if (!e) session_activate(g);
    else if (e!=-EBUSY && e!=-EAGAIN) session_failed(g,e,LGN_MSG_START_FAILED);
}
/* ACTIVE: a session generation the coordinator did not cause means the
 * kernel moved the gate (physical attention, a trusted role restart) or the
 * pair ended. One idempotent ATTENTION entry reads the current binding. */
static void session_watch(struct lgn *g)
{
    struct lgn_session *s=&g->session;
    uint32_t state=0, generation=0;
    int e=session_query(g,s->id,&state,&generation);
    if (e || state!=CIUKI_SECURITY_SESSION_ACTIVE) { begin_ending(g,LGN_END_CRASH); return; }
    if (generation==s->generation || g->enter_reason) return;
    e=console_enter(g,s->id,CIUKI_SECURITY_SECURE_ATTENTION);
    if (e && e!=-EBUSY) return;
    if (g->mode==LGN_MODE_LOCKED) return;
    if (e==-EBUSY) { set_screen(g,LGN_MODE_SECURITY); return; }
    if (s->console.authentication_required) {
        set_screen(g,LGN_MODE_LOCKED);
        if (g->message==LGN_MSG_NONE) set_message(g,LGN_MSG_SERVICE_RESTART,0);
    } else set_screen(g,LGN_MODE_SECURITY);
}
static void session_collect(struct lgn *g)
{
    struct lgn_session *s=&g->session;
    uint32_t state=0;
    if (!s->end_begun) {
        int e=session_end(g,CIUKI_SECURITY_END_BEGIN,&state);
        if (e && e!=-ESRCH) { lgn_note(g,"[logon] END_BEGIN failed: %d",e); return; }
        s->end_begun=1;
        if (e==-ESRCH) state=CIUKI_SECURITY_SESSION_DEAD;
    } else {
        int e=session_end(g,CIUKI_SECURITY_END_COLLECT,&state);
        if (e==-ESRCH) state=CIUKI_SECURITY_SESSION_DEAD;
        else if (e) { lgn_note(g,"[logon] END_COLLECT failed: %d",e); return; }
    }
    if (state!=CIUKI_SECURITY_SESSION_DEAD) return;
    uint32_t end=s->end, id=s->id;
    lgn_note(g,"[logon] session %u ended reason=%u",id,end);
    session_clear(g);
    g->logged_off=1;
    acct_logon_clear(&g->correlation);
    if (end==LGN_END_SHUTDOWN || end==LGN_END_RESTART) {
        g->power_action=end==LGN_END_SHUTDOWN ? LGN_EVENT_SHUTDOWN : LGN_EVENT_RESTART;
        set_screen(g,LGN_MODE_POWER);
        return;
    }
    if (end==LGN_END_CRASH) set_message(g,LGN_MSG_SESSION_ENDED,0);
    else if (end!=LGN_END_FAILED) set_message(g,LGN_MSG_NONE,0); /* a failed start keeps its notice */
    set_screen(g,LGN_MODE_WELCOME);
    (void)console_enter(g,0,CIUKI_SECURITY_SECURE_ATTENTION);
}
/* A restarted LOGON adopts the live session it finds: concealed and locked
 * by the kernel at the old instance's death (A2-3), owner from its processes. */
static void adopt(struct lgn *g)
{
    struct lgn_session *s=&g->session;
    for (uint32_t id=1;id;id++) {
        uint32_t state=0, generation=0;
        int e=session_query(g,id,&state,&generation);
        if (e==-ESRCH) return;
        if (e) { lgn_note(g,"[logon] session scan failed: %d",e); return; }
        if (state!=CIUKI_SECURITY_SESSION_ACTIVE && state!=CIUKI_SECURITY_SESSION_CLOSING) continue;
        lgn_wipe(s,sizeof(*s));
        s->id=id; s->generation=generation; s->adopted=1; s->uid=CIUKI_UID_INVALID;
        g->autologon_done=1; g->logged_off=1; /* a restarted LOGON never logs on automatically */
        static uint32_t pids[CIUKI_PROCESS_MAX];
        int n=g->sys->pids ? g->sys->pids(g->sys->ctx,pids,CIUKI_PROCESS_MAX) : -ENOSYS;
        for (int i=0;i<n;i++) {
            struct ciuki_security_args a; struct ciuki_credential c;
            memset(&a,0,sizeof(a)); a.version=1; a.size=64; a.operation=CIUKI_SECURITY_QUERY_PROCESS; a.target_pid=pids[i];
            if (g->sys->security(g->sys->ctx,&a,NULL,0,&c,64) || c.session_id!=id) continue;
            s->uid=c.uid; break;
        }
        if (state==CIUKI_SECURITY_SESSION_CLOSING) { s->phase=LGN_ENDING; set_screen(g,LGN_MODE_ENDING); return; }
        s->phase=LGN_ACTIVE; s->next_poll=now_of(g)+WATCH_MS;
        (void)console_enter(g,id,CIUKI_SECURITY_SECURE_LOCK);
        set_screen(g,LGN_MODE_LOCKED); set_message(g,LGN_MSG_SERVICE_RESTART,0);
        lgn_note(g,"[logon] adopted session %u uid=%u: locked",id,s->uid);
        return;
    }
}

/* ---- AUTH relay ------------------------------------------------------- */
static struct lgn_verify *verify_find(struct lgn *g, uint64_t attempt)
{
    for (unsigned i=0;i<4;i++) if (g->verify[i].occupied && g->verify[i].attempt==attempt) return &g->verify[i];
    return NULL;
}
static struct lgn_verify *verify_slot(struct lgn *g, uint64_t attempt)
{
    struct lgn_verify *v=verify_find(g,attempt);
    uint64_t now=now_of(g);
    for (unsigned i=0;i<4 && !v;i++) {
        struct lgn_verify *x=&g->verify[i];
        if (x->occupied && now-x->started>=LGN_SUBMIT_MS) memset(x,0,sizeof(*x));
        if (!x->occupied) v=x;
    }
    if (v) { memset(v,0,sizeof(*v)); v->occupied=1; v->attempt=attempt; v->started=now; }
    return v;
}
static int link_waiting(struct lgn *g, uint32_t link, uint64_t attempt)
{
    return link<LGN_LINKS && g->links[link].occupied && !g->links[link].retiring && g->links[link].pending &&
           g->links[link].attempt==attempt;
}
static void reply_progress(struct lgn *g, uint32_t link, uint32_t disposition, int status, uint32_t retry)
{
    struct lgn_progress p={disposition,status,retry}; uint8_t body[LGN_PROGRESS_BYTES];
    if (lgn_progress_encode(&p,body,sizeof(body))) { (void)lgn_reply(g,link,-EINVAL,NULL,0); return; }
    (void)lgn_reply(g,link,0,body,sizeof(body));
}
static void autologon(struct lgn *g)
{
    uint8_t relay[128];
    g->autologon_done=1;
    if (gate_refresh(g)) { set_screen(g,LGN_MODE_WELCOME); return; }
    int e=acct_logon_autologon(&g->correlation,g->safe,g->logged_off,relay);
    if (!e) e=lgn_auth_queue(g,LGN_AUTH_CHALLENGE,0,relay,128,LGN_PURPOSE_CHALLENGE,NO_LINK,a64(relay+64+8));
    lgn_wipe(relay,sizeof(relay));
    if (e) { lgn_note(g,"[logon] automatic logon not used: %d",e); set_screen(g,LGN_MODE_WELCOME); }
    else lgn_note(g,"[logon] automatic logon requested");
}
static void list_reply(struct lgn *g, int status, const uint8_t *p, size_t n)
{
    g->list_pending=0;
    if (status || n<16 || a32(p)!=1 || a32(p+4)>ACCT_MAX || n!=16+144*(size_t)a32(p+4)) {
        if (g->accounts_ready) { g->accounts_ready=0; changed(g); }
        if (!g->unavailable_since) g->unavailable_since=now_of(g);
        /* ENOENT: open but no published database (a G32 upgrade waiting for
         * its enrollment): say so now instead of waiting for a timeout. */
        if (status==-ENOENT && g->unavailable_since>LGN_UNAVAILABLE_MS) g->unavailable_since=now_of(g)-LGN_UNAVAILABLE_MS;
        if (g->error!=(status ? status : -EIO)) changed(g);
        g->error=status ? status : -EIO;
        g->next_list=now_of(g)+LGN_LIST_RETRY_MS;
        return;
    }
    g->accounts_generation=a64(p+8);
    if (!g->accounts_ready) { g->accounts_ready=1; changed(g); }
    g->unavailable_since=0; g->error=0;
    /* Only the ready image's sole enabled CiukiUser (uid 1000) can qualify;
     * ACCOUNT validates the whole tuple. Skipping other installs avoids a
     * refused challenge and its 1 s authentication delay at Welcome. */
    g->autologon_candidate=a32(p+4)==1 && a32(p+16)==1000 && acct_name_equal((const char *)p+32,"CiukiUser");
    struct lgn_session *s=&g->session;
    if (s->phase && !s->name[0])
        for (uint32_t i=0;i<a32(p+4);i++) if (a32(p+16+144*i)==s->uid) memcpy(s->name,p+16+144*i+16,31);
}
void lgn_auth_reply(struct lgn *g, const struct lgn_auth_request *q, int status, const void *body, size_t n)
{
    const uint8_t *p=body;
    if (q->purpose==LGN_PURPOSE_LIST) { list_reply(g,status,p,n); return; }
    if (q->purpose==LGN_PURPOSE_PLAN) {
        if (link_waiting(g,q->link,q->attempt)) (void)lgn_reply(g,q->link,status,status ? NULL : body,status ? 0 : n);
        return;
    }
    if (q->purpose==LGN_PURPOSE_CHALLENGE) {
        int e=status;
        struct acct_subject account;
        if (!e) e=lgn_retain_auth(g,&account);
        if (!e) {
            e=acct_logon_challenged(&g->correlation,&account,body,n);
            g->correlation.kernel.release_peer(g,&account);
        }
        if (q->link==NO_LINK) {
            if (!e) {
                struct lgn_verify *v=verify_slot(g,q->attempt);
                if (v) { v->link=NO_LINK; v->action=5; v->empty_secret=1; }
                e=v ? lgn_auth_queue(g,LGN_AUTH_AUTOLOGON,a64(p+48),body,64,LGN_PURPOSE_VERIFY,NO_LINK,q->attempt) : -EAGAIN;
            }
            if (e) { lgn_note(g,"[logon] automatic logon refused: %d",e); set_screen(g,LGN_MODE_WELCOME); }
            return;
        }
        if (!link_waiting(g,q->link,q->attempt)) return;
        if (e) (void)lgn_reply(g,q->link,e,NULL,0);
        else (void)lgn_reply(g,q->link,0,body,64);
        return;
    }
    /* VERIFY: the correlated AUTH_OUTCOME normally arrived first. */
    struct lgn_verify *v=verify_find(g,q->attempt);
    if (status && v && !v->outcome_seen) {
        if (q->link==NO_LINK) { lgn_note(g,"[logon] automatic logon failed: %d",status); set_screen(g,LGN_MODE_WELCOME); }
        else if (link_waiting(g,q->link,q->attempt)) reply_progress(g,q->link,LGN_FAILED,status,0);
    }
    if (v) memset(v,0,sizeof(*v));
}

/* ---- requests ----------------------------------------------------------- */
static uint32_t current_console(const struct lgn *g)
{
    if (g->session.phase) return g->session.console_valid ? g->session.console.console_generation : 0;
    return g->boot_valid ? g->boot.console_generation : 0;
}
void lgn_state(const struct lgn *g, struct lgn_state *st)
{
    const struct lgn_session *s=&g->session;
    memset(st,0,sizeof(*st));
    st->mode=g->mode ? g->mode : LGN_MODE_STARTING;
    st->flags=(s->blank ? LGN_STATE_BLANK : 0)|(g->safe ? LGN_STATE_SAFE : 0)|(s->phase ? LGN_STATE_SESSION : 0)|
              (g->power_action==LGN_EVENT_RESTART ? LGN_STATE_RESTART : 0);
    for (unsigned i=0;i<LGN_LINKS;i++)
        if (g->links[i].occupied && !g->links[i].retiring && g->links[i].peer_role==LGN_ROLE_DESKTOP &&
            s->phase && g->links[i].session_id==s->id) st->flags|=LGN_STATE_DESKTOP;
    st->session=s->id; st->console=current_console(g); st->owner_uid=s->phase ? s->uid : 0;
    st->message=g->message; st->error=g->error; st->generation=g->state_generation;
    memcpy(st->owner_name,s->name,31);
}
static void begin_auth(struct lgn *g, unsigned i)
{
    struct lgn_link *l=&g->links[i];
    uint8_t relay[128];
    if (l->total!=64) { (void)lgn_reply(g,i,-EINVAL,NULL,0); return; }
    uint32_t action=a32(l->body+4);
    int allowed=action==2 ? g->mode==LGN_MODE_WELCOME && !g->session.phase && g->accounts_ready :
                action==3 ? g->mode==LGN_MODE_LOCKED && g->session.phase==LGN_ACTIVE && !g->enter_reason : 0;
    if (!allowed) { (void)lgn_reply(g,i,action==4 ? -EOPNOTSUPP : -EBUSY,NULL,0); return; }
    int e=gate_refresh(g);
    struct acct_subject ui;
    if (!e) e=lgn_retain(g,i,&ui);
    if (!e) {
        e=acct_logon_begin(&g->correlation,&ui,l->body,64,relay);
        g->correlation.kernel.release_peer(g,&ui);
    }
    uint64_t attempt=a64(relay+64+8);
    if (!e) e=lgn_auth_queue(g,LGN_AUTH_CHALLENGE,0,relay,128,LGN_PURPOSE_CHALLENGE,i,attempt);
    lgn_wipe(relay,sizeof(relay));
    if (e) { (void)lgn_reply(g,i,e,NULL,0); return; }
    l->attempt=attempt;
}
static void submit_auth(struct lgn *g, unsigned i)
{
    struct lgn_link *l=&g->links[i];
    uint8_t relay[272];
    if (l->total!=LGN_SUBMIT_BYTES) { (void)lgn_reply(g,i,-EINVAL,NULL,0); return; }
    int empty=!a32(l->body+28);
    uint64_t attempt=a64(l->body+8);
    struct acct_subject ui;
    int e=lgn_retain(g,i,&ui);
    if (!e) {
        e=acct_logon_submit(&g->correlation,&ui,l->body,LGN_SUBMIT_BYTES,relay);
        g->correlation.kernel.release_peer(g,&ui);
    }
    lgn_wipe(l->body,sizeof(l->body));
    uint32_t action=e ? 0 : a32(relay+4);
    struct lgn_verify *v=e ? NULL : verify_slot(g,attempt);
    if (!e && !v) e=-EAGAIN;
    if (!e) { v->link=i; v->action=action; v->empty_secret=empty; }
    if (!e) e=lgn_auth_queue(g,action,a64(relay+48),relay,272,LGN_PURPOSE_VERIFY,i,attempt);
    lgn_wipe(relay,sizeof(relay));
    if (e) { if (v) memset(v,0,sizeof(*v)); (void)lgn_reply(g,i,e,NULL,0); return; }
    l->attempt=attempt;
}
/* Authenticated owner of the locked session: release with that exact proof.
 * The binding was refreshed before the outcome was correlated; a stale
 * LEAVE is not retried with a newer tuple (A2-3: fresh authentication). */
static int unlock(struct lgn *g, const struct acct_auth_outcome *o)
{
    struct lgn_session *s=&g->session;
    if (o->challenge.session!=s->id || o->challenge.uid!=s->uid || !s->console_valid ||
        o->challenge.authentication_epoch!=s->console.authentication_epoch ||
        o->challenge.console!=s->console.console_generation) return -EAGAIN;
    int e=console_leave(g);
    if (!e) { set_screen(g,LGN_MODE_HIDDEN); set_message(g,LGN_MSG_NONE,0); }
    return e;
}
static void outcome(struct lgn *g, unsigned i)
{
    struct lgn_link *l=&g->links[i];
    if (l->total!=304) { (void)lgn_reply(g,i,-EINVAL,NULL,0); return; }
    uint64_t attempt=a64(l->body+8);
    uint32_t disposition=a32(l->body+68);
    struct lgn_verify *v=verify_find(g,attempt);
    /* Correlate against the current binding, refreshed idempotently. */
    (void)gate_refresh(g);
    struct acct_subject account; struct acct_auth_outcome o; uint8_t progress[16];
    memset(&o,0,sizeof(o));
    int e=v ? lgn_retain(g,i,&account) : -EAGAIN;
    if (!e) {
        uint64_t generation=disposition==LGN_ENROLL_COMMITTED ? a64(l->body+80) : a64(l->body+48);
        e=acct_logon_outcome(&g->correlation,&account,generation,l->body,304,&o,progress);
        g->correlation.kernel.release_peer(g,&account);
    }
    lgn_wipe(l->body,sizeof(l->body));
    (void)lgn_reply(g,i,e,NULL,0); /* acknowledgment to ACCOUNT */
    if (e) { lgn_wipe(&o,sizeof(o)); return; }
    v->outcome_seen=1;
    uint32_t link=v->link;
    int waiting=link==NO_LINK || link_waiting(g,link,attempt);
    struct lgn_progress p; (void)lgn_progress_decode(&p,progress,16);
    if (o.disposition==LGN_AUTHENTICATED) {
        if (o.challenge.action==2 || o.challenge.action==5) {
            if (!g->session.phase) start_session(g,&o,v->empty_secret);
            else p=(struct lgn_progress){LGN_FAILED,-EBUSY,0};
        } else if (o.challenge.action==3) {
            if (v->empty_secret) {
                /* No empty-password unlock: a blank owner can only end the session. */
                g->session.blank=1; set_message(g,LGN_MSG_NO_EMPTY_UNLOCK,0);
                p=(struct lgn_progress){LGN_FAILED,-EPERM,0};
            } else {
                int u=unlock(g,&o);
                if (u) p=(struct lgn_progress){LGN_FAILED,u==-EBUSY ? -EBUSY : -EAGAIN,0};
                else g->session.blank=0;
            }
        }
    } else if (o.disposition==LGN_FAILED && link==NO_LINK) {
        lgn_note(g,"[logon] automatic logon refused: %d",o.status); set_screen(g,LGN_MODE_WELCOME);
    }
    if (link!=NO_LINK && waiting) {
        uint8_t body[16];
        if (lgn_progress_encode(&p,body,16)) (void)lgn_reply(g,link,-EIO,NULL,0);
        else (void)lgn_reply(g,link,0,body,16);
    }
    lgn_wipe(&o,sizeof(o)); lgn_wipe(progress,sizeof(progress));
}
static void reply_state(struct lgn *g, unsigned i)
{
    struct lgn_state st; uint8_t body[LGN_UI_STATE_BYTES];
    lgn_state(g,&st);
    if (lgn_state_encode(&st,body,sizeof(body))) (void)lgn_reply(g,i,-EIO,NULL,0);
    else (void)lgn_reply(g,i,0,body,sizeof(body));
}
static int security_target(const struct lgn *g, uint32_t session, uint32_t console)
{
    const struct lgn_session *s=&g->session;
    if (s->phase!=LGN_ACTIVE || session!=s->id) return -EACCES;
    return console==current_console(g) ? 0 : -EAGAIN;
}
static void attention(struct lgn *g)
{
    struct lgn_session *s=&g->session;
    if (!s->phase) { (void)console_enter(g,0,CIUKI_SECURITY_SECURE_ATTENTION); return; }
    if (s->phase!=LGN_ACTIVE) return;
    int e=console_enter(g,s->id,CIUKI_SECURITY_SECURE_ATTENTION);
    if (g->mode==LGN_MODE_LOCKED || (e && e!=-EBUSY)) return;
    set_screen(g,!e && s->console.authentication_required ? LGN_MODE_LOCKED : LGN_MODE_SECURITY);
}
static void ui_state(struct lgn *g, unsigned i)
{
    struct lgn_link *l=&g->links[i];
    struct lgn_ui_command c;
    if (lgn_ui_command_decode(&c,l->body,l->total)) { (void)lgn_reply(g,i,-EINVAL,NULL,0); return; }
    struct lgn_session *s=&g->session;
    int e=0;
    switch (c.command) {
    case LGN_CMD_QUERY: break;
    case LGN_CMD_ATTENTION: attention(g); break;
    case LGN_CMD_ACKNOWLEDGE: set_message(g,LGN_MSG_NONE,0); break;
    case LGN_CMD_FORCE_LOGOFF:
        e=security_target(g,c.session,c.console);
        if (!e && g->mode!=LGN_MODE_SECURITY && !(g->mode==LGN_MODE_LOCKED && s->blank)) e=-EACCES;
        if (!e) begin_ending(g,LGN_END_FORCED);
        break;
    default: {
        e=security_target(g,c.session,c.console);
        if (!e && g->mode!=LGN_MODE_SECURITY) e=-EBUSY;
        if (e) break;
        static const uint32_t actions[]={0,0,LGN_ACT_LOGOFF,LGN_ACT_TASKMGR,0,LGN_ACT_SET_PASSWORD,0,LGN_ACT_SHUTDOWN,LGN_ACT_RESTART};
        s->pending_action=actions[c.command];
        e=menu_return(g);
        if (e) s->pending_action=0;
        break;
    }
    }
    if (e) { (void)lgn_reply(g,i,e,NULL,0); return; }
    reply_state(g,i);
}
static void lock_event(struct lgn *g, unsigned i)
{
    struct lgn_link *l=&g->links[i];
    struct acct_ui_event ev;
    if (acct_event_decode(LGN_OP_LOCK_EVENT,0,&ev,l->body,l->total)) { (void)lgn_reply(g,i,-EINVAL,NULL,0); return; }
    struct lgn_session *s=&g->session;
    int e=security_target(g,ev.session,ev.console);
    if (!e && ev.action==LGN_EVENT_LOCK) {
        if (g->mode==LGN_MODE_LOCKED) e=0;
        else if (g->mode!=LGN_MODE_SECURITY) e=-EBUSY;
        else if (s->blank) { set_message(g,LGN_MSG_BLANK_LOCK,0); reply_progress(g,i,0,LGN_STATUS_NO_PASSWORD,0); return; }
        else {
            e=console_enter(g,s->id,CIUKI_SECURITY_SECURE_LOCK);
            if (!e || e==-EBUSY) { e=0; set_screen(g,LGN_MODE_LOCKED); }
        }
    } else if (!e) {
        /* Menu return never lowers a lock. */
        e=g->mode!=LGN_MODE_SECURITY ? -EACCES : menu_return(g);
    }
    if (e) { (void)lgn_reply(g,i,e,NULL,0); return; }
    reply_progress(g,i,0,0,0);
}
static void power_event(struct lgn *g, unsigned i)
{
    struct lgn_link *l=&g->links[i];
    struct acct_ui_event ev;
    if (acct_event_decode(LGN_OP_POWER_EVENT,0,&ev,l->body,l->total)) { (void)lgn_reply(g,i,-EINVAL,NULL,0); return; }
    struct lgn_session *s=&g->session;
    int e=0;
    if (!s->phase) {
        if (ev.session || ev.console!=current_console(g)) e=-EAGAIN;
        else if (g->mode!=LGN_MODE_WELCOME && g->mode!=LGN_MODE_UNAVAILABLE && g->mode!=LGN_MODE_STARTING) e=-EBUSY;
        else { g->power_action=ev.action; set_screen(g,LGN_MODE_POWER); }
    } else {
        /* A logged-on user's shutdown follows the desktop's save/veto. */
        e=security_target(g,ev.session,ev.console);
        if (!e && g->mode!=LGN_MODE_SECURITY) e=-EBUSY;
        if (!e) {
            s->pending_action=ev.action==LGN_EVENT_SHUTDOWN ? LGN_ACT_SHUTDOWN : LGN_ACT_RESTART;
            e=menu_return(g);
            if (e) s->pending_action=0;
        }
    }
    if (e) { (void)lgn_reply(g,i,e,NULL,0); return; }
    reply_progress(g,i,0,0,0);
}
static void session_lock(struct lgn *g, unsigned i)
{
    struct lgn_link *l=&g->links[i];
    struct acct_ui_event ev;
    if (acct_event_decode(LGN_OP_SESSION_LOCK,0,&ev,l->body,l->total)) { (void)lgn_reply(g,i,-EINVAL,NULL,0); return; }
    struct lgn_session *s=&g->session;
    if (ev.session!=l->session_id || s->phase!=LGN_ACTIVE || ev.session!=s->id) { (void)lgn_reply(g,i,-EACCES,NULL,0); return; }
    if (s->blank) { reply_progress(g,i,0,LGN_STATUS_NO_PASSWORD,0); return; }   /* set-password guidance, no false lock */
    if (g->mode!=LGN_MODE_LOCKED) {
        int e=console_enter(g,s->id,CIUKI_SECURITY_SECURE_LOCK);
        if (e && e!=-EBUSY) { (void)lgn_reply(g,i,e,NULL,0); return; }
        set_screen(g,LGN_MODE_LOCKED);
    }
    reply_progress(g,i,0,0,0);
}
static void session_event(struct lgn *g, unsigned i)
{
    struct lgn_link *l=&g->links[i];
    struct lgn_session_notice n;
    if (lgn_notice_decode(&n,l->body,l->total)) { (void)lgn_reply(g,i,-EINVAL,NULL,0); return; }
    struct lgn_session *s=&g->session;
    if (n.session!=l->session_id || !s->phase || n.session!=s->id) { (void)lgn_reply(g,i,-EACCES,NULL,0); return; }
    switch (n.notice) {
    case LGN_NOTICE_LOGOFF: s->end=LGN_END_LOGOFF; break;
    case LGN_NOTICE_SHUTDOWN: s->end=LGN_END_SHUTDOWN; break;
    case LGN_NOTICE_RESTART: s->end=LGN_END_RESTART; break;
    case LGN_NOTICE_PASSWORD: s->blank=0; changed(g); break;
    default: break;
    }
    struct lgn_session_event ev;
    memset(&ev,0,sizeof(ev));
    ev.action=n.notice==LGN_NOTICE_QUERY ? s->pending_action : LGN_ACT_NONE;
    if (n.notice==LGN_NOTICE_QUERY) s->pending_action=0;
    ev.flags=(s->blank ? LGN_EVENT_BLANK : 0)|(s->recovery ? LGN_EVENT_RECOVERY : 0);
    ev.uid=s->uid; ev.session=s->id; memcpy(ev.name,s->name,31);
    uint8_t body[LGN_SESSION_EVENT_BYTES];
    if (lgn_event_encode(&ev,body,sizeof(body))) (void)lgn_reply(g,i,-EIO,NULL,0);
    else (void)lgn_reply(g,i,0,body,sizeof(body));
}
static void enroll_plan(struct lgn *g, unsigned i)
{
    struct lgn_link *l=&g->links[i];
    /* Relayed unchanged on LOGON's own AUTH link (contract A2-1). */
    if (g->session.phase || !g->own_generation) { (void)lgn_reply(g,i,-EBUSY,NULL,0); return; }
    static uint64_t plan_attempt;
    uint64_t attempt=++plan_attempt|((uint64_t)1<<63);
    int e=lgn_auth_queue(g,LGN_AUTH_ENROLL_PLAN,l->frame_generation,l->body,l->total,LGN_PURPOSE_PLAN,i,attempt);
    if (e) { (void)lgn_reply(g,i,e,NULL,0); return; }
    l->attempt=attempt;
}
void lgn_request(struct lgn *g, unsigned i)
{
    struct lgn_link *l=&g->links[i];
    static const uint32_t role[]={0,LGN_ROLE_ACCOUNT,LGN_ROLE_UI,LGN_ROLE_UI,LGN_ROLE_UI,LGN_ROLE_DESKTOP,
                                  LGN_ROLE_UI,LGN_ROLE_UI,LGN_ROLE_UI,LGN_ROLE_DESKTOP};
    if (!l->operation || l->operation>LGN_OP_SESSION_EVENT) { (void)lgn_reply(g,i,-EOPNOTSUPP,NULL,0); return; }
    if (l->peer_role!=role[l->operation] || (l->peer_role==LGN_ROLE_DESKTOP ? !l->session_id : l->session_id)) {
        (void)lgn_reply(g,i,-EPERM,NULL,0); return;
    }
    switch (l->operation) {
    case LGN_OP_AUTH_OUTCOME: outcome(g,i); break;
    case LGN_OP_LOCK_EVENT: lock_event(g,i); break;
    case LGN_OP_POWER_EVENT: power_event(g,i); break;
    case LGN_OP_BEGIN_AUTH: begin_auth(g,i); break;
    case LGN_OP_SESSION_LOCK: session_lock(g,i); break;
    case LGN_OP_SUBMIT_AUTH: submit_auth(g,i); break;
    case LGN_OP_ENROLL_PLAN: enroll_plan(g,i); break;
    case LGN_OP_UI_STATE: ui_state(g,i); break;
    case LGN_OP_SESSION_EVENT: session_event(g,i); break;
    }
    lgn_wipe(l->body,sizeof(l->body));
}
/* A lost LOGON_UI link means LOGON_UI died or was cut off: A2-3 requires a
 * locked session afterwards (the kernel also raised authentication). */
void lgn_link_lost(struct lgn *g, unsigned i, uint32_t role)
{
    (void)i;
    struct lgn_session *s=&g->session;
    if (role==LGN_ROLE_UI && s->phase==LGN_ACTIVE && g->mode!=LGN_MODE_LOCKED) {
        int e=console_enter(g,s->id,CIUKI_SECURITY_SECURE_LOCK);
        if (!e || e==-EBUSY) { set_screen(g,LGN_MODE_LOCKED); set_message(g,LGN_MSG_SERVICE_RESTART,0); }
    }
}
void lgn_auth_lost(struct lgn *g)
{
    if (g->accounts_ready) { g->accounts_ready=0; changed(g); }
    g->list_pending=0; g->next_list=0;
}

/* ---- loop ------------------------------------------------------------- */
int lgn_init(struct lgn *g, const struct lgn_sys *sys)
{
    if (!g || !sys || !sys->security || !sys->send || !sys->recv || !sys->close || !sys->wait || !sys->now_ms) return -EINVAL;
    memset(g,0,sizeof(*g));
    g->sys=sys;
    lgn_links_bind(g);
    uint32_t flags=0;
    int e=sys->boot_flags ? sys->boot_flags(sys->ctx,&flags) : -ENOSYS;
    /* Safe Mode cannot be attested: no automatic logon this boot. */
    g->safe=e ? 1 : !!(flags&CIUKI_BOOT_SAFE_MODE);
    if (e) g->autologon_done=1;
    g->started=now_of(g); g->mode=LGN_MODE_STARTING;
    return 0;
}
static void advance(struct lgn *g)
{
    uint64_t now=now_of(g);
    struct lgn_session *s=&g->session;
    if (!g->boot_valid && !s->phase) {
        int e=console_enter(g,0,CIUKI_SECURITY_SECURE_ATTENTION);
        if (e==-EBUSY) adopt(g);
        else if (e) { if (now>=g->next_enter) { lgn_note(g,"[logon] boot gate refused: %d",e); g->next_enter=now+1000; } }
    }
    if (!g->accounts_ready && !g->list_pending && now>=g->next_list) {
        if (!lgn_auth_queue(g,LGN_AUTH_LIST,0,NULL,0,LGN_PURPOSE_LIST,NO_LINK,0)) g->list_pending=1;
        else g->next_list=now+LGN_LIST_RETRY_MS;
    }
    if (g->enter_reason && s->phase==LGN_ACTIVE && now>=g->next_enter) {
        int e=console_enter(g,s->id,g->enter_reason);
        if (e && e!=-EBUSY) { lgn_note(g,"[logon] secure entry failed: %d",e); g->enter_reason=0; }
        /* A completed entry that kept or raised authentication is a lock. */
        if (!e && s->console.authentication_required && g->mode!=LGN_MODE_LOCKED) set_screen(g,LGN_MODE_LOCKED);
    }
    switch (s->phase) {
    case LGN_NONE:
        if (g->power_action) break;
        if (g->accounts_ready && !g->autologon_done && (g->safe || !g->autologon_candidate)) {
            g->autologon_done=1; g->correlation.autologon_used=1; /* consumed: never later in this boot */
            lgn_note(g,"[logon] automatic logon not applicable safe=%d",g->safe);
            set_screen(g,LGN_MODE_WELCOME);
        } else if (g->accounts_ready && g->own_generation && !g->autologon_done && g->boot_valid) autologon(g);
        else if (g->accounts_ready && g->autologon_done && (g->mode==LGN_MODE_STARTING || g->mode==LGN_MODE_UNAVAILABLE)) {
            /* An automatic logon in flight keeps the waiting screen. */
            int pending=0;
            for (unsigned i=0;i<4;i++) pending|=g->verify[i].occupied && g->verify[i].link==NO_LINK;
            for (unsigned i=0;i<g->auth.queued;i++) pending|=g->auth.queue[i].link==NO_LINK && g->auth.queue[i].purpose!=LGN_PURPOSE_LIST;
            pending|=g->auth.busy && g->auth.active.link==NO_LINK && g->auth.active.purpose!=LGN_PURPOSE_LIST;
            if (!pending) set_screen(g,LGN_MODE_WELCOME);
        } else if (!g->accounts_ready && g->unavailable_since && now-g->unavailable_since>=LGN_UNAVAILABLE_MS)
            set_screen(g,LGN_MODE_UNAVAILABLE);
        break;
    case LGN_CREATE: if (now>=s->next_poll) session_create(g); break;
    case LGN_LAUNCH_W32: {
        int e=launch(g,CIUKI_SECURITY_LAUNCH_W32SRV);
        if (e) session_failed(g,e,LGN_MSG_START_FAILED); else s->phase=LGN_LAUNCH_DESKTOP;
        break;
    }
    case LGN_LAUNCH_DESKTOP: {
        int e=launch(g,CIUKI_SECURITY_LAUNCH_DESKTOP);
        if (e) session_failed(g,e,LGN_MSG_START_FAILED); else { s->phase=LGN_POLL; s->next_poll=now; }
        break;
    }
    case LGN_POLL: if (now>=s->next_poll) { s->next_poll=now+LGN_POLL_MS; session_poll(g); } break;
    case LGN_ACTIVE: if (now>=s->next_poll) { s->next_poll=now+WATCH_MS; session_watch(g); } break;
    case LGN_ENDING: if (now>=s->next_poll) { s->next_poll=now+LGN_COLLECT_MS; session_collect(g); } break;
    }
}
void lgn_pass(struct lgn *g)
{
    lgn_links_accept(g);
    lgn_links_receive(g);
    lgn_auth_poll(g);
    advance(g);
    lgn_auth_poll(g);
    if (g->power_action && !g->power_issued && !g->session.phase) {
        /* Once: REBOOT does not return; OFF returns after storage stopped
         * only when the machine stayed on (the screen then stays POWER). */
        unsigned action=g->power_action;
        g->power_issued=1;
        lgn_note(g,"[logon] %s requested at the logon screen",action==LGN_EVENT_RESTART ? "restart" : "shutdown");
        int e=g->sys->power ? g->sys->power(g->sys->ctx,action) : -ENOSYS;
        if (e && e!=-EIO && e!=-ENOTSUP) {
            lgn_note(g,"[logon] power request refused: %d",e);
            g->power_action=0; g->power_issued=0; set_message(g,LGN_MSG_POWER_FAILED,e); set_screen(g,LGN_MODE_WELCOME);
        } else if (e) { lgn_note(g,"[logon] power off did not complete: %d",e); set_message(g,LGN_MSG_POWER_FAILED,e); }
    }
}
void lgn_step(struct lgn *g, unsigned idle_ms)
{
    lgn_pass(g);
    int fds[LGN_LINKS+1];
    unsigned n=lgn_wait_fds(g,fds,LGN_LINKS+1);
    unsigned wait=idle_ms;
    uint32_t phase=g->session.phase;
    if (phase==LGN_LAUNCH_W32 || phase==LGN_LAUNCH_DESKTOP || (phase==LGN_CREATE && g->session.next_poll<=g->sys->now_ms(g->sys->ctx))) wait=0;
    else if (phase==LGN_CREATE) wait=LGN_COLLECT_MS;   /* SESSION_CREATE answered EBUSY: a session is still closing */
    else if (phase==LGN_POLL || g->enter_reason) wait=LGN_POLL_MS;
    else if (phase==LGN_ENDING) wait=LGN_COLLECT_MS;
    else if (phase==LGN_ACTIVE && wait>WATCH_MS) wait=WATCH_MS;
    if ((!g->accounts_ready || !g->boot_valid) && wait>LGN_LIST_RETRY_MS) wait=LGN_LIST_RETRY_MS;
    if (wait) (void)g->sys->wait(g->sys->ctx,fds,n,wait);
}
void lgn_close(struct lgn *g)
{
    acct_logon_clear(&g->correlation);
    if (g->auth.fd>=0) g->sys->close(g->sys->ctx,g->auth.fd);
    g->auth.fd=-1;
    for (unsigned i=0;i<LGN_LINKS;i++) {
        if (g->links[i].occupied && g->links[i].fd>=0) g->sys->close(g->sys->ctx,g->links[i].fd);
        g->links[i].occupied=0; g->links[i].fd=-1;
    }
}
