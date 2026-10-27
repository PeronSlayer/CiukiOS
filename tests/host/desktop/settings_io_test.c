/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "settings_io.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
int __real_rename(const char *,const char *);
FILE *__real_fopen(const char *,const char *);
size_t __real_fread(void *,size_t,size_t,FILE *);
int __real_ferror(FILE *);
int __real_fclose(FILE *);
static int open_error, partial_read_error, close_error;
static FILE *failed_stream;
FILE *__wrap_fopen(const char *path,const char *mode)
{ if (open_error) { errno=open_error; return NULL; } return __real_fopen(path,mode); }
size_t __wrap_fread(void *p,size_t size,size_t count,FILE *f)
{
    if (partial_read_error) { failed_stream=f; errno=EIO; return __real_fread(p,size,count>8 ? 8 : count,f); }
    return __real_fread(p,size,count,f);
}
int __wrap_ferror(FILE *f) { return f==failed_stream ? 1 : __real_ferror(f); }
int __wrap_fclose(FILE *f)
{ int r=__real_fclose(f); if (f==failed_stream) failed_stream=NULL; if (close_error) { errno=close_error; return -1; } return r; }
static int rename_error;
int __wrap_rename(const char *from,const char *to)
{ if(rename_error){errno=rename_error;return -1;}return __real_rename(from,to); }
static void durable_destination(void)
{
    char directory[]="build/host/settings-target-XXXXXX";assert(mkdtemp(directory));
    char path[512],temporary[512],text[32]={0};
    snprintf(path,sizeof(path),"%s/desktop.cfg",directory);snprintf(temporary,sizeof(temporary),"%s/desktop.new",directory);
    assert(!cfg_write_at(path,temporary,"first\n",6));
    size_t length=123;
    open_error=EIO; assert(cfg_read_at(path,text,sizeof(text),&length)==EIO && !length); open_error=0;
    partial_read_error=1; assert(cfg_read_at(path,text,sizeof(text),&length)==EIO && !length); partial_read_error=0;
    close_error=EIO; assert(cfg_read_at(path,text,sizeof(text),&length)==EIO && !length); close_error=0;
    assert(cfg_read_at(path,text,6,&length)==EFBIG && !length);
    assert(!cfg_read_at(path,text,sizeof(text),&length) && length==6 && !memcmp(text,"first\n",6));
    rename_error=EIO;assert(cfg_write_at(path,temporary,"second\n",7)==EIO);rename_error=0;
    FILE *f=fopen(path,"rb");assert(f&&fread(text,1,sizeof(text),f)==6&&!memcmp(text,"first\n",6));assert(!fclose(f));
    assert(!cfg_write_at(path,temporary,"second\n",7));
    memset(text,0,sizeof(text));f=fopen(path,"rb");assert(f&&fread(text,1,sizeof(text),f)==7&&!memcmp(text,"second\n",7));assert(!fclose(f));
    assert(!unlink(path)&&!rmdir(directory));
    puts("PASS shared snapshot I/O: injected open/partial-read/close EIO and full buffer reject; prior durable file preserved");
}
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static int entered, released, count, error;
static char stored[CFG_TEXT_MAX];
static void contention(struct cfg_writer *w)
{
    pthread_mutex_lock(&w->lock);
    size_t n=0; int error=0, job=0;
    assert(cfg_writer_try_submit(w,"retry",5)==-1 && errno==EAGAIN);
    assert(cfg_writer_job(w,&job,sizeof(job))==-1 && errno==EAGAIN);
    assert(cfg_writer_read(w)==-1 && errno==EAGAIN);
    assert(cfg_writer_log(w,"retry")==-1 && errno==EAGAIN);
    assert(cfg_writer_stage(w,0,0,0)==-1 && errno==EAGAIN);
    assert(!cfg_writer_read_result(w,stored,&n,&error));
    assert(!cfg_writer_job_result(w,&job,&n,&error));
    assert(!cfg_writer_result(w,&error) && cfg_writer_busy(w));
    pthread_mutex_unlock(&w->lock);
}
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
    contention(&w); /* even a suspended mailbox owner cannot block the UI */
    assert(!cfg_writer_submit(&w,"first",5));
    pthread_mutex_lock(&lock); while (!entered) pthread_cond_wait(&changed,&lock); pthread_mutex_unlock(&lock);
    assert(cfg_writer_busy(&w));
    char telemetry[CFG_LOG_MAX]; /* full desktop + demo record survives backlog */
    memset(telemetry,'x',sizeof(telemetry));
    const char prefix[]="[display] frame_p95_us=18446744073709551615 ";
    memcpy(telemetry,prefix,sizeof(prefix)-1);
    telemetry[sizeof(telemetry)-2]='\n'; telemetry[sizeof(telemetry)-1]=0;
    cfg_writer_log(&w,telemetry);
    pthread_mutex_lock(&w.lock);
    assert(w.log_pending && !strcmp(w.log,telemetry));
    pthread_mutex_unlock(&w.lock);
    for (unsigned i=0;i<CFG_STARTUP_STAGES;i++) cfg_writer_stage(&w,i,100+i,i==5 || i==6);
    pthread_mutex_lock(&w.lock); assert(w.stage_pending==((1u<<CFG_STARTUP_STAGES)-1)); pthread_mutex_unlock(&w.lock);
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
    durable_destination();
    puts("PASS settings worker: blocked I/O leaves mailbox/UI available, bounded latest snapshot/device job, copied records, independent read/write/device results, final durable/error result, idle stop");
    return 0;
}
