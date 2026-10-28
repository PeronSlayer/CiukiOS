/* G26 standalone host oracle; include the reviewed production facade. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <w32/ntdll.h>
static uint32_t wire_pointer(uintptr_t);
#include "wsock_host.inc"
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"Winsock oracle line %d: %s\n",__LINE__,#c); exit(1); } } while(0)
static struct w32_teb test_teb;
_Thread_local struct w32_teb *w32_model_teb=&test_teb;
void *w32_copy(void *to,const void *from,uint32_t n) { return memcpy(to,from,n); }
void *w32_fill(void *to,uint8_t byte,uint32_t n) { return memset(to,byte,n); }
uint32_t w32_strlen(const char *s) { return (uint32_t)strlen(s); }
int w32_strcmp(const char *a,const char *b) { return strcmp(a,b); }
int w32_stricmp(const char *a,const char *b) { while(*a&&*b) { unsigned char x=(unsigned char)*a++,y=(unsigned char)*b++; if(x>='A'&&x<='Z')x+=(unsigned char)('a'-'A'); if(y>='A'&&y<='Z')y+=(unsigned char)('a'-'A'); if(x!=y)return (int)x-(int)y; } return (unsigned char)*a-(unsigned char)*b; }
void w32_diag(const char *a,const char *b,const char *c) { (void)a;(void)b;(void)c; }
static union { uint64_t alignment; unsigned char bytes[CIUKI_SOCKET_IO_MAX+128]; } allocations[12];
static unsigned allocated[12],allocation_count;
static void *model_alloc(void *opaque,uint32_t bytes) { (void)opaque; CHECK(bytes<=sizeof(allocations[0].bytes)); for(unsigned i=0;i<12;i++)if(!allocated[i]) { allocated[i]=1; allocation_count++; memset(allocations[i].bytes,0,bytes); return allocations[i].bytes; } return 0; }
static void model_free(void *opaque,void *address,uint32_t bytes) { (void)opaque;(void)bytes; for(unsigned i=0;i<12;i++)if(address==allocations[i].bytes) { CHECK(allocated[i]); allocated[i]=0; allocation_count--; return; } CHECK(0); }
static const struct w32_services services={.alloc=model_alloc,.free=model_free};
const struct w32_services *w32_get_services(void) { return &services; }
/* The wire stays 32-bit even when host stack/static pointers share low bits.
 * Tokens retain the full host pointer instead of guessing its upper bits. */
