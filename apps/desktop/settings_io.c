/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "settings_io.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
const char *const cfg_startup_stage_names[CFG_STARTUP_STAGES]={"entry","settings_read","fonts_icons_ready",
    "first_frame_presented","wallpaper_file_read","wallpaper_decoded","wallpaper_scaled",
    "wallpaper_applied","first_input_handled","wallpaper_cache_read"};
int cfg_write_at(const char *path,const char *temporary,const char *text,size_t n)
{
    char parent[512];size_t length=strlen(path);
    if(length>=sizeof(parent))return ENAMETOOLONG;
    memcpy(parent,path,length+1);char *slash=strrchr(parent,'/');
    if(slash && slash!=parent) { *slash=0; if(mkdir(parent,0755) && errno!=EEXIST)return errno; }
    int fd=open(temporary,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0666);if(fd<0)return errno;
    int error=0;size_t done=0;
    while(done<n) {
        ssize_t bytes=write(fd,text+done,n-done);
        if(bytes<0 && errno==EINTR)continue;
        if(bytes<=0) { error=bytes<0 ? errno : EIO;break; }
        done+=(size_t)bytes;
    }
    if(!error && fsync(fd))error=errno;
    if(close(fd) && !error)error=errno;
    if(!error && rename(temporary,path)) {
        error=errno;
        /* Native POSIX rename replaces atomically. Retain the established
         * fallback only for a non-replacing backend's explicit EEXIST. */
        if(error==EEXIST && !unlink(path) && !rename(temporary,path))error=0;
        else if(error==EEXIST)error=errno;
    }
    if(!error) {
        fd=open(path,O_RDONLY|O_CLOEXEC);
        if(fd<0)error=errno;
        else { if(fsync(fd))error=errno;if(close(fd) && !error)error=errno; }
    }
    if(error)unlink(temporary);
    return error;
}
int cfg_write_file(struct cfg_writer *w,const char *text,size_t n)
{ return cfg_write_at(w->path,w->temporary,text,n); }
static void *worker(void *arg)
{
    struct cfg_writer *w=arg;
    /* Heap scratch: CFG_TEXT_MAX can exceed the native stack frame budget. */
    char *text=w->scratch;
    pthread_mutex_lock(&w->lock);
    for (;;) {
        while (!w->pending && !w->log_pending && !w->stage_pending && !w->read_pending && !w->job_pending && !w->stopping) pthread_cond_wait(&w->changed,&w->lock);
        if (!w->pending && !w->log_pending && !w->stage_pending && !w->read_pending && !w->job_pending && w->stopping) break;
        int job=w->job_pending;
        int staging=!job && w->stage_pending;
        unsigned stage=0; uint64_t us=0; int cached=0;
        if (staging) {
            while (!(w->stage_pending&(1u<<stage))) stage++;
            us=w->stage_us[stage]; cached=!!(w->stage_cached&(1u<<stage)); w->stage_pending&=~(1u<<stage);
        }
        int reading=!job && !staging && !w->pending && !w->log_pending;
        unsigned read_serial=w->read_serial;
        int logging=!job && !staging && !w->pending && !reading;
        size_t n=job ? w->job_length : logging ? strlen(w->log) : w->length;
        if (!reading && !staging) memcpy(text,job ? (char *)w->job : logging ? w->log : w->text,n);
        if (job) w->job_pending=0;
        else if (staging) { /* distinct bounded mailbox: startup records never coalesce */ }
        else if (reading) w->read_pending=0;
        else if (logging) w->log_pending=0; else w->pending=0;
        w->busy=1;
        w->activity=job ? 3 : reading ? 2 : logging || staging ? 4 : 1;
        pthread_mutex_unlock(&w->lock);
        int e=0;
        if (staging) {
            n=(size_t)snprintf(text,CFG_TEXT_MAX,"[desktop] stage=%s us=%llu%s\n",cfg_startup_stage_names[stage],(unsigned long long)us,
                cached ? (stage==4 ? " cache_hit=1" : " cache_hit=1 skipped=1") : "");
            e=fwrite(text,1,n,stderr)==n ? 0 : EIO;
        } else if (job) e=w->run_job(text,n);
        else if (reading) {
            FILE *f=fopen(w->path,"rb"); n=0;
            if (!f) e=errno;
            else { n=fread(text,1,CFG_TEXT_MAX,f); if (ferror(f)) e=errno ? errno : EIO; fclose(f); }
        } else e=logging ? (fwrite(text,1,n,stderr)==n ? 0 : EIO) : w->write(w,text,n);
        pthread_mutex_lock(&w->lock);
        w->busy=0; w->activity=0;
        if (job) {
            if (!w->job_pending) { memcpy(w->job,text,n); w->job_length=n; w->job_error=e; w->job_ready=1; }
        } else if (reading) {
            if (read_serial==w->read_serial) {
                memcpy(w->read_text,text,n); w->read_length=n; w->read_error=e; w->read_ready=1;
            }
        }
        else if (!logging && !staging && !w->pending) { w->error=e; w->completed=1; }
        if (w->notify) w->notify(w->notify_ctx);
    }
    w->live=0; pthread_mutex_unlock(&w->lock); free(text); return NULL;
}
int cfg_writer_start(struct cfg_writer *w, const char *path, const char *temporary)
{
    w->path=path; w->temporary=temporary;
    w->scratch=malloc(CFG_TEXT_MAX);
    if (!w->scratch) { errno=ENOMEM; return -1; }
    if (!w->write) w->write=cfg_write_file;
    int e=pthread_mutex_init(&w->lock,NULL); if (e) { free(w->scratch); errno=e; return -1; }
    e=pthread_cond_init(&w->changed,NULL);
    if (e) { free(w->scratch); pthread_mutex_destroy(&w->lock); errno=e; return -1; }
    w->live=1;
    e=pthread_create(&w->thread,NULL,worker,w);
    if (e) { free(w->scratch); w->live=0; pthread_cond_destroy(&w->changed); pthread_mutex_destroy(&w->lock); errno=e; return -1; }
    return 0;
}
static int ui_lock(struct cfg_writer *w)
{
    int e=w->notify ? pthread_mutex_lock(&w->lock) : pthread_mutex_trylock(&w->lock);
    if (e) { errno=e==EBUSY ? EAGAIN : e; return -1; }
    return 0;
}
static int submit(struct cfg_writer *w, const char *text, size_t n, int nonblocking)
{
    if (!n || n>=sizeof(w->text)) { errno=ENOSPC; return -1; }
    if (nonblocking) { if (ui_lock(w)) return -1; }
    else {
        int error=pthread_mutex_lock(&w->lock);
        if (error) { errno=error; return -1; }
    }
    if (!w->live || w->stopping) { pthread_mutex_unlock(&w->lock); errno=EIO; return -1; }
    memcpy(w->text,text,n); w->length=n; w->pending=1; w->completed=0;
    pthread_cond_signal(&w->changed); pthread_mutex_unlock(&w->lock); return 0;
}
int cfg_writer_submit(struct cfg_writer *w, const char *text, size_t n)
{ return submit(w,text,n,0); }
int cfg_writer_try_submit(struct cfg_writer *w, const char *text, size_t n)
{ return submit(w,text,n,1); }
int cfg_writer_busy(struct cfg_writer *w)
{
    if (ui_lock(w)) return 1;
    int busy=w->busy || w->pending || w->log_pending || w->stage_pending || w->read_pending || w->job_pending;
    pthread_mutex_unlock(&w->lock); return busy;
}
int cfg_writer_log(struct cfg_writer *w, const char *text)
{
    if (ui_lock(w)) return -1;
    if (w->live && !w->stopping) {
        snprintf(w->log,sizeof(w->log),"%s",text); w->log_pending=1; pthread_cond_signal(&w->changed);
    }
    pthread_mutex_unlock(&w->lock); return 0;
}
int cfg_writer_stage(struct cfg_writer *w, unsigned stage, uint64_t us, int cached)
{
    if (stage>=CFG_STARTUP_STAGES) { errno=EINVAL; return -1; }
    if (ui_lock(w)) return -1;
    if (w->live && !w->stopping) {
        w->stage_us[stage]=us; w->stage_pending|=1u<<stage;
        if (cached) w->stage_cached|=1u<<stage; else w->stage_cached&=~(1u<<stage);
        pthread_cond_signal(&w->changed);
    }
    pthread_mutex_unlock(&w->lock); return 0;
}
int cfg_writer_result(struct cfg_writer *w, int *error)
{
    if (ui_lock(w)) return 0;
    int ready=w->completed;
    if (ready) { *error=w->error; w->completed=0; }
    pthread_mutex_unlock(&w->lock); return ready;
}
int cfg_writer_read(struct cfg_writer *w)
{
    if (ui_lock(w)) return -1;
    if (!w->live || w->stopping) { pthread_mutex_unlock(&w->lock); errno=EIO; return -1; }
    ++w->read_serial; w->read_ready=0; w->read_pending=1; pthread_cond_signal(&w->changed);
    pthread_mutex_unlock(&w->lock); return 0;
}
int cfg_writer_read_result(struct cfg_writer *w, char *text, size_t *n, int *error)
{
    if (ui_lock(w)) return 0;
    int ready=w->read_ready;
    if (ready) { *n=w->read_length; *error=w->read_error; memcpy(text,w->read_text,*n); w->read_ready=0; }
    pthread_mutex_unlock(&w->lock); return ready;
}
void cfg_writer_stop(struct cfg_writer *w)
{
    pthread_mutex_lock(&w->lock); w->stopping=1;
    pthread_cond_signal(&w->changed); pthread_mutex_unlock(&w->lock);
    pthread_join(w->thread,NULL); pthread_cond_destroy(&w->changed); pthread_mutex_destroy(&w->lock);
}
int cfg_writer_job(struct cfg_writer *w, const void *record, size_t n)
{
    if (!w->run_job || !n || n>sizeof(w->job)) { errno=EINVAL; return -1; }
    if (ui_lock(w)) return -1;
    if (!w->live || w->stopping) { pthread_mutex_unlock(&w->lock); errno=EIO; return -1; }
    memcpy(w->job,record,n); w->job_length=n; w->job_pending=1; w->job_ready=0;
    pthread_cond_signal(&w->changed); pthread_mutex_unlock(&w->lock); return 0;
}
int cfg_writer_job_result(struct cfg_writer *w, void *record, size_t *n, int *error)
{
    if (ui_lock(w)) return 0;
    int ready=w->job_ready;
    if (ready) { *n=w->job_length; *error=w->job_error; memcpy(record,w->job,*n); w->job_ready=0; }
    pthread_mutex_unlock(&w->lock); return ready;
}
