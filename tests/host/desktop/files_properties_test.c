/* SPDX-License-Identifier: MIT
 * G6 production model/controller/toolkit: metadata sheets and edits. */
#include "fake_vfs.h"
#include "../../../apps/files/app.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "files_wait.h"
static struct files_app app;
static struct du_ui ui;
static uint32_t pixels[760*520];
static unsigned sets, label_sets;
static int refuse, counting, expect_worker;
static uint32_t volume_total=100,volume_free=75,volume_cluster=4096;
static pthread_t main_thread;
static char label[12]="CIUKIOS";
static int info(const char *path,struct ciuki_file_info *out)
{
    if (expect_worker) assert(!pthread_equal(main_thread,pthread_self()));
    int i=fake_lookup(path); if (i<0) return -ENOENT;
    *out=(struct ciuki_file_info){.version=1,.size=sizeof(*out),.mask=63,
        .attributes=fake_nodes[i].meta.attributes,.created=1791700000,.accessed=1791676800,
        .modified=1791700000,.file_size=fake_nodes[i].meta.size,.allocated=fake_nodes[i].meta.allocated,
        .flags=refuse ? CIUKI_FILE_INFO_READ_ONLY_MEDIUM : 0};
    strcpy(out->short_name,"LONGNA~1.TXT"); return 0;
}
static int version_info(const char *path,char *out,size_t n)
{ if (!strstr(path,"Program.exe")) return -ENOENT; snprintf(out,n,"File version:\t1.2.3.4\nCompany:\tCiukiOS"); return 0; }
/* fx-dosfull: an MS-DOS program's Screen usage (its DOS Box profile). */
static unsigned dos_value=3,dos_sets; static char dos_path[256];
static int dos_usage(const char *path,unsigned *usage)
{
    if (expect_worker) assert(!pthread_equal(main_thread,pthread_self()));
    if (!strstr(path,"GAME.EXE")) return -ENOEXEC;
    *usage=dos_value; return 0;
}
static int set_dos_usage(const char *path,unsigned usage)
{
    assert(pthread_equal(main_thread,pthread_self()) && usage<=3);
    dos_sets++; snprintf(dos_path,sizeof(dos_path),"%s",path);
    if (refuse) return -EROFS;
    dos_value=usage; return 0;
}
static int attributes(const char *path,uint32_t bits)
{
    sets++; if (refuse) return -EROFS;
    int i=fake_lookup(path); assert(i>=0); fake_nodes[i].meta.attributes=bits; return 0;
}
static int space(unsigned drive,struct ciuki_media_space *s)
{ assert(drive==2); *s=(struct ciuki_media_space){.cluster_bytes=volume_cluster,.total_clusters=volume_total,.free_clusters=volume_free,.flags=counting ? 1 : 0}; return 0; }
static int get_label(unsigned drive,char out[33]) { assert(drive==2); strcpy(out,label); return 0; }
static int set_label(unsigned drive,const char *text)
{ assert(drive==2); label_sets++; if (strlen(text)>11) return -EINVAL; strcpy(label,text); return 0; }
static void finish_job(void)
{
    for (unsigned i=0;i<5000 && app.job_active;i++) {
        struct du_event tick={.type=DU_EV_TICK,.id=1}; files_event(&ui,&tick,&app);
        struct timespec pause={.tv_sec=0,.tv_nsec=1000000}; nanosleep(&pause,NULL);
    }
    assert(!app.job_active && ui.dialog.sheet);
}
static void sheet(const char *path) { files_test_wait(&app); expect_worker=1; files_properties_path(&app,path); finish_job(); expect_worker=0; }
static void press(unsigned key)
{
    struct ciuki_input_event e={.type=CIUKI_INPUT_KEY,.code=(int)key,.value=1};
    du_input(&ui,&e); e.value=0; du_input(&ui,&e);
}
static void apply(void) { ui.dialog.focus=2; press(DU_KEY_ENTER); assert(ui.dialog.sheet); }
static void cancel(void) { press(DU_KEY_ESC); assert(!du_dialog_id(&ui)); }
static void dump(const char *dir,const char *name)
{
    du_render(&ui); char path[1200]; snprintf(path,sizeof(path),"%s/%s.xrgb",dir,name);
    FILE *f=fopen(path,"wb"); assert(f); assert(fwrite(pixels,4,760*520,f)==760*520); fclose(f);
    snprintf(path,sizeof(path),"%s/%s.size",dir,name); f=fopen(path,"w"); assert(f); fprintf(f,"760 520"); fclose(f);
}
/* A Win98 property sheet repaints its active page only, and switching pages leaves nothing of the previous one:
 * the incremental paint of the pending damage must equal a full repaint of the same state. */
