/* G9 shell drawing/input. Included after button_box. SPDX-License-Identifier: MIT */
static int run_rows(const struct desk_run_ui *r)
{ int n=(int)strlen(r->history.order); return n<8 ? n : 8; }
void desk_run_remember(struct desktop *d,const char *command)
{
    struct desk_shell *s=&d->sh;
    if (!command[0] || strlen(command)>=DESK_RUN_MAX) return;
    /* Keep acceptance order when several launcher jobs complete in one turn.
     * The newest 26 distinct pending commands completely determine the MRU. */
    unsigned at=0;
    while (at<s->run_save_count && !fm_same_path(s->run_saves[at],command)) at++;
    if (at==s->run_save_count && s->run_save_count==DESK_RUN_MRU) at=0;
    if (at<s->run_save_count) {
        memmove(s->run_saves+at,s->run_saves+at+1,(s->run_save_count-at-1)*sizeof(s->run_saves[0]));
        s->run_save_count--;
    }
    strcpy(s->run_saves[s->run_save_count++],command);
}
static void run_resize(struct desktop *d)
{
    int i=internal_find(d,DESK_INTERNAL_RUN); struct desk_run_ui *r=d->sh.run;
    if (i<0 || !r) return;
    struct desk_client *c=&d->clients[i]; damage(d,desk_outer(c));
    unsigned w,h; internal_size(d,DESK_INTERNAL_RUN,&w,&h);
    if (r->browser) { w=(unsigned)r->browser->dialog.box.w-2*DESK_BORDER; h=(unsigned)r->browser->dialog.box.h-DESK_TITLE-DESK_BORDER; }
    else if (r->dropdown) h+=(unsigned)(run_rows(r)*18+4);
    c->info.width=w; c->info.height=h;
    c->frame.w=(int)w+2*DESK_BORDER; c->frame.h=(int)h+DESK_TITLE+DESK_BORDER;
    snprintf(c->title,sizeof(c->title),"%s",r->browser ? "Browse" : "Run");
    damage(d,desk_outer(c));
}
static void run_select(struct desktop *d,int selected)
{
    struct desk_run_ui *r=d->sh.run; if (!r) return;
    int count=(int)strlen(r->history.order); if (!count) return;
    if (selected<0) selected=count-1;
    if (selected>=count) selected=0;
    r->selected=selected;
    if (selected<r->top) r->top=selected;
    if (selected>=r->top+run_rows(r)) r->top=selected-run_rows(r)+1;
    strcpy(d->sh.run_text,r->history.commands[(unsigned)(r->history.order[selected]-'a')]);
    d->sh.run_length=(unsigned)strlen(d->sh.run_text); r->cursor=d->sh.run_length; r->select_all=1;
}
void desk_run_history_ready(struct desktop *d,const struct desk_run_history *h)
{
    if (!d->sh.run) return;
    d->sh.run->history=*h; d->sh.run->selected=-1; d->sh.run->top=0;
    if (!d->sh.run_length && h->order[0] && !d->sh.run->busy) run_select(d,0);
    run_resize(d);
}
static int run_lister(void *ctx,const char *path,struct du_file_entry *out,unsigned cap,unsigned *count)
{
    struct desktop *d=ctx; struct desk_run_ui *r=d->sh.run; char native[FM_PATH];
    int e=fm_posix_path(native,path); if (e) return e;
    if (strlen(native)>=sizeof(r->browse_path)) return -ENAMETOOLONG;
    *count=0;
    if (r->browse_ready && !strcmp(native,r->browse_cached)) {
        if (r->browse_error) return r->browse_error;
        *count=r->entry_count<cap ? r->entry_count : cap;
        if (*count) memcpy(out,r->entries,*count*sizeof(*out));
    } else { strcpy(r->browse_path,native); r->browse_pending=1; }
    return 0;
}
static void run_browse_event(struct du_ui *u,const struct du_event *e,void *ctx)
{
    (void)u; struct desktop *d=ctx; struct desk_run_ui *r=d->sh.run;
    if (e->type!=DU_EV_DIALOG) return;
    r->browser_done=1; r->browser_accept=e->index==0;
    const char *path=e->text;
    /* The common picker's join treats a typed drive path as a filename.
     * Recover that absolute path without changing the shared dialog ABI. */
    if (path) for (const char *p=path;*p;p++) {
        if ((p==path || p[-1]=='/') && ((*p>='A' && *p<='Z') || (*p>='a' && *p<='z')) &&
            p[1]==':' && (p[2]=='/' || p[2]=='\\')) { path=p; break; }
    }
    if (r->browser_accept && path && strlen(path)<sizeof(r->browse_result)) strcpy(r->browse_result,path);
    else {
        if (r->browser_accept) desk_shell_message(d,"Browse","The selected path is too long.",1);
        r->browser_accept=0;
    }
}
static void run_browse_finish(struct desktop *d)
{
    struct desk_run_ui *r=d->sh.run; if (!r || !r->browser_done) return;
    if (r->browser_accept) {
        snprintf(d->sh.run_text,sizeof(d->sh.run_text),"\"%s\"",r->browse_result);
        d->sh.run_length=(unsigned)strlen(d->sh.run_text); r->cursor=d->sh.run_length; r->select_all=1;
    }
    du_dialog_close(r->browser); free(r->browser); free(r->pixels); r->browser=NULL; r->pixels=NULL;
    free(r->entries); r->entries=NULL; r->entry_count=0; r->browse_cached[0]=0;
    r->browser_done=r->browse_pending=r->browse_ready=0; r->focus=0;
    run_resize(d);
}
static void run_browse(struct desktop *d)
{
    struct desk_run_ui *r=d->sh.run; if (!r || r->busy || r->browser) return;
    r->browser=calloc(1,sizeof(*r->browser)); r->pixels=calloc(600*400,4);
    if (!r->browser || !r->pixels) {
        free(r->browser); free(r->pixels); r->browser=NULL; r->pixels=NULL;
        desk_shell_message(d,"Run","There is not enough memory to browse files.",1); return;
    }
    du_init(r->browser,r->pixels,600,400); r->browser->event=run_browse_event; r->browser->user=d;
    r->dropdown=0;
    if (du_file_dialog(r->browser,1,0,"Browse","/Programs","",run_lister,d)) {
        r->browser_done=1; r->browser_accept=0; run_browse_finish(d); return;
    }
    run_resize(d);
}
void desk_run_browse_ready(struct desktop *d,const char *path,const struct du_file_entry *entries,unsigned count,int error)
{
    struct desk_run_ui *r=d->sh.run;
    if (!r || !r->browser || strcmp(r->browse_path,path)) return;
    if (error==1) {
        strcpy(r->browse_result,path); r->browser_done=1; r->browser_accept=1; run_browse_finish(d); return;
    }
    if (count>DU_FILE_ENTRIES) return;
    struct du_file_entry *copy=count ? malloc(count*sizeof(*copy)) : NULL;
    if (count && !copy) { count=0; error=-ENOMEM; }
    if (count) memcpy(copy,entries,count*sizeof(*copy));
    free(r->entries); r->entries=copy; r->entry_count=count; r->browse_ready=1; r->browse_error=error;
    strcpy(r->browse_cached,path);
    if (error) {
        /* Keep the picker and allow a new path or Cancel after an I/O error. */
        desk_shell_message(d,"Browse",strerror(-error),1);
    } else {
        (void)du_file_dialog(r->browser,1,0,"Browse",path,"",run_lister,d);
    }
    run_resize(d);
}
static void run_browser_paint(struct desktop *d,struct desk_box content,struct desk_box clip)
{
    struct du_ui *u=d->sh.run->browser; if (u->dirty) du_render(u);
    struct desk_box b; if (!desk_intersect(content,clip,&b)) return;
    int sx=u->dialog.box.x+DESK_BORDER,sy=u->dialog.box.y+DESK_TITLE;
    for (int y=b.y;y<b.y+b.h;y++) memcpy(d->pixels+(size_t)y*d->width+b.x,
        u->canvas.pixels+(size_t)(sy+y-content.y)*u->canvas.width+sx+b.x-content.x,(size_t)b.w*4);
}
static void run_browser_input(struct desktop *d,const struct ciuki_input_event *e)
{
    struct desk_run_ui *r=d->sh.run;
    r->browser->keys[DU_KEY_LSHIFT]=!!(d->sh.modifiers&DU_SHIFT);
    r->browser->keys[DU_KEY_RSHIFT]=0;
    r->browser->keys[DU_KEY_LCTRL]=!!(d->sh.modifiers&DU_CTRL);
    r->browser->keys[DU_KEY_RCTRL]=0;
    r->browser->keys[DU_KEY_LALT]=!!(d->sh.modifiers&DU_ALT) && d->sh.alt_code!=0x138;
    r->browser->keys[DU_KEY_RALT]=d->sh.alt_code==0x138;
    r->browser->caps=d->text_caps;
    r->browser->input_layout=(unsigned)d->sh.cfg.keyboard;
    struct ciuki_input_event event=*e; event.monotonic_ns=d->now_ns;
    du_input(r->browser,&event); run_browse_finish(d);
    int i=internal_find(d,DESK_INTERNAL_RUN); if (i>=0) damage(d,desk_outer(&d->clients[i]));
}
void desk_run_result(struct desktop *d,const char *command,int error)
{
    int i=internal_find(d,DESK_INTERNAL_RUN);
    if (!error) { if (i>=0 && !strcmp(d->sh.run_text,command)) desk_request_close(d,i); return; }
    if (i<0) i=desk_internal_open(d,DESK_INTERNAL_RUN);
    if (i>=0 && d->sh.run && (!d->sh.run_length || !strcmp(d->sh.run_text,command))) {
        d->sh.run->busy=0; d->sh.run->select_all=1;
        if (command!=d->sh.run_text) snprintf(d->sh.run_text,sizeof(d->sh.run_text),"%s",command);
        d->sh.run_length=(unsigned)strlen(d->sh.run_text); d->sh.run->cursor=d->sh.run_length;
        damage(d,desk_outer(&d->clients[i]));
    }
    char text[240]; snprintf(text,sizeof(text),"'%.120s' could not be started.\n%s (%d). Check the name and try again.",command,strerror(-error),-error);
    desk_shell_message(d,"Run",text,1);
}
