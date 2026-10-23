/* Reuse the synthetic resident-memory/ledger backend, not vendor code. */
#define main nt_fixture_main
#include "nt_runtime_test.c"
#undef main
#include "src/kernel/ndis/ndis_services.h"
#include <stddef.h>
#include "tests/host/ndis/census_imports.h"
static struct ndis_miniport mp;
static struct ndis_services ns;
static struct ndis_channel channel;
static struct ndis_channel_shared shared;
static struct { uint32_t token,fn,args[6],value;bool ready,live; } cq[32];
static struct ndis_chars32 chars;
static unsigned completed,returned,records,irqs,transferred;
static bool deserialize,pending_oid,reset_async,inline_complete,carrier;
static uint32_t send_status,rx_packet;
static unsigned device_refusals;
static char refused_driver[32];
static void mp_refuse(void *ctx,const char *driver) { (void)ctx;device_refusals++;snprintf(refused_driver,sizeof(refused_driver),"%s",driver); }
enum { ENTRY=0,INIT=16,HALT=32,QUERY=48,SET=64,RESET=80,SEND=96,RETURN=112,HANG=128,TRANSFER=144,ISR=160,DPC=176,PNP=192,SHUTDOWN=208,SYNC=224 };
static uint32_t svc(unsigned op,const uint32_t *a,unsigned count)
{ memcpy(memory+0xe000,a,count*4);return ndis_service(&ns,op,0x20e000); }
static int mp_queue(void *ctx,enum ndis_host_kind kind,uint32_t fn,unsigned argc,const uint32_t *args,uint32_t *token)
{
    (void)ctx;assert(argc<=6);unsigned i=0;while(i<32 && cq[i].live) i++;assert(i<32);
    if(!ndis_host_enqueue(&h,kind,fn,0,now/10,token)) return 0;
    cq[i].token=*token;cq[i].fn=fn;if(argc) memcpy(cq[i].args,args,argc*4);cq[i].ready=false;cq[i].live=true;return 1;
}
static int mp_result(void *ctx,uint32_t token,uint32_t *value)
{
    (void)ctx;
    for(unsigned i=0;i<32;i++) if(cq[i].live && cq[i].ready && cq[i].token==token) {
        *value=cq[i].value;cq[i].live=false;return 1;
    }
    return 0;
}
static int mp_irq(void *ctx,bool install,uint32_t isr,uint32_t dpc)
{ (void)ctx;if(install) { assert(isr==0x100000+ISR && dpc==0x100000+DPC);irqs++; }else irqs--;return 1; }
static void mp_link(void *ctx,bool on) { (void)ctx;carrier=on; }
static void mp_record(void *ctx,const char *phase) { (void)ctx;assert(phase);records++; }
static void done(void *ctx,uint32_t token,int status) { (void)ctx;assert(token);assert(status==0 || status==-1);completed++; }
static unsigned fake_pci(void *ctx,bool write,unsigned off,void *data,unsigned n)
{ (void)ctx;(void)write;(void)off;memset(data,0,n);return n; }
static uint32_t fake_port(void *ctx,unsigned base,unsigned bytes,bool release)
{ (void)ctx;(void)release;return base==0x3000 && bytes==32?base:0; }
static uint32_t fake_mmio(void *ctx,uint32_t address,unsigned bytes,bool release)
{ (void)ctx;(void)address;(void)bytes;(void)release;return 0; }
static unsigned fake_resources(void *ctx,void *data,unsigned n)
{ (void)ctx;assert(n>=8);memset(data,0,8);return 8; }
static void fake_log(void *ctx,uint32_t code,const uint32_t *data,unsigned n)
{ (void)ctx;(void)code;(void)data;(void)n; }
static uint32_t query_oid(const uint32_t *a)
{
    uint32_t value=0,bytes=4;uint8_t data[256]={0};
    static const uint32_t supported[]={0x10101,0x10102,0x10103,0x10104,0x10105,0x10106,0x10107,0x10111,
        0x10113,0x10114,0x10115,0x1010e,0x1010f,0x1010c,0x1010d,0x10110,0x01010101,0x01010102,0x01010103,0x01010104};
    switch(a[1]) {
    case 0x10101:bytes=sizeof(supported);memcpy(data,supported,bytes);break;
    case 0x10105:case 0x10106:value=1500;break;
    case 0x10111:value=1514;break;
    case 0x10107:value=1000000;break;
    case 0x01010104:value=32;break;
    case 0x10115:value=16;break;
    case 0x1010c:value=0x8086;break;
    case 0x1010d:bytes=5;memcpy(data,"test",5);break;
    case 0x10110:bytes=2;data[1]=5;break;
    case 0x01010101:case 0x01010102:bytes=6;data[0]=2;data[5]=7;break;
    default:break;
    }
    if(bytes==4) memcpy(data,&value,4);
    assert(bytes<=a[3] && wr(0,a[2],data,bytes) && wr(0,a[4],&bytes,4));
    if(pending_oid) { assert(ndis_mp_oid_complete(&mp,false,NDIS_SUCCESS));return NDIS_PENDING; }
    return NDIS_SUCCESS;
}
static bool run_callback(void)
{
    struct ndis_host_event e;if(!ndis_host_take(&h,now/10,&e)) return false;
    unsigned i=0;while(i<32 && (!cq[i].live || cq[i].token!=e.token)) i++;assert(i<32);
    uint32_t *a=cq[i].args,result=0;
    switch(cq[i].fn-0x100000) {
    case ENTRY: {
        uint32_t ca=ndis_nt_pool(&r,0,sizeof(chars),1);assert(ca && wr(0,ca,&chars,sizeof(chars)));
        uint32_t v[4]={mp.scratch+64,0x20d000,0x20d100,0};assert(!svc(NDIS_OP_NdisInitializeWrapper,v,4));
        uint32_t reg[3]={mp.wrapper,ca,sizeof(chars)};assert(!svc(NDIS_OP_NdisMRegisterMiniport,reg,3));break;
    }
    case INIT: {
        uint32_t medium;assert(a[3]==1 && a[4]==mp.adapter && rd(0,a[2],&medium,4) && medium==0);
        uint32_t selected=0;assert(wr(0,a[1],&selected,4));
        uint32_t context=ndis_nt_pool(&r,0,64,1);
        uint32_t v[5]={mp.adapter,context,3,NDIS_BUS_MASTER|(deserialize?NDIS_DESERIALIZE:0),5};
        assert(!svc(NDIS_OP_NdisMSetAttributesEx,v,5) && mp.hang_seconds==2);
        /* A miniport that treats its optional control device as optional continues initialization. */
        uint32_t dev[6]={mp.wrapper,0x20d000,0x20d100,0x20d200,0x20d300,0x20d400},unchanged=0xa5a5a5a5;
        assert(wr(0,0x20d400,&unchanged,4));
        assert(svc(NDIS_OP_NdisMRegisterDevice,dev,6)==NDIS_NOT_SUPPORTED && mp.state!=NDIS_MP_FAILED);
        assert(rd(0,0x20d400,&unchanged,4) && unchanged==0xa5a5a5a5); /* no handle was produced */
        uint32_t irq[7]={context,mp.adapter,11,11,1,1,0};assert(!svc(NDIS_OP_NdisMRegisterInterrupt,irq,7));break;
    }
    case QUERY:result=query_oid(a);break;
    case SET:assert(wr(0,a[4],a+3,4));break;
    case SEND: {
        uint32_t packet;assert(a[2]==1 && rd(0,a[1],&packet,4));
        assert(wr(0,packet+100,&send_status,4));
        if(inline_complete) assert(ndis_mp_send_complete(&mp,packet,NDIS_SUCCESS));break;
    }
    case RETURN:assert(a[1]==rx_packet);returned++;break;
    case RESET:if(reset_async) result=NDIS_PENDING;else { uint32_t one=1;assert(wr(0,a[0],&one,4)); }break;
    case HALT:break;
    case TRANSFER: {
        struct ndis_packet32 p;struct ndis_nt_mdl32 b;
        assert(rd(0,a[0],&p,56) && rd(0,p.head,&b,sizeof(b)));
        uint8_t frame[64]={0};assert(a[5]<=sizeof(frame));memset(frame,0x5a,a[5]);
        assert(wr(0,b.mapped,frame,a[5]) && wr(0,a[1],a+5,4));transferred++;break;
    }
    case SYNC:assert(h.synchronized && h.irql==5);result=1;break;
    case HANG:case PNP:case SHUTDOWN:case ISR:case DPC:break;
    default:assert(0);
    }
    assert(ndis_host_complete(&h,7,e.token,now/10));cq[i].value=result;cq[i].ready=true;return true;
}
static void tick(void) { ndis_mp_poll(&mp);(void)run_callback();ndis_mp_poll(&mp); }
static void until_running(void)
{ for(unsigned i=0;i<200 && mp.state!=NDIS_MP_RUNNING;i++) tick();assert(mp.state==NDIS_MP_RUNNING && carrier); }
static void mp_setup(bool deserialized)
{
    setup();h.depth=h.count=0;h.irql=0;
    memset(cq,0,sizeof(cq));memset(&ns,0,sizeof(ns));completed=returned=records=irqs=transferred=0;
    deserialize=deserialized;pending_oid=true;reset_async=inline_complete=false;send_status=NDIS_PENDING;
    struct ndis_mp_ops ops={0,mp_queue,mp_result,mp_irq,mp_link,mp_record};
    struct ndis_service_ops so={0,fake_pci,fake_port,fake_mmio,fake_resources,fake_log,mp_refuse};
    assert(ndis_channel_init(&channel,&shared,7,0,simple,simple,done));
    assert(ndis_mp_init(&mp,&r,&channel,&ops));mp.raw_irq=11;
    assert(ndis_services_init(&ns,&mp,&so,0x500000));
    chars=(struct ndis_chars32){.major=5,.minor=1,.initialize=0x100000+INIT,.halt=0x100000+HALT,
        .query=0x100000+QUERY,.set=0x100000+SET,.reset=0x100000+RESET,.send_packets=0x100000+SEND,
        .return_packet=0x100000+RETURN,.transfer=0x100000+TRANSFER,.isr=0x100000+ISR,.handle_irq=0x100000+DPC,
        .pnp=0x100000+PNP,.shutdown=0x100000+SHUTDOWN};
    assert(ndis_mp_start(&mp,0x20d000,0x20d100));until_running();assert(irqs==1 && records>=4);
    struct ndis_block32 block;assert(rd(0,mp.adapter,&block,sizeof(block)));
    assert(block.send_complete==0x500000+NDIS_OP_NdisMSendComplete*32 && block.adapter_context==mp.context);
}
static void rx(void)
{
    uint32_t data=ndis_nt_pool(&r,0,60,1),mdl=ndis_nt_mdl(&r,data,60);
    uint8_t frame[60]={2};frame[12]=8;assert(wr(0,data,frame,60));
    rx_packet=ndis_mp_packet_alloc(&mp,0,16);
    struct ndis_packet32 p={.head=mdl,.tail=mdl,.oob=72,.packet_flags=0xc0,.status=NDIS_SUCCESS};
    assert(wr(0,rx_packet,&p,sizeof(p)) && ndis_mp_receive(&mp,&rx_packet,1));
    uint8_t out[NDIS_RING_FRAME];unsigned length;
    assert(ndis_channel_receive(&channel,out,&length)==1 && length==60 && !memcmp(out,frame,60));
    if(deserialize) { assert(!ndis_mp_receive(&mp,&rx_packet,1));tick();assert(returned==1); }
    else assert(returned==0);
    uint32_t resources=NDIS_RESOURCES;assert(wr(0,rx_packet+100,&resources,4));
    assert(ndis_mp_receive(&mp,&rx_packet,1));tick();assert(returned==(unsigned)deserialize);
    struct ndis_nt_mdl32 b;assert(rd(0,mdl,&b,sizeof(b)));b.next=mdl;assert(wr(0,mdl,&b,sizeof(b)));
    assert(!ndis_mp_receive(&mp,&rx_packet,1)); /* cycle refused before any copy */
}
static void census_resolution(void)
{
    size_t total;const struct ndis_export *table=ndis_exports(&total);
    const char *const (*sets[3])[2]={census_e1000325,census_atnicm51,census_e100b325};
    const size_t counts[3]={sizeof(census_e1000325)/sizeof(census_e1000325[0]),
        sizeof(census_atnicm51)/sizeof(census_atnicm51[0]),sizeof(census_e100b325)/sizeof(census_e100b325[0])};
    assert(counts[0]==80 && counts[1]==60 && counts[2]==76);
    for(unsigned d=0;d<3;d++) for(size_t i=0;i<counts[d];i++) {
        const struct ndis_export *e=ndis_export_find(table,total,sets[d][i][0],sets[d][i][1],0);
        if(!e || !e->implemented) fprintf(stderr,"unresolved %u %s!%s\n",d,sets[d][i][0],sets[d][i][1]);
        assert(e && e->implemented);
    }
    /* Exact NDIS 5.1 stdcall stack bytes (Microsoft DDK pages cited in the design note). */
    static const struct { const char *name;unsigned bytes; } refused[]={
        {"NdisMRegisterDevice",24},{"NdisMDeregisterDevice",4},{"NdisMSetMiniportSecondary",8},{"NdisMPromoteMiniport",4}};
    for(unsigned i=0;i<4;i++) {
        const struct ndis_export *e=ndis_export_find(table,total,"NDIS.SYS",refused[i].name,0);
        assert(e && e->implemented && e->stack_bytes==refused[i].bytes && e->abi==NDIS_ABI_STDCALL);
    }
}
static void refusal_paths(void)
{
    mp_setup(false);device_refusals=0;refused_driver[0]=0;
    uint32_t dev[6]={mp.wrapper,0x20d000,0x20d100,0x20d200,0x20d300,0x20d400};
    assert(svc(NDIS_OP_NdisMRegisterDevice,dev,6)==NDIS_NOT_SUPPORTED && device_refusals==1 && refused_driver[0]);
    uint32_t handle[1]={0};
    assert(svc(NDIS_OP_NdisMDeregisterDevice,handle,1)==NDIS_FAILURE);
    handle[0]=0x20d400;assert(svc(NDIS_OP_NdisMDeregisterDevice,handle,1)==NDIS_FAILURE);
    handle[0]=mp.adapter;assert(svc(NDIS_OP_NdisMDeregisterDevice,handle,1)==NDIS_FAILURE);
    uint32_t sec[2]={mp.adapter,mp.adapter};
    assert(svc(NDIS_OP_NdisMSetMiniportSecondary,sec,2)==NDIS_NOT_SUPPORTED);
    handle[0]=mp.adapter;assert(svc(NDIS_OP_NdisMPromoteMiniport,handle,1)==NDIS_NOT_SUPPORTED);
    assert(mp.state==NDIS_MP_RUNNING && device_refusals==1); /* refusals never fault or succeed */
}
int main(void)
{
    census_resolution();refusal_paths();
    mp_setup(false);uint8_t frame[60]={0};frame[12]=8;uint32_t token;
    assert(ndis_channel_send(&channel,frame,14,&token)==1);send_status=NDIS_RESOURCES;tick();assert(mp.tx_blocked && completed==0);
    uint32_t token2;assert(ndis_channel_send(&channel,frame,60,&token2)==1);tick();assert(mp.tx_consumed==1);
    assert(ndis_mp_send_resources(&mp));send_status=NDIS_SUCCESS;tick();assert(completed==1);tick();assert(completed==2);
    send_status=NDIS_PENDING;inline_complete=true;assert(ndis_channel_send(&channel,frame,60,&token)==1);tick();assert(completed==3);
    inline_complete=false;rx();
    assert(ndis_mp_reset(&mp));tick();until_running();
    reset_async=true;assert(ndis_mp_reset(&mp));tick();assert(mp.state==NDIS_MP_RESETTING);
    assert(ndis_mp_reset_complete(&mp,NDIS_SUCCESS,false));tick();assert(mp.state==NDIS_MP_RUNNING);
    assert(ndis_mp_halt(&mp));tick();assert(mp.state==NDIS_MP_STOPPED);
    mp_setup(true);send_status=NDIS_RESOURCES;
    assert(ndis_channel_send(&channel,frame,60,&token)==1);tick();assert(completed==0 && mp.tx[0].live && !mp.tx_blocked);
    uint32_t packet=mp.tx[0].packet;assert(ndis_mp_send_complete(&mp,packet,NDIS_SUCCESS));
    assert(!ndis_mp_send_complete(&mp,packet,NDIS_SUCCESS));tick();assert(completed==1);
    assert(!ndis_mp_send_resources(&mp));rx();
    /* TransferData is nested before the indication returns; the outer frame
     * remains parked, with its original deadline, through the user callback. */
    uint32_t active;assert(mp_queue(0,NDIS_HOST_DPC,0x100000+DPC,0,0,&active));
    struct ndis_host_event e;assert(ndis_host_take(&h,now/10,&e));
    uint32_t header=ndis_nt_pool(&r,0,14,1),look=ndis_nt_pool(&r,0,4,1);assert(wr(0,header,frame,14));
    assert(ndis_mp_lookahead(&mp,123,header,14,look,4,40));
    struct trap_frame tf={.cs=0x1b,.eip=0x100050,.eflags=0x202,.user_esp=0x20c000};
    assert(ndis_mp_park(&mp,&tf) && ndis_mp_resume(&mp,&tf)==-1);
    assert(run_callback() && ndis_mp_resume(&mp,&tf)==1 && transferred==1);tick();
    assert(!mp.transfer.live && ndis_host_complete(&h,7,active,now/10));
    assert(ndis_channel_send(&channel,frame,60,&token)==1);tick();
    ndis_mp_fault(&mp,"injected-fault");assert(h.state==NDIS_HOST_QUARANTINED && completed==2 && !channel.live);
    ndis_mp_fault(&mp,"duplicate-fault");assert(completed==2);
    puts("PASS synthetic NDIS initialize/attributes/IRQ, serialized/deserialized TX/RX, OIDs, reset, halt, TransferData and fault teardown");fflush(stdout);return 0;
}
