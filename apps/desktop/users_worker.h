/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_USERS_WORKER_H
#define CIUKI_USERS_WORKER_H
#include "users.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../control/job.h"
struct users_worker {
    struct cp_job job;
    const char *root; /* trusted store prefix; NULL on CiukiOS */
    struct users_request request;
    struct users_view view;
    int error;
    struct ciuki_settings settings;
};
static inline void users_work(void *ctx)
{
    struct users_worker *w=ctx;
    w->error=0;
    if (w->request.operation==USERS_DEFAULT) memset(&w->view,0,sizeof(w->view));
    else w->error=users_execute(w->root ? w->root : "",&w->request,&w->view);
    if (!w->error && (w->request.operation==USERS_AUTH || w->request.operation==USERS_DEFAULT)) {
        struct user_profile p; users_profile(&p,w->request.name,w->view.authenticated,w->view.flags);
        cfg_defaults(&w->settings);
        char path[512]; snprintf(path,sizeof(path),"%s%s",w->root ? w->root : "",p.settings);
        FILE *f=fopen(path,"rb");
        if (f) {
            char *text=malloc(CFG_TEXT_MAX+1);
            if (!text) w->error=-ENOMEM;
            else {
                size_t n=fread(text,1,CFG_TEXT_MAX+1,f);
                if (ferror(f) || n>CFG_TEXT_MAX) w->error=-EIO;
                else cfg_parse(&w->settings,text,n);
                free(text);
            }
            fclose(f);
        } else if (errno!=ENOENT) w->error=-errno;
    }
    users_erase(&w->request,sizeof(w->request));
}
static inline int users_begin(struct users_worker *w,const struct users_request *q,void (*notify)(void *),void *ctx)
{
    if (w->job.started) return -EBUSY;
    w->request=*q;
    int e=cp_job_start(&w->job,users_work,w,notify,ctx);
    if (e) users_erase(&w->request,sizeof(w->request));
    return e;
}
static inline int users_poll(struct users_worker *w,struct users_view *v,int *e)
{
    if (!cp_job_ready(&w->job)) return 0;
    cp_job_finish(&w->job); *v=w->view; *e=w->error; return 1;
}
#endif
