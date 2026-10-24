/* G31 controller fixture: real queue/rendering, tiny modal host boundary.
 * SPDX-License-Identifier: MIT */
#include "../../../apps/libprint/dialog.h"
#include <assert.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static struct du_ui ui;
void du_set_text(struct du_widget *w,const char *s) { snprintf(w->text,sizeof(w->text),"%s",s); }
void du_message(struct du_ui *u,int id,const char *title,const char *text,int icon,const char *const *buttons,unsigned count,int def,int cancel)
{
    (void)text;(void)icon;(void)buttons;(void)count;(void)def;
    memset(&u->dialog,0,sizeof(u->dialog));u->dialog.kind=DU_DLG_MESSAGE;
    u->dialog.id=id;u->dialog.count=2;u->dialog.cancel=cancel;
    snprintf(u->dialog.title,sizeof(u->dialog.title),"%s",title);
}
static void complete(struct cp_ui *p,int button)
{
    int id=ui.dialog.id;
    assert(!ui.dialog.accept || !ui.dialog.accept(&ui,button,ui.dialog.accept_ctx));
    ui.dialog.kind=0;
    struct du_event e={.type=DU_EV_DIALOG,.id=id,.index=button};
    assert(cp_ui_event(p,&ui,&e));
}
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);assert(argc==2);
    struct cp_ui p={.root=argv[1]};ui.canvas.width=640;ui.canvas.height=440;
    assert(!cp_init(p.root));
    struct cp_config *cfg=malloc(sizeof(*cfg));assert(cfg && !cp_config_load(p.root,cfg));
    cfg->printers[0].driver=CP_POSTSCRIPT;assert(!cp_config_save(p.root,cfg));free(cfg);
    cp_ui_setup(&p,&ui);struct cp_page before=p.page;
    du_set_text(&ui.dialog.items[3],"0 0");assert(ui.dialog.accept(&ui,0,ui.dialog.accept_ctx));
    assert(!memcmp(&before,&p.page,sizeof(before)));
    du_set_text(&ui.dialog.items[3],"595 842");complete(&p,1);
    assert(!memcmp(&before,&p.page,sizeof(before)));
    cp_ui_setup(&p,&ui);du_set_text(&ui.dialog.items[3],"595 842");
    du_set_text(&ui.dialog.items[6],"18 24 30 36");complete(&p,0);
    assert(p.page.width==595 && p.page.height==842 && p.page.left==18 && p.page.bottom==36);
    char text[]="unsaved text";
    cp_ui_text(&p,&ui,"Unsaved",text,strlen(text));assert(p.snapshot && !memcmp(p.snapshot,text,strlen(text)));
    text[0]='X';assert(((char *)p.snapshot)[0]=='u');complete(&p,1);assert(!p.snapshot && !p.job.started);
    struct cp_job job;assert(cp_job_read(p.root,1,&job));
    cp_ui_text(&p,&ui,"Unsaved","print me",8);
    ui.dialog.items[2].active=9;assert(ui.dialog.accept(&ui,0,ui.dialog.accept_ctx));
    ui.dialog.items[2].active=0;du_set_text(&ui.dialog.items[5],"2 3 1");
    assert(ui.dialog.accept(&ui,0,ui.dialog.accept_ctx));
    du_set_text(&ui.dialog.items[5],"2 0 0");ui.dialog.items[6].value=1;
    du_set_text(&ui.dialog.items[7],"relative.prn");assert(ui.dialog.accept(&ui,0,ui.dialog.accept_ctx));
    du_set_text(&ui.dialog.items[7],"/Documents/test-output.ps");complete(&p,0);
    struct timespec began,now;clock_gettime(CLOCK_MONOTONIC,&began);
    while(!du_job_ready(&p.job)) { sched_yield();clock_gettime(CLOCK_MONOTONIC,&now);assert(now.tv_sec-began.tv_sec<10); }
    ui.dialog.kind=DU_DLG_MESSAGE;cp_ui_poll(&p,&ui);assert(p.job.started); /* preserve a save prompt */
    ui.dialog.kind=0;cp_ui_poll(&p,&ui);assert(!p.job.started && !p.snapshot && p.result>0);
    assert(!cp_job_read(p.root,(unsigned)p.result,&job));assert(job.page.copies==2 && !strcmp(job.title,"Unsaved"));
    assert(!strcmp(job.target,"/Documents/test-output.ps"));
    ui.dialog.kind=0;uint32_t image[]={0xff0000,0x00ff00,0x0000ff,0xffffff};
    cp_ui_image(&p,&ui,"Image",image,2,2);assert(p.image && p.width==2 && p.height==2 && p.bytes==16);
    image[0]=0;assert(((uint32_t *)p.snapshot)[0]==0xff0000);
    p.config->printers[0].driver=CP_TEXT;assert(ui.dialog.accept(&ui,0,ui.dialog.accept_ctx));
    assert(strstr(ui.dialog.items[9].text,"Pictures require"));
    p.config->printers[0].driver=CP_PCL;p.page.width=600;
    assert(ui.dialog.accept(&ui,0,ui.dialog.accept_ctx) && strstr(ui.dialog.items[9].text,"Letter, Legal or A4"));
    complete(&p,1);
    cp_ui_destroy(&p);
    puts("PASS G31 Print/Page Setup: validation, cancel, immutable text/image, async publication, save-prompt preservation");
    return 0;
}
