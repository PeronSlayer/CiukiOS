/* SPDX-License-Identifier: MIT -- fx-files-nav, production controller and toolkit. */
#include "fake_vfs.h"
#include "../../../apps/files/app.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void raw_event(struct du_ui *u,const struct du_event *e,void *arg) { files_event(u,e,arg); }
#include "files_wait.h"
static struct files_app app;
static struct du_ui ui;
static uint32_t pixels[760*520];
static uint64_t clock_ns=1000000000ull;
static int opened,closed;
static char opened_path[FM_PATH];
static const char *frame_dir;
static void frame(const char *name)
{
    du_render(&ui);
    uint32_t *before=malloc(sizeof(pixels)); assert(before); memcpy(before,pixels,sizeof(pixels));
    ui.dirty=1; du_render(&ui); assert(!memcmp(before,pixels,sizeof(pixels))); free(before);
    if (!frame_dir) return;
    char path[1200]; snprintf(path,sizeof(path),"%s/%s.xrgb",frame_dir,name);
    FILE *f=fopen(path,"wb"); assert(f); assert(fwrite(pixels,1,sizeof(pixels),f)==sizeof(pixels)); fclose(f);
    snprintf(path,sizeof(path),"%s/%s.size",frame_dir,name); f=fopen(path,"w"); assert(f); fputs("760 520\n",f); fclose(f);
}
static int shell_open(struct files_app *a,const char *path)
{ (void)a; opened++; snprintf(opened_path,sizeof(opened_path),"%s",path); return 0; }
static void close_app(struct files_app *a) { (void)a; closed++; }
enum { BACK=1,FORWARD=2,UP=3,ICONS=18,DETAILS=20,SORT_NAME=21,ADDRESS=100,LIST=107,
       RENAME=109,ARRANGE=360,AUTO=361,LINEUP=362 };
