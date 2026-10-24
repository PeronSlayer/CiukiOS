/* SPDX-License-Identifier: MIT
 * The production G29 controller, shared with the five-presenter fx-15 row. */
#include "fake_vfs.h"
#include "../../../apps/files/app.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_t disk_test_main;
static unsigned disk_test_queries,disk_test_formats,disk_test_flags;
static int disk_test_error,disk_test_full,disk_test_counting;
static struct files_app *disk_test_app;
static struct du_ui *disk_test_ui;
static int disk_test_format(int write,struct ciuki_media_format *r)
{
    assert(!pthread_equal(pthread_self(),disk_test_main));
    assert(r->drive==4 && r->generation==17);
    if (write) { disk_test_formats++; disk_test_flags=r->flags; assert(r->filesystem==16 && !strcmp(r->label,"GAMES")); }
    else disk_test_queries++;
    if (disk_test_error) return disk_test_error;
    r->filesystem=16; r->sectors=65536; r->cluster_bytes=1024;
    r->supported_filesystems=CIUKI_FORMAT_FAT16;
    if (!write) strcpy(r->label,"STICK");
    return 0;
}
static int disk_test_space(unsigned drive,struct ciuki_media_space *r)
{
    /* Existing Files status text also makes this cheap, cached query. */
    assert(drive==2);
    *r=(struct ciuki_media_space){.cluster_bytes=4096,.total_clusters=32768,.free_clusters=disk_test_full ? 0 : 16,
        .flags=disk_test_counting ? CIUKI_MEDIA_SPACE_COUNTING : 0};
    return 0;
}
static void disk_test_settle(void)
{
    struct files_app *a=disk_test_app;
    for (unsigned k=0;k<10000;k++) {
        a->ui->service(a->ui,a);
        if (!a->reading && !a->loading && !a->job_active && !files_disk_busy(a) && !a->disk_pending) return;
        struct timespec pause={.tv_sec=0,.tv_nsec=1000000}; nanosleep(&pause,NULL);
    }
    assert(!"G29 UI worker timeout");
}
static void disk_test_press(unsigned key)
{
    struct ciuki_input_event e={.type=CIUKI_INPUT_KEY,.code=(int)key,.value=1};
    du_input(disk_test_ui,&e); e.value=0; du_input(disk_test_ui,&e);
}
static void disk_test_accept(unsigned button)
{ disk_test_ui->dialog.focus=(int)button; disk_test_press(DU_KEY_ENTER); }
static void disk_test_frame(void (*present)(struct du_ui *,const char *,void *),const char *label,void *ctx)
{ du_render(disk_test_ui); if (present) present(disk_test_ui,label,ctx); }

