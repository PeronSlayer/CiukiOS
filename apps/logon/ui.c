/* SPDX-License-Identifier: MIT
 * LOGON_UI model and view (ui.h). Windows XP's classic Welcome/Unlock/
 * Windows Security dialogs are the behavioural reference (MSGINA): Welcome
 * lists enabled accounts or takes a typed name; the locked screen asks only
 * the owner's password; Ctrl+Alt+Del while logged on offers Lock Computer,
 * Log Off, Shut Down, Task Manager and Cancel. Esc/Cancel clears or goes
 * back and never starts a session; there is no Close button.
 */
#include "ui.h"
#include "../libdeskui/keymap.h"
#include "../desktop/theme.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define WHITE 0xffffffu
#define SOFT 0xc8d8f0u
#define LINE 0x3b6ea5u
#define TILE 0x174a78u
#define WARN 0xffd27fu
#define RIGHT_X 324
#define RIGHT_W 292
#define TAGLINE "Classic computing, one woof at a time."

static uint64_t now_of(struct lui *u) { return u->host->now_ms(u->host->ctx); }
static void note(struct lui *u, const char *line) { if (u->host->log) u->host->log(u->host->ctx,line); }
static void wipe_secret(struct lui *u)
{
    lgn_wipe(u->password,sizeof(u->password));
    lgn_wipe(u->old_password,sizeof(u->old_password)); lgn_wipe(u->confirm,sizeof(u->confirm)); /* sec-user-04 */
}
void lui_wipe(struct lui *u) { wipe_secret(u); lgn_wipe(u->name,sizeof(u->name)); }
static void status(struct lui *u, const char *text) { snprintf(u->status,sizeof(u->status),"%s",text); u->dirty=1; }
static void rd(const uint8_t *p, uint32_t *v) { *v=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }

void lui_init(struct lui *u, const struct lui_host *h, const uint32_t *portrait, int layout)
{
    memset(u,0,sizeof(*u));
    u->host=h; u->portrait=portrait; u->layout=layout; u->visible=1; u->dirty=1;
    u->mouse_x=LUI_W/2; u->mouse_y=LUI_H/2; u->selected=-1;
    u->state.mode=LGN_MODE_STARTING;
}
static int logon_call(struct lui *u, uint32_t op, const void *body, size_t n, void *reply, size_t cap, size_t *used)
{
    int frame=0;
    int e=u->host->call(u->host->ctx,LGN_SERVICE_LOGON,op,body,n,reply,cap,used,&frame);
    return e ? e : frame;
}
static void list(struct lui *u)
{
    uint8_t reply[16+144*LUI_ACCOUNTS]; size_t used=0; int frame=0;
    int e=u->host->call(u->host->ctx,LGN_SERVICE_AUTH,LGN_AUTH_LIST,NULL,0,reply,sizeof(reply),&used,&frame);
    uint32_t count=0;
    if (!e && !frame && used>=16) rd(reply+4,&count);
    if (e || frame || used<16 || count>LUI_ACCOUNTS || used!=16+144*(size_t)count) { u->list_valid=0; u->count=0; u->dirty=1; return; }
    u->count=0;
    for (uint32_t i=0;i<count;i++) {
        const uint8_t *r=reply+16+144*i;
        uint32_t uid; rd(r,&uid);
        if (!memchr(r+16,0,32) || !lgn_name_syntax((const char *)r+16)) continue;
        u->accounts[u->count].uid=uid; memcpy(u->accounts[u->count].name,r+16,32); u->count++;
    }
    u->list_valid=1; u->dirty=1;
    if (u->selected>=(int)u->count) u->selected=-1;
}
static const char *message_text(uint32_t m)
{
    switch (m) {
    case LGN_MSG_SESSION_ENDED: return "Your session ended unexpectedly. Log on again to continue.";
    case LGN_MSG_START_FAILED: return "Your session could not be started. Try logging on again.";
    case LGN_MSG_PROFILE_FAILED: return "Your user profile could not be loaded. Try logging on again.";
    case LGN_MSG_POWER_FAILED: return "CiukiOS could not shut down. Try again in a moment.";
    case LGN_MSG_SERVICE_RESTART: return "A system service restarted, so this computer was locked. Type your password to continue.";
    /* sec-user-04 */
    case LGN_MSG_ADMIN_ENDED: return "An administrator logged off the previous user. Their unsaved work was lost.";
    case LGN_MSG_NOT_ADMIN: return "Only an administrator can log off another user's session.";
    case LGN_MSG_ENROLLED: return "The user accounts were converted. Log on with your new password.";
    default: return NULL;
    }
}
static void enroll_refresh(struct lui *u);   /* sec-user-04 */
static void enter_mode(struct lui *u, uint32_t mode)
{
    wipe_secret(u); u->busy=0; u->retry_until=0; u->status[0]=0; u->focus=0;
    switch (mode) {
    case LGN_MODE_WELCOME: u->view=LUI_VIEW_LIST; u->selected=-1; list(u); break;
    case LGN_MODE_LOCKED: u->view=LUI_VIEW_PASSWORD; u->focus=0; break;
    case LGN_MODE_SECURITY: u->view=LUI_VIEW_MENU; break;
    case LGN_MODE_UNAVAILABLE: u->view=LUI_VIEW_LIST; break;
    case LGN_MODE_ENROLL: u->first_admin=-1; memset(u->labels,0,sizeof(u->labels)); enroll_refresh(u); break; /* sec-user-04 */
    default: break;
    }
}
static int query(struct lui *u, uint32_t command)
{
    struct lgn_ui_command c={command,0,0};
    if (command==LGN_CMD_LOGOFF || command==LGN_CMD_TASKMGR || command==LGN_CMD_FORCE_LOGOFF ||
        command==LGN_CMD_SET_PASSWORD || command==LGN_CMD_SHUTDOWN || command==LGN_CMD_RESTART ||
        command==LGN_CMD_OVERRIDE) {   /* sec-user-04 */
        c.session=u->state.session; c.console=u->state.console;
    }
    uint8_t body[LGN_UI_COMMAND_BYTES], reply[LGN_UI_STATE_BYTES]; size_t used=0;
    if (lgn_ui_command_encode(&c,body,sizeof(body))) return -EINVAL;
    int e=logon_call(u,LGN_OP_UI_STATE,body,sizeof(body),reply,sizeof(reply),&used);
    struct lgn_state st;
    if (!e && (used!=sizeof(reply) || lgn_state_decode(&st,reply,used))) e=-EIO;
    u->next_query=now_of(u)+(u->visible ? LUI_QUERY_MS : LUI_HIDDEN_QUERY_MS);
    if (e) {
        if (command==LGN_CMD_QUERY) { u->state_valid=0; u->dirty=1; }
        return e;
    }
    uint32_t old=u->state_valid ? u->state.mode : 0;
    u->state=st; u->state_valid=1;
    u->visible=st.mode!=LGN_MODE_HIDDEN;
    if (st.mode!=old) enter_mode(u,st.mode);
    if (st.generation!=u->last_generation) { u->last_generation=st.generation; u->dirty=1; }
    const char *text=message_text(st.message);
    if (text) {
        status(u,text);
        (void)query(u,LGN_CMD_ACKNOWLEDGE);
    }
    return 0;
}
void lui_wake(struct lui *u) { u->next_query=0; u->dirty=1; }
void lui_tick(struct lui *u)
{
    uint64_t now=now_of(u);
    if (!u->busy && now>=u->next_query) (void)query(u,LGN_CMD_QUERY);
    if (u->state_valid && u->state.mode==LGN_MODE_WELCOME && !u->list_valid && now>=u->retry_until) {
        list(u); if (!u->list_valid) u->retry_until=now+1000;
    }
    if (u->state_valid && u->state.mode==LGN_MODE_ENROLL && !u->source_valid && !u->busy && now>=u->retry_until) {
        enroll_refresh(u); if (!u->source_valid) u->retry_until=now+1000;   /* sec-user-04 */
    }
}

