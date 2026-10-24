/* SPDX-License-Identifier: MIT */
#include "download_store.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define DOWNLOAD_MAX (8u*1024u*1024u)
void ciuki_download_discard(struct ciuki_download_store *s)
{
    if(s->fd>=0)close(s->fd);
    if(s->temporary[0])unlink(s->temporary);
    s->fd=-1;s->temporary[0]=0;s->complete=0;
}
int ciuki_download_begin(struct ciuki_download_store *s,const char *directory,const char *suggested)
{
    *s=(struct ciuki_download_store){.fd=-1};
    int n=snprintf(s->temporary,sizeof(s->temporary),"%s/.netsurf-XXXXXX",directory);
    if(n<0 || (size_t)n>=sizeof(s->temporary)) { s->temporary[0]=0;errno=ENAMETOOLONG;return -1; }
    const char *base=suggested?suggested:"download";
    for(const char *p=base;*p;p++)if(*p=='/' || *p=='\\')base=p+1;
    unsigned used=0;
    for(;*base && used+1<sizeof(s->name);base++) {
        unsigned char c=(unsigned char)*base;
        s->name[used++]=(c<32 || c>=127 || strchr(":*?\"<>|",c))?'_':(char)c;
    }
    while(used && (s->name[used-1]==' ' || s->name[used-1]=='.'))used--;
    s->name[used]=0;
    if(!used || !strcmp(s->name,".") || !strcmp(s->name,".."))strcpy(s->name,"download");
    s->fd=mkstemp(s->temporary);
    if(s->fd<0) { s->temporary[0]=0;return -1; }
    if(fcntl(s->fd,F_SETFD,FD_CLOEXEC)<0) { int e=errno;ciuki_download_discard(s);errno=e;return -1; }
    return 0;
}
static int write_all(int fd,const void *data,size_t bytes)
{
    const char *p=data;
    while(bytes) { ssize_t n=write(fd,p,bytes);if(n<0 && errno==EINTR)continue;if(n<=0) { if(!n)errno=EIO;return -1; }p+=n;bytes-=(size_t)n; }
    return 0;
}
int ciuki_download_write(struct ciuki_download_store *s,const void *data,size_t bytes)
{
    if(s->fd<0 || s->complete) { errno=EINVAL;return -1; }
    if(bytes>DOWNLOAD_MAX-s->bytes) { errno=EFBIG;return -1; }
    if(write_all(s->fd,data,bytes))return -1;
    s->bytes+=bytes;return 0;
}
int ciuki_download_complete(struct ciuki_download_store *s)
{
    if(s->fd<0 || s->complete) { errno=EINVAL;return -1; }
    if(fsync(s->fd))return -1;
    s->complete=1;return 0;
}
int ciuki_download_save(struct ciuki_download_store *s,const char *destination)
{
    if(s->fd<0 || !s->complete || !destination || destination[0]!='/') { errno=EINVAL;return -1; }
    int out=open(destination,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0666);if(out<0)return -1;
    int error=0;
    if(lseek(s->fd,0,SEEK_SET)<0)error=errno;
    char buffer[1024];size_t remaining=s->bytes;
    while(!error && remaining) {
        size_t count=remaining<sizeof(buffer)?remaining:sizeof(buffer);
        ssize_t n=read(s->fd,buffer,count);
        if(n<0 && errno==EINTR)continue;
        if(n<=0) { error=n<0?errno:EIO;break; }
        if(write_all(out,buffer,(size_t)n))error=errno;
        remaining-=(size_t)n;
    }
    if(!error && fsync(out))error=errno;
    if(close(out) && !error)error=errno;
    if(error) { unlink(destination);errno=error;return -1; }
    ciuki_download_discard(s);return 0;
}
