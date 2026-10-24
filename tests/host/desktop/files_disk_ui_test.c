/* SPDX-License-Identifier: MIT */
#include "disk_ui_scenario.h"
static int entered,released,closed,declined;
static int slow_list(void *ctx,const char *path,unsigned flags,fm_emit emit,void *arg)
{
    (void)ctx;
    if (!strcmp(path,"/tmp")) {
        __atomic_store_n(&entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&released,__ATOMIC_ACQUIRE)) { struct timespec t={.tv_sec=0,.tv_nsec=1000000}; nanosleep(&t,NULL); }
    }
    return fake_vfs.list(fake_vfs.ctx,path,flags,emit,arg);
}
static int slow_format(int write,struct ciuki_media_format *r)
{
    if (write) {
        __atomic_store_n(&entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&released,__ATOMIC_ACQUIRE)) { struct timespec t={.tv_sec=0,.tv_nsec=1000000}; nanosleep(&t,NULL); }
    }
    return disk_test_format(write,r);
}
static void did_close(struct files_app *a) { (void)a; closed++; }
static void did_decline(struct files_app *a) { (void)a; declined++; }
static int delayed_media(struct ciuki_media_state *state)
{
    (void)state;
    __atomic_store_n(&entered,1,__ATOMIC_RELEASE);
    while (!__atomic_load_n(&released,__ATOMIC_ACQUIRE)) { struct timespec t={.tv_sec=0,.tv_nsec=1000000}; nanosleep(&t,NULL); }
    return 0;
}
static void wait_entered(void)
{
    for (unsigned k=0;k<10000 && !__atomic_load_n(&entered,__ATOMIC_ACQUIRE);k++) {
        struct timespec t={.tv_sec=0,.tv_nsec=1000000}; nanosleep(&t,NULL);
    }
    assert(__atomic_load_n(&entered,__ATOMIC_ACQUIRE));
}
static void lifetimes(void)
{
    fake_image_tree(); fake_create("/mnt/e",FM_DIR);
    fake_file("/mnt/e/ordinary.txt",1,9);
    int old=fake_file("/tmp/lifetime.tmp",100,7); fake_nodes[old].meta.modified=(int64_t)time(NULL)-8*86400;
    disk_test_app=calloc(1,sizeof(*disk_test_app)); disk_test_ui=calloc(1,sizeof(*disk_test_ui));
    uint32_t *pixels=calloc(560*400,4); assert(disk_test_app && disk_test_ui && pixels);
    struct files_app *a=disk_test_app; struct du_ui *u=disk_test_ui;
    du_init(u,pixels,560,400); a->close=did_close; a->close_declined=did_decline;
    a->media_format=slow_format; a->media.type[4]=CIUKI_MEDIA_USB; a->media.generation[4]=17;
    struct fm_vfs slow=fake_vfs; slow.list=slow_list; a->cleanup_fs=&slow;
    assert(!files_setup(a,u,&fake_vfs,"/Documents")); disk_test_settle();
    entered=released=0; a->media_read=delayed_media;
    files_event(u,&(struct du_event){.type=DU_EV_TICK,.id=2},a); wait_entered();
    assert(a->reading && !a->loading);
    assert(!files_disk_open(a,2,0) && !files_disk_busy(a) && a->disk_pending);
    __atomic_store_n(&released,1,__ATOMIC_RELEASE); disk_test_settle();
    assert(du_dialog_id(u)==246); disk_test_press(DU_KEY_ESC); a->media_read=NULL;
    entered=released=0;
    assert(!files_disk_open(a,2,0)); wait_entered();
    disk_test_press(DU_KEY_ESC); assert(files_disk_busy(a));
    __atomic_store_n(&released,1,__ATOMIC_RELEASE); disk_test_settle();
    assert(!du_dialog_id(u) && fake_lookup("/tmp/lifetime.tmp")>=0);
    entered=released=0;
    assert(!files_disk_open(a,2,0)); wait_entered();
    assert(!files_close_request(a) && !closed);
    __atomic_store_n(&released,1,__ATOMIC_RELEASE); disk_test_settle();
    assert(closed==1 && fake_lookup("/tmp/lifetime.tmp")>=0); a->close_pending=0; du_dialog_close(u);
    assert(!files_disk_open(a,4,1)); disk_test_settle(); du_set_text(&u->dialog.items[8],"GAMES");
    entered=released=0; disk_test_accept(0); disk_test_accept(0); wait_entered();
    assert(!files_close_request(a) && declined==1 && !a->close_pending);
    disk_test_press(DU_KEY_ESC); assert(files_disk_busy(a) && du_dialog_id(u)==248);
    __atomic_store_n(&released,1,__ATOMIC_RELEASE); disk_test_settle();
    assert(du_dialog_id(u)==252 && closed==1);
    disk_test_press(DU_KEY_ESC);
    assert(!files_navigate(a,"/mnt/e")); disk_test_settle();
    assert(fm_select_name(&a->model,"ordinary.txt",0)>=0);
    files_event(u,&(struct du_event){.type=DU_EV_CONTEXT,.id=107,.index=(int)a->model.active,.x=200,.y=160},a);
    for (unsigned i=0;i<a->popup_count;i++) assert(a->popup[i].id!=313 && a->popup[i].id!=312);
    disk_test_press(DU_KEY_ESC);
    files_destroy(a); du_dialog_close(u); fake_assert_clean(); free(pixels); free(a); free(u);
    disk_test_app=NULL; disk_test_ui=NULL;
}
static void review_frame(struct du_ui *u,const char *label,void *ctx)
{
    char name[100],path[1200]; snprintf(name,sizeof(name),"%s",label);
    for (unsigned i=0;name[i];i++) if (name[i]==' ') name[i]='-';
    snprintf(path,sizeof(path),"%s/g29-%s.xrgb",(const char *)ctx,name);
    FILE *f=fopen(path,"wb"); assert(f);
    assert(fwrite(u->canvas.pixels,4,(size_t)u->canvas.width*u->canvas.height,f)==(size_t)u->canvas.width*u->canvas.height); fclose(f);
    snprintf(path,sizeof(path),"%s/g29-%s.size",(const char *)ctx,name);
    f=fopen(path,"w"); assert(f); fprintf(f,"%d %d",u->canvas.width,u->canvas.height); fclose(f);
}
int main(int argc,char **argv)
{
    assert(argc<=2);
    disk_ui_scenario(argc==2 ? review_frame : NULL,argc==2 ? argv[1] : NULL);
    maintenance_ui_scenario(argc==2 ? review_frame : NULL,argc==2 ? argv[1] : NULL);
    lifetimes();
    puts("PASS G29b maintenance dialogs: analysis/map/Settings, Start/Pause/Stop, write restart, busy, conversion warnings/refusal/cancel/success");
    puts("PASS G29 disk dialogs: worker-only I/O, drive scope, preview, Cancel, confirmation, cleanup, format choices/label/full, busy refusal, allocation warnings and Cleanup action");
    return 0;
}
