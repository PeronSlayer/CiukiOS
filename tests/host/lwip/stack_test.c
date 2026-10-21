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
#include "lwip/mem.h"
#include <ciuki/net_stack.h>
#include <ciuki/files.h>
#include <ciuki/desktop.h>
#include <ciuki/signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#define CHECK(e) do { if (!(e)) { fprintf(stderr,"FAIL line %u: %s\n",__LINE__,#e); abort(); } } while (0)
struct task *g_current;
struct desktop_ledger desktop_objects;
static struct process client;
static struct proc_fd fds[CIUKI_OPEN_MAX];
static _Thread_local struct proc_thread thread;
static _Thread_local bool caught;
static _Atomic unsigned live_pins;
static struct file_description regular, directory;
static struct desktop_description input_description, unsupported_description;
static struct desktop_description *channel_description;
static _Atomic unsigned input_pending;
struct proc_thread *proc_thread_for(const struct task *t) { (void)t; thread.process=&client; return &thread; }
struct process *proc_current(void) { return &client; }
bool proc_signal_caught(struct proc_thread *t) { return t->pending || (t==&thread && caught); }
void proc_signal_pipe(struct proc_thread *t) { (void)t; }
struct proc_object *file_fd_object(const struct process *p,int32_t fd) { return fd<0 || fd>=CIUKI_OPEN_MAX ? NULL : p->fds[fd].object; }
int file_fd_slot(const struct process *p,unsigned start) { for (unsigned i=start;i<CIUKI_OPEN_MAX;i++) if (!p->fds[i].object) return (int)i; return -EMFILE; }
static void reserved_ref(struct proc_object *o) { (void)o; }
static struct proc_object reserved={reserved_ref,reserved_ref,false};
int file_fd_reserve(struct process *p) { int fd=file_fd_slot(p,0);if(fd>=0)p->fds[fd]=(struct proc_fd){&reserved,0};return fd; }
int file_fd_publish(struct process *p,int fd,struct proc_object *o,uint32_t flags) { if(p->state==PROC_STOPPING||p->fds[fd].object!=&reserved)return -EBADF;p->fds[fd]=(struct proc_fd){o,flags};net_fd_changed(p,(unsigned)fd);return fd; }
void file_fd_cancel(struct process *p,int fd) { if(p->fds[fd].object==&reserved)p->fds[fd]=(struct proc_fd){0}; }
struct file_description *file_fd(const struct process *p,int32_t fd) {
    struct proc_object *o=file_fd_object(p,fd);
    return o==&regular.object ? &regular : o==&directory.object ? &directory : NULL;
}
struct desktop_description *desktop_fd(const struct process *p,int32_t fd) {
    struct proc_object *o=file_fd_object(p,fd);
    return o==&input_description.object ? &input_description : o==&unsupported_description.object ? &unsupported_description :
        channel_description && o==&channel_description->object ? channel_description : NULL;
}
uint32_t file_description_count(void) { return 0; }
int file_close(struct process *p,int32_t fd)
{
    struct proc_object *o=file_fd_object(p,fd); if (!o) return -EBADF;
    p->fds[fd]=(struct proc_fd){0}; net_fd_changed(p,(unsigned)fd); o->release(o); return 0;
}
void input_snapshot(struct input_stats *out) { memset(out,0,sizeof(*out));out->pending=input_pending; }
static unsigned char user[65536];
static void (*access_hook)(void);
static uint32_t access_pointer;
static bool access_on_pin;
static void user_access(uint32_t pointer, bool pinning)
{
    if (access_hook && pointer==access_pointer && pinning==access_on_pin) { void (*fn)(void)=access_hook;access_hook=NULL;fn(); }
}
int copy_from_user(void *out,uint32_t pointer,uint32_t bytes)
{ if (pointer<4096 || bytes>sizeof(user) || pointer>sizeof(user)-bytes) return -EFAULT; memcpy(out,user+pointer,bytes);user_access(pointer,false); return 0; }
int copy_to_user(uint32_t pointer,const void *in,uint32_t bytes)
{ if (pointer<4096 || bytes>sizeof(user) || pointer>sizeof(user)-bytes) return -EFAULT; memcpy(user+pointer,in,bytes); return 0; }
int ua_pin(struct uaddr *space,struct ua_pin *pin,uint32_t pointer,uint32_t bytes,bool write)
{ (void)space; (void)pin; (void)write; if(pointer<4096 || pointer>sizeof(user) || bytes>sizeof(user)-pointer)return -EFAULT;live_pins++;user_access(pointer,true);return 0; }
void ua_unpin(struct uaddr *space,struct ua_pin *pin) { (void)space; (void)pin; CHECK(live_pins);live_pins--; }
static unsigned get16(const unsigned char *p) { return (unsigned)p[0]*256+p[1]; }
static void put16(unsigned char *p,unsigned n) { p[0]=(unsigned char)(n>>8); p[1]=(unsigned char)n; }
static void put32(unsigned char *p,uint32_t n) { p[0]=(unsigned char)(n>>24);p[1]=(unsigned char)(n>>16);p[2]=(unsigned char)(n>>8);p[3]=(unsigned char)n; }
static _Atomic unsigned dhcp_discover, dhcp_request, dns_queries, arp_requests;
static _Atomic bool dns_drop, dhcp_drop, dhcp_nak, arp_conflict;
static unsigned char last_dns[1514];
static unsigned last_dns_bytes;
static _Atomic unsigned dhcp_declines;
static unsigned lease_dns_count=2;
static unsigned wire_syns, wire_packets, wire_data_transmits;
static uint32_t wire_ack, wire_first_seq;
static unsigned wire_first_count;
static unsigned char output_frame[1514];
static unsigned output_bytes;
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
static uint32_t get32(const unsigned char *p) { return (uint32_t)get16(p)<<16|get16(p+2); }
static void tcp_reply(const unsigned char *request,uint32_t sequence,uint32_t ack,unsigned flags,const char *data,unsigned bytes)
{
    unsigned char f[1514]={0};memcpy(f,request+6,6);memcpy(f+6,server_mac,6);put16(f+12,0x800);
    unsigned char *ip=f+14;ip[0]=0x45;put16(ip+2,40+bytes);ip[8]=64;ip[9]=6;
    memcpy(ip+12,request+30,4);memcpy(ip+16,request+26,4);uint16_t check=inet_chksum(ip,20);memcpy(ip+10,&check,2);
    unsigned char *tcp=ip+20;memcpy(tcp,request+36,2);memcpy(tcp+2,request+34,2);
    put32(tcp+4,sequence);put32(tcp+8,ack);tcp[12]=0x50;tcp[13]=(unsigned char)flags;put16(tcp+14,16384);
    if(bytes)memcpy(tcp+20,data,bytes);
    struct pbuf *p=pbuf_alloc(PBUF_RAW,(u16_t)(20+bytes),PBUF_RAM);CHECK(p && pbuf_take(p,tcp,(u16_t)(20+bytes))==ERR_OK);
    ip4_addr_t src,dst;memcpy(&src.addr,ip+12,4);memcpy(&dst.addr,ip+16,4);
    check=inet_chksum_pseudo(p,6,(u16_t)(20+bytes),&src,&dst);memcpy(tcp+16,&check,2);pbuf_free(p);
    net_stack_input(0,f,54+bytes);
}
int net_stack_transmit(unsigned index,const void *data,unsigned bytes)
{
    CHECK(index==0 && bytes>=14 && bytes<=1514); const unsigned char *f=data;
    memcpy(output_frame,data,bytes);output_bytes=bytes;
    if (get16(f+12)==0x806) {
        if (arp_conflict && bytes>=42 && get16(f+20)==1 && !memcmp(f+38,"\xc0\x00\x02\x64",4)) {
            unsigned char a[60]={0}; memcpy(a,f+6,6);memcpy(a+6,server_mac,6);put16(a+12,0x806);
            memcpy(a+14,f+14,6);put16(a+20,2);memcpy(a+22,server_mac,6);put32(a+28,0xc0000264);
            memcpy(a+32,client_mac,6);net_stack_input(0,a,60);
        }
        if (bytes>=42 && get16(f+20)==1 && !memcmp(f+38,"\xc0\x00\x02\x01",4)) {
            unsigned char a[60]={0}; memcpy(a,f+6,6);memcpy(a+6,server_mac,6);put16(a+12,0x806);
            memcpy(a+14,f+14,6);put16(a+20,2);memcpy(a+22,server_mac,6);put32(a+28,0xc0000201);
            memcpy(a+32,f+22,10);arp_requests++;net_stack_input(0,a,60);
        }
        return 0;
    }
    if(get16(f+12)==0x800 && bytes>=54 && f[23]==6 && get16(f+36)==33334) {
        CHECK(inet_chksum(f+14,20)==0);wire_packets++;
        unsigned flags=f[47],headers=(f[46]>>4)*4,length=get16(f+16)-20-headers;
        if(flags&2) { wire_syns++;if(wire_syns>1)tcp_reply(f,1000,get32(f+38)+1,0x12,NULL,0); }
        else if(length) {
            if(!wire_data_transmits){wire_first_seq=get32(f+38);wire_ack=wire_first_seq;}
            wire_data_transmits++;uint32_t end=get32(f+38)+length;
            if((int32_t)(end-wire_ack)>0)wire_ack=end;
            if(get32(f+38)==wire_first_seq)wire_first_count++;
            if(wire_first_count>1)tcp_reply(f,1001,wire_ack,0x19,"wire",4);
        } else if(flags&1)tcp_reply(f,1006,get32(f+38)+1,0x10,NULL,0);
        return 0;
    }
    if (get16(f+12)!=0x800 || bytes<42 || f[23]!=17) return 0;
    CHECK(inet_chksum(f+14,20)==0);
    unsigned port=get16(f+36), length=get16(f+38); if (length<8 || length>bytes-34) return 0;
    const unsigned char *body=f+42; unsigned size=length-8;
    if (port==67 && size>=240) {
        unsigned type=dhcp_type(body,size); if(type==4)dhcp_declines++;if (type!=1 && type!=3) return 0;
        if (type==1) dhcp_discover++; else dhcp_request++;
        if(dhcp_drop)return 0;
        unsigned char b[320]={0}; b[0]=2;b[1]=1;b[2]=6;memcpy(b+4,body+4,4);put32(b+16,0xc0000264);
        memcpy(b+28,body+28,16);put32(b+236,0x63825363);
        unsigned n=240; const unsigned char options[]={53,1,0,54,4,192,0,2,1,1,4,255,255,255,0,3,4,192,0,2,1,6,4,192,0,2,1,51,4,0,0,0,240,58,4,0,0,0,60,59,4,0,0,0,120,255};
        memcpy(b+n,options,sizeof(options));b[n+2]=type==1 ? 2 : dhcp_nak ? 6 : 5;n+=sizeof(options);
        if (lease_dns_count==2) {
            /* Option 6 starts at byte 261; insert the second server. */
            memmove(b+271,b+267,n-267);put32(b+267,0xc0000202);b[262]=8;n+=4;
        } else if (!lease_dns_count) {
            memmove(b+261,b+267,n-267);n-=6;
        }
        /* Truncated option length: rejected without installing an offer. */
        unsigned char bad[320];memcpy(bad,b,n);bad[241]=250;udp_reply(f,67,68,bad,243,true);
        udp_reply(f,67,68,b,n,true);
    } else if(port==33336) {
        udp_reply(f,33336,get16(f+34),"two",3,false);udp_reply(f,33336,get16(f+34),"one",3,false);
    } else if (port==53 && size>16) {
        dns_queries++;
        memcpy(last_dns,f,bytes);last_dns_bytes=bytes;
        if(dns_drop)return 0;
        unsigned char answer[512]={0};CHECK(size+16<sizeof(answer));memcpy(answer,body,size);
        answer[2]=0x81;answer[3]=0x80;put16(answer+6,1);
        unsigned char *a=answer+size;a[0]=0xc0;a[1]=12;put16(a+2,1);put16(a+4,1);put32(a+6,60);put16(a+10,4);put32(a+12,0xcb007109);
        /* Wrong transaction ID and a truncated option are rejected first. */
        answer[0]^=1;udp_reply(f,53,get16(f+34),answer,size+16,false);answer[0]^=1;
        udp_reply(f,54,get16(f+34),answer,size+16,false);
        udp_reply(f,53,get16(f+34),answer,size+5,false);
        unsigned char invalid[512];memcpy(invalid,answer,size+16);invalid[size+1]=0xff;
        udp_reply(f,53,get16(f+34),invalid,size+16,false);
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
    while (net_socket_count()) {
        if (cu_now()>=end) { extern void cu_socket_dump(void); cu_socket_dump(); CHECK(false); }
        cu_sleep(1);
    }
}
static void sem_thread(void *arg) { cu_sleep(5);sys_sem_signal(arg); }
static void mailbox_thread(void *arg) { sys_mbox_post(arg,(void *)(uintptr_t)3); }
static unsigned mutex_total;
static void mutex_thread(void *arg)
{ for(unsigned i=0;i<1000;i++){sys_mutex_lock(arg);mutex_total++;sys_mutex_unlock(arg);} }
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
    sys_mutex_t mutex;CHECK(sys_mutex_new(&mutex)==ERR_OK);
    sys_thread_t first=sys_thread_new("mutex-a",mutex_thread,&mutex,32768,0);
    sys_thread_t second=sys_thread_new("mutex-b",mutex_thread,&mutex,32768,0);
    CHECK(first && second);cu_host_join(first);cu_host_join(second);CHECK(mutex_total==2000);sys_mutex_free(&mutex);
    sys_sem_t semaphores[128],overflow;for(unsigned i=0;i<128;i++)CHECK(sys_sem_new(&semaphores[i],0)==ERR_OK);
    CHECK(sys_sem_new(&overflow,0)==ERR_MEM && !overflow);for(unsigned i=0;i<128;i++)sys_sem_free(&semaphores[i]);
    sys_mbox_t boxes[112],extra;for(unsigned i=0;i<112;i++)CHECK(sys_mbox_new(&boxes[i],64)==ERR_OK);
    CHECK(sys_mbox_new(&extra,1)==ERR_MEM && !extra);for(unsigned i=0;i<112;i++)sys_mbox_free(&boxes[i]);
    uint32_t before=sys_now();cu_host_clock_offset(UINT32_MAX-1u-before);before=sys_now();cu_sleep(5);uint32_t after=sys_now();CHECK((uint32_t)(after-before)>=4 && after<10);cu_host_clock_offset(0);
    puts("PASS sys_arch FIFO/full/null/blocking-post/semaphore/wrap/mutex contention/pool exhaustion");
}
static void udp_tests(void)
{
    int a=call(CIUKI_SYS_SOCKET,AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0,0),b=call(CIUKI_SYS_SOCKET,AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0,0);
    CHECK(a>=0 && b>=0);struct ciuki_sockaddr_in target=address(30000);CHECK(!net_socket_bind(&client,b,&target,NULL));
    CHECK(net_socket_io(&client,b,user,3,MSG_DONTWAIT,NULL,false,NULL)==-EAGAIN);
    memcpy(user+6000,"abcde",5);memcpy(user+7000,&target,16);
    struct ciuki_socket_io sendio={.size=40,.version=1,.fd=a,.buffer=6000,.bytes=5,.address=7000,.address_length=16};
    memcpy(user+4096,&sendio,40);CHECK(call(CIUKI_SYS_SENDTO,4096,0,0,0)==5);
    struct ciuki_pollfd p={b,POLLIN,0};CHECK(net_poll(&client,&p,1,1000,NULL)==1 && (p.revents&POLLIN));
    char text[16]={0};CHECK(net_socket_io(&client,b,text,2,MSG_PEEK,NULL,false,NULL)==2 && !memcmp(text,"ab",2));
    struct ciuki_socket_io io={.size=40,.version=1,.fd=b,.buffer=1,.bytes=2};memcpy(user+4096,&io,sizeof(io));
    CHECK(call(CIUKI_SYS_RECVFROM,4096,0,0,0)==-EFAULT);
    io.buffer=8192;memcpy(user+4096,&io,sizeof(io));CHECK(call(CIUKI_SYS_RECVFROM,4096,0,0,0)==2 && !memcmp(user+8192,"ab",2));
    CHECK(net_socket_io(&client,b,text,16,MSG_DONTWAIT,NULL,false,NULL)==-EAGAIN);
    CHECK(net_socket_io(&client,a,text,0,0,&target,true,NULL)==0);p.revents=0;CHECK(net_poll(&client,&p,1,1000,NULL)==1);
    CHECK(net_socket_io(&client,b,text,16,0,NULL,false,NULL)==0);
    CHECK(net_socket_io(&client,a,text,1473,0,&target,true,NULL)==-EMSGSIZE);
    CHECK(net_socket_io(&client,a,text,1,MSG_PEEK,&target,true,NULL)==-EINVAL);
    struct ciuki_sockaddr_in disconnect={0};CHECK(!net_socket_connect(&client,a,&disconnect,NULL));
    CHECK(net_socket_io(&client,a,text,1,0,NULL,true,NULL)==-EDESTADDRREQ);
    CHECK(net_socket_shutdown(&client,a,SHUT_RDWR,NULL)==-EOPNOTSUPP);
    CHECK(!file_close(&client,a) && !file_close(&client,b));drain();
    puts("PASS native UDP loopback/peek/truncation/zero-datagram/fault-before-consume/bounds");
}
static void tcp_tests(void)
{
    int listener=call(CIUKI_SYS_SOCKET,AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0,0);
    int sender=call(CIUKI_SYS_SOCKET,AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0,0);CHECK(listener>=0 && sender>=0);
    struct ciuki_sockaddr_in a=address(30001);CHECK(!net_socket_bind(&client,listener,&a,NULL));CHECK(!net_socket_listen(&client,listener,8,NULL));
    CHECK(net_socket_connect(&client,sender,&a,NULL)==-EINPROGRESS);
    struct ciuki_pollfd ready={sender,POLLOUT,0};CHECK(net_poll(&client,&ready,1,1000,NULL)==1);
    int error=-1;unsigned bytes=4;CHECK(!net_socket_option(&client,sender,0,SOL_SOCKET,SO_ERROR,&error,&bytes,NULL) && !error);
    ready=(struct ciuki_pollfd){listener,POLLIN,0};CHECK(net_poll(&client,&ready,1,1000,NULL)==1);
    unsigned capacity=16;memcpy(user+6000,&capacity,4);
    CHECK(call(CIUKI_SYS_ACCEPT,(unsigned)listener,1,6000,0)==-EFAULT);
    int receiver=call(CIUKI_SYS_ACCEPT,(unsigned)listener,0,0,0);CHECK(receiver>=0);CHECK(*net_socket_flags(fds[receiver].object)==O_RDWR);
    char text[128]={0};CHECK(net_socket_io(&client,sender,"hello world",11,0,NULL,true,NULL)==11);
    ready=(struct ciuki_pollfd){receiver,POLLIN,0};CHECK(net_poll(&client,&ready,1,1000,NULL)==1);
    CHECK(net_socket_io(&client,receiver,text,5,MSG_PEEK,NULL,false,NULL)==5 && !memcmp(text,"hello",5));
    CHECK(net_socket_io(&client,receiver,text,3,0,NULL,false,NULL)==3 && !memcmp(text,"hel",3));
    CHECK(net_socket_io(&client,receiver,text,128,0,NULL,false,NULL)==8 && !memcmp(text,"lo world",8));
    CHECK(net_socket_io(&client,sender,text,0,0,NULL,true,NULL)==0);
    CHECK(net_socket_io(&client,receiver,text,0,0,NULL,false,NULL)==0);
    CHECK(!net_socket_shutdown(&client,sender,SHUT_WR,NULL));
    CHECK(net_socket_io(&client,sender,text,1,MSG_NOSIGNAL,NULL,true,NULL)==-EPIPE);
    CHECK(net_socket_io(&client,receiver,text,128,0,NULL,false,NULL)==0);
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
    CHECK(net_poll(&client,entries,3,0,NULL)==2 && !entries[0].revents && entries[1].revents==POLLNVAL);
    entries[0].events=0x200;CHECK(net_poll(&client,entries,1,0,NULL)==-EINVAL);
    uint64_t start=cu_now();CHECK(!net_poll(&client,NULL,0,3,NULL) && cu_now()-start>=3);
    caught=true;CHECK(net_poll(&client,NULL,0,-1,NULL)==-EINTR);caught=false;
    int many[16];for (unsigned i=0;i<16;i++) { many[i]=net_socket_create(&client,AF_INET,SOCK_DGRAM,0);CHECK(many[i]>=0); }
    CHECK(net_socket_create(&client,AF_INET,SOCK_DGRAM,0)==-EMFILE);
    for (unsigned i=0;i<16;i++) CHECK(!file_close(&client,many[i]));drain();
    puts("PASS native errors/poll bounds/duplicates/negative fds/timeouts/signals/socket limits");
}
static void limits_tests(void)
{
    struct process owners[4];struct proc_fd tables[4][CIUKI_OPEN_MAX]={0};int all[3][16];
    for(unsigned i=0;i<4;i++)owners[i]=(struct process){.pid=i+10,.state=PROC_LIVE,.fds=tables[i]};
    for(unsigned i=0;i<3;i++)for(unsigned j=0;j<16;j++){
        all[i][j]=net_socket_create(&owners[i],AF_INET,i==2 ? SOCK_DGRAM : SOCK_STREAM,0);CHECK(all[i][j]>=0);
    }
    CHECK(net_socket_count()==48);
    CHECK(net_socket_create(&owners[3],AF_INET,SOCK_DGRAM,0)==-ENOBUFS);
    CHECK(net_socket_create(&owners[3],AF_INET,SOCK_STREAM,0)==-ENOBUFS);
    for(unsigned i=0;i<3;i++)for(unsigned j=0;j<16;j++)CHECK(!file_close(&owners[i],all[i][j]));drain();
    int listeners[9];for(unsigned i=0;i<9;i++){
        listeners[i]=net_socket_create(&client,AF_INET,SOCK_STREAM,0);CHECK(listeners[i]>=0);
        CHECK(net_socket_listen(&client,listeners[i],1,NULL)==(i<8 ? 0 : -ENOBUFS));
    }
    for(unsigned i=0;i<9;i++)CHECK(!file_close(&client,listeners[i]));drain();
    puts("PASS 48 global/32 TCP/16 UDP/eight listener resource limits across processes");
}
static void mixed_tests(void)
{
    extern struct desktop_description *cu_host_channel(unsigned,unsigned,bool);
    int fd=net_socket_create(&client,AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0);CHECK(fd==0);
    struct px_node file_node={.kind=PX_FILE},dir_node={.kind=PX_DEV};
    regular=(struct file_description){.object={reserved_ref,reserved_ref,true},.node=&file_node,.flags=O_RDONLY};
    directory=(struct file_description){.object={reserved_ref,reserved_ref,true},.node=&dir_node,.flags=O_RDONLY};
    input_description=(struct desktop_description){.object={reserved_ref,reserved_ref,true},.kind=DESKTOP_INPUT};
    unsupported_description=(struct desktop_description){.object={reserved_ref,reserved_ref,true},.kind=DESKTOP_SURFACE};
    channel_description=cu_host_channel(0,0,true);
    struct proc_object *objects[]={&regular.object,&directory.object,&channel_description->object,&input_description.object,&unsupported_description.object};
    for(unsigned i=0;i<5;i++)fds[i+1]=(struct proc_fd){objects[i],0};
    struct ciuki_pollfd query[6]={{0,POLLIN,0},{1,POLLIN|POLLOUT,0},{2,POLLIN,0},{3,POLLIN|POLLOUT,0},{4,POLLIN,0},{5,0,0}};
    CHECK(net_poll(&client,query,6,0,NULL)==4 && query[1].revents==POLLIN && query[2].revents==POLLIN &&
        query[3].revents==POLLOUT && query[5].revents==POLLNVAL);
    cu_host_channel(1,CIUKI_CHANNEL_QUEUE_MAX,false);input_pending=1;
    CHECK(net_poll(&client,query,6,0,NULL)==5 && query[3].revents==(POLLIN|POLLHUP) && query[4].revents==POLLIN);
    for(unsigned i=1;i<=5;i++)fds[i]=(struct proc_fd){0};input_pending=0;
    CHECK(!file_close(&client,fd));drain();
    puts("PASS common poll over UDP, regular file, /dev directory, production channel queue/peer close, input and non-pollable surface");
}
struct wait_job { int fd, kind, result; _Atomic bool entered; };
static void cancelled_user(void *unused)
{
    (void)unused;
    if(thread.cleanup)thread.cleanup(thread.operation);
    thread.cleanup=NULL;thread.operation=NULL;
}
static void *wait_job(void *arg)
{
    struct wait_job *j=arg;char b;
    pthread_cleanup_push(cancelled_user,NULL);
    j->entered=true;
    if(j->kind==1) {
        struct ciuki_pollfd f={j->fd,POLLIN,0};
        j->result=net_poll(&client,&f,1,5000,NULL);if(j->result==1)j->result=f.revents;
    } else if(j->kind==2) j->result=call(CIUKI_SYS_RECVFROM,4096,0,0,0);
    else if(j->kind==3)j->result=net_socket_accept(&client,j->fd,NULL,NULL);
    else j->result=net_socket_io(&client,j->fd,&b,1,0,NULL,false,NULL);
    pthread_cleanup_pop(0);return NULL;
}
static void lifecycle_tests(void)
{
    for(unsigned kind=0;kind<2;kind++) {
        int fd=net_socket_create(&client,AF_INET,SOCK_DGRAM,0);CHECK(fd>=0);
        struct wait_job job={.fd=fd,.kind=(int)kind};pthread_t t;CHECK(!pthread_create(&t,NULL,wait_job,&job));
        while(!job.entered)cu_sleep(1);cu_sleep(5);CHECK(!file_close(&client,fd));
        int reused=net_socket_create(&client,AF_INET,SOCK_DGRAM,0);CHECK(reused==fd);
        CHECK(!pthread_join(t,NULL));CHECK(job.result==(kind ? POLLNVAL : -EBADF));
        CHECK(!file_close(&client,reused));drain();
    }
    int fd=net_socket_create(&client,AF_INET,SOCK_DGRAM,0);CHECK(fd>=0);
    struct ciuki_socket_io io={.size=40,.version=1,.fd=fd,.buffer=8192,.bytes=1};memcpy(user+4096,&io,40);
    struct wait_job job={.fd=fd,.kind=2};pthread_t t;CHECK(!pthread_create(&t,NULL,wait_job,&job));
    uint64_t end=cu_now()+1000;while(!live_pins){CHECK(cu_now()<end);cu_sleep(1);}
    cu_sleep(5);CHECK(!pthread_cancel(t) && !pthread_join(t,NULL));CHECK(!live_pins);
    CHECK(!file_close(&client,fd));drain();
    fd=net_socket_create(&client,AF_INET,SOCK_DGRAM,0);CHECK(fd>=0);
    struct ciuki_socket_timeval timeout={.microseconds=1};unsigned length=sizeof(timeout);
    CHECK(!net_socket_option(&client,fd,1,SOL_SOCKET,SO_RCVTIMEO,&timeout,&length,NULL));
    char b;uint64_t start=cu_now();CHECK(net_socket_io(&client,fd,&b,1,0,NULL,false,NULL)==-EAGAIN && cu_now()>start);
    caught=true;CHECK(net_socket_io(&client,fd,&b,1,0,NULL,false,NULL)==-EINTR);caught=false;
    timeout.microseconds=1000000;length=sizeof(timeout);CHECK(net_socket_option(&client,fd,1,SOL_SOCKET,SO_RCVTIMEO,&timeout,&length,NULL)==-EINVAL);
    int val=1;length=4;CHECK(net_socket_option(&client,fd,1,IPPROTO_TCP,TCP_NODELAY,&val,&length,NULL)==-ENOPROTOOPT);
    CHECK(!file_close(&client,fd));drain();
    int tcp=net_socket_create(&client,AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0);CHECK(tcp>=0);
    struct ciuki_sockaddr_in none=address(39999);CHECK(net_socket_connect(&client,tcp,&none,NULL)==-EINPROGRESS);
    struct ciuki_pollfd p={tcp,POLLOUT,0};CHECK(net_poll(&client,&p,1,1000,NULL)==1 && (p.revents&POLLERR));
    length=4;val=0;CHECK(!net_socket_option(&client,tcp,0,SOL_SOCKET,SO_ERROR,&val,&length,NULL) && val==ECONNREFUSED);
    length=4;val=-1;CHECK(!net_socket_option(&client,tcp,0,SOL_SOCKET,SO_ERROR,&val,&length,NULL) && !val);
    CHECK(!file_close(&client,tcp));drain();
    puts("PASS native close/reuse generations, cancelled syscall pins, interrupted/timed waits, refused connect/SO_ERROR");
}
static void dns_servers_reset(void *unused)
{
    (void)unused;ip_addr_t ip;IP_ADDR4(&ip,192,0,2,2);dns_setserver(0,&ip);
    IP_ADDR4(&ip,192,0,2,1);dns_setserver(0,&ip);
    dns_setserver(1,IP_ADDR_ANY);
}
static void dns_two_servers(void *unused)
{ (void)unused;ip_addr_t ip;IP_ADDR4(&ip,192,0,2,2);dns_setserver(1,&ip); }
static void core_barrier(void *unused) { (void)unused; }
static void coarse(void *n) { for(unsigned i=0;i<(unsigned)(uintptr_t)n;i++)dhcp_coarse_tmr(); }
static void check_address(uint32_t expected)
{
    uint64_t end=cu_now()+12000;uint32_t actual;
    do { net_stack_address(0,&actual);if(actual==expected)return;cu_sleep(5); }while(cu_now()<end);
    CHECK(actual==expected);
}
static void wire_tests(void)
{
    int fd=net_socket_create(&client,AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0);CHECK(fd>=0);
    struct ciuki_sockaddr_in peer={.family=AF_INET,.port=PP_HTONS(33334),.address=PP_HTONL(0xc0000201)};
    CHECK(net_socket_connect(&client,fd,&peer,NULL)==-EINPROGRESS);
    struct ciuki_pollfd p={fd,POLLOUT,0};CHECK(net_poll(&client,&p,1,10000,NULL)==1 && wire_syns>=2);
    unsigned char payload[32768];memset(payload,'x',sizeof(payload));
    int sent=net_socket_io(&client,fd,payload,sizeof(payload),0,NULL,true,NULL);CHECK(sent>0 && sent<(int)sizeof(payload));
    p.events=POLLIN;CHECK(net_poll(&client,&p,1,12000,NULL)==1 && wire_data_transmits>=2 && wire_first_count>=2);
    char b[8];CHECK(net_socket_io(&client,fd,b,sizeof(b),0,NULL,false,NULL)==4 && !memcmp(b,"wire",4));
    CHECK(net_socket_io(&client,fd,b,sizeof(b),0,NULL,false,NULL)==0 && wire_packets>=4);
    CHECK(!file_close(&client,fd));drain();
    fd=net_socket_create(&client,AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0);CHECK(fd>=0);peer.port=PP_HTONS(33336);
    CHECK(net_socket_io(&client,fd,"query",5,0,&peer,true,NULL)==5);p=(struct ciuki_pollfd){fd,POLLIN,0};CHECK(net_poll(&client,&p,1,1000,NULL)==1);
    CHECK(net_socket_io(&client,fd,b,sizeof(b),0,NULL,false,NULL)==3 && !memcmp(b,"two",3));
    CHECK(net_socket_io(&client,fd,b,sizeof(b),0,NULL,false,NULL)==3 && !memcmp(b,"one",3));
    CHECK(net_socket_io(&client,fd,b,sizeof(b),0,NULL,false,NULL)==-EAGAIN);CHECK(!file_close(&client,fd));drain();
    puts("PASS synthetic Ethernet TCP SYN/data retransmit/partial send/FIN and UDP loss/reorder; copied frame path");
}
static void frame_resources(void *unused)
{
    (void)unused;struct netif *eth=netif_list;while(eth && eth->name[0]!='e')eth=eth->next;CHECK(eth);
    struct pbuf *head=pbuf_alloc(PBUF_RAW,5,PBUF_RAM),*tail=pbuf_alloc(PBUF_RAW,55,PBUF_RAM);CHECK(head && tail);
    memset(head->payload,0,5);memset(tail->payload,0,55);pbuf_cat(head,tail);
    CHECK(eth->linkoutput(eth,head)==ERR_OK && output_bytes==60 && !output_frame[59] && head->ref==1 && tail->ref==1);
    CHECK(pbuf_free(head)==2);
    struct pbuf *pool[128];for(unsigned i=0;i<128;i++){pool[i]=pbuf_alloc(PBUF_RAW,1536,PBUF_POOL);CHECK(pool[i]);}
    CHECK(!pbuf_alloc(PBUF_RAW,1,PBUF_POOL));unsigned drops=lwip_stats.link.drop;
    unsigned char frame[60]={0};net_stack_input(0,frame,sizeof(frame));CHECK(lwip_stats.link.drop==drops+1);
    for(unsigned i=0;i<128;i++)CHECK(pbuf_free(pool[i])==1);
    for(unsigned kind=0;kind<MEMP_MAX;kind++) {
        void *entries[256];unsigned n=0,used=lwip_stats.memp[kind]->used;
        while(n<256 && (entries[n]=memp_malloc((memp_t)kind)))n++;
        CHECK(!memp_malloc((memp_t)kind));
        if(kind==MEMP_TCP_PCB) {
            unsigned occupied=lwip_stats.memp[kind]->used;
            CHECK(!tcp_new() && lwip_stats.memp[kind]->used==occupied);
        }
        for(unsigned i=0;i<n;i++)memp_free((memp_t)kind,entries[i]);
        CHECK(lwip_stats.memp[kind]->used==used);
    }
    unsigned before=lwip_stats.mem.used;void *heap[64];unsigned n=0;
    while(n<64 && (heap[n]=mem_malloc(16384)))n++;CHECK(n && n<64);
    for(unsigned i=0;i<n;i++)mem_free(heap[i]);CHECK(lwip_stats.mem.used==before);
}
static void amendment_dhcp_start(void);
static void amendment_dns_renewal(void);
static void amendment_address_revocation(void);
static void dns_dhcp_tests(void)
{
    amendment_dhcp_start();uint32_t ip=0;uint64_t end=cu_now()+12000;
    do { net_stack_address(0,&ip);if (ip) break;cu_sleep(10); } while (cu_now()<end);
    CHECK(ip==PP_HTONL(0xc0000264) && dhcp_discover && dhcp_request);
    wire_tests();CHECK(tcpip_callback_wait(frame_resources,NULL)==ERR_OK);
    int status=-1;uint32_t answer=0;CHECK(!net_resolve(&client,"fixture.example",&answer,&status));
    CHECK(!status && answer==PP_HTONL(0xcb007109) && dns_queries && arp_requests);
    CHECK(net_resolve(&client,"bad..name",&answer,&status)==-EINVAL);
    unsigned before=dns_queries;CHECK(!net_resolve(&client,"fixture.example",&answer,&status) && !status && dns_queries==before);
    /* f6-18 test-profile entries: consulted before any DNS query, two slots,
     * grammar-checked names, cleared by address 0; nothing armed by default. */
    CHECK(net_resolve_override("bad..name",PP_HTONL(0x0a000264))==-EINVAL);
    CHECK(!net_resolve_override("fixture.test",PP_HTONL(0x0a000264)));
    CHECK(!net_resolve_override("untrusted.test",PP_HTONL(0x0a000264)));
    CHECK(net_resolve_override("third.test",PP_HTONL(0x0a000264))==-ENOSPC);
    before=dns_queries;answer=0;status=-1;
    CHECK(!net_resolve(&client,"fixture.test",&answer,&status) && !status && answer==PP_HTONL(0x0a000264) && dns_queries==before);
    CHECK(!net_resolve(&client,"untrusted.test",&answer,&status) && !status && answer==PP_HTONL(0x0a000264) && dns_queries==before);
    CHECK(!net_resolve_override("fixture.test",0));CHECK(!net_resolve_override("untrusted.test",0));
    CHECK(!net_resolve_override("third.test",PP_HTONL(0x0a000264)));CHECK(!net_resolve_override("third.test",0));
    amendment_dns_renewal();before=dns_queries;
    CHECK(tcpip_callback_wait(dns_servers_reset,NULL)==ERR_OK);
    CHECK(!net_resolve(&client,"fixture.example",&answer,&status) && !status && dns_queries>before);
    dns_drop=true;caught=true;
    const char *names[]={"cancel-a.example","cancel-b.example","cancel-c.example","cancel-d.example"};
    for(unsigned i=0;i<4;i++)CHECK(net_resolve(&client,names[i],&answer,&status)==-EINTR);
    caught=false;CHECK(tcpip_callback_wait(core_barrier,NULL)==ERR_OK);
    CHECK(!net_resolve(&client,"limit.example",&answer,&status) && status==2);
    CHECK(last_dns_bytes>=42);CHECK(tcpip_callback_wait(dns_servers_reset,NULL)==ERR_OK);dns_drop=false;
    CHECK(!net_resolve(&client,"after-cancel.example",&answer,&status) && !status);
    CHECK(tcpip_callback_wait(dns_two_servers,NULL)==ERR_OK);dns_drop=true;
    uint64_t began=cu_now();CHECK(!net_resolve(&client,"deadline.example",&answer,&status) && status==2 && cu_now()-began>=10000);
    CHECK(tcpip_callback_wait(dns_servers_reset,NULL)==ERR_OK);dns_drop=false;
    dhcp_drop=true;unsigned requests_before=dhcp_request;
    CHECK(tcpip_callback_wait(coarse,(void *)(uintptr_t)1)==ERR_OK);CHECK(tcpip_callback_wait(core_barrier,NULL)==ERR_OK);CHECK(dhcp_request>requests_before);
    requests_before=dhcp_request;CHECK(tcpip_callback_wait(coarse,(void *)(uintptr_t)1)==ERR_OK);CHECK(tcpip_callback_wait(core_barrier,NULL)==ERR_OK);CHECK(dhcp_request>requests_before);
    CHECK(tcpip_callback_wait(coarse,(void *)(uintptr_t)2)==ERR_OK);check_address(0);
    net_stack_link(0,false);CHECK(tcpip_callback_wait(core_barrier,NULL)==ERR_OK);dhcp_drop=false;
    net_stack_link(0,true);check_address(PP_HTONL(0xc0000264));
    amendment_address_revocation();
    int udp=net_socket_create(&client,AF_INET,SOCK_DGRAM,0);CHECK(udp>=0);
    struct ciuki_sockaddr_in external={.family=AF_INET,.address=PP_HTONL(0xc0000201),.port=PP_HTONS(33333)};
    CHECK(!net_socket_connect(&client,udp,&external,NULL));
    struct ciuki_sockaddr_in broadcast=external;broadcast.address=PP_HTONL(0xffffffff);
    CHECK(net_socket_io(&client,udp,"b",1,0,&broadcast,true,NULL)==-EACCES);
    int enabled=1;unsigned len=4;CHECK(!net_socket_option(&client,udp,1,SOL_SOCKET,SO_BROADCAST,&enabled,&len,NULL));
    CHECK(net_socket_io(&client,udp,"b",1,0,&broadcast,true,NULL)==1);
    net_stack_link(0,false);check_address(0);CHECK(net_socket_io(&client,udp,&enabled,1,MSG_DONTWAIT,NULL,false,NULL)==-ENETDOWN);
    CHECK(!file_close(&client,udp));drain();
    net_stack_link(0,true);check_address(PP_HTONL(0xc0000264));
    dhcp_nak=true;CHECK(tcpip_callback_wait(coarse,(void *)(uintptr_t)1)==ERR_OK);check_address(0);
    dhcp_nak=false;net_stack_link(0,false);CHECK(tcpip_callback_wait(core_barrier,NULL)==ERR_OK);
    arp_conflict=true;net_stack_link(0,true);
    uint64_t deadline=cu_now()+12000;while(!dhcp_declines){CHECK(cu_now()<deadline);cu_sleep(5);}
    check_address(0);arp_conflict=false;
    net_stack_link(0,false);CHECK(tcpip_callback_wait(core_barrier,NULL)==ERR_OK);
    net_stack_link(0,true);check_address(PP_HTONL(0xc0000264));
    puts("PASS DHCP exchange/malformed options/ACD/T1/T2/expiry/link recovery, DNS malformed/ID/port/cache/generation/cancel/limits, broadcast permission");
}
static void cycles(void)
{
    unsigned heap_before=lwip_stats.mem.used, connections_before=lwip_stats.memp[MEMP_NETCONN]->used;
    unsigned udp_before=lwip_stats.memp[MEMP_UDP_PCB]->used;
    for (unsigned i=0;i<10000;i++) {
        int fd;do { fd=net_socket_create(&client,AF_INET,SOCK_DGRAM,0);if(fd==-ENOBUFS)cu_sleep(1); }while(fd==-ENOBUFS);
        CHECK(fd>=0);CHECK(!file_close(&client,fd));
    }
    drain();CHECK(lwip_stats.mem.used==heap_before && lwip_stats.memp[MEMP_NETCONN]->used==connections_before &&
        lwip_stats.memp[MEMP_UDP_PCB]->used==udp_before);
    puts("PASS 10000 create/close cycles; heap/netconn/UDP PCB baseline stable; no public objects retained");
}
#include "amendment.inc"
#include "dos_bridge.inc"
int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    client=(struct process){.pid=1,.state=PROC_LIVE,.fds=fds};arch_tests();
    ciuki_lwip_seed(0x12345678);CHECK(!net_stack_init());
    CHECK(entropy_before_transport);
    refusal_tests();limits_tests();mixed_tests();udp_tests();tcp_tests();lifecycle_tests();
    preparation_races();queue_races();connect_error_race();shutdown_close_tests();
    cycles();dns_dhcp_tests();dos_bridge_pool_test();CHECK(!live_pins);
    extern unsigned cu_arch_bytes(void),cu_socket_bytes(void),cu_socket_object_bytes(void),cu_request_bytes(void);
    printf("MEASURE host pbuf=%zu tcp_pcb=%zu udp_pcb=%zu netconn=%zu socket=%u mbox=%u arch=%u socket_metadata=%u request=%u\n",
        sizeof(struct pbuf),sizeof(struct tcp_pcb),sizeof(struct udp_pcb),sizeof(struct netconn),cu_socket_object_bytes(),cu_mbox_bytes(),cu_arch_bytes(),cu_socket_bytes(),cu_request_bytes());
    for (unsigned i=0;i<MEMP_MAX;i++) printf("POOL %u size=%u count=%u bytes=%u used=%u peak=%u refusals=%u\n",i,memp_pools[i]->size,memp_pools[i]->num,(unsigned)memp_pools[i]->size*memp_pools[i]->num,lwip_stats.memp[i]->used,lwip_stats.memp[i]->max,lwip_stats.memp[i]->err);
    puts("PASS f6-10 focused real-stack host harness (ASan+UBSan)");cu_host_shutdown();return 0;
}
