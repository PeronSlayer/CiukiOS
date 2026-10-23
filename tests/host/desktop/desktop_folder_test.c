/* SPDX-License-Identifier: MIT
 * Production shell input replay with an injectable FAT-like tree. */
#include "fake.h"
#define fake_reset fake_tree_reset
#include "fake_vfs.h"
#undef fake_reset
#include "../../../apps/libdeskui/deskui.h"
#include "../../../apps/files/transfer_ui.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
static pthread_mutex_t poll_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t poll_changed=PTHREAD_COND_INITIALIZER;
static int poll_hold, poll_entered, poll_release, poll_error;
static pthread_t poll_frame;
static int copy_hold, copy_entered, copy_release;
static int folder_read(void *ctx,int fd,void *out,unsigned n)
{
    if (n>256) {
        assert(!pthread_equal(pthread_self(),poll_frame));
        pthread_mutex_lock(&poll_lock);
        if (copy_hold) {
            copy_entered=1; pthread_cond_broadcast(&poll_changed);
            while (!copy_release) pthread_cond_wait(&poll_changed,&poll_lock);
            copy_hold=0;
        }
        pthread_mutex_unlock(&poll_lock);
    }
    return fake_vfs.read(ctx,fd,out,n);
}
static int folder_list(void *ctx,const char *path,unsigned flags,fm_emit emit,void *arg)
{
    int e=fake_vfs.list(ctx,path,flags,emit,arg);
    if (!strcmp(path,DESK_FOLDER) && flags==FM_LIST_NAMES_ONLY && !pthread_equal(pthread_self(),poll_frame)) {
        pthread_mutex_lock(&poll_lock);
        if (poll_hold) {
            poll_entered=1; pthread_cond_broadcast(&poll_changed);
            while (!poll_release) pthread_cond_wait(&poll_changed,&poll_lock);
            poll_hold=0;
        }
        if (poll_error) { e=-EIO; poll_error=0; }
        pthread_mutex_unlock(&poll_lock);
    }
    return e;
}
static void poll_folder(struct desktop *desk)
{
    desk_folder_service(desk);
    for (unsigned i=0;desk_folder_busy(desk);i++) {
        assert(i<3000); struct timespec pause={0,1000000}; nanosleep(&pause,NULL);
        desk_folder_service(desk);
    }
    desk_folder_service(desk); /* consume a completion published after the last service */
}
static struct desktop d;
static const char *out;
static char launched[FM_PATH], program[32];
static int launch(struct desktop *desktop, const struct desk_program *p)
{ strcpy(program,p->name); snprintf(launched,sizeof(launched),"%s",desktop->launch_arg ? desktop->launch_arg : ""); return 100; }
static void key(int code) { desk_shell_key(&d,code,1); desk_shell_key(&d,code,0); }
static void capture(const char *name)
{
    if (!strncmp(name,"desktop-",8)) desk_composite(&d,(struct desk_box){0,0,1024,768});
    else desk_shell_background(&d,(struct desk_box){0,0,1024,768});
    desk_shell_topbar(&d,(struct desk_box){0,0,1024,768});
    desk_shell_overlay(&d,(struct desk_box){0,0,1024,768});
    char path[1024]; snprintf(path,sizeof(path),"%s/%s.xrgb",out,name);
    FILE *f=fopen(path,"wb"); assert(f && fwrite(d.pixels,4,1024*768,f)==1024*768 && !fclose(f));
}
/* A cached repaint must match a fresh wallpaper/icon render, including
 * live rename text and icons moved by the actual input path. */
