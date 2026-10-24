/* SPDX-License-Identifier: MIT
 * G29 disk dialogs. Included by app.c; no filesystem I/O in a UI callback. */
#include "cleanup.h"

/* Keep Find (212) and G8 operation dialogs (240..244) fixed. */
enum {
    D_DISK_SCAN=245, D_CLEANUP, D_CLEAN_CONFIRM, D_DISK_PROGRESS,
    D_FORMAT, D_FORMAT_CONFIRM, D_LOW_DISK, D_DISK_SUMMARY
};
enum { DISK_CLEAN_SCAN=1, DISK_FORMAT_QUERY, DISK_CLEAN_RUN, DISK_FORMAT_RUN, DISK_SPACE_QUERY };
struct files_disk {
    pthread_mutex_t lock;
    pthread_t thread;
    struct files_app *app;
    struct fm_vfs fs;
    struct fm_cleanup *cleanup;
    struct fm_cleanup_progress view;
    struct ciuki_media_format format;
    struct ciuki_media_space space;
    int (*format_call)(int,struct ciuki_media_format *);
    int (*space_call)(unsigned,struct ciuki_media_space *);
    unsigned drive, selected;
    int mode, started, done, error, cancel, warning;
};
/* One Files process normally owns one window. A desktop-wide broker for
 * warnings from other programs remains an inventory gap. */
static const struct du_item disk_pages[]={{.text={"Options"}}};
static const char *const disk_categories[]={"Temporary files (older than one week)","Recycle Bin","Temporary Internet files (unavailable)"};