static void disk_ui_scenario(void (*present)(struct du_ui *,const char *,void *),void *ctx)
{
    disk_test_main=pthread_self(); disk_test_queries=disk_test_formats=disk_test_flags=0;
    disk_test_error=disk_test_full=disk_test_counting=0;
    fake_image_tree(); fake_create("/mnt/e",FM_DIR);
    int old=fake_file("/tmp/old.tmp",200,1); fake_nodes[old].meta.modified=(int64_t)time(NULL)-8*86400;
    int recent=fake_file("/tmp/recent.tmp",100,2); fake_nodes[recent].meta.modified=(int64_t)time(NULL);
    fake_file("/Documents/keep.txt",80,3);
    disk_test_app=calloc(1,sizeof(*disk_test_app)); disk_test_ui=calloc(1,sizeof(*disk_test_ui));
    uint32_t *pixels=calloc(560*400,4); assert(disk_test_app && disk_test_ui && pixels);
    struct files_app *a=disk_test_app; struct du_ui *u=disk_test_ui;
    du_init(u,pixels,560,400); a->media_format=disk_test_format; a->media_space=disk_test_space;
    a->media.type[4]=CIUKI_MEDIA_USB; a->media.generation[4]=17;
    assert(!files_setup(a,u,&fake_vfs,"/Documents")); disk_test_settle();
    /* A drive root's property sheet is a real keyboard entry point. */
    files_properties_path(a,"/"); disk_test_settle();
    assert(du_dialog_id(u)==203 && u->dialog.items[5].id==312);
    disk_test_frame(present,"drive cleanup button",ctx);
    u->dialog.focus=5; disk_test_press(DU_KEY_SPACE); disk_test_settle();
    assert(du_dialog_id(u)==213); disk_test_press(DU_KEY_ESC);
    assert(!files_disk_open(a,2,0)); disk_test_frame(present,"cleanup scan",ctx); disk_test_settle();
    assert(du_dialog_id(u)==213 && !u->dialog.items[4].disabled && u->dialog.items[6].disabled);
    disk_test_frame(present,"cleanup categories",ctx);
    disk_test_press(DU_KEY_ESC); assert(fake_lookup("/tmp/old.tmp")>=0);
    assert(!files_disk_open(a,2,0)); disk_test_settle();
    u->dialog.items[4].value=1; disk_test_accept(0); assert(du_dialog_id(u)==214 && u->dialog.focus==1);
    disk_test_frame(present,"cleanup confirmation",ctx); disk_test_press(DU_KEY_ENTER);
    assert(!du_dialog_id(u) && fake_lookup("/tmp/old.tmp")>=0);
    assert(!files_disk_open(a,2,0)); disk_test_settle(); u->dialog.items[4].value=1; disk_test_accept(0); disk_test_accept(0);
    disk_test_frame(present,"cleanup progress",ctx); disk_test_settle();
    assert(du_dialog_id(u)==219 && strstr(u->dialog.text,"Deleted: 1") && fake_lookup("/tmp/old.tmp")<0 && fake_lookup("/tmp/recent.tmp")>=0);
    disk_test_frame(present,"cleanup summary",ctx); disk_test_press(DU_KEY_ESC);
    assert(files_disk_open(a,2,1)==-EINVAL && !disk_test_formats);
    assert(!files_disk_open(a,4,1)); disk_test_settle();
    assert(du_dialog_id(u)==216 && u->dialog.items[4].disabled && !u->dialog.items[5].disabled && u->dialog.items[6].disabled);
    disk_test_frame(present,"format choices",ctx);
    du_set_text(&u->dialog.items[8],"BAD:LABEL"); disk_test_accept(0);
    assert(du_dialog_id(u)==216 && strstr(u->dialog.text,"invalid") && !disk_test_formats);
    du_set_text(&u->dialog.items[8],"GAMES"); disk_test_accept(0);
    assert(du_dialog_id(u)==217 && !disk_test_formats); disk_test_frame(present,"format confirmation",ctx);
    disk_test_press(DU_KEY_ENTER); assert(!disk_test_formats && !du_dialog_id(u));
    assert(!files_disk_open(a,4,1)); disk_test_settle(); du_set_text(&u->dialog.items[8],"GAMES");
    u->dialog.items[9].value=0; disk_test_accept(0); disk_test_accept(0);
    disk_test_frame(present,"format progress",ctx); disk_test_settle();
    assert(disk_test_formats==1 && disk_test_flags==0 && du_dialog_id(u)==219 && strstr(u->dialog.text,"Format complete"));
    disk_test_frame(present,"format summary",ctx); disk_test_press(DU_KEY_ESC);
    disk_test_error=-EBUSY;
    assert(!files_disk_open(a,4,1)); disk_test_settle();
    assert(du_dialog_id(u)==202 && strstr(u->dialog.text,"in use"));
    disk_test_frame(present,"format busy error",ctx); disk_test_press(DU_KEY_ESC); disk_test_error=0;
    /* Real Copy/Paste completion, rather than calling the warning helper. */
    disk_test_settle(); assert(!files_navigate(a,"/Documents")); disk_test_settle();
    assert(fm_select_name(&a->model,"keep.txt",0)>=0);
    files_event(u,&(struct du_event){.type=DU_EV_ACTIVATE,.id=5},a); /* Copy */
    assert(!files_navigate(a,"/")); disk_test_settle();
    files_event(u,&(struct du_event){.type=DU_EV_ACTIVATE,.id=6},a); /* Paste */
    disk_test_settle(); assert(du_dialog_id(u)==218 && strstr(u->dialog.text,"low on disk space"));
    disk_test_frame(present,"low disk warning",ctx); disk_test_accept(0); disk_test_settle(); assert(du_dialog_id(u)==213);
    disk_test_frame(present,"low disk opens cleanup",ctx); disk_test_press(DU_KEY_ESC);
    a->disk_drive=2; a->disk_pending=3; disk_test_settle(); assert(!du_dialog_id(u)); /* once per session */
    disk_test_full=1;
    for (unsigned i=0;i<2;i++) { a->disk_drive=2; a->disk_pending=3; disk_test_settle(); assert(du_dialog_id(u)==218 && strstr(u->dialog.text,"no free disk space")); disk_test_accept(1); }
    disk_test_counting=1; a->disk_pending=3; disk_test_settle(); assert(!du_dialog_id(u));
    assert(fake_lookup("/Documents/keep.txt")>=0 && fake_lookup("/keep.txt")>=0 && disk_test_queries>=3);
    files_destroy(a); du_dialog_close(u); fake_assert_clean(); free(a); free(u); free(pixels);
    disk_test_app=NULL; disk_test_ui=NULL;
}

