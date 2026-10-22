/* SPDX-License-Identifier: MIT
 * One owned background job. Only poll/finish publishes worker writes to the
 * UI thread. The worker must not access a live UI, surface or window. */
#ifndef CIUKI_DESKUI_JOB_H
#define CIUKI_DESKUI_JOB_H
#include <pthread.h>
#include <errno.h>
struct du_job {
    pthread_t thread;
    pthread_mutex_t lock;
    void (*work)(void *);
    void *arg;
    int started, done;
};
static inline void *du_job_worker(void *arg)
{
    struct du_job *job=arg;
    job->work(job->arg);
    pthread_mutex_lock(&job->lock); job->done=1; pthread_mutex_unlock(&job->lock);
    return NULL;
}
static inline int du_job_start(struct du_job *job, void (*work)(void *), void *arg)
{
    if (job->started) return -EBUSY;
    int e=pthread_mutex_init(&job->lock,NULL); if (e) return -e;
    job->work=work; job->arg=arg; job->done=0;
    e=pthread_create(&job->thread,NULL,du_job_worker,job);
    if (e) { pthread_mutex_destroy(&job->lock); return -e; }
    job->started=1; return 0;
}
static inline int du_job_ready(struct du_job *job)
{
    if (!job->started || pthread_mutex_trylock(&job->lock)) return 0;
    int done=job->done; pthread_mutex_unlock(&job->lock); return done;
}
static inline void du_job_finish(struct du_job *job)
{
    if (!job->started) return;
    pthread_join(job->thread,NULL); pthread_mutex_destroy(&job->lock); job->started=0;
}
#endif
