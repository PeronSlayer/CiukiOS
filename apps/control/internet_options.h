/* Shared Internet page/browser preferences, private source API.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_INTERNET_OPTIONS_H
#define CIUKI_INTERNET_OPTIONS_H
#include <string.h>
#define INTERNET_OPTIONS_PATH "/system/settings/internet.cfg"
struct cp_internet_options { char home[512],proxy[254]; unsigned port,cache_mb; };
static inline void internet_defaults(struct cp_internet_options *o)
{ *o=(struct cp_internet_options){.home="about:welcome",.port=8080,.cache_mb=2}; }
static inline int internet_valid(const struct cp_internet_options *o)
{
    if(!o->home[0] || !memchr(o->home,0,sizeof(o->home)) || !memchr(o->proxy,0,sizeof(o->proxy)) || !o->port || o->port>65535 || o->cache_mb>16)return 0;
    if(strncmp(o->home,"http://",7) && strncmp(o->home,"https://",8) && strncmp(o->home,"about:",6) && strncmp(o->home,"file:///",8))return 0;
    for(const unsigned char *p=(const void *)o->home;*p;p++)if(*p<=32 || *p==127)return 0;
    for(const unsigned char *p=(const void *)o->proxy;*p;p++)
        if(!((*p>='a' && *p<='z') || (*p>='A' && *p<='Z') || (*p>='0' && *p<='9') || *p=='-' || *p=='.'))return 0;
    return 1;
}
#endif