/* ---- requests --------------------------------------------------------- */
static void failed(struct lui *u, int status_code, uint32_t retry)
{
    wipe_secret(u);
    if (status_code==-EACCES) status(u,"The user name or password is incorrect. Check the name and type the password again.");
    else if (status_code==-EAGAIN || status_code==-EBUSY) status(u,"Please wait a moment, then try again.");
    else if (status_code==LGN_STATUS_NO_PASSWORD && u->state.mode==LGN_MODE_LOCKED) status(u,"");
    else status(u,"Logon is not available right now. Try again in a moment.");
    u->retry_until=now_of(u)+(retry ? retry : status_code==-EAGAIN ? 1000u : 0u);
}
static void submit(struct lui *u)
{
    if (u->busy || now_of(u)<u->retry_until) return;
    int unlock=u->state.mode==LGN_MODE_LOCKED;
    struct acct_begin_wire { uint32_t action, session, console, selector, uid; char name[32]; } b;
    memset(&b,0,sizeof(b));
    b.action=unlock ? 3 : 2;
    if (unlock) { b.session=u->state.session; b.console=u->state.console; b.selector=1; b.uid=u->state.owner_uid; }
    else if (u->view==LUI_VIEW_TYPED) {
        if (!lgn_name_syntax(u->name)) { status(u,"Type a user name: letters, digits, '_' and '-' only."); return; }
        b.selector=2; memcpy(b.name,u->name,32);
    } else if (u->selected>=0 && (unsigned)u->selected<u->count) { b.selector=1; b.uid=u->accounts[u->selected].uid; }
    else return;
    if (unlock && (!b.session || !b.console || !b.uid)) { status(u,"Please wait a moment, then try again."); u->next_query=0; return; }
    /* BEGIN_AUTH (contract A2-1): version@0 action@4 session@8 console@12
     * selector@16 uid@20 name[32]@24 reserved@56. */
    uint8_t begin[64]; memset(begin,0,64);
    uint32_t words[6]={1,b.action,b.session,b.console,b.selector,b.uid};
    for (unsigned i=0;i<6;i++) for (unsigned j=0;j<4;j++) begin[4*i+j]=(uint8_t)(words[i]>>(8*j));
    memcpy(begin+24,b.name,32);
    status(u,unlock ? "Checking password..." : "Checking user name and password...");
    u->busy=1; u->dirty=1;
    uint8_t challenge[64]; size_t used=0;
    int e=logon_call(u,LGN_OP_BEGIN_AUTH,begin,64,challenge,64,&used);
    if (!e && used!=64) e=-EIO;
    uint8_t body[LGN_SUBMIT_BYTES], reply[LGN_PROGRESS_BYTES];
    if (!e) e=lgn_submit_encode(challenge,b.selector==2 ? u->name : "",u->password,"",body);
    wipe_secret(u);
    struct lgn_progress p={0,0,0};
    if (!e) {
        e=logon_call(u,LGN_OP_SUBMIT_AUTH,body,LGN_SUBMIT_BYTES,reply,sizeof(reply),&used);
        lgn_wipe(body,sizeof(body));
        if (!e && (used!=16 || lgn_progress_decode(&p,reply,16))) e=-EIO;
    }
    lgn_wipe(challenge,sizeof(challenge));
    u->busy=0;
    if (e) { failed(u,e,0); note(u,"[logon-ui] logon request refused"); return; }
    if (p.disposition==LGN_AUTHENTICATED && !p.status) {
        status(u,unlock ? "" : "Loading your personal settings...");
        u->next_query=0; lui_tick(u);
        return;
    }
    failed(u,p.status ? p.status : -EACCES,p.retry_after_ms);
    u->next_query=0;
}
/* ---- sec-user-04: administrator log-off and G32 enrollment ---------------
 * One BEGIN_AUTH/SUBMIT_AUTH exchange (contract A2-1 bodies); secrets are
 * wiped by the caller's wipe_secret on every path. */
static int exchange(struct lui *u, uint32_t action, uint32_t selector, uint32_t uid, const char *name,
                    const char *secret, const char *new_secret, struct lgn_progress *p)
{
    uint8_t begin[64]; memset(begin,0,64);
    uint32_t words[6]={1,action,0,0,selector,uid};
    for (unsigned i=0;i<6;i++) for (unsigned j=0;j<4;j++) begin[4*i+j]=(uint8_t)(words[i]>>(8*j));
    if (selector==2) { size_t k=0; while (k<31 && name[k]) k++; memcpy(begin+24,name,k); }
    uint8_t challenge[64], body[LGN_SUBMIT_BYTES], reply[LGN_PROGRESS_BYTES]; size_t used=0;
    int e=logon_call(u,LGN_OP_BEGIN_AUTH,begin,64,challenge,64,&used);
    if (!e && used!=64) e=-EIO;
    if (!e) e=lgn_submit_encode(challenge,selector==2 ? name : "",secret,new_secret,body);
    if (!e) {
        e=logon_call(u,LGN_OP_SUBMIT_AUTH,body,LGN_SUBMIT_BYTES,reply,sizeof(reply),&used);
        if (!e && (used!=16 || lgn_progress_decode(p,reply,16))) e=-EIO;
    }
    lgn_wipe(body,sizeof(body)); lgn_wipe(challenge,sizeof(challenge));
    return e;
}
/* Locked screen: an administrator's typed name and password end the locked
 * session (Windows XP WlxWkstaLockedSAS FORCE_LOGOFF after confirmation). */
