/* Internal stream parser, shared by native libc and the Winsock facade.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_INTERNAL_HOSTS_H
#define CIUKI_INTERNAL_HOSTS_H
#include <stdint.h>
static int cu_hosts_space(char c) { return c==' ' || c=='\t' || c=='\r'; }
static unsigned char cu_hosts_lower(unsigned char c)
{ return c>='A' && c<='Z' ? (unsigned char)(c+'a'-'A') : c; }
static int cu_hosts_line(char *s,const char *name,uint32_t *address)
{
    while(cu_hosts_space(*s))s++;
    uint32_t ip=0;
    for(unsigned i=0;i<4;i++) {
        unsigned n=0,digits=0;
        while(*s>='0' && *s<='9') { if(++digits>3)return 0; n=n*10+(unsigned)(*s++-'0'); }
        if(!digits || n>255 || (i<3 && *s++!='.'))return 0;
        ip|=(uint32_t)n<<(8*i);
    }
    if(!cu_hosts_space(*s))return 0;
    while(*s && *s!='#') {
        while(cu_hosts_space(*s))s++;
        if(!*s || *s=='#')break;
        const char *n=name; int equal=1;
        while(*s && !cu_hosts_space(*s) && *s!='#') {
            if(!*n || cu_hosts_lower((unsigned char)*s)!=cu_hosts_lower((unsigned char)*n))equal=0;
            if(*n)n++;
            s++;
        }
        if(equal && !*n) { *address=ip; return 1; }
    }
    return 0;
}
static int cu_hosts_lookup(int (*next)(void *),void *context,const char *name,uint32_t *address)
{
    char line[1024]; unsigned n=0; int overlong=0;
    for(unsigned total=0;total<=65536;total++) {
        int c=total==65536 ? -1 : next(context);
        if(c<0 || c=='\n') {
            /* A file over the bound must not turn a partial token into a hit. */
            if(!overlong && total!=65536) { line[n]=0; if(cu_hosts_line(line,name,address))return 1; }
            if(c<0)return 0;
            n=0;overlong=0;
        } else if(n+1<sizeof(line))line[n++]=(char)c;
        else overlong=1;
    }
    return 0;
}
#endif
