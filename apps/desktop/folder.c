#define _POSIX_C_SOURCE 200809L
/* SPDX-License-Identifier: MIT
 * Explicit Files operations and incremental jobs; idle polls run on a worker. */
#include "desktop.h"
#include "folder.h"
#include "../files/transfer_ui.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
struct folder_worker {
    struct desktop *desktop;
    pthread_mutex_t lock; pthread_cond_t changed; pthread_t thread;
    struct fm_model snapshot, result;
    uint16_t index[DESK_FOLDER_POLL_SLOTS];
    unsigned revision;
    int pending, busy, ready, stopping, error, different;
    int bin_full, migrated; uint32_t fixed;   /* G2: the Recycle Bin icon's state, read beside the folder */
    const char *activity;
};
static int poll_start(struct desk_folder *,struct desktop *);
static void poll_stop(struct desk_folder *);
static void poll_service(struct desktop *);

static void error(struct desktop *d, int r)
{
    char text[180];
    snprintf(text, sizeof(text), "Cannot complete the file operation.\n%s (%d)\n%s", strerror(-r), -r,
        r == -EINVAL ? "A name cannot contain \\ / : * ? \" < > | or end in a space or dot." :
        r == -EEXIST ? "A file or folder with that name already exists." : "The original item is kept when the operation fails.");
    desk_shell_message(d, "Desktop", text, 1);
}
void desk_folder_changed(struct desktop *d)
{
    if (d->sh.folder) d->sh.folder->revision++;
    d->sh.icon_count = 0; d->sh.icon_drag=d->sh.click_icon=-1;
    desk_shell_apply(d);
}
int desk_folder_init(struct desktop *d, const struct fm_vfs *fs)
{
    struct desk_folder *f = calloc(1, sizeof(*f)); if (!f) return -ENOMEM;
    fm_init(&f->model, fs); fm_init(&f->result,fs); f->dialog_window=-1; f->editing = -1; f->index = -1;
    int r = fm_navigate(&f->model, DESK_FOLDER);
    if (!r) r=poll_start(f,d);
    if (r) { fm_destroy(&f->model); free(f); return r; }
    f->ready = 1; d->sh.folder = f; desk_folder_changed(d); return 0;
}
void desk_folder_destroy(struct desktop *d)
{
    if (!d->sh.folder) return;
    poll_stop(d->sh.folder);
    fm_transfer_destroy(d->sh.folder->transfer);
    if (d->sh.folder->dialog) du_dialog_close(d->sh.folder->dialog);
    free(d->sh.folder->dialog); free(d->sh.folder->dialog_pixels);
    fm_destroy(&d->sh.folder->result);
    fm_job_destroy(&d->sh.folder->job); fm_destroy(&d->sh.folder->model);
    free(d->sh.folder); d->sh.folder = NULL;
}
void desk_folder_rename(struct desktop *d, int index)
{
    struct desk_folder *f = d->sh.folder;
    if (!f || f->busy || index < 3 || index>=d->sh.icon_count || (unsigned)(index-3) >= f->model.count) return;
    f->editing = index-3; f->select_all = 1;
    snprintf(f->edit, sizeof(f->edit), "%s", f->model.entries[f->editing].name);
    f->cursor = (unsigned)strlen(f->edit);
    desk_focus(d,-1); desk_damage_add(&d->damage,d->sh.icons[index].box,d->width,d->height);
}
enum { F_PROGRESS=850, F_REPLACE };
static int transfer_prepare(void *arg)
{
    struct desk_folder *f=arg;
    if (f->undo_job) { fm_job_destroy(&f->job); return fm_undo_start(&f->job,&f->model.fs); }
    if (f->clipboard_job) {
        int r=fm_clipboard_load(&f->model); if (r) return r;
        enum fm_job_kind kind=f->clipboard_job==2 ? (f->model.cut ? FM_MOVE : FM_COPY) : f->job.kind;
        char destination[FM_PATH]; snprintf(destination,sizeof(destination),"%s",f->job.destination);
        fm_job_destroy(&f->job);
        if (!f->model.clipboard_count) { f->job.state=FM_DONE; return 0; }
        int e=fm_job_start(&f->job,&f->model.fs,kind,f->model.clipboard,f->model.clipboard_count,destination);
        if (!e) f->job.record_undo=fm_undo_supported(&f->job.fs);
        return e;
    }
    return 0;
}
static void transfer_complete(void *arg)
{
    struct desk_folder *f=arg;
    if (f->job.state==FM_DONE && f->job.kind==FM_MOVE && !f->job.skipped && !f->job.kept && !f->job.kept_folders && f->clipboard_job && f->model.cut)
        fm_clipboard_consume(&f->model);
    snprintf(f->result.path,sizeof(f->result.path),"%s",DESK_FOLDER);
    f->result.sort=f->model.sort; f->result.descending=f->model.descending;
    f->result_error=fm_refresh(&f->result);
    f->bin_full_job=rb_full(&f->result.fs,f->model.fixed);   /* G2: the bin may have changed with this job */
}
static void dialog_event(struct du_ui *ui, const struct du_event *e, void *arg)
{
    (void)ui; struct desktop *d=arg; struct desk_folder *f=d->sh.folder;
    if (e->type!=DU_EV_DIALOG || !f->busy) return;
    if (e->id==F_REPLACE) fm_transfer_command(f->transfer,0,e->index==0 ? FM_YES : e->index==1 ? FM_YES_ALL : e->index==2 ? FM_NO : FM_CANCEL);
    else if (e->id==F_PROGRESS) fm_transfer_command(f->transfer,1,-1);
    f->progress_ns=0;
}
static void dialog_update(struct desktop *d, struct transfer_view *v)
{
    struct desk_folder *f=d->sh.folder; struct du_ui *u=f->dialog;
    if (!u) return;
    if (v->state==FM_REPLACE) { if (du_dialog_id(u)!=F_REPLACE) fm_replace_dialog(u,F_REPLACE,v); }
    else fm_progress_dialog(u,F_PROGRESS,f->display_kind,f->destination,v,f->transfer->started_at);
    int i=f->dialog_window;
    if (i<0 || !d->clients[i].window) i=f->dialog_window=desk_internal_open(d,DESK_INTERNAL_TRANSFER);
    if (i<0) { fm_transfer_command(f->transfer,1,-1); return; }
    struct desk_client *c=&d->clients[i]; struct desk_box old=desk_outer(c);
    unsigned w=(unsigned)u->dialog.box.w-2*DESK_BORDER, h=(unsigned)u->dialog.box.h-DESK_TITLE-DESK_BORDER;
    if (c->info.width!=w || c->info.height!=h) {
        c->info.width=w; c->info.height=h;
        c->frame.w=(int)w+2*DESK_BORDER; c->frame.h=(int)h+DESK_TITLE+DESK_BORDER;
        desk_damage_add(&d->damage,old,d->width,d->height);
    }
    snprintf(c->title,sizeof(c->title),"%.63s",u->dialog.title);
    desk_damage_add(&d->damage,desk_outer(c),d->width,d->height);
}
void desk_folder_dialog_paint(struct desktop *d, struct desk_box content, struct desk_box clip)
{
    struct desk_folder *f=d->sh.folder; if (!f || !f->dialog) return;
    struct du_ui *u=f->dialog; if (u->dirty) du_render(u);
    struct desk_box b; if (!desk_intersect(content,clip,&b)) return;
    int sx=u->dialog.box.x+DESK_BORDER, sy=u->dialog.box.y+DESK_TITLE;
    for (int y=b.y;y<b.y+b.h;y++)
        memcpy(d->pixels+(size_t)y*d->width+b.x,u->canvas.pixels+(size_t)(sy+y-content.y)*u->canvas.width+sx+b.x-content.x,(size_t)b.w*4);
}
void desk_folder_dialog_click(struct desktop *d, int x, int y)
{
    struct desk_folder *f=d->sh.folder; if (!f || !f->dialog || f->dialog_window<0) return;
    struct desk_box c=desk_content(&d->clients[f->dialog_window]); struct du_ui *u=f->dialog;
    struct ciuki_input_event e={.type=CIUKI_INPUT_MOTION,.value=x-c.x+u->dialog.box.x+DESK_BORDER,.value2=y-c.y+u->dialog.box.y+DESK_TITLE};
    du_input(u,&e); e=(struct ciuki_input_event){.type=CIUKI_INPUT_BUTTON,.code=CIUKI_BUTTON_LEFT,.value=1}; du_input(u,&e);
    e.value=0; du_input(u,&e); f->progress_ns=0;
}
void desk_folder_dialog_key(struct desktop *d, int code, int value)
{
    struct desk_folder *f=d->sh.folder; if (!f || !f->dialog) return;
    struct ciuki_input_event e={.type=CIUKI_INPUT_KEY,.code=code,.value=value}; du_input(f->dialog,&e); f->progress_ns=0;
}
void desk_folder_cancel(struct desktop *d)
{ if (d->sh.folder && d->sh.folder->busy) fm_transfer_command(d->sh.folder->transfer,1,-1); }
static int job_begin(struct desktop *d)
{
    struct desk_folder *f=d->sh.folder;
    if (!f->dialog) {
        f->dialog=calloc(1,sizeof(*f->dialog)); f->dialog_pixels=calloc(600*400,4);
        if (!f->dialog || !f->dialog_pixels) { free(f->dialog); free(f->dialog_pixels); f->dialog=NULL; f->dialog_pixels=NULL; return -ENOMEM; }
        du_init(f->dialog,f->dialog_pixels,600,400); f->dialog->event=dialog_event; f->dialog->user=d;
    }
    int created=!f->transfer;
    int r=fm_transfer_create(&f->transfer,&f->job); if (r) return r;
    if (created) { f->transfer->notify=d->wake; f->transfer->notify_ctx=d->wake_ctx; }
    f->transfer->prepare=transfer_prepare; f->transfer->complete=transfer_complete; f->transfer->ctx=f;
    fm_job_destroy(&f->job);
    return 0;
}
static int job_launch(struct desktop *d, const char *to)
{
    struct desk_folder *f=d->sh.folder;
    snprintf(f->destination,sizeof(f->destination),"%s",to ? to : "");
    f->busy=1; f->revision++; f->progress_ns=d->now_ns;
    /* Initialize the visible dialog before handing ownership to the worker. */
    struct transfer_view *view=&f->transfer->ui_view;
    memset(view,0,sizeof(*view)); view->state=FM_RUNNING; view->counting=view->preparing=1;
    snprintf(view->source,sizeof(view->source),"%s",f->job.sources[0]);
    fm_transfer_reset(f->transfer); dialog_update(d,view);
    if (f->dialog_window<0) { f->busy=0; fm_job_destroy(&f->job); return -ENOMEM; }
    fm_transfer_submit(f->transfer); return 0;
}
static int job(struct desktop *d, enum fm_job_kind kind, char **paths, unsigned n, const char *to, int clipboard)
{
    struct desk_folder *f=d->sh.folder;
    int r=job_begin(d); if (r) return r;
    f->undo_job=0; f->clipboard_job=clipboard; f->display_kind=kind; f->bin_op=0;
    char *placeholder[]={"/system/settings/files.clip"};
    r=fm_job_start(&f->job,&f->model.fs,kind,clipboard ? placeholder : paths,clipboard ? 1 : n,to);
    if (r) return r;
    f->job.record_undo=fm_undo_supported(&f->job.fs);
    return job_launch(d,to);
}
/* G2 Recycle Bin jobs: Delete (RB_RECYCLE) and Empty (RB_PURGE) run on the same worker and dialog. */
enum { BIN_DELETE = 1, BIN_EMPTY };
static uint32_t folder_fixed(const struct desktop *d)
{ return d->sh.media.version ? ~d->sh.media.removable_mask & 0x03fffff8u : 0; }
static int bin_job(struct desktop *d, int op, char *const *paths, unsigned n, unsigned flags)
{
    struct desk_folder *f=d->sh.folder;
    int r=job_begin(d); if (r) return r;
    f->undo_job=0; f->clipboard_job=0; f->display_kind=FM_DELETE; f->model.fixed=folder_fixed(d);
    r=op==BIN_DELETE ? rb_delete_job(&f->job,&f->model.fs,paths,n,&f->policy,f->model.fixed,flags) : rb_empty_job(&f->job,&f->model.fs,f->model.fixed);
    if (r) return r;
    f->bin_op=op; f->job.record_undo=fm_undo_supported(&f->job.fs);
    return job_launch(d,"");
}
static void pending_free(struct desk_folder *f)
{
    for (unsigned i=0;f->pending && i<f->pending_count;i++) free(f->pending[i]);
    free(f->pending); f->pending=NULL; f->pending_count=0;
}
/* Asks (unless the policy says not to), then deletes: Windows 98's confirmation wording. */
int desk_folder_delete_request(struct desktop *d, char *const *paths, unsigned count, unsigned flags)
{
    struct desk_folder *f=d->sh.folder;
    if (!f || d->power.state) return -EAGAIN;
    if (f->busy) return -EBUSY;
    if (!count) return -ENOENT;
    pending_free(f);
    f->pending=calloc(count,sizeof(*f->pending)); if (!f->pending) return -ENOMEM;
    for (unsigned i=0;i<count;i++) {
        f->pending[i]=strdup(paths[i]);
        if (!f->pending[i]) { f->pending_count=i; pending_free(f); return -ENOMEM; }
    }
    f->pending_count=count; f->delete_flags=flags;
    if (rb_policy_load(&f->model.fs,&f->policy)) rb_policy_default(&f->policy);
    f->model.fixed=folder_fixed(d);
    if (!f->policy.confirm) { desk_folder_confirmed(d,SH_CONFIRM_DELETE); return 0; }
    struct fm_meta meta; int folder=count==1 && !f->model.fs.stat_path(f->model.fs.ctx,paths[0],&meta) && meta.type==FM_DIR;
    int recycle=rb_classify(&f->policy,f->model.fixed,flags,f->pending,count)!=0;
    char title[64], text[FM_NAME+200];
    rb_delete_prompt(count,folder,fm_basename(paths[0]),recycle,title,sizeof(title),text,sizeof(text));
    desk_shell_confirm(d,title,text,recycle ? DU_DLG_ICON_QUESTION : DU_DLG_ICON_WARNING,SH_CONFIRM_DELETE);
    return 0;
}
void desk_folder_empty_request(struct desktop *d)
{
    struct desk_folder *f=d->sh.folder;
    if (!f || d->power.state || f->busy) return;
    if (rb_policy_load(&f->model.fs,&f->policy)) rb_policy_default(&f->policy);
    f->model.fixed=folder_fixed(d);
    if (!f->bin_full) { desk_shell_message(d,"Recycle Bin","The Recycle Bin is already empty.",0); return; }
    if (!f->policy.confirm) { desk_folder_confirmed(d,SH_CONFIRM_EMPTY); return; }
    char title[64], text[FM_NAME+200];
    rb_delete_prompt(0,0,"",0,title,sizeof(title),text,sizeof(text));
    desk_shell_confirm(d,title,text,DU_DLG_ICON_WARNING,SH_CONFIRM_EMPTY);
}
void desk_folder_confirmed(struct desktop *d, int action)
{
    struct desk_folder *f=d->sh.folder; if (!f) return;
    int r=0;
    if (action==SH_CONFIRM_DELETE && f->pending_count) r=bin_job(d,BIN_DELETE,f->pending,f->pending_count,f->delete_flags);
    else if (action==SH_CONFIRM_EMPTY) { r=bin_job(d,BIN_EMPTY,NULL,0,0); if (!r) f->bin_op=BIN_EMPTY; }
    pending_free(f);
    if (r==-EBUSY) desk_shell_message(d,"Desktop","Another file operation is in progress.\nTry again when it has finished.",1);
    else if (r) error(d,r);
}
void desk_folder_cancelled(struct desktop *d)
{ if (d->sh.folder) pending_free(d->sh.folder); }
void desk_folder_drop(struct desktop *d, int source, int copy)
{
    /* source == -1: desktop selection; otherwise Files has published it. */
    struct desk_folder *f=d->sh.folder; if (!f || f->busy || d->power.state) return;
    int r=0;
    if (source < 0) {
        int index=d->sh.icon_selected-3;
        if (index < 0 || (unsigned)index >= f->model.count) return;
        fm_select(&f->model,(unsigned)index,0);
        r=fm_clipboard(&f->model,!copy); if (!r) r=fm_clipboard_save(&f->model);
    }
    char destination[FM_PATH]; snprintf(destination,sizeof(destination),"%s",DESK_FOLDER);
    int folder_target=0;
    for (int i=3;i<d->sh.icon_count;i++) if ((unsigned)(i-3)<f->model.count &&
        f->model.entries[i-3].meta.type==FM_DIR && desk_contains(d->sh.icons[i].box,d->mouse_x,d->mouse_y) &&
        (source>=0 || i!=d->sh.icon_selected)) {
        if (!fm_entry_path(&f->model,(unsigned)(i-3),destination)) folder_target=1;
        break;
    }
    int target=desk_hit(d,d->mouse_x,d->mouse_y);
    if (!r && target >= 0 && d->clients[target].program == DESK_APP_FILES &&
        desk_contains(desk_content(&d->clients[target]),d->mouse_x,d->mouse_y)) {
        struct ciuki_message msg; struct desk_box b=desk_content(&d->clients[target]);
        desk_message(&msg,DESK_FILE_DROP,16); msg.data[0]=(uint8_t)d->clients[target].version; msg.data[2]=(uint8_t)d->clients[target].minor;
        desk_put32(msg.data+4,copy ? 1u : 0u); desk_put32(msg.data+8,(uint32_t)(d->mouse_x-b.x)); desk_put32(msg.data+12,(uint32_t)(d->mouse_y-b.y));
        desk_queue(d,target,&msg);
    } else if (!r && target < 0 && (source >= 0 || folder_target) && d->mouse_y >= DESK_BAR && d->mouse_y < (int)d->height-DESK_BAR) {
        r=job(d,copy ? FM_COPY : FM_MOVE,NULL,0,destination,1);
    }
    if (r) error(d,r);
}
int desk_folder_drag_save(struct desktop *d, char *const *paths, const char *types, unsigned count)
{ return d->sh.folder ? fm_drag_save(&d->sh.folder->model.fs,paths,types,count) : -ENODEV; }
int desk_folder_drag_load(struct desktop *d, char ***paths, char **types, unsigned *count)
{ *paths=NULL; *types=NULL; *count=0; return d->sh.folder ? fm_drag_load(&d->sh.folder->model.fs,paths,types,count) : -ENODEV; }
int desk_folder_transfer(struct desktop *d, char *const *paths, unsigned count, const char *destination,
                         unsigned effect, int trash, int place, int32_t x, int32_t y)
{
    struct desk_folder *f=d->sh.folder;
    if (!f || d->power.state) return -EAGAIN;
    if (f->busy) return -EBUSY;
    if (!count) return -ENOENT;
    if (trash) return desk_folder_delete_request(d,paths,count,0);   /* the old Trash drop is a Delete (G2) */
    if (effect==FM_EFFECT_LINK) {
        struct fm_model *target=calloc(1,sizeof(*target)); if (!target) return -ENOMEM;
        fm_init(target,&f->model.fs); snprintf(target->path,sizeof(target->path),"%s",destination);
        int e=fm_link_selection(target,paths,count); fm_destroy(target); free(target);
        if (!e) { e=fm_refresh(&f->model); if (!e) desk_folder_changed(d); }
        return e;
    }
    if (effect!=FM_EFFECT_COPY && effect!=FM_EFFECT_MOVE) return -EINVAL;
    f->revision++;
    int r=job(d,effect==FM_EFFECT_MOVE ? FM_MOVE : FM_COPY,(char **)paths,count,destination,0);
    if (!r) { f->place=place; f->place_x=x; f->place_y=y; }
    return r;
}
/* Saved desktop positions follow the items: forgotten when they leave the
 * Desktop folder, placed at the drop point when they arrive. */
