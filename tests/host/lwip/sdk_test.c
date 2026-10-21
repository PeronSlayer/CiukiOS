/* Execute the actual SDK wrappers with fixed-width records, using only a
 * deterministic syscall boundary. No host socket or resolver is called.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/runtime.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/select.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#undef stdout
extern FILE *stdout; /* Linux fixture stream; target newlib uses _reent. */
#define CHECK(e) do { if(!(e)){printf("FAIL SDK line %u: %s\n",__LINE__,#e);abort();} }while(0)
static int error_value, poll_mode, dns_status, dns_error;
static unsigned calls, dns_calls;
static const void *pointers[64];
static unsigned pointer_index;
uint32_t cu_test_pointer(const void *p)
{ if(!p)return 0;unsigned i=pointer_index++%64;pointers[i]=p;return i+1; }
void *cu_test_address(uint32_t p) { CHECK(p && p<=64);return (void *)pointers[p-1]; }
int *__errno(void) { return &error_value; }
uint32_t ciuki_syscall(uint32_t nr,uint32_t b,uint32_t c,uint32_t d,uint32_t s,uint32_t i,uint32_t p)
{
    calls++;CHECK(!s && !i && !p);
    if(nr==CIUKI_SYS_POLL){
        CHECK(c<=128);struct pollfd *fds=c ? cu_test_address(b) : NULL;
        CHECK((int32_t)d==1 || (int32_t)d==-1 || d==0);
        for(unsigned j=0;j<c;j++)fds[j].revents=poll_mode ? POLLNVAL : fds[j].events;
        return c;
    }
    if(nr==CIUKI_SYS_RESOLVE4){
        CHECK(!c && !d);struct ciuki_resolve4 *r=cu_test_address(b);
        CHECK(r->size==32 && r->version==1 && !r->reserved[0] && !r->reserved[1]);dns_calls++;
        CHECK(r->name_bytes==strlen(cu_test_address(r->name))+1);
        r->status=dns_status;r->address=htonl(0xc0000209);return (uint32_t)-dns_error;
    }
    if(nr==CIUKI_SYS_SOCKET){CHECK(b==AF_INET && c==SOCK_DGRAM && !d);return (uint32_t)-EAFNOSUPPORT;}
    if(nr==CIUKI_SYS_SENDTO || nr==CIUKI_SYS_RECVFROM){
        struct ciuki_socket_io *r=cu_test_address(b);CHECK(!c && !d && r->size==40 && r->version==1 && !r->reserved[0] && !r->reserved[1]);
        return r->bytes;
    }
    CHECK(false);return (uint32_t)-ENOSYS;
}
int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    struct in_addr ip={0};char text[16];errno=EIO;
    CHECK(inet_pton(AF_INET,"192.0.2.9",&ip)==1 && ntohl(ip.s_addr)==0xc0000209 && errno==EIO);
    CHECK(inet_ntop(AF_INET,&ip,text,sizeof(text))==text && !strcmp(text,"192.0.2.9"));
    CHECK(inet_pton(AF_INET,"01.2.3.4",&ip)==0 && inet_pton(AF_INET,"1.2.3.256",&ip)==0);
    CHECK(inet_pton(AF_INET,"1.2.3",&ip)==0 && inet_pton(AF_INET,"1.2.3.4x",&ip)==0);
    CHECK(!inet_ntop(AF_INET,&ip,text,4) && errno==ENOSPC);
    CHECK(inet_aton("127.0.0.1",&ip) && !strcmp(inet_ntoa(ip),"127.0.0.1"));
    CHECK(ntohs(htons(12345))==12345 && ntohl(htonl(0x12345678))==0x12345678);
    CHECK(socket(AF_INET,SOCK_DGRAM,0)==-1 && errno==EAFNOSUPPORT);
    errno=EIO;CHECK(send(3,"data",4,0)==4 && recv(3,text,3,0)==3 && errno==EIO);
    fd_set read,write,except;FD_ZERO(&read);FD_ZERO(&write);FD_ZERO(&except);FD_SET(3,&read);FD_SET(3,&write);
    struct timeval timeout={.tv_sec=0,.tv_usec=1},saved=timeout;unsigned before=calls;
    CHECK(select(4,&read,&write,&except,&timeout)==2 && calls==before+1 && FD_ISSET(3,&read) && FD_ISSET(3,&write));
    CHECK(!memcmp(&timeout,&saved,sizeof(timeout)));
    FD_SET(1,&except);CHECK(select(4,&read,&write,&except,NULL)==-1 && errno==EOPNOTSUPP);FD_ZERO(&except);
    CHECK(select(129,&read,NULL,NULL,NULL)==-1 && errno==EINVAL);
    poll_mode=1;CHECK(select(4,&read,NULL,NULL,&timeout)==-1 && errno==EBADF);poll_mode=0;
    struct addrinfo *answer=NULL,h={0};CHECK(!getaddrinfo(NULL,"123",&h,&answer));
    CHECK(answer && answer->ai_next && !answer->ai_next->ai_next && answer->ai_socktype==SOCK_STREAM);
    struct sockaddr_in *a=(void *)answer->ai_addr;CHECK(a->sin_family==AF_INET && ntohs(a->sin_port)==123 && ntohl(a->sin_addr.s_addr)==INADDR_LOOPBACK);freeaddrinfo(answer);
    h.ai_flags=AI_PASSIVE;h.ai_socktype=SOCK_DGRAM;CHECK(!getaddrinfo(NULL,"0",&h,&answer) && !answer->ai_next);a=(void *)answer->ai_addr;CHECK(!a->sin_addr.s_addr);freeaddrinfo(answer);
    h=(struct addrinfo){.ai_flags=AI_NUMERICHOST};CHECK(getaddrinfo("fixture.example","80",&h,&answer)==EAI_NONAME && !answer);
    CHECK(!getaddrinfo("192.0.2.1","65535",&h,&answer));freeaddrinfo(answer);
    h=(struct addrinfo){0};CHECK(!getaddrinfo("localhost",NULL,&h,&answer));freeaddrinfo(answer);CHECK(!dns_calls);
    CHECK(!getaddrinfo("fixture.example","80",&h,&answer) && dns_calls==1);freeaddrinfo(answer);
    for(int status=1;status<=3;status++){dns_status=status;CHECK(getaddrinfo("fixture.example",NULL,&h,&answer)==(status==1 ? EAI_NONAME : status==2 ? EAI_AGAIN : EAI_FAIL) && !answer);}
    dns_error=EINTR;CHECK(getaddrinfo("fixture.example",NULL,&h,&answer)==EAI_SYSTEM && errno==EINTR);
    dns_error=ENOMEM;CHECK(getaddrinfo("fixture.example",NULL,&h,&answer)==EAI_MEMORY);dns_error=0;
    h.ai_flags=0x8000;CHECK(getaddrinfo("a",NULL,&h,&answer)==EAI_BADFLAGS);h.ai_flags=0;h.ai_family=10;CHECK(getaddrinfo("a",NULL,&h,&answer)==EAI_FAMILY);
    h=(struct addrinfo){.ai_socktype=3};CHECK(getaddrinfo("a",NULL,&h,&answer)==EAI_SOCKTYPE);
    h=(struct addrinfo){0};CHECK(getaddrinfo("a","http",&h,&answer)==EAI_SERVICE && getaddrinfo("a","65536",&h,&answer)==EAI_SERVICE);
    CHECK(!strcmp(gai_strerror(EAI_AGAIN),"Temporary resolver failure"));freeaddrinfo(NULL);
    puts("PASS SDK socket records/errno, IPv4 conversion, select translation and getaddrinfo ownership/status/subset");
    return 0;
}
