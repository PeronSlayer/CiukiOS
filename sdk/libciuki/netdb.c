/* Bounded A lookup, with all linked records owned by this SDK.
 * SPDX-License-Identifier: MIT */
#include <ciuki/runtime.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include "hosts.h"
static int hosts_next(void *stream) { return fgetc(stream); }
static int hosts_address(const char *name,uint32_t *address)
{
    FILE *stream=fopen("/system/settings/HOSTS","r");
    if(!stream)return 0;
    int found=cu_hosts_lookup(hosts_next,stream,name,address);
    fclose(stream);return found;
}
struct answer { struct addrinfo info; struct sockaddr_in address; };
void freeaddrinfo(struct addrinfo *info)
{
    while (info) { struct addrinfo *next=info->ai_next; free(info); info=next; }
}
int getaddrinfo(const char *node,const char *service,const struct addrinfo *hints,struct addrinfo **out)
{
    if (!out) return EAI_FAIL;
    *out=NULL;
    struct addrinfo h={0}; if (hints) h=*hints;
    if (h.ai_flags&~(AI_PASSIVE|AI_NUMERICHOST|AI_NUMERICSERV)) return EAI_BADFLAGS;
    if (h.ai_family!=AF_UNSPEC && h.ai_family!=AF_INET) return EAI_FAMILY;
    if (h.ai_socktype && h.ai_socktype!=SOCK_STREAM && h.ai_socktype!=SOCK_DGRAM) return EAI_SOCKTYPE;
    if (h.ai_protocol && h.ai_protocol!=IPPROTO_TCP && h.ai_protocol!=IPPROTO_UDP) return EAI_SERVICE;
    if ((h.ai_socktype==SOCK_STREAM && h.ai_protocol==IPPROTO_UDP) || (h.ai_socktype==SOCK_DGRAM && h.ai_protocol==IPPROTO_TCP)) return EAI_SOCKTYPE;
    unsigned port=0;
    if (service) {
        if (!*service) return EAI_SERVICE;
        for (const char *p=service;*p;p++) { if (*p<'0'||*p>'9') return EAI_SERVICE; port=port*10+(unsigned)(*p-'0'); if (port>65535) return EAI_SERVICE; }
    }
    if (!node && !service) return EAI_NONAME;
    struct in_addr address;
    if (!node) address.s_addr=htonl(h.ai_flags&AI_PASSIVE ? INADDR_ANY : INADDR_LOOPBACK);
    else if (inet_pton(AF_INET,node,&address)!=1) {
        if (h.ai_flags&AI_NUMERICHOST) return EAI_NONAME;
        if (hosts_address(node,&address.s_addr)) { /* Local mappings precede DNS. */ }
        else if (!strcmp(node,"localhost")) address.s_addr=htonl(INADDR_LOOPBACK);
        else {
            size_t bytes=strlen(node)+1; if (bytes<2 || bytes>254) return EAI_NONAME;
            struct ciuki_resolve4 r={.size=sizeof(r),.version=1,.name=CU_PTR(node),.name_bytes=(uint32_t)bytes};
            int e=ciuki_error(CU_CALL(RESOLVE4,CU_PTR(&r),0,0,0,0,0));
            if (e) { if (e==ENOMEM) return EAI_MEMORY; errno=e; return EAI_SYSTEM; }
            if (r.status) return r.status==1 ? EAI_NONAME : r.status==2 ? EAI_AGAIN : EAI_FAIL;
            address.s_addr=r.address;
        }
    }
    struct addrinfo **tail=out;
    for (int type=SOCK_STREAM;type<=SOCK_DGRAM;type++) {
        int protocol=type==SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
        if ((h.ai_socktype && h.ai_socktype!=type) || (h.ai_protocol && h.ai_protocol!=protocol)) continue;
        struct answer *a=calloc(1,sizeof(*a));
        if (!a) { freeaddrinfo(*out); *out=NULL; return EAI_MEMORY; }
        a->address.sin_family=AF_INET; a->address.sin_port=htons((uint16_t)port); a->address.sin_addr=address;
        a->info=(struct addrinfo){.ai_flags=h.ai_flags,.ai_family=AF_INET,.ai_socktype=type,.ai_protocol=protocol,
            .ai_addrlen=sizeof(a->address),.ai_addr=(void *)&a->address};
        *tail=&a->info; tail=&a->info.ai_next;
    }
    return 0;
}
const char *gai_strerror(int code)
{
    switch (code) {
    case 0:return "Success";
    case EAI_BADFLAGS:return "Unsupported resolver flags";
    case EAI_NONAME:return "Name or service not found";
    case EAI_AGAIN:return "Temporary resolver failure";
    case EAI_FAIL:return "Resolver failure";
    case EAI_FAMILY:return "Unsupported address family";
    case EAI_SOCKTYPE:return "Unsupported socket type";
    case EAI_SERVICE:return "Unsupported service";
    case EAI_MEMORY:return "Out of memory";
    case EAI_SYSTEM:return "System error";
    default:return "Unknown resolver error";
    }
}
