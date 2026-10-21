/* Real pinned stack, native adapter and synthetic Ethernet server.
 * SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include "lwip/api.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/tcpip.h"
#include "lwip/dns.h"
#include "lwip/dhcp.h"
#include "lwip/inet_chksum.h"
#include "lwip/memp.h"
#include "lwip/stats.h"
#include <ciuki/net_stack.h>
#include <ciuki/files.h>
#include <ciuki/desktop.h>
#include <ciuki/signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#define CHECK(e) do { if (!(e)) { fprintf(stderr,"FAIL line %u: %s\n",__LINE__,#e); abort(); } } while (0)
struct task *g_current;
struct desktop_ledger desktop_objects;
static struct process client;
static struct proc_fd fds[CIUKI_OPEN_MAX];
static _Thread_local struct proc_thread thread;
static _Thread_local bool caught;
struct proc_thread *proc_thread_for(const struct task *t) { (void)t; thread.process=&client; return &thread; }
struct process *proc_current(void) { return &client; }
bool proc_signal_caught(struct proc_thread *t) { (void)t; return caught; }
void proc_signal_pipe(struct proc_thread *t) { (void)t; }
struct proc_object *file_fd_object(const struct process *p,int32_t fd) { return fd<0 || fd>=CIUKI_OPEN_MAX ? NULL : p->fds[fd].object; }
int file_fd_slot(const struct process *p,unsigned start) { for (unsigned i=start;i<CIUKI_OPEN_MAX;i++) if (!p->fds[i].object) return (int)i; return -EMFILE; }
static void reserved_ref(struct proc_object *o) { (void)o; }
static struct proc_object reserved={reserved_ref,reserved_ref,false};
int file_fd_reserve(struct process *p) { int fd=file_fd_slot(p,0);if(fd>=0)p->fds[fd]=(struct proc_fd){&reserved,0};return fd; }
int file_fd_publish(struct process *p,int fd,struct proc_object *o,uint32_t flags) { if(p->state==PROC_STOPPING||p->fds[fd].object!=&reserved)return -EBADF;p->fds[fd]=(struct proc_fd){o,flags};net_fd_changed(p,(unsigned)fd);return fd; }
void file_fd_cancel(struct process *p,int fd) { if(p->fds[fd].object==&reserved)p->fds[fd]=(struct proc_fd){0}; }
struct file_description *file_fd(const struct process *p,int32_t fd) { (void)p; (void)fd; return NULL; }
struct desktop_description *desktop_fd(const struct process *p,int32_t fd) { (void)p; (void)fd; return NULL; }
uint32_t file_description_count(void) { return 0; }
int file_close(struct process *p,int32_t fd)
{
    struct proc_object *o=file_fd_object(p,fd); if (!o) return -EBADF;
    p->fds[fd]=(struct proc_fd){0}; net_fd_changed(p,(unsigned)fd); o->release(o); return 0;
}
void input_snapshot(struct input_stats *out) { memset(out,0,sizeof(*out)); }
static unsigned char user[65536];
int copy_from_user(void *out,uint32_t pointer,uint32_t bytes)
{ if (pointer<4096 || bytes>sizeof(user) || pointer>sizeof(user)-bytes) return -EFAULT; memcpy(out,user+pointer,bytes); return 0; }
int copy_to_user(uint32_t pointer,const void *in,uint32_t bytes)
{ if (pointer<4096 || bytes>sizeof(user) || pointer>sizeof(user)-bytes) return -EFAULT; memcpy(user+pointer,in,bytes); return 0; }
int ua_pin(struct uaddr *space,struct ua_pin *pin,uint32_t pointer,uint32_t bytes,bool write)
{ (void)space; (void)pin; (void)write; return pointer<4096 || pointer>sizeof(user) || bytes>sizeof(user)-pointer ? -EFAULT : 0; }
void ua_unpin(struct uaddr *space,struct ua_pin *pin) { (void)space; (void)pin; }
static unsigned get16(const unsigned char *p) { return (unsigned)p[0]*256+p[1]; }
static void put16(unsigned char *p,unsigned n) { p[0]=(unsigned char)(n>>8); p[1]=(unsigned char)n; }
static void put32(unsigned char *p,uint32_t n) { p[0]=(unsigned char)(n>>24);p[1]=(unsigned char)(n>>16);p[2]=(unsigned char)(n>>8);p[3]=(unsigned char)n; }
static unsigned dhcp_discover, dhcp_request, dns_queries, arp_requests;
static unsigned char client_mac[6]={2,0,0,0,0,2}, server_mac[6]={2,0,0,0,0,1};
static void udp_reply(const unsigned char *request,unsigned sport,unsigned dport,const void *data,unsigned bytes,bool broadcast)
{
    unsigned char f[1514]={0};
    memcpy(f,broadcast ? (const unsigned char *)"\xff\xff\xff\xff\xff\xff" : request+6,6);
    memcpy(f+6,server_mac,6); put16(f+12,0x800); unsigned char *ip=f+14;
    ip[0]=0x45; put16(ip+2,28+bytes); ip[8]=64; ip[9]=17; put32(ip+12,0xc0000201);
    if (broadcast) put32(ip+16,0xffffffff); else memcpy(ip+16,request+26,4);
    uint16_t check=inet_chksum(ip,20); memcpy(ip+10,&check,2);
    unsigned char *udp=ip+20; put16(udp,sport); put16(udp+2,dport); put16(udp+4,8+bytes);
    memcpy(udp+8,data,bytes); net_stack_input(0,f,42+bytes);
}
static unsigned dhcp_type(const unsigned char *b,unsigned bytes)
{
    unsigned i=240;
    while (i<bytes) { unsigned option=b[i++]; if (option==255) break; if (!option) continue;
        if (i>=bytes) break; unsigned n=b[i++]; if (n>bytes-i) break;
        if (option==53 && n==1) return b[i]; i+=n;
    }
    return 0;
}
int net_stack_transmit(unsigned index,const void *data,unsigned bytes)
{
    CHECK(index==0 && bytes>=14 && bytes<=1514); const unsigned char *f=data;
    if (get16(f+12)==0x806) {
        if (bytes>=42 && get16(f+20)==1 && !memcmp(f+38,"\xc0\x00\x02\x01",4)) {
            unsigned char a[60]={0}; memcpy(a,f+6,6);memcpy(a+6,server_mac,6);put16(a+12,0x806);
            memcpy(a+14,f+14,6);put16(a+20,2);memcpy(a+22,server_mac,6);put32(a+28,0xc0000201);
            memcpy(a+32,f+22,10);arp_requests++;net_stack_input(0,a,60);
        }
        return 0;
    }
    if (get16(f+12)!=0x800 || bytes<42 || f[23]!=17) return 0;
    CHECK(inet_chksum(f+14,20)==0);
    unsigned port=get16(f+36), length=get16(f+38); if (length<8 || length>bytes-34) return 0;
    const unsigned char *body=f+42; unsigned size=length-8;
    if (port==67 && size>=240) {
        unsigned type=dhcp_type(body,size); if (type!=1 && type!=3) return 0;
        if (type==1) dhcp_discover++; else dhcp_request++;
        unsigned char b[320]={0}; b[0]=2;b[1]=1;b[2]=6;memcpy(b+4,body+4,4);put32(b+16,0xc0000264);
        memcpy(b+28,body+28,16);put32(b+236,0x63825363);
        unsigned n=240; const unsigned char options[]={53,1,0,54,4,192,0,2,1,1,4,255,255,255,0,3,4,192,0,2,1,6,4,192,0,2,1,51,4,0,0,0,120,58,4,0,0,0,60,59,4,0,0,0,105,255};
        memcpy(b+n,options,sizeof(options));b[n+2]=type==1 ? 2 : 5;n+=sizeof(options);
        udp_reply(f,67,68,b,n,true);
    } else if (port==53 && size>16) {
        dns_queries++;
        unsigned char answer[512]={0};CHECK(size+16<sizeof(answer));memcpy(answer,body,size);
        answer[2]=0x81;answer[3]=0x80;put16(answer+6,1);
        unsigned char *a=answer+size;a[0]=0xc0;a[1]=12;put16(a+2,1);put16(a+4,1);put32(a+6,60);put16(a+10,4);put32(a+12,0xcb007109);
        /* Wrong transaction ID and a truncated option are rejected first. */
        answer[0]^=1;udp_reply(f,53,get16(f+34),answer,size+16,false);answer[0]^=1;
        udp_reply(f,53,get16(f+34),answer,size+16,false);
    }
    return 0;
}
static int call(unsigned nr,unsigned b,unsigned c,unsigned d,unsigned s)
{ struct trap_frame tf={.eax=nr,.ebx=b,.ecx=c,.edx=d,.esi=s};return net_socket_call(&tf); }
static struct ciuki_sockaddr_in address(unsigned port)
{ return (struct ciuki_sockaddr_in){.family=AF_INET,.port=lwip_htons((uint16_t)port),.address=PP_HTONL(0x7f000001)}; }
static void drain(void)
{
    uint64_t end=cu_now()+4000;
    while (net_socket_count()) { CHECK(cu_now()<end);cu_sleep(1); }
}
static void sem_thread(void *arg) { cu_sleep(5);sys_sem_signal(arg); }
static void mailbox_thread(void *arg) { sys_mbox_post(arg,(void *)(uintptr_t)3); }
static void arch_tests(void)
{
    sys_sem_t sem;CHECK(sys_sem_new(&sem,0)==ERR_OK);
    CHECK(sys_arch_sem_wait(&sem,3)==SYS_ARCH_TIMEOUT);
    sys_thread_t id=sys_thread_new("sem-test",sem_thread,&sem,32768,0);CHECK(id);
    CHECK(sys_arch_sem_wait(&sem,0)!=SYS_ARCH_TIMEOUT);cu_host_join(id);
    sys_sem_signal(&sem);sys_sem_signal(&sem);CHECK(sys_arch_sem_wait(&sem,1)!=SYS_ARCH_TIMEOUT);
    CHECK(sys_arch_sem_wait(&sem,1)==SYS_ARCH_TIMEOUT);sys_sem_free(&sem);CHECK(!sys_sem_valid(&sem));
    sys_mbox_t box;CHECK(sys_mbox_new(&box,2)==ERR_OK);
    CHECK(sys_mbox_trypost(&box,NULL)==ERR_OK);CHECK(sys_mbox_trypost(&box,(void *)(uintptr_t)2)==ERR_OK);
    CHECK(sys_mbox_trypost(&box,(void *)(uintptr_t)3)==ERR_MEM);
    id=sys_thread_new("mbox-test",mailbox_thread,&box,32768,0);CHECK(id);cu_sleep(2);
    void *value=(void *)(uintptr_t)9;CHECK(sys_arch_mbox_fetch(&box,&value,0)!=SYS_ARCH_TIMEOUT && !value);
    CHECK(sys_arch_mbox_fetch(&box,&value,0)!=SYS_ARCH_TIMEOUT && value==(void *)(uintptr_t)2);
    CHECK(sys_arch_mbox_fetch(&box,&value,0)!=SYS_ARCH_TIMEOUT && value==(void *)(uintptr_t)3);
    cu_host_join(id);CHECK(sys_arch_mbox_tryfetch(&box,&value)==SYS_MBOX_EMPTY);
    CHECK(sys_arch_mbox_fetch(&box,&value,2)==SYS_ARCH_TIMEOUT);sys_mbox_free(&box);
    uint32_t before=sys_now();cu_host_clock_offset(UINT32_MAX-1u-before);before=sys_now();cu_sleep(5);uint32_t after=sys_now();CHECK((uint32_t)(after-before)>=4 && after<10);cu_host_clock_offset(0);
    puts("PASS sys_arch FIFO/full/null/blocking-post/semaphore/wrap");
}
static void udp_tests(void)
{
    int a=net_socket_create(&client,AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0),b=net_socket_create(&client,AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0);
    CHECK(a>=0 && b>=0);struct ciuki_sockaddr_in target=address(30000);CHECK(!net_socket_bind(&client,b,&target));
    CHECK(net_socket_io(&client,b,user,3,MSG_DONTWAIT,NULL,false)==-EAGAIN);
    CHECK(net_socket_io(&client,a,"abcde",5,0,&target,true)==5);
    struct ciuki_pollfd p={b,POLLIN,0};CHECK(net_poll(&client,&p,1,1000)==1 && (p.revents&POLLIN));
    char text[16]={0};CHECK(net_socket_io(&client,b,text,2,MSG_PEEK,NULL,false)==2 && !memcmp(text,"ab",2));
    struct ciuki_socket_io io={.size=40,.version=1,.fd=b,.buffer=1,.bytes=2};memcpy(user+4096,&io,sizeof(io));
    CHECK(call(CIUKI_SYS_RECVFROM,4096,0,0,0)==-EFAULT);
    io.buffer=8192;memcpy(user+4096,&io,sizeof(io));CHECK(call(CIUKI_SYS_RECVFROM,4096,0,0,0)==2 && !memcmp(user+8192,"ab",2));
    CHECK(net_socket_io(&client,b,text,16,MSG_DONTWAIT,NULL,false)==-EAGAIN);
    CHECK(net_socket_io(&client,a,text,0,0,&target,true)==0);p.revents=0;CHECK(net_poll(&client,&p,1,1000)==1);
    CHECK(net_socket_io(&client,b,text,16,0,NULL,false)==0);
    CHECK(net_socket_io(&client,a,text,1473,0,&target,true)==-EMSGSIZE);
    CHECK(net_socket_io(&client,a,text,1,MSG_PEEK,&target,true)==-EINVAL);
    CHECK(net_socket_shutdown(&client,a,SHUT_RDWR)==-EOPNOTSUPP);
    CHECK(!file_close(&client,a) && !file_close(&client,b));drain();
    puts("PASS native UDP loopback/peek/truncation/zero-datagram/fault-before-consume/bounds");
}
static void tcp_tests(void)
{
    int listener=net_socket_create(&client,AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0);
    int sender=net_socket_create(&client,AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0);CHECK(listener>=0 && sender>=0);
    struct ciuki_sockaddr_in a=address(30001);CHECK(!net_socket_bind(&client,listener,&a));CHECK(!net_socket_listen(&client,listener,8));
    CHECK(net_socket_connect(&client,sender,&a)==-EINPROGRESS);
    struct ciuki_pollfd ready={sender,POLLOUT,0};CHECK(net_poll(&client,&ready,1,1000)==1);
    int error=-1;unsigned bytes=4;CHECK(!net_socket_option(&client,sender,0,SOL_SOCKET,SO_ERROR,&error,&bytes) && !error);
    ready=(struct ciuki_pollfd){listener,POLLIN,0};CHECK(net_poll(&client,&ready,1,1000)==1);
    int receiver=net_socket_accept(&client,listener,&a);CHECK(receiver>=0);CHECK(*net_socket_flags(fds[receiver].object)==O_RDWR);
    char text[128]={0};CHECK(net_socket_io(&client,sender,"hello world",11,0,NULL,true)==11);
    ready=(struct ciuki_pollfd){receiver,POLLIN,0};CHECK(net_poll(&client,&ready,1,1000)==1);
    CHECK(net_socket_io(&client,receiver,text,5,MSG_PEEK,NULL,false)==5 && !memcmp(text,"hello",5));
    CHECK(net_socket_io(&client,receiver,text,3,0,NULL,false)==3 && !memcmp(text,"hel",3));
    CHECK(net_socket_io(&client,receiver,text,128,0,NULL,false)==8 && !memcmp(text,"lo world",8));
    CHECK(!net_socket_shutdown(&client,sender,SHUT_WR));
    CHECK(net_socket_io(&client,sender,text,1,MSG_NOSIGNAL,NULL,true)==-EPIPE);
    CHECK(net_socket_io(&client,receiver,text,128,0,NULL,false)==0);
    CHECK(!file_close(&client,sender));CHECK(!file_close(&client,receiver));CHECK(!file_close(&client,listener));drain();
    puts("PASS native TCP handshake/nonblocking/poll/accept/partial-tail/peek/FIN/shutdown");
}
static void refusal_tests(void)
{
    CHECK(net_socket_create(&client,10,SOCK_STREAM,0)==-EAFNOSUPPORT);
    CHECK(net_socket_create(&client,AF_INET,3,0)==-ESOCKTNOSUPPORT);
    CHECK(net_socket_create(&client,AF_INET,SOCK_STREAM,17)==-EPROTONOSUPPORT);
    CHECK(call(CIUKI_SYS_NETCTL,0,0,0,0)==-ENOSYS);
    struct ciuki_pollfd entries[3]={{-1,POLLIN,7},{127,POLLIN,7},{127,POLLOUT,7}};
    CHECK(net_poll(&client,entries,3,0)==2 && !entries[0].revents && entries[1].revents==POLLNVAL);
    entries[0].events=0x200;CHECK(net_poll(&client,entries,1,0)==-EINVAL);
    uint64_t start=cu_now();CHECK(!net_poll(&client,NULL,0,3) && cu_now()-start>=3);
    caught=true;CHECK(net_poll(&client,NULL,0,-1)==-EINTR);caught=false;
    int many[16];for (unsigned i=0;i<16;i++) { many[i]=net_socket_create(&client,AF_INET,SOCK_DGRAM,0);CHECK(many[i]>=0); }
    CHECK(net_socket_create(&client,AF_INET,SOCK_DGRAM,0)==-EMFILE);
    for (unsigned i=0;i<16;i++) CHECK(!file_close(&client,many[i]));drain();
    puts("PASS native errors/poll bounds/duplicates/negative fds/timeouts/signals/socket limits");
}
static void dns_dhcp_tests(void)
{
    CHECK(!net_stack_attach(0,client_mac,true));uint32_t ip=0;uint64_t end=cu_now()+12000;
    do { net_stack_address(0,&ip);if (ip) break;cu_sleep(10); } while (cu_now()<end);
    CHECK(ip==PP_HTONL(0xc0000264) && dhcp_discover && dhcp_request);
    int status=-1;uint32_t answer=0;CHECK(!net_resolve(&client,"fixture.example",&answer,&status));
    CHECK(!status && answer==PP_HTONL(0xcb007109) && dns_queries && arp_requests);
    CHECK(net_resolve(&client,"bad..name",&answer,&status)==-EINVAL);
    puts("PASS real DHCP DISCOVER/OFFER/REQUEST/ACK+ACD and DNS query/ID rejection/A answer");
}
static void cycles(void)
{
    for (unsigned i=0;i<10000;i++) {
        int fd;do { fd=net_socket_create(&client,AF_INET,SOCK_DGRAM,0);if(fd==-ENOBUFS)cu_sleep(1); }while(fd==-ENOBUFS);
        CHECK(fd>=0);CHECK(!file_close(&client,fd));
    }
    drain();puts("PASS 10000 create/close cycles; no public objects retained");
}
int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    client=(struct process){.pid=1,.state=PROC_LIVE,.fds=fds};arch_tests();
    ciuki_lwip_seed(0x12345678);CHECK(!net_stack_init());
    refusal_tests();udp_tests();tcp_tests();cycles();dns_dhcp_tests();
    extern unsigned cu_arch_bytes(void),cu_socket_bytes(void),cu_socket_object_bytes(void),cu_request_bytes(void);
    printf("MEASURE host pbuf=%zu tcp_pcb=%zu udp_pcb=%zu netconn=%zu socket=%u arch=%u socket_metadata=%u request=%u\n",
        sizeof(struct pbuf),sizeof(struct tcp_pcb),sizeof(struct udp_pcb),sizeof(struct netconn),cu_socket_object_bytes(),cu_arch_bytes(),cu_socket_bytes(),cu_request_bytes());
    for (unsigned i=0;i<MEMP_MAX;i++) printf("POOL %u size=%u count=%u bytes=%u\n",i,memp_pools[i]->size,memp_pools[i]->num,(unsigned)memp_pools[i]->size*memp_pools[i]->num);
    puts("PASS f6-10 focused real-stack host harness (ASan+UBSan)");cu_host_shutdown();return 0;
}
