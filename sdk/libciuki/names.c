/* NetBIOS LMHOSTS/NBNS and IPv4 PTR resolver. SPDX-License-Identifier: MIT */
#include <ciuki/net.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include "name_wire.h"
static int next_byte(void *f) { return fgetc(f); }
static uint16_t transaction(void)
{
    static uint32_t serial;struct timespec now={0};clock_gettime(CLOCK_MONOTONIC,&now);
    return (uint16_t)(__atomic_add_fetch(&serial,1,__ATOMIC_RELAXED)^(uint32_t)now.tv_nsec^(uint32_t)getpid());
}
static uint64_t now_ms(void)
{ struct timespec t={0};clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+(unsigned long)t.tv_nsec/1000000; }
static int exchange(const unsigned char *query,size_t size,const struct sockaddr_in *server,
                    int broadcast,uint32_t *address,char *hostname,size_t cap)
{
    int fd=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,IPPROTO_UDP);if(fd<0)return -1;
    int error=EHOSTUNREACH,one=1;
    if(broadcast && setsockopt(fd,SOL_SOCKET,SO_BROADCAST,&one,sizeof(one))) { error=errno;goto done; }
    if(!broadcast && connect(fd,(const void *)server,sizeof(*server))) { error=errno;goto done; }
    for(unsigned attempt=0;attempt<3;attempt++) {
        if(sendto(fd,query,size,0,broadcast?(const void *)server:NULL,broadcast?sizeof(*server):0)<0) { error=errno;break; }
        uint64_t end=now_ms()+500;
        while(now_ms()<end) {
            struct pollfd p={fd,POLLIN,0};int r=poll(&p,1,50);
            if(r<0) { error=errno;goto done; }if(!r)continue;
            unsigned char response[1536];struct sockaddr_in from;socklen_t length=sizeof(from);
            ssize_t bytes=recvfrom(fd,response,sizeof(response),MSG_DONTWAIT,(void *)&from,&length);
            if(bytes<0) { if(errno==EAGAIN)continue;error=errno;goto done; }
            if(length!=sizeof(from) || from.sin_family!=AF_INET || from.sin_port!=server->sin_port ||
                (!broadcast && from.sin_addr.s_addr!=server->sin_addr.s_addr))continue;
            if(cu_name_answer(response,(size_t)bytes,query,size,address,hostname,cap)) { error=0;goto done; }
        }
    }
done:
    close(fd);if(error) { errno=error;return -1; }return 0;
}
int ciuki_netbios_resolve(const char *name,unsigned suffix,uint32_t *address)
{
    if(!name || !address || suffix>255) { errno=EINVAL;return -1; }
    unsigned char query[50];size_t size=cu_nbns_query(query,transaction(),name,suffix);
    if(!size) { errno=EINVAL;return -1; }
    FILE *f=fopen("/system/settings/LMHOSTS","r");
    if(f) { int found=cu_lmhosts_lookup(next_byte,f,name,suffix,address);fclose(f);if(found)return 0; }
    struct ciuki_net_control c={.size=sizeof(c),.version=1};
    if(ciuki_net_info(CIUKI_NET_INFO_CONFIG,&c,sizeof(c)))return -1;
    unsigned count=c.count;
    for(unsigned i=0;i<count;i++) {
        c=(struct ciuki_net_control){.size=sizeof(c),.version=1,.index=i};
        if(ciuki_net_info(CIUKI_NET_INFO_CONFIG,&c,sizeof(c)) || !c.address || !c.mask)continue;
        struct sockaddr_in server={.sin_family=AF_INET,.sin_port=htons(137),.sin_addr={.s_addr=c.address|~c.mask}};
        if(!exchange(query,size,&server,1,address,NULL,0))return 0;
        if(errno==EINTR)return -1;
    }
    errno=EHOSTUNREACH;return -1;
}
int ciuki_hosts_reverse4(uint32_t address,char *hostname,size_t cap)
{
    if(!hostname || cap<2) { errno=EINVAL;return -1; }
    FILE *f=fopen("/system/settings/HOSTS","r");
    if(f) { int found=cu_hosts_reverse(next_byte,f,address,hostname,cap);fclose(f);if(found)return 0; }
    if(address==htonl(INADDR_LOOPBACK) && cap>=10) { memcpy(hostname,"localhost",10);return 0; }
    errno=ENOENT;return -1;
}
int ciuki_reverse4(uint32_t address,char *hostname,size_t cap)
{
    if(!ciuki_hosts_reverse4(address,hostname,cap))return 0;
    if(!hostname || cap<2) { errno=EINVAL;return -1; }
    unsigned char query[90];size_t size=cu_ptr_query(query,transaction(),address);
    struct ciuki_net_control c={.size=sizeof(c),.version=1};
    if(ciuki_net_info(CIUKI_NET_INFO_CONFIG,&c,sizeof(c)))return -1;
    unsigned count=c.count;
    for(unsigned i=0;i<count;i++) {
        c=(struct ciuki_net_control){.size=sizeof(c),.version=1,.index=i};
        if(ciuki_net_info(CIUKI_NET_INFO_CONFIG,&c,sizeof(c)))continue;
        for(unsigned d=0;d<3;d++)if(c.dns[d]) {
            struct sockaddr_in server={.sin_family=AF_INET,.sin_port=htons(53),.sin_addr={.s_addr=c.dns[d]}};
            if(!exchange(query,size,&server,0,NULL,hostname,cap))return 0;
            if(errno==EINTR)return -1;
        }
    }
    errno=EHOSTUNREACH;return -1;
}
