/* SPDX-License-Identifier: MIT
 * Control Panel jobs publish private results, then signal the UI wait set.
 * The owner joins before adopting or releasing any worker-owned data. */
#ifndef CIUKI_CONTROL_JOB_H
#define CIUKI_CONTROL_JOB_H
#include <pthread.h>
#include <errno.h>
struct cp_job {
    pthread_t thread;
    void (*work)(void *), (*notify)(void *);
    void *arg, *notify_ctx;
    int started, done;
};
static inline void *cp_job_worker(void *arg)
{
    struct cp_job *job=arg;
    job->work(job->arg);
    __atomic_store_n(&job->done,1,__ATOMIC_RELEASE);
    if (job->notify) job->notify(job->notify_ctx);
    return NULL;
}
static inline int cp_job_start(struct cp_job *job, void (*work)(void *), void *arg,
                               void (*notify)(void *), void *ctx)
{
    if (job->started) return -EBUSY;
    job->work=work; job->arg=arg; job->notify=notify; job->notify_ctx=ctx;
    __atomic_store_n(&job->done,0,__ATOMIC_RELEASE);
    int error=pthread_create(&job->thread,NULL,cp_job_worker,job);
    if (error) return -error;
    job->started=1;
    return 0;
}
static inline int cp_job_ready(struct cp_job *job)
{ return job->started && __atomic_load_n(&job->done,__ATOMIC_ACQUIRE); }
static inline void cp_job_finish(struct cp_job *job)
{
    if (!job->started) return;
    pthread_join(job->thread,NULL); job->started=0;
}
#endif
