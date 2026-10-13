/* SPDX-License-Identifier: MIT
 * Production shell input replay with an injectable FAT-like tree. */
#include "fake.h"
#define fake_reset fake_tree_reset
#include "fake_vfs.h"
#undef fake_reset
#include "../../../apps/libdeskui/deskui.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct desktop d;
static const char *out;
static char launched[FM_PATH], program[32];
static int launch(struct desktop *desktop, const struct desk_program *p)
{ strcpy(program,p->name); snprintf(launched,sizeof(launched),"%s",desktop->launch_arg ? desktop->launch_arg : ""); return 100; }
static void key(int code) { desk_shell_key(&d,code,1); desk_shell_key(&d,code,0); }
static void capture(const char *name)
{
    desk_shell_background(&d,(struct desk_box){0,0,1024,768});
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
{ for (unsigned i=0;i<1000 && d.sh.folder->busy;i++) desk_folder_service(&d); assert(!d.sh.folder->busy); }
static void close_message(void)
{ for (int i=0;i<DESK_CLIENTS;i++) if (d.clients[i].internal) desk_drop_client(&d,i); }
int main(int argc,char **argv)
{
    assert(argc==3); out=argv[1]; fake_reset(); fake_image_tree();
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
    desk_shell_enable(&d); assert(!desk_folder_init(&d,&fake_vfs));
    assert(d.sh.icon_count==6 && !strcmp(d.sh.icons[0].label,"Computer") && !strcmp(d.sh.icons[1].label,"Documents") && !strcmp(d.sh.icons[2].label,"Trash"));
    assert(named("Projects")>=3 && named("readme.txt")>=3); capture("listing");
    /* Scripted New > Folder replay through real shell menu keyboard input. */
    d.mouse_x=300; d.mouse_y=220; assert(desk_shell_button(&d,CIUKI_BUTTON_RIGHT,1)); capture("new-menu");
    key(DU_KEY_DOWN); key(DU_KEY_ENTER); assert(d.sh.levels==2 && d.sh.menus[1].kind==SH_MENU_NEW);
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
    int x,y; assert(cfg_icon_position(&d.sh.cfg,"Renamed = Folder",&x,&y) && x==352 && y==303);
    static char text[CFG_TEXT_MAX]; static struct ciuki_settings parsed;
    size_t len=cfg_format(&d.sh.cfg,text,sizeof(text)); assert(len); cfg_defaults(&parsed); assert(!cfg_parse(&parsed,text,len));
    assert(cfg_icon_position(&parsed,"Renamed = Folder",&x,&y) && x==352 && y==303);
    d.sh.cfg=parsed; desk_folder_changed(&d); n=named("Renamed = Folder"); assert(d.sh.icons[n].box.x==352); capture("positions");
    /* Rename carries the position to the new name. */
    desk_folder_rename(&d,n); strcpy(d.sh.folder->edit,"Moved name"); key(DU_KEY_ENTER); desk_folder_service(&d); n=named("Moved name"); assert(d.sh.icons[n].box.x==352);
    d.sh.icon_selected=n; key(DU_KEY_ENTER); assert(!strcmp(program,"files") && !strcmp(launched,DESK_FOLDER "/Moved name"));
    d.sh.icon_selected=named("Scene.BMP"); key(DU_KEY_ENTER); assert(!strcmp(program,"paint"));
    d.sh.icon_selected=named("readme.txt"); key(DU_KEY_ENTER); assert(!strcmp(program,"ciuknote"));
    d.sh.icon_selected=named("Moved name"); key(DU_KEY_DELETE); desk_folder_service(&d);
    assert(fake_lookup(DESK_FOLDER "/Moved name")<0 && fake_lookup(DESK_TRASH "/Moved name")>=0);
    command(DF_FOLDER,-1); key(DU_KEY_ESC); n=named("New Folder"); command(DF_DELETE,n-3);
    fake_create(DESK_FOLDER "/Moved name",FM_DIR); command(DF_REFRESH,-1); command(DF_DELETE,named("Moved name")-3); assert(fake_lookup(DESK_TRASH "/Moved name (2)")>=0); capture("trash");
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
    assert(strstr(d.sh.message_text,"error 2") && !strstr(d.sh.message_text,"error -2")); close_message();
    /* Shutdown cancels the job before the shell stops serving filesystem work. */
    fake_file("/Documents/Cancel.txt",30000,10); assert(!fm_refresh(other));
    assert(fm_select_name(other,"Cancel.txt",0)>=0 && !fm_clipboard(other,0) && !fm_clipboard_save(other));
    command(DF_PASTE,-1); assert(d.sh.folder->busy); fm_job_step(&d.sh.folder->job); fm_job_step(&d.sh.folder->job);
    d.sh.want_power=2; desk_folder_service(&d); d.sh.want_power=0;
    assert(!d.sh.folder->busy && d.sh.folder->job.state==FM_CANCELLED && fake_lookup("/Documents/Cancel.txt")>=0);
    fm_destroy(snapshot); free(snapshot);
    fm_destroy(other); free(other);
    /* Arrange resets saved positions through the context action. */
    desk_menu_open(&d,SH_MENU_DESKTOP,500,300); key(DU_KEY_DOWN); key(DU_KEY_DOWN); key(DU_KEY_DOWN); key(DU_KEY_ENTER);
    assert(!cfg_icon_position(&d.sh.cfg,"Moved name",&x,&y)); capture("arranged");
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
    assert(d.sh.icons[n].box.x==602); cache_equivalent();
    puts("PASS desktop folder wallpaper cache: live rename, cancel and icon drag match fresh render");
    desk_render_release(&d);
    fake_assert_clean(); desk_folder_destroy(&d); free(d.sh.icons); free(d.sh.wall); free((void *)d.portrait); free(d.pixels);
    puts("PASS desktop folder: listing, scripted New Folder, unique folder/text/BMP, FAT rename/errors, Trash collisions, association launches, shared clipboard copy/move, positions/rename/reparse/Arrange");
    return 0;
}
