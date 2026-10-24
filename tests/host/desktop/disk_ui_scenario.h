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
    assert(du_dialog_id(u)==246); disk_test_press(DU_KEY_ESC);
    assert(!files_disk_open(a,2,0)); disk_test_frame(present,"cleanup scan",ctx); disk_test_settle();
    assert(du_dialog_id(u)==246 && !u->dialog.items[4].disabled && u->dialog.items[6].disabled);
    disk_test_frame(present,"cleanup categories",ctx);
    disk_test_press(DU_KEY_ESC); assert(fake_lookup("/tmp/old.tmp")>=0);
    assert(!files_disk_open(a,2,0)); disk_test_settle();
    u->dialog.items[4].value=1; disk_test_accept(0); assert(du_dialog_id(u)==247 && u->dialog.focus==1);
    disk_test_frame(present,"cleanup confirmation",ctx); disk_test_press(DU_KEY_ENTER);
    assert(!du_dialog_id(u) && fake_lookup("/tmp/old.tmp")>=0);
    assert(!files_disk_open(a,2,0)); disk_test_settle(); u->dialog.items[4].value=1; disk_test_accept(0); disk_test_accept(0);
    disk_test_frame(present,"cleanup progress",ctx); disk_test_settle();
    assert(du_dialog_id(u)==252 && strstr(u->dialog.text,"Deleted: 1") && fake_lookup("/tmp/old.tmp")<0 && fake_lookup("/tmp/recent.tmp")>=0);
    disk_test_frame(present,"cleanup summary",ctx); disk_test_press(DU_KEY_ESC);
    assert(files_disk_open(a,2,1)==-EINVAL && !disk_test_formats);
    assert(!files_disk_open(a,4,1)); disk_test_settle();
    assert(du_dialog_id(u)==249 && u->dialog.items[4].disabled && !u->dialog.items[5].disabled && u->dialog.items[6].disabled);
    disk_test_frame(present,"format choices",ctx);
    du_set_text(&u->dialog.items[8],"BAD:LABEL"); disk_test_accept(0);
    assert(du_dialog_id(u)==249 && strstr(u->dialog.text,"invalid") && !disk_test_formats);
    du_set_text(&u->dialog.items[8],"GAMES"); disk_test_accept(0);
    assert(du_dialog_id(u)==250 && !disk_test_formats); disk_test_frame(present,"format confirmation",ctx);
    disk_test_press(DU_KEY_ENTER); assert(!disk_test_formats && !du_dialog_id(u));
    assert(!files_disk_open(a,4,1)); disk_test_settle(); du_set_text(&u->dialog.items[8],"GAMES");
    u->dialog.items[9].value=0; disk_test_accept(0); disk_test_accept(0);
    disk_test_frame(present,"format progress",ctx); disk_test_settle();
    assert(disk_test_formats==1 && disk_test_flags==0 && du_dialog_id(u)==252 && strstr(u->dialog.text,"Format complete"));
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
    disk_test_settle(); assert(du_dialog_id(u)==251 && strstr(u->dialog.text,"low on disk space"));
    disk_test_frame(present,"low disk warning",ctx); disk_test_accept(0); disk_test_settle(); assert(du_dialog_id(u)==246);
    disk_test_frame(present,"low disk opens cleanup",ctx); disk_test_press(DU_KEY_ESC);
    a->disk_drive=2; a->disk_pending=3; disk_test_settle(); assert(!du_dialog_id(u)); /* once per session */
    disk_test_full=1;
    for (unsigned i=0;i<2;i++) { a->disk_drive=2; a->disk_pending=3; disk_test_settle(); assert(du_dialog_id(u)==251 && strstr(u->dialog.text,"no free disk space")); disk_test_accept(1); }
    disk_test_counting=1; a->disk_pending=3; disk_test_settle(); assert(!du_dialog_id(u));
    assert(fake_lookup("/Documents/keep.txt")>=0 && fake_lookup("/keep.txt")>=0 && disk_test_queries>=3);
    files_destroy(a); du_dialog_close(u); fake_assert_clean(); free(a); free(u); free(pixels);
    disk_test_app=NULL; disk_test_ui=NULL;
}
