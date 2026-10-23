/* Native VFS/configuration adapter. Restricted metadata devctls retain a
 * real description and mount/chain generation throughout raw verification.
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"
#include <ciuki/abi.h>
#pragma clang diagnostic pop
#include "../../apps/control/configuration.h"
#include "../../apps/desktop/settings_io.h"
int ciuki_devctl(int,uint32_t,void *,uint32_t);
static int map_fd=-1;
static char map_path[272];
static struct ciuki_setup_fat map_record;
void st_native_cleanup(void) { if (map_fd>=0) close(map_fd); map_fd=-1; }
static int path_of(char *out,size_t cap,char drive,const char *rel)
{
    if (drive<'C' || drive>'Z' || !st_path_valid(rel)) return -EINVAL;
    int n=drive=='C' ? snprintf(out,cap,"/%s",rel) : snprintf(out,cap,"/mnt/%c/%s",drive-'A'+'a',rel);
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
static int query(int h,struct ciuki_setup_fat *q)
{ return ciuki_devctl(h,CIUKI_SETUP_FAT_QUERY,q,sizeof(*q))<0 ? -errno : 0; }
static int n_info(void *ctx,char drive,const char *rel,struct st_info *out)
{
    int h=n_open(ctx,drive,rel,false); if(h<0)return h;
    struct ciuki_setup_fat q={.version=CIUKI_SETUP_FAT_VERSION,.size=sizeof(q),.cluster_index=CIUKI_SETUP_INDEX_NONE};
    int e=query(h,&q);
    if(e==-ENOTSUP && drive=='D') {
        struct stat s;
        if(fstat(h,&s))e=-errno;
        else { q.file_size=(uint64_t)s.st_size; q.attributes=S_ISDIR(s.st_mode) ? 16 : 32;
            const char *base=strrchr(rel,'/'); snprintf(q.name,sizeof(q.name),"%s",base ? base+1 : rel); e=0; }
    }
    if(!e && drive!='D' && !q.alias[0])e=-EIO;
    if(!e) { memset(out,0,sizeof(*out)); out->size=q.file_size; out->attr=(uint8_t)q.attributes; memcpy(out->name,q.name,sizeof(out->name)); memcpy(out->alias,q.alias,sizeof(out->alias)); }
    int c=n_close(ctx,h); return e ? e : c;
}
static int n_mkdir(void *ctx,char drive,const char *rel)
{
    (void)ctx; char path[272]; int e=path_of(path,sizeof(path),drive,rel); if(e)return e;
    return mkdir(path,0777) ? -errno : 0;
}
static int n_attrib(void *ctx,char drive,const char *path,uint8_t attr)
{
    int h=n_open(ctx,drive,path,false); if(h<0)return h;
    struct ciuki_setup_fat q={.version=CIUKI_SETUP_FAT_VERSION,.size=sizeof(q),.cluster_index=CIUKI_SETUP_INDEX_NONE};
    int e=query(h,&q);
    if(!e) { q.attributes=attr; e=ciuki_devctl(h,CIUKI_SETUP_FAT_ATTRIB,&q,sizeof(q))<0 ? -errno : 0; }
    int c=n_close(ctx,h); return e ? e : c;
}
static int n_cluster(void *ctx,char drive,const char *path,uint32_t index,uint64_t *lba)
{
    char full[272]; int e=path_of(full,sizeof(full),drive,path); if(e)return e;
    if(map_fd<0 || strcmp(full,map_path) || !index) {
        st_native_cleanup(); map_fd=n_open(ctx,drive,path,false); if(map_fd<0)return map_fd;
        memcpy(map_path,full,strlen(full)+1);
        map_record=(struct ciuki_setup_fat){.version=CIUKI_SETUP_FAT_VERSION,.size=sizeof(map_record),.cluster_index=CIUKI_SETUP_INDEX_NONE};
        e=query(map_fd,&map_record);
    }
    if(!e) { map_record.cluster_index=index; e=query(map_fd,&map_record); }
    if(!e) { if(map_record.cluster_sectors!=DISK_SPC)e=-EINVAL; else *lba=map_record.lba; }
    if(e)st_native_cleanup();
    return e;
}
struct configure_context { char drive; };
static int config_read(void *ctx,const char *path,void *out,unsigned cap,unsigned *bytes)
{
    struct configure_context *c=ctx; int h=n_open(NULL,c->drive,path,false); if(h<0)return h;
    unsigned n=0; int e=0;
    while(n<cap) { int r=n_read(NULL,h,(char *)out+n,cap-n); if(r<0){e=r;break;} if(!r)break; n+=(unsigned)r; }
    if(!e && n==cap) { char extra; int r=n_read(NULL,h,&extra,1); if(r)e=r<0 ? r : -E2BIG; }
    int close_error=n_close(NULL,h); *bytes=n; return e ? e : close_error;
}
static int config_write(void *ctx,const char *path,const void *data,unsigned bytes)
{
    struct configure_context *c=ctx;char full[272],temporary[288];
    int e=path_of(full,sizeof(full),c->drive,path);if(e)return e;
    int n=snprintf(temporary,sizeof(temporary),"%s.new",full);
    if(n<0 || (size_t)n>=sizeof(temporary))return -ENAMETOOLONG;
    return -cfg_write_at(full,temporary,data,bytes);
}
static int config_mkdir(void *ctx,const char *path)
{ struct configure_context *c=ctx; return n_mkdir(NULL,c->drive,path); }
static int config_network(void *ctx,struct net_config_identity ids[2])
{
    (void)ctx; memset(ids,0,sizeof(*ids)*2);
    int h=open("/dev/net",O_RDONLY|O_CLOEXEC); if(h<0)return -errno;
    struct ciuki_setup_net q={.version=CIUKI_SETUP_FAT_VERSION,.size=sizeof(q)};
    int e=ciuki_devctl(h,CIUKI_SETUP_NET_QUERY,&q,sizeof(q))<0 ? -errno : 0;
    if(close(h) && !e)e=-errno;
    if(!e && q.count>2)e=-EOVERFLOW;
    if(!e)for(unsigned i=0;i<q.count;i++) {
        ids[i].vendor=q.identity[i].vendor; ids[i].device=q.identity[i].device; memcpy(ids[i].mac,q.identity[i].mac,6);
    }
    return e;
}
static int config_clock(void *ctx,int64_t seconds,int16_t offset,bool local)
{
    struct configure_context *c=ctx; int h=n_open(NULL,c->drive,"system/settings/clock.cfg",false); if(h<0)return h;
    struct ciuki_setup_clock q={.version=CIUKI_SETUP_FAT_VERSION,.size=sizeof(q),.seconds=seconds,.offset_minutes=offset,.rtc_local=local};
    int e=ciuki_devctl(h,CIUKI_SETUP_CLOCK_SET,&q,sizeof(q))<0 ? -errno : 0; int close_error=n_close(NULL,h);
    return e ? e : close_error;
}
static int64_t config_now(void *ctx) { (void)ctx; return (int64_t)time(NULL); }
static int n_configure(void *ctx,char drive,const struct setup_answers *a)
{
    (void)ctx; struct configure_context c={drive};
    struct cp_configuration_store s={&c,config_read,config_write,config_mkdir,config_network,config_clock,config_now};
    return cp_configuration_write(&s,a);
}
static int preflight(void *ctx,const struct setup_answers *a)
{
    (void)a; st_native_cleanup(); struct st_info info;
    int e=n_info(ctx,'C',"system/BOOT.CFG",&info);
    struct net_config_identity ids[2];
    return e ? e : config_network(NULL,ids);
}
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
