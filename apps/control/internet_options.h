/* Shared Internet page/browser preferences, private source API.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_INTERNET_OPTIONS_H
#define CIUKI_INTERNET_OPTIONS_H
#include <string.h>
#define INTERNET_OPTIONS_PATH "/system/settings/internet.cfg"
struct cp_internet_options {
    char home[512],proxy[254];
    unsigned port,cache_mb,history_days;
    char proxy_user[96],proxy_password[96],trusted[254],restricted[254];
    unsigned bypass_local,scripts[4];
};
static inline int internet_sites_valid(const char *s)
{
    for(unsigned i=0;s[i];i++) {
        unsigned c=(unsigned char)s[i];
        if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='.' || c==';'))return 0;
    }
    return 1;
}
static inline int internet_site_match(const char *list,const char *host)
{
    for(const char *p=list;*p;) {
        const char *h=host,*q=p;int equal=1;
        while(*q && *q!=';') { unsigned a=(unsigned char)*q++,b=(unsigned char)*h;if(b)h++;if(a>='A' && a<='Z')a+=32;if(b>='A' && b<='Z')b+=32;if(a!=b)equal=0; }
        if(equal && !*h)return 1;p=*q?q+1:q;
    }
    return 0;
}
/* Internet=0, local intranet=1, trusted=2, restricted=3. */
static inline unsigned internet_zone(const struct cp_internet_options *o,const char *host)
{
    if(internet_site_match(o->restricted,host))return 3;
    if(internet_site_match(o->trusted,host))return 2;
    if(!strchr(host,'.') || !strncmp(host,"127.",4) || !strncmp(host,"10.",3) || !strncmp(host,"192.168.",8))return 1;
    return 0;
}
static inline void internet_defaults(struct cp_internet_options *o)
{ *o=(struct cp_internet_options){.home="about:welcome",.port=8080,.cache_mb=2,.history_days=20,.scripts={1,1,1,0}}; }
static inline int internet_valid(const struct cp_internet_options *o)
{
    if(!memchr(o->proxy_user,0,sizeof(o->proxy_user)) || !memchr(o->proxy_password,0,sizeof(o->proxy_password)) ||
        !memchr(o->trusted,0,sizeof(o->trusted)) || !memchr(o->restricted,0,sizeof(o->restricted)) ||
        o->history_days>999 || o->bypass_local>1 || !internet_sites_valid(o->trusted) || !internet_sites_valid(o->restricted))return 0;
    for(unsigned i=0;i<4;i++)if(o->scripts[i]>1)return 0;
    for(const unsigned char *p=(const void *)o->proxy_user;*p;p++)if(*p<32 || *p>126 || *p==':')return 0;
    for(const unsigned char *p=(const void *)o->proxy_password;*p;p++)if(*p<32 || *p>126)return 0;
    if(!o->home[0] || !memchr(o->home,0,sizeof(o->home)) || !memchr(o->proxy,0,sizeof(o->proxy)) || !o->port || o->port>65535 || o->cache_mb>16)return 0;
    if(strncmp(o->home,"http://",7) && strncmp(o->home,"https://",8) && strncmp(o->home,"about:",6) && strncmp(o->home,"file:///",8))return 0;
    for(const unsigned char *p=(const void *)o->home;*p;p++)if(*p<=32 || *p==127)return 0;
    for(const unsigned char *p=(const void *)o->proxy;*p;p++)
        if(!((*p>='a' && *p<='z') || (*p>='A' && *p<='Z') || (*p>='0' && *p<='9') || *p=='-' || *p=='.'))return 0;
    return 1;
}
#endif
