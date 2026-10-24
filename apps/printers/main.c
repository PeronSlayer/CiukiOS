/* SPDX-License-Identifier: MIT */
#include "../libprint/print.h"
#include "../libdeskui/deskui.h"
#include "../libdeskui/job.h"
#include "../accessories/acc.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { LIST=1,JOBS,ADD,DEFAULT,PRINTER_PAUSE,PURGE,PAUSE,RESUME,CANCEL,STATUS,NAME,PORT,DRIVER,FINISH,BACK,INTRO };
struct printers {
    struct du_window window;
    struct du_ui ui;
    struct du_job work;
    const char *root;
    struct cp_config config,loaded_config;
    struct cp_job jobs[CP_JOBS],loaded_jobs[CP_JOBS];
    unsigned loaded_count;
    unsigned ids[CP_JOBS],printer_ids[CP_PRINTERS],count,printers;
    struct du_item printer_items[CP_PRINTERS],job_items[CP_JOBS];
    char labels[CP_PRINTERS][96],job_labels[CP_JOBS][32],states[CP_JOBS][48];
    int command,error,wizard;
    unsigned selected,id,driver;
    char name[64],port[CP_PATH];
};
static void background(void *v)
{
    struct printers *a=v;int e=0;
    if(cp_init(a->root))e=-1;
    if(!e)switch(a->command){
    case DEFAULT:e=cp_set_default(a->root,a->selected);break;
    case PRINTER_PAUSE:e=cp_printer_pause(a->root,a->selected,!a->config.printers[a->selected].paused);break;
    case PURGE:e=cp_purge(a->root,a->selected);break;
    case PAUSE:e=cp_job_control(a->root,a->id,CP_PAUSE);break;
    case RESUME:e=cp_job_control(a->root,a->id,CP_RESUME);break;
    case CANCEL:e=cp_job_control(a->root,a->id,CP_CANCEL);break;
    case FINISH:e=cp_add_printer(a->root,a->name,a->port,a->driver,NULL);break;
    }
    a->error=e?errno:0;
    if(cp_config_load(a->root,&a->loaded_config)){a->error=errno;return;}
    a->loaded_count=0;
    for(unsigned id=1;id<=CP_JOBS;id++)if(!cp_job_read(a->root,id,&a->loaded_jobs[a->loaded_count]))a->loaded_count++;
}
static void schedule(struct printers *a,int command)
{
    if(a->work.started)return;a->command=command;
    int e=du_job_start_ui(&a->work,background,a,&a->ui);
    if(e)du_set_text(du_get(&a->ui,STATUS),"Cannot start print queue worker.");
}
static void refresh(struct printers *a)
{
    a->printers=0;
    for(unsigned i=0;i<CP_PRINTERS;i++)if(a->config.printers[i].present){
        unsigned k=a->printers++;a->printer_ids[k]=i;
        snprintf(a->labels[k],sizeof(a->labels[k]),"%s%s%s",a->config.printers[i].name,i==a->config.default_printer?" (Default)":"",a->config.printers[i].paused?" [Paused]":"");
        a->printer_items[k].text[0]=a->labels[k];a->printer_items[k].text[1]=a->config.printers[i].port;
    }
    struct du_widget *w=du_get(&a->ui,LIST);w->items=a->printer_items;w->count=a->printers;if(w->active>=w->count)w->active=0;
    if(a->printers)a->selected=a->printer_ids[w->active];
    unsigned n=0;
    for(unsigned i=0;i<a->count;i++)if(a->jobs[i].printer==a->selected){
        snprintf(a->job_labels[n],sizeof(a->job_labels[n]),"%u",a->jobs[i].id);
        snprintf(a->states[n],sizeof(a->states[n]),"%s (%u/%u)",cp_state_name(a->jobs[i].state),a->jobs[i].offset,a->jobs[i].bytes);
        a->job_items[n].text[0]=a->job_labels[n];a->job_items[n].text[1]=a->jobs[i].title;a->job_items[n].text[2]=a->states[n];n++;
    }
    w=du_get(&a->ui,JOBS);w->items=a->job_items;w->count=n;if(w->active>=n)w->active=0;
    du_set_text(du_get(&a->ui,STATUS),a->error?strerror(a->error):"Print jobs continue in the background. Offline jobs retry automatically.");
    du_invalidate(&a->ui,(struct du_rect){0,0,660,430});
}
static void wizard(struct printers *a,int show)
{
    a->wizard=show;
    for(int id=LIST;id<=CANCEL;id++){struct du_widget *w=du_get(&a->ui,id);if(w)w->hidden=show;}
    for(int id=NAME;id<=INTRO;id++){struct du_widget *w=du_get(&a->ui,id);if(w)w->hidden=!show;}
    if(show){du_set_text(du_get(&a->ui,NAME),"My Printer");du_set_text(du_get(&a->ui,PORT),"/dev/lpt0");a->driver=CP_TEXT;du_set_text(du_get(&a->ui,DRIVER),"Driver: Generic Text");}
    du_invalidate(&a->ui,(struct du_rect){0,0,660,430});
}
static void event(struct du_ui *u,const struct du_event *e,void *v)
{
    struct printers *a=v;
    if(e->type==DU_EV_TICK){if(!a->wizard)schedule(a,0);return;}
    if(e->type==DU_EV_SELECT && e->id==LIST && !a->work.started){refresh(a);return;}
    if(e->type!=DU_EV_ACTIVATE || a->work.started)return;
    if(e->id==ADD){wizard(a,1);return;}
    if(e->id==BACK){wizard(a,0);return;}
    if(e->id==DRIVER){static const char *const names[]={"Driver: Generic Text","Driver: PostScript","Driver: PCL 5"};a->driver=(a->driver+1)%3;du_set_text(du_get(u,DRIVER),names[a->driver]);return;}
    if(e->id==FINISH){
        const char *name=du_get(u,NAME)->text,*port=du_get(u,PORT)->text;
        if(!*name || strlen(name)>=sizeof(a->name) || port[0]!='/' || strlen(port)>=sizeof(a->port)){du_set_text(du_get(u,STATUS),"Enter a printer name and an absolute file path or /dev/lpt0, /dev/lpt1, /dev/lpt2.");return;}
        strcpy(a->name,name);strcpy(a->port,port);wizard(a,0);schedule(a,FINISH);return;
    }
    struct du_widget *j=du_get(u,JOBS);a->id=j->active<j->count?(unsigned)strtoul(a->job_labels[j->active],NULL,10):0;
    if(e->id>=PAUSE && e->id<=CANCEL && !a->id)return;
    schedule(a,e->id);
}
static void idle(void *v)
{struct printers *a=v;if(du_job_ready(&a->work)){du_job_finish(&a->work);a->config=a->loaded_config;a->count=a->loaded_count;memcpy(a->jobs,a->loaded_jobs,a->count*sizeof(a->jobs[0]));refresh(a);}}
static void setup(struct printers *a)
{
    struct du_ui *u=&a->ui;u->event=event;u->user=a;
    struct du_widget *w=du_add(u,DU_LISTVIEW,LIST,(struct du_rect){12,12,636,100},"");w->view=DU_DETAILS;w->column_count=2;w->columns[0]=(struct du_column){"Printer",280,0};w->columns[1]=(struct du_column){"Port / output file",330,0};
    du_add(u,DU_BUTTON,ADD,(struct du_rect){12,120,116,26},"Add Printer...");
    du_add(u,DU_BUTTON,DEFAULT,(struct du_rect){136,120,120,26},"Set as Default");
    du_add(u,DU_BUTTON,PRINTER_PAUSE,(struct du_rect){264,120,164,26},"Pause / Resume Printer");
    du_add(u,DU_BUTTON,PURGE,(struct du_rect){436,120,140,26},"Purge Queue");
    w=du_add(u,DU_LISTVIEW,JOBS,(struct du_rect){12,158,636,174},"");w->view=DU_DETAILS;w->column_count=3;w->columns[0]=(struct du_column){"Job",48,0};w->columns[1]=(struct du_column){"Document",300,0};w->columns[2]=(struct du_column){"Status / bytes",260,0};
    du_add(u,DU_BUTTON,PAUSE,(struct du_rect){12,342,100,26},"Pause Job");du_add(u,DU_BUTTON,RESUME,(struct du_rect){120,342,100,26},"Resume Job");du_add(u,DU_BUTTON,CANCEL,(struct du_rect){228,342,100,26},"Cancel Job");
    du_add(u,DU_LABEL,STATUS,(struct du_rect){12,382,636,34},"Reading print queue...");
    du_add(u,DU_EDIT,NAME,(struct du_rect){12,74,450,26},"");du_add(u,DU_EDIT,PORT,(struct du_rect){12,132,610,26},"");
    du_add(u,DU_BUTTON,DRIVER,(struct du_rect){12,190,220,28},"");du_add(u,DU_BUTTON,FINISH,(struct du_rect){12,250,100,28},"Finish");du_add(u,DU_BUTTON,BACK,(struct du_rect){124,250,100,28},"Cancel");
    du_add(u,DU_LABEL,INTRO,(struct du_rect){12,16,626,44},"Add Printer: enter a name, choose the output port or file, then choose a driver.\nUse the driver button to select Generic Text, PostScript or PCL 5.");
    wizard(a,0);
}
int main(int argc,char **argv)
{
    int channel,test;const char *path;if(acc_args(argc,argv,"printers",&channel,&test,&path)||path)return 2;
    struct printers *a=calloc(1,sizeof(*a));if(!a)return 1;
    if(du_window_open(&a->window,&a->ui,channel,660,430,"Printers")){free(a);return 1;}
    setup(a);
    wizard(a,0);du_timer_start(&a->ui,1,1000000000ull,1);schedule(a,0);
    int r=du_run(&a->window,idle,a);du_job_finish(&a->work);du_window_destroy(&a->window);free(a);return r<0;
}
