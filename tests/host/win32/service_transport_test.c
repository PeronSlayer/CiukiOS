/* Production ntdll CSC1 client and SYSTEM broker. Only syscalls/storage are faked. */
#include <w32/ntdll.h>
#include <ciuki/abi.h>
#include "../../../apps/registry/broker.h"
#include "../../../sdk/libreg/internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
_Thread_local struct w32_teb *w32_model_teb;
static struct wr_server server;
static struct rb_broker broker;
static struct ciuki_message responses[44];
static uint8_t input[RB_BODY_MAX];
static unsigned locked,requests,connections,slot,refs,response_count,response_at,received;
static int live,lose_reply,refuse;
static uint32_t fd=7,generation;
static struct wr_io disk_io;
static const char *leaf(const char *n){const char *p=strrchr(n,'/');return p?p+1:n;}
static int mapped_read(void *p,const char *n,uint8_t **out,size_t *bytes){(void)p;return disk_io.read(disk_io.opaque,leaf(n),out,bytes);}
static int mapped_write(void *p,const char *n,const void *v,size_t bytes){(void)p;return disk_io.write(disk_io.opaque,leaf(n),v,bytes);}
static int mapped_patch(void *p,const char *n,size_t at,const void *v,size_t bytes){(void)p;return disk_io.patch(disk_io.opaque,leaf(n),at,v,bytes);}
static int mapped_sync(void *p,const char *n){(void)p;return disk_io.sync(disk_io.opaque,leaf(n));}
static void mapped_log(void *p,const char *n){(void)p;disk_io.log(disk_io.opaque,n);}