static void override_submit(struct lui *u)
{
    if (u->busy || now_of(u)<u->retry_until) return;
    if (!lgn_name_syntax(u->name)) { status(u,"Type an administrator's user name."); return; }
    u->busy=1; status(u,"Checking the administrator's password...");
    int e=query(u,LGN_CMD_OVERRIDE);
    struct lgn_progress p={0,0,0};
    if (!e) e=exchange(u,2,2,0,u->name,u->password,"",&p);
    wipe_secret(u); u->busy=0;
    if (e) { (void)query(u,LGN_CMD_OVERRIDE_CANCEL); failed(u,e,0); u->next_query=0; return; }
    if (p.disposition==LGN_AUTHENTICATED && !p.status) { status(u,"Logging off..."); u->view=LUI_VIEW_PASSWORD; }
    else failed(u,p.status ? p.status : -EACCES,p.retry_after_ms);
    u->next_query=0; lui_tick(u);
}
/* Enrollment state: the protected source (LOGON ENROLL_SOURCE) and, once a
 * plan is staged, the provisional catalogue (AUTH LIST); the next record to
 * enroll is the first unstaged one other than Guest (uid 501). */
static void enroll_refresh(struct lui *u)
{
    uint8_t reply[LGN_SOURCE_BYTES(LGN_SOURCE_MAX)]; size_t used=0;
    int e=logon_call(u,LGN_OP_ENROLL_SOURCE,NULL,0,reply,sizeof(reply),&used);
    u->source_valid=!e && !lgn_source_decode(&u->source,reply,used);
    if (!u->source_valid) { u->view=LUI_VIEW_ENROLL_PLAN; status(u,"The account service is not ready. Try again in a moment."); return; }
    for (unsigned i=0;i<u->source.count;i++)
        if (!u->labels[i][0] && !(u->source.rows[i].flags&LGN_SOURCE_REPLACE)) memcpy(u->labels[i],u->source.rows[i].name,32);
    if (!(u->source.flags&LGN_SOURCE_PLAN)) { u->view=LUI_VIEW_ENROLL_PLAN; return; }
    list(u);
    u->enroll_record=-1;
    for (unsigned i=0;i<u->count;i++)
        if (u->accounts[i].uid!=501 && !(u->source.staged&(1u<<i))) { u->enroll_record=(int)i; break; }
    u->view=LUI_VIEW_ENROLL_PASSWORD; u->focus=0; u->dirty=1;
}
static int enroll_imported(const struct lui *u, uint32_t uid)
{
    for (unsigned i=0;i<u->source.count;i++) if (u->source.rows[i].uid==uid) return 1;
    return 0;
}
static void plan_submit(struct lui *u)
{
    if (u->busy) return;
    uint32_t count=u->source.count ? u->source.count : 1;
    uint8_t body[32+40*LGN_SOURCE_MAX]; memset(body,0,sizeof(body));
    if (u->source.count && (u->first_admin<0 || u->first_admin>=(int)u->source.count)) { status(u,"Choose the account that becomes the first administrator."); return; }
    for (uint32_t i=0;i<count;i++) {
        const char *label=u->source.count ? u->labels[i] : u->name;
        if (!lgn_name_syntax(label)) {
            status(u,u->source.count ? "Every account needs a name: letters, digits, '_' and '-' only." :
                                      "Type a name for the new administrator: letters, digits, '_' and '-' only.");
            return;
        }
        uint8_t *r=body+32+40*i; uint32_t id=u->source.count ? u->source.rows[i].old_id : 0;
        for (unsigned j=0;j<4;j++) r[j]=(uint8_t)(id>>(8*j));
        { size_t k=0; while (k<31 && label[k]) k++; memcpy(r+4,label,k); }
    }
    uint32_t first=u->source.count ? u->source.rows[u->first_admin].old_id : 0;
    uint32_t words[3]={1,count,first};
    for (unsigned i=0;i<3;i++) for (unsigned j=0;j<4;j++) body[4*i+j]=(uint8_t)(words[i]>>(8*j));
    u->busy=1; status(u,"Preparing the accounts...");
    uint8_t reply[16+144*LUI_ACCOUNTS]; size_t used=0;
    int e=logon_call(u,LGN_OP_ENROLL_PLAN,body,32+40*count,reply,sizeof(reply),&used);
    u->busy=0;
    if (e) { status(u,e==-EINVAL ? "These names cannot be used. Choose different names." : "The accounts could not be prepared. Try again."); return; }
    u->status[0]=0; enroll_refresh(u);
}
static void enroll_submit(struct lui *u)
{
    if (u->busy || now_of(u)<u->retry_until || u->enroll_record<0 || u->enroll_record>=(int)u->count) return;
    size_t n=strlen(u->password);
    if (n<8 || n>63) { wipe_secret(u); status(u,"The new password must have 8 to 63 characters."); return; }
    if (strcmp(u->password,u->confirm)) { wipe_secret(u); status(u,"The passwords do not match. Type them again."); return; }
    uint32_t uid=u->accounts[u->enroll_record].uid;
    u->busy=1; status(u,"Saving the password...");
    struct lgn_progress p={0,0,0};
    int e=exchange(u,4,1,uid,"",enroll_imported(u,uid) ? u->old_password : "",u->password,&p);
    wipe_secret(u); u->busy=0;
    if (e) { failed(u,e,0); return; }
    if (!p.status && p.disposition==LGN_ENROLL_STAGED) { u->status[0]=0; enroll_refresh(u); return; }
    if (!p.status && p.disposition==LGN_ENROLL_COMMITTED) { status(u,"The accounts were converted."); u->next_query=0; lui_tick(u); return; }
    if (p.status==-EACCES) { status(u,"The old password is incorrect. Type the password this account used before the upgrade."); u->retry_until=now_of(u)+1000; }
    else failed(u,p.status ? p.status : -EIO,p.retry_after_ms);
}
/* ---- end sec-user-04 ---------------------------------------------------- */
static void event(struct lui *u, uint32_t op, uint32_t action)
{
    uint8_t body[32], reply[16]; size_t used=0; memset(body,0,32);
    uint32_t words[4]={1,action,op==LGN_OP_POWER_EVENT && !(u->state.flags&LGN_STATE_SESSION) ? 0 : u->state.session,u->state.console};
    for (unsigned i=0;i<4;i++) for (unsigned j=0;j<4;j++) body[4*i+j]=(uint8_t)(words[i]>>(8*j));
    struct lgn_progress p={0,0,0};
    int e=logon_call(u,op,body,32,reply,16,&used);
    if (!e && (used!=16 || lgn_progress_decode(&p,reply,16))) e=-EIO;
    if (!e && p.status==LGN_STATUS_NO_PASSWORD) { u->view=LUI_VIEW_BLANK_LOCK; u->focus=0; u->dirty=1; }
    else if (e) failed(u,e,0);
    u->next_query=0; lui_tick(u);
}
static void command(struct lui *u, uint32_t c)
{
    int e=query(u,c);
    if (e) failed(u,e,0);
    u->next_query=0; u->dirty=1;
}
static void activate(struct lui *u, const struct lui_item *it)
{
    if (!it) return;
    switch (it->action) {
    case LUI_TILE:
        u->selected=it->index; u->view=LUI_VIEW_PASSWORD; u->focus=0; wipe_secret(u); u->status[0]=0; break;
    case LUI_FIELD_NAME: case LUI_FIELD_PASSWORD: break;
    case LUI_OK: submit(u); break;
    case LUI_BACK:
        wipe_secret(u); u->status[0]=0; u->focus=0;
        if (u->view==LUI_VIEW_OVERRIDE) { u->view=LUI_VIEW_PASSWORD; lgn_wipe(u->name,sizeof(u->name)); (void)query(u,LGN_CMD_OVERRIDE_CANCEL); } /* sec-user-04 */
        else if (u->state.mode==LGN_MODE_SECURITY) u->view=LUI_VIEW_MENU;
        else if (u->state.mode!=LGN_MODE_LOCKED) { u->view=LUI_VIEW_LIST; u->selected=-1; lgn_wipe(u->name,sizeof(u->name)); }
        break;
    case LUI_TYPED: u->view=LUI_VIEW_TYPED; u->focus=0; wipe_secret(u); lgn_wipe(u->name,sizeof(u->name)); u->status[0]=0; break;
    case LUI_POWER_MENU: u->view=LUI_VIEW_CONFIRM_POWER; u->focus=0; break;
    case LUI_SHUTDOWN: case LUI_RESTART:
        if (u->state.mode==LGN_MODE_SECURITY) command(u,it->action==LUI_SHUTDOWN ? LGN_CMD_SHUTDOWN : LGN_CMD_RESTART);
        else event(u,LGN_OP_POWER_EVENT,it->action==LUI_SHUTDOWN ? LGN_EVENT_SHUTDOWN : LGN_EVENT_RESTART);
        break;
    case LUI_RETRY:
        if (u->state.mode==LGN_MODE_ENROLL) { u->status[0]=0; enroll_refresh(u); break; }   /* sec-user-04 */
        u->next_query=0; u->list_valid=0; u->retry_until=0; lui_tick(u); break;
    case LUI_LOCK: event(u,LGN_OP_LOCK_EVENT,LGN_EVENT_LOCK); break;
    case LUI_LOGOFF: u->view=LUI_VIEW_CONFIRM_LOGOFF; u->focus=0; break;
    case LUI_LOGOFF_CONFIRM: command(u,LGN_CMD_LOGOFF); break;
    case LUI_TASKMGR: command(u,LGN_CMD_TASKMGR); break;
    case LUI_SET_PASSWORD: command(u,LGN_CMD_SET_PASSWORD); break;
    case LUI_END_SESSION: command(u,LGN_CMD_FORCE_LOGOFF); break;
    case LUI_CANCEL:
        if (u->view!=LUI_VIEW_MENU && u->state.mode==LGN_MODE_SECURITY) u->view=LUI_VIEW_MENU;
        else if (u->state.mode==LGN_MODE_SECURITY) event(u,LGN_OP_LOCK_EVENT,LGN_EVENT_MENU_RETURN);
        else if (u->state.mode==LGN_MODE_WELCOME || u->state.mode==LGN_MODE_UNAVAILABLE) u->view=LUI_VIEW_LIST;
        else if (u->state.mode==LGN_MODE_ENROLL) enroll_refresh(u);   /* sec-user-04: back to the enrollment step */
        u->focus=0; break;
    case LUI_DISMISS: u->status[0]=0; break;
    /* sec-user-04 */
    case LUI_OVERRIDE: u->view=LUI_VIEW_OVERRIDE; u->focus=0; wipe_secret(u); lgn_wipe(u->name,sizeof(u->name)); u->status[0]=0; break;
    case LUI_OVERRIDE_OK: override_submit(u); break;
    case LUI_ROW: u->first_admin=it->index; u->focus=0; u->status[0]=0; break;
    case LUI_PLAN_OK: plan_submit(u); break;
    case LUI_ENROLL_OK: enroll_submit(u); break;
    case LUI_FIELD_OLD: case LUI_FIELD_CONFIRM: case LUI_FIELD_LABEL: break;
    }
    u->dirty=1;
}

