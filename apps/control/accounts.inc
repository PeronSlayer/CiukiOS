/* SPDX-License-Identifier: MIT
 * sec-user-04: User Accounts (contract 7.1), included by app.c after
 * users.inc. Windows XP's User Accounts is the reference: one applet that
 * merges Users and Passwords; an administrator picks an account, then a task
 * (create, name, password, picture, type, turn on/off, delete); a limited
 * (Standard) user sees only his own account and may change his own password
 * and picture; Guest may change its own password with the old one. Controls
 * a caller may not use stay visible but disabled, with the reason ("You must
 * be an administrator to change this setting."), as Microsoft's Fast User
 * Switching design guide asks ("Disabling the operation in the first place").
 * The SYSTEM account service re-checks every change when it is applied.
 * Typed secrets live only in a->account_request, painted as a mask by
 * DU_CANVAS fields and wiped after every use. The old Passwords launch name
 * (--panel=passwords) opens this page on the caller's own password task.
 */
enum { UA_LIST=1620, UA_HEAD, UA_LINE0, UA_LINE1, UA_LINE2, UA_TASK_HEAD, UA_TASK0=1630,
       UA_FORM_HEAD=1640, UA_NAME_LABEL, UA_NAME, UA_ADMIN, UA_OLD_LABEL, UA_OLD, UA_NEW_LABEL, UA_NEW, UA_CONFIRM_LABEL, UA_CONFIRM,
       UA_GO, UA_CANCEL, UA_NOTE, UA_TEXT, UA_NOTE2 };
enum { UA_TASKS=7 };
static const unsigned account_actions[UA_TASKS]={CPA_CREATE,CPA_RENAME,CPA_PASSWORD,CPA_PICTURE,CPA_TYPE,CPA_ENABLE,CPA_DELETE};
static int accounts_mode(const struct cp_app *a) { return a->host.accounts_begin!=NULL; }
/* Secret length bounded by the 64-byte request field (plain C17: some host
 * builds of app.c define no POSIX feature level). */
