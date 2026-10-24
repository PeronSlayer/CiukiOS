/* G29b: maintenance UI. All device operations run on the worker. */
enum { D_MAINT=225, D_CONVERT_WARNING, D_MAINT_RESULT };
enum { M_ANALYZE=1, M_DEFRAG, M_CONVERT_QUERY, M_CONVERT };
enum { M_DETAILS=680, M_CHECK, M_REARRANGE, M_PAUSE };
struct files_maintenance {
    struct files_app *app;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t thread;
    int started, done, error, mode, pause, stop, restarts, moved;
    int details, check;
    struct ciuki_media_defrag defrag;
    struct ciuki_media_convert convert;
    int (*defrag_call)(int,struct ciuki_media_defrag *);
    int (*convert_call)(int,struct ciuki_media_convert *);
};
static void maintenance_sheet(struct files_app *);
static void maintenance_stop(struct files_app *a)
{
    struct files_maintenance *w=a->maintenance; if (!w) return;
    pthread_mutex_lock(&w->lock); w->stop=1; w->pause=0; pthread_cond_broadcast(&w->changed); pthread_mutex_unlock(&w->lock);
    if (w->started) pthread_join(w->thread,NULL);
    pthread_cond_destroy(&w->changed); pthread_mutex_destroy(&w->lock); free(w); a->maintenance=NULL;
}
static int maintenance_busy(const struct files_app *a)
{ return a->maintenance && a->maintenance->started; }
static void *maintenance_run(void *ctx)
{
    struct files_maintenance *w=ctx; int error=0;
    struct ciuki_media_defrag r=w->defrag;
    struct ciuki_media_convert c=w->convert;
    if (w->mode==M_ANALYZE) error=w->defrag_call ? w->defrag_call(0,&r) : -ENOSYS;
    else if (w->mode==M_CONVERT_QUERY || w->mode==M_CONVERT)
        error=w->convert_call ? w->convert_call(w->mode==M_CONVERT,&c) : -ENOSYS;
    else {
        error=w->defrag_call ? w->defrag_call(0,&r) : -ENOSYS;
        for (;!error;) {
            pthread_mutex_lock(&w->lock);
            while (w->pause && !w->stop) pthread_cond_wait(&w->changed,&w->lock);
            int stop=w->stop; pthread_mutex_unlock(&w->lock);
            if (stop) break;
            error=w->defrag_call ? w->defrag_call(1,&r) : -ENOSYS;
            if (error==-EAGAIN) {
                error=w->defrag_call(0,&r);
                pthread_mutex_lock(&w->lock); w->restarts++; pthread_mutex_unlock(&w->lock);
            } else if (!error) {
                pthread_mutex_lock(&w->lock); w->moved+=(int)r.moved; pthread_mutex_unlock(&w->lock);
                if (r.moved && !r.finished) {
                    struct ciuki_media_defrag map=r; map.flags=0;
                    error=w->defrag_call(0,&map);
                    if (!error) {
                        if (map.sequence==r.sequence) map.ordinal=r.ordinal;
                        else { pthread_mutex_lock(&w->lock); w->restarts++; pthread_mutex_unlock(&w->lock); }
                        map.flags=r.flags; r=map;
                    }
                }
            }
            pthread_mutex_lock(&w->lock); w->defrag=r; pthread_mutex_unlock(&w->lock);
            du_wake(w->app->ui);
            if (error || r.finished) break;
        }
        if (!error) error=w->defrag_call(0,&r); /* actual remaining fragmentation */
    }
    pthread_mutex_lock(&w->lock); w->defrag=r; w->convert=c; w->error=error; w->done=1; pthread_mutex_unlock(&w->lock);
    du_wake(w->app->ui); return NULL;
}
static int maintenance_launch(struct files_app *a,int mode)
{
    struct files_maintenance *w=a->maintenance;
    if (!w || w->started) return -EBUSY;
    w->mode=mode; w->done=w->error=w->stop=w->pause=0;
    int e=pthread_create(&w->thread,NULL,maintenance_run,w);
    if (e) return -e;
    w->started=1; maintenance_sheet(a); return 0;
}
static void maintenance_paint(struct du_ui *u,void *ctx)
{
    struct files_maintenance *w=((struct files_app *)ctx)->maintenance;
    if (!w || !w->details || w->mode>=M_CONVERT_QUERY) return;
    uint8_t map[CIUKI_DEFRAG_CELLS];
    pthread_mutex_lock(&w->lock); memcpy(map,w->defrag.map,sizeof(map)); pthread_mutex_unlock(&w->lock);
    int x=u->dialog.box.x+24,y=u->dialog.box.y+244;
    for (unsigned i=0;i<CIUKI_DEFRAG_CELLS;i++) {
        uint8_t bits=map[i];
        uint32_t color=bits&CIUKI_DEFRAG_BAD ? 0x202020 : bits&CIUKI_DEFRAG_FRAGMENTED ? 0xcc4422 :
            bits&CIUKI_DEFRAG_DIRECTORY ? 0x207020 : bits&CIUKI_DEFRAG_USED ? 0x2255cc : 0xffffff;
        du_fill(&u->canvas,(struct du_rect){x+(int)(i%32)*14,y+(int)(i/32)*10,12,8},color);
    }
    du_text(&u->canvas,x,y+47,"White Free   Blue Used   Red Fragmented",DU_INK);
    du_text(&u->canvas,x,y+63,"Green Directory   Black Bad   Mixed: highest priority",DU_INK);
}
static void maintenance_status(struct files_maintenance *w,char *text,size_t capacity)
{
    struct ciuki_media_defrag r; int paused,restarts,moved;
    pthread_mutex_lock(&w->lock); r=w->defrag; paused=w->pause; restarts=w->restarts; moved=w->moved; pthread_mutex_unlock(&w->lock);
    if (w->mode==M_ANALYZE && w->started) snprintf(text,capacity,"Analyzing drive contents...");
    else snprintf(text,capacity,"Files: %u    Fragmented: %u (%u%%)\n%s  Files moved: %d%s",
        r.files,r.fragmented,r.files ? (unsigned)((uint64_t)r.fragmented*100/r.files) : 0,
        w->started ? paused ? "Paused after the current file." : "Defragmenting..." : "Ready.",moved,
        restarts ? "\nDrive contents changed... restarting" : "");
}
static void maintenance_changed(struct du_ui *u,const struct du_event *e,void *ctx)
{
    struct files_app *a=ctx; struct files_maintenance *w=a->maintenance;
    if (!w) return;
    if (e->id==M_DETAILS) w->details=u->dialog.items[4].value;
    if (e->id==M_CHECK && !w->started) w->check=u->dialog.items[5].value;
    if (e->type==DU_EV_ACTIVATE && e->id==M_PAUSE && w->started && w->mode==M_DEFRAG) {
        pthread_mutex_lock(&w->lock); w->pause=!w->pause; pthread_cond_broadcast(&w->changed); pthread_mutex_unlock(&w->lock);
        du_set_text(&u->dialog.items[8],w->pause ? "&Resume" : "&Pause");
        maintenance_status(w,u->dialog.text,sizeof(u->dialog.text));
    }
    du_invalidate(u,u->dialog.box);
}
static int maintenance_accept(struct du_ui *u,int button,void *ctx)
{
    struct files_app *a=ctx; struct files_maintenance *w=a->maintenance;
    if (!w) return 0;
    if (w->started) {
        if (button==1 && w->mode==M_DEFRAG) {
            pthread_mutex_lock(&w->lock); w->stop=1; w->pause=0; pthread_cond_broadcast(&w->changed); pthread_mutex_unlock(&w->lock);
        }
        return 1;
    }
    if (button==0 && w->mode==M_ANALYZE && !w->error) {
        w->check=u->dialog.items[5].value;
        w->defrag.flags=w->check ? CIUKI_DEFRAG_CHECK : 0;
        /* Re-analyze with Settings before the first move. */
        w->mode=M_DEFRAG; w->defrag.ordinal=0;
        int e=maintenance_launch(a,M_DEFRAG);
        if (e) error_box(a,"Disk Defragmenter","Cannot start the worker:",e);
        return 1;
    }
    return 0;
}
static void maintenance_sheet(struct files_app *a)
{
    struct files_maintenance *w=a->maintenance;
    static const struct du_item tabs[]={{.text={"Options"}}};
    char title[80]; unsigned drive=w->mode>=M_CONVERT_QUERY ? w->convert.drive : w->defrag.drive;
    snprintf(title,sizeof(title),"%s - %c:",w->mode>=M_CONVERT_QUERY ? "Drive Converter (FAT32)" : "Disk Defragmenter",'A'+drive);
    du_property_sheet(a->ui,D_MAINT,title,520,390,tabs,1);
    struct du_dialog *d=&a->ui->dialog; d->accept=maintenance_accept; d->accept_ctx=a;
    d->changed=maintenance_changed; d->sheet_ctx=a; d->paint=maintenance_paint;
    d->items[2].hidden=1; d->items[2].disabled=1;
    du_set_text(&d->items[0],"&Start"); d->items[0].disabled=w->started || w->error;
    du_set_text(&d->items[1],w->started && w->mode==M_DEFRAG ? "S&top" : "&Close");
    if (w->mode>=M_CONVERT_QUERY) {
        snprintf(d->text,sizeof(d->text),"%s",w->mode==M_CONVERT ? "Converting the drive. Please wait.\nDo not remove or power off the disk." : "Reading fixed drive information...");
        d->items[0].disabled=1; d->items[1].disabled=w->started; return;
    }
    pthread_mutex_lock(&w->lock); int paused=w->pause; pthread_mutex_unlock(&w->lock);
    maintenance_status(w,d->text,sizeof(d->text));
    du_property_control(a->ui,4,DU_CHECKBOX,M_DETAILS,(struct du_rect){24,108,450,24},"Show &Details (drive ranges)")->value=w->details;
    struct du_widget *check=du_property_control(a->ui,5,DU_CHECKBOX,M_CHECK,(struct du_rect){24,136,450,24},"&Check the drive for errors first");
    check->value=w->check; check->disabled=w->started;
    du_property_control(a->ui,6,DU_CHECKBOX,M_REARRANGE,(struct du_rect){24,164,470,24},"Rearrange program files so they start faster")->disabled=1;
    du_property_control(a->ui,7,DU_LABEL,0,(struct du_rect){40,190,450,22},"Startup access profiles are not available.");
    struct du_widget *pause=du_property_control(a->ui,8,DU_BUTTON,M_PAUSE,(struct du_rect){365,73,120,25},paused ? "&Resume" : "&Pause");
    pause->disabled=!w->started || w->mode!=M_DEFRAG;
}
static int maintenance_result(struct files_app *a,const struct du_event *e)
{
    struct files_maintenance *w=a->maintenance;
    if (!w || e->id<D_MAINT || e->id>D_MAINT_RESULT) return 0;
    if (e->id==D_CONVERT_WARNING && e->index==0) {
        int error=maintenance_launch(a,M_CONVERT);
        if (error) error_box(a,"Drive Converter","Cannot start the worker:",error);
    }
    return 1;
}
static void maintenance_service(struct files_app *a)
{
    struct files_maintenance *w=a->maintenance;
    if (!w || !w->started) return;
    pthread_mutex_lock(&w->lock); int done=w->done,error=w->error; pthread_mutex_unlock(&w->lock);
    if (!done) {
        if (w->mode==M_DEFRAG && du_dialog_id(a->ui)==D_MAINT) {
            maintenance_status(w,a->ui->dialog.text,sizeof(a->ui->dialog.text));
            du_invalidate(a->ui,a->ui->dialog.box);
        }
        return;
    }
    pthread_join(w->thread,NULL); w->started=0;
    if (a->close_pending) { if (a->close) a->close(a); return; }
    du_dialog_close(a->ui);
    if (error) error_box(a,w->mode>=M_CONVERT_QUERY ? "Drive Converter" : "Disk Defragmenter",
        "The operation could not be completed. Close files on this drive and check it for errors:",error);
    else if (w->mode==M_ANALYZE) maintenance_sheet(a);
    else if (w->mode==M_CONVERT_QUERY) {
        char text[390]; snprintf(text,sizeof(text),"Convert fixed drive %c: to FAT32 (%u bytes per cluster)?\nBack up your files first. You cannot return to FAT16.\nOlder DOS and dual-boot systems may no longer read this drive.\nContinue?",'A'+w->convert.drive,w->convert.cluster_bytes);
        du_message(a->ui,D_CONVERT_WARNING,"Drive Converter (FAT32)",text,DU_DLG_ICON_WARNING,yes_no,2,1,1);
    } else {
        char text[240];
        if (w->mode==M_CONVERT) snprintf(text,sizeof(text),"Drive %c: successfully converted to FAT32.\nAllocation unit: %u bytes.",'A'+w->convert.drive,w->convert.cluster_bytes);
        else snprintf(text,sizeof(text),"%s\nFiles moved: %d\nFragmented files remaining: %u (%u%%)\nFiles without a large enough free run stay fragmented.",
            w->stop ? "Defragmentation stopped." : "Defragmentation finished.",w->moved,w->defrag.fragmented,
            w->defrag.files ? (unsigned)((uint64_t)w->defrag.fragmented*100/w->defrag.files) : 0);
        du_message(a->ui,D_MAINT_RESULT,w->mode==M_CONVERT ? "Drive Converter" : "Disk Defragmenter",text,DU_DLG_ICON_INFO,NULL,0,0,0);
    }
}
int files_maintenance_open(struct files_app *a,unsigned drive,int convert)
{
    if (drive>=26 || a->reading || a->loading || a->job_active || files_disk_busy(a) || du_dialog_id(a->ui)) return -EBUSY;
    if (a->media.type[drive]==CIUKI_MEDIA_OPTICAL || (convert && (drive==2 ||
        a->media.type[drive]==CIUKI_MEDIA_USB || a->media.type[drive]==CIUKI_MEDIA_FLOPPY))) return -EROFS;
    disk_stop(a);
    struct files_maintenance *w=calloc(1,sizeof(*w)); if (!w) return -ENOMEM;
    int e=pthread_mutex_init(&w->lock,NULL); if (e) { free(w); return -e; }
    e=pthread_cond_init(&w->changed,NULL); if (e) { pthread_mutex_destroy(&w->lock); free(w); return -e; }
    w->app=a; w->check=1; w->defrag_call=a->media_defrag; w->convert_call=a->media_convert;
    w->defrag=(struct ciuki_media_defrag){.version=CIUKI_MEDIA_VERSION,.size=sizeof(w->defrag),.drive=drive,.generation=a->media.generation[drive],.flags=CIUKI_DEFRAG_CHECK};
    w->convert=(struct ciuki_media_convert){.version=CIUKI_MEDIA_VERSION,.size=sizeof(w->convert),.drive=drive,.generation=a->media.generation[drive]};
    a->maintenance=w; return maintenance_launch(a,convert ? M_CONVERT_QUERY : M_ANALYZE);
}