/* ---- layout ------------------------------------------------------------- */
static void add(struct lui *u, int action, int index, int x, int y, int w, int h, const char *label)
{
    if (u->item_count<LUI_ITEMS) u->items[u->item_count++]=(struct lui_item){action,index,{x,y,w,h},label};
}
static int blank_locked(const struct lui *u) { return u->state.mode==LGN_MODE_LOCKED && (u->state.flags&LGN_STATE_BLANK); }
static void layout(struct lui *u)
{
    u->item_count=0;
    if (!u->state_valid) return;
    uint32_t mode=u->state.mode;
    int welcome=mode==LGN_MODE_WELCOME, power_choice=u->view==LUI_VIEW_CONFIRM_POWER;
    if (welcome && !power_choice) {
        if (u->view==LUI_VIEW_LIST) {
            for (unsigned i=0;i<u->count && i<6;i++) add(u,LUI_TILE,(int)i,RIGHT_X,124+(int)i*44,RIGHT_W,38,u->accounts[i].name);
            add(u,LUI_TYPED,0,24,438,180,28,"Type a user name");
        } else if (u->view==LUI_VIEW_PASSWORD) {
            add(u,LUI_FIELD_PASSWORD,0,RIGHT_X,168,212,24,NULL);
            add(u,LUI_OK,0,RIGHT_X+220,167,72,26,"OK");
            add(u,LUI_BACK,0,24,438,180,28,"Back to the user list");
        } else {
            add(u,LUI_FIELD_NAME,0,RIGHT_X,150,RIGHT_W,24,NULL);
            add(u,LUI_FIELD_PASSWORD,0,RIGHT_X,206,RIGHT_W,24,NULL);
            add(u,LUI_OK,0,RIGHT_X+132,246,76,26,"OK");
            add(u,LUI_BACK,0,RIGHT_X+216,246,76,26,"Cancel");
        }
        add(u,LUI_POWER_MENU,0,LUI_W-148,438,124,28,"Turn Off Computer");
    } else if (power_choice && (welcome || mode==LGN_MODE_SECURITY || mode==LGN_MODE_UNAVAILABLE || mode==LGN_MODE_ENROLL /* sec-user-04 */)) {
        add(u,LUI_SHUTDOWN,0,RIGHT_X,196,92,28,"Shut Down");
        add(u,LUI_RESTART,0,RIGHT_X+100,196,92,28,"Restart");
        add(u,LUI_CANCEL,0,RIGHT_X+200,196,92,28,"Cancel");
    } else if (mode==LGN_MODE_LOCKED && u->view==LUI_VIEW_OVERRIDE) {   /* sec-user-04 */
        add(u,LUI_FIELD_NAME,0,RIGHT_X,236,RIGHT_W,24,NULL);
        add(u,LUI_FIELD_PASSWORD,0,RIGHT_X,286,RIGHT_W,24,NULL);
        add(u,LUI_OVERRIDE_OK,0,RIGHT_X+92,322,108,26,"Log Off User");
        add(u,LUI_BACK,0,RIGHT_X+208,322,84,26,"Cancel");
    } else if (mode==LGN_MODE_LOCKED) {
        if (blank_locked(u)) add(u,LUI_END_SESSION,0,RIGHT_X,236,120,28,"End Session");
        else {
            add(u,LUI_FIELD_PASSWORD,0,RIGHT_X,190,212,24,NULL);
            add(u,LUI_OK,0,RIGHT_X+220,189,72,26,"OK");
        }
        add(u,LUI_OVERRIDE,0,24,438,220,28,"Log off this user (administrator)");   /* sec-user-04 */
    } else if (mode==LGN_MODE_SECURITY) {
        if (u->view==LUI_VIEW_CONFIRM_LOGOFF) {
            add(u,LUI_LOGOFF_CONFIRM,0,RIGHT_X,212,92,28,"Log Off");
            add(u,LUI_CANCEL,0,RIGHT_X+100,212,92,28,"Cancel");
        } else if (u->view==LUI_VIEW_BLANK_LOCK) {
            add(u,LUI_SET_PASSWORD,0,RIGHT_X,224,124,28,"Set Password...");
            add(u,LUI_CANCEL,0,RIGHT_X+132,224,92,28,"Cancel");
        } else {
            static const struct { int action; const char *label; } menu[]={
                {LUI_LOCK,"Lock Computer"},{LUI_LOGOFF,"Log Off..."},{LUI_POWER_MENU,"Shut Down..."},
                {LUI_TASKMGR,"Task Manager"},{LUI_CANCEL,"Cancel"}};
            for (unsigned i=0;i<5;i++) add(u,menu[i].action,0,RIGHT_X,140+(int)i*36,180,28,menu[i].label);
        }
    } else if (mode==LGN_MODE_UNAVAILABLE) {
        add(u,LUI_RETRY,0,RIGHT_X,230,92,28,"Retry");
        add(u,LUI_POWER_MENU,0,LUI_W-148,438,124,28,"Turn Off Computer");
    } else if (mode==LGN_MODE_ENROLL && u->view==LUI_VIEW_ENROLL_PASSWORD && u->enroll_record>=0) {   /* sec-user-04 */
        int imported=enroll_imported(u,u->accounts[u->enroll_record].uid), y=imported ? 248 : 200;
        if (imported) add(u,LUI_FIELD_OLD,0,RIGHT_X,200,RIGHT_W,22,NULL);
        add(u,LUI_FIELD_PASSWORD,0,RIGHT_X,y,RIGHT_W,22,NULL);
        add(u,LUI_FIELD_CONFIRM,0,RIGHT_X,y+42,RIGHT_W,22,NULL);
        add(u,LUI_ENROLL_OK,0,RIGHT_X+200,y+74,92,26,"Next");
        add(u,LUI_POWER_MENU,0,LUI_W-148,438,124,28,"Turn Off Computer");
    } else if (mode==LGN_MODE_ENROLL && u->source_valid) {
        if (!u->source.count) add(u,LUI_FIELD_NAME,0,RIGHT_X,196,RIGHT_W,22,NULL);
        for (unsigned i=0;i<u->source.count;i++) add(u,LUI_ROW,(int)i,RIGHT_X,154+(int)i*15,RIGHT_W,15,u->labels[i]);
        if (u->first_admin>=0 && (unsigned)u->first_admin<u->source.count) add(u,LUI_FIELD_LABEL,u->first_admin,RIGHT_X,398,180,20,NULL);
        add(u,LUI_PLAN_OK,0,RIGHT_X+200,396,92,24,"Continue");
        add(u,LUI_POWER_MENU,0,LUI_W-148,438,124,28,"Turn Off Computer");
    } else if (mode==LGN_MODE_ENROLL) {
        add(u,LUI_RETRY,0,RIGHT_X,230,92,28,"Retry");
        add(u,LUI_POWER_MENU,0,LUI_W-148,438,124,28,"Turn Off Computer");
    }
    if (u->focus>=(int)u->item_count) u->focus=0;
}
static const struct lui_item *focused(const struct lui *u)
{ return u->item_count ? &u->items[u->focus<(int)u->item_count ? u->focus : 0] : NULL; }
static char *field_text(struct lui *u, int action)
{
    /* sec-user-04: enrollment's old/confirm secrets and the selected row's label. */
    if (action==LUI_FIELD_OLD) return u->old_password;
    if (action==LUI_FIELD_CONFIRM) return u->confirm;
    if (action==LUI_FIELD_LABEL) return u->first_admin>=0 && u->first_admin<(int)LGN_SOURCE_MAX ? u->labels[u->first_admin] : u->name;
    return action==LUI_FIELD_NAME ? u->name : u->password;
}
static int field_action(int action)
{ return action==LUI_FIELD_NAME || action==LUI_FIELD_PASSWORD || action==LUI_FIELD_OLD || action==LUI_FIELD_CONFIRM || action==LUI_FIELD_LABEL; }

