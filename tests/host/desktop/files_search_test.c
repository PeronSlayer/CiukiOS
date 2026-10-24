/* SPDX-License-Identifier: MIT
 * G7: production model, worker, controls and usual result actions. */
#include "fake_vfs.h"
#include "../../../apps/files/app.c"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>

static struct files_app app;
static struct du_ui ui;
static uint32_t pixels[620*380], reference[620*380];
static int launched;
static char launched_path[FM_PATH];
static int on_spawn(struct files_app *a, const char *program, char *const argv[])
{ (void)a; assert(!strcmp(program,"/Programs/ciuknote")); assert(argv[1]); strcpy(launched_path,argv[1]); launched++; return 0; }
static void wait_worker(void)
{
    for (unsigned i=0;(app.reading || app.job_active) && i<5000;i++) { scan_poll(&app); job_tick(&app); struct timespec t={.tv_sec=0,.tv_nsec=1000000}; nanosleep(&t,NULL); }
    assert(!app.reading && !app.job_active);
}
static void frame(void)
{
    static unsigned number; number++;
    du_render(&ui); memcpy(reference,pixels,sizeof(pixels)); ui.dirty=1; du_render(&ui);
    if (memcmp(reference,pixels,sizeof(pixels))) fprintf(stderr,"frame %u dialog=%d page=%d\n",number,ui.dialog.id,app.find_page);
    assert(!memcmp(reference,pixels,sizeof(pixels)));
    if (ui.dialog.kind) {
        struct du_dialog *d=&ui.dialog;
        for (unsigned i=0;i<d->count;i++) {
            assert(d->items[i].box.x>=d->box.x && d->items[i].box.y>=d->box.y);
            assert(d->items[i].box.x+d->items[i].box.w<=d->box.x+d->box.w);
            assert(d->items[i].box.y+d->items[i].box.h<=d->box.y+d->box.h);
        }
    }
}
static void button(unsigned index)
{
    struct du_rect b=du_dialog_button(&ui,index);
    struct ciuki_input_event e={.type=CIUKI_INPUT_MOTION,.value=b.x+b.w/2,.value2=b.y+b.h/2}; du_input(&ui,&e);
    e=(struct ciuki_input_event){.type=CIUKI_INPUT_BUTTON,.code=CIUKI_BUTTON_LEFT,.value=1}; du_input(&ui,&e);
    e.value=0; du_input(&ui,&e);
}
static unsigned row(const char *path)
{
    for (unsigned i=0;i<app.model.count;i++) { char p[FM_PATH]; assert(!fm_entry_path(&app.model,i,p)); if (!strcmp(p,path)) return i; }
    assert(!"missing result"); return 0;
}
static void tree(void)
{
    fake_reset(); assert(fake_create("/system",FM_DIR)>=0); assert(fake_create("/system/settings",FM_DIR)>=0); assert(fake_create("/Documents",FM_DIR)>=0); assert(fake_create("/Documents/Desktop",FM_DIR)>=0); assert(fake_create("/tmp",FM_DIR)>=0); assert(fake_create("/out",FM_DIR)>=0); assert(fake_create("/one",FM_DIR)>=0); assert(fake_create("/two",FM_DIR)>=0);
    assert(fake_create("/one/sub",FM_DIR)>=0); assert(fake_create("/Programs",FM_DIR)>=0);
    fake_file("/Programs/ciuknote",1,0); fake_file("/one/same.txt",100,0); fake_file("/two/same.txt",200,0);
    fake_file("/one/sub/Report.TXT",9000,0); fake_file("/one/plain",0,0); fake_file("/one/space name.txt",0,0);
    assert(fake_create("/dev",FM_DIR)>=0); fake_file("/dev/private.txt",0,0);
    assert(fake_create("/RECYCLED",FM_DIR)>=0); fake_file("/RECYCLED/private.txt",0,0);
    int n=fake_lookup("/one/sub/Report.TXT"); memset(fake_nodes[n].data,'x',9000);
    memcpy(fake_nodes[n].data+4093,"HeLLo Needle",12);
}
static int search_read_error;
static int error_read(void *ctx, int fd, void *buf, unsigned n)
{ return search_read_error ? search_read_error : fake_vfs.read(ctx,fd,buf,n); }
static int denied_list(void *ctx, const char *path, unsigned flags, fm_emit emit, void *arg)
{ return !strcmp(path,"/one/sub") ? -EACCES : fake_vfs.list(ctx,path,flags,emit,arg); }
static int many_list(void *ctx, const char *path, unsigned flags, fm_emit emit, void *arg)
{
    (void)ctx; (void)flags;
    if (!strcmp(path,"/one")) {
        struct fm_meta dir={.type=FM_DIR,.ino=100,.dev=1}; int r=emit("a",&dir,arg);
        dir.ino++; return r ? r : emit("b",&dir,arg);
    }
    unsigned count=!strcmp(path,"/one/a") ? FM_ENTRIES : 1;
    struct fm_meta file={.type=FM_FILE};
    for (unsigned i=0;i<count;i++) { char name[32]; snprintf(name,sizeof(name),"f%u.txt",i); int r=emit(name,&file,arg); if(r) return r; }
    return 0;
}
static void model_tests(void)
{
    struct fm_model *m=calloc(1,sizeof(*m)); assert(m); fm_init(m,&fake_vfs);
    struct fm_search_query *q=calloc(1,sizeof(*q)); assert(q); strcpy(q->look_in,"C:\\");
    q->recursive=1; strcpy(q->named,"*.TXT"); assert(!fm_search(m,q) && m->count==4 && !m->search_incomplete);
    /* G6 attributes govern result visibility and traversal, with metadata retained. */
    fake_nodes[fake_lookup("/one/same.txt")].meta.attributes=2;
    fake_nodes[fake_lookup("/one/sub")].meta.attributes=4;
    m->hide_system=1;
    assert(!fm_search(m,q) && m->count==2);
    m->show_hidden=1; assert(!fm_search(m,q) && m->count==3);
    m->hide_system=0; assert(!fm_search(m,q) && m->count==4);
    fm_sort(m,4,1); assert(m->sort==4 && m->entries[0].meta.attributes==2);
    fm_sort(m,FM_SORT_LOCATION,0); assert(m->sort==FM_SORT_LOCATION);
    fake_nodes[fake_lookup("/one/same.txt")].meta.attributes=0;
    fake_nodes[fake_lookup("/one/sub")].meta.attributes=0;
    m->show_hidden=0; assert(!fm_search(m,q) && m->count==4);
    char p[FM_PATH]; assert(!fm_entry_path(m,0,p)); assert(strstr(p,"/one/") || strstr(p,"/two/"));
    strcpy(q->look_in,"C:\\one"); q->recursive=0; assert(!fm_search(m,q) && m->count==2);
    q->recursive=1; strcpy(q->containing,"hello needle"); assert(!fm_search(m,q) && m->count==1);
    fake_short_io=1; assert(!fm_search(m,q) && m->count==1); fake_short_io=0; fake_assert_clean();
    q->containing[0]=0; strcpy(q->type,"txt"); q->size_mode=1; q->size_bytes=9000; assert(!fm_search(m,q) && m->count==1);
    q->size_bytes=9001; assert(!fm_search(m,q) && !m->count);
    q->size_mode=2; q->size_bytes=100; assert(!fm_search(m,q) && m->count==2);
    q->size_mode=0; q->type[0]=0; q->named[0]=0;
    q->date_from_set=q->date_to_set=1; q->date_from=q->date_to=fake_nodes[fake_lookup("/one/same.txt")].meta.modified;
    assert(!fm_search(m,q) && m->count==1);
    q->date_from++; assert(!fm_search(m,q) && !m->count);
    q->date_from_set=q->date_to_set=0; strcpy(q->type,"folders"); assert(!fm_search(m,q) && m->count==1);
    strcpy(q->type,"files"); assert(!fm_search(m,q) && m->count==4);
    q->look_in[0]=0; q->type[0]=0; strcpy(q->named,"same.txt"); assert(!fm_search(m,q) && m->count==2);
    m->active=1; m->anchor=1; unsigned identity=m->entries[1].result; fm_sort(m,1,1);
    assert(m->entries[m->active].result==identity && m->entries[m->anchor].result==identity);
    fm_select(m,m->active,0); assert(!fm_clipboard(m,0) && m->clipboard_count==1);
    assert(!strcmp(m->clipboard[0],m->result_paths[identity-1]));
    assert(!fm_navigate(m,"/one") && !m->searching && !m->result_paths);
    assert(fm_search_name("*.*","plain") && fm_search_name("*.txt;*.png","x.PNG"));
    assert(fm_search_name("\"space name.txt\" *.png","space name.txt"));
    assert(fm_search_name("REPORT","my report.txt") && fm_search_name("r?p*.*","Report"));
    assert(!fm_search_name("?.txt","ab.txt") && !fm_search_name("*.txt","a.png"));
    int64_t a,b; assert(!fm_search_date("2024-02-29",0,&a) && !fm_search_date("2024-02-29",1,&b) && b-a==86399);
    assert(fm_search_date("2023-02-29",0,&a)==-EINVAL && fm_search_date("2024-13-01",0,&a)==-EINVAL);
    uint64_t size; assert(!fm_search_size("0",&size) && !size); assert(!fm_search_size("4",&size) && size==4096);
    assert(fm_search_size("-1",&size)==-EINVAL && fm_search_size("18446744073709551615",&size)==-EINVAL);
    setenv("TZ","EST5EDT,M3.2.0/2,M11.1.0/2",1); tzset();
    assert(!fm_search_date("2024-03-10",0,&a) && !fm_search_date("2024-03-10",1,&b) && b-a==82799);
    setenv("TZ","UTC0",1); tzset();
    memset(q,0,sizeof(*q)); strcpy(q->look_in,"/one"); strcpy(q->named,"*.txt"); q->recursive=1;
    m->fs.list=denied_list; assert(!fm_search(m,q) && m->search_incomplete && m->search_error==-EACCES && m->count==2);
    m->fs.list=fake_vfs.list; m->fs.read=error_read; strcpy(q->containing,"needle");
    search_read_error=-EIO; assert(!fm_search(m,q) && !m->count && m->search_incomplete && m->search_error==-EIO); fake_assert_clean();
    search_read_error=-ECANCELED; assert(fm_search(m,q)==-ECANCELED && m->search_incomplete); fake_assert_clean();
    search_read_error=0; q->containing[0]=0; m->fs.list=many_list;
    assert(!fm_search(m,q) && m->count==FM_ENTRIES && m->search_incomplete && m->search_error==-E2BIG);
    m->fs.list=fake_vfs.list; m->fs.read=fake_vfs.read;
    strcpy(q->look_in,"/missing"); assert(fm_search(m,q)==-ENOENT && m->count==FM_ENTRIES);
    /* A deep directory tree reports its bound instead of silently losing it. */
    char deep[FM_PATH]="/deep"; assert(fake_create(deep,FM_DIR)>=0);
    for(unsigned i=0;i<FM_DEPTH+1;i++) { strcat(deep,"/d"); assert(fake_create(deep,FM_DIR)>=0); }
    memset(q,0,sizeof(*q)); strcpy(q->look_in,"/deep"); q->recursive=1;
    assert(!fm_search(m,q) && m->search_incomplete && m->search_error==-E2BIG);
    fm_destroy(m); free(m); free(q); fake_assert_clean();
    puts("PASS G7 model: names, wildcards, text/chunk boundaries/short I/O, recursion, date/size/type, Computer, duplicate paths, sorting and clipboard");
}
static int slow_reads, read_entered, released;
static int slow_read(void *ctx,int fd,void *buf,unsigned n)
{
    if (__atomic_load_n(&slow_reads,__ATOMIC_ACQUIRE)) {
        __atomic_store_n(&read_entered,1,__ATOMIC_RELEASE);
        while (!__atomic_load_n(&released,__ATOMIC_ACQUIRE)) { struct timespec t={.tv_sec=0,.tv_nsec=1000000}; nanosleep(&t,NULL); }
    }
    return fake_vfs.read(ctx,fd,buf,n);
}
static void ui_tests(void)
{
    struct fm_vfs fs=fake_vfs; fs.read=slow_read;
    du_init(&ui,pixels,620,380); app.spawn=on_spawn;
    assert(!files_setup(&app,&ui,&fs,"/one")); wait_worker(); frame();
    struct ciuki_input_event key={.type=CIUKI_INPUT_KEY,.code=FM_FIND_KEY,.value=1}; du_input(&ui,&key); key.value=0; du_input(&ui,&key);
    assert(ui.dialog.id==D_FIND && ui.dialog.count==10); frame();
    assert(!strcmp(ui.dialog.items[8].text,"C:\\one"));
    du_set_text(&ui.dialog.items[4],"*.txt"); du_set_text(&ui.dialog.items[8],"Computer");
    button(2); assert(app.find_page==1); frame();
    du_set_text(&ui.dialog.items[4],"2023-02-29"); button(0); assert(ui.dialog.id==D_FIND && strstr(ui.dialog.title,"Check dates"));
    du_set_text(&ui.dialog.items[4],""); button(2); assert(app.find_page==2); frame();
    du_set_text(&ui.dialog.items[4],"files"); button(0); assert(app.loading); wait_worker();
    assert(app.model.searching && app.model.count==4); assert(W(&app,W_LIST)->column_count==5); frame();
    /* Both Details columns survive, and their header mapping keeps main's key 4. */
    app.attribute_column=1; files_layout(&app); files_update(&app);
    assert(W(&app,W_LIST)->column_count==6);
    assert(!strcmp(W(&app,W_LIST)->columns[4].title,"Attributes"));
    assert(!strcmp(W(&app,W_LIST)->columns[5].title,"In Folder"));
    struct du_event sort={.type=DU_EV_SORT,.id=W_LIST,.index=5}; files_event(&ui,&sort,&app);
    assert(app.model.sort==FM_SORT_LOCATION && W(&app,W_LIST)->sort==5);
    sort.index=4; files_event(&ui,&sort,&app); assert(app.model.sort==4);
    app.attribute_column=0; files_layout(&app); files_update(&app);
    sort.index=4; files_event(&ui,&sort,&app);
    assert(app.model.sort==FM_SORT_LOCATION && W(&app,W_LIST)->sort==4); frame();
    assert(W(&app,C_PROPERTIES)->disabled && W(&app,C_PASTE)->disabled);
    command(&app,C_PROPERTIES); assert(!app.job_active && !ui.dialog.kind);
    const int new_commands[]={FILES_NEW_SHORTCUT,FILES_NEW_BITMAP,FILES_NEW_REGISTERED,FILES_PASTE_SHORTCUT};
    for (unsigned i=0;i<sizeof(new_commands)/sizeof(*new_commands);i++) {
        command(&app,new_commands[i]); assert(!app.job_active && !ui.dialog.kind);
    }
    assert(ui.menu_count==6 && !strcmp(ui.menus[4].label,"&Tools"));
    struct du_event background={.type=DU_EV_CONTEXT,.id=W_LIST,.index=-1,.x=300,.y=150}; context(&app,&background);
    for(unsigned i=0;i<app.popup_count;i++) if (app.popup[i].id==C_PASTE || app.popup[i].id==C_NEW_FOLDER || app.popup[i].id==C_NEW_TEXT || app.popup[i].id==C_PROPERTIES) assert(app.popup[i].flags & DU_ITEM_DISABLED);
    frame(); struct ciuki_input_event escape={.type=CIUKI_INPUT_KEY,.code=DU_KEY_ESC,.value=1}; du_input(&ui,&escape); escape.value=0; du_input(&ui,&escape); frame();
    unsigned a=row("/one/same.txt"),b=row("/two/same.txt"); fm_select(&app.model,a,0); fm_select(&app.model,b,DU_CTRL);
    assert(fm_selected(&app.model,NULL)==2); command(&app,C_COPY); assert(app.model.clipboard_count==2 && !ui.dialog.kind);
    /* Cut and selection survive a refresh without selecting another equal name. */
    fm_select(&app.model,b,0); command(&app,C_CUT); command(&app,C_REFRESH); wait_worker();
    assert(fm_selected(&app.model,NULL)==1); assert(app.model.active==row("/two/same.txt"));
    command(&app,C_OPEN_NOTE); assert(launched==1 && !strcmp(launched_path,"/two/same.txt"));
    command(&app,C_RENAME); du_set_text(W(&app,W_RENAME),"changed.txt"); rename_commit(&app); wait_worker();
    assert(fake_lookup("/two/changed.txt")>=0 && fake_lookup("/two/same.txt")<0);
    /* G8 records the actual result parent, even for equal names in other folders. */
    command(&app,FILES_UNDO); wait_worker();
    assert(fake_lookup("/two/same.txt")>=0 && fake_lookup("/two/changed.txt")<0);
    fm_select(&app.model,row("/two/same.txt"),0);
    command(&app,C_RENAME); du_set_text(W(&app,W_RENAME),"changed.txt"); rename_commit(&app); wait_worker();
    fm_select(&app.model,row("/two/changed.txt"),0);
    command(&app,FILES_OPEN_WITH); assert(ui.dialog.id==D_OP_WITH);
    assert(!strcmp(app.operations->selected,"/two/changed.txt")); frame(); du_dialog_close(&ui);
    command(&app,FILES_SHORTCUT); wait_worker();
    assert(fake_lookup("/two/Shortcut to changed.txt.lnk")>=0 && fake_lookup("/Shortcut to changed.txt.lnk")<0);
    fm_select(&app.model,row("/two/changed.txt"),0);
    command(&app,FILES_SEND_TO); assert(ui.popup_count>=5); du_menu_close(&ui);
    command(&app,OP_SEND_BASE); wait_worker();
    assert(fake_lookup("/Documents/Desktop/Shortcut to changed.txt.lnk")>=0);
    fm_select(&app.model,row("/two/changed.txt"),0);
    command(&app,FILES_COPY_TO); assert(ui.dialog.kind==DU_DLG_FOLDER); du_dialog_close(&ui);
    struct du_event dest={.type=DU_EV_DIALOG,.id=D_OP_DEST,.index=0,.text="C:\\out"};
    files_event(&ui,&dest,&app); wait_worker(); assert(fake_lookup("/out/changed.txt")>=0);
    command(&app,FILES_UNDO); wait_worker(); assert(fake_lookup("/out/changed.txt")<0);
    b=row("/two/changed.txt"); fm_select(&app.model,b,0); command(&app,C_PROPERTIES); wait_worker(); assert(ui.dialog.id==D_PROPERTIES); frame(); du_dialog_close(&ui);
    char **paths; unsigned count;
    assert(!selection_paths(&app,&paths,&count) && count==1);
    assert(!start_job(&app,FM_COPY,paths,count,"/out"));
    for(unsigned i=0;i<count;i++) free(paths[i]); free(paths); wait_worker();
    assert(fake_lookup("/out/changed.txt")>=0); frame();
    fm_select(&app.model,row("/two/changed.txt"),0);
    assert(!selection_paths(&app,&paths,&count) && count==1); assert(!start_job(&app,FM_COPY,paths,count,"/"));
    for(unsigned i=0;i<count;i++) free(paths[i]); free(paths); wait_worker();
    assert(fm_selected(&app.model,NULL)==1 && app.model.active==row("/changed.txt"));
    command(&app,C_DELETE_NOW); button(0); wait_worker(); assert(fake_lookup("/changed.txt")<0); frame();
    fm_select(&app.model,row("/one/same.txt"),0); command(&app,C_DELETE); assert(ui.dialog.id==D_DELETE); button(0); wait_worker();
    assert(fake_lookup("/one/same.txt")<0 && fake_lookup("/two/changed.txt")>=0);
    assert(app.job.bin && app.job.bin->mode==RB_RECYCLE && app.job.state==FM_DONE); frame();
    fm_select(&app.model,row("/two/changed.txt"),0); command(&app,C_DELETE_NOW); assert(ui.dialog.id==D_DELETE); button(0); wait_worker();
    assert(fake_lookup("/two/changed.txt")<0 && fake_lookup("/out/changed.txt")>=0 && fake_lookup("/one/same.txt")<0); frame();
    fm_select(&app.model,row("/out/changed.txt"),0);
    command(&app,C_FIND_FOLDER); wait_worker(); assert(!strcmp(app.model.path,"/out") && !app.model.searching); frame();
    /* Search cancellation is nonblocking; a subsequent navigation wins. */
    files_find(&app); du_set_text(&ui.dialog.items[4],"*.txt"); du_set_text(&ui.dialog.items[6],"never matches"); du_set_text(&ui.dialog.items[8],"C:\\one");
    __atomic_store_n(&slow_reads,1,__ATOMIC_RELEASE); button(0);
    for(unsigned i=0;i<5000 && !__atomic_load_n(&read_entered,__ATOMIC_ACQUIRE);i++) { struct timespec t={.tv_sec=0,.tv_nsec=1000000}; nanosleep(&t,NULL); }
    assert(read_entered && app.loading); files_find(&app); assert(!app.loading);
    assert(!files_navigate(&app,"/two")); __atomic_store_n(&released,1,__ATOMIC_RELEASE); wait_worker();
    assert(!app.model.searching && !strcmp(app.model.path,"/two")); frame();
    files_destroy(&app); du_release(&ui); fake_assert_clean();
    memset(&app,0,sizeof(app)); du_init(&ui,pixels,620,380); app.find_initial=1;
    assert(!files_setup(&app,&ui,&fake_vfs,FM_COMPUTER)); wait_worker();
    assert(ui.dialog.id==D_FIND && !strcmp(ui.dialog.items[8].text,"Computer")); frame();
    button(0); wait_worker(); assert(app.model.searching);
    unsigned drive=0; while (drive<app.tree_count && strcmp(app.tree[drive].path,"/")) drive++;
    assert(drive<app.tree_count);
    struct du_event select={.type=DU_EV_SELECT,.id=W_TREE,.index=(int)drive}; files_event(&ui,&select,&app); wait_worker();
    assert(!app.model.searching && !strcmp(app.model.path,"/")); frame();
    files_destroy(&app); du_release(&ui); fake_assert_clean();
    puts("PASS G7 UI: F3, all criteria pages, validation, results/actions, full-redraw equivalence, cancellation and stale-result exclusion");
}
int main(void) { setenv("TZ","UTC0",1); tzset(); tree(); model_tests(); tree(); ui_tests(); return 0; }
