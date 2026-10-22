/* Native POSIX side of libsetup. Missing facilities are refused by preflight;
 * do not fall back to cached verification or invent RTC/network formats.
 * See docs/validation/f9-04-libsetup.md for the required lead interfaces.
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static int preflight(void *ctx,const struct setup_answers *a)
{ (void)ctx; (void)a; return -ENOTSUP; }
static int path_of(char *out,size_t cap,char drive,const char *rel)
{
    int n=snprintf(out,cap,"/mnt/%c/%s",drive,rel);
    return n<0 || (size_t)n>=cap ? -ENAMETOOLONG : 0;
}
static int n_open(void *ctx,char drive,const char *rel,bool write_access)
{
    (void)ctx; char path[272]; int e=path_of(path,sizeof(path),drive,rel); if(e)return e;
    int h=open(path,(write_access ? O_WRONLY|O_CREAT|O_TRUNC : O_RDONLY)|O_CLOEXEC,0666);
    return h<0 ? -errno : h;
}
static int n_read(void *ctx,int h,void *p,unsigned n) { (void)ctx; ssize_t r=read(h,p,n); return r<0 ? -errno : (int)r; }
static int n_write(void *ctx,int h,const void *p,unsigned n) { (void)ctx; ssize_t r=write(h,p,n); return r<0 ? -errno : (int)r; }
static int n_seek(void *ctx,int h,uint64_t n) { (void)ctx; return lseek(h,(off_t)n,SEEK_SET)<0 ? -errno : 0; }
static int n_sync(void *ctx,int h) { (void)ctx; return fsync(h) ? -errno : 0; }
static int n_close(void *ctx,int h) { (void)ctx; return close(h) ? -errno : 0; }
static int n_info(void *ctx,char drive,const char *path,struct st_info *out)
{ (void)ctx; (void)drive; (void)path; (void)out; return -ENOTSUP; }
static int n_mkdir(void *ctx,char drive,const char *rel)
{
    (void)ctx; char path[272]; int e=path_of(path,sizeof(path),drive,rel); if(e)return e;
    return mkdir(path,0777) ? -errno : 0;
}
static int n_attrib(void *ctx,char drive,const char *path,uint8_t attr)
{ (void)ctx; (void)drive; (void)path; (void)attr; return -ENOTSUP; }
static int n_cluster(void *ctx,char drive,const char *path,uint32_t index,uint64_t *lba)
{ (void)ctx; (void)drive; (void)path; (void)index; (void)lba; return -ENOTSUP; }
static int n_configure(void *ctx,char drive,const struct setup_answers *a)
{ (void)ctx; (void)drive; (void)a; return -ENOTSUP; }
int st_native_media(void *,bool,bool *,uint32_t *);
int st_native_restart(void *);
static uint64_t n_ms(void *ctx)
{ (void)ctx; struct timespec t={0}; if(clock_gettime(CLOCK_MONOTONIC,&t))return 0; return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000; }
static void n_sleep(void *ctx,unsigned ms)
{ (void)ctx; struct timespec t={.tv_sec=ms/1000,.tv_nsec=(long)(ms%1000)*1000000}; while(nanosleep(&t,&t) && errno==EINTR) {} }
const struct st_platform *st_native_platform(void)
{
    static const struct st_platform p={NULL,preflight,n_open,n_read,n_write,n_seek,n_sync,n_close,n_info,n_mkdir,n_attrib,n_cluster,n_configure,st_native_media,n_ms,n_sleep,st_native_restart};
    return &p;
}