static uintptr_t wire_pointers[256];
static unsigned wire_pointer_count;
static uint32_t wire_pointer(uintptr_t pointer)
{
    if (!pointer) return 0;
    for (unsigned i=0;i<wire_pointer_count;i++) if (wire_pointers[i]==pointer) return i+1;
    CHECK(wire_pointer_count<sizeof(wire_pointers)/sizeof(wire_pointers[0]));
    wire_pointers[wire_pointer_count++]=pointer;
    return wire_pointer_count;
}
static void *field_pointer(uint32_t token,uintptr_t record)
{ (void)record; CHECK(token && token<=wire_pointer_count); return (void *)wire_pointers[token-1]; }
struct object { int used,type,family,references,flags,ready; struct ciuki_socket_timeval timeout; uint32_t position; struct ciuki_sockaddr_in address; };
static struct object objects[32];
static struct object *descriptors[128];
static int failure_op=-1,failure_error;
static unsigned native_calls,created,closed,dns_queries,hosts_opens,sends;
static struct ciuki_w32_apc_args queued_apc;
static unsigned completion_calls;
static void W32API completion_(DWORD error,DWORD bytes_,OVERLAPPED *ov,DWORD flags_)
{ CHECK(!error && bytes_==5 && ov && !flags_);completion_calls++; }
int32_t w32_ciuki_call(void *record,uint32_t bytes_)
{
    struct ciuki_w32_header *h=record;CHECK(h->size==bytes_);
    if(h->operation==W32_OBJECT) {
        struct ciuki_w32_object_args *a=record;CHECK(a->action==W32_OBJ_DUPLICATE || a->action==W32_OBJ_CLOSE);a->result=17;return 0;
    }
    CHECK(h->operation==W32_APC);struct ciuki_w32_apc_args *a=record;
    queued_apc=*a;a->result=1;return 0;
}
static unsigned posted,signaled;static WPARAM posted_w;static LPARAM posted_l;
static HANDLE W32API fake_thread(void *sa,DWORD size,DWORD(W32API *start)(void *),void *arg,DWORD flags_,DWORD *id)
{ (void)sa;(void)size;(void)start;(void)arg;(void)flags_;(void)id;return (HANDLE)1; }
static BOOL W32API fake_close(HANDLE h) { (void)h;return 1; }
static void W32API fake_sleep(DWORD ms) { (void)ms; }
static BOOL W32API fake_post(HWND w,UINT m,WPARAM wp,LPARAM lp) { CHECK(w && m);posted++;posted_w=wp;posted_l=lp;return 1; }
static BOOL W32API fake_set(HANDLE h) { CHECK(h);signaled++;return 1; }
static BOOL W32API fake_reset(HANDLE h) { CHECK(h);signaled=0;return 1; }
static HANDLE W32API fake_event(void *s,BOOL manual,BOOL initial,const char *n)
{ (void)s;CHECK(manual && !initial && !n);return (HANDLE)2; }
static DWORD W32API fake_wait(DWORD count,const HANDLE *e,BOOL all,DWORD timeout,BOOL alert)
{ (void)e;(void)all;(void)timeout;(void)alert;CHECK(count);return signaled?0:258; }
void *ws_async_proc(const char *dll,const char *name)
{
    (void)dll;
    if(!strcmp(name,"CreateThread"))return fake_thread;
    if(!strcmp(name,"CloseHandle"))return fake_close;
    if(!strcmp(name,"Sleep"))return fake_sleep;
    if(!strcmp(name,"PostMessageA"))return fake_post;
    if(!strcmp(name,"SetEvent"))return fake_set;
    if(!strcmp(name,"ResetEvent"))return fake_reset;
    if(!strcmp(name,"CreateEventA"))return fake_event;
    if(!strcmp(name,"WaitForMultipleObjectsEx"))return fake_wait;
    return 0;
}
static int last_native_fd,last_level,last_option,last_poll_timeout;
static struct ciuki_socket_timeval last_timeout;
static char transmitted[128]; static unsigned transmitted_length;
static const char hosts_text[]="# local test database\r\n10.20.30.40 sample.example ALIAS # note\r\n";
static int descriptor(struct object *object) { for(int fd=3;fd<128;fd++)if(!descriptors[fd]) { descriptors[fd]=object; object->references++; return fd; } return -EMFILE; }
static int create(int type) { for(unsigned i=0;i<32;i++)if(!objects[i].used) { objects[i]=(struct object){.used=1,.type=type,.ready=POLLOUT}; return descriptor(&objects[i]); } return -ENOBUFS; }
static struct object *lookup(uintptr_t fd) { return fd<128 ? descriptors[fd] : 0; }
int32_t ws_native_model(uint32_t op,uintptr_t a,uintptr_t b,uintptr_t c,uintptr_t d) {
    native_calls++; if((int)op==failure_op) { failure_op=-1; return -failure_error; }
    struct object *object=lookup(a);
    if(op==CIUKI_SYS_SOCKET) { CHECK(a==AF_INET || a==AF_IPX); int fd=create((int)b); if(fd>=0) { descriptors[fd]->family=(int)a; descriptors[fd]->address.family=(uint16_t)a; } created++; last_native_fd=fd; return fd; }
    if(op==CIUKI_SYS_OPEN) { if(!strcmp((const char *)a,"/system/settings/LMHOSTS"))return -ENOENT;CHECK(!strcmp((const char *)a,"/system/settings/HOSTS")); CHECK(b==(O_RDONLY|O_CLOEXEC)); hosts_opens++; return create(0x100); }
    if(op==CIUKI_SYS_CLOSE) { if(!object)return -EBADF; descriptors[a]=0; closed++; if(!--object->references)object->used=0; return 0; }
    if(op==CIUKI_SYS_FCNTL) { if(!object)return -EBADF; if(b==F_DUPFD_CLOEXEC)return descriptor(object); if(b==F_GETFL)return object->flags; if(b==F_SETFL) { object->flags=(int)c; return 0; } return -EINVAL; }
    if(op==CIUKI_SYS_READ) { CHECK(object&&object->type==0x100); unsigned remaining=(unsigned)sizeof(hosts_text)-1-object->position; if(c>remaining)c=remaining; memcpy((void *)b,hosts_text+object->position,c); object->position+=(uint32_t)c; return (int)c; }
    if(op==CIUKI_SYS_BIND||op==CIUKI_SYS_CONNECT) { CHECK(object&&c==16&&!d); CHECK(((const struct ciuki_sockaddr_in *)b)->family==object->family); memcpy(&object->address,(void *)b,16); return 0; }
    if(op==CIUKI_SYS_LISTEN) { CHECK(object&&object->type==SOCK_STREAM); return 0; }
    if(op==CIUKI_SYS_ACCEPT) { CHECK(object); if(b) { struct ciuki_sockaddr_in addr={.family=AF_INET,.port=0x5000,.address=0x0100007f}; memcpy((void *)b,&addr,16); *(int *)c=16; } return create(SOCK_STREAM); }
    if(op==CIUKI_SYS_SHUTDOWN) { CHECK(object); return b>2?-EINVAL:0; }
    if(op==CIUKI_SYS_SOCKNAME) { CHECK(object&&c&&d); struct ciuki_sockaddr_in addr={.family=AF_INET,.port=0x5000,.address=0x0100007f}; if(object->family==AF_IPX)addr=object->address; memcpy((void *)c,&addr,16); *(int *)d=16; return 0; }
    if(op==CIUKI_SYS_SENDTO||op==CIUKI_SYS_RECVFROM) {
        struct ciuki_socket_io *io=(void *)a; CHECK(io->size==40&&io->version==2&&!io->reserved[1]);
        CHECK(lookup((uintptr_t)io->fd)); object=lookup((uintptr_t)io->fd); CHECK(io->bytes<=CIUKI_SOCKET_IO_MAX);
        void *data=field_pointer(io->buffer,a);
        if(op==CIUKI_SYS_SENDTO) { CHECK((io->flags&~MSG_DONTWAIT)==MSG_NOSIGNAL); CHECK(io->bytes<=sizeof(transmitted)); memcpy(transmitted,data,io->bytes); transmitted_length=io->bytes; sends++; return (int)io->bytes; }
        if(io->reserved[0])*(unsigned *)field_pointer(io->reserved[0],a)=io->bytes<5 && lookup(io->fd)->type==SOCK_DGRAM?MSG_TRUNC:0;
        unsigned n=io->bytes<5?io->bytes:5; memcpy(data,"reply",n);
        if(io->address) { struct ciuki_sockaddr_in addr={.family=AF_INET,.port=0x5000,.address=0x0100007f}; if(object->family==AF_IPX)addr=object->address; memcpy(field_pointer(io->address,a),&addr,16); *(int *)field_pointer(io->address_length,a)=16; } return (int)n;
    }
    if(op==CIUKI_SYS_SOCKOPT) {
        struct ciuki_socket_option *request=(void *)a; CHECK(request->size==40&&request->version==1); object=lookup((uintptr_t)request->fd); CHECK(object);
        last_level=request->level; last_option=request->name; void *value=field_pointer(request->value,a);
        unsigned n=request->operation?request->length:*(unsigned *)field_pointer(request->length,a);
        if(request->level==NSPROTO_IPX) {
            CHECK(n==(request->name==IPX_ADDRESS?24:request->name==IPX_GETNETINFO?20:4));
            if(!request->operation && request->name==IPX_ADDRESS) {
                struct ciuki_ipx_address_data info={.status=1,.maxpkt=1500};memcpy(info.nodenum,"abcdef",6);memcpy(value,&info,24);
            }
            return 0;
        }
        if(request->level==SOL_SOCKET&&(request->name==SO_SNDTIMEO||request->name==SO_RCVTIMEO)) { CHECK(n==16); if(request->operation) { memcpy(&object->timeout,value,16); last_timeout=object->timeout; } else memcpy(value,&object->timeout,16); }
        else { CHECK(n==4); if(!request->operation) { int v=request->name==SO_ERROR?ECONNREFUSED:request->name==SO_TYPE?object->type:1; memcpy(value,&v,4); } }
        return 0;
    }
    if(op==CIUKI_SYS_POLL) { struct ciuki_pollfd *fds=(void *)a; int ready=0; last_poll_timeout=(int)c; for(unsigned i=0;i<b;i++) { object=lookup((uintptr_t)fds[i].fd); CHECK(object); fds[i].revents=(int16_t)(object->ready&(fds[i].events|POLLERR|POLLHUP)); ready+=fds[i].revents!=0; } return ready; }
    if(op==CIUKI_SYS_NET_INFO) { struct ciuki_net_control *c=(void *)b;c->count=0;return 0; }
    if(op==CIUKI_SYS_RESOLVE4 && ((uint32_t *)a)[1]==2) {
        struct ciuki_resolve_query *q=(void *)a;
        if(q->action==CIUKI_RESOLVE_START) { q->token=123;q->status=4; }
        else if(q->action==CIUKI_RESOLVE_POLL) { q->address=0x04030201;q->status=0; }
        else CHECK(q->action==CIUKI_RESOLVE_CANCEL);
        return 0;
    }
    if(op==CIUKI_SYS_RESOLVE4) { struct ciuki_resolve4 *request=(void *)a; CHECK(request->size==32&&request->version==1); const char *name=field_pointer(request->name,a); dns_queries++; if(!strcmp(name,"remote.example")) { request->address=0x04030201; request->status=0; } else request->status=1; return 0; }
    CHECK(0); return -ENOSYS;
}
static void no_descriptors(void) { for(unsigned i=0;i<128;i++)CHECK(!descriptors[i]); }
static struct ws_data data;
static struct ciuki_sockaddr_in address={.family=AF_INET,.port=0x5000,.address=0x0100007f};
static char bytes[32],left[8],right[8]; static int address_length; static ULONG flags,done;
int main(void) {
    setvbuf(stdout,NULL,_IOLBF,0);
#if UINTPTR_MAX > UINT32_MAX
    uintptr_t low=0x1234,high=UINT64_C(0x100001234);
    CHECK((uint32_t)low==(uint32_t)high);
    CHECK(wire_pointer(low)!=wire_pointer(high));
    CHECK((uintptr_t)field_pointer(wire_pointer(low),0)==low && (uintptr_t)field_pointer(wire_pointer(high),0)==high);
#endif
    test_teb.LastErrorValue=77;
    CHECK(socket(AF_INET,SOCK_STREAM,0)==WS_INVALID&&WSAGetLastError()==10093);
    CHECK(WSACleanup()==-1&&WSAGetLastError()==10093);
    CHECK(WSAStartup(0,&data)==10092); CHECK(WSAStartup(0x101,0)==10014);
    CHECK(WSAStartup(0x101,&data)==0&&data.version==0x101&&data.high==0x202&&data.max_sockets==16);
    CHECK(WSAStartup(0x303,&data)==0&&data.version==0x202); CHECK(WSACleanup()==0);
    CHECK(closesocket(0)==-1&&WSAGetLastError()==10038);
    uintptr_t a=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP); CHECK(a!=WS_INVALID); int original=last_native_fd;
    CHECK(bind(a,&address,16)==0&&connect(a,&address,16)==0&&listen(a,4)==0);
    CHECK(shutdown(a,3)==-1&&WSAGetLastError()==10022);
    CHECK(send(a,"hello",5,0)==5&&transmitted_length==5&&!memcmp(transmitted,"hello",5));
    CHECK(send(a,"",-1,0)==-1&&WSAGetLastError()==10022);
    CHECK(recv(a,bytes,2,0)==2&&!memcmp(bytes,"re",2));
    address_length=16; CHECK(getsockname(a,&address,&address_length)==0&&address_length==16);
    address_length=4; CHECK(getpeername(a,&address,&address_length)==-1&&WSAGetLastError()==10014);
    ULONG nonblocking=1; CHECK(ioctlsocket(a,(int)0x8004667eU,&nonblocking)==0&&(descriptors[original]->flags&O_NONBLOCK));
    nonblocking=0; CHECK(ioctlsocket(a,(int)0x8004667eU,&nonblocking)==0&&!(descriptors[original]->flags&O_NONBLOCK));
    ULONG timeout=2345; CHECK(setsockopt(a,0xffff,0x1005,(char *)&timeout,4)==0); CHECK(last_level==SOL_SOCKET&&last_option==SO_SNDTIMEO&&last_timeout.seconds==2&&last_timeout.microseconds==345000);
    timeout=0; int n=4; CHECK(getsockopt(a,0xffff,0x1005,(char *)&timeout,&n)==0&&timeout==2345&&n==4);
    int error=0; n=4; CHECK(getsockopt(a,0xffff,0x1007,(char *)&error,&n)==0&&error==10061);
    CHECK(setsockopt(a,0xffff,1234,(char *)&error,4)==-1&&WSAGetLastError()==10042);
    struct ws_set read={.count=1,.socket={a}},write=read,except=read; struct ws_timeval wait={.microseconds=1};
    descriptors[original]->ready=POLLIN|POLLOUT; CHECK(select(-99,&read,&write,&except,&wait)==2&&read.count==1&&write.count==1&&!except.count&&last_poll_timeout==1);
    read=(struct ws_set){.count=1,.socket={a}};write=read;except=read; descriptors[original]->ready=POLLOUT|POLLERR;
    CHECK(select(0,&read,&write,&except,&wait)==2&&read.count==1&&!write.count&&except.count==1);
    CHECK(select(0,0,0,0,&wait)==-1&&WSAGetLastError()==10022);
    uintptr_t notify=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);int notify_fd=last_native_fd;
    unsigned old_posts=posted;
    CHECK(!WSAAsyncSelect(notify,(HWND)1,0x401,1|2|32));descriptors[notify_fd]->ready=POLLIN|POLLOUT;
    async_step();CHECK(posted==old_posts+2);async_step();CHECK(posted==old_posts+2);
    CHECK(recv(notify,bytes,2,0)==2);async_step();CHECK(posted==old_posts+3);
    CHECK(!WSAEventSelect(notify,(HANDLE)2,1));async_step();CHECK(signaled);
    struct ws_network_events events;CHECK(!WSAEnumNetworkEvents(notify,(HANDLE)2,&events) && events.events==1 && !signaled);
    async_step();CHECK(!signaled);CHECK(recv(notify,bytes,2,0)==2);async_step();CHECK(signaled);
    CHECK(!WSAEventSelect(notify,0,0));
    CHECK(!closesocket(notify));
    char dns_buffer[1024];memset(dns_buffer,0x5a,sizeof(dns_buffer));old_posts=posted;
    uintptr_t task=WSAAsyncGetHostByName((HWND)1,0x402,"remote.example",dns_buffer,sizeof(dns_buffer));CHECK(task);
    CHECK(!WSACancelAsyncRequest(task));async_step();CHECK(posted==old_posts && dns_buffer[0]==0x5a);
    task=WSAAsyncGetHostByName((HWND)1,0x402,"remote.example",dns_buffer,sizeof(dns_buffer));CHECK(task);async_step();
    CHECK(posted==old_posts+1 && posted_w==task && !(posted_l>>16));
    struct ws_hostent async_host;memcpy(&async_host,dns_buffer,sizeof(async_host));CHECK(!strcmp(async_host.name,"remote.example"));hosts_opens=0;
    CHECK(closesocket(a)==0&&!descriptors[original]); uintptr_t b=socket(AF_INET,SOCK_DGRAM,0); CHECK(b!=a&&last_native_fd==original);
    CHECK(send(a,"hello",5,0)==-1&&WSAGetLastError()==10038); CHECK(closesocket(a)==-1&&WSAGetLastError()==10038);
    address_length=16; CHECK(recvfrom(b,bytes,32,0,&address,&address_length)==5&&address_length==16&&!memcmp(bytes,"reply",5));
    CHECK(recvfrom(b,bytes,2,0,0,0)==-1 && WSAGetLastError()==10040 && !memcmp(bytes,"re",2));
    ULONG available;CHECK(!ioctlsocket(b,(int)0x4004667fU,&available) && available==1);
    struct ws_buffer vector[2]={{2,left},{3,right}}; memcpy(left,"ab",2);memcpy(right,"cde",3);done=0;
    CHECK(WSASend(b,vector,2,&done,0,0,0)==0&&done==5&&!memcmp(transmitted,"abcde",5));
    memset(left,0,sizeof(left));memset(right,0,sizeof(right));flags=0;done=0;CHECK(WSARecv(b,vector,2,&done,&flags,0,0)==0&&done==5&&!memcmp(left,"re",2)&&!memcmp(right,"ply",3));
    OVERLAPPED ov={.event=(HANDLE)2};done=99;
    CHECK(!WSASend(b,vector,2,&done,0,&ov,0) && done==5 && ov.internal==0 && ov.internal_high==5);
    DWORD transferred=0,result_flags=99;CHECK(WSAGetOverlappedResult(b,&ov,&transferred,0,&result_flags) && transferred==5 && !result_flags);
    failure_op=CIUKI_SYS_RECVFROM;failure_error=EAGAIN;flags=0;done=99;
    CHECK(WSARecv(b,vector,2,&done,&flags,&ov,0)==-1 && WSAGetLastError()==997 && done==99);
    CHECK(!WSAGetOverlappedResult(b,&ov,&transferred,0,&result_flags) && WSAGetLastError()==996);
    async_step();CHECK(WSAGetOverlappedResult(b,&ov,&transferred,0,&result_flags) && transferred==5);

    CHECK(!WSASend(b,vector,2,&done,0,&ov,completion_) && !completion_calls && queued_apc.argument);
    CHECK(field_pointer(queued_apc.routine,0)==(void *)(uintptr_t)overlapped_apc);
    overlapped_apc((uintptr_t)field_pointer(queued_apc.argument,0));CHECK(completion_calls==1);
    failure_op=CIUKI_SYS_SENDTO;failure_error=ENETUNREACH;CHECK(send(b,"x",1,0)==-1&&WSAGetLastError()==10051);
    failure_op=CIUKI_SYS_CONNECT;failure_error=EINPROGRESS;CHECK(connect(b,&address,16)==-1 && WSAGetLastError()==10035);
    CHECK(inet_addr("127.1")==0x0100007f&&inet_addr("0x7f.01")==0x0100007f&&inet_addr("0177.0.0.1")==0x0100007f&&inet_addr("")==0&&inet_addr("256.0.0.1")==~0u&&inet_addr("0x100000000")==~0u);
    CHECK(htons(0x1234)==0x3412&&htonl(0x12345678)==0x78563412&&!strcmp(inet_ntoa(0x0100007f),"127.0.0.1"));
    struct ws_hostent *host=gethostbyname("ALIAS");CHECK(host&&host->family==AF_INET&&host->length==4&&*(uint32_t *)host->addresses[0]==0x281e140a&&hosts_opens==1&&!dns_queries);
    host=gethostbyname("127.1");CHECK(host&&*(uint32_t *)host->addresses[0]==0x0100007f&&!dns_queries);
    host=gethostbyname("remote.example");CHECK(host&&*(uint32_t *)host->addresses[0]==0x04030201&&dns_queries==1);
    CHECK(!gethostbyname("absent.example")&&WSAGetLastError()==11001);
    struct ws_addrinfo *info=0; CHECK(getaddrinfo("127.0.0.1","80",0,&info)==0&&info&&info->family==AF_INET&&((struct ciuki_sockaddr_in *)info->address)->port==htons(80));freeaddrinfo(info);
    CHECK(getaddrinfo("127.0.0.1","65536",0,&info)==10109&&!info);
    CHECK(((struct ws_protoent *)getprotobyname("TCP"))->protocol==IPPROTO_TCP);
    CHECK(((struct ws_servent *)getservbyname("domain","udp"))->port==(int16_t)htons(53));
    CHECK(getservbyname("http","udp")==0 && WSAGetLastError()==11004);
    CHECK(((struct ws_servent *)getservbyport(htons(443),"tcp"))->port==(int16_t)htons(443));
    CHECK(gethostname(bytes,sizeof(bytes))==0&&!strcmp(bytes,"CiukiOS"));
    CHECK(WSACleanup()==0);no_descriptors();CHECK(send(b,"x",1,0)==-1&&WSAGetLastError()==10093&&test_teb.LastErrorValue==77&&allocation_count==1);
    for(unsigned i=0;i<64;i++)CHECK(!threads[i].owner);
    CHECK(WSAStartup(0x202,&data)==0); uintptr_t c=socket(AF_INET,3,1);CHECK(c!=WS_INVALID);CHECK(closesocket(c)==0);CHECK(WSACleanup()==0);no_descriptors();
    for(unsigned i=0;i<128;i++) { CHECK(WSAStartup(0x202,&data)==0);CHECK(inet_ntoa(0x0100007f)!=0);CHECK(WSACleanup()==0); }
    CHECK(created==4&&sends==4&&native_calls>30&&closed>created);
    CHECK(!WSAStartup(0x202,&data));
    uintptr_t listener=socket(AF_INET,SOCK_STREAM,0);CHECK(listener!=WS_INVALID && !listen(listener,4));
    CHECK(!WSAAsyncSelect(listener,(HWND)1,0x410,8|32));
    uintptr_t inherited=accept(listener,0,0);CHECK(inherited!=WS_INVALID);
    CHECK(async_slot(inherited)->mask==(8|32) && (descriptors[async_slot(inherited)->fd]->flags&O_NONBLOCK));
    CHECK(!closesocket(listener) && !closesocket(inherited));
    char small[4]={0x33,0x33,0x33,0x33};unsigned posts_before=posted;
    uintptr_t short_task=WSAAsyncGetHostByName((HWND)1,0x411,"remote.example",small,sizeof(small));CHECK(short_task);async_step();
    CHECK(posted==posts_before+1 && ((unsigned)posted_l>>16)==10055 && small[0]==0x33);
    uintptr_t cancelled=WSAAsyncGetHostByName((HWND)1,0x411,"remote.example",small,sizeof(small));CHECK(cancelled);
    uintptr_t datagram=socket(AF_INET,SOCK_DGRAM,0);CHECK(datagram!=WS_INVALID);
    struct ws_buffer short_vector={2,bytes};OVERLAPPED truncated={.event=(HANDLE)2};ULONG short_flags=0,short_done=0;
    CHECK(WSARecv(datagram,&short_vector,1,&short_done,&short_flags,&truncated,0)==-1 && WSAGetLastError()==10040 && short_done==2 && short_flags==0x8000 && !memcmp(bytes,"re",2));
    struct ws_buffer full_vector={5,bytes};OVERLAPPED queued={0};
    CHECK(!WSASend(datagram,&full_vector,1,&short_done,0,&queued,completion_));
    unsigned callbacks_before=completion_calls;posts_before=posted;
    CHECK(!WSACleanup());async_step();CHECK(posted==posts_before);
    overlapped_apc((uintptr_t)field_pointer(queued_apc.argument,0));CHECK(completion_calls==callbacks_before && allocation_count==1);
    no_descriptors();
    CHECK(!WSAStartup(0x202,&data));
    uintptr_t ipx=socket(AF_IPX,SOCK_DGRAM,NSPROTO_IPX+4);CHECK(ipx!=WS_INVALID);int ipx_fd=last_native_fd;
    struct { uint8_t address[14],guard[8]; } ipx_address={0};
    uint16_t ipx_family=AF_IPX;memcpy(ipx_address.address,&ipx_family,2);memcpy(ipx_address.address+6,"abcdef",6);
    ipx_address.address[12]=0x86;ipx_address.address[13]=0x9c;memset(ipx_address.guard,0x5a,8);
    CHECK(!bind(ipx,ipx_address.address,14));address_length=14;
    CHECK(!getsockname(ipx,ipx_address.address,&address_length) && address_length==14 && ipx_address.guard[0]==0x5a);
    CHECK(sendto(ipx,"IPX",3,0,ipx_address.address,14)==3);address_length=14;
    CHECK(recvfrom(ipx,bytes,5,0,ipx_address.address,&address_length)==5 && address_length==14 && ipx_address.guard[0]==0x5a);
    struct ciuki_ipx_address_data adapter={0};n=24;CHECK(!getsockopt(ipx,NSPROTO_IPX,IPX_ADDRESS,(char *)&adapter,&n) && n==24 && adapter.status && !memcmp(adapter.nodenum,"abcdef",6));
    int ptype=7;CHECK(!setsockopt(ipx,NSPROTO_IPX,IPX_FILTERPTYPE,(char *)&ptype,4));
    read=(struct ws_set){.count=1,.socket={ipx}};descriptors[ipx_fd]->ready=POLLIN;CHECK(select(0,&read,0,0,&wait)==1);
    old_posts=posted;CHECK(!WSAAsyncSelect(ipx,(HWND)1,0x412,1));async_step();CHECK(posted==old_posts+1);
    CHECK(!closesocket(ipx));CHECK(!WSACleanup());no_descriptors();
    puts("PASS f6-25 Winsock IPX 14-byte address conversion, datagrams, adapter/ptype options, select and WSAAsyncSelect");
    puts("PASS G26 Winsock G26B async/event/DNS cancellation/FIONREAD/truncation/overlapped startup handles refs fd-reuse options select HOSTS DNS numeric synchronous vectors errors cleanup");return 0;
}
