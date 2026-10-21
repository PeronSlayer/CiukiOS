/* Strict dotted IPv4 only. SPDX-License-Identifier: MIT */
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
uint16_t htons(uint16_t v) { return __builtin_bswap16(v); }
uint16_t ntohs(uint16_t v) { return __builtin_bswap16(v); }
uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }
int inet_pton(int family,const char *text,void *out)
{
    if (family!=AF_INET) { errno=EAFNOSUPPORT; return -1; }
    if (!text || !out) { errno=EFAULT; return -1; }
    unsigned char bytes[4]; const char *p=text;
    for (unsigned i=0;i<4;i++) {
        unsigned n=0, digits=0;
        while (*p>='0' && *p<='9') {
            if (++digits>3 || (digits>1 && p[-1]=='0' && n==0)) return 0;
            n=n*10+(unsigned)(*p++-'0'); if (n>255) return 0;
        }
        if (!digits || (i<3 ? *p++!='.' : *p!=0)) return 0;
        bytes[i]=(unsigned char)n;
    }
    memcpy(out,bytes,4); return 1;
}
const char *inet_ntop(int family,const void *address,char *out,socklen_t capacity)
{
    if (family!=AF_INET) { errno=EAFNOSUPPORT; return NULL; }
    if (!address || !out) { errno=EFAULT; return NULL; }
    const unsigned char *p=address; char text[16];
    int n=snprintf(text,sizeof(text),"%u.%u.%u.%u",p[0],p[1],p[2],p[3]);
    if ((unsigned)n>=capacity) { errno=ENOSPC; return NULL; }
    memcpy(out,text,(unsigned)n+1); return out;
}
int inet_aton(const char *text,struct in_addr *out) { return inet_pton(AF_INET,text,out)==1; }
in_addr_t inet_addr(const char *text) { struct in_addr a; return inet_aton(text,&a) ? a.s_addr : INADDR_NONE; }
char *inet_ntoa(struct in_addr a) { static char text[16]; return (char *)inet_ntop(AF_INET,&a,text,sizeof(text)); }
