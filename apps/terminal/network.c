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
        if(op!=CIUKI_NETCTL_GET && !r.config.dhcp) { term_printf(t,"%s: DHCP is not enabled.\n",r.name);continue; }
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
struct netstat_options { int all,numeric,statistics,ethernet,routes;unsigned protocol,interval; };
static int netstat_options(int argc,char **argv,struct netstat_options *o)
{
    *o=(struct netstat_options){0};
    for(int i=1;i<argc;i++) {
        if(argv[i][0]=='-') {
            if(!argv[i][1])return -EINVAL;
            for(const char *p=argv[i]+1;*p;p++)switch(*p>='A' && *p<='Z'?*p+32:*p) {
            case 'a':o->all=1;break;case 'n':o->numeric=1;break;
            case 's':o->statistics=1;break;case 'e':o->ethernet=1;break;case 'r':o->routes=1;break;
            case 'p':
                if(p[1] || ++i==argc)return -EINVAL;
                if(!strcasecmp(argv[i],"tcp"))o->protocol=IPPROTO_TCP;
                else if(!strcasecmp(argv[i],"udp"))o->protocol=IPPROTO_UDP;
                else if(!strcasecmp(argv[i],"ip"))o->protocol=255;
                else if(!strcasecmp(argv[i],"icmp"))o->protocol=IPPROTO_ICMP;
                else return -EINVAL;
                break;
            default:return -EINVAL;
            }
        } else {
            char *end;unsigned long n=strtoul(argv[i],&end,10);
            if(!*argv[i] || *end || !n || n>86400 || o->interval || i!=argc-1)return -EINVAL;
            o->interval=(unsigned)n;
        }
    }
    if(o->protocol && o->protocol!=IPPROTO_TCP && o->protocol!=IPPROTO_UDP && !o->statistics)return -EINVAL;
    return 0;
}
static void protocol_stats(struct terminal *t,const char *name,const struct ciuki_net_protocol_stats *s)
{
    term_printf(t,"%s Statistics\n  Received = %u\n  Sent = %u\n  Discarded = %u\n  Errors = %u\n  Checksum Errors = %u\n  Length Errors = %u\n  Memory Errors = %u\n  Protocol Errors = %u\n",
        name,s->received,s->sent,s->dropped,s->errors,s->checksum_errors,s->length_errors,s->memory_errors,s->protocol_errors);
}
static int netstat_once(struct terminal *t,const struct netstat_options *o)
{
    if(o->ethernet) { char *args[]={"NETSTAT","-s"};int e=config(t,2,args,1);if(e)return e; }
    if(o->routes) {
        struct ciuki_net_routes routes;
        if(ciuki_net_info(CIUKI_NET_INFO_ROUTES,&routes,sizeof(routes)))return -errno;
        if(routes.version!=1 || routes.size!=sizeof(routes) || routes.count>CIUKI_NET_ROUTES_MAX)return -EIO;
        term_write(t,"Destination      Netmask          Gateway          Interface\n");
        for(unsigned i=0;i<routes.count;i++) {
            struct ciuki_net_route *r=&routes.routes[i];char ip[16],mask[16],gw[16],net[16];
            ip_text(ip,r->interface_address);ip_text(mask,r->mask);ip_text(gw,r->gateway);ip_text(net,r->destination);
            term_printf(t,"%-16s %-16s %-16s %s\n",net,mask,gw,ip);
        }
    }
    if(o->statistics) {
        struct ciuki_net_statistics s;
        if(ciuki_net_info(CIUKI_NET_INFO_STATS,&s,sizeof(s)))return -errno;
        if(s.size!=sizeof(s) || s.version!=1)return -EIO;
        if(!o->protocol || o->protocol==255)protocol_stats(t,"IPv4",&s.ip);
        if(!o->protocol || o->protocol==IPPROTO_ICMP)protocol_stats(t,"ICMP",&s.icmp);
        if(!o->protocol || o->protocol==IPPROTO_TCP) {
            protocol_stats(t,"TCP",&s.tcp);
            term_printf(t,"  Active Opens = %u\n  Passive Opens = %u\n  Failed Connection Attempts = %u\n  Reset Connections = %u\n  Current Connections = %u\n  Segments Received = %u\n  Segments Sent = %u\n  Segments Retransmitted = %u\n  Receive Errors = %u\n  Resets Sent = %u\n",s.tcp_active_opens,s.tcp_passive_opens,s.tcp_failed_connects,s.tcp_resets,s.tcp_connections,s.tcp_received,s.tcp_sent,s.tcp_retransmitted,s.tcp_receive_errors,s.tcp_reset_segments);
        }
        if(!o->protocol || o->protocol==IPPROTO_UDP) {
            protocol_stats(t,"UDP",&s.udp);term_printf(t,"  No Ports = %u\n  Receive Errors = %u\n",s.udp_no_ports,s.udp_receive_errors);
        }
    }
    if(o->statistics || o->ethernet || o->routes)return 0;
    struct ciuki_net_socket_list *list=malloc(sizeof(*list));if(!list)return -ENOMEM;
    if(ciuki_net_info(CIUKI_NET_INFO_SOCKETS,list,sizeof(*list))) { int e=errno;free(list);return -e; }
    if(list->version!=1 || list->size!=sizeof(*list) || list->count>CIUKI_SOCKET_MAX) { free(list);return -EIO; }
    static const char *const states[]={"CLOSED","LISTENING","CONNECTING","ESTABLISHED","CLOSE_WAIT"};
    term_write(t,"Proto Local Address           Foreign Address         State         PID\n");
    for(unsigned i=0;i<list->count;i++) {
        struct ciuki_net_socket_info *s=&list->sockets[i];
        if(s->type!=SOCK_STREAM && s->type!=SOCK_DGRAM)continue;
        if(!o->all && (s->type!=SOCK_STREAM || (s->state!=CIUKI_NET_SOCKET_CONNECTED && s->state!=CIUKI_NET_SOCKET_CLOSE_WAIT)))continue;
        if(o->protocol && o->protocol!=(s->type==SOCK_STREAM?IPPROTO_TCP:IPPROTO_UDP))continue;
        char local[254],peer[254],le[272],pe[272];ip_text(local,s->local_address);ip_text(peer,s->peer_address);
        if(!o->numeric) { if(ciuki_hosts_reverse4(s->local_address,local,sizeof(local)))ip_text(local,s->local_address);if(ciuki_hosts_reverse4(s->peer_address,peer,sizeof(peer)))ip_text(peer,s->peer_address); }
        snprintf(le,sizeof(le),"%s:%u",local,ntohs(s->local_port));snprintf(pe,sizeof(pe),"%s:%u",peer,ntohs(s->peer_port));
        term_printf(t,"%-5s %-23s %-23s %-13s %u\n",s->type==SOCK_STREAM?"TCP":"UDP",le,pe,
            s->type==SOCK_STREAM && s->state<=CIUKI_NET_SOCKET_CLOSE_WAIT?states[s->state]:"",s->pid);
        if(t->host.pump)t->host.pump(t->host.ctx);
        if(t->interrupted || t->quit) { free(list);return -EINTR; }
    }
    free(list);return 0;
}
static int netstat(struct terminal *t,int argc,char **argv)
{
    struct netstat_options o;int e=netstat_options(argc,argv,&o);if(e)return e;
    do {
        e=netstat_once(t,&o);if(e || !o.interval)return e;
        uint64_t end=milliseconds()+o.interval*1000;
        while(milliseconds()<end) {
            if(t->host.pump)t->host.pump(t->host.ctx);
            if(t->interrupted || t->quit)return -EINTR;
            struct timespec delay={.tv_nsec=10000000};nanosleep(&delay,NULL);
        }
    } while(1);
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
    if(!strcasecmp(argv[0],"NETSTAT"))return netstat(t,argc,argv);
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
    struct ciuki_resolve_query query;
    if(ciuki_resolve4_begin(name,&query))return -errno;
    while(query.status==4) {
        if(t->host.pump)t->host.pump(t->host.ctx);
        if(t->interrupted || t->quit) {
            query.action=CIUKI_RESOLVE_CANCEL;(void)ciuki_resolve4_query(&query);return -EINTR;
        }
        query.action=CIUKI_RESOLVE_POLL;
        if(ciuki_resolve4_query(&query))return -errno;
        if(query.status==4) { struct timespec delay={.tv_nsec=10000000};nanosleep(&delay,NULL); }
    }
    if(query.status) { term_printf(t,"Cannot resolve %s: DNS lookup failed.\n",name);return -EINVAL; }
    struct sockaddr_in target={.sin_family=AF_INET,.sin_addr={.s_addr=query.address}};
    int r;
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
