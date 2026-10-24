/* G26 standalone host oracle; include the reviewed production facade. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <w32/ntdll.h>
#include "../../../sdk/win32/offline/wsock.c"
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"Winsock oracle line %d: %s\n",__LINE__,#c); exit(1); } } while(0)
static struct w32_teb test_teb;
_Thread_local struct w32_teb *w32_model_teb=&test_teb;
void *w32_copy(void *to,const void *from,uint32_t n) { return memcpy(to,from,n); }
void *w32_fill(void *to,uint8_t byte,uint32_t n) { return memset(to,byte,n); }
uint32_t w32_strlen(const char *s) { return (uint32_t)strlen(s); }
int w32_strcmp(const char *a,const char *b) { return strcmp(a,b); }
int w32_stricmp(const char *a,const char *b) { while(*a&&*b) { unsigned char x=(unsigned char)*a++,y=(unsigned char)*b++; if(x>='A'&&x<='Z')x+=(unsigned char)('a'-'A'); if(y>='A'&&y<='Z')y+=(unsigned char)('a'-'A'); if(x!=y)return (int)x-(int)y; } return (unsigned char)*a-(unsigned char)*b; }
void w32_diag(const char *a,const char *b,const char *c) { (void)a;(void)b;(void)c; }
static union { uint64_t alignment; unsigned char bytes[CIUKI_SOCKET_IO_MAX+128]; } allocations[4];
static unsigned allocated[4],allocation_count;
static void *model_alloc(void *opaque,uint32_t bytes) { (void)opaque; CHECK(bytes<=sizeof(allocations[0].bytes)); for(unsigned i=0;i<4;i++)if(!allocated[i]) { allocated[i]=1; allocation_count++; memset(allocations[i].bytes,0,bytes); return allocations[i].bytes; } return 0; }
static void model_free(void *opaque,void *address,uint32_t bytes) { (void)opaque;(void)bytes; for(unsigned i=0;i<4;i++)if(address==allocations[i].bytes) { CHECK(allocated[i]); allocated[i]=0; allocation_count--; return; } CHECK(0); }
static const struct w32_services services={.alloc=model_alloc,.free=model_free};
const struct w32_services *w32_get_services(void) { return &services; }
/* No PIE keeps static test data/allocator addresses representable on the wire.
 * Ordinary-stack pointer fields recover their upper bits from the record.
 * ASan's optional fake stack must be disabled, while leak detection stays on. */