static uint32_t partial[760*520];
static void same_as_full(const char *what)
{
    du_render(&ui); memcpy(partial,pixels,sizeof(partial));
    ui.dirty=1; du_render(&ui);
    if (memcmp(partial,pixels,sizeof(partial))) { fprintf(stderr,"incremental paint differs from a full repaint: %s\n",what); abort(); }
}
int main(int argc,char **argv)
{
    assert(argc==2); main_thread=pthread_self(); fake_reset(); fake_image_tree();
    int n=fake_file("/Documents/Long name.txt",10,1); assert(n>=0);
    fake_nodes[n].meta.allocated=4096; fake_nodes[n].meta.allocated_known=1;
    fake_nodes[n].meta.attributes=CIUKI_ATTR_ARCHIVE;
    int second=fake_file("/Documents/Plain.txt",20,1); assert(second>=0);
    fake_file("/Documents/Program.exe",12,1);
    fake_create("/Documents/Sub",FM_DIR); int hidden=fake_file("/Documents/Sub/Hidden.txt",11,1); assert(hidden>=0);
    fake_nodes[hidden].meta.attributes=CIUKI_ATTR_HIDDEN;
    fake_nodes[hidden].meta.allocated=4096; fake_nodes[hidden].meta.allocated_known=1;
    du_init(&ui,pixels,760,520); assert(!files_setup(&app,&ui,&fake_vfs,"/Documents"));
    app.file_info=info; app.set_attributes=attributes; app.media_space=space; app.media_label=get_label; app.set_label=set_label; app.version_info=version_info;
    app.dos_usage=dos_usage; app.set_dos_usage=set_dos_usage;
    files_test_wait(&app);app.show_folder_options=1;assert(ui.service);ui.service(&ui,ui.user);
    assert(!app.show_folder_options && ui.dialog.sheet && app.property_options);cancel();
    sheet("/Documents/Long name.txt");
    assert(strstr(ui.dialog.text,"LONGNA~1.TXT") && strstr(ui.dialog.text,"4.00 KB") && strstr(ui.dialog.text,"Created:"));
    assert(ui.dialog.items[6].value==1 && !ui.dialog.items[4].disabled);
    dump(argv[1],"g6-file");
    ui.dialog.items[4].value=1; cancel(); assert(!sets);
    sheet("/Documents/Long name.txt"); ui.dialog.items[4].value=1; apply();
    assert(sets==1 && fake_nodes[n].meta.attributes==(CIUKI_ATTR_ARCHIVE|CIUKI_ATTR_READONLY));
    ui.dialog.focus=0; press(DU_KEY_ENTER); assert(!du_dialog_id(&ui) && sets==1);
    sheet("/Documents/Long name.txt"); refuse=1; ui.dialog.items[5].value=1; apply();
    assert(sets==2 && strstr(ui.dialog.text,"Cannot apply changes:")); cancel();
    sheet("/Documents/Long name.txt"); assert(ui.dialog.items[4].disabled); cancel(); refuse=0;
    /* Mixed attributes change only the edited bit, preserving Archive. */
    files_test_wait(&app); assert(!files_navigate(&app,"/Documents"));
    fm_select_none(&app.model); assert(fm_select_name(&app.model,"Long name.txt",0)>=0); assert(fm_select_name(&app.model,"Plain.txt",1)>=0);
    struct du_event prop={.type=DU_EV_ACTIVATE,.id=8}; files_event(&ui,&prop,&app); finish_job();
    assert(ui.dialog.items[4].value==2 && ui.dialog.items[6].value==2);
    ui.dialog.items[4].value=1; apply();
    assert(fake_nodes[n].meta.attributes==(CIUKI_ATTR_READONLY|CIUKI_ATTR_ARCHIVE));
    assert(fake_nodes[second].meta.attributes==CIUKI_ATTR_READONLY); cancel();
    sheet("/Documents/Program.exe"); assert(ui.dialog.items[3].count==2);
    ui.dialog.items[3].active=1; ui.dialog.changed(&ui,&(struct du_event){.type=DU_EV_SELECT},&app);
    assert(strstr(ui.dialog.text,"1.2.3.4") && ui.dialog.items[4].hidden); dump(argv[1],"g6-version"); cancel();
    sheet("/Documents/Sub"); assert(app.job.files==1 && app.job.bytes==11 && app.job.allocated==4096); dump(argv[1],"g6-folder"); cancel();
    sheet("/"); assert(app.property_drive==2 && app.property_have_space && ui.dialog.items[3].count==2);
    assert(!strcmp(app.property_rows[0],"102,400 bytes 0 MB") && !strcmp(app.property_rows[1],"307,200 bytes 0 MB") && !strcmp(app.property_rows[2],"409,600 bytes 0 MB") && !ui.dialog.text[0]); dump(argv[1],"g6-drive");
    du_set_text(&ui.dialog.items[4],"DATA"); apply(); assert(label_sets==1 && !strcmp(label,"DATA"));
    struct ciuki_input_event ctrl={.type=CIUKI_INPUT_KEY,.code=DU_KEY_LCTRL,.value=1};
    du_input(&ui,&ctrl); press(DU_KEY_TAB); ctrl.value=0; du_input(&ui,&ctrl);
    same_as_full("drive sheet, Ctrl+Tab to Tools");
    ctrl.value=1; du_input(&ui,&ctrl); press(DU_KEY_TAB); ctrl.value=0; du_input(&ui,&ctrl);
    same_as_full("drive sheet, Ctrl+Tab back to General");
    ctrl.value=1; du_input(&ui,&ctrl); press(DU_KEY_TAB); ctrl.value=0; du_input(&ui,&ctrl);
    /* G14: Check Now (slot 7, id 410: slot 5 is G8's Disk Cleanup) is on the Tools page only. */
    assert(ui.dialog.items[3].active==1 && strstr(ui.dialog.text,"Check Now checks this drive") && ui.dialog.items[4].hidden &&
           !ui.dialog.items[7].hidden && ui.dialog.items[7].id==410 && !strcmp(ui.dialog.items[7].text,"Check &Now...")); dump(argv[1],"g6-tools"); cancel();
    counting=1; sheet("/"); assert(strstr(app.property_rows[0],"calculating...") && strstr(app.property_rows[1],"calculating...")); dump(argv[1],"g6-counting"); cancel();
    counting=0;volume_total=76544;volume_free=0;sheet("/");
    assert(strstr(app.property_rows[2],"313,524,224 bytes 299 MB") && strstr(app.property_rows[1],"0 bytes 0 MB"));
    du_render(&ui);
    unsigned top_blue=0,top_purple=0,front=0;
    for (unsigned i=0;i<760*520;i++) {
        uint32_t c=pixels[i]&0xffffffu;
        top_blue+=c==0x2059c5;top_purple+=c==0xc52daf;front+=c==0x102c62;
    }
    assert(top_blue>5000 && top_purple<=100 && front>1000);cancel();
    volume_free=volume_total;sheet("/");du_render(&ui);cancel();
    volume_total=0;volume_free=0;sheet("/");assert(!strcmp(app.property_rows[2],"0 bytes 0 MB"));du_render(&ui);cancel();
    volume_total=100;volume_free=75;
    /* View options filter only browser listings; Properties includes hidden children. */
    assert(!files_navigate(&app,"/Documents/Sub") && app.model.count==0);
    app.model.show_hidden=1; assert(!files_navigate(&app,"/Documents/Sub") && app.model.count==1);
    app.model.hide_system=1; fake_nodes[hidden].meta.attributes|=CIUKI_ATTR_SYSTEM;
    assert(!files_navigate(&app,"/Documents/Sub") && app.model.count==0);
    sheet("/Documents/Sub"); assert(app.job.files==1 && app.job.bytes==11); cancel();
    counting=0; assert(!files_navigate(&app,"/Documents"));
    struct du_event options={.type=DU_EV_ACTIVATE,.id=310}; files_event(&ui,&options,&app);
    assert(ui.dialog.sheet); ui.dialog.items[4].value=1; ui.dialog.items[6].value=1; apply(); cancel();
    files_test_wait(&app); assert(app.model.show_hidden && app.model.hide_extensions);
    unsigned long_index=0; for (;long_index<app.model.count;long_index++) if (!strcmp(app.model.entries[long_index].name,"Long name.txt")) break;
    assert(long_index<app.model.count && !strcmp(app.rows[long_index].text[0],"Long name"));
    struct du_event column={.type=DU_EV_ACTIVATE,.id=311}; files_event(&ui,&column,&app);
    assert(du_get(&ui,107)->column_count==5 && !strcmp(app.rows[long_index].text[4],"RA"));
    assert(strstr(du_get(&ui,108)->text,"300 KB free"));
    dump(argv[1],"g6-attributes");
    /* fx-dosfull: a DOS program's sheet has General and Screen (Windows 98
     * Screen tab, Usage); the Usage group shows only on Screen, the default
     * graphics full screen first, and Apply stores only a changed usage. */
    fake_file("/Documents/GAME.EXE",12,1);   /* added last: the earlier frames keep their listing */
    sheet("/Documents/GAME.EXE"); assert(app.property_have_dos && app.property_dos_usage==3 && !app.property_have_version);
    assert(ui.dialog.items[3].count==2 && !strcmp(ui.dialog.items[3].items[1].text[0],"Screen"));
    assert(!ui.dialog.items[4].hidden && ui.dialog.items[8].hidden && ui.dialog.items[9].hidden && ui.dialog.items[12].hidden);
    ui.dialog.items[3].active=1; ui.dialog.changed(&ui,&(struct du_event){.type=DU_EV_SELECT},&app);
    assert(ui.dialog.items[4].hidden && !ui.dialog.items[8].hidden && ui.dialog.items[8].kind==DU_GROUP);
    assert(ui.dialog.items[9].kind==DU_RADIO && ui.dialog.items[9].value==1 && !ui.dialog.items[10].value && !ui.dialog.items[12].value);
    assert(!strcmp(ui.dialog.items[12].text,"&Window") && strstr(ui.dialog.text,"Alt+Enter"));
    same_as_full("DOS program sheet, Screen page");
    dump(argv[1],"g6-dos-screen");
    apply(); assert(!dos_sets);
    ui.dialog.items[9].value=0; ui.dialog.items[12].value=1; apply();
    assert(dos_sets==1 && dos_value==0 && !strcmp(dos_path,"/Documents/GAME.EXE") && app.property_dos_usage==0);
    apply(); assert(dos_sets==1); cancel();
    sheet("/Documents/GAME.EXE"); assert(app.property_dos_usage==0);
    ui.dialog.items[3].active=1; ui.dialog.changed(&ui,&(struct du_event){.type=DU_EV_SELECT},&app);
    assert(ui.dialog.items[12].value==1 && !ui.dialog.items[9].value);
    refuse=1; ui.dialog.items[12].value=0; ui.dialog.items[10].value=1; apply();
    assert(dos_sets==2 && dos_value==0 && app.property_dos_usage==0 && strstr(ui.dialog.text,"Cannot apply changes:")); cancel(); refuse=0;
    struct fm_model *fresh=calloc(1,sizeof(*fresh)); assert(fresh); fm_init(fresh,&fake_vfs);
    int columns=0; assert(!fm_view_load(fresh,&columns) && fresh->show_hidden && fresh->hide_system && fresh->hide_extensions && columns);
    struct du_event failure_options={.type=DU_EV_ACTIVATE,.id=310}; files_event(&ui,&failure_options,&app);
    strcpy(fake_readonly,"/system/settings"); ui.dialog.items[4].value=0; apply();
    assert(ui.dialog.sheet && app.model.show_hidden && strstr(ui.dialog.text,"Cannot save Folder Options"));
    fake_readonly[0]=0; cancel();
    int settings=fake_lookup("/system/settings/files.view"); assert(settings>=0);
    fake_nodes[settings].data[0]='X'; fresh->show_hidden=0; assert(fm_view_load(fresh,&columns)==-EINVAL && !fresh->show_hidden);
    fm_destroy(fresh); free(fresh);
    files_destroy(&app); du_dialog_close(&ui); fake_assert_clean();
    puts("PASS G6 Properties: file/folder/drive, allocation, alias/dates, Cancel/Apply/OK, error retry, read-only, labels, Tools, Ctrl+Tab, hidden/system and raw totals");
    return 0;
}
