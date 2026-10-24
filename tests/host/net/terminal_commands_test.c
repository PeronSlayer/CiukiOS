/* Production command state machine with deterministic transport and time.
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <assert.h>
/* Linux's self-referential socket enum macros have the same numeric values
 * as CiukiOS; include them first, then use the guest constants in the model. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"
#include <ciuki/abi.h>
#pragma clang diagnostic pop
#include <ciuki/net.h>
#define CIUKI_TERMINAL_H
struct terminal {
    struct { void *ctx;void (*pump)(void *); } host;
    int interrupted,quit;
};
static char output[16384];static size_t used;
void term_write(struct terminal *t,const char *text)
{ (void)t;size_t n=strlen(text);assert(n<sizeof(output)-used);memcpy(output+used,text,n+1);used+=n; }
void term_printf(struct terminal *t,const char *format,...)
{ char text[1024];va_list args;va_start(args,format);vsnprintf(text,sizeof(text),format,args);va_end(args);term_write(t,text); }
static uint64_t clock_ms;static int dropped,force_error,ttl,sequence,pumps,closed,unrelated;
static uint8_t request[40];static struct sockaddr_in destination;
static int fake_clock(clockid_t c,struct timespec *out)
{ assert(c==CLOCK_MONOTONIC);clock_ms++;out->tv_sec=(time_t)(clock_ms/1000);out->tv_nsec=(long)(clock_ms%1000)*1000000;return 0; }
static int fake_socket(int family,int type,int protocol)
{ assert(family==AF_INET && (type&15)==SOCK_RAW && protocol==IPPROTO_ICMP);if(force_error) { errno=force_error;return -1; }return 21; }
static int fake_option(int fd,int level,int name,const void *value,socklen_t bytes)
{ assert(fd==21 && level==IPPROTO_IP && name==IP_TTL && bytes==4);ttl=*(const int *)value;return 0; }
static ssize_t fake_sendto(int fd,const void *data,size_t bytes,int flags,const struct sockaddr *address,socklen_t length)
{ assert(fd==21 && bytes==sizeof(request) && !flags && length==sizeof(destination));memcpy(request,data,bytes);destination=*(const struct sockaddr_in *)address;sequence++;unrelated=1;return (ssize_t)bytes; }
static int fake_poll(struct pollfd *fds,nfds_t n,int timeout)
{ assert(n==1 && timeout==10 && fds[0].fd==21);clock_ms+=10;fds[0].revents=dropped?0:POLLIN;return !dropped; }
static ssize_t fake_recvfrom(int fd,void *data,size_t bytes,int flags,struct sockaddr *address,socklen_t *length);
static int fake_close(int fd) { assert(fd==21);closed++;return 0; }
static int fake_getpid(void) { return 0x5326; }
static int fake_getaddrinfo(const char *name,const char *service,const struct addrinfo *hints,struct addrinfo **out)
{
    assert(!service && hints->ai_family==AF_INET);
    if(!strcmp(name,"unknown"))return EAI_NONAME;
    static struct sockaddr_in address;static struct addrinfo answer;
    address=(struct sockaddr_in){.sin_family=AF_INET,.sin_addr={.s_addr=0x0100007f}};
    answer=(struct addrinfo){.ai_addr=(void *)&address};*out=&answer;return 0;
}
static void fake_freeaddrinfo(struct addrinfo *answer) { assert(answer); }
static void pump(void *context) { struct terminal *t=context;pumps++;if(t->quit)t->interrupted=1; }
int ciuki_net_info(unsigned operation,void *record,uint32_t bytes)
{
    if(operation==CIUKI_NET_INFO_CONFIG) {
        assert(bytes==sizeof(struct ciuki_net_control));struct ciuki_net_control *r=record;unsigned index=r->index;
        *r=(struct ciuki_net_control){.size=sizeof(*r),.version=1,.count=1,.index=index,.link=CIUKI_NET_LINK_UP,
            .address=0x640200c0,.mask=0x00ffffff,.gateway=0x010200c0,.dns={0x350200c0},.rx_packets=42,.tx_packets=17};
        strcpy(r->name,"en0");strcpy(r->driver,"e100");strcpy(r->config.hostname,"CiukiOS");return 0;
    }
    assert(operation==CIUKI_NET_INFO_SOCKETS && bytes==sizeof(struct ciuki_net_socket_list));
    struct ciuki_net_socket_list *r=record;*r=(struct ciuki_net_socket_list){.size=sizeof(*r),.version=1,.count=1};
    r->sockets[0]=(struct ciuki_net_socket_info){.pid=7,.type=SOCK_STREAM,.state=CIUKI_NET_SOCKET_CONNECTED,
        .local_address=0x0100007f,.local_port=0x5000,.peer_address=0x0200007f,.peer_port=0xbb01};return 0;
}
int ciuki_net_control(unsigned operation,struct ciuki_net_control *record)
{ (void)record;assert(operation==CIUKI_NETCTL_RENEW || operation==CIUKI_NETCTL_RELEASE);errno=EPERM;return -1; }
#define clock_gettime fake_clock
#define socket fake_socket
#define setsockopt fake_option
#define sendto fake_sendto
#define poll fake_poll
#define recvfrom fake_recvfrom
#define close fake_close
#define getpid fake_getpid
#define getaddrinfo fake_getaddrinfo
#define freeaddrinfo fake_freeaddrinfo
#include "../../../apps/terminal/network.c"
static ssize_t fake_recvfrom(int fd,void *data,size_t bytes,int flags,struct sockaddr *address,socklen_t *length)
{
    assert(fd==21 && bytes>=68 && flags==MSG_DONTWAIT && *length==sizeof(struct sockaddr_in));
    uint8_t *p=data;memset(p,0,68);p[0]=0x45;p[8]=64;p[9]=1;p[12]=127;p[15]=1;
    unsigned n=60;
    if(ttl==1) {
        n=56;p[20]=11;p[28]=0x45;p[37]=1;memcpy(p+44,&destination.sin_addr.s_addr,4);memcpy(p+48,request,8);
        p[15]=2;
    } else { memcpy(p+20,request,40);p[20]=0; }
    if(unrelated) { p[n-1]^=1;unrelated=0; }
    p[2]=(uint8_t)(n>>8);p[3]=(uint8_t)n;p[22]=p[23]=0;
    uint16_t sum=term_icmp_checksum(p+20,n-20);p[22]=(uint8_t)(sum>>8);p[23]=(uint8_t)sum;
    /* One otherwise valid reply with a different identifier must be ignored. */
    if(!unrelated && (sequence&0x10000)==0) { p[ttl==1?52:24]^=1;sequence|=0x10000;sum=term_icmp_checksum(p+20,n-20);p[23]^=(uint8_t)sum; }
    *(struct sockaddr_in *)address=(struct sockaddr_in){.sin_family=AF_INET,.sin_addr={.s_addr=ttl==1?0x0200007f:destination.sin_addr.s_addr}};
    return (ssize_t)n;
}
static int command(struct terminal *t,int argc,char **argv)
{ used=0;output[0]=0;return term_network_command(t,argc,argv); }
int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);struct terminal t={.host={.pump=pump}};t.host.ctx=&t;
    char *ipconfig[]={"ipconfig","/all"};assert(!command(&t,2,ipconfig));assert(strstr(output,"192.0.2.100") && strstr(output,"192.0.2.53"));
    char *netstat[]={"netstat"};assert(!command(&t,1,netstat) && strstr(output,"ESTABLISHED") && strstr(output,"127.0.0.2:443"));
    char *stats[]={"netstat","-s"};assert(!command(&t,2,stats) && strstr(output,"RX 42"));
    char *renew[]={"ipconfig","/renew"};assert(command(&t,2,renew)==-EPERM);
    char *ping[]={"ping","-n","2","-w","50","localhost"};assert(!command(&t,6,ping) && strstr(output,"Received = 2") && closed==1 && pumps);
    dropped=1;assert(!command(&t,6,ping) && strstr(output,"Lost = 2") && closed==2);dropped=0;
    char *trace[]={"tracert","-h","3","localhost"};assert(!command(&t,4,trace) && ttl==2 && strstr(output,"127.0.0.2") && closed==3);
    t.quit=1;assert(command(&t,6,ping)==-EINTR && closed==4);t.quit=t.interrupted=0;
    force_error=ENETDOWN;assert(command(&t,6,ping)==-ENETDOWN && closed==4);force_error=0;
    char *unknown[]={"ping","unknown"};assert(command(&t,2,unknown)==-EINVAL && strstr(output,"Cannot resolve"));
    char *bad[]={"ping","-n","0","localhost"};assert(command(&t,4,bad)==-EINVAL);
    puts("PASS G26 Terminal commands: ipconfig/netstat, privilege, echo/TTL/timeout/cancel/DNS/link error and fd cleanup");return 0;
}
