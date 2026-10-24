/* Deterministic regressions for Amendment 1, using the real pinned stack.
 * Included by stack_test.c. SPDX-License-Identifier: GPL-2.0-only */
static bool entropy_before_transport;
void net_stack_driver_entropy(void)
{
    /* This is called synchronously by net_stack_init, before tcpip_init. */
    extern unsigned cu_host_random_calls(void);
    CHECK(!cu_host_random_calls());
    uint64_t mac=0;memcpy(&mac,client_mac,6);net_stack_entropy(mac);
    entropy_before_transport=true;
}
static _Atomic unsigned pause_point;
static _Atomic bool paused, unpause;
static _Atomic bool pause_control, control_paused, control_unpause;
static _Thread_local bool caller_hook;
void cu_socket_test_hook(unsigned point)
{
    if(point==4 && pause_control) {
        control_paused=true;while(!control_unpause)cu_sleep(1);return;
    }
    if(point!=pause_point || (point!=1 && point!=5 && !caller_hook))return;
    bool expected=false;
    if(!atomic_compare_exchange_strong(&paused,&expected,true))return;
    while(!unpause)cu_sleep(1);
}
static void arm(unsigned point) { paused=false;unpause=false;pause_point=point; }
static void await_pause(void)
{
    uint64_t end=cu_now()+2000;
    while(!paused){CHECK(cu_now()<end);cu_sleep(1);}
}
static void resume(void) { pause_point=0;unpause=true; }
static int stale_fd, replacement_fd;
static void replace_fd(void)
{
    sys_prot_t f=sys_arch_protect();CHECK(!file_close(&client,stale_fd));
    struct proc_object *o=fds[replacement_fd].object;o->retain(o);
    fds[stale_fd]=(struct proc_fd){o,0};net_fd_changed(&client,(unsigned)stale_fd);
    sys_arch_unprotect(f);
}
static void preparation_races(void)
{
    for(unsigned same=0;same<2;same++)for(unsigned kind=0;kind<11;kind++) {
        stale_fd=net_socket_create(&client,AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0);CHECK(stale_fd>=0);
        if(same) {
            replacement_fd=file_fd_reserve(&client);CHECK(replacement_fd>=0);
            fds[stale_fd].object->retain(fds[stale_fd].object);
            CHECK(file_fd_publish(&client,replacement_fd,fds[stale_fd].object,0)>=0);
        } else { replacement_fd=net_socket_create(&client,AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0);CHECK(replacement_fd>=0); }
        struct ciuki_sockaddr_in a=address(31000+kind);memcpy(user+8192,&a,16);
        unsigned cap=16;memcpy(user+12288,&cap,4);
        struct ciuki_socket_io io={.size=40,.version=1,.fd=stale_fd,.buffer=8192,.bytes=1,.flags=MSG_DONTWAIT};
        memcpy(user+4096,&io,sizeof(io));
        access_pointer=8192;access_on_pin=true;
        unsigned nr=CIUKI_SYS_RECVFROM,b=4096,c=0,d=0,s=0;
        if(kind==1) { nr=CIUKI_SYS_SENDTO;access_on_pin=false; }
        if(kind==2 || kind==3) { nr=kind==2 ? CIUKI_SYS_BIND : CIUKI_SYS_CONNECT;b=(unsigned)stale_fd;c=8192;d=16;access_on_pin=false; }
        if(kind==4) { nr=CIUKI_SYS_ACCEPT;b=(unsigned)stale_fd;c=8192;d=12288; }
        if(kind==5) { nr=CIUKI_SYS_SOCKNAME;b=(unsigned)stale_fd;d=8192;s=12288; }
        if(kind==6 || kind==7) {
            struct ciuki_socket_option opt={.size=sizeof(opt),.version=1,.fd=stale_fd,.operation=kind==7,
                .level=SOL_SOCKET,.name=kind==7 ? SO_REUSEADDR : SO_TYPE,.value=8192,.length=kind==7 ? 4 : 12288};
            memcpy(user+4096,&opt,sizeof(opt));int value=1;memcpy(user+8192,&value,4);
            nr=CIUKI_SYS_SOCKOPT;access_on_pin=kind==6;
        }
        if(kind==8) {
            struct ciuki_pollfd pollfd={stale_fd,POLLOUT,0};memcpy(user+4096,&pollfd,sizeof(pollfd));
            nr=CIUKI_SYS_POLL;c=1;access_pointer=4096;
        }
        if(kind==10)access_on_pin=false;
        access_hook=replace_fd;
        int e=kind>=9 ? net_socket_rw(&client,stale_fd,8192,1,kind==10) : call(nr,b,c,d,s);
        CHECK(!access_hook && !live_pins);
        if(kind==8) { struct ciuki_pollfd out;memcpy(&out,user+4096,sizeof(out));CHECK(e==1 && out.revents==POLLNVAL); }
        else CHECK(e==-EBADF);
        CHECK(!file_close(&client,stale_fd) && !file_close(&client,replacement_fd));drain();
    }
    puts("PASS Amendment 1.1 early fd retention: copy/pin close/reuse, same-object generations, recv/send/read/write/bind/connect/accept/name/options/poll");
}
struct race_job { int fd, kind, result; _Atomic(struct proc_thread *) caller; };
static void *race_job(void *arg)
{
    struct race_job *j=arg;caller_hook=true;thread.process=&client;j->caller=&thread;
    pthread_cleanup_push(cancelled_user,NULL);
    if(j->kind==1) { struct ciuki_sockaddr_in a=address(39998);j->result=net_socket_connect(&client,j->fd,&a,NULL); }
    else j->result=call(CIUKI_SYS_RECVFROM,4096,0,0,0);
    pthread_cleanup_pop(0);return NULL;
}
static void queue_races(void)
{
    int sender=net_socket_create(&client,AF_INET,SOCK_DGRAM,0);CHECK(sender>=0);
    int receiver=net_socket_create(&client,AF_INET,SOCK_DGRAM,0);CHECK(receiver>=0);
    struct ciuki_sockaddr_in a=address(31020);CHECK(!net_socket_bind(&client,receiver,&a,NULL));
    for(unsigned kind=0;kind<4;kind++) {
        CHECK(net_socket_io(&client,sender,"q",1,0,&a,true,NULL)==1);
        struct ciuki_pollfd pollfd={receiver,POLLIN,0};CHECK(net_poll(&client,&pollfd,1,1000,NULL)==1);
        struct ciuki_socket_timeval timeout={.microseconds=kind==1 ? 20000 : 0};unsigned length=16;
        CHECK(!net_socket_option(&client,receiver,1,SOL_SOCKET,SO_RCVTIMEO,&timeout,&length,NULL));
        struct ciuki_socket_io io={.size=40,.version=1,.fd=receiver,.buffer=8192,.bytes=1};memcpy(user+4096,&io,40);
        arm(1);struct race_job job={.fd=receiver};pthread_t t;CHECK(!pthread_create(&t,NULL,race_job,&job));await_pause();
        if(kind==0) { sys_prot_t f=sys_arch_protect();atomic_load(&job.caller)->pending=1;sys_arch_unprotect(f); }
        if(kind==1)cu_sleep(30);
        if(kind==2)client.state=PROC_STOPPING;
        if(kind==3)CHECK(!pthread_cancel(t));
        if(kind==3) { CHECK(!pthread_join(t,NULL));CHECK(!live_pins);resume(); }
        else { resume();CHECK(!pthread_join(t,NULL));CHECK(job.result==(kind==0 ? -EINTR : kind==1 ? -EAGAIN : -EBADF)); }
        client.state=PROC_LIVE;
        char value=0;CHECK(net_socket_io(&client,receiver,&value,1,MSG_DONTWAIT,NULL,false,NULL)==1 && value=='q');
        CHECK(!live_pins);
    }
    CHECK(!file_close(&client,sender) && !file_close(&client,receiver));drain();
    puts("PASS Amendment 1.2 queued receive signal/deadline/process-stop/termination: no dequeue, pinned outputs drain safely");
}
static void connect_error_race(void)
{
    int fd=net_socket_create(&client,AF_INET,SOCK_STREAM,0);CHECK(fd>=0);
    arm(3);struct race_job job={.fd=fd,.kind=1};pthread_t t;CHECK(!pthread_create(&t,NULL,race_job,&job));await_pause();
    struct ciuki_pollfd p={fd,POLLOUT,0};CHECK(net_poll(&client,&p,1,1000,NULL)==1 && (p.revents&POLLERR));
    int error=0;unsigned length=4;CHECK(!net_socket_option(&client,fd,0,SOL_SOCKET,SO_ERROR,&error,&length,NULL) && error==ECONNREFUSED);
    resume();CHECK(!pthread_join(t,NULL) && job.result==-ECONNREFUSED);
    CHECK(!file_close(&client,fd));drain();
    puts("PASS Amendment 1.3 blocking connect retains refusal after another thread clears SO_ERROR");
}
#include "lwip/priv/tcp_priv.h"
#include "lwip/priv/api_msg.h"
extern struct netconn *cu_socket_test_conn(struct process *,int);
extern bool cu_socket_test_control_pending(struct process *,int);
static void make_pair(int *sender,int *receiver)
{
    static unsigned port=31100;
    unsigned before=net_socket_count();
    int listener=net_socket_create(&client,AF_INET,SOCK_STREAM,0);CHECK(listener>=0);
    struct ciuki_sockaddr_in a=address(port++);CHECK(!net_socket_bind(&client,listener,&a,NULL));
    CHECK(!net_socket_listen(&client,listener,1,NULL));
    *sender=net_socket_create(&client,AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0);CHECK(*sender>=0);
    CHECK(net_socket_connect(&client,*sender,&a,NULL)==-EINPROGRESS);
    struct ciuki_pollfd p={*sender,POLLOUT,0};CHECK(net_poll(&client,&p,1,1000,NULL)==1);
    *net_socket_flags(fds[*sender].object)=O_RDWR;
    *receiver=net_socket_accept(&client,listener,NULL,NULL);CHECK(*receiver>=0);
    CHECK(!file_close(&client,listener));
    uint64_t end=cu_now()+1000;
    while(net_socket_count()!=before+2){CHECK(cu_now()<end);cu_sleep(1);}
    g26b_stream_available(*sender,*receiver);
}
static void *held_segments[MEMP_NUM_TCP_SEG];
static void exhaust_segments(void *unused)
{
    (void)unused;
    for(unsigned i=0;i<MEMP_NUM_TCP_SEG;i++){held_segments[i]=memp_malloc(MEMP_TCP_SEG);CHECK(held_segments[i]);}
    CHECK(!memp_malloc(MEMP_TCP_SEG));
}
static void release_segments(void *unused)
{
    (void)unused;for(unsigned i=0;i<MEMP_NUM_TCP_SEG;i++)memp_free(MEMP_TCP_SEG,held_segments[i]);
    tcp_slowtmr();
}
struct raw_close_probe { struct tcp_pcb *pcb; bool retained; };
static void raw_close_snapshot(void *arg)
{
    struct raw_close_probe *p=arg;p->retained=false;
    for(struct tcp_pcb *c=tcp_active_pcbs;c;c=c->next)if(c==p->pcb)p->retained=(c->flags&TF_CLOSEPEND)!=0;
}
static void raw_close_cleanup(void *arg)
{
    struct raw_close_probe *p=arg;
    for(struct tcp_pcb *c=tcp_active_pcbs;c;c=c->next)if(c==p->pcb){tcp_abort(c);break;}
}
static void shutdown_close_tests(void)
{
    int sender,receiver;make_pair(&sender,&receiver);
    CHECK(net_socket_io(&client,sender,"p",1,0,NULL,true,NULL)==1);
    struct ciuki_pollfd ready={receiver,POLLIN,0};CHECK(net_poll(&client,&ready,1,1000,NULL)==1);
    struct ciuki_socket_io io={.size=40,.version=1,.fd=receiver,.buffer=8192,.bytes=1};memcpy(user+4096,&io,40);
    arm(5);struct race_job job={.fd=receiver};pthread_t waiter;CHECK(!pthread_create(&waiter,NULL,race_job,&job));await_pause();
    sys_prot_t f=sys_arch_protect();atomic_load(&job.caller)->pending=1;sys_arch_unprotect(f);
    resume();CHECK(!pthread_join(waiter,NULL) && job.result==-EINTR && !live_pins);
    char value;CHECK(net_socket_io(&client,receiver,&value,1,MSG_DONTWAIT,NULL,false,NULL)==1 && value=='p');
    puts("PASS Amendment 1.2 signal after internal TCP getaddr preparation prevents the later receive/dequeue");
    CHECK(!net_socket_shutdown(&client,sender,SHUT_RDWR,NULL));
    uint64_t end=cu_now()+2000;
    while(cu_socket_test_control_pending(&client,sender)){CHECK(cu_now()<end);cu_sleep(1);}
    int error=-1;unsigned length=4;CHECK(!net_socket_option(&client,sender,0,SOL_SOCKET,SO_ERROR,&error,&length,NULL) && !error);
    struct ciuki_pollfd p={sender,POLLIN|POLLOUT,0};CHECK(net_poll(&client,&p,1,0,NULL)==1 && !(p.revents&POLLERR) && (p.revents&POLLHUP));
    char b;CHECK(net_socket_io(&client,sender,&b,1,0,NULL,false,NULL)==0);
    CHECK(!file_close(&client,sender) && !file_close(&client,receiver));drain();
    puts("PASS Amendment 1.5 successful SHUT_RDWR: EOF/HUP, SO_ERROR=0, no POLLERR");
    for(unsigned kind=0;kind<2;kind++) {
        make_pair(&sender,&receiver);
        struct ciuki_socket_timeval timeout={.microseconds=kind ? 20000 : 0};length=16;
        CHECK(!net_socket_option(&client,sender,1,SOL_SOCKET,SO_RCVTIMEO,&timeout,&length,NULL));
        pause_control=true;control_paused=false;control_unpause=false;
        CHECK(!net_socket_shutdown(&client,sender,SHUT_WR,NULL));
        end=cu_now()+1000;while(!control_paused){CHECK(cu_now()<end);cu_sleep(1);}
        struct ciuki_socket_io io={.size=40,.version=1,.fd=sender,.buffer=8192,.bytes=1};memcpy(user+4096,&io,40);
        struct race_job job={.fd=sender};pthread_t t;CHECK(!pthread_create(&t,NULL,race_job,&job));
        end=cu_now()+1000;while(!live_pins){CHECK(cu_now()<end);cu_sleep(1);}
        cu_sleep(2);
        if(!kind) { sys_prot_t f=sys_arch_protect();atomic_load(&job.caller)->pending=1;sys_arch_unprotect(f); }
        else cu_sleep(30);
        CHECK(!pthread_join(t,NULL) && job.result==(kind ? -EAGAIN : -EINTR) && !live_pins);
        CHECK(cu_socket_test_control_pending(&client,sender));
        pause_control=false;control_unpause=true;
        CHECK(!file_close(&client,sender) && !file_close(&client,receiver));drain();
    }
    puts("PASS Amendment 1.2 control-ownership waits honor signals/deadlines while internal close completion remains alive");
    make_pair(&sender,&receiver);
    struct raw_close_probe probe={.pcb=cu_socket_test_conn(&client,sender)->pcb.tcp};
    CHECK(tcpip_callback_wait(exhaust_segments,NULL)==ERR_OK);
    unsigned baseline=net_socket_count();uint64_t start=cu_now();CHECK(!file_close(&client,sender));
    end=cu_now()+200;while(net_socket_count()==baseline){CHECK(cu_now()<end);cu_sleep(1);}
    CHECK(net_socket_count()==baseline-1 && cu_now()-start<1000 && lwip_stats.memp[MEMP_TCP_SEG]->used==MEMP_NUM_TCP_SEG);
    CHECK(((LWIP_TCP_CLOSE_TIMEOUT_MS_DEFAULT+TCP_SLOW_INTERVAL-1)/TCP_SLOW_INTERVAL)+1==2);
    CHECK(tcpip_callback_wait(raw_close_snapshot,&probe)==ERR_OK && probe.retained);
    /* This is a distinct upstream path: raw FIN allocation returns ERR_OK
     * with TF_CLOSEPEND, so the sequential timeout polls are never entered. */
    cu_sleep(1050);CHECK(tcpip_callback_wait(raw_close_snapshot,&probe)==ERR_OK && probe.retained);
    CHECK(tcpip_callback_wait(raw_close_cleanup,&probe)==ERR_OK);
    CHECK(tcpip_callback_wait(release_segments,NULL)==ERR_OK);
    CHECK(!file_close(&client,receiver));drain();
    puts("PASS Amendment 1.8 persistent FIN exhaustion: netconn reclaimed <1s; two-poll configuration; raw TF_CLOSEPEND survives >1s (lead decision required)");
}
static void *held_heap[600],*held_udp[MEMP_NUM_UDP_PCB];
static unsigned heap_count,udp_held;
static void exhaust_dhcp(void *arg)
{
    if(arg) {
        udp_held=0;
        while(udp_held<MEMP_NUM_UDP_PCB) {
            void *p=memp_malloc(MEMP_UDP_PCB);if(!p)break;held_udp[udp_held++]=p;
        }
        CHECK(udp_held && !memp_malloc(MEMP_UDP_PCB));
    } else {
        heap_count=0;unsigned sizes[]={4096,32,1};
        for(unsigned i=0;i<3;i++)for(;;) {
            void *p=mem_malloc(sizes[i]);if(!p)break;CHECK(heap_count<600);held_heap[heap_count++]=p;
        }
        CHECK(!mem_malloc(sizeof(struct dhcp)));
    }
}
static void release_dhcp(void *arg)
{
    if(arg) { while(udp_held)memp_free(MEMP_UDP_PCB,held_udp[--udp_held]); }
    else { while(heap_count)mem_free(held_heap[--heap_count]); }
}
static void retry_acquisition(void)
{
    uint64_t end=cu_now()+15000;uint32_t ip=0;
    do { net_stack_link(0,true);net_stack_address(0,&ip);if(ip)break;CHECK(cu_now()<end);cu_sleep(5); }while(true);
    CHECK(ip==PP_HTONL(0xc0000264) && !net_stack_dhcp_error(0));
}
static void amendment_dhcp_start(void)
{
    for(unsigned which=0;which<2;which++) {
        if(which){net_stack_link(0,false);CHECK(tcpip_callback_wait(core_barrier,NULL)==ERR_OK);}
        CHECK(tcpip_callback_wait(exhaust_dhcp,(void *)(uintptr_t)which)==ERR_OK);
        unsigned before=dhcp_discover;
        if(!which)CHECK(!net_stack_attach(0,client_mac,true));
        else { net_stack_link(0,true);CHECK(tcpip_callback_wait(core_barrier,NULL)==ERR_OK); }
        CHECK(net_stack_dhcp_error(0)==-ENOMEM && dhcp_discover==before);
        CHECK(tcpip_callback_wait(release_dhcp,(void *)(uintptr_t)which)==ERR_OK);
        for(unsigned n=0;n<10;n++){net_stack_link(0,true);CHECK(tcpip_callback_wait(core_barrier,NULL)==ERR_OK);}
        CHECK(net_stack_dhcp_error(0)==-ENOMEM && dhcp_discover==before);
        retry_acquisition();CHECK(dhcp_discover>before);
    }
    puts("PASS Amendment 1.6 real DHCP object/PCB allocation failures are visible, paced and recover without link transitions");
}
static unsigned dns_count;
static void dns_snapshot(void *unused)
{
    (void)unused;dns_count=0;
    for(unsigned i=0;i<DNS_MAX_SERVERS;i++)if(!ip_addr_isany(dns_getserver((u8_t)i)))dns_count++;
}
static void renew(void *unused) { (void)unused;CHECK(dhcp_renew(netif_default)==ERR_OK); }
static void renew_with_dns(unsigned count)
{
    lease_dns_count=count;CHECK(tcpip_callback_wait(renew,NULL)==ERR_OK);
    uint64_t end=cu_now()+1000;
    do { CHECK(tcpip_callback_wait(dns_snapshot,NULL)==ERR_OK);if(dns_count==count)break;CHECK(cu_now()<end);cu_sleep(1); }while(true);
    uint32_t ip;net_stack_address(0,&ip);CHECK(ip==PP_HTONL(0xc0000264));
}
static void amendment_dns_renewal(void)
{
    CHECK(tcpip_callback_wait(dns_snapshot,NULL)==ERR_OK && dns_count==2);
    renew_with_dns(1);
    int status;uint32_t answer;unsigned before=dns_queries;
    CHECK(!net_resolve(&client,"fixture.example",&answer,&status) && !status && dns_queries>before);
    renew_with_dns(0);before=dns_queries;
    CHECK(!net_resolve(&client,"fixture.example",&answer,&status) && status && !answer && dns_queries==before);
    renew_with_dns(1);before=dns_queries;
    CHECK(!net_resolve(&client,"fixture.example",&answer,&status) && !status && dns_queries>before);
    puts("PASS Amendment 1.7 same-address DHCP ACK replaces [A,B] with [A] and [], invalidating cached DNS answers");
}
static void amendment_address_revocation(void)
{
    for(unsigned nak=0;nak<2;nak++) {
        int listener=net_socket_create(&client,AF_INET,SOCK_STREAM,0);CHECK(listener>=0);
        struct ciuki_sockaddr_in a={.family=AF_INET,.address=PP_HTONL(0xc0000264),.port=lwip_htons((u16_t)(31200+nak))};
        CHECK(!net_socket_bind(&client,listener,&a,NULL) && !net_socket_listen(&client,listener,1,NULL));
        int sender=net_socket_create(&client,AF_INET,SOCK_STREAM|SOCK_NONBLOCK,0);CHECK(sender>=0);
        CHECK(net_socket_connect(&client,sender,&a,NULL)==-EINPROGRESS);
        struct ciuki_pollfd p={sender,POLLOUT,0};CHECK(net_poll(&client,&p,1,1000,NULL)==1 && !(p.revents&POLLERR));
        int receiver=net_socket_accept(&client,listener,NULL,NULL);CHECK(receiver>=0);
        struct wait_job job={.fd=listener,.kind=3};pthread_t t;CHECK(!pthread_create(&t,NULL,wait_job,&job));
        while(!job.entered)cu_sleep(1);cu_sleep(5);
        dhcp_drop=!nak;dhcp_nak=nak;
        CHECK(tcpip_callback_wait(coarse,(void *)(uintptr_t)(nak ? 1 : 4))==ERR_OK);check_address(0);
        CHECK(!pthread_join(t,NULL) && job.result==-ENETDOWN);
        char byte;
        CHECK(net_socket_io(&client,sender,"s",1,MSG_DONTWAIT|MSG_NOSIGNAL,NULL,true,NULL)==-ENETDOWN);
        CHECK(net_socket_io(&client,receiver,&byte,1,MSG_DONTWAIT,NULL,false,NULL)==-ENETDOWN);
        CHECK(net_socket_io(&client,sender,&byte,1,MSG_DONTWAIT,NULL,false,NULL)==-ENETDOWN);
        CHECK(!file_close(&client,sender) && !file_close(&client,receiver) && !file_close(&client,listener));drain();
        net_stack_link(0,false);CHECK(tcpip_callback_wait(core_barrier,NULL)==ERR_OK);
        dhcp_drop=false;dhcp_nak=false;net_stack_link(0,true);check_address(PP_HTONL(0xc0000264));
    }
    puts("PASS Amendment 1.4 DHCP expiry/NAK with aborted TCP PCBs and blocked bound accept: send/recv/accept return ENETDOWN");
}
