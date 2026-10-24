/* Private durable storage for Internet preferences. SPDX-License-Identifier: MIT */
#ifndef CIUKI_INTERNET_STORAGE_H
#define CIUKI_INTERNET_STORAGE_H
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "internet_options.h"
static inline int internet_load(const char *path,struct cp_internet_options *out)
{
    internet_defaults(out);FILE *f=fopen(path,"r");if(!f)return errno==ENOENT?0:-1;
    struct cp_internet_options parsed;internet_defaults(&parsed);
    char line[600];unsigned count=0;int bad=0;
    while(fgets(line,sizeof(line),f)) {
        size_t n=strlen(line);if(++count>32 || !n || (line[n-1]!='\n' && !feof(f))) { bad=1;break; }
        while(n && (line[n-1]=='\n' || line[n-1]=='\r'))line[--n]=0;
        if(!n || line[0]=='#')continue;
        char *value=strchr(line,'=');if(!value) { bad=1;break; }*value++=0;
        if(!strcmp(line,"home")) { if(strlen(value)>=sizeof(parsed.home)) { bad=1;break; }strcpy(parsed.home,value); }
        else if(!strcmp(line,"proxy")) { if(strlen(value)>=sizeof(parsed.proxy)) { bad=1;break; }strcpy(parsed.proxy,value); }
        else if(!strcmp(line,"proxy_user") || !strcmp(line,"proxy_password") || !strcmp(line,"trusted") || !strcmp(line,"restricted")) {
            char *dest=!strcmp(line,"proxy_user")?parsed.proxy_user:!strcmp(line,"proxy_password")?parsed.proxy_password:!strcmp(line,"trusted")?parsed.trusted:parsed.restricted;
            size_t cap=!strncmp(line,"proxy_",6)?sizeof(parsed.proxy_user):sizeof(parsed.trusted);
            if(strlen(value)>=cap) { bad=1;break; }strcpy(dest,value);
        }
        else if(!strcmp(line,"scripts")) {
            if(strlen(value)!=4) { bad=1;break; }
            for(unsigned i=0;i<4;i++)if(value[i]!='0' && value[i]!='1')bad=1;else parsed.scripts[i]=(unsigned)(value[i]-'0');
        }
        else if(!strcmp(line,"port") || !strcmp(line,"cache_mb") || !strcmp(line,"history_days") || !strcmp(line,"bypass_local")) {
            char *end;unsigned long number=strtoul(value,&end,10);
            if(!*value || *end || number>65535) { bad=1;break; }
            if(!strcmp(line,"port"))parsed.port=(unsigned)number;else if(!strcmp(line,"cache_mb"))parsed.cache_mb=(unsigned)number;
            else if(!strcmp(line,"history_days"))parsed.history_days=(unsigned)number;else parsed.bypass_local=(unsigned)number;
        } else { bad=1;break; }
    }
    if(ferror(f))bad=1;
    if(fclose(f))bad=1;
    if(bad || !internet_valid(&parsed)) { errno=EINVAL;return -1; }
    *out=parsed;return 0;
}
static inline int internet_save(const char *path,const struct cp_internet_options *o)
{
    if(!internet_valid(o)) { errno=EINVAL;return -1; }
    char temporary[560],data[2048];
    int n=snprintf(temporary,sizeof(temporary),"%s.XXXXXX",path);
    if(n<0 || (size_t)n>=sizeof(temporary)) { errno=ENAMETOOLONG;return -1; }
    n=snprintf(data,sizeof(data),"home=%s\nproxy=%s\nport=%u\ncache_mb=%u\nhistory_days=%u\nproxy_user=%s\nproxy_password=%s\ntrusted=%s\nrestricted=%s\nbypass_local=%u\nscripts=%u%u%u%u\n",o->home,o->proxy,o->port,o->cache_mb,o->history_days,o->proxy_user,o->proxy_password,o->trusted,o->restricted,o->bypass_local,o->scripts[0],o->scripts[1],o->scripts[2],o->scripts[3]);
    if(n<0 || (size_t)n>=sizeof(data)) { errno=EFBIG;return -1; }
    int fd=mkstemp(temporary);if(fd<0)return -1;
    int error=0;size_t used=0;
    while(used<(size_t)n) { ssize_t wrote=write(fd,data+used,(size_t)n-used);if(wrote<0 && errno==EINTR)continue;if(wrote<=0) { error=wrote<0?errno:EIO;break; }used+=(size_t)wrote; }
    if(!error && fsync(fd))error=errno;
    if(close(fd) && !error)error=errno;
    if(!error && rename(temporary,path))error=errno;
    if(error) { unlink(temporary);errno=error;return -1; }
    return 0;
}
#endif