/* G29b controller, also presented by a new fx-15 row. */
static unsigned maint_steps,maint_conversions;
static int maint_entered,maint_release,maint_stale,maint_error,maint_restart_seen;
static int maint_defrag(int step,struct ciuki_media_defrag *r)
{
    assert(!pthread_equal(pthread_self(),disk_test_main));
    assert(r->drive==4 && r->generation==17);
    if (maint_error) return maint_error;
    if (!step) {
        r->files=4; r->fragmented=maint_steps ? 0 : 2; r->clusters=1000; r->free_clusters=500;
        r->sequence=21+maint_steps; r->ordinal=0; r->finished=0;
        for (unsigned i=0;i<CIUKI_DEFRAG_CELLS;i++) r->map[i]=(uint8_t)(1u<<(i%5));
    } else {
        __atomic_store_n(&maint_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&maint_release,__ATOMIC_ACQUIRE)) { struct timespec t={.tv_sec=0,.tv_nsec=1000000}; nanosleep(&t,NULL); }
        if (maint_stale) { maint_stale=0; __atomic_store_n(&maint_release,0,__ATOMIC_RELEASE); __atomic_store_n(&maint_restart_seen,1,__ATOMIC_RELEASE); return -EAGAIN; }
        maint_steps++; r->ordinal++; r->sequence++; r->moved=1; r->finished=maint_steps>=2;
    }
    return 0;
}
static int maint_convert(int run,struct ciuki_media_convert *r)
{
    assert(!pthread_equal(pthread_self(),disk_test_main)); assert((r->drive==4 || r->drive==2) && r->generation==17);
    if (maint_error) return maint_error;
    if (r->drive==2) r->reserved=CIUKI_CONVERT_NEXT_BOOT;
    if (run) maint_conversions++;
    r->cluster_bytes=4096; r->filesystem=run ? 32 : 16; r->sectors=1048576;
    return 0;
}
static void maint_wait_entered(void)
{
    for (unsigned k=0;k<10000 && !__atomic_load_n(&maint_entered,__ATOMIC_ACQUIRE);k++) { struct timespec t={.tv_sec=0,.tv_nsec=1000000}; nanosleep(&t,NULL); }
    assert(__atomic_load_n(&maint_entered,__ATOMIC_ACQUIRE));
}
static void maint_control(unsigned index,unsigned key)
{ disk_test_ui->dialog.focus=(int)index; disk_test_press(key); }
static void maintenance_ui_scenario(void (*present)(struct du_ui *,const char *,void *),void *ctx)
{
    disk_test_main=pthread_self(); maint_steps=maint_conversions=0; maint_stale=maint_error=0;
    __atomic_store_n(&maint_release,0,__ATOMIC_RELEASE); __atomic_store_n(&maint_entered,0,__ATOMIC_RELEASE);
    fake_image_tree();
    disk_test_app=calloc(1,sizeof(*disk_test_app)); disk_test_ui=calloc(1,sizeof(*disk_test_ui));
    uint32_t *pixels=calloc(560*400,4); assert(disk_test_app && disk_test_ui && pixels);
    struct files_app *a=disk_test_app; struct du_ui *u=disk_test_ui;
    du_init(u,pixels,560,400); a->media_defrag=maint_defrag; a->media_convert=maint_convert;
    a->media.generation[4]=17;
    assert(!files_setup(a,u,&fake_vfs,"/Documents")); disk_test_settle();
    assert(!files_maintenance_open(a,4,0)); disk_test_settle(); assert(du_dialog_id(u)==225);
    assert(strstr(u->dialog.text,"50%") && !u->dialog.items[6].disabled);
    maint_control(6,DU_KEY_SPACE); assert(u->dialog.items[6].value);
    disk_test_frame(present,"startup placement enabled",ctx);
    disk_test_frame(present,"defrag analysis and Settings",ctx);
    maint_control(4,DU_KEY_SPACE); assert(u->dialog.items[4].value);
    disk_test_frame(present,"defrag Show Details and map legend",ctx);
    disk_test_accept(0); maint_wait_entered();
    disk_test_frame(present,"defrag running",ctx);
    u->dialog.focus=8; u->service(u,a); assert(u->dialog.focus==8); /* async progress preserves keyboard focus */
    maint_control(8,DU_KEY_ENTER);
    assert(strstr(u->dialog.text,"Paused")); disk_test_frame(present,"defrag paused",ctx);
    maint_control(8,DU_KEY_ENTER); assert(!strstr(u->dialog.text,"Paused"));
    disk_test_frame(present,"defrag resumed",ctx); maint_control(8,DU_KEY_ENTER);
    disk_test_accept(1); /* Stop wakes a paused worker at the safe file boundary. */
    __atomic_store_n(&maint_release,1,__ATOMIC_RELEASE); disk_test_settle();
    assert(du_dialog_id(u)==227 && strstr(u->dialog.text,"stopped")); disk_test_frame(present,"defrag stopped",ctx); disk_test_press(DU_KEY_ESC);
    maint_steps=0; maint_stale=1; __atomic_store_n(&maint_restart_seen,0,__ATOMIC_RELEASE);
    assert(!files_maintenance_open(a,4,0)); disk_test_settle();
    disk_test_accept(0);
    for (unsigned k=0;k<10000;k++) {
        u->service(u,a);
        if (__atomic_load_n(&maint_restart_seen,__ATOMIC_ACQUIRE) && strstr(u->dialog.text,"restarting")) break;
        struct timespec t={.tv_sec=0,.tv_nsec=1000000}; nanosleep(&t,NULL);
    }
    assert(strstr(u->dialog.text,"Drive contents changed... restarting"));
    disk_test_frame(present,"defrag drive contents changed restarting",ctx);
    __atomic_store_n(&maint_release,1,__ATOMIC_RELEASE); disk_test_settle();
    assert(du_dialog_id(u)==227 && strstr(u->dialog.text,"finished") && maint_steps==2);
    disk_test_frame(present,"defrag complete after write restart",ctx); disk_test_press(DU_KEY_ESC);
    maint_error=-EBUSY; assert(!files_maintenance_open(a,4,0)); disk_test_settle();
    assert(du_dialog_id(u)==202); disk_test_frame(present,"defrag busy error",ctx); disk_test_press(DU_KEY_ESC); maint_error=0;
    a->media.type[4]=CIUKI_MEDIA_USB; assert(files_maintenance_open(a,4,1)==-EROFS); a->media.type[4]=0;
    a->media.generation[2]=17;
    assert(!files_maintenance_open(a,2,1)); disk_test_settle();
    assert(du_dialog_id(u)==226 && strstr(u->dialog.text,"next start"));
    disk_test_frame(present,"boot conversion next start warning",ctx); disk_test_accept(1);
    assert(!maint_conversions);
    assert(!files_maintenance_open(a,2,1)); disk_test_settle(); disk_test_accept(0); disk_test_settle();
    assert(du_dialog_id(u)==227 && strstr(u->dialog.text,"scheduled") && !strstr(u->dialog.text,"successfully converted"));
    disk_test_frame(present,"boot conversion scheduled",ctx); disk_test_press(DU_KEY_ESC);
    maint_conversions=0;
    assert(!files_maintenance_open(a,4,1)); disk_test_settle(); assert(du_dialog_id(u)==226);
    assert(strstr(u->dialog.text,"cannot return to FAT16") && strstr(u->dialog.text,"dual-boot"));
    disk_test_frame(present,"converter irreversible and compatibility warning",ctx);
    disk_test_accept(1); assert(!maint_conversions);
    assert(!files_maintenance_open(a,4,1)); disk_test_settle(); disk_test_accept(0); disk_test_settle();
    assert(du_dialog_id(u)==227 && maint_conversions==1 && strstr(u->dialog.text,"successfully converted"));
    disk_test_frame(present,"converter complete",ctx); disk_test_press(DU_KEY_ESC);
    files_destroy(a); du_dialog_close(u); fake_assert_clean(); free(a); free(u); free(pixels);
    disk_test_app=NULL; disk_test_ui=NULL;
}
/* G29c: the same production Copy Disk controller on one/two drives. */
static unsigned copy_reads,copy_writes,copy_drive,copy_generation,copy_sectors;
static int copy_error;
static int copy_load(unsigned drive) { copy_drive=drive; return 0; }
static int copy_state(struct ciuki_media_state *state)
{
    *state=(struct ciuki_media_state){.removable_mask=3};
    state->type[0]=state->type[1]=CIUKI_MEDIA_FLOPPY;
    state->generation[0]=state->generation[1]=copy_generation; return 0;
}
static int copy_query(int write,struct ciuki_media_format *r)
{
    assert(!write && r->drive==copy_drive && r->generation==copy_generation);
    r->sectors=copy_sectors; r->filesystem=12; r->supported_filesystems=CIUKI_FORMAT_FAT12; return 0;
}
static int copy_image(int write,struct ciuki_floppy_image *r,void *buffer)
{
    assert(!pthread_equal(pthread_self(),disk_test_main));
    assert(r->drive==copy_drive && r->generation==copy_generation && r->sectors==2880);
    if(copy_error)return copy_error;
    uint8_t *p=buffer;
    if(write) { assert(p[0]==42 && p[2880*512-1]==99); copy_writes++; }
    else { memset(p,0,2880*512); p[0]=42; p[2880*512-1]=99; copy_reads++; }
    return 0;
}
static void copy_disk_scenario(void (*present)(struct du_ui *,const char *,void *),void *ctx)
{
    fake_image_tree(); disk_test_main=pthread_self(); copy_reads=copy_writes=copy_error=0; copy_generation=17; copy_sectors=2880;
    struct files_app *a=calloc(1,sizeof(*a)); struct du_ui *u=calloc(1,sizeof(*u));
    uint32_t *pixels=calloc(560*400,4); assert(a && u && pixels);
    disk_test_app=a; disk_test_ui=u; du_init(u,pixels,560,400);
    assert(!files_setup(a,u,&fake_vfs,"/Documents")); disk_test_settle();
    a->media.type[0]=a->media.type[1]=CIUKI_MEDIA_FLOPPY;
    a->media_image=copy_image; a->media_load=copy_load; a->media_format=copy_query; a->media_read=copy_state;
    assert(!files_copy_disk_open(a,0)); disk_test_frame(present,"copy source prompt",ctx);
    disk_test_press(DU_KEY_ESC); assert(!copy_reads);
    assert(!files_copy_disk_open(a,0)); disk_test_accept(0); disk_test_frame(present,"copy read progress",ctx); disk_test_settle();
    assert(copy_reads==1 && du_dialog_id(u)==221); disk_test_frame(present,"copy destination choice",ctx);
    disk_test_accept(0); assert(du_dialog_id(u)==222 && strstr(u->dialog.text,"DESTINATION")); disk_test_frame(present,"copy swap confirmation",ctx);
    disk_test_press(DU_KEY_ESC); assert(!copy_writes);
    assert(!files_copy_disk_open(a,0)); disk_test_accept(0); disk_test_settle(); disk_test_accept(0); disk_test_accept(0); disk_test_settle();
    assert(!copy_writes && du_dialog_id(u)==202); disk_test_frame(present,"copy source still inserted",ctx); disk_test_press(DU_KEY_ESC);
    assert(!files_copy_disk_open(a,0)); disk_test_accept(0); disk_test_settle(); disk_test_accept(0);
    copy_generation++; disk_test_accept(0); disk_test_frame(present,"copy write progress",ctx); disk_test_settle();
    assert(copy_writes==1 && du_dialog_id(u)==219 && strstr(u->dialog.text,"verified")); disk_test_frame(present,"copy verified summary",ctx); disk_test_press(DU_KEY_ESC);
    assert(!files_copy_disk_open(a,0)); disk_test_accept(0); disk_test_settle();
    u->dialog.items[4].value=0; u->dialog.items[5].value=1; disk_test_accept(0); disk_test_accept(0); disk_test_settle();
    assert(copy_writes==2 && copy_drive==1); disk_test_frame(present,"copy two drives",ctx); disk_test_press(DU_KEY_ESC);
    copy_error=-EIO; assert(!files_copy_disk_open(a,0)); disk_test_accept(0); disk_test_settle();
    assert(du_dialog_id(u)==202 && copy_writes==2); disk_test_frame(present,"copy source read error",ctx); disk_test_press(DU_KEY_ESC); copy_error=0;
    files_destroy(a); du_dialog_close(u); fake_assert_clean(); free(a); free(u); free(pixels); disk_test_app=NULL; disk_test_ui=NULL;
}
static unsigned backup_ui_calls;
static int backup_ui_mode,backup_ui_error;
static int backup_ui_call(int mode,const char *archive,const char *source,const char *base,const char *root,
                          enum bk_replace policy,const struct bk_callbacks *cb,struct bk_result *result)
{
    assert(!pthread_equal(pthread_self(),disk_test_main));
    assert(!strcmp(archive,"/Documents/Backup.cbk") && !strcmp(source,"/Documents"));
    assert(mode==backup_ui_mode && policy==BK_OLDER);
    if(mode==1)assert(!strcmp(base,"/Documents/Full.cbk"));
    if(mode==3)assert(!strcmp(root,"/Documents/Restored"));
    backup_ui_calls++; *result=(struct bk_result){.files=3,.folders=1,.bytes=456};
    cb->progress(cb->ctx,source,result); return backup_ui_error;
}
static void backup_ui_scenario(void (*present)(struct du_ui *,const char *,void *),void *ctx)
{
    fake_image_tree(); disk_test_main=pthread_self(); backup_ui_calls=backup_ui_error=0;
    struct files_app *a=calloc(1,sizeof(*a)); struct du_ui *u=calloc(1,sizeof(*u)); uint32_t *pixels=calloc(560*400,4); assert(a && u && pixels);
    disk_test_app=a; disk_test_ui=u; du_init(u,pixels,560,400);
    assert(!files_setup(a,u,&fake_vfs,"/Documents")); disk_test_settle(); a->backup_call=backup_ui_call;
    assert(!files_backup_open(a,"/Documents",NULL,0)); disk_test_frame(present,"backup job selection",ctx); disk_test_press(DU_KEY_ESC); assert(!backup_ui_calls);
    for(int mode=0;mode<4;mode++) {
        backup_ui_mode=mode;
        assert(!files_backup_open(a,"/Documents",NULL,mode));
        if(mode==1) { disk_test_accept(0); assert(du_dialog_id(u)==223 && backup_ui_calls==1); u->dialog.items[3].active=1; u->dialog.changed(u,&(struct du_event){.type=DU_EV_SELECT},a); du_set_text(&u->dialog.items[7],"/Documents/Full.cbk"); }
        if(mode==3) { u->dialog.items[3].active=2; u->dialog.changed(u,&(struct du_event){.type=DU_EV_SELECT},a); du_set_text(&u->dialog.items[5],"/Documents/Restored"); }
        disk_test_frame(present,mode==1 ? "backup differential options" : mode==3 ? "backup restore policies" : "backup full or verify options",ctx);
        disk_test_accept(0); assert(du_dialog_id(u)==224 && u->dialog.focus==1); disk_test_frame(present,"backup confirmation",ctx);
        disk_test_accept(0); disk_test_frame(present,"backup worker progress",ctx); disk_test_settle();
        assert(backup_ui_calls==(unsigned)mode+1 && du_dialog_id(u)==219 && strstr(u->dialog.text,"completed")); disk_test_frame(present,"backup job summary",ctx); disk_test_press(DU_KEY_ESC);
    }
    files_properties_path(a,"/"); disk_test_settle();
    u->dialog.items[3].active=1; u->dialog.changed(u,&(struct du_event){.type=DU_EV_SELECT},a);
    assert(!u->dialog.items[6].hidden); disk_test_frame(present,"drive Backup Now",ctx);
    u->dialog.changed(u,&(struct du_event){.type=DU_EV_ACTIVATE,.id=315},a);
    assert(du_dialog_id(u)==223); disk_test_press(DU_KEY_ESC);
    backup_ui_error=-EIO; backup_ui_mode=0;
    assert(!files_backup_open(a,"/Documents",NULL,0)); disk_test_accept(0); disk_test_accept(0); disk_test_settle();
    assert(du_dialog_id(u)==202); disk_test_frame(present,"backup checksum or IO error",ctx); disk_test_press(DU_KEY_ESC);
    files_destroy(a); du_dialog_close(u); fake_assert_clean(); free(a); free(u); free(pixels); disk_test_app=NULL; disk_test_ui=NULL;
}
static int blank_query(int write,struct ciuki_media_format *r)
{
    assert(!write && r->drive==4); r->filesystem=16; r->sectors=65536;
    r->supported_filesystems=CIUKI_FORMAT_FAT16; r->flags=CIUKI_FORMAT_UNFORMATTED; return 0;
}
static void blank_ui_scenario(void (*present)(struct du_ui *,const char *,void *),void *ctx)
{
    fake_image_tree(); disk_test_main=pthread_self();
    struct files_app *a=calloc(1,sizeof(*a)); struct du_ui *u=calloc(1,sizeof(*u)); uint32_t *pixels=calloc(560*400,4); assert(a && u && pixels);
    disk_test_app=a; disk_test_ui=u; du_init(u,pixels,560,400);
    a->media.type[4]=CIUKI_MEDIA_USB; a->media.removable_mask=1u<<4; a->media.generation[4]=17; a->media_format=blank_query;
    assert(!files_setup(a,u,&fake_vfs,FM_COMPUTER)); disk_test_settle();
    assert(fm_select_name(&a->model,"E:",0)>=0); disk_test_frame(present,"unformatted drive listed",ctx);
    assert(!files_navigate(a,"/mnt/e")); disk_test_settle();
    assert(du_dialog_id(u)==216 && strstr(u->dialog.title,"Format")); disk_test_frame(present,"unformatted open offers format",ctx); disk_test_press(DU_KEY_ESC);
    files_destroy(a); du_dialog_close(u); fake_assert_clean(); free(a); free(u); free(pixels); disk_test_app=NULL; disk_test_ui=NULL;
}
