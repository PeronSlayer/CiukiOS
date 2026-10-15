/* SPDX-License-Identifier: MIT */
#include <unistd.h>
#include <sys/wait.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../apps/libdeskui/internal.h"
#include "../../../apps/libdeskui/textarea.h"
#include "../../../assets/cursors/paint.h"
#include <sys/stat.h>

static struct du_ui ui;
static uint32_t pixels[400*300];
static uint64_t now=1000000000ull;
static unsigned activated;
static void menu_event(struct du_ui *u,const struct du_event *e,void *ctx)
{ (void)u; (void)ctx; activated+=e->type==DU_EV_ACTIVATE; }
static void event(int type,int code,int value,int value2)
{
    struct ciuki_input_event e={.type=(unsigned)type,.code=code,.value=value,.value2=value2,.monotonic_ns=now+=10000000ull};
    du_input(&ui,&e);
}
static void key(unsigned code,unsigned modifiers)
{
    ui.modifiers=modifiers;
    assert(du_widget_key(&ui,&ui.widgets[ui.focus],code));
    ui.modifiers=0;
}
static void click(int x,int y)
{
    event(CIUKI_INPUT_MOTION,0,x,y); event(CIUKI_INPUT_BUTTON,1,1,0); event(CIUKI_INPUT_BUTTON,1,0,0);
}
static void frame(const char *folder,const char *name)
{
    char path[1200]; snprintf(path,sizeof(path),"%s/%s.xrgb",folder,name);
    du_render(&ui); FILE *f=fopen(path,"wb"); assert(f);
    assert(fwrite(pixels,4,400*300,f)==400*300); assert(!fclose(f));
}
static void edits(const char *folder)
{
    du_init(&ui,pixels,400,300);
    struct du_widget *w=du_add(&ui,DU_EDIT,1,(struct du_rect){10,30,280,24},"alpha beta, caf\xc3\xa9");
    du_focus(&ui,1);
    int x=15+du_text_width("alpha ")+3;
    click(x,40); click(x,40); assert(w->anchor==6 && w->cursor==10);
    frame(folder,"selection-field-word");
    click(x,40); assert(w->anchor==0 && w->cursor==strlen(w->text));
    key(0x2e,DU_CTRL); assert(!strcmp(du_clipboard_get(),w->text));
    key(0x2d,DU_CTRL); assert(!w->text[0]);
    key(0x2f,DU_CTRL); assert(!strcmp(w->text,"alpha beta, caf\xc3\xa9"));
    key(DU_KEY_HOME,0); key(DU_KEY_RIGHT,DU_SHIFT); key(DU_KEY_END,DU_SHIFT);
    assert(w->anchor==0 && w->cursor==strlen(w->text));
    key(0x2c,0); assert(!strcmp(w->text,"z") && w->cursor==w->anchor);
    du_set_text(w,"one two three"); now+=1000000000ull;
    x=15+du_text_width("one ")+2;
    click(x,40); event(CIUKI_INPUT_BUTTON,1,1,0);
    event(CIUKI_INPUT_MOTION,0,15+du_text_width("one two three"),40);
    assert(w->anchor==4 && w->cursor==13); event(CIUKI_INPUT_BUTTON,1,0,0);
    frame(folder,"selection-field-drag");
    du_set_text(w,"caf\xc3\xa9 next"); now+=1000000000ull; x=17;
    click(x,40); click(x,40); assert(w->anchor==0 && w->cursor==5);
    key(0x2e,DU_CTRL); assert(!strcmp(du_clipboard_get(),"caf\xc3\xa9"));
    du_set_text(w,"one"); now+=1000000000ull; click(17,40); click(240,40);
    assert(w->anchor==3 && w->cursor==3); /* distant second click is not a word click */
    w->disabled=1; assert(!du_widget_key(&ui,w,DU_KEY_DELETE)); assert(!strcmp(w->text,"one")); w->disabled=0;
    du_input_box(&ui,8,"Rename","Name:","alpha beta");
    struct du_widget *field=NULL;
    for(unsigned i=0;i<ui.dialog.count;i++)if(ui.dialog.items[i].kind==DU_EDIT)field=&ui.dialog.items[i];
    assert(field); now+=1000000000ull;
    x=field->box.x+5+du_text_width("alpha ")+3;
    click(x,field->box.y+8); click(x,field->box.y+8);
    assert(field->anchor==6 && field->cursor==10); frame(folder,"selection-dialog-word");
    du_window_focus(&ui,0); assert(!field->drag && !field->selection.ns); du_dialog_close(&ui);
}
static void textarea(const char *folder)
{
    struct du_textarea ta; assert(!du_ta_init(&ta,8192));
    const char *text="alpha beta\nsecond line\nlast"; assert(!du_ta_set_text(&ta,text,strlen(text)));
    du_init(&ui,pixels,400,300); struct du_widget *w=du_add(&ui,DU_TEXTAREA,1,(struct du_rect){10,30,350,200},"");
    w->data=&ta; du_focus(&ui,1); int x=14+7*DU_TA_CELL_W,y=34+3;
    click(x,y); click(x,y); assert(ta.anchor==6 && ta.cursor==10);
    click(x,y); assert(ta.anchor==0 && ta.cursor==11);
    frame(folder,"selection-textarea-line");
    key(0x2e,DU_CTRL); assert(!strcmp(du_clipboard_get(),"alpha beta\n"));
    key(DU_KEY_END,0); key(DU_KEY_HOME,DU_SHIFT); assert(du_ta_has_selection(&ta));
    key(0x1e,DU_CTRL); key(0x2d,DU_CTRL); assert(!ta.len); key(0x2f,DU_CTRL); assert(!strcmp(ta.text,text));
    key(DU_KEY_UP,DU_SHIFT); assert(du_ta_has_selection(&ta)); key(DU_KEY_DELETE,0);
    assert(ta.len<strlen(text));
    ta.readonly=1; key(0x1e,DU_CTRL); key(0x2d,DU_CTRL); assert(ta.len && !strcmp(du_clipboard_get(),ta.text));
    struct du_menu_item copy={"Copy",99,0x2e,DU_CTRL,0}; struct du_menu menu={"Edit",&copy,1};
    ui.menus=&menu; ui.menu_count=1; ui.event=menu_event;
    event(CIUKI_INPUT_KEY,DU_KEY_LCTRL,1,0); event(CIUKI_INPUT_KEY,0x2e,1,0);
    event(CIUKI_INPUT_KEY,0x2e,0,0); event(CIUKI_INPUT_KEY,DU_KEY_LCTRL,0,0);
    assert(!activated && !strcmp(du_clipboard_get(),ta.text)); /* output owns Copy ahead of an editor menu */
    size_t len=ta.len; key(0x2f,DU_CTRL); key(DU_KEY_DELETE,0); assert(ta.len==len);
    ta.readonly=0; now+=1000000000ull;
    click(x,y); event(CIUKI_INPUT_BUTTON,1,1,0); assert(ta.drag==DU_TA_DRAG_SELECT);
    event(CIUKI_INPUT_RESYNC,0,0,0); assert(ta.drag==DU_TA_DRAG_NONE);
    assert(!du_ta_set_text(&ta,"first\nsecond",12)); du_ta_set_wrap(&ta,1); w->box.h=21;
    now+=1000000000ull; click(18,37); click(18,37); click(18,37);
    assert(ta.rows==1 && ta.anchor==0 && ta.cursor==6 && ta.top==0);
    du_ta_free(&ta);
}
static void lists(const char *folder)
{
    du_init(&ui,pixels,400,300);
    static const struct du_item rows[]={ {.text={"Alpha"}}, {.text={"Beta"}}, {.text={"Gamma"}} };
    struct du_widget *w=du_add(&ui,DU_LISTVIEW,1,(struct du_rect){10,30,250,160},"");
    w->items=rows; w->count=3; w->view=DU_LIST; du_focus(&ui,1);
    struct du_rect r=du_list_label(w,1); click(r.x+3,r.y+3);
    assert(w->active==1 && w->selection_active && !rows[1].selected);
    frame(folder,"selection-list-item");
    key(DU_KEY_DOWN,DU_SHIFT); assert(w->active==2 && w->selection_active);
    frame(folder,"selection-list-keyboard");
}
static void clipboard(const char *folder)
{
    char path[1200]; snprintf(path,sizeof(path),"%s/shared.clip",folder); assert(!du_clipboard_path(path));
    char *long_text=malloc(70001); assert(long_text); memset(long_text,'x',70000); long_text[100]='\n'; long_text[70000]=0;
    assert(!du_clipboard_set_n(long_text,70000)); assert(strlen(du_clipboard_get())==70000);
    pid_t child=fork(); assert(child>=0);
    if(!child) {
        assert(!du_clipboard_path(path)); assert(!strcmp(du_clipboard_get(),long_text));
        assert(!du_clipboard_set_n("child\nclipboard",15)); _exit(0);
    }
    int status; assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(!strcmp(du_clipboard_get(),"child\nclipboard"));
    assert(du_clipboard_set_n("x",DU_CLIPBOARD_MAX+1)<0); assert(!strcmp(du_clipboard_get(),"child\nclipboard"));
    struct du_widget edit={.kind=DU_EDIT}; du_edit_insert(&edit,du_clipboard_get()); assert(!strcmp(edit.text,"child"));
    FILE *f=fopen(path,"wb"); assert(f); assert(fwrite("bad\0text",1,8,f)==8); assert(!fclose(f));
    assert(!strcmp(du_clipboard_get(),"child\nclipboard")); /* reject malformed shared content */
    char saved[1240]; snprintf(saved,sizeof(saved),"%s.saved",path);
    const char *previous=du_clipboard_get();
    assert(!rename(path,saved) && !mkdir(path,0700));
    assert(du_clipboard_set_n("replacement",11)<0 && !strcmp(previous,"child\nclipboard"));
    assert(!rmdir(path) && !rename(saved,path));
    unlink(path); free(long_text); assert(!du_clipboard_path(NULL));
}
int main(int argc,char **argv)
{
    assert(argc==2); edits(argv[1]); textarea(argv[1]); lists(argv[1]); clipboard(argv[1]);
    for(unsigned shape=0;shape<DESK_CURSOR_COUNT;shape++) {
        struct desk_tool_cursor r=desk_tool_cursor(shape); assert(r.hot_x>=0 && r.hot_x<32 && r.hot_y>=0 && r.hot_y<32);
        for(unsigned y=0;y<32;y++)assert(!(r.image[y]&~r.mask[y]));
    }
    puts("selection: PASS fields, dialogs, UTF-8, word/line drag, generic text clipboard, read-only output, list highlight, cross-process publication");
}