static void *field_pointer(uint32_t low,uintptr_t record) { if(low<0x10000000U)return (void *)(uintptr_t)low; return (void *)((record&~(uintptr_t)UINT32_MAX)|low); }
struct object { int used,type,references,flags,ready; struct ciuki_socket_timeval timeout; uint32_t position; };
static struct object objects[32];
static struct object *descriptors[128];
static int failure_op=-1,failure_error;
static unsigned native_calls,created,closed,dns_queries,hosts_opens,sends;
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
    if(op==CIUKI_SYS_SOCKET) { CHECK(a==AF_INET); int fd=create((int)b); created++; last_native_fd=fd; return fd; }
    if(op==CIUKI_SYS_OPEN) { CHECK(!strcmp((const char *)a,"/system/settings/HOSTS")); CHECK(b==(O_RDONLY|O_CLOEXEC)); hosts_opens++; return create(0x100); }
    if(op==CIUKI_SYS_CLOSE) { if(!object)return -EBADF; descriptors[a]=0; closed++; if(!--object->references)object->used=0; return 0; }
    if(op==CIUKI_SYS_FCNTL) { if(!object)return -EBADF; if(b==F_DUPFD_CLOEXEC)return descriptor(object); if(b==F_GETFL)return object->flags; if(b==F_SETFL) { object->flags=(int)c; return 0; } return -EINVAL; }
    if(op==CIUKI_SYS_READ) { CHECK(object&&object->type==0x100); unsigned remaining=(unsigned)sizeof(hosts_text)-1-object->position; if(c>remaining)c=remaining; memcpy((void *)b,hosts_text+object->position,c); object->position+=(uint32_t)c; return (int)c; }
    if(op==CIUKI_SYS_BIND||op==CIUKI_SYS_CONNECT) { CHECK(object&&c==16&&!d); CHECK(((const struct ciuki_sockaddr_in *)b)->family==AF_INET); return 0; }
    if(op==CIUKI_SYS_LISTEN) { CHECK(object&&object->type==SOCK_STREAM); return 0; }
    if(op==CIUKI_SYS_ACCEPT) { CHECK(object); if(b) { struct ciuki_sockaddr_in addr={.family=AF_INET,.port=0x5000,.address=0x0100007f}; memcpy((void *)b,&addr,16); *(int *)c=16; } return create(SOCK_STREAM); }
    if(op==CIUKI_SYS_SHUTDOWN) { CHECK(object); return b>2?-EINVAL:0; }
    if(op==CIUKI_SYS_SOCKNAME) { CHECK(object&&c&&d); struct ciuki_sockaddr_in addr={.family=AF_INET,.port=0x5000,.address=0x0100007f}; memcpy((void *)c,&addr,16); *(int *)d=16; return 0; }
    if(op==CIUKI_SYS_SENDTO||op==CIUKI_SYS_RECVFROM) {
        struct ciuki_socket_io *io=(void *)a; CHECK(io->size==40&&io->version==1&&!io->reserved[0]&&!io->reserved[1]);
        CHECK(lookup((uintptr_t)io->fd)); CHECK(io->bytes<=CIUKI_SOCKET_IO_MAX);
        void *data=field_pointer(io->buffer,a);
        if(op==CIUKI_SYS_SENDTO) { CHECK(io->flags==MSG_NOSIGNAL); CHECK(io->bytes<=sizeof(transmitted)); memcpy(transmitted,data,io->bytes); transmitted_length=io->bytes; sends++; return (int)io->bytes; }
        unsigned n=io->bytes<5?io->bytes:5; memcpy(data,"reply",n);
        if(io->address) { struct ciuki_sockaddr_in addr={.family=AF_INET,.port=0x5000,.address=0x0100007f}; memcpy(field_pointer(io->address,a),&addr,16); *(int *)field_pointer(io->address_length,a)=16; } return (int)n;
    }
    if(op==CIUKI_SYS_SOCKOPT) {
        struct ciuki_socket_option *request=(void *)a; CHECK(request->size==40&&request->version==1); object=lookup((uintptr_t)request->fd); CHECK(object);
        last_level=request->level; last_option=request->name; void *value=field_pointer(request->value,a);
        unsigned n=request->operation?request->length:*(unsigned *)field_pointer(request->length,a);
        if(request->level==SOL_SOCKET&&(request->name==SO_SNDTIMEO||request->name==SO_RCVTIMEO)) { CHECK(n==16); if(request->operation) { memcpy(&object->timeout,value,16); last_timeout=object->timeout; } else memcpy(value,&object->timeout,16); }
        else { CHECK(n==4); if(!request->operation) { int v=request->name==SO_ERROR?ECONNREFUSED:request->name==SO_TYPE?object->type:1; memcpy(value,&v,4); } }
        return 0;
    }
    if(op==CIUKI_SYS_POLL) { struct ciuki_pollfd *fds=(void *)a; int ready=0; last_poll_timeout=(int)c; for(unsigned i=0;i<b;i++) { object=lookup((uintptr_t)fds[i].fd); CHECK(object); fds[i].revents=(int16_t)(object->ready&(fds[i].events|POLLERR|POLLHUP)); ready+=fds[i].revents!=0; } return ready; }
    if(op==CIUKI_SYS_RESOLVE4) { struct ciuki_resolve4 *request=(void *)a; CHECK(request->size==32&&request->version==1); const char *name=field_pointer(request->name,a); dns_queries++; if(!strcmp(name,"remote.example")) { request->address=0x04030201; request->status=0; } else request->status=1; return 0; }
    CHECK(0); return -ENOSYS;
}
static void no_descriptors(void) { for(unsigned i=0;i<128;i++)CHECK(!descriptors[i]); }
static struct ws_data data;
static struct ciuki_sockaddr_in address={.family=AF_INET,.port=0x5000,.address=0x0100007f};
static char bytes[32],left[8],right[8]; static int address_length; static ULONG flags,done;
int main(void) {
    setvbuf(stdout,NULL,_IOLBF,0);
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
    CHECK(closesocket(a)==0&&!descriptors[original]); uintptr_t b=socket(AF_INET,SOCK_DGRAM,0); CHECK(b!=a&&last_native_fd==original);
    CHECK(send(a,"hello",5,0)==-1&&WSAGetLastError()==10038); CHECK(closesocket(a)==-1&&WSAGetLastError()==10038);
    address_length=16; CHECK(recvfrom(b,bytes,32,0,&address,&address_length)==5&&address_length==16&&!memcmp(bytes,"reply",5));
    struct ws_buffer vector[2]={{2,left},{3,right}}; memcpy(left,"ab",2);memcpy(right,"cde",3);done=0;
    CHECK(WSASend(b,vector,2,&done,0,0,0)==0&&done==5&&!memcmp(transmitted,"abcde",5));
    memset(left,0,sizeof(left));memset(right,0,sizeof(right));flags=0;done=0;CHECK(WSARecv(b,vector,2,&done,&flags,0,0)==0&&done==5&&!memcmp(left,"re",2)&&!memcmp(right,"ply",3));
    done=99; CHECK(WSASend(b,vector,2,&done,0,(void *)1,0)==-1&&WSAGetLastError()==10045&&done==99);
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
    CHECK(WSACleanup()==0);no_descriptors();CHECK(send(b,"x",1,0)==-1&&WSAGetLastError()==10093&&test_teb.LastErrorValue==77&&allocation_count==0);
    for(unsigned i=0;i<64;i++)CHECK(!threads[i].owner);
    CHECK(WSAStartup(0x202,&data)==0); uintptr_t c=socket(AF_INET,3,1);CHECK(c!=WS_INVALID);CHECK(closesocket(c)==0);CHECK(WSACleanup()==0);no_descriptors();
    for(unsigned i=0;i<128;i++) { CHECK(WSAStartup(0x202,&data)==0);CHECK(inet_ntoa(0x0100007f)!=0);CHECK(WSACleanup()==0); }
    CHECK(created==3&&sends==2&&native_calls>30&&closed>created);
    puts("PASS G26 Winsock startup handles refs fd-reuse options select HOSTS DNS numeric synchronous vectors errors cleanup");return 0;
}