static void positions_after(struct desktop *d, struct fm_job *j)
{
    struct desk_folder *f=d->sh.folder; int changed=0;
    /* G2: an item deleted or sent to the bin leaves its Desktop position behind. */
    if (j->kind==FM_DELETE) for (unsigned i=0;i<j->count;i++) {
        char parent[FM_PATH];
        if ((!j->bin || (j->bin->done && j->bin->done[i])) && !fm_parent(parent,j->sources[i]) && fm_same_path(parent,DESK_FOLDER))
            { cfg_icon_forget(&d->sh.cfg,fm_basename(j->sources[i])); changed=1; }
    }
    if (j->kind==FM_MOVE) for (unsigned i=0;i<j->completed && i<j->count;i++) {
        char parent[FM_PATH];
        if (!fm_parent(parent,j->sources[i]) && fm_same_path(parent,DESK_FOLDER) && !fm_same_path(j->destination,DESK_FOLDER))
            { cfg_icon_forget(&d->sh.cfg,fm_basename(j->sources[i])); changed=1; }
    }
    if (f->place && fm_same_path(j->destination,DESK_FOLDER)) {
        int top=DESK_BAR+10, cell_h=94, rows=((int)d->height-DESK_BAR-4-top)/cell_h;
        if (rows<1) rows=1;
        for (unsigned k=0;k<j->created_count;k++) {
            int x=f->place_x-48+(int)(k/(unsigned)rows)*96, y=f->place_y-24+(int)(k%(unsigned)rows)*cell_h;
            if (x<0) x=0;
            if (y<top) y=top;
            changed|=cfg_icon_move(&d->sh.cfg,j->created[k],x,y);
        }
    }
    f->place=0;
    if (changed) d->sh.want_save=1;
}
_Static_assert(FM_ENTRIES<UINT16_MAX,"folder poll index fits its reserved slots");
static unsigned entry_bucket(const char *name)
{
    uint32_t hash=2166136261u;
    for (const unsigned char *p=(const unsigned char *)name;*p;p++) hash=(hash^*p)*16777619u;
    return hash%DESK_FOLDER_POLL_SLOTS;
}
struct folder_check { const struct fm_model *model; const uint16_t *index; unsigned count; int changed; };
static int check_entry(const char *name, const struct fm_meta *meta, void *arg)
{
    struct folder_check *check=arg;
    if (!strcmp(name,".") || !strcmp(name,"..") || fm_hidden(DESK_FOLDER,name)) return 0;
    if (!*name || strlen(name)>=FM_NAME || strchr(name,'/')) return -EIO;
    if (++check->count>FM_ENTRIES) return -E2BIG;
    unsigned slot=entry_bucket(name);
    while (check->index[slot]) {
        const struct fm_entry *entry=&check->model->entries[check->index[slot]-1];
        if (!strcmp(entry->name,name)) {
            if (entry->meta.type!=meta->type || entry->meta.size!=meta->size || entry->meta.modified!=meta->modified) check->changed=1;
            return 0;
        }
        slot=(slot+1)%DESK_FOLDER_POLL_SLOTS;
    }
    check->changed=1;
    return 0;
}
static void folder_activity(struct folder_worker *w,const char *name)
{ __atomic_store_n(&w->activity,name,__ATOMIC_RELEASE); }
static void *folder_poll_worker(void *arg)
{
    struct folder_worker *w=arg;
    pthread_mutex_lock(&w->lock);
    for (;;) {
        folder_activity(w,"mailbox-wait");
        while (!w->pending && !w->stopping) pthread_cond_wait(&w->changed,&w->lock);
        if (w->stopping) break;
        w->pending=0; w->busy=1;
        pthread_mutex_unlock(&w->lock);
        folder_activity(w,"directory-compare");
        memset(w->index,0,sizeof(w->index));
        for (unsigned i=0;i<w->snapshot.count;i++) {
            unsigned slot=entry_bucket(w->snapshot.entries[i].name);
            while (w->index[slot]) slot=(slot+1)%DESK_FOLDER_POLL_SLOTS;
            w->index[slot]=(uint16_t)(i+1);
        }
        struct folder_check check={.model=&w->snapshot,.index=w->index};
        struct fm_vfs *fs=&w->snapshot.fs;
        w->error=fs->list(fs->ctx,DESK_FOLDER,FM_LIST_NAMES_ONLY,check_entry,&check);
        w->different=!w->error && (check.changed || check.count!=w->snapshot.count);
        if (!w->migrated) { (void)rb_migrate(fs); w->migrated=1; }
        w->bin_full=rb_full(fs,w->fixed);
        if (w->different) {
            folder_activity(w,"directory-refresh");
            w->result.sort=w->snapshot.sort; w->result.descending=w->snapshot.descending;
            w->error=fm_refresh(&w->result);
        }
        pthread_mutex_lock(&w->lock);
        w->busy=0; w->ready=1;
        desk_wake(w->desktop);
    }
    pthread_mutex_unlock(&w->lock); return NULL;
}
static int poll_start(struct desk_folder *f,struct desktop *d)
{
    struct folder_worker *w=calloc(1,sizeof(*w)); if (!w) return -ENOMEM;
    w->desktop=d;
    fm_init(&w->snapshot,&f->model.fs); fm_init(&w->result,&f->model.fs);
    snprintf(w->result.path,sizeof(w->result.path),"%s",DESK_FOLDER);
    w->snapshot.capacity=f->model.capacity;
    if (w->snapshot.capacity) w->snapshot.entries=malloc(w->snapshot.capacity*sizeof(*w->snapshot.entries));
    if (w->snapshot.capacity && !w->snapshot.entries) { free(w); return -ENOMEM; }
    folder_activity(w,"starting");
    int e=pthread_mutex_init(&w->lock,NULL);
    if (e) goto fail;
    e=pthread_cond_init(&w->changed,NULL);
    if (e) { pthread_mutex_destroy(&w->lock); goto fail; }
    e=pthread_create(&w->thread,NULL,folder_poll_worker,w);
    if (e) { pthread_cond_destroy(&w->changed); pthread_mutex_destroy(&w->lock); goto fail; }
    f->worker=w; return 0;
fail:
    fm_destroy(&w->snapshot); free(w); return -e;
}
static void poll_stop(struct desk_folder *f)
{
    struct folder_worker *w=f->worker; if (!w) return;
    pthread_mutex_lock(&w->lock); w->stopping=1;
    pthread_cond_signal(&w->changed); pthread_mutex_unlock(&w->lock);
    pthread_join(w->thread,NULL);
    pthread_cond_destroy(&w->changed); pthread_mutex_destroy(&w->lock);
    fm_destroy(&w->snapshot); fm_destroy(&w->result); free(w); f->worker=NULL;
}
int desk_folder_busy(struct desktop *d)
{
    struct desk_folder *f=d->sh.folder; if (!f) return 0;
    struct folder_worker *w=f->worker; if (!w) return f->busy;
    if (d->wake) pthread_mutex_lock(&w->lock);
    else if (pthread_mutex_trylock(&w->lock)) return 1;
    int busy=f->busy || w->pending || w->busy;
    pthread_mutex_unlock(&w->lock); return busy;
}
const char *desk_folder_activity(const struct desktop *d)
{
    struct folder_worker *w=d && d->sh.folder ? d->sh.folder->worker : NULL;
    return w ? __atomic_load_n(&w->activity,__ATOMIC_ACQUIRE) : "unavailable";
}
static void poll_service(struct desktop *d)
{
    struct desk_folder *f=d->sh.folder; struct folder_worker *w=f->worker;
    if (!w) return;
    pthread_mutex_lock(&w->lock);
    struct fm_model *m=&f->model;
    if (w->ready) {
        if (w->bin_full!=f->bin_full && !f->busy) { f->bin_full=w->bin_full; desk_shell_bin_state(d,f->bin_full); }
        if (!w->error && w->different && w->revision==f->revision) {
            struct fm_entry *old=m->entries;
            m->entries=w->result.entries; m->count=w->result.count; m->capacity=w->result.capacity;
            m->active=m->anchor=0; m->find_length=0; m->find[0]=0;
            w->result.entries=NULL; w->result.count=w->result.capacity=0;
            free(old); desk_folder_changed(d);
        }
        /* A rejected result stays worker-owned; its next refresh releases it. */
        w->ready=0;
    }
    if (!w->pending && !w->busy && !d->sh.want_power && d->now_ns-f->poll_ns>=1000000000ull) {
        if (m->count>w->snapshot.capacity) {
            struct fm_entry *p=realloc(w->snapshot.entries,m->capacity*sizeof(*p));
            if (!p) { pthread_mutex_unlock(&w->lock); return; }
            w->snapshot.entries=p; w->snapshot.capacity=m->capacity;
        }
        if (m->count) memcpy(w->snapshot.entries,m->entries,m->count*sizeof(*m->entries));
        w->snapshot.count=m->count; w->snapshot.sort=m->sort; w->snapshot.descending=m->descending;
        w->revision=f->revision; w->fixed=folder_fixed(d); w->pending=1; f->poll_ns=d->now_ns;
        pthread_cond_signal(&w->changed);
    }
    pthread_mutex_unlock(&w->lock);
}
void desk_folder_service(struct desktop *d)
{
    struct desk_folder *f=d->sh.folder; if (!f || d->power.state) return;
    struct fm_model *m=&f->model;
    if (f->busy) {
        if (d->sh.want_power) desk_folder_cancel(d);
        struct transfer_view *v=&f->transfer->ui_view; int finished;
        pthread_mutex_lock(&f->transfer->lock);
        *v=f->transfer->view; finished=f->transfer->finished;
        pthread_mutex_unlock(&f->transfer->lock);
        f->display_kind=v->kind;
        if (finished) {
            f->busy=0;
            if (f->dialog_window>=0) { desk_drop_client(d,f->dialog_window); f->dialog_window=-1; }
            du_dialog_close(f->dialog);
            if (!f->clipboard_job) positions_after(d,&f->job);
            if (f->job.bin && f->job.bin->mode==RB_PURGE && f->job.state==FM_DONE && f->bin_op==BIN_EMPTY) desk_sound(d,SOUND_EMPTY_RECYCLE);
            f->bin_op=0;
            if (f->bin_full_job!=f->bin_full) { f->bin_full=f->bin_full_job; desk_shell_bin_state(d,f->bin_full); }
            if (!f->result_error) {
                free(m->entries); m->entries=f->result.entries; m->count=f->result.count; m->capacity=f->result.capacity;
                m->active=m->anchor=0; f->result.entries=NULL; f->result.count=f->result.capacity=0;
                desk_folder_changed(d);
            }
            char title[64], text[320]; int icon=0;
            if (fm_job_report(&f->job,title,sizeof(title),text,sizeof(text),&icon)) desk_shell_message_class(d,title,text,icon);
            else if (v->state==FM_FAILED) error(d,-f->job.error);
            else if (f->result_error) error(d,f->result_error);
        } else if (!f->progress_ns || d->now_ns-f->progress_ns>=100000000ull || (v->state==FM_REPLACE && du_dialog_id(f->dialog)!=F_REPLACE)) {
            dialog_update(d,v); f->progress_ns=d->now_ns;
        }
    }
    int command=f->command, index=f->index, multi=f->multi; unsigned flags=f->command_flags; f->command=DF_NONE; f->multi=0; f->command_flags=0;
    if (f->busy || !command) {
        if (!f->busy && f->editing<0) poll_service(d);
        return;
    }
    f->revision++; /* an in-flight poll predates this explicit operation */
    int r=0; char name[FM_NAME], path[FM_PATH];
    if (command==DF_FOLDER || command==DF_TEXT || command==DF_BITMAP) {
        r=fm_unique(m,command==DF_FOLDER ? "New Folder" : command==DF_TEXT ? "New Text Document" : "New Bitmap Image",command==DF_FOLDER ? "" : command==DF_TEXT ? ".txt" : ".bmp",name);
        if (!r) r=command==DF_BITMAP ? fm_bitmap(m,name) : fm_create(m,name,command==DF_FOLDER);
        if (!r && command==DF_BITMAP) { r=fm_refresh(m); if (!r) fm_select_name(m,name,0); }
        if (!r) { desk_folder_changed(d); d->sh.icon_selected=(int)m->active+3; desk_folder_rename(d,d->sh.icon_selected); }
    } else if (command==DF_REFRESH) { r=fm_refresh(m); if (!r) desk_folder_changed(d); }
    else if (command==DF_UNDO) {
        r=job_begin(d);
        char *paths[]={FM_UNDO_FILE};
        if (!r) r=fm_job_start(&f->job,&m->fs,FM_MOVE,paths,1,m->path);
        if (!r) { f->undo_job=1; f->clipboard_job=0; f->bin_op=0; f->display_kind=FM_MOVE; r=job_launch(d,m->path); }
    }
    else if (command==DF_PASTE_SHORTCUT) {
        r=fm_clipboard_load(m); if (!r) r=fm_link_selection(m,m->clipboard,m->clipboard_count);
        if (!r) { r=fm_refresh(m); if (!r) desk_folder_changed(d); }
    }
    else if (command==DF_PASTE) r=job(d,FM_COPY,NULL,0,m->path,2);
    else if (index>=0 && (unsigned)index<m->count) {
        if (command==DF_SHORTCUT) {
            unsigned n=0; char **paths=NULL;
            if (!multi || !fm_selected(m,NULL)) fm_select(m,(unsigned)index,0);
            n=fm_selected(m,NULL); paths=calloc(n,sizeof(*paths)); if (!paths) r=-ENOMEM;
            unsigned k=0;
            for (unsigned i=0;!r && i<m->count;i++) if (m->entries[i].selected) {
                paths[k]=malloc(FM_PATH); if (!paths[k]) r=-ENOMEM; else r=fm_entry_path(m,i,paths[k]); k++;
            }
            if (!r) r=fm_link_selection(m,paths,n);
            for (unsigned i=0;i<k;i++) free(paths[i]); free(paths);
            if (!r) { r=fm_refresh(m); if (!r) desk_folder_changed(d); }
        } else if (command==DF_PASTE_INTO && m->entries[index].meta.type==FM_DIR) {
            r=fm_entry_path(m,(unsigned)index,path); if (!r) r=job(d,FM_COPY,NULL,0,path,2);
        } else if (command==DF_RENAME) {
            int x,y,placed=cfg_icon_position(&d->sh.cfg,m->entries[index].name,&x,&y);
            snprintf(name,sizeof(name),"%s",m->entries[index].name);
            r=fm_rename_undo(m,(unsigned)index,f->edit);
            if (!r) { cfg_icon_forget(&d->sh.cfg,name); d->sh.want_save=1; if (placed) { cfg_icon_move(&d->sh.cfg,f->edit,x,y); d->sh.want_save=1; } f->editing=-1; desk_folder_changed(d); d->sh.icon_selected=(int)m->active+3; }
        } else if (command==DF_DELETE && multi && fm_selected(m,NULL)>1) {
            unsigned n=fm_selected(m,NULL),k=0; char **paths=calloc(n,sizeof(*paths));
            if (!paths) r=-ENOMEM;
            for (unsigned i=0;!r && i<m->count;i++) if (m->entries[i].selected) {
                paths[k]=malloc(FM_PATH);
                if (!paths[k]) { r=-ENOMEM; break; }
                r=fm_entry_path(m,i,paths[k++]);
            }
            if (!r) r=desk_folder_delete_request(d,paths,n,flags);
            for (unsigned i=0;paths && i<k;i++) free(paths[i]);
            free(paths);
        } else if (command==DF_DELETE) {
            r=fm_entry_path(m,(unsigned)index,path);
            char *paths[]={path};
            if (!r) r=desk_folder_delete_request(d,paths,1,flags);
        } else if (command==DF_CUT || command==DF_COPY) {
            if (!multi || !fm_selected(m,NULL)) fm_select(m,(unsigned)index,0);
            r=fm_clipboard(m,command==DF_CUT); if (!r) r=fm_clipboard_save(m);
        }
        else if (command==DF_PROPERTIES) {
            char text[360], type[48]; const struct fm_entry *e=&m->entries[index];
            snprintf(text,sizeof(text),"%.180s\n\nType: %s\nLocation: C:\\Documents\\Desktop\nSize: %llu bytes",e->name,fm_type_name(e,type,sizeof(type)),(unsigned long long)e->meta.size);
            desk_shell_message(d,"Properties",text,0);
        }
    }
    if (r) error(d,r);
}
