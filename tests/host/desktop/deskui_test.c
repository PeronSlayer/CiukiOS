/* SPDX-License-Identifier: MIT */
#include "deskui.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned count; static struct du_event last;
static void event(struct du_ui *u,const struct du_event *e,void *arg) { (void)u;(void)arg; count++; last=*e; }
static void key(struct du_ui *u,int code,int value) { struct ciuki_input_event e={.type=CIUKI_INPUT_KEY,.code=code,.value=value}; du_input(u,&e); }
static void text(struct du_ui *u,int ch) { struct ciuki_input_event e={.type=CIUKI_INPUT_TEXT,.code=ch}; du_input(u,&e); }
static void click(struct du_ui *u,int x,int y) { struct ciuki_input_event e={.type=CIUKI_INPUT_MOTION,.value=x,.value2=y}; du_input(u,&e); e=(struct ciuki_input_event){.type=CIUKI_INPUT_BUTTON,.code=1,.value=1}; du_input(u,&e); e.value=0; du_input(u,&e); }
int main(int argc,char **argv)
{
    uint32_t *pixels=calloc(322*242,4); assert(pixels); pixels[320*240]=0xaabbccddu;
    struct du_ui *u=calloc(1,sizeof(*u)); assert(u); du_init(u,pixels,320,240); u->event=event;
    struct du_widget *button=du_add(u,DU_BUTTON,1,(struct du_rect){4,28,90,28},"Open"); assert(button);
    struct du_widget *edit=du_add(u,DU_EDIT,2,(struct du_rect){4,60,310,28},"/system"); assert(edit);
    struct du_row rows[]={{"Alpha","10","File","2026-10-11",NULL,0,0,0},{"Beta","20","Folder","2026-10-11",NULL,1,0,0},{"Gamma","3","File","2026-10-11",NULL,0,1,1}};
    struct du_widget *list=du_add(u,DU_LIST,3,(struct du_rect){4,94,276,100},""); list->rows=rows; list->count=3;
    assert(du_hit(u,5,30)==0 && du_hit(u,-1,0)==-1); click(u,20,40); assert(last.type==DU_ACTIVATE && last.id==1);
    du_focus(u,2); key(u,0x1d,1); key(u,0x1e,1); key(u,0x1d,0); text(u,'x'); assert(!strcmp(edit->text,"x"));
    text(u,0xe9); key(u,0x0e,1); assert(!strcmp(edit->text,"x"));
    du_clipboard_set("/bin"); key(u,0x1d,1); key(u,0x1e,1); key(u,0x2f,1); key(u,0x1d,0); assert(!strcmp(edit->text,"/bin"));
    assert(du_list_hit(list,10,120)==0 && du_list_hit(list,10,100)==-1);
    du_focus(u,3); key(u,0x150,1); assert(last.type==DU_SELECT && last.index==1);
    key(u,0x2a,1); key(u,0x150,1); assert(last.index==2 && last.modifiers==DU_SHIFT); du_release(u); assert(!u->modifiers && u->capture==-1);
    assert(!du_timer_start(u,7,100,10)); unsigned before=count; du_tick(u,109); assert(count==before); du_tick(u,110); assert(last.type==DU_TICK && last.id==7); du_tick(u,110); assert(count==before+1); du_timer_stop(u,7);
    static const struct du_menu_item menu_items[]={{"Open",9,0,0,0},{"Cancel",10,0,0,0}};
    static const struct du_menu menu[]={ {"File",menu_items,2} }; u->menus=menu; u->menu_count=1;
    click(u,10,10); assert(u->menu==0); key(u,0x150,1); key(u,0x1c,1); assert(last.id==10);
    du_dialog_show(u,DU_INPUT,20,"Name","Enter a filename","New Folder"); text(u,'Z'); assert(!strcmp(u->dialog.edit.text,"Z")); key(u,0x1c,1); assert(!u->dialog.visible && last.id==20 && last.index==0);
    du_add(u,DU_STATUS,4,(struct du_rect){4,210,310,24},"3 items");
    for (int mode=0;mode<3;mode++) { list->view=(enum du_view)mode; du_render(u); assert(pixels[320*240]==0xaabbccddu);
        if (argc>1) { char path[1040]; snprintf(path,sizeof(path),"%s-%d.xrgb",argv[1],mode); FILE *f=fopen(path,"wb"); assert(f); assert(fwrite(pixels,4,320*240,f)==320*240); fclose(f); }
    }
    du_dialog_show(u,DU_JOB,30,"Copying","readme.txt",NULL); u->dialog.progress=1; u->dialog.total=3; du_render(u);
    if (argc>1) { char path[1040]; snprintf(path,sizeof(path),"%s-dialog.xrgb",argv[1]); FILE *f=fopen(path,"wb"); assert(f); fwrite(pixels,4,320*240,f); fclose(f); }
    free(u); free(pixels); puts("PASS deskui: hit tests, edits/UTF-8, keys, menus, dialogs, focus release, timers, deterministic views"); return 0;
}
