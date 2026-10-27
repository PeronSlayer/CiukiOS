/* Fixed protected stores over native FSYNC (data + volume metadata) and the
 * host equivalent (file + directory FSYNC). SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "restart_store.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int path(struct rj_posix *s,const char *name,char out[256])
{
    if(!name||!*name||strchr(name,'/')||strchr(name,'\\')||strstr(name,".."))return -EINVAL;
    int n=snprintf(out,256,"%s/%s",s->directory,name);return n<0||n>=256?-EOVERFLOW:0;
}
static int read_path(const char *path,uint8_t **out,size_t *n,size_t bound)
{
    *out=NULL;*n=0;int fd=open(path,O_RDONLY|O_CLOEXEC);if(fd<0)return -errno;
    struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<0||(uint64_t)st.st_size>bound){close(fd);return -EIO;}
    size_t size=(size_t)st.st_size;uint8_t *v=malloc(size?size:1);if(!v){close(fd);return -ENOMEM;}
    size_t at=0;int e=0;while(at<size){ssize_t r=read(fd,v+at,size-at);if(r<0&&errno==EINTR)continue;
        if(r<=0){e=r<0?-errno:-EIO;break;}at+=(size_t)r;}
    if(close(fd)&&!e)e=-errno;if(e){memset(v,0,size);free(v);return e;}*out=v;*n=size;return 0;
}
static int read_file(void *p,const char *name,uint8_t **out,size_t *n)
{char full[256];int e=path(p,name,full);return e?e:read_path(full,out,n,W32R_TEXT+40u);}
static int transfer(int fd,const void *data,size_t n)
{
    const uint8_t *p=data;while(n){ssize_t bytes=write(fd,p,n);if(bytes<0&&errno==EINTR)continue;
        if(bytes<=0)return bytes<0?-errno:-EIO;p+=bytes;n-=(size_t)bytes;}return 0;
}
static int write_file(void *p,const char *name,const void *data,size_t n)
{char full[256];int e=path(p,name,full);if(e)return e;int fd=open(full,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600);if(fd<0)return -errno;
    e=transfer(fd,data,n);if(close(fd)&&!e)e=-errno;return e;}
static int patch_file(void *p,const char *name,size_t offset,const void *data,size_t n)
{char full[256];int e=path(p,name,full);if(e)return e;int fd=open(full,O_WRONLY|O_CLOEXEC);if(fd<0)return -errno;
    if(lseek(fd,(off_t)offset,SEEK_SET)<0)e=-errno;else e=transfer(fd,data,n);if(close(fd)&&!e)e=-errno;return e;}
static int sync_file(void *p,const char *name)
{
    struct rj_posix *s=p;char full[256];int e=path(s,name,full);if(e)return e;int fd=open(full,O_RDONLY|O_CLOEXEC);if(fd<0)return -errno;
    if(fsync(fd))e=-errno;if(close(fd)&&!e)e=-errno;
#if !defined(__CIUKIOS__) || defined(__linux__)
    if(!e){fd=open(s->directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(fd<0)return -errno;
        if(fsync(fd))e=-errno;if(close(fd)&&!e)e=-errno;}
#endif
    return e;
}
static int move_file(void *p,const char *from,const char *to)
{char a[256],b[256];int e=path(p,from,a);if(!e)e=path(p,to,b);if(e)return e;
    struct stat st;
#if defined(__CIUKIOS__) && !defined(__linux__)
    /* CiukiOS does not implement symlinks/lstat. Both names are generated
     * leaves in a SYSTEM-only anchored directory. */
    if(!stat(b,&st))return -EEXIST;
#else
    if(!lstat(b,&st))return -EEXIST;
#endif
    if(errno!=ENOENT)return -errno;
    return rename(a,b)?-errno:0;}
static int remove_file(void *p,const char *name)
{char full[256];int e=path(p,name,full);return e?e:unlink(full)?-errno:0;}
int rj_posix_io(struct rj_posix *s,const char *directory,struct rj_io *out)
{
    if(!s||!directory||!out||*directory!='/'||strlen(directory)>=sizeof(s->directory)||strstr(directory,"/../")||strstr(directory,"/./"))return -EINVAL;
    memcpy(s->directory,directory,strlen(directory)+1);
    if(mkdir(directory,0700)&&errno!=EEXIST)return -errno;
    *out=(struct rj_io){s,read_file,write_file,patch_file,sync_file,move_file,remove_file};return 0;
}
int rj_posix_legacy(void *unused,unsigned index,uint8_t **out,size_t *n)
{
    (void)unused;static const char *const files[]={"/Windows/CIUKIRST.LOG","/Windows/CIUKIRST.TMP",
        "/Windows/WININIT.INI","/Windows/WININIT.BAK","/Windows/USER.CRG","/Windows/USER.CRG.0",
        "/Windows/USER.CRG.1","/Windows/SYSTEM.CRG.0","/Windows/SYSTEM.CRG.1"};
    if(index>=RJ_LEGACY_COUNT)return -EINVAL;return read_path(files[index],out,n,W32R_TEXT+40u);
}
