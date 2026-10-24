/* SPDX-License-Identifier: MIT
 * Passwords stay in the controller; DU_CANVAS displays only a mask. */
enum { U_LIST=1600,U_NAME,U_OLD,U_NEW,U_CONFIRM,U_CREATE,U_DELETE,U_CHANGE,U_LOGON,U_PROFILES,U_LOCK,U_APPLY,U_NOTE,U_DELETE_CONFIRM,U_REFRESH };
static int user_page(const struct cp_app *a) { return a->page==CP_USERS || a->page==CP_PASSWORDS; }
static void user_wipe(struct cp_app *a)
{
    volatile char *p=(volatile char *)&a->user_request;
    for (size_t i=0;i<sizeof(a->user_request);i++) p[i]=0;
}
static void user_submit(struct cp_app *a,unsigned op)
{
    if (a->user_busy) return;
    struct users_request *q=&a->user_request;
    struct du_widget *w=du_get(a->ui,U_NAME);
    if (w) snprintf(q->name,sizeof(q->name),"%s",w->text);
    const char *current=getenv("USER");
    snprintf(q->current,sizeof(q->current),"%s",current ? current : "");
    q->operation=op;
    if (op==USERS_POLICY) {
        q->flags=0;
        if ((w=du_get(a->ui,U_LOGON)) && w->value) q->flags|=USERS_LOGON;
        if ((w=du_get(a->ui,U_PROFILES)) && w->value) q->flags|=USERS_PROFILES;
        if ((w=du_get(a->ui,U_LOCK)) && w->value) q->flags|=USERS_LOCK;
    }
    int e=a->host.user_begin ? a->host.user_begin(a->host.ctx,q) : -ENOSYS;
    user_wipe(a);
    if (e) {
        snprintf(a->user_note,sizeof(a->user_note),"User service unavailable (%d).",e);
        text(a,U_NOTE,a->user_note);
    } else { a->user_busy=1; a->user_operation=op; text(a,U_NOTE,"Working..."); }
    du_invalidate(a->ui,(struct du_rect){0,0,a->ui->canvas.width,a->ui->canvas.height});
}
static void user_poll(struct cp_app *a)
{
    if (a->building || !a->user_busy || !a->host.user_poll) return;
    struct users_view v; int e;
    if (!a->host.user_poll(a->host.ctx,&v,&e)) return;
    a->user_busy=0; a->user_loaded=1;
    if (!e) { a->users=v; a->user_loaded=1; }
    snprintf(a->user_note,sizeof(a->user_note),"%s",e==-EACCES ? "The old password is incorrect, or this is not your account." :
        e==-EINVAL ? "Check the name and password confirmation." : e==-EBUSY ? "The account is in use or another change is running." :
        e==-EEXIST ? "That user already exists." : e ? "The change could not be confirmed. Refresh the user list before retrying." :
        a->user_operation==USERS_CHANGE ? "Your Windows password was changed." :
        a->user_operation==USERS_CREATE ? "The user was created." :
        a->user_operation==USERS_DELETE ? "The user was removed. Profile files were kept." :
        a->user_operation==USERS_POLICY ? "Options saved. Profile choices apply at the next logon." : "User settings loaded.");
    if (!e && a->user_operation!=USERS_READ && a->host.apply) a->host.apply(a->host.ctx,1,0,0);
    if (user_page(a)) build(a);
}
static void user_build(struct cp_app *a)
{
    int passwords=a->page==CP_PASSWORDS;
    if (!passwords) {
        for (unsigned i=0;i<a->users.count;i++) a->user_items[i]=(struct du_item){.text={a->users.names[i]}};
        struct du_widget *w=add(a,DU_LISTVIEW,U_LIST,24,44,180,152,"");
        w->items=a->user_items; w->count=a->users.count; w->inactive_selection=1;
        add(a,DU_LABEL,1615,218,44,290,18,"User name (letters, digits, _ or -):");
        add(a,DU_EDIT,U_NAME,218,66,290,24,"");
    } else {
        const char *current=getenv("USER");
        add(a,DU_LABEL,1615,24,44,480,20,current && *current ? current : "Default profile: log on to a named user to change its password.");
        struct du_widget *w=add(a,DU_EDIT,U_NAME,24,68,180,24,current ? current : ""); w->disabled=1;
    }
    int x=218;
    if (passwords) { add(a,DU_LABEL,1616,24,104,180,18,"Old password:"); add(a,DU_CANVAS,U_OLD,24,124,180,24,""); }
    add(a,DU_LABEL,1617,x,104,260,18,"New password:"); add(a,DU_CANVAS,U_NEW,x,124,290,24,"");
    add(a,DU_LABEL,1618,x,154,260,18,"Confirm new password:"); add(a,DU_CANVAS,U_CONFIRM,x,174,290,24,"");
    if (!passwords) {
        add(a,DU_BUTTON,U_CREATE,218,208,140,24,"New user");
        add(a,DU_BUTTON,U_DELETE,366,208,142,24,"Delete user...");
    } else add(a,DU_BUTTON,U_CHANGE,218,208,290,24,"Change Windows password");
    add(a,DU_BUTTON,U_REFRESH,24,208,180,24,"Refresh");
    add(a,DU_CHECKBOX,U_LOGON,24,244,480,20,"Show logon at startup")->value=!!(a->users.flags&USERS_LOGON);
    add(a,DU_CHECKBOX,U_PROFILES,24,268,480,20,"Separate Desktop, Start Menu, Documents and settings")->value=!!(a->users.flags&USERS_PROFILES);
    add(a,DU_CHECKBOX,U_LOCK,24,292,480,20,"Password protection for screen saver / Lock")->value=!!(a->users.flags&USERS_LOCK);
    add(a,DU_BUTTON,U_APPLY,24,320,140,24,"Apply options");
    add(a,DU_LABEL,1619,176,320,340,28,"Remote administration: no provider installed.");
    add(a,DU_LABEL,U_NOTE,24,352,500,38,a->user_note);
    if (!a->user_loaded && !a->user_busy) user_submit(a,USERS_READ);
}
static void user_paint(struct cp_app *a)
{
    if (!user_page(a)) return;
    for (int id=U_OLD;id<=U_CONFIRM;id++) {
        struct du_widget *w=du_get(a->ui,id); if (!w) continue;
        const char *value=id==U_OLD ? a->user_request.old_password : id==U_NEW ? a->user_request.password : a->user_request.confirm;
        char mask[USERS_PASSWORD]; size_t n=strlen(value); memset(mask,'*',n); mask[n]=0;
        struct du_canvas *c=&a->ui->canvas;
        du_fill(c,w->box,DU_WINDOW); du_edge(c,w->box,DU_SUNKEN);
        if (du_focused(a->ui)==id) du_outline(c,(struct du_rect){w->box.x+2,w->box.y+2,w->box.w-4,w->box.h-4},DU_SELECT);
        du_text_fit(c,w->box.x+6,w->box.y+5,w->box.w-12,mask,DU_INK);
    }
}
static int user_event(struct cp_app *a,const struct du_event *e)
{
    if (!user_page(a)) return 0;
    if (e->type==DU_EV_KEY && e->id>=U_OLD && e->id<=U_CONFIRM) {
        if (a->user_busy) return 1;
        char *p=e->id==U_OLD ? a->user_request.old_password : e->id==U_NEW ? a->user_request.password : a->user_request.confirm;
        size_t n=strlen(p);
        if (e->key==DU_KEY_ESC) { user_wipe(a); return 0; }
        if (e->key==DU_KEY_BACKSPACE && n) p[n-1]=0;
        else if (e->ch>=32 && e->ch<=126 && n+1<USERS_PASSWORD) { p[n]=(char)e->ch; p[n+1]=0; }
        du_invalidate_widget(a->ui,e->id); return 1;
    }
    if (e->id==U_LIST && e->type==DU_EV_SELECT && e->index>=0 && (unsigned)e->index<a->users.count) {
        du_set_text(du_get(a->ui,U_NAME),a->users.names[e->index]);
        for (unsigned i=0;i<a->users.count;i++) a->user_items[i].selected=i==(unsigned)e->index;
        du_invalidate_widget(a->ui,U_NAME); du_invalidate_widget(a->ui,U_LIST); return 1;
    }
    if (e->id==U_DELETE_CONFIRM && e->type==DU_EV_DIALOG) {
        if (!e->index) user_submit(a,USERS_DELETE);
        return 1;
    }
    if (e->type!=DU_EV_ACTIVATE || a->user_busy) return 0;
    if (e->id==U_CREATE) user_submit(a,USERS_CREATE);
    else if (e->id==U_CHANGE) user_submit(a,USERS_CHANGE);
    else if (e->id==U_APPLY) user_submit(a,USERS_POLICY);
    else if (e->id==U_REFRESH) user_submit(a,USERS_READ);
    else if (e->id==U_DELETE) {
        static const char *const buttons[]={"Delete","Cancel"};
        du_message(a->ui,U_DELETE_CONFIRM,"Delete user","Remove this user from the logon list?\nThe profile's files will be kept.",DU_DLG_ICON_WARNING,buttons,2,1,1);
    } else return 0;
    return 1;
}