static int disk_cancelled(void *ctx)
{ struct files_disk *w=ctx; return __atomic_load_n(&w->cancel,__ATOMIC_ACQUIRE); }
int files_disk_busy(const struct files_app *a)
{ return a->disk && a->disk->started; }
static void disk_publish(struct files_disk *w)
{
    struct fm_cleanup_progress p; fm_cleanup_progress(w->cleanup,&p);
    pthread_mutex_lock(&w->lock); w->view=p; pthread_mutex_unlock(&w->lock);
    du_wake(w->app->ui);
}
static void *disk_run(void *ctx)
{
    struct files_disk *w=ctx; int error=0;
    if (w->mode==DISK_CLEAN_SCAN)
        error=fm_cleanup_scan_cancelable(&w->fs,(char)('A'+w->drive),(int64_t)time(NULL),disk_cancelled,w,&w->cleanup);
    else if (w->mode==DISK_CLEAN_RUN) {
        error=fm_cleanup_start(w->cleanup,w->selected);
        uint64_t published=0;
        if (!error) for (;;) {
            if (disk_cancelled(w)) fm_cleanup_cancel(w->cleanup);
            int more=fm_cleanup_step(w->cleanup);
            uint64_t now=job_now();
            if (!more || now-published>=100000000ull) { disk_publish(w); published=now; }
            if (!more) break;
        }
    } else if (w->mode==DISK_SPACE_QUERY)
        error=w->space_call ? w->space_call(w->drive,&w->space) : -ENOSYS;
    else error=w->format_call ? w->format_call(w->mode==DISK_FORMAT_RUN,&w->format) : -ENOSYS;
    pthread_mutex_lock(&w->lock); w->error=error; w->done=1; pthread_mutex_unlock(&w->lock);
    du_wake(w->app->ui);
    return NULL;
}
static void disk_wait_dialog(struct files_app *a,int mode)
{
    const char *title=mode==DISK_CLEAN_SCAN ? "Disk Cleanup" : mode==DISK_FORMAT_QUERY ? "Format" :
                      mode==DISK_CLEAN_RUN ? "Disk Cleanup" : "Format";
    const char *text=mode==DISK_CLEAN_SCAN ? "Calculating how much space can be freed..." :
                     mode==DISK_FORMAT_QUERY ? "Reading removable disk information..." :
                     mode==DISK_CLEAN_RUN ? "Removing selected files..." : "Formatting disk. Please wait.";
    du_progress(a->ui,mode==DISK_CLEAN_SCAN || mode==DISK_FORMAT_QUERY ? D_DISK_SCAN : D_DISK_PROGRESS,title,text,0,0);
    if (mode==DISK_FORMAT_RUN) a->ui->dialog.items[0].disabled=1;
}
static int disk_launch(struct files_app *a,int mode)
{
    struct files_disk *w=a->disk;
    if (!w || w->started) return -EBUSY;
    w->mode=mode; w->done=0; w->error=0; memset(&w->view,0,sizeof(w->view));
    __atomic_store_n(&w->cancel,0,__ATOMIC_RELEASE);
    int e=pthread_create(&w->thread,NULL,disk_run,w);
    if (e) { error_box(a,"Disk tools","Cannot start the disk worker:",-e); return -e; }
    w->started=1;
    if (mode!=DISK_SPACE_QUERY) disk_wait_dialog(a,mode);
    return 0;
}
static void disk_stop(struct files_app *a)
{
    struct files_disk *w=a->disk;
    if (!w) return;
    __atomic_store_n(&w->cancel,1,__ATOMIC_RELEASE);
    if (w->started) pthread_join(w->thread,NULL);
    fm_cleanup_destroy(w->cleanup);
    pthread_mutex_destroy(&w->lock); free(w); a->disk=NULL;
}
static int disk_close_request(struct files_app *a)
{
    struct files_disk *w=a->disk;
    if (w->mode==DISK_FORMAT_RUN) {
        if (a->close_declined) a->close_declined(a);
    } else {
        a->close_pending=1;
        __atomic_store_n(&w->cancel,1,__ATOMIC_RELEASE);
    }
    return 0;
}
static int disk_target(const struct files_app *a)
{
    if (!a->model.path[0]) {
        if (fm_selected(&a->model,NULL)!=1 || a->model.active>=a->model.count) return -1;
        char path[FM_PATH];
        if (fm_entry_path(&a->model,a->model.active,path) || !fm_drive_root(path)) return -1;
        return !strcmp(path,"/") ? 2 : media_drive(path);
    }
    return fm_drive_root(a->model.path) ? (!strcmp(a->model.path,"/") ? 2 : media_drive(a->model.path)) : -1;
}
int files_disk_open(struct files_app *a,unsigned drive,int format)
{
    if (drive>=26 || a->loading || a->job_active || files_disk_busy(a) || du_dialog_id(a->ui)) return -EBUSY;
    if (!format && drive<2) return -EOPNOTSUPP;
    if (format && (drive==2 || (a->media.type[drive]!=CIUKI_MEDIA_USB && a->media.type[drive]!=CIUKI_MEDIA_FLOPPY))) return -EINVAL;
    if (a->reading) { a->disk_drive=(int)drive; a->disk_pending=format ? 2 : 1; return 0; }
    disk_stop(a);
    struct files_disk *w=calloc(1,sizeof(*w));
    if (!w) return -ENOMEM;
    int e=pthread_mutex_init(&w->lock,NULL); if (e) { free(w); return -e; }
    a->disk=w; w->app=a; w->drive=drive;
    w->fs=*(a->cleanup_fs ? a->cleanup_fs : &a->model.fs);
    w->format_call=a->media_format; w->space_call=a->media_space;
    w->format=(struct ciuki_media_format){.version=CIUKI_MEDIA_VERSION,.size=sizeof(w->format),
        .drive=drive,.generation=a->media.generation[drive]};
    return disk_launch(a,format ? DISK_FORMAT_QUERY : DISK_CLEAN_SCAN);
}
static void disk_sheet(struct files_app *a,int id,const char *title,const char *button)
{
    du_property_sheet(a->ui,id,title,510,350,disk_pages,1);
    du_set_text(&a->ui->dialog.items[0],button);
    a->ui->dialog.items[2].hidden=1; a->ui->dialog.items[2].disabled=1;
}
static int disk_options_accept(struct du_ui *u,int button,void *ctx)
{
    struct files_app *a=ctx; struct files_disk *w=a->disk;
    if (button!=0) return 0;
    if (du_dialog_id(u)==D_CLEANUP) {
        unsigned mask=0;
        for (unsigned i=0;i<FM_CLEAN_CATEGORIES;i++)
            if (!u->dialog.items[4+i].disabled && u->dialog.items[4+i].value) mask|=FM_CLEAN_MASK(i);
        if (!mask) { snprintf(u->dialog.text,sizeof(u->dialog.text),"Select the file categories to remove."); return 1; }
        w->selected=mask;
    } else {
        unsigned family=0;
        static const unsigned families[]={12,16,32};
        for (unsigned i=0;i<3;i++) if (u->dialog.items[4+i].value && !u->dialog.items[4+i].disabled) family=families[i];
        if (!family) return 1;
        const char *label=u->dialog.items[8].text; size_t n=strlen(label);
        if (n>11) { snprintf(u->dialog.text,sizeof(u->dialog.text),"The label must be at most 11 characters."); return 1; }
        for (unsigned i=0;i<n;i++) if ((unsigned char)label[i]<32 || (unsigned char)label[i]>126 || strchr("*?.,;:/\\|+=<>[]\"",label[i])) {
            snprintf(u->dialog.text,sizeof(u->dialog.text),"The volume label contains an invalid character."); return 1;
        }
        w->format.filesystem=family;
        w->format.flags=u->dialog.items[9].value ? CIUKI_FORMAT_QUICK : 0;
        memcpy(w->format.label,label,n+1);
    }
    return 0;
}
static void disk_cleanup_options(struct files_app *a)
{
    struct files_disk *w=a->disk; char title[64];
    snprintf(title,sizeof(title),"Disk Cleanup for %c:",'A'+w->drive);
    disk_sheet(a,D_CLEANUP,title,"&Clean up");
    struct du_dialog *d=&a->ui->dialog;
    d->accept=disk_options_accept; d->accept_ctx=a;
    snprintf(d->text,sizeof(d->text),"Select files to delete permanently:");
    for (unsigned i=0;i<FM_CLEAN_CATEGORIES;i++) {
        const struct fm_cleanup_category *category=fm_cleanup_category(w->cleanup,i);
        char amount[48],label[240]; files_size_text(amount,sizeof(amount),category->bytes);
        snprintf(label,sizeof(label),"%s: %s",disk_categories[i],amount);
        struct du_widget *b=du_property_control(a->ui,4+i,DU_CHECKBOX,600+(int)i,(struct du_rect){24,76+(int)i*34,450,26},label);
        b->disabled=!category->available || !category->items;
    }
    du_property_control(a->ui,7,DU_LABEL,0,(struct du_rect){24,194,450,42},"Recent temporary files and browser settings are kept.");
    struct fm_cleanup_progress view; fm_cleanup_progress(w->cleanup,&view);
    if (view.errors) {
        char warning[100]; snprintf(warning,sizeof(warning),"%u file(s) could not be inspected; the estimate is incomplete.",view.errors);
        du_property_control(a->ui,8,DU_LABEL,0,(struct du_rect){24,225,450,24},warning);
    }
}
static void disk_format_paint(struct du_ui *u,void *ctx)
{
    (void)ctx;
    int x=u->dialog.items[9].box.x,y=u->dialog.items[9].box.y+38;
    du_text(&u->canvas,x,y,"Copy system files: unavailable.",DU_INK);
    du_text(&u->canvas,x,y+34,"Formatting erases all files on this disk.",DU_INK);
}
static void disk_format_options(struct files_app *a)
{
    struct files_disk *w=a->disk; char title[48],size[48];
    snprintf(title,sizeof(title),"Format %c:",'A'+w->drive);
    disk_sheet(a,D_FORMAT,title,"&Start");
    struct du_dialog *d=&a->ui->dialog; d->accept=disk_options_accept; d->accept_ctx=a;
    d->paint=disk_format_paint;
    files_size_text(size,sizeof(size),(uint64_t)w->format.sectors*512);
    snprintf(d->text,sizeof(d->text),"Capacity: %s    File system:",size);
    static const unsigned families[]={12,16,32};
    static const char *const labels[]={"FAT12","FAT16","FAT32"};
    for (unsigned i=0;i<3;i++) {
        struct du_widget *b=du_property_control(a->ui,4+i,DU_RADIO,620+(int)i,(struct du_rect){24+(int)i*144,73,130,24},labels[i]);
        b->value=w->format.filesystem==families[i]; b->disabled=!(w->format.supported_filesystems & (1u<<i));
    }
    du_property_control(a->ui,7,DU_LABEL,0,(struct du_rect){24,111,105,24},"Volume &label:");
    du_property_control(a->ui,8,DU_EDIT,628,(struct du_rect){132,110,230,24},w->format.label);
    du_property_control(a->ui,9,DU_CHECKBOX,629,(struct du_rect){24,148,400,24},"&Quick format (skip the surface check)")->value=1;
}
static int disk_dialog_result(struct files_app *a,const struct du_event *e)
{
    struct files_disk *w=a->disk;
    if (!w || e->id<D_DISK_SCAN || e->id>D_DISK_SUMMARY) return 0;
    switch (e->id) {
    case D_DISK_SCAN: case D_DISK_PROGRESS:
        if (w->mode!=DISK_FORMAT_RUN) __atomic_store_n(&w->cancel,1,__ATOMIC_RELEASE);
        if (w->started) disk_wait_dialog(a,w->mode);
        break;
    case D_CLEANUP: case D_FORMAT:
        if (e->index==0) {
            char text[240];
            snprintf(text,sizeof(text),e->id==D_FORMAT ? "WARNING: Formatting will erase ALL data on drive %c:.\nContinue?" :
                "Permanently delete the selected files from drive %c:?\nYou cannot undo this operation.",'A'+w->drive);
            du_message(a->ui,e->id==D_FORMAT ? D_FORMAT_CONFIRM : D_CLEAN_CONFIRM,e->id==D_FORMAT ? "Confirm Format" : "Disk Cleanup",text,DU_DLG_ICON_WARNING,yes_no,2,1,1);
        }
        break;
    case D_FORMAT_CONFIRM: case D_CLEAN_CONFIRM:
        if (!e->index) (void)disk_launch(a,e->id==D_FORMAT_CONFIRM ? DISK_FORMAT_RUN : DISK_CLEAN_RUN);
        break;
    case D_LOW_DISK:
        if (!e->index) { a->disk_drive=(int)w->drive; a->disk_pending=1; }
        break;
    default: break;
    }
    return 1;
}
static void disk_allocation_completed(struct files_app *a)
{
    int drive=fm_volume(a->job.destination);
    if (drive>='A' && drive<='Z' && a->media_space) { a->disk_drive=drive-'A'; a->disk_pending=3; }
}
static void disk_service(struct files_app *a)
{
    struct files_disk *w=a->disk;
    if (w && w->started) {
        pthread_mutex_lock(&w->lock); int done=w->done,error=w->error; struct fm_cleanup_progress view=w->view; pthread_mutex_unlock(&w->lock);
        if (!done) {
            if (w->mode==DISK_CLEAN_RUN && du_dialog_id(a->ui)==D_DISK_PROGRESS) {
                char text[200]; snprintf(text,sizeof(text),"Deleted %u file(s), %llu bytes.\nSkipped: %u    Errors: %u",view.items,(unsigned long long)view.bytes,view.skipped,view.errors);
                if (strcmp(a->ui->dialog.text,text)) { snprintf(a->ui->dialog.text,sizeof(a->ui->dialog.text),"%s",text); du_invalidate(a->ui,a->ui->dialog.box); }
            }
            return;
        }
        pthread_join(w->thread,NULL); w->started=0;
        if (a->close_pending) { if (a->close) a->close(a); return; }
        if (w->mode!=DISK_SPACE_QUERY) du_dialog_close(a->ui);
        if (error && w->mode==DISK_SPACE_QUERY) { /* A background warning query is best effort. */ }
        else if (error && error!=-ECANCELED) error_box(a,w->mode==DISK_CLEAN_SCAN || w->mode==DISK_CLEAN_RUN ? "Disk Cleanup" : "Format","The operation could not be completed:",error);
        else if (!error && w->mode==DISK_CLEAN_SCAN) disk_cleanup_options(a);
        else if (!error && w->mode==DISK_FORMAT_QUERY) {
            if (!w->format.supported_filesystems) error_box(a,"Format","No FAT format fits this disk:",-EINVAL);
            else disk_format_options(a);
        }
        else if (!error && w->mode==DISK_SPACE_QUERY) {
            if (!(w->space.flags & CIUKI_MEDIA_SPACE_COUNTING))
                w->warning=fm_cleanup_space_warning(&a->disk_low_seen,(char)('A'+w->drive),(uint64_t)w->space.total_clusters*w->space.cluster_bytes,(uint64_t)w->space.free_clusters*w->space.cluster_bytes);
        } else if (w->mode==DISK_CLEAN_RUN || w->mode==DISK_FORMAT_RUN) {
            char text[256];
            if (w->mode==DISK_CLEAN_RUN) snprintf(text,sizeof(text),"%s\nDeleted: %u file(s), %llu bytes\nSkipped: %u    Errors: %u%s%s",view.state==FM_CLEAN_CANCELLED ? "Disk Cleanup cancelled." : "Disk Cleanup finished.",
                view.items,(unsigned long long)view.bytes,view.skipped,view.errors,view.first_error ? "\nFirst error: " : "",view.first_error ? strerror(-view.first_error) : "");
            else snprintf(text,sizeof(text),"Format complete.\nDrive %c:    FAT%u\nCapacity: %llu bytes\nAllocation unit: %u bytes\nLabel: %.11s",'A'+w->drive,w->format.filesystem,(unsigned long long)w->format.sectors*512,w->format.cluster_bytes,w->format.label);
            du_message(a->ui,D_DISK_SUMMARY,w->mode==DISK_CLEAN_RUN ? "Disk Cleanup" : "Format",text,view.errors ? DU_DLG_ICON_WARNING : DU_DLG_ICON_INFO,NULL,0,0,0);
            if (a->worker) { a->drive_labels[w->drive].valid=0; scan_submit(a,SCAN_REFRESH,NULL,0); }
        }
    }
    if (w && w->warning && !a->loading && !a->job_active && !du_dialog_id(a->ui)) {
        char text[240]; snprintf(text,sizeof(text),"Drive %c: %s\nDisk Cleanup can help free space.\nRun Disk Cleanup now?",'A'+w->drive,w->warning==FM_SPACE_FULL ? "has no free disk space." : "is running low on disk space.");
        w->warning=0; du_message(a->ui,D_LOW_DISK,"Low Disk Space",text,DU_DLG_ICON_WARNING,yes_no,2,0,1);
    }
    if (a->disk_pending && !a->reading && !a->loading && !a->job_active && !du_dialog_id(a->ui) && !files_disk_busy(a)) {
        int pending=a->disk_pending; unsigned drive=(unsigned)a->disk_drive; a->disk_pending=0;
        if (pending!=3) (void)files_disk_open(a,drive,pending==2);
        else {
            disk_stop(a); w=calloc(1,sizeof(*w)); if (!w) return;
            int e=pthread_mutex_init(&w->lock,NULL); if (e) { free(w); return; }
            a->disk=w; w->app=a; w->drive=drive; w->space_call=a->media_space;
            (void)disk_launch(a,DISK_SPACE_QUERY);
        }
    }
}
