/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "wallpaper_io.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static int read_only;
static unsigned read_calls, write_calls;
static size_t read_limit=512, write_limit=512;
static unsigned full_reads, full_writes;
ssize_t __real_read(int, void *, size_t);
ssize_t __real_write(int, const void *, size_t);
ssize_t __wrap_read(int fd, void *data, size_t n)
{
    assert(n<=65536);
    full_reads+=n==65536;
    if (!read_calls++) { errno=EINTR; return -1; }
    return __real_read(fd,data,read_limit && n>read_limit ? read_limit : n);
}
ssize_t __wrap_write(int fd, const void *data, size_t n)
{
    assert(n<=65536);
    full_writes+=n==65536;
    if (!write_calls++) { errno=EINTR; return -1; }
    return __real_write(fd,data,write_limit && n>write_limit ? write_limit : n);
}
int __real_mkdir(const char *, mode_t);
int __wrap_mkdir(const char *path, mode_t mode)
{
    if (read_only) { errno=EROFS; return -1; }
    return __real_mkdir(path,mode);
}
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static int entered, released, decoded, scaled;
static void slow_stage(const char *name)
{
    pthread_mutex_lock(&lock);
    if (!strcmp(name,"wallpaper_decoded")) decoded++;
    if (!strcmp(name,"wallpaper_scaled")) scaled++;
    if (!strcmp(name,"wallpaper_file_read") && !entered) {
        entered=1; pthread_cond_signal(&changed);
        while (!released) pthread_cond_wait(&changed,&lock);
    }
    pthread_mutex_unlock(&lock);
}
static unsigned raw_count(const char *dir)
{
    DIR *d=opendir(dir); assert(d); struct dirent *e; unsigned count=0;
    while ((e=readdir(d))) if (strstr(e->d_name,".raw")) count++;
    closedir(d); return count;
}
static void stamp(const char *path, time_t seconds)
{
    struct timespec times[2]={{seconds,0},{seconds,0}};
    assert(!utimensat(AT_FDCWD,path,times,0));
}
static void check_keys(struct desk_wall_request r)
{
    char base[DESK_WALL_NAME_MAX], name[DESK_WALL_NAME_MAX];
    desk_wall_cache_name(base,&r,1234,5678);
    printf("KEY %s\n",base);
    desk_wall_cache_name(name,&r,1235,5678); assert(strcmp(base,name));
    desk_wall_cache_name(name,&r,1234,5680); assert(strcmp(base,name));
    struct desk_wall_request other=r;
    other.width++; desk_wall_cache_name(name,&other,1234,5678); assert(strcmp(base,name)); other=r;
    other.height++; desk_wall_cache_name(name,&other,1234,5678); assert(strcmp(base,name)); other=r;
    other.colour++; desk_wall_cache_name(name,&other,1234,5678); assert(strcmp(base,name)); other=r;
    other.mode=CFG_WALL_FIT; desk_wall_cache_name(name,&other,1234,5678); assert(strcmp(base,name)); other=r;
    other.top++; desk_wall_cache_name(name,&other,1234,5678); assert(strcmp(base,name)); other=r;
    strcat(other.path,"x"); desk_wall_cache_name(name,&other,1234,5678); assert(strcmp(base,name));
}
static unsigned cache_stages;
static void cache_stage(const char *name)
{
    assert(!strcmp(name,"wallpaper_cache_read")); cache_stages++;
}
static void bulk_io(struct desk_wall_request r, const char *root)
{
    char dir[512]; snprintf(dir,sizeof(dir),"%s/bulk/wallpaper",root);
    r.width=1024; r.height=768;
    read_limit=0; write_limit=0;
    read_calls=write_calls=1; full_reads=full_writes=0; /* EINTR covered by the short-transfer tests. */
    uint32_t *pixels; int hit;
    assert(!desk_wall_prepare(&r,dir,&pixels,&hit,NULL) && !hit); free(pixels);
    assert(full_reads && full_writes==48 && write_calls==50); /* frame + journal */
    read_calls=write_calls=1; full_reads=full_writes=0;
    assert(!desk_wall_prepare(&r,dir,&pixels,&hit,cache_stage) && hit); free(pixels);
    assert(read_calls==49 && full_reads==48 && write_calls==2 && cache_stages==1);
    printf("WALLPAPER cache_bytes=3145728 request_bytes=65536 cache_read_calls=%u stage=wallpaper_cache_read\n",full_reads);
    r.colour++;
    write_limit=4096; write_calls=1; full_writes=0;
    assert(!desk_wall_prepare(&r,dir,&pixels,&hit,NULL) && !hit); free(pixels);
    assert(full_writes && write_calls==770); /* native writes may still be one page */
    read_limit=write_limit=512;
}
int main(int argc, char **argv)
{
    assert(argc==3);
    struct desk_wall_request r={.width=160,.height=120,.top=32,.bottom=32,.colour=CFG_DESKTOP_COLOR,.mode=CFG_WALL_FILL};
    assert(strlen(argv[1])<sizeof(r.path)); strcpy(r.path,argv[1]); check_keys(r);
    char dir[512]; snprintf(dir,sizeof(dir),"%s/cache/wallpaper",argv[2]);
    uint32_t *pixels; int hit;
    assert(!desk_wall_prepare(&r,dir,&pixels,&hit,NULL) && !hit); free(pixels);
    assert(raw_count(dir)==1);
    assert(!desk_wall_prepare(&r,dir,&pixels,&hit,NULL) && hit); free(pixels);
    struct stat s; assert(!stat(r.path,&s));
    char name[DESK_WALL_NAME_MAX], file[640]; desk_wall_cache_name(name,&r,(uint64_t)s.st_size,s.st_mtime);
    snprintf(file,sizeof(file),"%s/%s",dir,name);
    assert(!truncate(file,4)); /* A truncated cache is decoded again, never published. */
    assert(!desk_wall_prepare(&r,dir,&pixels,&hit,NULL) && !hit); free(pixels);
    stamp(r.path,s.st_mtime+2);
    assert(!desk_wall_prepare(&r,dir,&pixels,&hit,NULL) && !hit); free(pixels);
    r.colour++;
    assert(!desk_wall_prepare(&r,dir,&pixels,&hit,NULL) && !hit); free(pixels);
    r.mode=CFG_WALL_FIT;
    assert(!desk_wall_prepare(&r,dir,&pixels,&hit,NULL) && !hit); free(pixels);
    assert(raw_count(dir)==3);
    /* The most recently USED frame survives another write, including a hit. */
    r.colour--; r.mode=CFG_WALL_FILL;
    assert(!desk_wall_prepare(&r,dir,&pixels,&hit,NULL) && hit); free(pixels);
    r.width++;
    assert(!desk_wall_prepare(&r,dir,&pixels,&hit,NULL) && !hit); free(pixels);
    r.width--;
    assert(!desk_wall_prepare(&r,dir,&pixels,&hit,NULL) && hit); free(pixels);
    assert(raw_count(dir)==3);
    char absent[512]; snprintf(absent,sizeof(absent),"%s/read-only/wallpaper",argv[2]);
    read_only=1;
    assert(!desk_wall_prepare(&r,absent,&pixels,&hit,NULL) && !hit && pixels); free(pixels);
    assert(access(absent,F_OK)==-1 && errno==ENOENT); read_only=0;
    bulk_io(r,argv[2]);
    /* Golden frames are generated with the identical production cache path. */
    for (int mode=0;mode<CFG_WALL_MODES;mode++) {
        r.width=1024; r.height=768; r.mode=mode;
        assert(!desk_wall_prepare(&r,NULL,&pixels,&hit,NULL));
        snprintf(file,sizeof(file),"%s/%s.raw",argv[2],cfg_mode_names[mode]);
        FILE *f=fopen(file,"wb"); assert(f);
        assert(fwrite(pixels,4,1024u*768u,f)==1024u*768u); assert(!fclose(f)); free(pixels);
    }
    struct desk_wall_worker w; assert(!desk_wall_worker_start(&w,NULL,slow_stage));
    r.width=160; r.height=120; unsigned first=desk_wall_worker_submit(&w,&r);
    pthread_mutex_lock(&lock); while (!entered) pthread_cond_wait(&changed,&lock); pthread_mutex_unlock(&lock);
    assert(desk_wall_worker_busy(&w));
    assert(!pthread_mutex_trylock(&w.lock)); pthread_mutex_unlock(&w.lock);
    int error; assert(!desk_wall_worker_take(&w,first,&pixels,&error,&hit));
    r.width++; r.mode=CFG_WALL_FIT; desk_wall_worker_submit(&w,&r);
    r.width++; r.colour++; unsigned latest=desk_wall_worker_submit(&w,&r);
    pthread_mutex_lock(&lock); released=1; pthread_cond_signal(&changed); pthread_mutex_unlock(&lock);
    int ready=0;
    for (unsigned i=0;i<10000 && !ready;i++) {
        assert(!desk_wall_worker_take(&w,first,&pixels,&error,&hit));
        ready=desk_wall_worker_take(&w,latest,&pixels,&error,&hit);
        if (!ready) { struct timespec pause={0,1000000}; nanosleep(&pause,NULL); }
    }
    assert(ready && !error && pixels && !hit && decoded==1); free(pixels);
    /* A colour change blends the retained ARGB layer; a repeated request
     * transfers a private copy of the memory frame without decoding/scaling. */
    assert(scaled==2);
    for (unsigned repeat=0;repeat<2;repeat++) {
        if (!repeat) r.colour++;
        latest=desk_wall_worker_submit(&w,&r); ready=0;
        for (unsigned i=0;i<10000 && !ready;i++) {
            ready=desk_wall_worker_take(&w,latest,&pixels,&error,&hit);
            if (!ready) { struct timespec pause={0,1000000}; nanosleep(&pause,NULL); }
        }
        assert(ready && !error && pixels && hit==(int)repeat && decoded==1 && scaled==2);
        uint32_t *expected; int disk_hit;
        assert(!desk_wall_prepare(&r,NULL,&expected,&disk_hit,NULL));
        assert(!memcmp(pixels,expected,(size_t)r.width*r.height*4)); free(expected); free(pixels);
    }
    stamp(r.path,s.st_mtime+4);
    latest=desk_wall_worker_submit(&w,&r); ready=0;
    for (unsigned i=0;i<10000 && !ready;i++) {
        ready=desk_wall_worker_take(&w,latest,&pixels,&error,&hit);
        if (!ready) { struct timespec pause={0,1000000}; nanosleep(&pause,NULL); }
    }
    assert(ready && !error && !hit && pixels && decoded==2 && scaled==3); free(pixels);
    strcpy(r.path,"/nonexistent-ciuki-wallpaper.png"); latest=desk_wall_worker_submit(&w,&r); ready=0;
    for (unsigned i=0;i<10000 && !ready;i++) {
        ready=desk_wall_worker_take(&w,latest,&pixels,&error,&hit);
        if (!ready) { struct timespec pause={0,1000000}; nanosleep(&pause,NULL); }
    }
    assert(ready && error==ENOENT && !pixels); desk_wall_worker_stop(&w);
    assert(read_calls>100 && write_calls>100);
    puts("PASS wallpaper worker: slow read/decode leaves mailbox free; latest request wins; errors; native I/O bounds, short transfers/EINTR; cache key, truncation, mtime, settings, three-frame LRU and read-only/missing-directory fallback; memory frame/source/layer reuse and source invalidation");
    return 0;
}
