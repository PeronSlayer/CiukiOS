/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "settings_io.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static int entered, released, count, error;
static char stored[CFG_TEXT_MAX];
static int setting_job(void *record, size_t n)
{
    assert(n==sizeof(int)); int value; memcpy(&value,record,n);
    value++; memcpy(record,&value,n); return EACCES;
}
static int write_snapshot(struct cfg_writer *w, const char *text, size_t n)
{
    /* The worker may be blocked on disk while input/UI submits a new snapshot. */
    if (!count) { assert(!pthread_mutex_trylock(&w->lock)); pthread_mutex_unlock(&w->lock); }
    pthread_mutex_lock(&lock); entered=1; pthread_cond_signal(&changed);
    while (!released) pthread_cond_wait(&changed,&lock);
    memcpy(stored,text,n); stored[n]=0; count++;
    int e=error; pthread_mutex_unlock(&lock); return e;
}
static int result(struct cfg_writer *w)
{
    int e=0;
    for (unsigned n=0;n<1000;n++) {
        if (cfg_writer_result(w,&e)) return e;
        struct timespec pause={0,1000000}; nanosleep(&pause,NULL);
    }
    assert(!"worker did not complete within 1 s"); return EIO;
}
int main(void)
{
    struct cfg_writer w={.write=write_snapshot,.run_job=setting_job};
    assert(!cfg_writer_start(&w,"unused","unused"));
    assert(!cfg_writer_submit(&w,"first",5));
    pthread_mutex_lock(&lock); while (!entered) pthread_cond_wait(&changed,&lock); pthread_mutex_unlock(&lock);
    assert(cfg_writer_busy(&w));
    assert(!cfg_writer_submit(&w,"second",6)); assert(!cfg_writer_submit(&w,"latest",6));
    int value=20; assert(!cfg_writer_job(&w,&value,sizeof(value)));
    value=30; assert(!cfg_writer_job(&w,&value,sizeof(value))); value=999;
    pthread_mutex_lock(&lock); released=1; pthread_cond_signal(&changed); pthread_mutex_unlock(&lock);
    assert(!result(&w)); assert(!cfg_writer_busy(&w) && count==2 && !strcmp(stored,"latest"));
    size_t job_length; int job_error;
    assert(cfg_writer_job_result(&w,&value,&job_length,&job_error) && value==31 && job_length==sizeof(value) && job_error==EACCES);
    pthread_mutex_lock(&lock); error=EIO; pthread_mutex_unlock(&lock);
    assert(!cfg_writer_submit(&w,"failed",6)); assert(result(&w)==EIO);
    assert(cfg_writer_submit(&w,"",0)==-1 && errno==ENOSPC);
    assert(cfg_writer_submit(&w,stored,CFG_TEXT_MAX)==-1 && errno==ENOSPC);
    cfg_writer_read(&w);
    size_t length=123; int read_error=0, ready=0;
    for (unsigned i=0;i<1000 && !ready;i++) {
        ready=cfg_writer_read_result(&w,stored,&length,&read_error);
        if (!ready) { struct timespec pause={0,1000000}; nanosleep(&pause,NULL); }
    }
    assert(ready && !length && read_error==ENOENT);
    assert(!cfg_writer_result(&w,&read_error)); /* read errors are independent of write completion */
    cfg_writer_stop(&w);
    puts("PASS settings worker: blocked I/O leaves mailbox/UI available, bounded latest snapshot/device job, copied records, independent read/write/device results, final durable/error result, idle stop");
    return 0;
}