static size_t accounts_secret_len(const char *s) { size_t n=0; while (n<CPA_SECRET-1 && s[n]) n++; return n; }
static void accounts_wipe(struct cp_app *a)
{
    cpa_wipe(a->account_request.old_password,sizeof(a->account_request.old_password));
    cpa_wipe(a->account_request.password,sizeof(a->account_request.password));
    cpa_wipe(a->account_request.confirm,sizeof(a->account_request.confirm));
}
static const struct cpa_record *accounts_target(const struct cp_app *a) { return cpa_find(&a->accounts,a->account_uid); }
static const char *account_type(const struct cpa_record *r)
{ return r->groups==4u || r->uid==CPA_UID_GUEST ? "Guest" : (r->flags&CPA_ADMIN) ? "Computer administrator" : "Standard user"; }
/* The note spans two label lines (labels do not wrap): split at a space. */
static void accounts_note_lines(const struct cp_app *a, char first[96], char second[96])
{
    size_t n=strlen(a->account_note), cut=n;
    if (n>78) { cut=78; while (cut && a->account_note[cut]!=' ') cut--; if (!cut) cut=78; }
    snprintf(first,96,"%.*s",(int)cut,a->account_note);
    snprintf(second,96,"%s",cut<n ? a->account_note+cut+(a->account_note[cut]==' ') : "");
}
static void accounts_note(struct cp_app *a, const char *s)
{
    snprintf(a->account_note,sizeof(a->account_note),"%s",s);
    static char first[96], second[96]; accounts_note_lines(a,first,second);
    text(a,UA_NOTE,first); text(a,UA_NOTE2,second);
}
static void accounts_submit(struct cp_app *a, unsigned action)
{
    if (a->account_busy || !a->host.accounts_begin) return;
    struct cpa_request *q=&a->account_request;
    q->action=action; q->uid=action==CPA_CREATE || action==CPA_READ ? 0 : a->account_uid;
    struct du_widget *w;
    if ((w=du_get(a->ui,UA_NAME))) snprintf(q->name,sizeof(q->name),"%s",w->text);
    if ((w=du_get(a->ui,UA_ADMIN))) q->admin=!!w->value;
    if (action==CPA_CREATE) q->enable=1;
    if (action==CPA_ENABLE) { const struct cpa_record *r=accounts_target(a); q->enable=r && !(r->flags&CPA_ENABLED); }
    if (action==CPA_PICTURE) q->picture[0]=0;   /* the default picture: no installed picture set yet */
    int e=a->host.accounts_begin(a->host.ctx,q);
    accounts_wipe(a);
    a->account_action=action;
    if (e) { char line[96]; snprintf(line,sizeof(line),"The account service is not available (%d).",e); accounts_note(a,line); }
    else { a->account_busy=1; accounts_note(a,action==CPA_READ ? "Reading the accounts..." : "Applying the change..."); }
    du_invalidate(a->ui,(struct du_rect){0,0,a->ui->canvas.width,a->ui->canvas.height});
}
static const char *accounts_result(const struct cp_app *a, int e)
{
    unsigned action=a->account_action;
    switch (e) {
    case 0:
        return action==CPA_CREATE ? "The account was created." : action==CPA_DELETE ? "The account was deleted. Its files were kept." :
               action==CPA_RENAME ? "The name was changed." : action==CPA_TYPE ? "The account type was changed." :
               action==CPA_PASSWORD ? "The password was changed." : action==CPA_PICTURE ? "The picture was changed." :
               action==CPA_ENABLE ? "The account was turned on or off." : "";
    case -EPERM:
        if (!a->accounts.self_admin) return "You must be an administrator to change this setting.";
        return action==CPA_RENAME || action==CPA_TYPE ? "Not allowed: give this account a password first, or keep another enabled administrator with a password." :
               "CiukiOS needs at least one enabled administrator with a password; this change would leave none.";
    case -EBUSY: return "This account is logged on. It can be changed after the user logs off.";
    case -EACCES: return action==CPA_PASSWORD ? "The current password is incorrect." : "You must be an administrator to change this setting.";
    case -EINVAL: return action==CPA_ENABLE ? "Give the account a password before turning it on." :
                         action==CPA_CREATE || action==CPA_RENAME ? "Use 1 to 31 letters, digits, _ or -, not a name already in use or reserved, and a password of 8 to 63 characters typed twice." :
                         "Type a password of 8 to 63 characters, the same in both boxes.";
    case -EAGAIN: return "The accounts changed meanwhile or the service is busy. Review the list and try again.";
    case -ENOSPC: return "No more accounts can be created on this computer.";
    case -ENOENT: return "That account no longer exists.";
    case -ENOTCONN: return "The change did not reach the account service. Try again.";
    default: return "The change could not be confirmed. Review the list before trying again.";
    }
}
static void accounts_build(struct cp_app *a)
{
    const struct cpa_view *v=&a->accounts;
    if (!a->account_loaded && !a->account_busy) accounts_submit(a,CPA_READ);
    /* A Standard user or Guest sees only his own account (Windows XP). */
    a->account_count=0;
    for (unsigned i=0;i<v->count && a->account_count<CPA_MAX;i++) {
        const struct cpa_record *r=&v->records[i];
        if (!v->self_admin && r->uid!=v->self_uid) continue;
        unsigned k=a->account_count++;
        snprintf(a->account_labels[k],sizeof(a->account_labels[k]),"%s (%s%s)",r->name,
                 r->groups==4u || r->uid==CPA_UID_GUEST ? "Guest" : (r->flags&CPA_ADMIN) ? "Admin" : "Standard",
                 (r->flags&CPA_ENABLED) ? "" : ", off");
        a->account_shown[k]=r->uid;
        a->account_items[k]=(struct du_item){.text={a->account_labels[k]},.selected=r->uid==a->account_uid};
    }
    if (v->valid && !accounts_target(a)) a->account_uid=v->self_uid;
    for (unsigned k=0;k<a->account_count;k++) a->account_items[k].selected=a->account_shown[k]==a->account_uid;
    add(a,DU_LABEL,UA_HEAD,24,44,200,16,v->self_admin ? "Pick an account to change:" : "Your account:");
    struct du_widget *w=add(a,DU_LISTVIEW,UA_LIST,24,64,200,220,"");
    list_style(w,"Accounts"); w->items=a->account_items; w->count=a->account_count;
    const struct cpa_record *t=accounts_target(a);
    static char line0[64], line1[64], line2[64];
    snprintf(line0,sizeof(line0),"%s",t ? t->name : !v->valid && a->account_busy ? "Reading the accounts..." : "");
    snprintf(line1,sizeof(line1),"%s",t ? account_type(t) : "");
    snprintf(line2,sizeof(line2),"%s",t ? ((t->flags&CPA_ENABLED) ? "The account is on." : "The account is off.") : "");
    add(a,DU_LABEL,UA_LINE0,236,44,300,16,line0);
    add(a,DU_LABEL,UA_LINE1,236,62,300,16,line1);
    add(a,DU_LABEL,UA_LINE2,236,80,300,16,line2);
    int busy=a->account_busy || !v->valid;
    if (!a->account_task) {
        static const char *const labels[UA_TASKS]={"Create a new account","Change the name","Change the password",
            "Change the picture","Change the account type","Turn off the account","Delete the account"};
        add(a,DU_LABEL,UA_TASK_HEAD,236,104,300,16,"Pick a task:");
        int limited=0;
        for (unsigned k=0;k<UA_TASKS;k++) {
            const char *label=labels[k];
            if (account_actions[k]==CPA_PASSWORD && t && t->uid!=v->self_uid) label="Reset the password";
            if (account_actions[k]==CPA_ENABLE && t && !(t->flags&CPA_ENABLED)) label="Turn on the account";
            int allowed=cpa_allowed(v,account_actions[k],account_actions[k]==CPA_CREATE ? NULL : t);
            if (!allowed && !v->self_admin) limited=1;
            add(a,DU_BUTTON,UA_TASK0+(int)k,236,124+(int)k*30,300,24,label)->disabled=busy || !allowed;
        }
        const char *why=limited ? "You must be an administrator to change this setting." :
            t && t->uid==v->self_uid && v->self_admin ? "You cannot delete, change the type of or turn off the account you are using." :
            t && (t->uid==CPA_UID_RECOVERY || t->uid==CPA_UID_GUEST) ? "The built-in CiukiAdministrator and Guest accounts keep their name and type." : "";
        if (!a->account_note[0] && !a->account_busy) snprintf(a->account_note,sizeof(a->account_note),"%s",why);
    } else {
        unsigned action=account_actions[a->account_task-1];
        static char head[96];
        int y=124;
        if (action==CPA_CREATE) snprintf(head,sizeof(head),"Create a new account");
        else snprintf(head,sizeof(head),"%s: %s",t ? t->name : "",action==CPA_RENAME ? "change the name" : action==CPA_PASSWORD ?
                      (t && t->uid==v->self_uid ? "change the password" : "reset the password") : action==CPA_PICTURE ? "change the picture" :
                      action==CPA_TYPE ? "change the account type" : action==CPA_ENABLE ? (t && (t->flags&CPA_ENABLED) ? "turn off the account" : "turn on the account") :
                      "delete the account");
        add(a,DU_LABEL,UA_FORM_HEAD,236,104,300,16,head);
        const char *go="OK";
        if (action==CPA_CREATE || action==CPA_RENAME) {
            add(a,DU_LABEL,UA_NAME_LABEL,236,y,300,16,action==CPA_CREATE ? "Name (letters, digits, _ or -):" : "New name:");
            w=add(a,DU_EDIT,UA_NAME,236,y+18,300,24,action==CPA_RENAME && t ? t->name : ""); y+=50;
            go=action==CPA_CREATE ? "Create Account" : "Change Name";
        }
        if (action==CPA_CREATE || action==CPA_TYPE) {
            w=add(a,DU_CHECKBOX,UA_ADMIN,236,y,300,20,"Computer administrator");
            w->value=action==CPA_TYPE && t ? !!(t->flags&CPA_ADMIN) : 0; y+=28;
            if (action==CPA_TYPE) { add(a,DU_LABEL,UA_TEXT,236,y,300,32,"Administrators can change every setting; standard users only their own."); go="Change Account Type"; y+=36; }
        }
        if (action==CPA_PASSWORD && t && t->uid==v->self_uid) {
            add(a,DU_LABEL,UA_OLD_LABEL,236,y,300,16,"Current password (empty if you have none):");
            add(a,DU_CANVAS,UA_OLD,236,y+18,300,24,""); y+=48;
        }
        if (action==CPA_CREATE || action==CPA_PASSWORD) {
            add(a,DU_LABEL,UA_NEW_LABEL,236,y,300,16,action==CPA_CREATE ? "Password (8 to 63 characters):" : "New password (8 to 63 characters):");
            add(a,DU_CANVAS,UA_NEW,236,y+18,300,24,""); y+=48;
            add(a,DU_LABEL,UA_CONFIRM_LABEL,236,y,300,16,"Type the password again to confirm:");
            add(a,DU_CANVAS,UA_CONFIRM,236,y+18,300,24,""); y+=48;
            if (action==CPA_PASSWORD) go=t && t->uid==v->self_uid ? "Change Password" : "Reset Password";
        }
        if (action==CPA_PICTURE) {
            add(a,DU_LABEL,UA_TEXT,236,y,300,48,"No account pictures are installed yet. The account shows the approved Ciuki portrait.");
            y+=52; go="Use Default Picture";
        }
        if (action==CPA_ENABLE) {
            add(a,DU_LABEL,UA_TEXT,236,y,300,48,t && (t->flags&CPA_ENABLED) ? "The account will not appear on the Welcome screen and cannot log on." :
                                               "The account will appear on the Welcome screen. It needs a password first.");
            y+=52; go=t && (t->flags&CPA_ENABLED) ? "Turn Off Account" : "Turn On Account";
        }
        if (action==CPA_DELETE) {
            add(a,DU_LABEL,UA_TEXT,236,y,300,48,"The account is removed from this computer. Its documents and settings stay on the disk.");
            y+=52; go="Delete Account";
        }
        add(a,DU_BUTTON,UA_GO,236,y,150,24,go)->disabled=busy;
        add(a,DU_BUTTON,UA_CANCEL,394,y,142,24,"Cancel");
    }
    static char first[96], second[96]; accounts_note_lines(a,first,second);
    add(a,DU_LABEL,UA_NOTE,24,336,512,18,first);
    add(a,DU_LABEL,UA_NOTE2,24,354,512,18,second);
}
static void accounts_paint(struct cp_app *a)
{
    for (int id=UA_OLD;id<=UA_CONFIRM;id++) {
        if (id!=UA_OLD && id!=UA_NEW && id!=UA_CONFIRM) continue;
        struct du_widget *w=du_get(a->ui,id); if (!w) continue;
        const char *value=id==UA_OLD ? a->account_request.old_password : id==UA_NEW ? a->account_request.password : a->account_request.confirm;
        char mask[CPA_SECRET]; size_t n=accounts_secret_len(value); memset(mask,'*',n); mask[n]=0;
        struct du_canvas *c=&a->ui->canvas;
        du_fill(c,w->box,DU_WINDOW); du_edge(c,w->box,DU_SUNKEN);
        if (du_focused(a->ui)==id) du_outline(c,(struct du_rect){w->box.x+2,w->box.y+2,w->box.w-4,w->box.h-4},DU_SELECT);
        du_text_fit(c,w->box.x+6,w->box.y+5,w->box.w-12,mask,DU_INK);
    }
}
static void accounts_open_task(struct cp_app *a, unsigned k)
{
    accounts_wipe(a); a->account_task=(int)k+1; a->account_note[0]=0; build(a);
    int first=du_get(a->ui,UA_NAME) ? UA_NAME : du_get(a->ui,UA_OLD) ? UA_OLD : du_get(a->ui,UA_NEW) ? UA_NEW : UA_GO;
    du_focus(a->ui,first);
}
static int accounts_event(struct cp_app *a, const struct du_event *e)
{
    if (e->type==DU_EV_KEY && (e->id==UA_OLD || e->id==UA_NEW || e->id==UA_CONFIRM)) {
        if (a->account_busy) return 1;
        char *p=e->id==UA_OLD ? a->account_request.old_password : e->id==UA_NEW ? a->account_request.password : a->account_request.confirm;
        size_t n=accounts_secret_len(p);
        if (e->key==DU_KEY_ESC) { accounts_wipe(a); du_invalidate(a->ui,(struct du_rect){0,0,a->ui->canvas.width,a->ui->canvas.height}); return 0; }
        if (e->key==DU_KEY_ENTER) { struct du_event go={.type=DU_EV_ACTIVATE,.id=UA_GO}; return accounts_event(a,&go); }
        if (e->key==DU_KEY_BACKSPACE && n) p[n-1]=0;
        else if (e->ch>=32 && e->ch<=126 && n+1<CPA_SECRET) { p[n]=(char)e->ch; p[n+1]=0; }
        du_invalidate_widget(a->ui,e->id); return 1;
    }
    if (e->id==UA_LIST && e->type==DU_EV_SELECT && e->index>=0 && (unsigned)e->index<a->account_count) {
        if (a->account_shown[e->index]!=a->account_uid) {
            a->account_uid=a->account_shown[e->index]; a->account_task=0; a->account_note[0]=0; accounts_wipe(a); build(a);
        }
        return 1;
    }
    if (e->type!=DU_EV_ACTIVATE) return 0;
    if (e->id>=UA_TASK0 && e->id<UA_TASK0+UA_TASKS) {
        unsigned k=(unsigned)(e->id-UA_TASK0);
        const struct cpa_record *t=accounts_target(a);
        if (!a->account_busy && cpa_allowed(&a->accounts,account_actions[k],account_actions[k]==CPA_CREATE ? NULL : t)) accounts_open_task(a,k);
        return 1;
    }
    if (e->id==UA_CANCEL) { accounts_wipe(a); a->account_task=0; a->account_note[0]=0; build(a); return 1; }
    if (e->id==UA_GO && a->account_task && !a->account_busy) {
        unsigned action=account_actions[a->account_task-1];
        /* Re-checked by the service when applied, not only when the page opened. */
        accounts_submit(a,action);
        return 1;
    }
    return 0;
}
static void accounts_poll(struct cp_app *a)
{
    if (a->building || !a->account_busy || !a->host.accounts_poll) return;
    struct cpa_view v; int e=0;
    if (!a->host.accounts_poll(a->host.ctx,&v,&e)) return;
    a->account_busy=0;
    if (v.valid) { a->accounts=v; a->account_loaded=1; }
    if (e==-EPERM && !a->accounts.valid) e=-EIO;
    if (a->account_action==CPA_READ) {
        /* fx-uaccounts: a finished READ, failed or not, is not re-submitted
         * by the rebuild below (that loop kept "Reading the accounts..." on
         * the page and hid the error); opening the page again retries. */
        a->account_loaded=1;
        a->account_note[0]=0;
        if (e) { char line[96]; snprintf(line,sizeof(line),"The account service is not available (%d). Try again later.",e); accounts_note(a,line); }
        if (!a->account_uid) a->account_uid=a->accounts.self_uid;
        if (a->account_password_page && !e) {
            /* --panel=passwords: the caller's own password task. */
            a->account_password_page=0; a->account_uid=a->accounts.self_uid;
            a->account_task=3;
        }
    } else {
        accounts_note(a,accounts_result(a,e));
        if (!e) {
            /* A changed own password: tell LOGON through the session desktop
             * that Lock and the protected saver can now lock (SESSION_EVENT
             * notice PASSWORD); the account service already holds it. */
            if (a->account_action==CPA_PASSWORD && a->account_uid==a->accounts.self_uid && a->host.apply)
                (void)a->host.apply(a->host.ctx,DESK_SETTINGS_PASSWORD_SET,0,0);
            if (a->account_action==CPA_DELETE) a->account_uid=a->accounts.self_uid;
            a->account_task=0;
        }
    }
    if (user_page(a)) build(a);
    else if (a->page==CP_DISPLAY && a->display_tab==CP_SCREEN_SAVER) build(a);
}