/* ---- input -------------------------------------------------------------- */
static void type_key(struct lui *u, unsigned code)
{
    const struct lui_item *it=focused(u);
    if (!it || !field_action(it->action)) return;
    char *text=field_text(u,it->action);
    size_t n=strlen(text), cap=it->action==LUI_FIELD_NAME || it->action==LUI_FIELD_LABEL ? 31 : 63;
    if (code==DU_KEY_BACKSPACE) { if (n) text[n-1]=0; u->dirty=1; return; }
    unsigned state=((u->modifiers&DU_SHIFT) ? DU_KEYMAP_SHIFT : 0)|((u->modifiers&8u) ? DU_KEYMAP_ALTGR : 0)|
                   ((u->modifiers&16u) ? DU_KEYMAP_CAPS : 0);
    uint32_t cp=du_keymap((enum du_layout)u->layout,code,state);
    if (cp<0x20 || cp>0x7e || n>=cap) return;   /* printable ASCII: the account service's secret/name rules */
    text[n]=(char)cp; text[n+1]=0; u->dirty=1;
}
void lui_input(struct lui *u, const struct ciuki_input_event *e)
{
    if (e->type==CIUKI_INPUT_ATTENTION) { (void)query(u,LGN_CMD_ATTENTION); u->dirty=1; return; }
    if (e->type==CIUKI_INPUT_RESYNC) { u->modifiers&=16u; lui_wake(u); return; }
    if (!u->visible || u->busy) return;
    layout(u);
    if (e->type==CIUKI_INPUT_MOTION) { u->mouse_x=e->value; u->mouse_y=e->value2; u->dirty=1; return; }
    if (e->type==CIUKI_INPUT_BUTTON && e->code==CIUKI_BUTTON_LEFT && e->value) {
        for (unsigned i=0;i<u->item_count;i++) if (du_contains(u->items[i].box,u->mouse_x,u->mouse_y)) {
            u->focus=(int)i; struct lui_item it=u->items[i]; activate(u,&it); break;
        }
        u->dirty=1; return;
    }
    if (e->type!=CIUKI_INPUT_KEY) return;   /* text, wheel and consumer keys never reach a field */
    unsigned code=(unsigned)e->code, bit=code==DU_KEY_LSHIFT || code==DU_KEY_RSHIFT ? DU_SHIFT :
        code==DU_KEY_LCTRL || code==DU_KEY_RCTRL ? DU_CTRL : code==DU_KEY_LALT ? DU_ALT : code==DU_KEY_RALT ? 8u : 0;
    if (bit) { if (e->value) u->modifiers|=bit; else u->modifiers&=~bit; return; }
    if (!e->value) return;
    if (code==0x3a) { u->modifiers^=16u; return; }
    if (now_of(u)<u->retry_until && (code==DU_KEY_ENTER || code==0x11c)) return;
    const struct lui_item *it=focused(u);
    switch (code) {
    case DU_KEY_ENTER: case 0x11c:
        if (it && field_action(it->action)) {
            /* sec-user-04: multi-field forms move to the next field, then submit. */
            if (u->view==LUI_VIEW_OVERRIDE) { if (it->action==LUI_FIELD_NAME) u->focus++; else override_submit(u); break; }
            if (u->view==LUI_VIEW_ENROLL_PASSWORD) { if (it->action!=LUI_FIELD_CONFIRM) u->focus++; else enroll_submit(u); break; }
            if (u->view==LUI_VIEW_ENROLL_PLAN) { plan_submit(u); break; }
            if (it->action==LUI_FIELD_NAME && u->view==LUI_VIEW_TYPED) { u->focus++; break; }
            submit(u);
        } else if (it) { struct lui_item copy=*it; activate(u,&copy); }
        break;
    case DU_KEY_ESC: {
        /* Esc clears the attempt or returns; it never starts a session. */
        int back=u->state.mode==LGN_MODE_SECURITY ? LUI_CANCEL : LUI_BACK;
        if (u->state.mode==LGN_MODE_LOCKED && u->view==LUI_VIEW_OVERRIDE) { struct lui_item c={LUI_BACK,0,{0,0,0,0},0}; activate(u,&c); } /* sec-user-04 */
        else if (u->state.mode==LGN_MODE_ENROLL && u->view!=LUI_VIEW_CONFIRM_POWER) { wipe_secret(u); u->status[0]=0; }
        else if (u->state.mode==LGN_MODE_LOCKED) { wipe_secret(u); u->status[0]=0; }
        else if (u->state.mode==LGN_MODE_SECURITY && u->view==LUI_VIEW_MENU) { struct lui_item c={LUI_CANCEL,0,{0,0,0,0},0}; activate(u,&c); }
        else { struct lui_item c={back,0,{0,0,0,0},0}; if (u->view==LUI_VIEW_CONFIRM_POWER) c.action=LUI_CANCEL; activate(u,&c); }
        break;
    }
    case DU_KEY_TAB:
        if (u->item_count) u->focus=(u->modifiers&DU_SHIFT) ? (u->focus+(int)u->item_count-1)%(int)u->item_count : (u->focus+1)%(int)u->item_count;
        break;
    case DU_KEY_UP: case DU_KEY_DOWN:
        if (u->item_count) u->focus=code==DU_KEY_UP ? (u->focus+(int)u->item_count-1)%(int)u->item_count : (u->focus+1)%(int)u->item_count;
        break;
    default: type_key(u,code); break;
    }
    u->dirty=1;
}

