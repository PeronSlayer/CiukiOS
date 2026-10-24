/* Real Control Panel worker with system-resolver/socket seams.
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "replay_base.h"
#undef IPPROTO_IP
#undef IPPROTO_TCP
#undef IPPROTO_UDP
#undef IPPROTO_ICMP
#undef SOCK_RAW
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "control.h"
static atomic_int hold;
static unsigned dns_calls,connects,closed;
static int mode,dns_fail,control_error;
static struct sockaddr_in answer_address={.sin_family=AF_INET};
static struct addrinfo answer={.ai_family=AF_INET,.ai_addr=(void *)&answer_address,.ai_addrlen=sizeof(answer_address)};
static int system_resolve(const char *name,const char *service,const struct addrinfo *hints,struct addrinfo **out)
{
    assert(!strcmp(name,"example.test") && !service && hints->ai_family==AF_INET);dns_calls++;
    while(atomic_load(&hold)){struct timespec t={.tv_nsec=1000000};nanosleep(&t,NULL);}
    if(dns_fail){*out=NULL;return EAI_FAIL;}*out=&answer;return 0;
}
static void system_free(struct addrinfo *a){assert(a==&answer);}
static int test_socket(int domain,int type,int protocol){assert(domain==AF_INET && type==(SOCK_STREAM|SOCK_NONBLOCK) && !protocol);return 41;}
static int test_bind(int fd,const struct sockaddr *address,socklen_t bytes)
{const struct sockaddr_in *a=(const void *)address;assert(fd==41 && bytes==sizeof(*a) && a->sin_addr.s_addr==htonl(0xc0000264));return 0;}
static int test_connect(int fd,const struct sockaddr *address,socklen_t bytes)
{const struct sockaddr_in *a=(const void *)address;assert(fd==41 && bytes==sizeof(*a) && a->sin_addr.s_addr==htonl(0xc0000201) && ntohs(a->sin_port)==80);connects++;if(!mode)return 0;errno=EINPROGRESS;return -1;}
static int test_poll(struct pollfd *p,nfds_t count,int timeout){assert(count==1 && p->fd==41 && timeout==2000);return mode==2 ? 0 : 1;}
static int test_option(int fd,int level,int option,void *out,socklen_t *size)
{assert(fd==41 && level==SOL_SOCKET && option==SO_ERROR && *size==sizeof(int));*(int *)out=ECONNREFUSED;return 0;}
static int test_close(int fd){assert(fd==41);closed++;return 0;}
int ciuki_net_control(unsigned op,struct ciuki_net_control *r)
{
    if(control_error){errno=control_error;return -1;}
    assert(op<=3);r->count=1;r->address=htonl(0xc0000264);r->gateway=htonl(0xc0000201);r->config.enabled=1;return 0;
}
#define getaddrinfo system_resolve
#define freeaddrinfo system_free
#define socket test_socket
#define bind test_bind
#define connect test_connect
#define poll test_poll
#define getsockopt test_option
#define close test_close
#include "../../../apps/control/network_io.inc"
static void finish(struct cp_network_worker *w,struct cp_network_result *out)
{
    for(unsigned i=0;i<2000;i++) {
        if(network_worker_poll(w,out))return;
        struct timespec t={.tv_nsec=1000000};nanosleep(&t,NULL);
    }
    assert(!"worker did not complete");
}
int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);struct cp_network_worker w={0};struct cp_network_result result;
    struct ciuki_net_control r={.version=1,.size=sizeof(r)};answer_address.sin_addr.s_addr=htonl(0xc0000208);
    atomic_store(&hold,1);assert(!network_worker_begin(&w,CP_NET_TEST,&r,"example.test"));
    assert(network_worker_begin(&w,CP_NET_TEST,&r,"example.test")==-EBUSY && !network_worker_poll(&w,&result));
    atomic_store(&hold,0);finish(&w,&result);assert(strstr(result.message,"DNS 192.0.2.8; gateway TCP/80 connected"));
    mode=1;assert(!network_worker_begin(&w,CP_NET_TEST,&r,"example.test"));finish(&w,&result);assert(strstr(result.message,"refused"));
    mode=2;dns_fail=1;assert(!network_worker_begin(&w,CP_NET_TEST,&r,"example.test"));finish(&w,&result);assert(strstr(result.message,"DNS failed") && strstr(result.message,"timed out"));
    assert(dns_calls==3 && connects==3 && closed==3);
    control_error=EPERM;assert(!network_worker_begin(&w,CIUKI_NETCTL_SET,&r,"example.test"));finish(&w,&result);assert(result.error==-EPERM && !result.count && strstr(result.message,"failed"));
    control_error=0;assert(!network_worker_begin(&w,CIUKI_NETCTL_SET,&r,"example.test"));finish(&w,&result);assert(!result.error && result.count==1 && strstr(result.message,"saved and applied"));
    assert(!network_worker_begin(&w,CIUKI_NETCTL_GET,&r,"example.test"));network_worker_stop(&w);assert(!w.active);
    puts("PASS f2-100 network worker: nonblocking UI boundary, copied jobs, system DNS, selected-source gateway TCP/80 connect/refusal/timeout, errors, joins and socket cleanup");return 0;
}