static uint64_t request_id;
static unsigned operation,total;
NTSTATUS W32API RtlEnterCriticalSection(RTL_CRITICAL_SECTION *lock){assert(lock&&!locked);locked=1;return 0;}
NTSTATUS W32API RtlLeaveCriticalSection(RTL_CRITICAL_SECTION *lock){assert(lock&&locked);locked=0;return 0;}
void *W32API RtlAllocateHeap(void *heap,ULONG flags,uint32_t n){(void)heap;(void)flags;return malloc(n);}
BOOLEAN W32API RtlFreeHeap(void *heap,ULONG flags,void *p){(void)heap;(void)flags;free(p);return 1;}
struct rt *reg_hardware(uint64_t previous){(void)previous;return NULL;}
static int role(void *p){(void)p;return 0;}
static int retain(void *p,uintptr_t lease){(void)p;assert(lease==1);refs++;return 0;}
static void release(void *p,uintptr_t lease){(void)p;assert(lease==1&&refs);refs--;}
static int inspect(void *p,uintptr_t lease,struct rb_identity *out){(void)p;assert(lease==1);if(!live)return -3;*out=(struct rb_identity){.uid=1000,.session=1,.groups={544,545},.group_count=2,.process_generation=1};return 0;}
static int read_root(void *p,unsigned root,char **text,size_t *n){(void)p;(void)root;(void)text;(void)n;return -5;}
static int write_root(void *p,unsigned root,const char *text,size_t n,uint64_t expected){(void)p;(void)root;(void)text;(void)n;(void)expected;return -5;}
int32_t w32r_host_security(struct ciuki_security_args *a,const struct ciuki_security_link *in,struct ciuki_security_link *out)
{
    assert(locked&&a->operation==CIUKI_SECURITY_SERVICE_CONNECT&&in->service==4);
    live=1;connections++;assert(!rb_connect(&broker,1,&slot));generation++;
    *out=(struct ciuki_security_link){.version=1,.size=64,.service=4,.endpoint_fd=(int32_t)fd,
        .endpoint_generation=generation,.peer_pid=99,.server_role=CIUKI_SECURITY_ROLE_REGISTRY};return 0;
}
uint32_t w32r_host_gate(uint32_t op,uintptr_t a,uintptr_t b,uintptr_t c)
{
    assert(locked);(void)c;assert(a==fd);
    if(op==CIUKI_SYS_CLOSE){rb_disconnect(&broker,slot);response_at=response_count=received=0;return 0;}
    if(!live)return (uint32_t)-32;
    struct ciuki_message *m=(void *)b;
    if(op==CIUKI_SYS_CHANNEL_SEND){
        assert(m->length>=64&&m->length<=256&&!m->fd_count);const uint8_t *h=m->data;
        unsigned offset=rb_get32(h+56),bytes=rb_get32(h+36);
        if(!offset){received=0;operation=rb_get32(h+16);request_id=rb_get64(h+24);total=rb_get32(h+52);}
        assert(total<=RB_BODY_MAX&&offset==received&&bytes<=total-received);
        memcpy(input+received,h+64,bytes);received+=bytes;
        if(received==total){
            uint8_t reply[RB_BODY_MAX];size_t used=0;
            if(operation==RB_WIN32_COMMIT)requests++;
            int failure=refuse;refuse=0;
            int e=failure?(failure==2?-5:-3):rb_exchange(&broker,slot,operation,input,total,reply,sizeof(reply),&used);
            if(e)used=0;response_at=response_count=0;unsigned at=0;
            do{
                struct ciuki_message *r=responses+response_count++;memset(r,0,sizeof(*r));r->sender_pid=99;
                unsigned take=(unsigned)used-at;if(take>192)take=192;
                memcpy(r->data,h,64);rb_put32(r->data+20,2);rb_put64(r->data+24,request_id);
                rb_put32(r->data+36,take);rb_put32(r->data+48,(uint32_t)e);rb_put32(r->data+52,(uint32_t)used);rb_put32(r->data+56,at);
                if(take)memcpy(r->data+64,reply+at,take);r->length=64+take;at+=take;
            }while(at<used);
        }
        return 0;
    }
    assert(op==CIUKI_SYS_CHANNEL_RECV&&response_at<response_count);
    if(lose_reply&&operation==RB_WIN32_READ){lose_reply=0;return 0;}
    *m=responses[response_at++];return 1;
}
static uint32_t request(void)
{
    struct {struct w32r_request r;char empty[2];} a={.r={.version=1,.op=W32R_OPEN,
        .key=W32R_HKLM,.access=0x20019,.path_bytes=1,.name_bytes=1}};
    struct w32r_reply reply;uint32_t used=99;uint32_t e=w32_server_exchange(&a,sizeof(a.r)+2,&reply,sizeof(reply),&used);
    if(!e)assert(used==sizeof(reply)&&!reply.error&&reply.version==1);else assert(!used);return e;
}
int main(int argc,char **argv)
{
    setbuf(stdout,NULL);assert(argc==2);
    RTL_CRITICAL_SECTION lock={0};struct w32_peb peb={.FastPebLock=&lock};struct w32_teb teb={.ProcessEnvironmentBlock=&peb};w32_model_teb=&teb;
    struct wr_io io;wr_posix_io(&io,argv[1]);assert(wr_init(&server,&io,0));disk_io=io;
    server.io=(struct wr_io){NULL,mapped_read,mapped_write,mapped_patch,mapped_sync,mapped_log};server.secured=1;server.subject.uid=1000;server.subject.session=1;
    struct rb_authority authority={NULL,role,retain,release,inspect};struct rb_storage storage={.read=read_root,.write=write_root};
    assert(!rb_init(&broker,&authority,&storage,&server));
    for(unsigned i=0;i<12;i++)assert(!request());assert(requests==12&&connections==1&&!locked);
    size_t length=sizeof(struct w32r_request)+6+65536;uint8_t *wire=calloc(1,length),*out=malloc(65576);assert(wire&&out);
    struct w32r_request large={.version=1,.op=W32R_SET,.key=W32R_HKLM,.type=3,.path_bytes=1,.name_bytes=5,.data_bytes=65536};
    memcpy(wire,&large,sizeof(large));memcpy(wire+41,"Long",5);memset(wire+46,0x59,65536);uint32_t used=0;
    assert(!w32_server_exchange(wire,(uint32_t)length,out,65576,&used)&&used==40&&!((struct w32r_reply *)out)->error);
    large.op=W32R_QUERY;large.data_bytes=0;memcpy(wire,&large,sizeof(large));
    assert(!w32_server_exchange(wire,46,out,65576,&used)&&used==65576&&!((struct w32r_reply *)out)->error);
    for(unsigned i=40;i<65576;i++)assert(out[i]==0x59);free(wire);free(out);
    live=0;unsigned before=requests;assert(request()==1062&&requests==before);assert(!request()&&connections==2);
    before=requests;lose_reply=1;assert(request()==1062&&requests==before+1);assert(!request()&&connections==3);
    refuse=1;before=requests;assert(request()==1062&&requests==before);assert(!request()&&connections==4);
    refuse=2;before=requests;assert(request()==1016&&requests==before);assert(!request()&&connections==5);
    rb_destroy(&broker);wr_destroy(&server);assert(!refs);
    puts("PASS direct REGISTRY transport: endpoint generations, 64KiB streaming, revoked link, no lost-reply replay, whole-transaction lock");return 0;
}