/* ---- view ----------------------------------------------------------------- */
static void text(struct du_canvas *c, int font, int x, int y, const char *s, uint32_t ink) { du_text_font(c,font,x,y,s,ink); }
static void centred(struct du_canvas *c, int font, int cx, int y, const char *s, uint32_t ink)
{ text(c,font,cx-du_text_width_font(font,s)/2,y,s,ink); }
/* Word wrap within width; returns the next y. */
static int paragraph(struct du_canvas *c, int x, int y, int w, const char *s, uint32_t ink)
{
    char line[160]; int line_h=desk_font_line(DESK_FONT_UI)+3;
    while (*s) {
        size_t n=0, fit=0;
        while (s[n] && n<sizeof(line)-1) {
            if (s[n]==' ' || !s[n+1]) {
                size_t end=s[n]==' ' ? n : n+1;
                memcpy(line,s,end); line[end]=0;
                if (du_text_width_font(DESK_FONT_UI,line)>w && fit) break;
                fit=end;
            }
            n++;
        }
        if (!fit) fit=n;
        memcpy(line,s,fit); line[fit]=0;
        text(c,DESK_FONT_UI,x,y,line,ink); y+=line_h;
        s+=fit; while (*s==' ') s++;
    }
    return y;
}
static void button(struct du_canvas *c, const struct lui_item *it, int focus)
{
    du_raised(c,it->box,0,focus);
    int tw=du_text_width_font(DESK_FONT_UI,it->label);
    text(c,DESK_FONT_UI,it->box.x+(it->box.w-tw)/2,it->box.y+(it->box.h-desk_font_cap(DESK_FONT_UI))/2,it->label,desk_theme->ink);
    if (focus) du_focus_ring(c,(struct du_rect){it->box.x+3,it->box.y+3,it->box.w-6,it->box.h-6});
}
static void field(struct du_canvas *c, const struct lui_item *it, const char *value, int secret, int focus)
{
    du_well(c,it->box,WHITE);
    char shown[64]; size_t n=strlen(value);
    if (secret) { memset(shown,'*',n); shown[n]=0; } else snprintf(shown,sizeof(shown),"%s",value);
    du_text_fit(c,it->box.x+6,it->box.y+6,it->box.w-12,shown,desk_theme->ink);
    if (focus) {
        int x=it->box.x+6+du_text_width_font(DESK_FONT_UI,shown);
        if (x>it->box.x+it->box.w-4) x=it->box.x+it->box.w-4;
        du_vline(c,x,it->box.y+4,it->box.h-8,desk_theme->ink);
    }
}
static void portrait(struct lui *u, struct du_canvas *c, int x, int y, int size)
{
    if (!u->portrait) { du_fill(c,(struct du_rect){x,y,size,size},TILE); return; }
    for (int j=0;j<size;j++) for (int i=0;i<size;i++) {
        int px=x+i, py=y+j;
        if (px<c->clip.x || py<c->clip.y || px>=c->clip.x+c->clip.w || py>=c->clip.y+c->clip.h) continue;
        c->pixels[py*c->width+px]=u->portrait[(j*256/size)*256+(i*256/size)]&0xffffffu;
    }
}
static void user_glyph(struct du_canvas *c, int x, int y, const char *name)
{
    du_fill(c,(struct du_rect){x,y,30,30},WARN);
    du_outline(c,(struct du_rect){x,y,30,30},WHITE);
    char initial[2]={(char)(name[0]>='a' && name[0]<='z' ? name[0]-32 : name[0]),0};
    centred(c,DESK_FONT_TITLE,x+15,y+9,initial,LUI_BAND);
}
/* sec-user-06: one serial record per painted view change (mode, view,
 * items, listed accounts, status class), so the suites drive Welcome, the
 * locked screen and the security menu from LOGON_UI's own state. Classes
 * and counts only: no name, no typed text. */
