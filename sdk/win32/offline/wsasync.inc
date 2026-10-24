/* Winsock notifications use a single bounded worker and existing Win32
 * threads/events/window messages. No desktop protocol or syscall allocation. */
#include <w32/ldr.h>
static void *async_proc(const char *dll,const char *name)
{
#ifdef W32_HOST_MODEL
    extern void *ws_async_proc(const char *,const char *);
    return ws_async_proc(dll,name);
#else
    WCHAR wide[32];unsigned i=0;for(;dll[i] && i<31;i++)wide[i]=(unsigned char)dll[i];wide[i]=0;
    UNICODE_STRING module={(uint16_t)(i*2),(uint16_t)((i+1)*2),wide};void *base=0,*proc=0;
    if(LdrGetDllHandle(0,0,&module,&base))return 0;
    ANSI_STRING function={(uint16_t)w32_strlen(name),(uint16_t)(w32_strlen(name)+1),(char *)name};
    if(LdrGetProcedureAddress(base,&function,0,&proc))return 0;return proc;
#endif
}
static HANDLE (W32API *async_thread)(void *,DWORD,DWORD(W32API *)(void *),void *,DWORD,DWORD *);
static BOOL (W32API *async_close)(HANDLE);
static void (W32API *async_sleep)(DWORD);
static BOOL (W32API *async_post)(HWND,UINT,WPARAM,LPARAM);
static BOOL (W32API *async_set)(HANDLE), (W32API *async_reset)(HANDLE);
static HANDLE (W32API *async_create_event)(void *,BOOL,BOOL,const char *);
static DWORD (W32API *async_wait)(DWORD,const HANDLE *,BOOL,DWORD,BOOL);
static unsigned async_worker_live;
struct ws_async_host {
    uintptr_t token;
    HWND window;
    UINT message;
    char name[254];
    char *buffer;
    int bytes;
    struct ciuki_resolve_query query;
};
static struct ws_async_host async_hosts[8];
static uint32_t next_async=1;
struct ws_network_events { int32_t events;int errors[10]; };
static struct ws_slot *async_slot(uintptr_t s)
{ for(unsigned i=0;i<WS_SLOTS;i++)if(sockets[i].handle==s && s)return &sockets[i];return 0; }
static void async_rearm(uintptr_t s,unsigned events)
{ lock();struct ws_slot *slot=async_slot(s);if(slot)slot->armed|=events&slot->mask;unlock(); }
static int async_record(struct ws_slot *,unsigned,int);
static void async_connect(uintptr_t s,int result_)
{
    int error=result_<0?WSAGetLastError():0;
    lock();struct ws_slot *slot=async_slot(s);
    if(slot) { slot->connecting=result_==0 || error==10035;if(error && error!=10035)async_record(slot,16,error); }
    unlock();
}
static void async_inherit(uintptr_t parent,uintptr_t child)
{
    lock();struct ws_slot *p=async_slot(parent),*c=async_slot(child);
    if(p && c && p->mask) {
        c->window=p->window;c->event=p->event;c->message=p->message;c->mask=c->armed=p->mask;
        int flags=native(CIUKI_SYS_FCNTL,(uintptr_t)c->fd,F_GETFL,0,0);
        if(flags>=0)(void)native(CIUKI_SYS_FCNTL,(uintptr_t)c->fd,F_SETFL,(unsigned)flags|O_NONBLOCK,0);
    }
    unlock();
}
static int async_record(struct ws_slot *s,unsigned bit,int error)
{
    if(!(s->mask&s->armed&bit))return 0;
    unsigned index=0;for(unsigned b=bit;b>1;b>>=1)index++;
    if(s->window) {
        if(!async_post || !async_post(s->window,s->message,(WPARAM)s->handle,(LPARAM)(bit|((unsigned)error<<16))))return 0;
    } else {
        s->recorded|=bit;s->errors[index]=error;
        if(s->event && async_set)async_set(s->event);
    }
    s->armed&=~bit;return 1;
}
static void overlapped_step(void);
static void overlapped_cleanup(void);
static void async_step(void)
{
    /* A duplicated fd is retained only for this nonblocking poll. Recheck the
     * monotonically allocated public handle before writing any notification. */
    for(unsigned i=0;i<WS_SLOTS;i++) {
        lock();struct ws_slot *s=&sockets[i];uintptr_t h=s->handle;
        int fd=-1;
        if(starts && h && s->mask)fd=native(CIUKI_SYS_FCNTL,(uintptr_t)s->fd,F_DUPFD_CLOEXEC,0,0);
        unlock();if(fd<0)continue;
        struct ciuki_pollfd p={fd,POLLIN|POLLOUT|POLLERR,0};
        int r=native(CIUKI_SYS_POLL,(uintptr_t)&p,1,0,0);
        lock();s=&sockets[i];
        if(starts && s->handle==h && r>0) {
            int error=0;
            if(p.revents&POLLERR) {
                unsigned length=4;struct ciuki_socket_option o={.size=sizeof(o),.version=1,.fd=fd,.level=SOL_SOCKET,.name=SO_ERROR,.value=WP(&error),.length=WP(&length)};
                if(native(CIUKI_SYS_SOCKOPT,(uintptr_t)&o,0,0,0)<0)error=ENETDOWN;
                error=error?win_error(error):0;
            }
            if(s->connecting && (p.revents&(POLLOUT|POLLERR|POLLHUP))) {
                if(async_record(s,16,error))s->connecting=0;
            }
            if(p.revents&POLLOUT)async_record(s,2,error);
            if(p.revents&POLLIN) {
                if(s->listening)async_record(s,8,error);
                else {
                    unsigned available=0,length=4;struct ciuki_socket_option o={.size=sizeof(o),.version=1,.fd=fd,.level=SOL_SOCKET,.name=SO_RX_BYTES,.value=WP(&available),.length=WP(&length)};
                    int e=native(CIUKI_SYS_SOCKOPT,(uintptr_t)&o,0,0,0);
                    if(s->type!=SOCK_STREAM || available)async_record(s,1,error);
                    else if(!e)async_record(s,32,error);
                }
            }
            if(p.revents&POLLHUP)async_record(s,32,error);
        }
        unlock();release(fd);
    }
    overlapped_step();
    for(unsigned i=0;i<8;i++) {
        lock();struct ws_async_host *h=&async_hosts[i];
        if(h->token) {
            int error=0;
            if(h->query.status==4) {
                h->query.action=CIUKI_RESOLVE_POLL;
                int r=native(CIUKI_SYS_RESOLVE4,(uintptr_t)&h->query,0,0,0);
                if(r<0)error=win_error(-r);
            }
            if(error || h->query.status!=4) {
                if(!error && h->query.status)error=h->query.status==2?11002:11001;
                unsigned name_bytes=w32_strlen(h->name)+1;
                unsigned required=sizeof(struct ws_hostent)+sizeof(char *)*3+4+name_bytes;
                if(!error && (unsigned)h->bytes<required)error=10055;
                if(!error) {
                    /* memcpy each naturally unaligned field: the caller's
                     * buffer need not have hostent or pointer alignment. */
                    char *base=h->buffer;char **aliases=(void *)(base+sizeof(struct ws_hostent));
                    char **addresses=aliases+1;char *ip=(void *)(addresses+2);char *name=ip+4;
                    char *zero=0;
                    w32_copy(aliases,&zero,sizeof(zero));w32_copy(addresses,&ip,sizeof(ip));w32_copy(addresses+1,&zero,sizeof(zero));
                    w32_copy(ip,&h->query.address,4);w32_copy(name,h->name,name_bytes);
                    struct ws_hostent host={name,aliases,AF_INET,4,addresses};w32_copy(base,&host,sizeof(host));
                }
                if(async_post)async_post(h->window,h->message,(WPARAM)h->token,(LPARAM)(required|((unsigned)error<<16)));
                h->token=0;
            }
        }
        unlock();
    }
}
static DWORD W32API async_main(void *unused)
{
    (void)unused;
    for(;;) {
        lock();int alive=starts!=0;if(!alive)async_worker_live=0;unlock();if(!alive)return 0;
        async_step();async_sleep(10);
    }
}
static int async_initialize(void)
{
    lock();
    if(!async_thread) {
        async_thread=async_proc("kernel32.dll","CreateThread");async_close=async_proc("kernel32.dll","CloseHandle");
        async_sleep=async_proc("kernel32.dll","Sleep");async_post=async_proc("user32.dll","PostMessageA");
        async_set=async_proc("kernel32.dll","SetEvent");async_reset=async_proc("kernel32.dll","ResetEvent");
        async_create_event=async_proc("kernel32.dll","CreateEventA");async_wait=async_proc("kernel32.dll","WaitForMultipleObjectsEx");
    }
    int ready=async_thread && async_close && async_sleep && async_set && async_reset;
    int launch=ready && !async_worker_live;if(launch)async_worker_live=1;
    unlock();if(!ready)return fail(10045);
    if(launch) {
        HANDLE thread=async_thread(0,128*1024,async_main,0,0,0);
        if(!thread) { lock();async_worker_live=0;unlock();return fail(10055); }
        async_close(thread);
    }
    return 0;
}
static void async_cleanup(void)
{
    /* Called under guard by final WSACleanup. Cancellation is serialized
     * against both completion-buffer writes and completion posting. */
    overlapped_cleanup();
    for(unsigned i=0;i<8;i++)if(async_hosts[i].token) {
        struct ciuki_resolve_query *q=&async_hosts[i].query;
        if(q->token && q->status==4) { q->action=CIUKI_RESOLVE_CANCEL;(void)native(CIUKI_SYS_RESOLVE4,(uintptr_t)q,0,0,0); }
        async_hosts[i].token=0;
    }
}
static int async_select(uintptr_t h,HWND window,UINT message,HANDLE event,unsigned mask)
{
    if(!initialized())return -1;if(mask&~0x3bu)return fail(10022);
    if(mask && ((!window && !event) || (window && !message)))return fail(10022);
    if(async_initialize())return -1;
    if(window && !async_post)return fail(10045);
    int fd=acquire(h,0);if(fd<0)return -1;
    int r=native(CIUKI_SYS_FCNTL,(uintptr_t)fd,F_GETFL,0,0);
    if(r>=0)r=native(CIUKI_SYS_FCNTL,(uintptr_t)fd,F_SETFL,(unsigned)r|O_NONBLOCK,0);
    release(fd);if(r<0)return result(r);
    lock();struct ws_slot *s=async_slot(h);if(!s) { unlock();return fail(10038); }
    s->window=window;s->message=message;s->event=event;s->mask=s->armed=mask;s->recorded=0;
    w32_fill(s->errors,0,sizeof(s->errors));unlock();return 0;
}
int W32API WSAAsyncSelect(uintptr_t s,HWND w,UINT msg,int32_t events) { return async_select(s,w,msg,0,(unsigned)events); }
int W32API WSAEventSelect(uintptr_t s,HANDLE e,int32_t events) { return async_select(s,0,0,e,(unsigned)events); }
int W32API WSAEnumNetworkEvents(uintptr_t h,HANDLE event,struct ws_network_events *out)
{
    if(!initialized())return -1;if(!out)return fail(10014);
    lock();struct ws_slot *s=async_slot(h);if(!s) { unlock();return fail(10038); }
    if(event && (!async_reset || !async_reset(event))) { unlock();return fail(10022); }
    w32_fill(out,0,sizeof(*out));out->events=(int32_t)s->recorded;
    for(unsigned i=0;i<6;i++)out->errors[i]=s->errors[i];s->recorded=0;unlock();return 0;
}
HANDLE W32API WSACreateEvent(void)
{ if(async_initialize() || !async_create_event)return 0;return async_create_event(0,1,0,0); }
BOOL W32API WSACloseEvent(HANDLE e) { if(async_initialize())return 0;return async_close(e); }
BOOL W32API WSASetEvent(HANDLE e) { if(async_initialize())return 0;return async_set(e); }
BOOL W32API WSAResetEvent(HANDLE e) { if(async_initialize())return 0;return async_reset(e); }
DWORD W32API WSAWaitForMultipleEvents(DWORD count,const HANDLE *events,BOOL all,DWORD timeout,BOOL alertable)
{ if(!count || count>64 || !events) { fail(10022);return ~0u; }if(async_initialize() || !async_wait)return ~0u;return async_wait(count,events,all,timeout,alertable); }
uintptr_t W32API WSAAsyncGetHostByName(HWND window,UINT message,const char *name,char *buffer,int bytes)
{
    if(!initialized())return 0;
    if(!window || !message || !name || !buffer || bytes<0) { fail(10014);return 0; }
    unsigned n=0;while(n<254 && name[n])n++;if(!n || n==254) { fail(11001);return 0; }
    if(async_initialize() || !async_post)return 0;
    uint32_t address=0;int local=resolve_local(name,&address);
    lock();struct ws_async_host *h=0;
    if(next_async<0x7fffffffU)for(unsigned i=0;i<8;i++)if(!async_hosts[i].token) { h=&async_hosts[i];break; }
    if(!h) { unlock();fail(10055);return 0; }
    *h=(struct ws_async_host){.token=next_async++,.window=window,.message=message,.buffer=buffer,.bytes=bytes};
    w32_copy(h->name,name,n+1);
    h->query=(struct ciuki_resolve_query){.size=sizeof(h->query),.version=2,.action=CIUKI_RESOLVE_START,.address=address};
    if(!local) {
        h->query.name=WP(h->name);h->query.name_bytes=n+1;
        int r=native(CIUKI_SYS_RESOLVE4,(uintptr_t)&h->query,0,0,0);h->query.name=h->query.name_bytes=0;
        if(r<0) { h->token=0;unlock();fail(win_error(-r));return 0; }
    }
    uintptr_t token=h->token;unlock();return token;
}
int W32API WSACancelAsyncRequest(uintptr_t token)
{
    if(!initialized())return -1;lock();
    for(unsigned i=0;i<8;i++)if(token && async_hosts[i].token==token) {
        struct ciuki_resolve_query *q=&async_hosts[i].query;
        if(q->token && q->status==4) { q->action=CIUKI_RESOLVE_CANCEL;(void)native(CIUKI_SYS_RESOLVE4,(uintptr_t)q,0,0,0); }
        async_hosts[i].token=0;unlock();return 0;
    }
    unlock();return fail(10022);
}

#include "wsoverlapped.inc"