static void cache_equivalent(void)
{
    size_t bytes=(size_t)d.width*d.height*4;
    uint32_t *cached=malloc(bytes); assert(cached);
    struct desk_box full={0,0,(int)d.width,(int)d.height};
    desk_shell_background(&d,full); memcpy(cached,d.pixels,bytes);
    d.sh.background_valid=0; desk_shell_background(&d,full);
    assert(!memcmp(cached,d.pixels,bytes)); free(cached);
}
static void command(int command, int index)
{ d.sh.folder->command=command; d.sh.folder->index=index; desk_folder_service(&d); }
static int named(const char *name)
{ for (int i=3;i<d.sh.icon_count;i++) if (!strcmp(d.sh.icons[i].label,name)) return i; return -1; }
static void settle(void)
{
    for (unsigned i=0;d.sh.folder->busy;i++) {
        assert(i<5000); d.now_ns+=1000000; desk_folder_service(&d);
        struct timespec t={0,1000000}; nanosleep(&t,NULL);
    }
}
#include "folder_transfer_test.h"
static void close_message(void)
{ for (int i=0;i<DESK_CLIENTS;i++) if (d.clients[i].internal) desk_drop_client(&d,i); }
#include "desktop_dnd_test.h"
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IOLBF,0); assert(argc==3); poll_frame=pthread_self(); out=argv[1]; fake_reset(); fake_image_tree();
    fake_create(DESK_FOLDER,FM_DIR); fake_create(DESK_TRASH,FM_DIR);
    fake_create(DESK_FOLDER "/Projects",FM_DIR); fake_file(DESK_FOLDER "/readme.txt",5,20);
    fake_file(DESK_FOLDER "/Scene.BMP",58,1);
    desk_init(&d,&fake_ops,1024,768); d.launch=launch; d.icon=du_icon_argb;
    uint8_t elf[84]={0x7f,'E','L','F',1,1,1}; /* register known programs directly, independent of ELF inspection */
    (void)elf;
    const char *names[]={"files","ciuknote","paint","viewer"};
    for (unsigned i=0;i<4;i++) { strcpy(d.programs[i].name,names[i]); snprintf(d.programs[i].path,sizeof(d.programs[i].path),"/Programs/%s",names[i]); } d.program_count=4;
    d.pixels=calloc(1024*768,4); assert(d.pixels);
    d.portrait=malloc(256*256*4); assert(d.portrait);
    FILE *p=fopen(argv[2],"rb"); assert(p && fread((void *)d.portrait,4,256*256,p)==256*256 && !fclose(p));
    struct fm_vfs fs=fake_vfs; fs.list=folder_list; fs.read=folder_read;
    desk_shell_enable(&d); assert(!desk_folder_init(&d,&fs));
    assert(d.sh.icon_count==6 && !strcmp(d.sh.icons[0].label,"Computer") && !strcmp(d.sh.icons[1].label,"Documents") && !strcmp(d.sh.icons[2].label,"Trash"));
    assert(named("Pictures")<0);
    d.pictures_exists=1; desk_folder_changed(&d);
    int pictures=d.sh.icon_count-1;
    assert(d.sh.icon_count==7 && !strcmp(d.sh.icons[pictures].label,"Pictures"));
    assert(!strcmp(d.sh.icons[pictures].argument,"/Documents/Pictures"));
    d.sh.icon_selected=pictures; key(DU_KEY_F2); key(DU_KEY_DELETE);
    assert(d.sh.folder->editing<0 && d.sh.folder->command==DF_NONE);
    key(DU_KEY_ENTER); assert(!strcmp(program,"files") && !strcmp(launched,"/Documents/Pictures"));
    d.sh.menu_owner=pictures; desk_menu_open(&d,SH_MENU_ICON,500,200);
    assert(d.sh.menus[0].count==3); key(DU_KEY_ESC);
    assert(named("Projects")>=3 && named("readme.txt")>=3); capture("listing");
    /* Scripted New > Folder replay through real shell menu keyboard input. */
    d.mouse_x=300; d.mouse_y=220; assert(desk_shell_button(&d,CIUKI_BUTTON_RIGHT,1)); capture("new-menu");
    for (unsigned k=0;k<4;k++) key(DU_KEY_DOWN); key(DU_KEY_ENTER); assert(d.sh.levels==2 && d.sh.menus[1].kind==SH_MENU_NEW);
    key(DU_KEY_ENTER); desk_folder_service(&d);
    assert(fake_lookup(DESK_FOLDER "/New Folder")>=0 && d.sh.folder->editing>=0); capture("new-folder"); key(DU_KEY_ESC);
    command(DF_FOLDER,-1); assert(fake_lookup(DESK_FOLDER "/New Folder (2)")>=0); key(DU_KEY_ESC);
    command(DF_TEXT,-1); assert(fake_lookup(DESK_FOLDER "/New Text Document.txt")>=0); key(DU_KEY_ESC);
    command(DF_TEXT,-1); assert(fake_lookup(DESK_FOLDER "/New Text Document (2).txt")>=0); key(DU_KEY_ESC);
    fake_short_io=1; command(DF_BITMAP,-1); fake_short_io=0;
    int bmp=fake_lookup(DESK_FOLDER "/New Bitmap Image.bmp"); assert(bmp>=0 && fake_nodes[bmp].meta.size==58 && !memcmp(fake_nodes[bmp].data,"BM",2)); key(DU_KEY_ESC);
    command(DF_BITMAP,-1); assert(fake_lookup(DESK_FOLDER "/New Bitmap Image (2).bmp")>=0); key(DU_KEY_ESC);
    int n=named("New Folder"); d.sh.icon_selected=n; key(DU_KEY_F2); assert(d.sh.folder->editing==n-3);
    strcpy(d.sh.folder->edit,"bad:name"); key(DU_KEY_ENTER); desk_folder_service(&d); assert(d.sh.folder->editing>=0 && fake_lookup(DESK_FOLDER "/New Folder")>=0); close_message();
    strcpy(d.sh.folder->edit,"trailing."); key(DU_KEY_ENTER); desk_folder_service(&d); assert(d.sh.folder->editing>=0); close_message();
    strcpy(d.sh.folder->edit,"Projects"); key(DU_KEY_ENTER); desk_folder_service(&d); assert(d.sh.folder->editing>=0); close_message();
    strcpy(d.sh.folder->edit,"Renamed = Folder"); key(DU_KEY_ENTER); desk_folder_service(&d);
    assert(fake_lookup(DESK_FOLDER "/Renamed = Folder")>=0 && d.sh.folder->editing<0); capture("renamed");
    n=named("Renamed = Folder"); struct desk_box b=d.sh.icons[n].box;
    d.mouse_x=b.x+40; d.mouse_y=b.y+20; d.now_ns=2000000000ull; desk_shell_button(&d,1,1);
    d.mouse_x=400; d.mouse_y=350; desk_shell_motion(&d); desk_shell_button(&d,1,0);
    int x,y; assert(cfg_icon_position(&d.sh.cfg,"Renamed = Folder",&x,&y) && x==360 && y==330);
    static char text[CFG_TEXT_MAX]; static struct ciuki_settings parsed;
    size_t len=cfg_format(&d.sh.cfg,text,sizeof(text)); assert(len); cfg_defaults(&parsed); assert(!cfg_parse(&parsed,text,len));
    assert(cfg_icon_position(&parsed,"Renamed = Folder",&x,&y) && x==360 && y==330);
    d.sh.cfg=parsed; desk_folder_changed(&d); n=named("Renamed = Folder"); assert(d.sh.icons[n].box.x==360); capture("positions");
    /* Rename carries the position to the new name. */
    desk_folder_rename(&d,n); strcpy(d.sh.folder->edit,"Moved name"); key(DU_KEY_ENTER); desk_folder_service(&d); n=named("Moved name"); assert(d.sh.icons[n].box.x==360);
    d.sh.icon_selected=n; key(DU_KEY_ENTER); assert(!strcmp(program,"files") && !strcmp(launched,DESK_FOLDER "/Moved name"));
    /* G6: desktop Alt+Enter and context Properties route to the same sheet. */
    d.focus=-1; desk_shell_key(&d,DU_KEY_LALT,1); key(DU_KEY_ENTER); desk_shell_key(&d,DU_KEY_LALT,0);
    assert(!strcmp(program,"files") && !strcmp(launched,"--properties=" DESK_FOLDER "/Moved name"));
    d.sh.menu_owner=n; desk_menu_open(&d,SH_MENU_ICON,500,200);
    int properties_item=-1;
    for (int i=0;i<d.sh.menus[0].count;i++) if (!strcmp(d.sh.menus[0].items[i].label,"Properties")) properties_item=i;
    assert(properties_item>=0); d.sh.menus[0].hot=properties_item; key(DU_KEY_ENTER);
    assert(!strcmp(program,"files") && !strcmp(launched,"--properties=" DESK_FOLDER "/Moved name"));
    puts("PASS G6 desktop Properties: Alt+Enter and context menu share the Files sheet path");
    d.sh.icon_selected=named("Scene.BMP"); key(DU_KEY_ENTER); assert(!strcmp(program,"viewer"));
    d.sh.menu_owner=d.sh.icon_selected; desk_menu_open(&d,SH_MENU_OPEN_WITH,500,200);
    int paint_item=-1;
    for (int i=0;i<d.sh.menus[0].count;i++) if (!strcmp(d.sh.menus[0].items[i].label,"CiukPaint")) paint_item=i;
    assert(paint_item>=0);
    for (int i=0;i<=paint_item;i++) key(DU_KEY_DOWN);
    key(DU_KEY_ENTER); assert(!strcmp(program,"paint") && !strcmp(launched,DESK_FOLDER "/Scene.BMP"));
    d.sh.icon_selected=named("readme.txt"); key(DU_KEY_ENTER); assert(!strcmp(program,"ciuknote"));
    d.sh.icon_selected=named("Moved name"); key(DU_KEY_DELETE); desk_folder_service(&d); settle();
    assert(fake_lookup(DESK_FOLDER "/Moved name")<0 && fake_lookup(DESK_TRASH "/Moved name")>=0);
    command(DF_FOLDER,-1); key(DU_KEY_ESC); n=named("New Folder"); command(DF_DELETE,n-3); settle();
    fake_create(DESK_FOLDER "/Moved name",FM_DIR); command(DF_REFRESH,-1); command(DF_DELETE,named("Moved name")-3); settle(); assert(fake_lookup(DESK_TRASH "/Moved name (2)")>=0); capture("trash");
    /* Shared clipboard and incremental copy/move with short I/O. */
    struct fm_model *other=calloc(1,sizeof(*other)); assert(other); fm_init(other,&fake_vfs); assert(!fm_navigate(other,"/Documents"));
    assert(!fm_create(other,"Incoming.txt",0)); assert(!fm_clipboard(other,0) && !fm_clipboard_save(other));
    command(DF_PASTE,-1); settle(); assert(fake_lookup(DESK_FOLDER "/Incoming.txt")>=0 && fake_lookup("/Documents/Incoming.txt")>=0);
    assert(!fm_create(other,"Dragged.txt",0)); assert(!fm_clipboard(other,1) && !fm_clipboard_save(other));
    d.mouse_x=700; d.mouse_y=600; desk_folder_drop(&d,0,0); settle(); assert(fake_lookup(DESK_FOLDER "/Dragged.txt")>=0 && fake_lookup("/Documents/Dragged.txt")<0);
    /* A Files drop onto a desktop folder uses that directory. */
    assert(!fm_create(other,"Into folder.txt",0) && !fm_clipboard(other,0) && !fm_clipboard_save(other));
    int projects=named("Projects"); assert(projects>=3); d.mouse_x=d.sh.icons[projects].box.x+30; d.mouse_y=d.sh.icons[projects].box.y+30;
    desk_folder_drop(&d,0,1); settle(); assert(fake_lookup(DESK_FOLDER "/Projects/Into folder.txt")>=0 && fake_lookup("/Documents/Into folder.txt")>=0);
    /* Desktop-to-folder move through the actual shell drag/release path. */
    int incoming=named("Incoming.txt"); projects=named("Projects"); assert(incoming>=3);
    b=d.sh.icons[incoming].box; d.mouse_x=b.x+30; d.mouse_y=b.y+30; desk_shell_button(&d,1,1);
    d.mouse_x=d.sh.icons[projects].box.x+30; d.mouse_y=d.sh.icons[projects].box.y+30;
    desk_shell_motion(&d); desk_shell_button(&d,1,0); settle();
    assert(fake_lookup(DESK_FOLDER "/Projects/Incoming.txt")>=0 && fake_lookup(DESK_FOLDER "/Incoming.txt")<0);
    /* Production compositor broker: desktop -> Files, Files -> desktop,
     * authenticated sender and strict packet shape. */
    fake_channel(10); int client=desk_add_client(&d,10); assert(client>=0);
    struct desk_client *c=&d.clients[client]; c->hello=1; c->window=1; c->version=2; c->minor=1; c->pid=42;
    c->program=DESK_APP_FILES; c->frame=(struct desk_box){500,100,400,300}; strcpy(c->title,"Files");
    d.mouse_x=650; d.mouse_y=200; d.sh.icon_selected=named("readme.txt");
    unsigned sent=fake_sent; desk_folder_drop(&d,-1,1); desk_flush(&d,client);
    assert(fake_sent>sent && fake_messages[fake_sent-1].data[1]==DESK_FILE_DROP && fake_destinations[fake_sent-1]==10);
    struct desk_packet packet; assert(desk_parse(&fake_messages[fake_sent-1],0,NULL,&packet) && packet.flags==1);
    assert(!fm_create(other,"Broker.txt",0) && !fm_clipboard(other,0) && !fm_clipboard_save(other));
    struct ciuki_message msg; desk_message(&msg,DESK_FILE_DROP,16); msg.data[0]=2; msg.data[2]=1; msg.sender_pid=42;
    struct desk_box content=desk_content(c); desk_put32(msg.data+4,1); desk_put32(msg.data+8,(uint32_t)(300-content.x)); desk_put32(msg.data+12,(uint32_t)(600-content.y));
    c->program=0; desk_receive(&d,client,&msg); assert(!d.sh.folder->busy);
    c->program=DESK_APP_FILES; desk_receive(&d,client,&msg); settle(); assert(fake_lookup(DESK_FOLDER "/Broker.txt")>=0);
    desk_put32(msg.data+4,2); assert(!desk_parse(&msg,1,NULL,&packet)); msg.fd_count=1; msg.fds[0]=11; assert(!desk_parse(&msg,1,NULL,&packet));
    desk_drop_client(&d,client);
    struct fm_model *snapshot=calloc(1,sizeof(*snapshot)); assert(snapshot); fm_init(snapshot,&fake_vfs);
    assert(!fm_navigate(snapshot,"/Documents") && !fm_create(snapshot,"Old cut.txt",0) && !fm_clipboard(snapshot,1) && !fm_clipboard_save(snapshot));
    assert(!fm_create(other,"Latest copy.txt",0) && !fm_clipboard(other,0) && !fm_clipboard_save(other));
    assert(!fm_clipboard_consume(snapshot)); fake_short_io=1; assert(!fm_clipboard_load(snapshot)); fake_short_io=0;
    assert(snapshot->clipboard_count==1 && !strcmp(snapshot->clipboard[0],"/Documents/Latest copy.txt"));
    fake_nodes[fake_lookup("/Documents/Latest copy.txt")].live=0;
    command(DF_PASTE,-1); settle();
    /* fx-17: the failure names the item with Explorer's reason text. */
    assert(strstr(d.sh.message_text,"Cannot copy 'Latest copy.txt':\nThe file or folder cannot be found.") && !strstr(d.sh.message_text,"(-2)")); close_message();
    /* Shutdown cancels the job before the shell stops serving filesystem work. */
    fake_file("/Documents/Cancel.txt",30000,10); assert(!fm_refresh(other));
    assert(fm_select_name(other,"Cancel.txt",0)>=0 && !fm_clipboard(other,0) && !fm_clipboard_save(other));
    command(DF_PASTE,-1); assert(d.sh.folder->busy);
    d.sh.want_power=2; desk_folder_service(&d); settle(); d.sh.want_power=0;
    assert(!d.sh.folder->busy && d.sh.folder->job.state==FM_CANCELLED && fake_lookup("/Documents/Cancel.txt")>=0);
    folder_transfer_cases(other);
    fx19_desktop_cases();
    fm_destroy(snapshot); free(snapshot);
    fm_destroy(other); free(other);
    /* Line up snaps without placing two icons in the same grid cell. */
    desk_menu_open(&d,SH_MENU_DESKTOP,500,300); key(DU_KEY_DOWN); key(DU_KEY_DOWN); key(DU_KEY_ENTER);
    for (int a=0;a<d.sh.icon_count;a++) {
        struct desk_box b=d.sh.icons[a].box;
        assert((b.x-8)%b.w==0 && (b.y-DESK_BAR-10)%b.h==0);
        for (int c=0;c<a;c++) assert(b.x!=d.sh.icons[c].box.x || b.y!=d.sh.icons[c].box.y);
    }
    /* Arrange resets saved positions through the context action. */
    desk_menu_open(&d,SH_MENU_DESKTOP,500,300); key(DU_KEY_DOWN); key(DU_KEY_ENTER);
    assert(!cfg_icon_position(&d.sh.cfg,"Moved name",&x,&y)); capture("arranged");
    /* A Desktop entry called Pictures has an independent position and actions. */
    fake_file(DESK_FOLDER "/Pictures",1,0); command(DF_REFRESH,-1);
    pictures=d.sh.icon_count-1; int file_pictures=named("Pictures");
    assert(file_pictures>=3 && file_pictures<pictures);
    assert(cfg_icon_move(&d.sh.cfg,"Pictures",200,200));
    assert(cfg_icon_move(&d.sh.cfg,"/Pictures",400,400)); desk_shell_apply(&d);
    assert(d.sh.icons[file_pictures].box.x==200 && d.sh.icons[pictures].box.x==400);
    d.sh.icon_selected=pictures; key(DU_KEY_F2); key(DU_KEY_DELETE);
    assert(d.sh.folder->editing<0 && d.sh.folder->command==DF_NONE);
    d.sh.menu_owner=pictures; desk_menu_open(&d,SH_MENU_ICON,500,200);
    assert(d.sh.menus[0].count==3); key(DU_KEY_ESC);
    d.sh.menu_owner=file_pictures; desk_menu_open(&d,SH_MENU_ICON,500,200);
    assert(d.sh.menus[0].count>3); key(DU_KEY_ESC);
    command(DF_DELETE,file_pictures-3); settle();
    assert(fake_lookup(DESK_FOLDER "/Pictures")<0 && fake_lookup(DESK_TRASH "/Pictures")>=0);
    d.pictures_exists=0; desk_folder_changed(&d); assert(named("Pictures")<0);
    d.sh.wall=malloc((size_t)d.width*d.height*4); assert(d.sh.wall);
    for (unsigned i=0;i<d.width*d.height;i++) d.sh.wall[i]=0x234567;
    d.sh.wall_w=d.width; d.sh.wall_h=d.height; d.sh.wall_valid=1;
    cache_equivalent(); assert(d.sh.background_valid);
    n=named("readme.txt"); assert(n>=3);
    desk_folder_rename(&d,n); cache_equivalent();
    key(0x1e); cache_equivalent(); key(DU_KEY_ESC); cache_equivalent();
    b=d.sh.icons[n].box; d.mouse_x=b.x+30; d.mouse_y=b.y+20;
    d.now_ns=10000000000ull; desk_shell_button(&d,1,1);
    d.mouse_x=650; d.mouse_y=600; desk_shell_motion(&d); desk_shell_button(&d,1,0);
    assert(d.sh.icons[n].box.x==620); cache_equivalent();
    puts("PASS desktop folder wallpaper cache: live rename, cancel and icon drag match fresh render");
    /* Periodic scans preserve an unchanged model and notice external edits. */
    struct fm_entry *original=d.sh.folder->model.entries;
    d.now_ns+=1000000000ull; poll_folder(&d);
    assert(d.sh.folder->model.entries==original);
    fake_file(DESK_FOLDER "/External.txt",7,1);
    d.now_ns+=1000000000ull; poll_folder(&d); assert(named("External.txt")>=3);
    int external=fake_lookup(DESK_FOLDER "/External.txt"); assert(external>=0);
    fake_nodes[external].meta.size=8; fake_nodes[external].meta.modified++;
    d.now_ns+=1000000000ull; poll_folder(&d);
    assert(d.sh.folder->model.entries[named("External.txt")-3].meta.size==8);
    fake_nodes[external].live=0;
    d.now_ns+=1000000000ull; poll_folder(&d); assert(named("External.txt")==-1);
    /* A failed enumeration keeps the model; successful retry sees the edit. */
    original=d.sh.folder->model.entries; fake_file(DESK_FOLDER "/Retry.txt",1,0);
    pthread_mutex_lock(&poll_lock); poll_error=1; pthread_mutex_unlock(&poll_lock);
    d.now_ns+=1000000000ull; poll_folder(&d);
    assert(d.sh.folder->model.entries==original && named("Retry.txt")==-1);
    d.now_ns+=1000000000ull; poll_folder(&d); assert(named("Retry.txt")>=3);
    /* A completed old poll must not overwrite a newer explicit command. */
    fake_file(DESK_FOLDER "/Stale.txt",1,0);
    pthread_mutex_lock(&poll_lock); poll_hold=1; poll_release=poll_entered=0; pthread_mutex_unlock(&poll_lock);
    d.now_ns+=1000000000ull; desk_folder_service(&d);
    for (unsigned i=0;;i++) {
        pthread_mutex_lock(&poll_lock); int entered=poll_entered; pthread_mutex_unlock(&poll_lock);
        if (entered) break;
        assert(i<3000); struct timespec pause={0,1000000}; nanosleep(&pause,NULL);
    }
    command(DF_TEXT,-1); key(DU_KEY_ESC); original=d.sh.folder->model.entries;
    pthread_mutex_lock(&poll_lock); poll_release=1; pthread_cond_broadcast(&poll_changed); pthread_mutex_unlock(&poll_lock);
    poll_folder(&d);
    assert(d.sh.folder->model.entries==original && named("Stale.txt")>=3);
    puts("PASS F2-79 folder: unchanged buffer retained, async add/resize/remove, failed poll/retry and stale command result rejection");
    /* fx-17 (T23 trial of e831e685): a move from the read-only stick
     * (E:) into the desktop folder copied the song, failed to delete the
     * source and reported a failed operation. A move whose original cannot
     * be deleted keeps both copies and says so (cut + paste: a move). */
    close_message();
    if (fake_lookup("/mnt")<0) fake_create("/mnt",FM_DIR);
    fake_create("/mnt/e",FM_DIR); fake_file("/mnt/e/song.mp3",30000,5);
    snprintf(fake_readonly,sizeof(fake_readonly),"/mnt/e");
    struct fm_model *stick=calloc(1,sizeof(*stick)); assert(stick); fm_init(stick,&fake_vfs); assert(!fm_navigate(stick,"/mnt/e"));
    assert(fm_select_name(stick,"song.mp3",0)>=0 && !fm_clipboard(stick,1) && !fm_clipboard_save(stick));
    command(DF_PASTE,-1); settle();
    assert(d.sh.folder->job.kind==FM_MOVE && d.sh.folder->job.state==FM_DONE && d.sh.folder->job.kept==1);
    assert(fake_lookup(DESK_FOLDER "/song.mp3")>=0 && fake_lookup("/mnt/e/song.mp3")>=0 && named("song.mp3")>=3);
    assert(d.sh.message_kind==DU_DLG_ICON_WARNING && !strcmp(d.sh.message_title,"Moving Files"));
    assert(strstr(d.sh.message_text,"'song.mp3' was copied to C:\\Documents\\Desktop, but the original on E: could not be deleted")
           && strstr(d.sh.message_text,"Both copies are kept") && !strstr(d.sh.message_text,"Cannot"));
    /* The cut stays on the clipboard: the original was not moved. */
    assert(!fm_clipboard_load(stick) && stick->cut && stick->clipboard_count==1);
    close_message(); fake_readonly[0]=0; fm_destroy(stick); free(stick);
    puts("PASS fx-17 desktop move from a read-only stick: both copies kept, warning names the file and the drive, cut retained");
    desk_render_release(&d);
    fake_assert_clean(); desk_folder_destroy(&d); free(d.sh.icons); free(d.sh.wall); free((void *)d.portrait); free(d.pixels);
    puts("PASS desktop folder: listing, scripted New Folder, unique folder/text/BMP, FAT rename/errors, Trash collisions, association launches, shared clipboard copy/move, positions/rename/reparse/Arrange, conditional Pictures isolation");
    return 0;
}
