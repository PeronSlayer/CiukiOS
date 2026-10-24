/* SPDX-License-Identifier: MIT -- G8 real input/dialogs and partial redraw. */
#include "fake_vfs.h"
#include "../../../apps/files/app.h"
#include "../../../apps/w32srv/registry.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static int raw_navigate(struct files_app *a,const char *p) { return files_navigate(a,p); }
#include "files_wait.h"
static struct files_app app;
static struct du_ui ui;
static struct wr_server registry;
static uint32_t *pixels,*reference;
static unsigned frames,launches;
static const char *frame_directory;
static char launched[260],argument[1040];
static uint32_t exchange(const void *in,uint32_t n,void *out,uint32_t cap,uint32_t *used)
{ size_t count=0; uint32_t e=wr_dispatch(&registry,42,in,n,out,cap,&count); *used=(uint32_t)count; return e; }
static int spawn_test(struct files_app *a,const char *program,char *const argv[])
{ (void)a; launches++; snprintf(launched,sizeof(launched),"%s",program); snprintf(argument,sizeof(argument),"%s",argv[1]?argv[1]:""); return 7; }
static void frame(void)
{
    du_render(&ui); memcpy(reference,pixels,760*520*4); ui.dirty=1; du_render(&ui);
    if (memcmp(reference,pixels,760*520*4)) fprintf(stderr,"redraw mismatch frame=%u dialog=%d focus=%d popup=%u\n",frames,du_dialog_id(&ui),ui.dialog.focus,ui.popup_count);
    assert(!memcmp(reference,pixels,760*520*4)); assert(pixels[760*520]==0x1234abcdu); frames++;
    int id=du_dialog_id(&ui);
    if (id>=240 && id<=244) {
        char path[1100]; snprintf(path,sizeof(path),"%s/g8-dialog-%d.xrgb",frame_directory,id);
        FILE *f=fopen(path,"wb"); assert(f && fwrite(pixels,4,760*520,f)==760*520 && !fclose(f));
    }
}
static void settle(void)
{
    for (unsigned i=0;app.job_active || app.reading;i++) {
        assert(i<5000); ui.now+=10000000;
        du_tick(&ui,ui.now); files_test_wait(&app);
        struct timespec pause={0,1000000}; nanosleep(&pause,NULL);
    }
    frame();
}
static void event(int type,int id,int index,const char *text)
{ struct du_event e={.type=type,.id=id,.index=index,.text=text}; files_event(&ui,&e,&app); frame(); }
static void action(int id) { du_menu_close(&ui); event(DU_EV_ACTIVATE,id,0,NULL); }
static void key(int code)
{
    struct ciuki_input_event e={.type=CIUKI_INPUT_KEY,.code=code,.value=1,.monotonic_ns=ui.now+20000000};
    du_input(&ui,&e); e.value=0; du_input(&ui,&e); files_test_wait(&app); frame();
}
static void select_name(const char *name)
{
    assert(!files_navigate(&app,"/work")); assert(fm_select_name(&app.model,name,0)>=0);
    files_update(&app); frame();
}
static void accept(void) { key(DU_KEY_ENTER); settle(); }
static void cancel(void) { key(DU_KEY_ESC); settle(); }
static void choose_dest(void)
{
    assert(ui.dialog.folders_only); du_set_text(&ui.dialog.items[5],"C:\\dest"); ui.dialog.focus=5;
    key(DU_KEY_ENTER); assert(!strcmp(ui.dialog.folder,"C:\\dest")); ui.dialog.focus=0; accept();
}
int main(int argc,char **argv)
{
    assert(argc==2); frame_directory=argv[1]; setvbuf(stdout,NULL,_IONBF,0);
    struct wr_io io; wr_posix_io(&io,argv[1]); assert(wr_init(&registry,&io,0)); fm_registry_exchange=exchange;
    fake_image_tree(); fake_create("/work",FM_DIR); fake_create("/dest",FM_DIR); fake_create("/system/settings",FM_DIR);
    fake_create(FM_SENDTO,FM_DIR); fake_file("/work/a.txt",12,1); fake_file("/work/b.txt",9,2);
    int note=fake_file("/Programs/ciuknote",4,1),paint=fake_file("/Programs/paint",4,1);
    memcpy(fake_nodes[note].data,"\177ELF",4); memcpy(fake_nodes[paint].data,"\177ELF",4);
    pixels=calloc(760*520+1,4); reference=calloc(760*520,4); assert(pixels && reference); pixels[760*520]=0x1234abcd;
    du_init(&ui,pixels,760,520); app.spawn=spawn_test; assert(!files_setup(&app,&ui,&fake_vfs,"/work")); frame();
    char *selected_paths[]={"/work/a.txt","/work/b.txt"}; char manifest[FM_PATH];
    assert(!fm_shell_selection_write(&fake_vfs,selected_paths,2,manifest));
    assert(!fm_shell_selection_read(&fake_vfs,manifest,&app.shell_paths,&app.shell_count)); assert(!fake_vfs.unlink(NULL,manifest));
    assert(!raw_navigate(&app,"/work")); files_shell_action(&app,FILES_COPY_TO,NULL); files_test_wait(&app);
    assert(fm_selected(&app.model,NULL)==2 && ui.dialog.folders_only); choose_dest();
    assert(fake_lookup("/dest/a.txt")>=0 && fake_lookup("/dest/b.txt")>=0); action(FILES_UNDO); settle();
    assert(fake_lookup("/dest/a.txt")<0 && fake_lookup("/dest/b.txt")<0);
    action(FILES_NEW_BITMAP); settle(); assert(fake_lookup("/work/New Bitmap Image.bmp")>=0);
    select_name("a.txt"); action(FILES_SHORTCUT); settle(); assert(fake_lookup("/work/Shortcut to a.txt.lnk")>=0);
    assert(!fm_clipboard(&app.model,0) && !fm_clipboard_save(&app.model));
    action(FILES_PASTE_SHORTCUT); settle(); assert(fake_lookup("/work/Shortcut to a.txt (2).lnk")>=0);
    action(FILES_NEW_SHORTCUT); assert(du_dialog_id(&ui)==241);
    du_set_text(&ui.dialog.items[2],"\"/work/b.txt\""); accept(); assert(fake_lookup("/work/Shortcut to b.txt.lnk")>=0);
    select_name("Shortcut to a.txt.lnk"); event(DU_EV_OPEN,107,(int)app.model.active,NULL);
    assert(launches && !strcmp(launched,"/Programs/ciuknote") && !strcmp(argument,"/work/a.txt"));
    select_name("a.txt"); action(FILES_COPY_TO); choose_dest(); assert(fake_lookup("/dest/a.txt")>=0);
    action(FILES_UNDO); settle(); assert(fake_lookup("/dest/a.txt")<0);
    select_name("b.txt"); action(FILES_MOVE_TO); choose_dest(); assert(fake_lookup("/work/b.txt")<0 && fake_lookup("/dest/b.txt")>=0);
    action(FILES_UNDO); settle(); assert(fake_lookup("/work/b.txt")>=0 && fake_lookup("/dest/b.txt")<0);
    action(FILES_UNDO); settle(); assert(du_dialog_id(&ui) && !strcmp(ui.dialog.title,"Undo")); accept();
    select_name("a.txt"); action(FILES_COPY_TO); cancel(); assert(fake_lookup("/dest/a.txt")<0);
    action(FILES_OPEN_WITH); assert(du_dialog_id(&ui)==242);
    ui.dialog.focus=2; key(DU_KEY_ENTER); assert(du_dialog_id(&ui)==243);
    du_set_text(&ui.dialog.items[5],"paint"); ui.dialog.focus=5; accept();
    assert(du_dialog_id(&ui)==242 && !strcmp(ui.dialog.items[3].text,"/Programs/paint"));
    cancel(); action(FILES_OPEN_WITH);
    du_set_text(&ui.dialog.items[3],"/Programs/paint"); ui.dialog.items[4].value=0; ui.dialog.focus=0; accept();
    assert(!strcmp(launched,"/Programs/paint"));
    struct desk_launch_record r; assert(!fm_association("/work/a.txt",NULL,&r) && !strcmp(r.target,"/Programs/ciuknote"));
    action(FILES_OPEN_WITH); du_set_text(&ui.dialog.items[3],"/Programs/paint"); ui.dialog.items[4].value=1; ui.dialog.focus=0; accept();
    assert(!fm_association("/work/a.txt",NULL,&r) && !strcmp(r.target,"/Programs/paint"));
    action(FILES_TYPES); assert(du_dialog_id(&ui)==244);
    du_set_text(&ui.dialog.items[5],"Ciuki edited text"); du_set_text(&ui.dialog.items[6],"C:\\Windows\\Paint.exe,2");
    du_set_text(&ui.dialog.items[7],"edit"); du_set_text(&ui.dialog.items[9],"\"/Programs/ciuknote\" \"%1\"");
    ui.dialog.items[11].value=0; ui.dialog.focus=0; accept();
    struct fm_filetype *type=calloc(1,sizeof(*type)); assert(type); strcpy(type->extension,".txt"); assert(!fm_filetype_get(type));
    assert(!strcmp(type->description,"Ciuki edited text") && !strcmp(type->verb,"edit") && (type->edit_flags&0x10000)); free(type);
    char type_buffer[80]; assert(!strcmp(fm_type_name(&app.model.entries[app.model.active],type_buffer,sizeof(type_buffer)),"Ciuki edited text"));
    action(FILES_TYPES); ui.dialog.focus=3; key(DU_KEY_ENTER);
    assert(du_dialog_id(&ui)==244 && ui.dialog.items[4].text[0]=='.');
    du_set_text(&ui.dialog.items[4],".txt"); ui.dialog.focus=2; key(DU_KEY_ENTER);
    assert(!strcmp(ui.dialog.items[5].text,"Ciuki edited text")); cancel();
    select_name("a.txt"); action(FILES_SEND_TO); assert(ui.popup_count>=5); action(400); settle();
    assert(fake_lookup("/Documents/Desktop/Shortcut to a.txt.lnk")>=0);
    action(FILES_SEND_TO); action(401); settle(); assert(fake_lookup("/Documents/a.txt")>=0);
    struct fm_model *send=calloc(1,sizeof(*send)); assert(send); fm_init(send,&fake_vfs); strcpy(send->path,FM_SENDTO);
    r=(struct desk_launch_record){.label="Destination",.target="/dest",.show=1}; char link[FM_PATH]; assert(!fm_shortcut(send,&r,link)); fm_destroy(send); free(send);
    action(FILES_SEND_TO); assert(app.popup_count==6); action(405); settle(); assert(fake_lookup("/dest/a.txt")>=0);
    send=calloc(1,sizeof(*send)); assert(send); fm_init(send,&fake_vfs); strcpy(send->path,FM_SENDTO);
    r=(struct desk_launch_record){.label="Paint",.target="/Programs/paint",.show=1};
    assert(!fm_shortcut(send,&r,link)); fm_destroy(send); free(send);
    action(FILES_SEND_TO); assert(app.popup_count==7);
    int paint_action=0;
    for (unsigned i=0;i<app.popup_count;i++) if (strstr(app.popup[i].label,"Paint")) paint_action=app.popup[i].id;
    assert(paint_action); unsigned launch_count=launches; action(paint_action); settle();
    assert(launches==launch_count+1 && !strcmp(launched,"/Programs/paint") && !strcmp(argument,"/work/a.txt"));
    assert(!fm_registry_set(".ciuki\\ShellNew","NullFile","")); action(FILES_NEW_REGISTERED);
    assert(app.popup_count==1); action(460); settle();
    assert(fake_lookup("/work/New Document.ciuki")>=0);
    files_destroy(&app); du_release(&ui); wr_destroy(&registry); fm_registry_exchange=NULL; fake_assert_clean(); fake_reset();
    free(pixels); free(reference);
    printf("PASS G8 UI: Copy To/Move To folder browser and cancellation, Undo, SendTo/user folder, create/paste/new shortcuts, Bitmap, Open With once/always, File Types and ShellNew; %u frames equal full redraw\n",frames);
    return 0;
}
