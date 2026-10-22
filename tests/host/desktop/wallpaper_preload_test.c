/* SPDX-License-Identifier: MIT
 * f2-90: synchronous start-up preload of the seeded native-format frame:
 * exact hit, every miss/corruption falls back (never a partial frame), and the
 * ready time on the host with the T23 device-rate model. */
#define _POSIX_C_SOURCE 200809L
#include "wallpaper_io.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static unsigned fail_after, read_calls; /* fail_after: 1-based read call that returns EIO */
static size_t read_limit;
static uint64_t bytes_read;
ssize_t __real_read(int, void *, size_t);
ssize_t __wrap_read(int fd, void *data, size_t n)
{
    unsigned call=++read_calls;
    if (fail_after && call==fail_after) { errno=EIO; return -1; }
    if (call==2 && !fail_after && read_limit) { errno=EINTR; return -1; }
    ssize_t got=__real_read(fd,data,read_limit && n>read_limit ? read_limit : n);
    if (got>0) bytes_read+=(uint64_t)got;
    return got;
}
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec; }
int main(int argc, char **argv)
{
    assert(argc==3);
    const char *source=argv[1], *dir=argv[2];
    struct desk_wall_request r; memset(&r,0,sizeof(r));
    snprintf(r.path,sizeof(r.path),"%s",source);
    r.width=1024; r.height=768; r.top=r.bottom=32; r.mode=CFG_WALL_FILL; r.colour=0x375564;
    size_t bytes=(size_t)r.width*r.height*4;
    uint32_t *seed=NULL, *got=NULL; int hit=0;
    /* Miss before seeding: absent file, nothing returned. */
    assert(desk_wall_preload(&r,dir,&got,NULL)!=0 && !got);
    assert(desk_wall_preload(&r,NULL,&got,NULL)!=0 && !got);
    /* Seed through the synchronous API (the build's renderer path). */
    assert(!desk_wall_prepare(&r,dir,&seed,&hit,NULL));
    char name[DESK_WALL_NAME_MAX], path[600]; struct stat s; assert(!stat(source,&s));
    desk_wall_cache_name(name,&r,(uint64_t)s.st_size,(int64_t)s.st_mtime);
    snprintf(path,sizeof(path),"%s/%s",dir,name);
    /* Exact hit, timed on the host; the T23 model divides the bytes by the device rate. */
    uint64_t t0=now_ns(); read_calls=0; bytes_read=0;
    assert(!desk_wall_preload(&r,dir,&got,NULL) && got);
    uint64_t host_ns=now_ns()-t0;
    assert(bytes_read==bytes && !memcmp(got,seed,bytes)); free(got); got=NULL;
    printf("PRELOAD host_read_ms=%.2f bytes=%zu reads=%u\n",(double)host_ns/1e6,bytes,read_calls);
    static const struct { const char *name; double mbps; } model[]={
        {"measured-t23-trial-2.8MBps",2.8},{"floor-5MBps",5.0},{"pio4-nominal-16.6MBps",16.6},{"budget-needs",21.0}};
    for (unsigned i=0;i<4;i++)
        printf("MODEL %s ready_ms=%.1f budget_150ms=%s\n",model[i].name,(double)bytes/(model[i].mbps*1e6)*1000.0,
               (double)bytes/(model[i].mbps*1e6)*1000.0<=150.0 ? "met" : "missed");
    /* Short reads and EINTR still give the exact frame. */
    read_limit=4097; read_calls=0;
    assert(!desk_wall_preload(&r,dir,&got,NULL) && !memcmp(got,seed,bytes)); free(got); got=NULL; read_limit=0;
    /* Read error part-way: failure, no frame. */
    for (unsigned at=1;at<=3;at++) { fail_after=at; read_calls=0; assert(desk_wall_preload(&r,dir,&got,NULL)==EIO && !got); }
    fail_after=0;
    /* Corrupt: truncated, oversized, zero length, then absent. */
    assert(!truncate(path,(off_t)bytes-4)); assert(desk_wall_preload(&r,dir,&got,NULL)!=0 && !got);
    assert(!truncate(path,(off_t)bytes+4)); assert(desk_wall_preload(&r,dir,&got,NULL)!=0 && !got);
    assert(!truncate(path,0)); assert(desk_wall_preload(&r,dir,&got,NULL)!=0 && !got);
    assert(!unlink(path)); assert(desk_wall_preload(&r,dir,&got,NULL)!=0 && !got);
    /* The key follows the source identity: a touched source no longer matches the seed. */
    assert(!desk_wall_prepare(&r,dir,&got,&hit,NULL)); free(got); got=NULL;
    assert(!desk_wall_preload(&r,dir,&got,NULL)); free(got); got=NULL;
    struct timespec times[2]={{s.st_mtime+2,0},{s.st_mtime+2,0}};
    assert(!utimensat(AT_FDCWD,source,times,0)); assert(desk_wall_preload(&r,dir,&got,NULL)!=0 && !got);
    free(seed);
    puts("PASS wallpaper preload: exact hit, short reads/EINTR, EIO and corrupt files fall back with no frame, key follows the source");
    return 0;
}
