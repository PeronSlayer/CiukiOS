/* SPDX-License-Identifier: MIT
 * G15B arguments carried by existing DESK_LAUNCH. No new wire opcode.
 * hlp:<command>:<context>:<path-byte-count>:<path><keyword-or-macro>
 * chm: uses the HTML Help command numbers. All fields fit DESK_ARG_MAX. */
#ifndef CIUKI_HELP_REQUEST_H
#define CIUKI_HELP_REQUEST_H
#include <stdint.h>
#include <stddef.h>
struct desk_help_request {
    unsigned format, command;
    uint32_t context;
    char path[161], query[161];
};
static inline int desk_help_number(const char **at,uint32_t *out)
{
    const char *p=*at; uint32_t n=0; unsigned digits=0;
    while (*p>='0' && *p<='9') {
        unsigned digit=(unsigned)(*p++-'0'); if (n>(UINT32_MAX-digit)/10) return 0;
        n=n*10+digit; digits++;
    }
    if (!digits || *p++!=':') return 0;
    *at=p; *out=n; return 1;
}
static inline int desk_help_decode(const char *arg,struct desk_help_request *r)
{
    if (!arg || !r) return 0;
    size_t len=0; while (arg[len]) { if (len==160 || (unsigned char)arg[len]<32 || arg[len]==127) return 0; len++; }
    if (len<4 || arg[3]!=':') return 0;
    if (arg[0]=='h' && arg[1]=='l' && arg[2]=='p') r->format=1;
    else if (arg[0]=='c' && arg[1]=='h' && arg[2]=='m') r->format=2;
    else return 0;
    const char *p=arg+4; uint32_t command,n;
    if (!desk_help_number(&p,&command) || !desk_help_number(&p,&r->context) || !desk_help_number(&p,&n) || n>160) return 0;
    size_t remaining=len-(size_t)(p-arg); if (n>remaining) return 0;
    r->command=command;
    for (uint32_t i=0;i<n;i++) r->path[i]=p[i];
    r->path[n]=0; p+=n;
    remaining-=n; for (size_t i=0;i<=remaining;i++) r->query[i]=p[i];
    return n || (r->format==1 && command==2) || (r->format==2 && command==18);
}
static inline int desk_help_decimal(char *out,size_t *at,uint32_t n)
{
    char digits[10]; unsigned count=0;
    do { digits[count++]=(char)('0'+n%10); n/=10; } while (n);
    if (*at+count+1>160) return 0;
    while (count) out[(*at)++]=digits[--count];
    out[(*at)++]=':'; return 1;
}
static inline int desk_help_encode(char out[161],unsigned format,unsigned command,uint32_t context,const char *path,const char *query)
{
    if (format!=1 && format!=2) return 0;
    size_t n=0,q=0; if (!path) path=""; if (!query) query="";
    while (path[n]) { if (n==160) return 0; n++; } while (query[q]) { if (q==160) return 0; q++; }
    const char *prefix=format==1 ? "hlp:" : "chm:";
    for (size_t i=0;i<4;i++) out[i]=prefix[i];
    size_t at=4;
    if (!desk_help_decimal(out,&at,command) || !desk_help_decimal(out,&at,context) || !desk_help_decimal(out,&at,(uint32_t)n) || n+q>160-at) return 0;
    for (size_t i=0;i<n;i++) out[at++]=path[i];
    for (size_t i=0;i<q;i++) out[at++]=query[i];
    out[at]=0; struct desk_help_request parsed; return desk_help_decode(out,&parsed);
}
#endif