static const char *status_class(const struct lui *u)
{
    if (!u->status[0]) return "none";
    if (!strncmp(u->status,"The user name or password is incorrect",38)) return "incorrect";
    if (!strncmp(u->status,"Please wait",11)) return "wait";
    if (!strncmp(u->status,"Logon is not available",22)) return "unavailable";
    return "other";
}
static void view_record(struct lui *u)
{
    static const char *const modes[]={"none","starting","welcome","loading","hidden","locked","security",
                                      "ending","unavailable","power","enroll"};
    static const char *const views[]={"list","password","typed","confirm-logoff","confirm-power","blank-lock","menu",
                                      "override","enroll-plan","enroll-password"};
    static const char *const classes[]={"none","incorrect","wait","unavailable","other"};
    const char *cls=status_class(u);
    uint32_t klass=0;
    while (klass<4 && strcmp(classes[klass],cls)) klass++;
    uint32_t mode=u->state_valid ? u->state.mode : 0;
    uint32_t now[5]={mode,(uint32_t)u->view,u->item_count,u->count,klass};
    if (u->recorded_valid && !memcmp(now,u->recorded,sizeof(now))) return;
    memcpy(u->recorded,now,sizeof(now)); u->recorded_valid=1;
    char line[128];
    snprintf(line,sizeof(line),"[logon-ui] view mode=%s view=%s items=%u accounts=%u status=%s",
             mode<sizeof(modes)/sizeof(modes[0]) ? modes[mode] : "other",
             (unsigned)u->view<sizeof(views)/sizeof(views[0]) ? views[u->view] : "other",
             u->item_count,u->count,cls);
    note(u,line);
}
void lui_paint(struct lui *u, struct du_canvas *c)
{
    layout(u);
    view_record(u);   /* sec-user-06 */
    du_fill(c,(struct du_rect){0,0,LUI_W,LUI_H},LUI_BACKGROUND);
    du_fill(c,(struct du_rect){0,0,LUI_W,56},LUI_BAND);
    du_fill(c,(struct du_rect){0,424,LUI_W,56},LUI_BAND);
    du_hline(c,0,56,LUI_W,LINE); du_hline(c,0,423,LUI_W,LINE);
    portrait(u,c,70,90,160);
    centred(c,DESK_FONT_LARGE,150,268,"CiukiOS",WHITE);
    centred(c,DESK_FONT_UI,150,300,TAGLINE,SOFT);
    du_vline(c,300,80,320,LINE);
    uint32_t mode=u->state_valid ? u->state.mode : 0;
    const char *head=NULL, *body=NULL; char line[320];
    switch (mode) {
    case 0: head="Please wait..."; body="Connecting to the logon service."; break;
    case LGN_MODE_STARTING: head="Please wait..."; body="CiukiOS is preparing the logon."; break;
    case LGN_MODE_LOADING: head="Welcome"; body="Loading your personal settings..."; break;
    case LGN_MODE_ENDING: head="Logging off..."; body="Saving your settings and closing your programs."; break;
    case LGN_MODE_POWER: head=(u->state.flags&LGN_STATE_RESTART) ? "CiukiOS is restarting..." : "CiukiOS is shutting down...";
        body="Please wait."; break;
    case LGN_MODE_UNAVAILABLE: head="Logon is not available";
        body=u->state.error==-ENOENT ?
            "The user accounts of this computer come from an earlier CiukiOS and must be converted before anyone can log on. This version cannot convert them yet. Turn off the computer." :
            "CiukiOS cannot reach its account service, so nobody can log on right now. Try again, or turn off the computer."; break;
    case LGN_MODE_HIDDEN: head="Please wait..."; break;
    default: break;
    }
    if (u->view==LUI_VIEW_CONFIRM_POWER && (mode==LGN_MODE_WELCOME || mode==LGN_MODE_SECURITY || mode==LGN_MODE_UNAVAILABLE ||
                                           mode==LGN_MODE_ENROLL /* sec-user-04 */)) {
        head="Turn off computer";
        body=mode==LGN_MODE_SECURITY ? "What do you want the computer to do? Your programs will be asked to save your work first." :
                                      "What do you want the computer to do?";
    } else if (mode==LGN_MODE_WELCOME) {
        if (u->view==LUI_VIEW_LIST) {
            head="To begin, click your user name";
            if (!u->list_valid) body="The list of users is not available. Type a user name instead.";
            else if (!u->count) body="No user can log on from this list. Type a user name instead.";
        } else if (u->view==LUI_VIEW_PASSWORD && u->selected>=0) {
            head=u->accounts[u->selected].name; body="Type your password";
        } else { head="Log on to CiukiOS"; }
    } else if (mode==LGN_MODE_LOCKED && u->view==LUI_VIEW_OVERRIDE) {   /* sec-user-04 */
        snprintf(line,sizeof(line),"An administrator can end this session. %s's programs will be closed and unsaved work "
                 "will be lost. Type an administrator's user name and password.",
                 u->state.owner_name[0] ? u->state.owner_name : "The user");
        head="Log off this user"; body=line;
    } else if (mode==LGN_MODE_ENROLL) {   /* sec-user-04 */
        if (!u->source_valid) { head="Please wait..."; body="The upgraded user accounts are being prepared."; }
        else if (u->view==LUI_VIEW_ENROLL_PASSWORD && u->enroll_record>=0) {
            uint32_t uid=u->accounts[u->enroll_record].uid;
            head=u->accounts[u->enroll_record].name;
            if (uid==500) body="Choose the recovery password of the built-in administrator, used in Safe Mode (8 to 63 characters).";
            else if (enroll_imported(u,uid)) body="Type the password this account used before the upgrade (leave it empty if it had none), then choose a new password (8 to 63 characters).";
            else body="Choose a password for this administrator (8 to 63 characters).";
        } else if (u->view==LUI_VIEW_ENROLL_PASSWORD) { head="Please wait..."; body="Converting the accounts."; }
        else if (!u->source.count) { head="Create the administrator";
            body="This computer was upgraded without user accounts. Type a name for the first administrator."; }
        else { head="Convert the user accounts";
            body="Click the first administrator; the others become standard users. Rows marked * need a new name."; }
    } else if (mode==LGN_MODE_LOCKED) {
        head="This computer is locked";
        if (blank_locked(u)) {
            snprintf(line,sizeof(line),"%s has no password, so this session cannot be unlocked. End the session to log on again; "
                     "programs that are still open will be closed and unsaved work will be lost.",
                     u->state.owner_name[0] ? u->state.owner_name : "The owner");
        } else snprintf(line,sizeof(line),"Only %s can unlock this computer. Type the password, then press Enter.",
                        u->state.owner_name[0] ? u->state.owner_name : "the user who locked it");
        body=line;
    } else if (mode==LGN_MODE_SECURITY) {
        if (u->view==LUI_VIEW_CONFIRM_LOGOFF) { head="Log Off CiukiOS";
            body="Are you sure you want to log off? Your programs will be asked to save your work first."; }
        else if (u->view==LUI_VIEW_BLANK_LOCK) { head="Set a password first";
            body="Set a password to lock this account. Without a password, anyone can use this computer while you are away."; }
        else { head="CiukiOS Security";
            snprintf(line,sizeof(line),"You are logged on as %s.",u->state.owner_name[0] ? u->state.owner_name : "a user"); body=line; }
    }
    if (head) text(c,DESK_FONT_TITLE,RIGHT_X,90,head,WHITE);
    int y=114;
    if (body) y=paragraph(c,RIGHT_X,y,RIGHT_W,body,SOFT);
    if (mode==LGN_MODE_WELCOME && u->view==LUI_VIEW_TYPED && u->view!=LUI_VIEW_CONFIRM_POWER) {
        text(c,DESK_FONT_UI,RIGHT_X,134,"User name:",WHITE);
        text(c,DESK_FONT_UI,RIGHT_X,190,"Password:",WHITE);
    }
    if (mode==LGN_MODE_LOCKED && !blank_locked(u) && u->view!=LUI_VIEW_OVERRIDE) text(c,DESK_FONT_UI,RIGHT_X,174,"Password:",WHITE);
    /* sec-user-04: field captions of the override and enrollment forms. */
    for (unsigned i=0;i<u->item_count;i++) {
        const struct lui_item *it=&u->items[i];
        const char *caption=it->action==LUI_FIELD_OLD ? "Old password:" : it->action==LUI_FIELD_CONFIRM ? "Confirm new password:" :
            it->action==LUI_FIELD_LABEL ? "New name for the selected account:" : NULL;
        if (it->action==LUI_FIELD_PASSWORD && (u->view==LUI_VIEW_OVERRIDE || u->view==LUI_VIEW_ENROLL_PASSWORD))
            caption=u->view==LUI_VIEW_OVERRIDE ? "Administrator's password:" : "New password:";
        if (it->action==LUI_FIELD_NAME && (u->view==LUI_VIEW_OVERRIDE || mode==LGN_MODE_ENROLL))
            caption=u->view==LUI_VIEW_OVERRIDE ? "Administrator's user name:" : "Administrator name:";
        if (caption) text(c,DESK_FONT_UI,it->box.x,it->box.y-14,caption,WHITE);
    }
    for (unsigned i=0;i<u->item_count;i++) {
        const struct lui_item *it=&u->items[i]; int f=(int)i==u->focus;
        int hot=du_contains(it->box,u->mouse_x,u->mouse_y);
        if (it->action==LUI_TILE) {
            if (f || hot || it->index==u->selected) du_fill(c,it->box,TILE);
            if (f) du_outline(c,it->box,LINE);
            user_glyph(c,it->box.x+4,it->box.y+4,it->label);
            text(c,DESK_FONT_TITLE,it->box.x+44,it->box.y+13,it->label,WHITE);
        } else if (it->action==LUI_ROW) {   /* sec-user-04: one G32 source row */
            int chosen=it->index==u->first_admin;
            if (chosen || f || hot) du_fill(c,it->box,TILE);
            char row[96]; const struct lgn_source *src=&u->source;
            snprintf(row,sizeof(row),"%s%s%s",it->label[0] ? it->label : src->rows[it->index].name,
                     (src->rows[it->index].flags&LGN_SOURCE_REPLACE) && !it->label[0] ? " *" : "",
                     chosen ? "  - administrator" : "");
            du_text_fit(c,it->box.x+4,it->box.y+2,it->box.w-8,row,chosen ? WHITE : SOFT);
        } else if (field_action(it->action))
            field(c,it,field_text(u,it->action),it->action==LUI_FIELD_PASSWORD || it->action==LUI_FIELD_OLD || it->action==LUI_FIELD_CONFIRM,f);
        else if (it->action==LUI_TYPED || it->action==LUI_BACK || it->action==LUI_OVERRIDE) {
            if (it->action==LUI_BACK && it->box.y<400) { button(c,it,f); continue; }
            text(c,DESK_FONT_UI,it->box.x,it->box.y+8,it->label,hot || f ? WHITE : SOFT);
            du_hline(c,it->box.x,it->box.y+8+desk_font_cap(DESK_FONT_UI)+3,du_text_width_font(DESK_FONT_UI,it->label),hot || f ? WHITE : SOFT);
        } else button(c,it,f);
    }
    /* sec-user-04: the taller forms keep their status under the portrait. */
    if (u->status[0] && (u->view==LUI_VIEW_OVERRIDE || mode==LGN_MODE_ENROLL)) paragraph(c,24,326,252,u->status,WARN);
    else if (u->status[0]) paragraph(c,RIGHT_X,y>290 ? y+8 : 300,RIGHT_W,u->status,WARN);
    if (u->state_valid && (u->state.flags&LGN_STATE_SAFE)) {
        text(c,DESK_FONT_BOLD,8,8,"Safe Mode",WHITE); text(c,DESK_FONT_BOLD,LUI_W-8-du_text_width_font(DESK_FONT_BOLD,"Safe Mode"),8,"Safe Mode",WHITE);
    }
    /* Arrow pointer. */
    for (int j=0;j<12;j++) for (int i=0;i<=j/2;i++) {
        int px=u->mouse_x+i, py=u->mouse_y+j;
        if (px>=0 && py>=0 && px<LUI_W && py<LUI_H) c->pixels[py*c->width+px]=(!i || i==j/2 || j==11) ? 0 : WHITE;
    }
    u->dirty=0;
}
