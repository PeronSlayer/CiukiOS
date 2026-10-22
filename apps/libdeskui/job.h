/* SPDX-License-Identifier: MIT
 * One owned background job. Only ready/finish publishes worker writes to the
 * UI thread. The work callback must not access a live UI, surface or window;
 * the adapter uses only the immutable notifier installed before start. */
#ifndef CIUKI_DESKUI_JOB_H
#define CIUKI_DESKUI_JOB_H
#include <pthread.h>
#include <errno.h>
#include "deskui.h"
struct du_job {
    pthread_t thread;
    struct du_ui *ui;
    void (*work)(void *);
    void *arg;
    int started, done;
};
static inline void *du_job_worker(void *arg)
{
    struct du_job *job=arg;
    job->work(job->arg);
    __atomic_store_n(&job->done,1,__ATOMIC_RELEASE);
    du_wake(job->ui);
    return NULL;
}
static inline int du_job_start_ui(struct du_job *job, void (*work)(void *), void *arg, struct du_ui *ui)
{
    if (job->started) return -EBUSY;
    job->work=work; job->arg=arg; job->ui=ui;
    __atomic_store_n(&job->done,0,__ATOMIC_RELEASE);
    int e=pthread_create(&job->thread,NULL,du_job_worker,job);
    if (e) return -e;
    job->started=1; return 0;
}
static inline int du_job_start(struct du_job *job, void (*work)(void *), void *arg)
{ return du_job_start_ui(job,work,arg,NULL); }
static inline int du_job_ready(struct du_job *job)
{
    return job->started && __atomic_load_n(&job->done,__ATOMIC_ACQUIRE);
}
static inline void du_job_finish(struct du_job *job)
{
    if (!job->started) return;
    pthread_join(job->thread,NULL); job->started=0;
}
#endif
