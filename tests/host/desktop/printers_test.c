/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include <unistd.h>
#undef _SC_PAGESIZE
#undef _SC_OPEN_MAX
#undef _SC_ARG_MAX
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
#define main printers_program_main
#include "../../../apps/printers/main.c"
#undef main
#include <assert.h>
#include <unistd.h>
static void publish(struct printers *a)
{
    a->config=a->loaded_config;a->count=a->loaded_count;
    memcpy(a->jobs,a->loaded_jobs,a->count*sizeof(a->jobs[0]));
    a->ui.dirty=0;refresh(a);assert(a->ui.dirty);
}
static void click(struct printers *a,int id)
{
    struct du_event e={.type=DU_EV_ACTIVATE,.id=id};
    event(&a->ui,&e,a);assert(a->work.started);du_job_finish(&a->work);publish(a);
}
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    struct printers *a=calloc(1,sizeof(*a));assert(a);uint32_t *pixels=calloc(660*430,4);assert(pixels);
    char tmp[]="/tmp/ciuki-printers-ui-XXXXXX";assert(mkdtemp(tmp));a->root=tmp;
    du_init(&a->ui,pixels,660,430);setup(a);assert(du_get(&a->ui,LIST)->hidden==0 && du_get(&a->ui,NAME)->hidden);
    background(a);publish(a);assert(a->printers==1 && strstr(a->labels[0],"Default"));
    a->ui.dirty=0;struct du_event e={.type=DU_EV_ACTIVATE,.id=ADD};event(&a->ui,&e,a);assert(a->wizard && a->ui.dirty && !du_get(&a->ui,NAME)->hidden && du_get(&a->ui,LIST)->hidden);
    e.id=DRIVER;event(&a->ui,&e,a);assert(a->driver==CP_POSTSCRIPT);event(&a->ui,&e,a);assert(a->driver==CP_PCL);
    du_set_text(du_get(&a->ui,PORT),"relative");e.id=FINISH;event(&a->ui,&e,a);assert(a->wizard && !a->work.started && strstr(du_get(&a->ui,STATUS)->text,"absolute"));
    du_set_text(du_get(&a->ui,NAME),"");du_set_text(du_get(&a->ui,PORT),"/dev/lpt0");event(&a->ui,&e,a);assert(a->wizard && !a->work.started);
    du_set_text(du_get(&a->ui,NAME),"Office Laser");char port[256];snprintf(port,sizeof(port),"%s/output",tmp);du_set_text(du_get(&a->ui,PORT),port);a->driver=CP_POSTSCRIPT;click(a,FINISH);assert(!a->wizard && !a->error && a->printers==2);
    du_get(&a->ui,LIST)->active=1;e.type=DU_EV_SELECT;e.id=LIST;a->ui.dirty=0;event(&a->ui,&e,a);assert(a->selected==1 && a->ui.dirty);
    click(a,DEFAULT);assert(a->config.default_printer==1 && strstr(a->labels[1],"Default"));
    click(a,PRINTER_PAUSE);assert(a->config.printers[1].paused);
    int id=cp_submit_raw(tmp,1,"UI Queue","x",1,NULL,NULL);assert(id>0);a->command=0;background(a);publish(a);assert(du_get(&a->ui,JOBS)->count==1 && !strcmp(a->job_items[0].text[1],"UI Queue"));
    click(a,PAUSE);assert(cp_service_step(tmp)==0);struct cp_job j;assert(!cp_job_read(tmp,id,&j) && j.state==CP_PAUSED);
    click(a,RESUME);assert(cp_service_step(tmp)==0);assert(!cp_job_read(tmp,id,&j) && j.state==CP_QUEUED);
    click(a,CANCEL);assert(!a->error && cp_service_step(tmp)==1);
    assert(cp_submit_raw(tmp,1,"Purge UI","x",1,NULL,NULL)>0);click(a,PURGE);assert(cp_service_step(tmp)==1);
    a->command=0;background(a);publish(a);assert(!du_get(&a->ui,JOBS)->count);
    a->ui.dirty=0;e.type=DU_EV_ACTIVATE;e.id=ADD;event(&a->ui,&e,a);assert(a->wizard && a->ui.dirty);e.id=BACK;a->ui.dirty=0;event(&a->ui,&e,a);assert(!a->wizard && a->ui.dirty);
    char path[256];snprintf(path,sizeof(path),"%s/printers",tmp);unlink(path);assert(!rmdir(tmp));free(pixels);free(a);puts("printers UI: PASS (wizard validation, drivers, default, printer pause, job controls, purge, repaint)");return 0;
}
