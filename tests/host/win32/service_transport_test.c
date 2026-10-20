/* Production ntdll packet transport and w32srv, mocked only at the gate. */
#include <w32/ntdll.h>
#include <ciuki/abi.h>
#include "../../../apps/w32srv/registry.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
_Thread_local struct w32_teb *w32_model_teb;
static struct wr_server server;
static struct wr_session session;
static struct w32r_packet response;
static unsigned locked,hellos,requests,packets,connections;
static int live,lose_reply,refuse;
static uint32_t fd=7;
static uint32_t private_fd;
NTSTATUS W32API RtlEnterCriticalSection(RTL_CRITICAL_SECTION *lock)
{assert(lock && !locked);locked=1;return 0;}
NTSTATUS W32API RtlLeaveCriticalSection(RTL_CRITICAL_SECTION *lock)
{assert(lock && locked);locked=0;return 0;}
uint32_t w32r_host_gate(uint32_t op,uintptr_t a,uintptr_t b,uintptr_t c)
{
    assert(locked);(void)c;
    if(op==CIUKI_SYS_W32){
        struct w32r_service *s=(void *)a;
        assert(s->action==2 && s->service==2);s->result=1;s->endpoint=fd;
        live=1;connections++;return 0;
    }
    if(op==CIUKI_SYS_FCNTL){assert(a==fd && b==F_DUPFD_CLOEXEC && !c && !private_fd);private_fd=fd+100;return private_fd;}
    if(op==CIUKI_SYS_CLOSE){assert(a==private_fd);private_fd=0;return 0;}
    assert(a==private_fd); /* never use the kernel-owned, reusable descriptor */
    if(!live)return (uint32_t)-32;
    struct ciuki_message *m=(void *)b;
    if(op==CIUKI_SYS_CHANNEL_SEND){
        assert(m->length==256 && !m->fd_count);packets++;
        const struct w32r_packet *p=(void *)m->data;
        if(p->kind==W32R_BEGIN){
            struct w32r_request r;memcpy(&r,p->bytes,sizeof(r));
            if(r.op==W32R_CLIENT)hellos++;else requests++;
        }
        assert(!wr_packet(&server,&session,p,&response));
        if(refuse){response.error=1062;refuse=0;}
        return 0;
    }
    assert(op==CIUKI_SYS_CHANNEL_RECV);
    if(lose_reply && response.kind==W32R_READ){lose_reply=0;return 0;}
    memset(m,0,sizeof(*m));m->length=256;memcpy(m->data,&response,256);return 1;
}
static uint32_t request(void)
{
    struct {struct w32r_request r;char empty[2];} a={.r={.version=1,.op=W32R_OPEN,
        .key=W32R_HKLM,.access=0x20019,.path_bytes=1,.name_bytes=1}};
    struct w32r_reply reply;uint32_t used=99;
    uint32_t e=w32_server_exchange(&a,sizeof(a.r)+2,&reply,sizeof(reply),&used);
    if(!e)assert(used==sizeof(reply) && !reply.error && reply.version==1);
    else assert(!used);
    return e;
}
static int alive(void *o,uint32_t pid){(void)o;return pid==10;}
int main(int argc,char **argv)
{
    setbuf(stdout,NULL);
    assert(argc==2);
    RTL_CRITICAL_SECTION lock={0};struct w32_peb peb={.FastPebLock=&lock};
    struct w32_teb teb={.ProcessEnvironmentBlock=&peb};w32_model_teb=&teb;
    struct wr_io io;wr_posix_io(&io,argv[1]);assert(wr_init(&server,&io,0));session.pid=10;
    for(unsigned i=0;i<12;i++)assert(!request());
    assert(hellos==1 && requests==12 && connections==1 && packets==26 && !locked);
    /* Exercise WRITE fragments and multiple READs under the same lock. */
    uint8_t input[646]={0},output[640];
    struct w32r_request large={.version=1,.op=W32R_SET,.key=W32R_HKLM,
        .type=3,.path_bytes=1,.name_bytes=5,.data_bytes=600};
    memcpy(input,&large,sizeof(large));memcpy(input+41,"Long",5);memset(input+46,0x59,600);
    uint32_t used=0;
    assert(!w32_server_exchange(input,sizeof(input),output,sizeof(output),&used));
    assert(used==40 && !((struct w32r_reply *)output)->error && hellos==1);
    large.op=W32R_QUERY;large.data_bytes=0;memcpy(input,&large,sizeof(large));
    assert(!w32_server_exchange(input,46,output,sizeof(output),&used));
    assert(used==640 && !((struct w32r_reply *)output)->error && hellos==1);
    for(unsigned i=40;i<640;i++)assert(output[i]==0x59);
    /* A real destroy/load/reconcile and a reused numeric FD. */
    wr_session_close(&session);wr_destroy(&server);assert(wr_init(&server,&io,0));
    assert(!wr_reconcile(&server,alive,NULL));session.pid=10;live=0;
    assert(!request() && hellos==2 && requests==15 && connections==2);
    /* Endpoint change and a failed send are likewise one new connection. */
    wr_session_close(&session);session.pid=10;live=0;fd=19;
    assert(!request() && hellos==3 && connections==3);
    /* Lost replies may follow mutations: no automatic replay. */
    unsigned before=requests;lose_reply=1;
    assert(request()==1062 && requests==before+1);
    assert(!request() && hellos==4 && connections==4);
    refuse=1;before=requests;
    assert(request()==1062 && requests==before+1);
    assert(!request() && hellos==5 && connections==5);
    wr_session_close(&session);wr_destroy(&server);
    puts("PASS transport N requests one CLIENT, FD reuse/restart, endpoint change, refusal, no lost-reply replay, whole-transaction lock");
    return 0;
}