static struct du_widget *widget(int id) { return du_get(&ui,id); }
static void event(unsigned type,unsigned code,int value,int value2)
{
    clock_ns+=10000000ull;
    struct ciuki_input_event e={.type=type,.code=code,.value=value,.value2=value2,.monotonic_ns=clock_ns};
    du_input(&ui,&e); files_test_wait(&app);
}
static void motion(int x,int y) { event(CIUKI_INPUT_MOTION,0,x,y); }
static void button(int down) { event(CIUKI_INPUT_BUTTON,1,down,0); }
static void click(int x,int y) { motion(x,y); button(1); button(0); }
static void key(unsigned code) { event(CIUKI_INPUT_KEY,code,1,0); event(CIUKI_INPUT_KEY,code,0,0); }
static void chord(unsigned mod,unsigned code) { event(CIUKI_INPUT_KEY,mod,1,0); key(code); event(CIUKI_INPUT_KEY,mod,0,0); }
static void activate(int id) { struct du_event e={.type=DU_EV_ACTIVATE,.id=id}; files_event(&ui,&e,&app); }
static void press_widget(int id,int arrow)
{ struct du_rect b=widget(id)->box; click(arrow?b.x+b.w-5:b.x+8,b.y+b.h/2); }
static void start(const char *path)
{ memset(&app,0,sizeof(app)); app.shell_open=shell_open; du_init(&ui,pixels,760,520); assert(!files_setup(&app,&ui,&fake_vfs,path)); }
static void menu_click(const struct du_menu_item *items,unsigned count,int x,int y,int id)
{
    struct du_rect box=du_menu_box(&ui,items,count,x,y); int at=box.y+3;
    for (unsigned i=0;i<count;i++) {
        int h=items[i].flags&DU_ITEM_SEPARATOR?DU_MENU_SEP_H:DU_MENU_ITEM_H;
        if (items[i].id==id && !(items[i].flags&DU_ITEM_SEPARATOR)) { click(box.x+30,at+h/2); return; }
        at+=h;
    }
    assert(!"menu item missing");
}
static void view_pick(int id)
{
    struct du_rect title=du_menu_title(&ui,2); click(title.x+title.w/2,title.y+title.h/2);
    menu_click(app.view_items,app.menus[2].count,title.x,title.y+title.h,id);
}
static void popup_pick(int id) { menu_click(app.popup,app.popup_count,ui.popup_x,ui.popup_y,id); }
static void dialog_pick(unsigned index)
{ struct du_rect b=du_dialog_button(&ui,index); click(b.x+b.w/2,b.y+b.h/2); }
static int find(const char *name)
{ for (unsigned i=0;i<app.model.count;i++) if (!strcmp(app.model.entries[i].name,name)) return (int)i; return -1; }
static void edit_address(const char *text)
{
    du_focus(&ui,ADDRESS); du_set_text(widget(ADDRESS),text);
    struct du_event e={.type=DU_EV_CHANGE,.id=ADDRESS}; files_event(&ui,&e,&app);
}
static void job_wait(void)
{
    for (unsigned i=0;app.job_active && i<5000;i++) {
        struct du_event e={.type=DU_EV_TICK,.id=1}; files_event(&ui,&e,&app);
        struct timespec pause={.tv_nsec=1000000}; nanosleep(&pause,NULL);
    }
    files_test_wait(&app); assert(!app.job_active);
}
static void model_tests(void)
{
    fake_image_tree();
    struct fm_navigation *n=calloc(1,sizeof(*n)),*loaded=calloc(1,sizeof(*loaded)); assert(n && loaded);
    for (unsigned i=0;i<30;i++) { char path[32]; snprintf(path,sizeof(path),"/folder%u",i); fm_address_visit(n,path); }
    assert(n->count==25 && !strcmp(n->addresses[0],"/folder29") && !strcmp(n->addresses[24],"/folder5"));
    fm_address_visit(n,"/FOLDER10"); assert(n->count==25 && !strcmp(n->addresses[0],"/FOLDER10"));
    struct fm_folder_view *v=fm_folder_view(n,"/Documents",0); v->auto_arrange=0; v->widths[0]=287;
    fm_folder_view(n,"/Documents",1)->widths[0]=129;
    fake_short_io=1; assert(!fm_navigation_save(&fake_vfs,n)); assert(!fm_navigation_load(&fake_vfs,loaded));
    assert(!memcmp(n,loaded,sizeof(*n))); fake_short_io=0;
    int record=fake_lookup("/system/settings/files.navigation"); assert(record>=0);
    fake_nodes[record].data[0]='x';
    assert(fm_navigation_load(&fake_vfs,loaded)==-EINVAL && !memcmp(n,loaded,sizeof(*n)));
    assert(!fm_navigation_save(&fake_vfs,n));
    fake_create("/Windows",FM_DIR); fake_create("/Windows/Profiles",FM_DIR);
    fake_create("/Windows/Profiles/alice",FM_DIR); fake_create("/Windows/Profiles/bob",FM_DIR);
    assert(!setenv("USERPROFILE","/Windows/Profiles/alice",1)); assert(!fm_navigation_save(&fake_vfs,n));
    assert(!setenv("USERPROFILE","/Windows/Profiles/bob",1)); assert(fm_navigation_load(&fake_vfs,loaded)==-ENOENT);
    assert(!setenv("USERPROFILE","/Windows/Profiles/alice",1)); assert(!fm_navigation_load(&fake_vfs,loaded));
    assert(loaded->count==25); unsetenv("USERPROFILE");
    struct fm_model *m=calloc(1,sizeof(*m)); assert(m); fm_init(m,&fake_vfs);
    fake_file("/Documents/known.txt",1,0);
    assert(!fm_navigate(m,"/Documents")); int at=-1;
    for (unsigned i=0;i<m->count;i++) if (!strcmp(m->entries[i].name,"known.txt")) { at=(int)i; break; }
    assert(at>=0); assert(fm_extension_changes(m,(unsigned)at,"renamed.zzz"));
    assert(!fm_extension_changes(m,(unsigned)at,"bad/name"));
    m->hide_extensions=1; assert(!fm_extension_changes(m,(unsigned)at,"renamed.zzz"));
    snprintf(m->entries[at].name,FM_NAME,"unknown.zzz");
    assert(fm_extension_changes(m,(unsigned)at,"unknown.bin")); /* unknown extensions remain displayed */
    m->entries[at].meta.type=FM_DIR; assert(!fm_extension_changes(m,(unsigned)at,"unknown.bin"));
    m->hide_extensions=0;
    struct fm_completions *c=calloc(1,sizeof(*c)); assert(c);
    fake_create("/Documents/Alpha",FM_DIR); fake_create("/Documents/Alpine",FM_DIR); fake_file("/Documents/Also.txt",1,0);
    fake_create("/Documents/Alhidden",FM_DIR); fake_nodes[fake_lookup("/Documents/Alhidden")].meta.attributes=CIUKI_ATTR_HIDDEN;
    assert(!fm_complete(m,"C:\\Documents\\al",c)); assert(c->count==3 && strstr(c->paths[0],"Alpha"));
    assert(!fm_complete(m,"C:\\Documents\\Alpha\\",c) && !c->count);
    char resolved[FM_PATH]; int directory=0;
    assert(!fm_address_resolve(m,"c:\\documents\\KNOWN.TXT",resolved,&directory) && !directory && strstr(resolved,"known.txt"));
    for (unsigned i=0;i<30;i++) { char path[64]; snprintf(path,sizeof(path),"/Documents/Prefix%02u",i); fake_create(path,FM_DIR); }
    assert(!fm_complete(m,"C:\\Documents\\pre",c) && c->count==25);
    fm_destroy(m); free(m); free(c); free(n); free(loaded); fake_assert_clean();
    puts("PASS navigation model: 25-entry deduplicated MRU, short I/O, corruption, per-user isolation, independent folder/search widths, prefix/filter/trailing slash, extensions");
}
static void ui_tests(void)
{
    fake_image_tree(); fake_create("/Documents/Alpha",FM_DIR); fake_create("/Documents/Alpine",FM_DIR);
    fake_file("/Documents/one.txt",5,1); fake_file("/Documents/two.bin",4096,2);
    fake_file("/Documents/a very long visible file name.txt",20,1);
    start("/Documents");
    assert(widget(FILES_UNDO)->disabled);
    assert(!files_navigate(&app,"/system")); assert(!files_navigate(&app,"/Programs"));
    press_widget(BACK,1); assert(ui.popup && app.popup_count==2);
    frame("navigation-back-history");
    assert(strstr(app.popup[0].label,"System") && strstr(app.popup[1].label,"Documents"));
    key(DU_KEY_DOWN); key(DU_KEY_DOWN); key(DU_KEY_ENTER);
    assert(!strcmp(app.model.path,"/Documents") && app.model.history_at==0 && app.model.history_count==3);
    press_widget(FORWARD,1); assert(app.popup_count==2);
    /* Mouse-pick the second Forward entry. */
    struct du_rect pb=du_menu_box(&ui,app.popup,app.popup_count,ui.popup_x,ui.popup_y);
    click(pb.x+20,pb.y+3+DU_MENU_ITEM_H+DU_MENU_ITEM_H/2);
    assert(!strcmp(app.model.path,"/Programs") && app.model.history_at==2);
    chord(DU_KEY_LALT,DU_KEY_LEFT); assert(fm_same_path(app.model.path,"/system"));
    assert(!files_navigate(&app,"/Documents/Alpha"));
    assert(app.model.history_count==3 && widget(FORWARD)->disabled);
    du_focus(&ui,LIST); key(DU_KEY_BACKSPACE); assert(!strcmp(app.model.path,"/Documents"));
    key(DU_KEY_F4); assert(ui.combo_open==widget(ADDRESS) && widget(ADDRESS)->count>=4);
    key(DU_KEY_ESC); assert(!ui.combo_open);
    press_widget(ADDRESS,1); assert(ui.combo_open==widget(ADDRESS)); key(DU_KEY_ESC);
    edit_address("C:\\Documents\\al"); assert(ui.combo_open==widget(ADDRESS) && widget(ADDRESS)->count==2);
    frame("navigation-completion");
    key(DU_KEY_DOWN); key(DU_KEY_DOWN); key(DU_KEY_ENTER); assert(!strcmp(app.model.path,"/Documents/Alpine"));
    key(DU_KEY_BACKSPACE); /* now List has focus */
    edit_address("C:\\Documents\\"); assert(ui.combo_open && ui.combo_hot<0);
    key(DU_KEY_ENTER); assert(!strcmp(app.model.path,"/Documents")); /* keep the typed folder */
    edit_address("C:\\Documents\\al"); key(DU_KEY_ESC); assert(!ui.combo_open && !strcmp(app.model.path,"/Documents"));
    /* A key typed with suggestions open filters the list, rather than doing list type-ahead. */
    edit_address("C:\\Documents\\al"); key(0x19); /* p */
    assert(!strcmp(widget(ADDRESS)->text,"C:\\Documents\\alp") && ui.combo_open && widget(ADDRESS)->count==2);
    key(DU_KEY_UP); key(DU_KEY_ENTER); assert(!strcmp(app.model.path,"/Documents/Alpha"));
    assert(!files_navigate(&app,"/Documents"));
    edit_address("c:\\documents\\ONE."); assert(widget(ADDRESS)->count==1);
    key(DU_KEY_DOWN); key(DU_KEY_ENTER);
    assert(opened==1 && !strcmp(opened_path,"/Documents/one.txt") && !strcmp(app.model.path,"/Documents"));
    press_widget(ADDRESS,1); assert(ui.combo_open && ui.combo_hot==0);
    click(widget(ADDRESS)->box.x+30,widget(ADDRESS)->box.y+widget(ADDRESS)->box.h+2+DU_ROW/2);
    assert(opened==2 && !strcmp(opened_path,"/Documents/one.txt"));
    /* Text editing accelerators still take precedence over file operations. */
    edit_address("C:\\Documents\\no-match");
    ui.undo.w=NULL; /* du_set_text above is an external replacement, not a typing run. */
    chord(DU_KEY_LCTRL,0x1e); /* Ctrl+A in the combo selects text, not files */
    assert(widget(ADDRESS)->anchor==0 && widget(ADDRESS)->cursor==strlen(widget(ADDRESS)->text));
    key(0x1e); assert(!strcmp(widget(ADDRESS)->text,"a"));
    chord(DU_KEY_LCTRL,0x2c); assert(!strcmp(widget(ADDRESS)->text,"C:\\Documents\\no-match"));
    key(DU_KEY_ESC);
    /* Resizing and double-clicking divider preserve sorting and folder identity. */
    struct du_widget *list=widget(LIST); int width=list->columns[0].width;
    int divider=list->box.x+2+width,hy=list->box.y+8;
    motion(divider,hy); button(1); motion(divider+47,hy); button(0);
    assert(list->columns[0].width==width+47); int resized=list->columns[0].width;
    assert(!files_navigate(&app,"/system")); assert(!files_navigate(&app,"/Documents")); assert(list->columns[0].width==resized);
    divider=list->box.x+2+resized; clock_ns+=600000000ull;
    click(divider,hy); click(divider,hy);
    int fit=24;
    for (unsigned i=0;i<list->count;i++) { int n=du_text_width(list->items[i].text[0])+30; if (n>fit) fit=n; }
    assert(list->columns[0].width==fit && !app.model.descending);
    activate(ICONS); activate(ARRANGE); assert(ui.popup && app.popup_count==6);
    frame("navigation-arrange");
    key(DU_KEY_UP); key(DU_KEY_ENTER); assert(!fm_folder_view(&app.navigation,app.model.path,0)->auto_arrange);
    int index=find("one.txt"); assert(index>=0); fm_select(&app.model,(unsigned)index,0); files_update(&app);
    char *paths[]={"/Documents/one.txt"},types[]={'F'}; assert(!fm_drag_save(&fake_vfs,paths,types,1));
    struct du_rect body=du_list_body(list);
    files_drag_over(&app,91,0,body.x+body.w-40,body.y+body.h-40,0); assert(app.drag_effect==FM_EFFECT_MOVE);
    files_drag_drop(&app,91,body.x+body.w-40,body.y+body.h-40,FM_EFFECT_MOVE);
    assert(app.model.entries[index].icon_x%76 || app.model.entries[index].icon_y%68);
    frame("navigation-manual-icons");
    struct du_rect label=du_list_label(list,(unsigned)index);
    assert(du_list_hit(list,label.x+label.w/2,label.y+label.h/2)==index);
    activate(LINEUP);
    for (unsigned i=0;i<app.model.count;i++) assert(!(app.model.entries[i].icon_x%76) && !(app.model.entries[i].icon_y%68));
    view_pick(LINEUP); /* mouse path */
    assert(!files_navigate(&app,"/system")); assert(fm_folder_view(&app.navigation,app.model.path,0)->auto_arrange);
    assert(!files_navigate(&app,"/Documents")); assert(!fm_folder_view(&app.navigation,app.model.path,0)->auto_arrange);
    for (int k=0;k<4;k++) { activate(ARRANGE); key(DU_KEY_DOWN); for (int j=0;j<k;j++) key(DU_KEY_DOWN); key(DU_KEY_ENTER); assert(app.model.sort==(unsigned)k); }
    activate(ARRANGE); key(DU_KEY_UP); key(DU_KEY_ENTER); assert(fm_folder_view(&app.navigation,app.model.path,0)->auto_arrange);
    activate(DETAILS);
    index=find("one.txt"); fm_select(&app.model,(unsigned)index,0); files_update(&app); du_focus(&ui,LIST); key(DU_KEY_F2);
    assert(app.rename_index==index); du_set_text(widget(RENAME),"one.bin"); key(DU_KEY_ENTER);
    assert(ui.dialog.kind && !strcmp(ui.dialog.title,"Rename") && strstr(ui.dialog.text,"file may become unusable"));
    frame("navigation-extension-warning");
    assert(fake_lookup("/Documents/one.txt")>=0 && fake_lookup("/Documents/one.bin")<0);
    dialog_pick(1); assert(fake_lookup("/Documents/one.txt")>=0);
    du_focus(&ui,LIST); key(DU_KEY_F2); du_set_text(widget(RENAME),"one.bin"); key(DU_KEY_ENTER); key(DU_KEY_ESC);
    assert(fake_lookup("/Documents/one.txt")>=0);
    du_focus(&ui,LIST); key(DU_KEY_F2); du_set_text(widget(RENAME),"one.bin"); key(DU_KEY_ENTER); dialog_pick(0);
    files_test_wait(&app); assert(fake_lookup("/Documents/one.bin")>=0 && !widget(FILES_UNDO)->disabled);
    press_widget(FILES_UNDO,0); job_wait(); assert(fake_lookup("/Documents/one.txt")>=0 && widget(FILES_UNDO)->disabled);
    /* Case-only extension changes and hidden known extensions do not prompt. */
    index=find("one.txt"); fm_select(&app.model,(unsigned)index,0); files_update(&app); du_focus(&ui,LIST); key(DU_KEY_F2);
    du_set_text(widget(RENAME),"one.TXT"); key(DU_KEY_ENTER); assert(!ui.dialog.kind);
    app.model.hide_extensions=1; files_update(&app); du_focus(&ui,LIST); key(DU_KEY_F2);
    du_set_text(widget(RENAME),"one.bin"); key(DU_KEY_ENTER); assert(!ui.dialog.kind);
    du_focus(&ui,LIST); chord(DU_KEY_LCTRL,0x2c); job_wait(); assert(fake_lookup("/Documents/one.txt")>=0);
    index=find("one.TXT"); assert(index>=0); fm_select(&app.model,(unsigned)index,0); files_update(&app);
    du_focus(&ui,LIST); key(DU_KEY_F2); assert(app.rename_index>=0);
    edit_address("C:\\Documents\\Alpha"); key(DU_KEY_ENTER);
    assert(app.rename_index<0 && !strcmp(app.model.path,"/Documents/Alpha"));
    key(DU_KEY_BACKSPACE);
    activate(ICONS); view_pick(ARRANGE); popup_pick(AUTO);
    assert(!fm_folder_view(&app.navigation,app.model.path,0)->auto_arrange);
    activate(DETAILS);
    widget(LIST)->columns[1].width=153;
    struct du_event resized_event={.type=DU_EV_CHANGE,.id=LIST,.index=1}; raw_event(&ui,&resized_event,&app);
    app.close=close_app; assert(!files_close_request(&app));
    files_test_wait(&app); ui.service(&ui,&app); assert(closed==1);
    files_destroy(&app); start("/Documents"); assert(widget(LIST)->columns[0].width==fit);
    assert(widget(LIST)->columns[1].width==153 && !fm_folder_view(&app.navigation,app.model.path,0)->auto_arrange);
    assert(app.navigation.count && !strcmp(app.navigation.addresses[0],"/Documents"));
    files_destroy(&app); fake_assert_clean();
    puts("PASS navigation UI: mouse/keyboard history and cursor jumps, Backspace, F4/address arrow, completion typing/arrows/Enter/Esc, divider drag/double-click, per-folder widths/reopen, Arrange/Auto/Line Up/manual placement, extension Yes/No, toolbar Undo and disabled state");
}
int main(int argc,char **argv) { frame_dir=argc>1?argv[1]:NULL; model_tests(); ui_tests(); return 0; }
