/* G26 Winsock IPv4 facade over the native socket ABI.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/ntdll.h>
#include <w32/user32.h>
#include <ciuki/abi.h>
#include "../../libciuki/hosts.h"
#include "../../libciuki/name_wire.h"
#define WP(p) ((uint32_t)(uintptr_t)(p))
#define WS_INVALID (~(uintptr_t)0)
#define WS_SLOTS CIUKI_SOCKET_PROCESS_MAX
struct ws_slot {
    uintptr_t handle;
    int fd,type,family;
    HWND window;
    HANDLE event;
    UINT message;
    unsigned mask,armed,recorded;
    int errors[6];
    int connecting,listening;
};
static void async_cleanup(void);
static void async_rearm(uintptr_t,unsigned);
static void async_connect(uintptr_t,int);
static void async_inherit(uintptr_t,uintptr_t);
static struct ws_slot sockets[WS_SLOTS];
static volatile uint32_t guard;
static uint32_t starts, next_handle = 0x10000;
struct ws_data { uint16_t version, high; char description[257], status[129]; uint16_t max_sockets, max_udp; uint32_t vendor; };
struct ws_set { uint32_t count; uintptr_t socket[64]; };
struct ws_timeval { int32_t seconds, microseconds; };
struct ws_hostent { char *name, **aliases; int16_t family, length; char **addresses; };
struct ws_servent { char *name, **aliases; int16_t port; char *protocol; };
struct ws_protoent { char *name, **aliases; int16_t protocol; };
struct ws_thread { uintptr_t owner; struct ws_hostent host; char name[254], text[16]; uint32_t address; char *aliases[1], *addresses[2]; struct ws_servent service; struct ws_protoent protocol; };
static struct ws_thread *threads; /* lazily allocated process-lifetime resolver cache */
struct ws_buffer { uint32_t length; char *buffer; };
struct ws_addrinfo { int flags, family, socktype, protocol; uint32_t length; char *canonname; void *address; struct ws_addrinfo *next; };
struct ws_answer { struct ws_addrinfo info; struct ciuki_sockaddr_in address; };
static int native(uint32_t op, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
#ifdef W32_HOST_MODEL
    extern int32_t ws_native_model(uint32_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
    return ws_native_model(op,a,b,c,d);
#else
    int32_t result;
    __asm__ volatile("pushl %%ebp; xorl %%ebp,%%ebp; int $0x80; popl %%ebp"
        : "=a"(result) : "a"(op),"b"(a),"c"(b),"d"(c),"S"(d),"D"(0) : "memory","cc");
    return result;
#endif
}
static void lock(void) { while (__atomic_exchange_n(&guard,1,__ATOMIC_ACQUIRE)) {} }
static void unlock(void) { __atomic_store_n(&guard,0,__ATOMIC_RELEASE); }
static int fail(int error) { NtCurrentTeb()->CoreSocketError=(ULONG)error; return -1; }
static int win_error(int e) {
    switch(e) {
    case EINTR:return 10004; case EACCES:case EPERM:return 10013; case EFAULT:return 10014;
    case EINVAL:return 10022; case EMFILE:case ENFILE:return 10024; case EAGAIN:return 10035;
    case EINPROGRESS:return 10035; case EALREADY:return 10037; case EBADF:case ENOTSOCK:return 10038;
    case EDESTADDRREQ:return 10039; case EMSGSIZE:return 10040; case EPROTOTYPE:return 10041;
    case ENOPROTOOPT:return 10042; case EPROTONOSUPPORT:return 10043; case ESOCKTNOSUPPORT:return 10044;
    case EOPNOTSUPP:case ENOSYS:return 10045; case EAFNOSUPPORT:return 10047; case EADDRINUSE:return 10048;
    case EADDRNOTAVAIL:return 10049; case ENETDOWN:case ENODEV:return 10050; case ENETUNREACH:return 10051;
    case ECONNABORTED:return 10053; case ECONNRESET:case EPIPE:return 10054; case ENOBUFS:case ENOMEM:return 10055;
    case EISCONN:return 10056; case ENOTCONN:return 10057; case ETIMEDOUT:return 10060;
    case ECONNREFUSED:return 10061; case EHOSTUNREACH:return 10065; case ECANCELED:return 10004;
    default:return 10022;
    }
}
static int result(int r) { return r<0 ? fail(win_error(-r)) : r; }
static int initialized(void) { lock(); int yes=starts!=0; unlock(); return yes ? 1 : (fail(10093),0); }
static struct ws_thread *thread_data(void) {
    uintptr_t owner=(uintptr_t)NtCurrentTeb(); struct ws_thread *found=0; lock();
    if(!threads) {
        const struct w32_services *s=w32_get_services();
        threads=s && s->alloc?s->alloc(s->opaque,64*sizeof(*threads)):0;
        if(threads)w32_fill(threads,0,64*sizeof(*threads));
    }
    if(!threads) { unlock();fail(10055);return 0; }
    for(unsigned i=0;i<64;i++) if(threads[i].owner==owner) { found=&threads[i]; break; }
    if(!found) for(unsigned i=0;i<64;i++) if(!threads[i].owner) { threads[i].owner=owner; found=&threads[i]; break; }
    unlock(); if(!found) fail(10055); return found;
}
static int acquire(uintptr_t socket_,int *type) {
    int fd=-1, error=10038; lock();
    if(!starts) error=10093;
    else for(unsigned i=0;i<WS_SLOTS;i++) if(sockets[i].handle && sockets[i].handle==socket_) {
        fd=native(CIUKI_SYS_FCNTL,(uintptr_t)sockets[i].fd,F_DUPFD_CLOEXEC,0,0);
        if(fd<0) { error=win_error(-fd); fd=-1; }
        else if(type) *type=sockets[i].type;
        break;
    }
    unlock(); if(fd<0) fail(error); return fd;
}
static void release(int fd) { (void)native(CIUKI_SYS_CLOSE,(uintptr_t)fd,0,0,0); }
static uintptr_t adopt(int fd,int type,int family) {
    uintptr_t handle=WS_INVALID; int error=10024; lock();
    if(!starts) error=10093;
    else if(next_handle<0x7fffffffU) for(unsigned i=0;i<WS_SLOTS;i++) if(!sockets[i].handle) {
        handle=next_handle++; sockets[i]=(struct ws_slot){.handle=handle,.fd=fd,.type=type,.family=family}; break;
    }
    unlock(); if(handle==WS_INVALID) { release(fd); fail(error); } return handle;
}
int W32API WSAGetLastError(void) { return (int)NtCurrentTeb()->CoreSocketError; }
void W32API WSASetLastError(int e) { NtCurrentTeb()->CoreSocketError=(ULONG)e; }
int W32API WSAStartup(uint16_t version,void *out) {
    unsigned major=version&255,minor=version>>8; if(!out) return 10014; if(!major) return 10092;
    uint16_t negotiated=major>2 ? 0x0202 : major==2 ? (uint16_t)(2|((minor>2?2:minor)<<8)) : (uint16_t)(1|((minor?1:0)<<8));
    struct ws_data data={.version=negotiated,.high=0x0202,.max_sockets=WS_SLOTS,.max_udp=CIUKI_UDP_MAX};
    static const char description[]="CiukiOS IPv4 and IPX sockets",status[]="Running";
    w32_copy(data.description,description,sizeof(description)); w32_copy(data.status,status,sizeof(status));
    lock(); if(starts==UINT32_MAX) { unlock(); return 10067; } starts++; unlock(); w32_copy(out,&data,sizeof(data)); return 0;
}
int W32API WSACleanup(void) {
    int fds[WS_SLOTS]; unsigned count=0; lock();
    if(!starts) { unlock(); return fail(10093); }
    if(!--starts) {
        async_cleanup();
        for(unsigned i=0;i<WS_SLOTS;i++) if(sockets[i].handle) { fds[count++]=sockets[i].fd; sockets[i].handle=0; }
        if(threads)for(unsigned i=0;i<64;i++) threads[i].owner=0;
    }
    unlock(); for(unsigned i=0;i<count;i++) release(fds[i]); return 0;
}
uintptr_t W32API socket(int family,int type,int protocol) {
    if(!initialized()) return WS_INVALID;
    if(family!=AF_INET && family!=AF_IPX) return (uintptr_t)fail(10047);
    if(type!=SOCK_STREAM && type!=SOCK_DGRAM && type!=3 && !(family==AF_IPX && type==SOCK_SEQPACKET)) return (uintptr_t)fail(10044);
    int fd=native(CIUKI_SYS_SOCKET,(uintptr_t)family,(uintptr_t)type,(uintptr_t)protocol,0);
    return fd<0 ? (uintptr_t)result(fd) : adopt(fd,type,family);
}
int W32API closesocket(uintptr_t socket_) {
    int fd=-1,error=10038; lock(); if(!starts) error=10093;
    else for(unsigned i=0;i<WS_SLOTS;i++) if(sockets[i].handle && sockets[i].handle==socket_) { fd=sockets[i].fd; sockets[i].handle=0; break; }
    unlock(); return fd<0 ? fail(error) : result(native(CIUKI_SYS_CLOSE,(uintptr_t)fd,0,0,0));
}
static int family_of(uintptr_t s) {
    int family=AF_INET; lock();
    for(unsigned i=0;i<WS_SLOTS;i++)if(sockets[i].handle==s) { family=sockets[i].family;break; }
    unlock();return family;
}
static int address_in(void *out,const void *address,int length,int family) {
    int need=family==AF_IPX?14:16;
    if(!address)return -EFAULT;
    if(length<need || (family!=AF_IPX && length!=16))return -EINVAL;
    w32_fill(out,0,16);w32_copy(out,address,(uint32_t)need);return 0;
}
static int addressed(uint32_t op,uintptr_t socket_,const void *address,int length) {
    int fd=acquire(socket_,0); if(fd<0) return -1;
    struct ciuki_sockaddr_in carrier;
    int r=address_in(&carrier,address,length,family_of(socket_));
    if(!r)r=native(op,(uintptr_t)fd,(uintptr_t)&carrier,16,0);
    release(fd); return result(r);
}
int W32API bind(uintptr_t s,const void *a,int n) { return addressed(CIUKI_SYS_BIND,s,a,n); }
int W32API connect(uintptr_t s,const void *a,int n) { int r=addressed(CIUKI_SYS_CONNECT,s,a,n);async_connect(s,r);return r; }
int W32API listen(uintptr_t s,int backlog) { int fd=acquire(s,0); if(fd<0)return -1; int r=native(CIUKI_SYS_LISTEN,(uintptr_t)fd,(uintptr_t)backlog,0,0); release(fd);if(r>=0) { lock();for(unsigned i=0;i<WS_SLOTS;i++)if(sockets[i].handle==s)sockets[i].listening=1;unlock(); }return result(r); }
uintptr_t W32API accept(uintptr_t s,void *a,int *n) {
    int type=0,fd=acquire(s,&type); if(fd<0)return WS_INVALID;
    int family=family_of(s),need=family==AF_IPX?14:16,cap=16;
    struct ciuki_sockaddr_in carrier;
    int r=(a && !n) || (!a && n) ? -EFAULT : n && *n<need ? -EFAULT :
        native(CIUKI_SYS_ACCEPT,(uintptr_t)fd,a?(uintptr_t)&carrier:0,n?(uintptr_t)&cap:0,0);
    if(r>=0 && a) { w32_copy(a,&carrier,(uint32_t)need);*n=need; }
    release(fd);async_rearm(s,8);if(r<0)return (uintptr_t)result(r);
    uintptr_t child=adopt(r,type,family);if(child!=WS_INVALID)async_inherit(s,child);return child;
}
int W32API shutdown(uintptr_t s,int how) { int fd=acquire(s,0); if(fd<0)return -1; int r=native(CIUKI_SYS_SHUTDOWN,(uintptr_t)fd,(uintptr_t)how,0,0); release(fd); return result(r); }
static int name_call(uintptr_t s,int peer,void *a,int *n) {
    int fd=acquire(s,0); if(fd<0)return -1;
    int need=family_of(s)==AF_IPX?14:16,cap=16;struct ciuki_sockaddr_in carrier;
    int r=!a||!n||*n<need ? -EFAULT : native(CIUKI_SYS_SOCKNAME,(uintptr_t)fd,(uintptr_t)peer,(uintptr_t)&carrier,(uintptr_t)&cap);
    if(r>=0) { w32_copy(a,&carrier,(uint32_t)need);*n=need; }
    release(fd); return result(r);
}
int W32API getsockname(uintptr_t s,void *a,int *n) { return name_call(s,0,a,n); }
int W32API getpeername(uintptr_t s,void *a,int *n) { return name_call(s,1,a,n); }
static int transfer_ex(uintptr_t s,void *data,int n,int flags,void *address,intptr_t length,int sending,unsigned extra,unsigned *moved,unsigned *received_flags) {
    int type=0,fd=acquire(s,&type); if(fd<0)return -1; int r;
    int ipx=family_of(s)==AF_IPX,need=ipx?14:16,capacity=16; intptr_t original_length=length;
    void *original_address=address;struct ciuki_sockaddr_in carrier;
    if(n<0) r=-EINVAL;
    else if(flags&~(sending?0:2)) r=-EOPNOTSUPP;
    else if((n && !data)||(!address && length)||(address && !length)) r=-EFAULT;
    else if(address && sending && (length<need || (!ipx && length!=16))) r=-EINVAL;
    else if(address && !sending && *(int *)length<need) r=-EFAULT;
    else {
        if(ipx && address) {
            if(sending) { w32_fill(&carrier,0,16);w32_copy(&carrier,address,14);length=16; }
            else length=(intptr_t)&capacity;
            address=&carrier;
        }
        unsigned bytes=(unsigned)n;
        if(type==SOCK_STREAM && bytes>CIUKI_SOCKET_IO_MAX) bytes=CIUKI_SOCKET_IO_MAX;
        uint32_t output_flags=0;
        struct ciuki_socket_io io={.size=sizeof(io),.version=CIUKI_SOCKET_IO_V2,.fd=fd,.buffer=WP(data),.bytes=bytes,.flags=(uint32_t)(sending?MSG_NOSIGNAL:flags)|extra,.address=WP(address),.address_length=(uint32_t)length,.reserved={sending?0:WP(&output_flags),0}};
        r=native(sending?CIUKI_SYS_SENDTO:CIUKI_SYS_RECVFROM,(uintptr_t)&io,0,0,0);
        if(r>=0 && ipx && original_address && !sending) {
            w32_copy(original_address,&carrier,14);*(int *)original_length=14;
        }
        if(r>=0) { if(moved)*moved=(unsigned)r;if(received_flags)*received_flags=output_flags; }
        if(r>=0 && (output_flags&MSG_TRUNC))r=-EMSGSIZE;
    }
    release(fd);if(!sending)async_rearm(s,1);else if(r==-EAGAIN)async_rearm(s,2);return result(r);
}
static int transfer(uintptr_t s,void *data,int n,int flags,void *address,intptr_t length,int sending)
{ return transfer_ex(s,data,n,flags,address,length,sending,0,0,0); }
int W32API send(uintptr_t s,const char *p,int n,int f) { return transfer(s,(void *)p,n,f,0,0,1); }
int W32API recv(uintptr_t s,char *p,int n,int f) { return transfer(s,p,n,f,0,0,0); }
int W32API sendto(uintptr_t s,const char *p,int n,int f,const void *a,int len) { return transfer(s,(void *)p,n,f,(void *)a,len,1); }
int W32API recvfrom(uintptr_t s,char *p,int n,int f,void *a,int *len) { return transfer(s,p,n,f,a,(intptr_t)len,0); }
int W32API ioctlsocket(uintptr_t s,int command,ULONG *arg) {
    int fd=acquire(s,0); if(fd<0)return -1; int r;
    if(!arg) r=-EFAULT;
    else if((uint32_t)command==0x8004667eU && !*arg) {
        int selected=0;lock();for(unsigned i=0;i<WS_SLOTS;i++)if(sockets[i].handle==s && sockets[i].mask)selected=1;unlock();
        if(selected)r=-EINVAL;
        else { r=native(CIUKI_SYS_FCNTL,(uintptr_t)fd,F_GETFL,0,0);if(r>=0)r=native(CIUKI_SYS_FCNTL,(uintptr_t)fd,F_SETFL,(unsigned)r&~O_NONBLOCK,0); }
    }
    else if((uint32_t)command==0x8004667eU) {
        r=native(CIUKI_SYS_FCNTL,(uintptr_t)fd,F_GETFL,0,0);
        if(r>=0) r=native(CIUKI_SYS_FCNTL,(uintptr_t)fd,F_SETFL,*arg ? (uint32_t)r|O_NONBLOCK : (uint32_t)r&~O_NONBLOCK,0);
    } else if((uint32_t)command==0x4004667fU) {
        uint32_t bytes=4;
        struct ciuki_socket_option o={.size=sizeof(o),.version=1,.fd=fd,.level=SOL_SOCKET,
            .name=SO_RX_BYTES,.value=WP(arg),.length=WP(&bytes)};
        r=native(CIUKI_SYS_SOCKOPT,(uintptr_t)&o,0,0,0);
    } else r=-EOPNOTSUPP;
    release(fd); return result(r);
}
static int option_map(int *level,int name) {
    if(*level==0xffff) { *level=SOL_SOCKET; switch(name) {
        case 4:return SO_REUSEADDR; case 0x20:return SO_BROADCAST; case 0x1007:return SO_ERROR;
        case 0x1008:return SO_TYPE; case 0x1005:return SO_SNDTIMEO; case 0x1006:return SO_RCVTIMEO; default:return -1;
    }}
    if(*level==IPPROTO_TCP && name==1)return TCP_NODELAY;
    if(*level==IPPROTO_IP && name==4)return IP_TTL;
    return -1;
}
static int socket_option(uintptr_t s,int level,int option,void *value,intptr_t length,int setting) {
    int fd=acquire(s,0); if(fd<0)return -1;
    if(family_of(s)==AF_IPX && (level==NSPROTO_IPX || level==NSPROTO_SPX)) {
        int n=setting?(int)length:length?*(int *)length:0;
        unsigned need=option==IPX_ADDRESS?24:(option==IPX_GETNETINFO || option==IPX_GETNETINFO_NORIP)?20:4;
        int r;
        if(!value || (!setting&&!length) || n<(int)need)r=-EFAULT;
        else {
            unsigned cap=(unsigned)n;
            struct ciuki_socket_option q={.size=sizeof(q),.version=1,.fd=fd,.operation=(unsigned)setting,
                .level=level,.name=option,.value=WP(value),.length=setting?(unsigned)n:WP(&cap)};
            r=native(CIUKI_SYS_SOCKOPT,(uintptr_t)&q,0,0,0);if(r>=0&&!setting)*(int *)length=(int)cap;
        }
        release(fd);return result(r);
    }
    if(family_of(s)==AF_IPX && level==0xffff && option==0x80) {
        int n=setting?(int)length:length?*(int *)length:0,r;uint16_t shorts[2]={0};
        if(!value || n<4)r=-EFAULT;
        else {
            if(setting)w32_copy(shorts,value,4);
            struct ciuki_socket_linger linger={shorts[0],shorts[1]};unsigned cap=sizeof(linger);
            struct ciuki_socket_option q={.size=sizeof(q),.version=1,.fd=fd,.operation=(unsigned)setting,
                .level=SOL_SOCKET,.name=SO_LINGER,.value=WP(&linger),.length=setting?cap:WP(&cap)};
            r=native(CIUKI_SYS_SOCKOPT,(uintptr_t)&q,0,0,0);
            if(r>=0&&!setting) { shorts[0]=(uint16_t)linger.on;shorts[1]=(uint16_t)linger.seconds;w32_copy(value,shorts,4);*(int *)length=4; }
        }
        release(fd);return result(r);
    }
    int name=option_map(&level,option),r;
    int n=setting ? (int)length : length ? *(int *)length : 0;
    if(name<0) r=-ENOPROTOOPT;
    else if(!value||(!setting&&!length)||n<4) r=-EFAULT;
    else {
        struct ciuki_socket_timeval time={0}; int v=0; unsigned bytes=4;
        int timeout=level==SOL_SOCKET && (name==SO_SNDTIMEO||name==SO_RCVTIMEO);
        if(setting) {
            w32_copy(&v,value,4);
            if(timeout) { time.seconds=(uint32_t)v/1000; time.microseconds=(int32_t)((uint32_t)v%1000)*1000; bytes=sizeof(time); }
            else if(level==SOL_SOCKET && (name==SO_REUSEADDR||name==SO_BROADCAST)) v=!!v;
        } else if(timeout) bytes=sizeof(time);
        struct ciuki_socket_option request={.size=sizeof(request),.version=1,.fd=fd,.operation=(uint32_t)setting,.level=level,.name=name,.value=timeout?WP(&time):WP(&v),.length=setting?bytes:WP(&bytes)};
        r=native(CIUKI_SYS_SOCKOPT,(uintptr_t)&request,0,0,0);
        if(r>=0&&!setting) {
            if(timeout) v=(int)(time.seconds*1000+time.microseconds/1000);
            else if(level==SOL_SOCKET && name==SO_ERROR && v) v=win_error(v<0?-v:v);
            w32_copy(value,&v,4); *(int *)length=4;
        }
    }
    release(fd); return result(r);
}
int W32API setsockopt(uintptr_t s,int l,int o,const char *v,int n) { return socket_option(s,l,o,(void *)v,n,1); }
int W32API getsockopt(uintptr_t s,int l,int o,char *v,int *n) { return socket_option(s,l,o,v,(intptr_t)n,0); }
int W32API __WSAFDIsSet(uintptr_t s,const struct ws_set *set) { if(!set||set->count>64)return 0; for(unsigned i=0;i<set->count;i++)if(set->socket[i]==s)return 1; return 0; }
int W32API select(int ignored,struct ws_set *read,struct ws_set *write,struct ws_set *except,const struct ws_timeval *timeout) {
    (void)ignored; if(!initialized())return -1;
    struct ws_set *sets[3]={read,write,except}; struct ciuki_pollfd pollfds[WS_SLOTS]; uintptr_t handles[WS_SLOTS]; unsigned count=0; int ms=-1,error=0;
    if(timeout) {
        if(timeout->seconds<0||timeout->seconds>INT32_MAX/1000||timeout->microseconds<0||timeout->microseconds>=1000000)return fail(10022);
        int64_t value=(int64_t)timeout->seconds*1000+(timeout->microseconds+999)/1000;
        if(value>INT32_MAX)return fail(10022); ms=(int)value;
    }
    for(unsigned set=0;set<3&&!error;set++) if(sets[set]) {
        if(sets[set]->count>64) { error=10022; break; }
        for(unsigned i=0;i<sets[set]->count;i++) {
            uintptr_t h=sets[set]->socket[i]; unsigned j=0; for(;j<count;j++)if(handles[j]==h)break;
            if(j==count) {
                if(count==WS_SLOTS) { error=10055; break; }
                int fd=acquire(h,0); if(fd<0) { error=WSAGetLastError(); break; }
                handles[count]=h; pollfds[count++]=(struct ciuki_pollfd){.fd=fd};
            }
            pollfds[j].events|=(int16_t)(set==0?POLLIN:set==1?POLLOUT:POLLERR);
        }
    }
    if(!error&&!count) error=10022;
    int ready=0;
    if(!error) { int r=native(CIUKI_SYS_POLL,(uintptr_t)pollfds,count,(uintptr_t)ms,0); if(r<0)error=win_error(-r); }
    if(!error) for(unsigned i=0;i<count;i++) if(pollfds[i].revents&POLLNVAL) { error=10038; break; }
    if(!error) for(unsigned set=0;set<3;set++) if(sets[set]) {
        unsigned out=0;
        for(unsigned i=0;i<sets[set]->count;i++) {
            uintptr_t h=sets[set]->socket[i]; unsigned j=0; for(;j<count;j++)if(handles[j]==h)break;
            int mask=set==0 ? (POLLIN|POLLHUP|POLLERR) : set==1 ? POLLOUT : POLLERR;
            if(j<count && (pollfds[j].revents&mask) && (set!=1 || !(pollfds[j].revents&POLLERR))) { sets[set]->socket[out++]=h; ready++; }
        }
        sets[set]->count=out;
    }
    for(unsigned i=0;i<count;i++)release(pollfds[i].fd);
    return error ? fail(error) : ready;
}
uint16_t W32API htons(uint16_t v) { return (uint16_t)((v<<8)|(v>>8)); }
uint16_t W32API ntohs(uint16_t v) { return htons(v); }
ULONG W32API htonl(ULONG v) { return ((v&255)<<24)|((v&0xff00)<<8)|((v>>8)&0xff00)|(v>>24); }
ULONG W32API ntohl(ULONG v) { return htonl(v); }
ULONG W32API inet_addr(const char *text) {
    if(!text)return ~0u;
    while(*text==' '||*text=='\t'||*text=='\r'||*text=='\n')text++;
    /* Windows 98/XP accept an empty address as INADDR_ANY. */
    if(!*text)return 0;
    uint32_t parts[4]; unsigned count=0;
    for(;;) {
        unsigned base=10,digits=0; uint32_t value=0;
        if(*text=='0') { base=8; text++; digits=1; if(*text=='x'||*text=='X') { base=16; text++; digits=0; } }
        for(;;) {
            unsigned digit;
            if(*text>='0'&&*text<='9')digit=(unsigned)(*text-'0');
            else if(*text>='a'&&*text<='f')digit=(unsigned)(*text-'a')+10;
            else if(*text>='A'&&*text<='F')digit=(unsigned)(*text-'A')+10;
            else break;
            if(digit>=base||value>(UINT32_MAX-digit)/base)return ~0u;
            value=value*base+digit; digits++; text++;
        }
        if(!digits||count==4)return ~0u; parts[count++]=value;
        if(*text!='.')break;
        text++;
    }
    while(*text==' '||*text=='\t'||*text=='\r'||*text=='\n')text++;
    if(*text)return ~0u;
    uint32_t address=parts[count-1];
    unsigned last_bits=32-(count-1)*8;
    if(last_bits<32 && address>(((uint32_t)1<<last_bits)-1))return ~0u;
    for(unsigned i=0;i+1<count;i++) { if(parts[i]>255)return ~0u; address|=parts[i]<<(24-i*8); }
    return htonl(address);
}
static char *decimal(char *p,unsigned n) { if(n>=100)*p++=(char)('0'+n/100); if(n>=10)*p++=(char)('0'+n/10%10); *p++=(char)('0'+n%10); return p; }
char *W32API inet_ntoa(ULONG address) { struct ws_thread *t=thread_data(); if(!t)return 0; char *p=t->text; for(unsigned i=0;i<4;i++) { p=decimal(p,(address>>(i*8))&255); *p++=i==3?0:'.'; } return t->text; }
struct hosts_reader { int fd; unsigned at,count; char buffer[128]; };
static int hosts_next(void *opaque) {
    struct hosts_reader *r=opaque; if(r->at==r->count) { int n=native(CIUKI_SYS_READ,(uintptr_t)r->fd,(uintptr_t)r->buffer,sizeof(r->buffer),0); if(n<=0)return -1; r->count=(unsigned)n; r->at=0; } return (unsigned char)r->buffer[r->at++];
}
static int resolve_local(const char *name,uint32_t *address) {
    if(!name||!*name)name="localhost";
    unsigned bytes=0; while(bytes<254&&name[bytes])bytes++; if(!bytes||bytes==254)return 0;
    uint32_t numeric=inet_addr(name); if(numeric!=~0u||!w32_strcmp(name,"255.255.255.255")) { *address=numeric; return 1; }
    struct hosts_reader reader={.fd=native(CIUKI_SYS_OPEN,(uintptr_t)"/system/settings/HOSTS",O_RDONLY|O_CLOEXEC,0,0)};
    if(reader.fd>=0) { int found=cu_hosts_lookup(hosts_next,&reader,name,address); release(reader.fd); if(found)return 1; }
    if(!w32_stricmp(name,"localhost")) { *address=0x0100007f; return 1; }
    return 0;
}
static int ws_name_exchange(const unsigned char *query,unsigned bytes,uint32_t server,unsigned port,int broadcast,uint32_t *address,char *hostname,unsigned capacity)
{
    int fd=native(CIUKI_SYS_SOCKET,AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,IPPROTO_UDP,0);if(fd<0)return 0;
    struct ciuki_sockaddr_in peer={.family=AF_INET,.port=(uint16_t)((port<<8)|(port>>8)),.address=server};
    if(broadcast) {
        unsigned one=1;struct ciuki_socket_option o={.size=sizeof(o),.version=1,.fd=fd,.operation=1,.level=SOL_SOCKET,.name=SO_BROADCAST,.value=WP(&one),.length=4};
        if(native(CIUKI_SYS_SOCKOPT,(uintptr_t)&o,0,0,0)<0) { release(fd);return 0; }
    } else if(native(CIUKI_SYS_CONNECT,(uintptr_t)fd,(uintptr_t)&peer,16,0)<0) { release(fd);return 0; }
    for(unsigned attempt=0;attempt<3;attempt++) {
        struct ciuki_socket_io io={.size=sizeof(io),.version=1,.fd=fd,.buffer=WP(query),.bytes=bytes,
            .flags=MSG_NOSIGNAL,.address=broadcast?WP(&peer):0,.address_length=broadcast?16:0};
        if(native(CIUKI_SYS_SENDTO,(uintptr_t)&io,0,0,0)<0)break;
        for(unsigned wait=0;wait<10;wait++) {
            struct ciuki_pollfd p={fd,POLLIN,0};int r=native(CIUKI_SYS_POLL,(uintptr_t)&p,1,50,0);
            if(r<0) { release(fd);return 0; }if(!r)continue;
            unsigned char packet[1536];struct ciuki_sockaddr_in from;unsigned length=16;
            io=(struct ciuki_socket_io){.size=sizeof(io),.version=1,.fd=fd,.buffer=WP(packet),.bytes=sizeof(packet),.flags=MSG_DONTWAIT,.address=WP(&from),.address_length=WP(&length)};
            r=native(CIUKI_SYS_RECVFROM,(uintptr_t)&io,0,0,0);
            if(r<0)continue;
            if(length!=16 || from.family!=AF_INET || from.port!=peer.port || (!broadcast && from.address!=server))continue;
            if(cu_name_answer(packet,(unsigned)r,query,bytes,address,hostname,capacity)) { release(fd);return 1; }
        }
    }
    release(fd);return 0;
}
static uint16_t name_transaction(void)
{ static uint32_t serial;return (uint16_t)(__atomic_add_fetch(&serial,1,__ATOMIC_RELAXED)^(uintptr_t)NtCurrentTeb()); }
static int ws_netbios(const char *name,uint32_t *address)
{
    unsigned char query[50];if(!cu_nbns_query(query,name_transaction(),name,0x20))return 0;
    struct hosts_reader reader={.fd=native(CIUKI_SYS_OPEN,(uintptr_t)"/system/settings/LMHOSTS",O_RDONLY|O_CLOEXEC,0,0)};
    if(reader.fd>=0) { int found=cu_lmhosts_lookup(hosts_next,&reader,name,0x20,address);release(reader.fd);if(found)return 1; }
    struct ciuki_net_control c={.size=sizeof(c),.version=1};
    if(native(CIUKI_SYS_NET_INFO,CIUKI_NET_INFO_CONFIG,(uintptr_t)&c,sizeof(c),0)<0)return 0;
    unsigned count=c.count;for(unsigned i=0;i<count;i++) {
        c=(struct ciuki_net_control){.size=sizeof(c),.version=1,.index=i};
        if(native(CIUKI_SYS_NET_INFO,CIUKI_NET_INFO_CONFIG,(uintptr_t)&c,sizeof(c),0)<0 || !c.address || !c.mask)continue;
        if(ws_name_exchange(query,sizeof(query),c.address|~c.mask,137,1,address,0,0))return 1;
    }
    return 0;
}
static int ws_reverse(uint32_t address,char *name,unsigned cap)
{
    struct hosts_reader reader={.fd=native(CIUKI_SYS_OPEN,(uintptr_t)"/system/settings/HOSTS",O_RDONLY|O_CLOEXEC,0,0)};
    if(reader.fd>=0) { int found=cu_hosts_reverse(hosts_next,&reader,address,name,cap);release(reader.fd);if(found)return 1; }
    if(address==0x0100007f && cap>=10) { w32_copy(name,"localhost",10);return 1; }
    unsigned char query[90];unsigned bytes=(unsigned)cu_ptr_query(query,name_transaction(),address);
    struct ciuki_net_control c={.size=sizeof(c),.version=1};
    if(native(CIUKI_SYS_NET_INFO,CIUKI_NET_INFO_CONFIG,(uintptr_t)&c,sizeof(c),0)<0)return 0;
    unsigned count=c.count;for(unsigned i=0;i<count;i++) {
        c=(struct ciuki_net_control){.size=sizeof(c),.version=1,.index=i};
        if(native(CIUKI_SYS_NET_INFO,CIUKI_NET_INFO_CONFIG,(uintptr_t)&c,sizeof(c),0)<0)continue;
        for(unsigned d=0;d<3;d++)if(c.dns[d] && ws_name_exchange(query,bytes,c.dns[d],53,0,0,name,cap))return 1;
    }
    return 0;
}
static int resolve(const char *name,uint32_t *address) {
    if(!name||!*name)name="localhost";
    unsigned bytes=0;while(bytes<254 && name[bytes])bytes++;if(!bytes || bytes==254)return 11001;
    if(resolve_local(name,address))return 0;
    struct ciuki_resolve4 request={.size=sizeof(request),.version=1,.name=WP(name),.name_bytes=bytes+1};
    int r=native(CIUKI_SYS_RESOLVE4,(uintptr_t)&request,0,0,0); if(r<0)return win_error(-r);
    if(request.status) { if(!cu_name_equal(name,"localhost") && ws_netbios(name,address))return 0;return request.status==1?11001:request.status==2?11002:11003; } *address=request.address; return 0;
}
void *W32API gethostbyname(const char *name) {
    if(!initialized())return 0; struct ws_thread *t=thread_data(); if(!t)return 0; uint32_t address;
    int error=resolve(name,&address); if(error) { fail(error); return 0; }
    if(!name||!*name)name="localhost"; unsigned length=w32_strlen(name); w32_copy(t->name,name,length+1);
    t->address=address; t->aliases[0]=0; t->addresses[0]=(char *)&t->address; t->addresses[1]=0;
    t->host=(struct ws_hostent){t->name,t->aliases,AF_INET,4,t->addresses}; return &t->host;
}
int W32API gethostname(char *out,int length) { if(!initialized())return -1; static const char name[]="CiukiOS"; if(!out||length<(int)sizeof(name))return fail(10014); w32_copy(out,name,sizeof(name)); return 0; }
void *W32API getprotobyname(const char *name)
{
    if(!initialized())return 0;
    if(!name) { fail(10014);return 0; }
    int number=!w32_stricmp(name,"tcp")?IPPROTO_TCP:!w32_stricmp(name,"udp")?IPPROTO_UDP:!w32_stricmp(name,"icmp")?IPPROTO_ICMP:0;
    if(!number) { fail(11004);return 0; }
    struct ws_thread *t=thread_data();if(!t)return 0;
    const char *canonical=number==IPPROTO_TCP?"tcp":number==IPPROTO_UDP?"udp":"icmp";
    w32_copy(t->name,canonical,w32_strlen(canonical)+1);t->aliases[0]=0;
    t->protocol=(struct ws_protoent){t->name,t->aliases,(int16_t)number};return &t->protocol;
}
void *W32API getprotobynumber(int number)
{
    if(number==IPPROTO_TCP)return getprotobyname("tcp");
    if(number==IPPROTO_UDP)return getprotobyname("udp");
    if(number==IPPROTO_ICMP)return getprotobyname("icmp");
    fail(initialized()?11004:10093);return 0;
}
static struct { const char *name; unsigned port; } const service_names[]={{"http",80},{"https",443},{"ftp",21},{"telnet",23},{"domain",53}};
void *W32API getservbyname(const char *name,const char *protocol)
{
    if(!initialized())return 0;if(!name) { fail(10014);return 0; }
    if(protocol && w32_stricmp(protocol,"tcp") && w32_stricmp(protocol,"udp")) { fail(11004);return 0; }
    for(unsigned i=0;i<sizeof(service_names)/sizeof(*service_names);i++)if(!w32_stricmp(name,service_names[i].name)) {
        if(protocol && !w32_stricmp(protocol,"udp") && service_names[i].port!=53)break;
        struct ws_thread *t=thread_data();if(!t)return 0;
        w32_copy(t->name,service_names[i].name,w32_strlen(service_names[i].name)+1);t->aliases[0]=0;
        t->service=(struct ws_servent){t->name,t->aliases,(int16_t)htons((uint16_t)service_names[i].port),
            (char *)(protocol && !w32_stricmp(protocol,"udp")?"udp":"tcp")};return &t->service;
    }
    fail(11004);return 0;
}
void *W32API getservbyport(int port,const char *protocol)
{
    for(unsigned i=0;i<sizeof(service_names)/sizeof(*service_names);i++)if((uint16_t)port==htons((uint16_t)service_names[i].port))return getservbyname(service_names[i].name,protocol);
    fail(initialized()?11004:10093);return 0;
}
void *W32API gethostbyaddr(const char *address,int length,int family)
{
    if(!initialized())return 0;
    if(!address || length!=4 || family!=AF_INET) { fail(family!=AF_INET?10047:10014);return 0; }
    struct ws_thread *t=thread_data();if(!t)return 0;uint32_t ip;w32_copy(&ip,address,4);
    if(!ws_reverse(ip,t->name,sizeof(t->name))) { fail(11004);return 0; }
    t->address=ip;t->aliases[0]=0;t->addresses[0]=(char *)&t->address;t->addresses[1]=0;
    t->host=(struct ws_hostent){t->name,t->aliases,AF_INET,4,t->addresses};return &t->host;
}
uintptr_t W32API WSASocketA(int family,int type,int protocol,void *info,unsigned group,unsigned flags) { if(info||group||(flags&~1u)) { fail(10045); return WS_INVALID; } return socket(family,type,protocol); }
uintptr_t W32API WSASocketW(int family,int type,int protocol,void *info,unsigned group,unsigned flags) { return WSASocketA(family,type,protocol,info,group,flags); }
int W32API WSAConnect(uintptr_t s,const void *a,int n,void *caller,void *callee,void *sqos,void *gqos) { if(caller||callee||sqos||gqos)return fail(10045); return connect(s,a,n); }
uintptr_t W32API WSAJoinLeaf(uintptr_t s,const void *a,int n,void *caller,void *callee,void *sqos,void *gqos,unsigned flags) { (void)s;(void)a;(void)n;(void)caller;(void)callee;(void)sqos;(void)gqos;(void)flags; return fail(10045); }
static int overlapped_io(uintptr_t,struct ws_buffer *,unsigned,ULONG *,ULONG *,unsigned,void *,intptr_t,void *,void *,int);
static int vector_io(uintptr_t s,struct ws_buffer *buffers,unsigned count,ULONG *done,ULONG *receive_flags,unsigned send_flags,void *address,intptr_t length,void *overlapped,void *completion,int sending) {
    if(!initialized())return -1; if(overlapped)return overlapped_io(s,buffers,count,done,receive_flags,send_flags,address,length,overlapped,completion,sending);
    if(completion)return fail(10022);
    if(!buffers||!count||count>16||!done||(!sending&&!receive_flags))return fail(10014);
    unsigned bytes=0; for(unsigned i=0;i<count;i++) { if((buffers[i].length&&!buffers[i].buffer)||buffers[i].length>CIUKI_SOCKET_IO_MAX-bytes)return fail(10040); bytes+=buffers[i].length; }
    const struct w32_services *services=w32_get_services(); if(!services||!services->alloc||!services->free)return fail(10055);
    char *data=services->alloc(services->opaque,bytes?bytes:1); if(!data)return fail(10055);
    unsigned at=0; if(sending)for(unsigned i=0;i<count;i++) { w32_copy(data+at,buffers[i].buffer,buffers[i].length); at+=buffers[i].length; }
    unsigned moved=0,received=0;
    int r=transfer_ex(s,data,(int)bytes,(int)(sending?send_flags:*receive_flags),address,length,sending,0,&moved,&received);
    if(r>=0 || (!sending && WSAGetLastError()==10040)) {
        *done=moved;
        if(!sending) { at=0;for(unsigned i=0;i<count && at<moved;i++) { unsigned n=buffers[i].length;if(n>moved-at)n=moved-at;w32_copy(buffers[i].buffer,data+at,n);at+=n; }*receive_flags=(received&MSG_TRUNC)?0x8000u:0; }
    }
    services->free(services->opaque,data,bytes?bytes:1); return r<0?-1:0;
}
int W32API WSASend(uintptr_t s,struct ws_buffer *b,unsigned n,ULONG *done,unsigned flags,void *ov,void *completion) { return vector_io(s,b,n,done,0,flags,0,0,ov,completion,1); }
int W32API WSARecv(uintptr_t s,struct ws_buffer *b,unsigned n,ULONG *done,ULONG *flags,void *ov,void *completion) { return vector_io(s,b,n,done,flags,0,0,0,ov,completion,0); }
int W32API WSASendTo(uintptr_t s,struct ws_buffer *b,unsigned n,ULONG *done,unsigned flags,const void *a,int length,void *ov,void *completion) { return vector_io(s,b,n,done,0,flags,(void *)a,length,ov,completion,1); }
int W32API WSARecvFrom(uintptr_t s,struct ws_buffer *b,unsigned n,ULONG *done,ULONG *flags,void *a,int *length,void *ov,void *completion) { return vector_io(s,b,n,done,flags,0,a,(intptr_t)length,ov,completion,0); }
int W32API getaddrinfo(const char *node,const char *service,const struct ws_addrinfo *hints,struct ws_addrinfo **out) {
    if(!initialized())return 10093; if(!out)return 10014; *out=0;
    struct ws_addrinfo h={0}; if(hints)h=*hints;
    if(h.flags&~(1|4|8))return 10022; if(h.family!=0&&h.family!=AF_INET)return 10047;
    if(h.socktype&&h.socktype!=SOCK_STREAM&&h.socktype!=SOCK_DGRAM)return 10044;
    if(h.protocol&&h.protocol!=IPPROTO_TCP&&h.protocol!=IPPROTO_UDP)return 10043;
    if((h.socktype==SOCK_STREAM&&h.protocol==IPPROTO_UDP)||(h.socktype==SOCK_DGRAM&&h.protocol==IPPROTO_TCP))return 10041;
    if(!node&&!service)return 11001; unsigned port=0;
    if(service) { if(!*service)return 10109; for(const char *p=service;*p;p++) { if(*p<'0'||*p>'9')return 10109; port=port*10+(unsigned)(*p-'0'); if(port>65535)return 10109; } }
    uint32_t address=(h.flags&1)?0:0x0100007f;
    if(node) { if(h.flags&4) { address=inet_addr(node); if(address==~0u&&w32_strcmp(node,"255.255.255.255"))return 11001; } else { int error=resolve(node,&address); if(error)return error; } }
    const struct w32_services *services=w32_get_services(); if(!services||!services->alloc||!services->free)return 10055;
    struct ws_answer *answer=services->alloc(services->opaque,sizeof(*answer)); if(!answer)return 10055; w32_fill(answer,0,sizeof(*answer));
    int type=h.socktype?h.socktype:h.protocol==IPPROTO_UDP?SOCK_DGRAM:SOCK_STREAM;
    answer->address=(struct ciuki_sockaddr_in){.family=AF_INET,.port=htons((uint16_t)port),.address=address};
    answer->info=(struct ws_addrinfo){.flags=h.flags,.family=AF_INET,.socktype=type,.protocol=type==SOCK_DGRAM?IPPROTO_UDP:IPPROTO_TCP,.length=16,.address=&answer->address};
    *out=&answer->info; return 0;
}
void W32API freeaddrinfo(struct ws_addrinfo *info) { const struct w32_services *services=w32_get_services(); if(!services||!services->free)return; while(info) { struct ws_addrinfo *next=info->next; services->free(services->opaque,info,sizeof(struct ws_answer)); info=next; } }

#include "wsasync.inc"
