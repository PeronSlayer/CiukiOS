/* SPDX-License-Identifier: MIT — G20 Control Panel worker adapter. */
#include "../desktop/customize_io.h"
static void menu_work(void *ctx)
{
    struct cp_main *m=ctx;struct cp_menu_result *out=&m->menu_result;
    memset(out,0,sizeof(*out));out->operation=m->menu_edit.operation;
    out->error=m->menu_edit.operation==SH_EDIT_ADD ? sh_shortcut_add(m->menu_edit.destination,m->menu_edit.path,m->menu_edit.name,m->menu_args) : sh_edit_apply(&m->menu_edit);
    struct ciuki_settings *cfg=malloc(sizeof(*cfg));struct sh_folder_snapshot *s=malloc(sizeof(*s));
    if (!cfg || !s) { if (!out->error) out->error=-ENOMEM;free(cfg);free(s);return; }
    cfg_defaults(cfg);sh_snapshot_load(s,cfg);
    size_t prefix=strlen(SH_START_FOLDER);
    for (unsigned k=0;k<s->count && out->count<CP_MENU_MAX;k++) {
        const struct sh_folder_item *e=&s->entries[k];
        if (!strncmp(e->path,SH_START_FOLDER,prefix) && e->path[prefix]=='/') out->entries[out->count++]=*e;
    }
    free(cfg);free(s);
}
static int menu_begin_io(void *ctx,const struct sh_edit_request *r,const char *args)
{
    struct cp_main *m=ctx;
    if (m->menu_job.started) return -EBUSY;
    m->menu_edit=*r;snprintf(m->menu_args,sizeof(m->menu_args),"%s",args ? args : "");
    return cp_job_start(&m->menu_job,menu_work,m,notify_completion,m);
}
static int menu_poll_io(void *ctx,struct cp_menu_result *r)
{
    struct cp_main *m=ctx;if (!cp_job_ready(&m->menu_job)) return 0;
    cp_job_finish(&m->menu_job);*r=m->menu_result;return 1;
}
static int menu_explore(void *ctx,const char *path)
{
    struct cp_main *m=ctx;struct ciuki_message request;
    unsigned n=(unsigned)strlen(path),t=5;
    if (n>CFG_PATH_MAX-1) return -ENAMETOOLONG;
    desk_message(&request,DESK_LAUNCH,16+t+n);
    desk_put32(request.data+4,++m->serial);desk_put32(request.data+8,t);
    memcpy(request.data+12,"files",t);desk_put32(request.data+12+t,n);memcpy(request.data+16+t,path,n);
    return du_window_queue(&m->window,&request) ? -errno : 0;
}
