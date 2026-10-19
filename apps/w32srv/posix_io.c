/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "registry.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __CIUKIOS__
#include <ciuki/runtime.h>
#endif
static char *path(void *o,const char *n)
{
    const char *dir=o;size_t len=strlen(dir)+strlen(n)+2;char *p=malloc(len);if(p)snprintf(p,len,"%s/%s",dir,n);return p;
}
static int read_file(void *o,const char *n,uint8_t **out,size_t *size)
{
    char *p=path(o,n);if(!p)return -1;int fd=open(p,O_RDONLY);free(p);if(fd<0)return errno==ENOENT?0:-1;
    struct stat st;if(fstat(fd,&st)||st.st_size<0||(uint64_t)st.st_size>W32R_TEXT+40){close(fd);*out=NULL;*size=0;return 1;}
    size_t len=(size_t)st.st_size;uint8_t *b=malloc(len?len:1);if(!b){close(fd);return -1;}
    size_t at=0;while(at<len){ssize_t r=read(fd,b+at,len-at);if(r<0&&errno==EINTR)continue;if(r<=0){free(b);close(fd);return -1;}at+=(size_t)r;}
    if(close(fd)){free(b);return -1;}*out=b;*size=len;return 1;
}
static int write_all(int fd,const void *v,size_t n)
{
    const uint8_t *b=v;while(n){ssize_t w=write(fd,b,n);if(w<0&&errno==EINTR)continue;if(w<=0)return -1;b+=w;n-=(size_t)w;}return 0;
}
static int write_file(void *o,const char *n,const void *b,size_t size)
{
    char *p=path(o,n);if(!p)return -1;int fd=open(p,O_WRONLY|O_CREAT|O_TRUNC,0600);free(p);if(fd<0)return -1;
    int e=write_all(fd,b,size);if(close(fd))e=-1;return e;
}
static int patch_file(void *o,const char *n,size_t off,const void *b,size_t size)
{
    char *p=path(o,n);if(!p)return -1;int fd=open(p,O_WRONLY);free(p);if(fd<0)return -1;
    int e=lseek(fd,(off_t)off,SEEK_SET)<0?-1:write_all(fd,b,size);if(close(fd))e=-1;return e;
}
static int sync_file(void *o,const char *n)
{
    char *p=path(o,n);if(!p)return -1;int fd=open(p,O_RDWR);free(p);if(fd<0)return -1;
    int e=fsync(fd);if(close(fd))e=-1;
#ifndef __CIUKIOS__
    if(!e){fd=open(o,O_RDONLY|O_DIRECTORY);if(fd<0)return -1;e=fsync(fd);if(close(fd))e=-1;}
#endif
    return e;
}
static void log_file(void *o,const char *text)
{
    (void)o;
#ifdef __CIUKIOS__
    CU_CALL(DEBUG_WRITE,CU_PTR(text),strlen(text),0,0,0,0);
#else
    fprintf(stderr,"%s\n",text);
#endif
}
void wr_posix_io(struct wr_io *io,const char *dir)
{
    *io=(struct wr_io){(void *)dir,read_file,write_file,patch_file,sync_file,log_file};
}
