/* Interactive IPv4 diagnostics over the existing native socket API.
 * SPDX-License-Identifier: MIT */
#include "terminal.h"
#include "icmp.h"
#include <ciuki/net.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
static uint64_t milliseconds(void)
{ struct timespec t={0};clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+(unsigned long)t.tv_nsec/1000000; }
static void ip_text(char out[16],uint32_t ip)
{ const uint8_t *b=(const void *)&ip;snprintf(out,16,"%u.%u.%u.%u",b[0],b[1],b[2],b[3]); }
static int config(struct terminal *t,int argc,char **argv,int statistics)
{
    unsigned op=CIUKI_NETCTL_GET;
    if(argc==2) {
        if(statistics) { if(strcasecmp(argv[1],"-s"))return -EINVAL; }
        else if(!strcasecmp(argv[1],"/renew"))op=CIUKI_NETCTL_RENEW;
        else if(!strcasecmp(argv[1],"/release"))op=CIUKI_NETCTL_RELEASE;
        else if(strcasecmp(argv[1],"/all"))return -EINVAL;
    } else if(argc!=1)return -EINVAL;
    struct ciuki_net_control r={.size=sizeof(r),.version=1};
    if(ciuki_net_info(CIUKI_NET_INFO_CONFIG,&r,sizeof(r))) { if(errno==ENODEV) { term_write(t,"No network adapters.\n");return 0; }return -errno; }
    unsigned count=r.count;
    for(unsigned i=0;i<count;i++) {
        r=(struct ciuki_net_control){.size=sizeof(r),.version=1,.index=i};
        if(ciuki_net_info(CIUKI_NET_INFO_CONFIG,&r,sizeof(r))) { if(errno==ENODEV) { term_write(t,"No network adapters.\n");return 0; }return -errno; }
        if(op!=CIUKI_NETCTL_GET && ciuki_net_control(op,&r))return -errno;
        if(statistics) {
            term_printf(t,"%s (%s)  RX %llu  TX %llu  RX errors %llu  TX errors %llu\n",r.name,r.driver,
                (unsigned long long)r.rx_packets,(unsigned long long)r.tx_packets,
                (unsigned long long)r.rx_errors,(unsigned long long)r.tx_errors);
        } else {
            char ip[16],mask[16],gateway[16];ip_text(ip,r.address);ip_text(mask,r.mask);ip_text(gateway,r.gateway);
            term_printf(t,"%s (%s)  %s\n  IP Address: %s\n  Subnet Mask: %s\n  Default Gateway: %s\n",r.name,r.driver,r.link==CIUKI_NET_LINK_UP?"connected":"disconnected",ip,mask,gateway);
            if(argc==2) {
                term_printf(t,"  Host: %s  DHCP: %s\n  MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",r.config.hostname,r.config.dhcp?"yes":"no",r.mac[0],r.mac[1],r.mac[2],r.mac[3],r.mac[4],r.mac[5]);
                for(unsigned d=0;d<3;d++)if(r.dns[d]) { ip_text(ip,r.dns[d]);term_printf(t,"  DNS: %s\n",ip); }
                term_printf(t,"  Lease: %u seconds, %u remaining\n",r.lease_seconds,r.lease_remaining);
            }
        }
        if(t->host.pump)t->host.pump(t->host.ctx);
        if(t->interrupted)return -EINTR;
    }
    if(!count)term_write(t,"No network adapters.\n");
    return 0;
}
static int probe(struct terminal *t,int fd,const struct sockaddr_in *target,unsigned id,unsigned sequence,unsigned ttl,unsigned wait)
{
    int hop=(int)ttl;
    if(setsockopt(fd,IPPROTO_IP,IP_TTL,&hop,sizeof(hop)))return -errno;
    uint8_t packet[40]={8,0,0,0,(uint8_t)(id>>8),(uint8_t)id,(uint8_t)(sequence>>8),(uint8_t)sequence};
    memcpy(packet+8,"CiukiOS classic computing",25);
    uint16_t checksum=term_icmp_checksum(packet,sizeof(packet));packet[2]=(uint8_t)(checksum>>8);packet[3]=(uint8_t)checksum;
    uint64_t start=milliseconds(),end=start+wait;
    if(sendto(fd,packet,sizeof(packet),0,(const void *)target,sizeof(*target))<0)return -errno;
    while(milliseconds()<end) {
        if(t->host.pump)t->host.pump(t->host.ctx);
        if(t->interrupted || t->quit)return -EINTR;
        struct pollfd p={fd,POLLIN,0};int r=poll(&p,1,10);
        if(r<0) { if(errno==EINTR)continue;return -errno; }
        if(!r)continue;
        uint8_t received[1536];struct sockaddr_in source;socklen_t bytes=sizeof(source);
        int n=(int)recvfrom(fd,received,sizeof(received),MSG_DONTWAIT,(void *)&source,&bytes);
        if(n<0) { if(errno==EAGAIN || errno==EINTR)continue;return -errno; }
        int match=term_icmp_match(received,(size_t)n,id,sequence,target->sin_addr.s_addr);
        if(!match)continue;
        char ip[16];ip_text(ip,source.sin_addr.s_addr);
        term_printf(t,"%s  %llu ms%s",ip,(unsigned long long)(milliseconds()-start),match==3?" (destination unreachable)":"");
        return match;
    }
    term_write(t,"*");return 0;
}
int term_network_command(struct terminal *t,int argc,char **argv)
{
    if(!strcasecmp(argv[0],"IPCONFIG"))return config(t,argc,argv,0);
    if(!strcasecmp(argv[0],"NETSTAT")) {
        if(argc==2 && !strcasecmp(argv[1],"-s"))return config(t,argc,argv,1);
        if(argc>2 || (argc==2 && strcasecmp(argv[1],"-n") && strcasecmp(argv[1],"-a")))return -EINVAL;
        struct ciuki_net_socket_list *list=malloc(sizeof(*list));if(!list)return -ENOMEM;
        if(ciuki_net_info(CIUKI_NET_INFO_SOCKETS,list,sizeof(*list))) { int e=errno;free(list);return -e; }
        if(list->version!=1 || list->size!=sizeof(*list) || list->count>CIUKI_SOCKET_MAX) { free(list);return -EIO; }
        static const char *const states[]={"CLOSED","LISTENING","CONNECTING","ESTABLISHED","CLOSE_WAIT"};
        term_write(t,"Proto Local Address           Foreign Address         State         PID\n");
        for(unsigned i=0;i<list->count;i++) {
            struct ciuki_net_socket_info *s=&list->sockets[i];char local[16],peer[16],le[24],pe[24];
            ip_text(local,s->local_address);ip_text(peer,s->peer_address);
            snprintf(le,sizeof(le),"%s:%u",local,ntohs(s->local_port));snprintf(pe,sizeof(pe),"%s:%u",peer,ntohs(s->peer_port));
            term_printf(t,"%-5s %-23s %-23s %-13s %u\n",s->type==SOCK_STREAM?"TCP":s->type==SOCK_DGRAM?"UDP":"ICMP",le,pe,
                s->type==SOCK_STREAM && s->state<=CIUKI_NET_SOCKET_CLOSE_WAIT?states[s->state]:"",s->pid);
        }
        free(list);return 0;
    }
    int trace=!strcasecmp(argv[0],"TRACERT");
    const char *name=NULL;unsigned count=4,wait=1000,hops=30;
    for(int i=1;i<argc;i++) {
        if(!strcasecmp(argv[i],"-d") && trace)continue;
        if((!strcasecmp(argv[i],"-n") && !trace) || !strcasecmp(argv[i],"-w") || (!strcasecmp(argv[i],"-h") && trace)) {
            char option=argv[i][1];if(++i==argc)return -EINVAL;
            char *end;unsigned long n=strtoul(argv[i],&end,10);if(!*argv[i] || *end || !n)return -EINVAL;
            if(option=='w' || option=='W') { if(n>60000)return -EINVAL;wait=(unsigned)n; }
            else if(trace) { if(n>255)return -EINVAL;hops=(unsigned)n; }
            else { if(n>100)return -EINVAL;count=(unsigned)n; }
        } else if(!name && argv[i][0]!='-')name=argv[i];else return -EINVAL;
    }
    if(!name)return -EINVAL;
    struct addrinfo hints={.ai_family=AF_INET,.ai_socktype=SOCK_DGRAM},*answer=NULL;
    int r=getaddrinfo(name,NULL,&hints,&answer);
    if(r) { term_printf(t,"Cannot resolve %s: %s\n",name,gai_strerror(r));return -EINVAL; }
    struct sockaddr_in target=*(struct sockaddr_in *)answer->ai_addr;freeaddrinfo(answer);target.sin_port=0;
    int fd=socket(AF_INET,SOCK_RAW|SOCK_NONBLOCK,IPPROTO_ICMP);if(fd<0)return -errno;
    unsigned id=(unsigned)getpid()&65535,sequence=0,received=0;
    char ip[16];ip_text(ip,target.sin_addr.s_addr);term_printf(t,"%s %s [%s]\n",trace?"Tracing route to":"Pinging",name,ip);
    for(unsigned hop=1;hop<=(trace?hops:count);hop++) {
        if(trace)term_printf(t,"%2u  ",hop);
        int reached=0;
        for(unsigned j=0;j<(trace?3u:1u);j++) {
            r=probe(t,fd,&target,id,++sequence,trace?hop:128,wait);
            term_write(t,j==2 || !trace?"\n":"  ");
            if(r<0)goto done;
            if(r==1) { received++;reached=1; }
            if(r==3) { reached=1; }
        }
        if(trace && reached)break;
    }
    if(!trace)term_printf(t,"Packets: Sent = %u, Received = %u, Lost = %u\n",count,received,count-received);
    r=0;
done:
    close(fd);return r;
}
