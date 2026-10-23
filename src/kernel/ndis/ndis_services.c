/* NDIS 5 scalar services. No catch-all exports and no kernel pointers in the
 * driver ABI. Unsupported private/WDM entry points remain load errors.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "ndis_services.h"
#include <ciuki/kernel.h>
#include <stddef.h>
static const struct { const char *name;unsigned argc,bytes;bool implemented; } specs[]={
#define ND(name,args,bytes,implemented) {#name,args,bytes,implemented},
#include "ndis_exports.def"
#undef ND
};
static bool live(struct ndis_services *s)
{ return s && s->miniport && ndis_host_live(s->miniport->nt->host); }
static uint32_t fault(struct ndis_services *s,const char *why)
{ if(s && s->miniport) ndis_mp_fault(s->miniport,why);return NDIS_FAILURE; }
static int rd(struct ndis_services *s,uint32_t at,void *out,unsigned bytes)
{
    struct ndis_nt_runtime *r=s->miniport->nt;
    if(!live(s) || at<0x10000 || at>=0xc0000000 || bytes>0xc0000000-at ||
       !r->ops.read(r->ops.context,at,out,bytes)) { fault(s,"ndis-service-read");return 0; }
    return 1;
}
static int wr(struct ndis_services *s,uint32_t at,const void *in,unsigned bytes)
{
    struct ndis_nt_runtime *r=s->miniport->nt;
    if(!live(s) || at<0x10000 || at>=0xc0000000 || bytes>0xc0000000-at ||
       !r->ops.write(r->ops.context,at,in,bytes)) { fault(s,"ndis-service-write");return 0; }
    return 1;
}
static uint32_t output(struct ndis_services *s,uint32_t at,uint32_t value)
{ return wr(s,at,&value,4)?0:NDIS_FAILURE; }
static uint32_t pool_alloc(struct ndis_services *s,unsigned bytes)
{ return ndis_nt_pool(s->miniport->nt,0,bytes,0x7369644e); }
static int pool_index(struct ndis_services *s,uint32_t handle,bool packet)
{ for(unsigned i=0;i<16;i++) if(s->pool[i].live && s->pool[i].handle==handle && s->pool[i].packet==packet) return (int)i;return -1; }
static int object_index(struct ndis_services *s,uint32_t address,bool packet)
{ for(unsigned i=0;i<64;i++) if(s->object[i].live && s->object[i].address==address && s->object[i].packet==packet) return (int)i;return -1; }
int ndis_veneers(void *memory,uint32_t bytes,uint32_t base,struct ndis_user_export *out,unsigned capacity,unsigned *count)
{
    if(!memory || !out || !count || capacity<NDIS_SERVICE_COUNT || bytes<NDIS_SERVICE_COUNT*32 ||
       base<0x10000 || base>=0xc0000000 || bytes>0xc0000000-base) return 0;
    size_t total;const struct ndis_export *table=ndis_exports(&total);*count=0;
    for(unsigned i=0;i<NDIS_SERVICE_COUNT;i++) {
        if(!specs[i].implemented) continue;
        const struct ndis_export *e=ndis_export_find(table,total,"NDIS.SYS",specs[i].name,0);
        if(!e || !e->implemented || e->stack_bytes!=specs[i].bytes) return 0;
        uint8_t *p=(uint8_t *)memory+i*32;
        /* push ebx; lea ecx,[esp+8]; mov ebx,op; mov eax,NDIS_USER_NDIS;
         * int 80; pop ebx; ret exact stack bytes (or caller-cleaned cdecl). */
        p[0]=0x53;p[1]=0x8d;p[2]=0x4c;p[3]=0x24;p[4]=8;p[5]=0xbb;pe_put32(p+6,i);
        p[10]=0xb8;pe_put32(p+11,0x4e44000a);p[15]=0xcd;p[16]=0x80;p[17]=0x5b;
        p[18]=specs[i].bytes?0xc2:0xc3;
        if(specs[i].bytes) pe_put16(p+19,(uint16_t)specs[i].bytes);
        out[(*count)++]=(struct ndis_user_export){e,base+i*32,specs[i].bytes?21u:19u};
    }
    return 1;
}
static uint32_t veneer(struct ndis_services *s,enum ndis_service_operation op)
{ return s->veneer_base+(uint32_t)op*32; }
int ndis_services_init(struct ndis_services *s,struct ndis_miniport *m,const struct ndis_service_ops *ops,uint32_t base)
{
    if(!s || !m || !ops || !ops->pci || !ops->port || !ops->mmio || !ops->resources || !ops->log ||
       base<0x10000 || base>0xc0000000-NDIS_SERVICE_COUNT*32) return 0;
    *s=(struct ndis_services){.miniport=m,.ops=*ops,.veneer_base=base};
    struct ndis_block32 block;
    if(!rd(s,m->adapter,&block,sizeof(block))) return 0;
#define SLOT(field,op) block.field=veneer(s,NDIS_OP_##op)
    SLOT(indicate_packet,NdisMIndicateReceivePacket);SLOT(send_complete,NdisMSendComplete);
    SLOT(send_resources,NdisMSendResourcesAvailable);SLOT(reset_complete,NdisMResetComplete);
    SLOT(query_complete,NdisMQueryInformationComplete);SLOT(set_complete,NdisMSetInformationComplete);
    SLOT(indicate_status,NdisMIndicateStatus);SLOT(status_complete,NdisMIndicateStatusComplete);
    SLOT(transfer_complete,NdisMTransferDataComplete);SLOT(eth_receive,CiukiEthReceive);SLOT(eth_complete,CiukiEthReceiveComplete);
#undef SLOT
    if(!wr(s,m->adapter,&block,sizeof(block))) return 0;
    uint8_t resources[128];unsigned count=ops->resources(ops->context,resources,sizeof(resources));
    if(!count || count>sizeof(resources)) return 0;
    s->resource_list=pool_alloc(s,count);
    return s->resource_list && wr(s,s->resource_list,resources,count);
}
static uint32_t pool_create(struct ndis_services *s,const uint32_t *a,bool packet)
{
    if(!a[2] || a[2]>64 || (packet && a[3]>128)) { output(s,a[0],NDIS_RESOURCES);return 0; }
    unsigned i=0;while(i<16 && s->pool[i].live) i++;
    uint32_t handle=i<16?pool_alloc(s,8):0;
    if(handle) {
        s->pool[i].handle=handle;s->pool[i].maximum=a[2];s->pool[i].protocol=packet?a[3]:0;
        s->pool[i].packet=packet;s->pool[i].used=0;s->pool[i].live=true;
    }
    output(s,a[1],handle);output(s,a[0],handle?NDIS_SUCCESS:NDIS_RESOURCES);return 0;
}
static uint32_t object_create(struct ndis_services *s,const uint32_t *a,bool packet)
{
    int p=pool_index(s,a[2],packet);unsigned i=0;while(i<64 && s->object[i].live) i++;
    uint32_t at=0;
    if(p>=0 && i<64 && s->pool[p].used<s->pool[p].maximum) {
        at=packet?ndis_mp_packet_alloc(s->miniport,a[2],s->pool[p].protocol):ndis_nt_mdl(s->miniport->nt,a[3],a[4]);
        if(at) {
            s->object[i].address=at;s->object[i].pool=a[2];s->object[i].packet=packet;s->object[i].live=true;s->pool[p].used++;
        }
    }
    output(s,a[1],at);output(s,a[0],at?NDIS_SUCCESS:NDIS_RESOURCES);return 0;
}
static uint32_t object_free(struct ndis_services *s,uint32_t address,bool packet)
{
    int i=object_index(s,address,packet);
    if(i<0) return fault(s,"ndis-object-free");
    int p=pool_index(s,s->object[i].pool,packet);
    if(p<0 || !s->pool[p].used || !(packet?ndis_mp_packet_free(s->miniport,address):ndis_nt_free_mdl(s->miniport->nt,address)))
        return fault(s,"ndis-object-owned");
    s->object[i].live=false;s->pool[p].used--;return 0;
}
static uint32_t pool_free(struct ndis_services *s,uint32_t handle,bool packet)
{
    int i=pool_index(s,handle,packet);
    if(i<0 || s->pool[i].used || !ndis_nt_free(s->miniport->nt,handle)) return fault(s,"ndis-pool-owned");
    s->pool[i].live=false;return 0;
}
static int config_handle(struct ndis_services *s,uint32_t handle)
{ for(unsigned i=0;i<16;i++) if(s->configuration[i] && s->configuration[i]==handle) return (int)i;return -1; }
static uint32_t read_configuration(struct ndis_services *s,const uint32_t *a)
{
    if(config_handle(s,a[2])<0) return fault(s,"ndis-configuration-handle");
    struct ndis_nt_string name;uint16_t key[64];
    if(!rd(s,a[3],&name,8) || (name.length&1) || name.length>name.maximum || name.length>sizeof(key) ||
       !rd(s,name.buffer,key,name.length)) return fault(s,"ndis-configuration-key");
    unsigned found=32;
    for(unsigned i=0;i<s->parameter_count;i++) if(s->parameter[i].name_bytes==name.length) {
        bool same=true;
        for(unsigned j=0;j<name.length/2;j++) {
            unsigned l=key[j],r=s->parameter[i].name[j];
            if(l>='a' && l<='z') l-=32;if(r>='a' && r<='z') r-=32;
            if(l!=r) same=false;
        }
        if(same) { found=i;break; }
    }
    if(found==32 || (a[4]!=s->parameter[found].type && !(a[4]<=1 && s->parameter[found].type<=1))) { output(s,a[0],NDIS_FAILURE);return 0; }
    unsigned slot=0;while(slot<32 && s->config_value[slot].address) slot++;
    uint32_t at=slot<32?pool_alloc(s,16+s->parameter[found].value_bytes):0;
    if(!at) { output(s,a[0],NDIS_RESOURCES);return 0; }
    uint32_t value[3]={a[4],0,0};
    if(a[4]<=1) {
        if(s->parameter[found].value_bytes!=4) return fault(s,"ndis-configuration-integer");
        value[1]=pe_u32(s->parameter[found].value);
    } else if(a[4]==2 || a[4]==3) {
        unsigned bytes=s->parameter[found].value_bytes;
        if(bytes&1) return fault(s,"ndis-configuration-string");
        value[1]=bytes|(bytes<<16);value[2]=at+16;
    } else if(a[4]==4) { value[1]=s->parameter[found].value_bytes;value[2]=at+16; }
    else return fault(s,"ndis-configuration-type");
    if(!wr(s,at,value,12) || !wr(s,at+16,s->parameter[found].value,s->parameter[found].value_bytes)) return NDIS_FAILURE;
    s->config_value[slot].address=at;s->config_value[slot].handle=a[2];
    output(s,a[1],at);output(s,a[0],NDIS_SUCCESS);return 0;
}
uint32_t ndis_service(struct ndis_services *s,uint32_t op,uint32_t arguments)
{
    if(!live(s)) return NDIS_FAILURE;
    if(op>=NDIS_SERVICE_COUNT || !specs[op].implemented) return fault(s,"ndis-unsupported-service");
    uint32_t a[8]={0};
    if(!rd(s,arguments,a,specs[op].argc*4)) return NDIS_FAILURE;
    struct ndis_miniport *m=s->miniport;struct ndis_nt_runtime *r=m->nt;uint8_t old=0;
    switch(op) {
    case NDIS_OP_NdisInitializeWrapper:
        if(s->wrapper_open || m->state!=NDIS_MP_ENTRY || a[3]) return fault(s,"ndis-wrapper-init");
        s->wrapper_open=true;return output(s,a[0],m->wrapper);
    case NDIS_OP_NdisTerminateWrapper:
        if(!s->wrapper_open || a[0]!=m->wrapper || a[1] || (m->state!=NDIS_MP_STOPPED && m->state!=NDIS_MP_ENTRY)) return fault(s,"ndis-wrapper-terminate");
        s->wrapper_open=false;return 0;
    case NDIS_OP_NdisMRegisterMiniport:
        return s->wrapper_open && a[0]==m->wrapper?ndis_mp_register(m,a[1],a[2]):NDIS_FAILURE;
    case NDIS_OP_NdisMSetAttributesEx:
        return a[0]==m->adapter && ndis_mp_attributes(m,a[1],a[2],a[3],a[4])?0:fault(s,"ndis-attributes");
    case NDIS_OP_NdisMRegisterInterrupt:
        return a[1]==m->adapter && ndis_mp_irq(m,a[0],a[2],a[3],a[4]!=0,a[5]!=0,a[6])?NDIS_SUCCESS:NDIS_FAILURE;
    case NDIS_OP_NdisMDeregisterInterrupt:
        if(a[0]!=m->interrupt_object || !m->ops.interrupt(m->ops.context,false,0,0)) return fault(s,"ndis-interrupt-drain");
        m->interrupt_object=0;return 0;
    case NDIS_OP_NdisMSynchronizeWithInterrupt: {
        if(!m->interrupt_object || a[0]!=m->interrupt_object || !ndis_mp_inline(m,a[1],1,a+2,true)) return fault(s,"ndis-interrupt-synchronize");
        return NDIS_PENDING;
    }
    case NDIS_OP_NdisAllocateMemoryWithTag: {
        uint32_t p=ndis_nt_pool(r,0,a[1],a[2]);output(s,a[0],p);return p?NDIS_SUCCESS:NDIS_RESOURCES;
    }
    case NDIS_OP_NdisFreeMemory:return ndis_nt_free(r,a[0])?0:fault(s,"ndis-memory-free");
    case NDIS_OP_NdisAllocatePacketPool:return pool_create(s,a,true);
    case NDIS_OP_NdisAllocateBufferPool:return pool_create(s,a,false);
    case NDIS_OP_NdisAllocatePacket:return object_create(s,a,true);
    case NDIS_OP_NdisAllocateBuffer:return object_create(s,a,false);
    case NDIS_OP_NdisFreePacketPool:return pool_free(s,a[0],true);
    case NDIS_OP_NdisFreeBufferPool:return pool_free(s,a[0],false);
    case NDIS_OP_NdisFreePacket:return object_free(s,a[0],true);
    case NDIS_OP_NdisFreeBuffer:return object_free(s,a[0],false);
    case NDIS_OP_NdisAdjustBufferLength:
    case NDIS_OP_NdisQueryBufferOffset:
    case NDIS_OP_NdisQueryBufferSafe:
    case NDIS_OP_NDIS_BUFFER_TO_SPAN_PAGES: {
        unsigned i=0;while(i<NDIS_NT_OBJECTS && (!r->mdl[i].live || r->mdl[i].address!=a[0])) i++;
        struct ndis_nt_mdl32 b;
        if(i==NDIS_NT_OBJECTS || !rd(s,a[0],&b,sizeof(b))) return fault(s,"ndis-buffer-owned");
        if(b.bytes>r->mdl[i].bytes || b.mapped!=r->mdl[i].buffer) return fault(s,"ndis-buffer-bounds");
        if(op==NDIS_OP_NdisAdjustBufferLength) {
            if(a[1]>r->mdl[i].bytes) return fault(s,"ndis-buffer-adjust");
            b.bytes=a[1];return wr(s,a[0],&b,sizeof(b))?0:NDIS_FAILURE;
        }
        if(op==NDIS_OP_NDIS_BUFFER_TO_SPAN_PAGES) return b.bytes?(b.offset+b.bytes+4095)/4096:1;
        output(s,a[1],op==NDIS_OP_NdisQueryBufferOffset?b.offset:b.mapped);return output(s,a[2],b.bytes);
    }
    case NDIS_OP_NdisUnchainBufferAtFront: {
        struct ndis_packet32 p;struct ndis_nt_mdl32 b;uint32_t head;
        if(!rd(s,a[0],&p,56)) return NDIS_FAILURE;head=p.head;
        if(head) {
            if(!rd(s,head,&b,sizeof(b))) return NDIS_FAILURE;
            p.head=b.next;if(!p.head) p.tail=0;p.valid=0;b.next=0;
            if(!wr(s,head,&b,sizeof(b)) || !wr(s,a[0],&p,56)) return NDIS_FAILURE;
        }
        return output(s,a[1],head);
    }
    case NDIS_OP_NdisAllocateSpinLock:
    case NDIS_OP_NdisInitializeReadWriteLock:return ndis_nt_lock_init(r,a[0])?0:fault(s,"ndis-lock-init");
    case NDIS_OP_NdisAcquireSpinLock:
    case NDIS_OP_NdisDprAcquireSpinLock:
    case NDIS_OP_NdisAcquireReadWriteLock: {
        bool dpc=op==NDIS_OP_NdisDprAcquireSpinLock;
        if(op==NDIS_OP_NdisAcquireReadWriteLock && a[1]>1) return fault(s,"ndis-rw-mode");
        if(!ndis_nt_lock(r,a[0],dpc,&old)) return NDIS_FAILURE;
        return output(s,op==NDIS_OP_NdisAcquireReadWriteLock?a[2]:a[0]+4,old);
    }
    case NDIS_OP_NdisReleaseSpinLock:
    case NDIS_OP_NdisDprReleaseSpinLock:
    case NDIS_OP_NdisReleaseReadWriteLock: {
        uint32_t saved=0;bool dpc=op==NDIS_OP_NdisDprReleaseSpinLock;
        if(!dpc && !rd(s,op==NDIS_OP_NdisReleaseReadWriteLock?a[1]:a[0]+4,&saved,4)) return NDIS_FAILURE;
        return saved<=255 && ndis_nt_unlock(r,a[0],dpc,(uint8_t)saved)?0:fault(s,"ndis-lock-release");
    }
    case NDIS_OP_NdisFreeSpinLock:
        for(unsigned i=0;i<NDIS_NT_OBJECTS;i++) if(r->lock[i].initialized && r->lock[i].address==a[0]) {
            if(r->lock[i].held) return fault(s,"ndis-lock-held");r->lock[i].initialized=false;return 0;
        }
        return fault(s,"ndis-lock-free");
    case NDIS_OP_NdisInitializeEvent:return ndis_nt_event_init(r,a[0],true,0,1)?0:fault(s,"ndis-event-init");
    case NDIS_OP_NdisSetEvent:case NDIS_OP_NdisResetEvent: {
        int32_t previous;
        return (op==NDIS_OP_NdisSetEvent?ndis_nt_event_set(r,a[0],1,&previous):ndis_nt_event_reset(r,a[0],&previous))?0:fault(s,"ndis-event-state");
    }
    case NDIS_OP_NdisWaitEvent: {
        int64_t timeout=-(int64_t)a[1]*10000;
        return r->host->irql?fault(s,"ndis-wait-irql"):ndis_nt_wait_begin(r,a,1,false,a[1]?&timeout:0,true);
    }
    case NDIS_OP_NdisMSleep:return ndis_nt_delay_begin(r,(uint64_t)a[0]*10);
    case NDIS_OP_NdisInitializeTimer:case NDIS_OP_NdisMInitializeTimer: {
        bool mini=op==NDIS_OP_NdisMInitializeTimer;
        if(mini && a[1]!=m->adapter) return fault(s,"ndis-timer-adapter");
        return ndis_nt_timer_init(r,a[0]) && ndis_nt_dpc_init(r,a[0]+40,a[mini?2:1],a[mini?3:2])?0:fault(s,"ndis-timer-init");
    }
    case NDIS_OP_NdisSetTimer:case NDIS_OP_NdisMSetPeriodicTimer: {
        bool previous;return ndis_nt_timer_set(r,a[0],-(int64_t)a[1]*10000,a[0] && op==NDIS_OP_NdisMSetPeriodicTimer?a[1]:0,a[0]+40,&previous)?0:fault(s,"ndis-timer-set");
    }
    case NDIS_OP_NdisCancelTimer:case NDIS_OP_NdisMCancelTimer: {
        bool previous;if(!ndis_nt_timer_cancel(r,a[0],&previous)) return fault(s,"ndis-timer-cancel");
        /* Cancel a queued generation as well, but not an executing callback. */
        if(ndis_nt_dpc_remove(r,a[0]+40)) previous=true;
        uint8_t cancelled=previous;return wr(s,a[1],&cancelled,1)?0:NDIS_FAILURE;
    }
    case NDIS_OP_NdisInterlockedIncrement:case NDIS_OP_NdisInterlockedDecrement: {
        uint32_t value;if((a[0]&3) || !rd(s,a[0],&value,4)) return fault(s,"ndis-interlocked");
        value+=op==NDIS_OP_NdisInterlockedIncrement?1u:UINT32_MAX;
        output(s,a[0],value);return value; /* UP, syscall IRQ exclusion */
    }
    case NDIS_OP_NdisInterlockedInsertTailList: {
        uint32_t list[2],last[2],entry[2];
        if(!ndis_nt_lock(r,a[2],false,&old) || !rd(s,a[0],list,8) || !rd(s,list[1],last,8) || last[0]!=a[0]) return fault(s,"ndis-list-state");
        entry[0]=a[0];entry[1]=list[1];uint32_t previous=list[1]==a[0]?0:list[1];
        output(s,list[1],a[1]);output(s,a[0]+4,a[1]);wr(s,a[1],entry,8);
        if(!ndis_nt_unlock(r,a[2],false,old)) return NDIS_FAILURE;return previous;
    }
    case NDIS_OP_NdisMAllocateMapRegisters:
        return a[0]==m->adapter && m->attributes_set && (m->attributes&NDIS_BUS_MASTER) && !a[1] &&
            (a[2]==1 || a[2]==2) && ndis_nt_dma_reserve(r,a[3],a[4])?NDIS_SUCCESS:NDIS_RESOURCES;
    case NDIS_OP_NdisMInitializeScatterGatherDma:
        if(a[0]!=m->adapter || !m->attributes_set || !(m->attributes&NDIS_BUS_MASTER) || a[1]>1 || !a[2]) return NDIS_NOT_SUPPORTED;
        if(!ndis_nt_dma_reserve(r,NDIS_RING_SLOTS,NDIS_RING_FRAME)) return NDIS_RESOURCES;
        m->sg=true;return NDIS_SUCCESS;
    case NDIS_OP_NdisMFreeMapRegisters:
        return a[0]==m->adapter && ndis_nt_dma_release(r)?0:fault(s,"ndis-map-registers-live");
    case NDIS_OP_NdisMStartBufferPhysicalMapping: {
        struct ndis_nt_segment segments[NDIS_NT_MAP_REGISTERS];unsigned count=NDIS_NT_MAP_REGISTERS;
        if(a[0]!=m->adapter || a[3]>1 || !ndis_nt_dma_start(r,a[2],a[1],a[3]?MM_PIN_TO_DEVICE:MM_PIN_FROM_DEVICE,
            UINT32_MAX,1,0,UINT32_MAX,segments,&count)) return fault(s,"ndis-dma-map");
        for(unsigned i=0;i<count;i++) { uint32_t unit[4]={segments[i].physical,0,segments[i].bytes,0};
            if(!wr(s,a[4]+i*16,unit,16)) return NDIS_FAILURE; }
        return output(s,a[5],count);
    }
    case NDIS_OP_NdisMCompleteBufferPhysicalMapping:
        return a[0]==m->adapter && a[2]<r->map_registers && r->dma[a[2]].mdl==a[1] &&
            ndis_nt_dma_complete(r,a[2],m->generation,r->dma[a[2]].pin.ticket)?0:fault(s,"ndis-dma-complete");
    case NDIS_OP_NdisMAllocateSharedMemory: {
        uint32_t at=0,physical[2]={0};unsigned i=0;while(i<16 && s->shared[i].live) i++;
        /* WB aliases use the existing pinned arena. UC requests cannot alias
         * its WB direct map; fail allocation rather than invent UC memory. */
        if(a[0]==m->adapter && m->attributes_set && (m->attributes&NDIS_BUS_MASTER) && a[2]==1 && i<16 && a[1]) {
            at=pool_alloc(s,a[1]);
            if(at) {
                physical[0]=r->pin.first_pfn*4096+r->pin_offset+at-r->arena;
                struct mm_pin p={.generation=m->generation,.first_pfn=physical[0]/4096,
                    .pages=((physical[0]&4095)+a[1]+4095)/4096,.direction=MM_PIN_BIDIRECTIONAL,.alignment=1,.address_mask=UINT32_MAX};
                if(!mm_pin || mm_pin(&p)) { ndis_nt_free(r,at);at=0;physical[0]=0; }
                else { s->shared[i].address=at;s->shared[i].bytes=a[1];s->shared[i].physical=physical[0];
                    s->shared[i].cached=true;s->shared[i].pin=p;s->shared[i].live=true; }
            }
        }
        output(s,a[3],at);return wr(s,a[4],physical,8)?0:NDIS_FAILURE;
    }
    case NDIS_OP_NdisMFreeSharedMemory:
        for(unsigned i=0;i<16;i++) if(s->shared[i].live && s->shared[i].address==a[3]) {
            if(a[0]!=m->adapter || a[1]!=s->shared[i].bytes || a[2]!=1 || a[4]!=s->shared[i].physical || a[5] ||
               !mm_unpin || mm_unpin(&s->shared[i].pin,m->generation) || !ndis_nt_free(r,a[3])) return fault(s,"ndis-shared-free");
            s->shared[i].live=false;return 0;
        }
        return fault(s,"ndis-shared-foreign");
    case NDIS_OP_NdisMGetDmaAlignment:return a[0]==m->adapter?4096:0;
    case NDIS_OP_NdisReadPciSlotInformation:case NDIS_OP_NdisWritePciSlotInformation: {
        if(a[0]!=m->adapter || a[1] || !a[4] || a[4]>256) return 0;
        uint8_t data[256];bool write=op==NDIS_OP_NdisWritePciSlotInformation;
        if(write && !rd(s,a[3],data,a[4])) return 0;
        unsigned n=s->ops.pci(s->ops.context,write,a[2],data,a[4]);
        return n<=a[4] && (write || wr(s,a[3],data,n))?n:0;
    }
    case NDIS_OP_NdisMRegisterIoPortRange: {
        uint32_t port=a[1]==m->adapter && m->attributes_set?s->ops.port(s->ops.context,a[2],a[3],false):0;
        output(s,a[0],port);return port?NDIS_SUCCESS:NDIS_FAILURE;
    }
    case NDIS_OP_NdisMDeregisterIoPortRange:
        return a[0]==m->adapter && a[1]==a[3] && s->ops.port(s->ops.context,a[1],a[2],true)?0:fault(s,"ndis-port-release");
    case NDIS_OP_NdisMMapIoSpace: {
        uint32_t va=a[1]==m->adapter && m->attributes_set && !a[3]?s->ops.mmio(s->ops.context,a[2],a[4],false):0;
        output(s,a[0],va);return va?NDIS_SUCCESS:NDIS_FAILURE;
    }
    case NDIS_OP_NdisMUnmapIoSpace:return a[0]==m->adapter && s->ops.mmio(s->ops.context,a[1],a[2],true)?0:fault(s,"ndis-mmio-release");
    case NDIS_OP_NdisMQueryAdapterResources: {
        uint8_t data[128];uint32_t capacity;if(a[1]!=m->adapter || !rd(s,a[3],&capacity,4)) return fault(s,"ndis-resource-handle");
        unsigned n=s->ops.resources(s->ops.context,data,sizeof(data));
        if(!n || n>sizeof(data)) return fault(s,"ndis-resource-list");
        output(s,a[3],n);if(capacity<n) return output(s,a[0],NDIS_INVALID_LENGTH);
        wr(s,a[2],data,n);return output(s,a[0],NDIS_SUCCESS);
    }
    case NDIS_OP_NdisMPciAssignResources:
        output(s,a[2],a[0]==m->adapter?s->resource_list:0);return a[0]==m->adapter && !a[1]?NDIS_SUCCESS:NDIS_FAILURE;
    case NDIS_OP_NdisOpenConfiguration: {
        unsigned i=0;while(i<16 && s->configuration[i]) i++;
        uint32_t handle=a[2]==m->config && i<16?pool_alloc(s,8):0;
        if(handle) s->configuration[i]=handle;output(s,a[1],handle);return output(s,a[0],handle?NDIS_SUCCESS:NDIS_FAILURE);
    }
    case NDIS_OP_NdisCloseConfiguration: {
        int i=config_handle(s,a[0]);if(i<0) return fault(s,"ndis-config-close");
        for(unsigned j=0;j<32;j++) if(s->config_value[j].handle==a[0]) {
            if(!ndis_nt_free(r,s->config_value[j].address)) return fault(s,"ndis-config-value-free");
            s->config_value[j].address=s->config_value[j].handle=0;
        }
        s->configuration[i]=0;return ndis_nt_free(r,a[0])?0:NDIS_FAILURE;
    }
    case NDIS_OP_NdisReadConfiguration:return read_configuration(s,a);
    case NDIS_OP_NdisReadNetworkAddress: {
        if(config_handle(s,a[3])<0) return fault(s,"ndis-network-address-handle");
        uint8_t address[6]={0};unsigned digits=0;bool found=false;
        const char key[]="NetworkAddress";
        for(unsigned i=0;i<s->parameter_count;i++) {
            if(s->parameter[i].type!=2 || s->parameter[i].name_bytes!=(sizeof(key)-1)*2) continue;
            bool match=true;
            for(unsigned j=0;j<sizeof(key)-1;j++) {
                unsigned c=s->parameter[i].name[j],k=(unsigned char)key[j];
                if(c>='a' && c<='z') c-=32;if(k>='a' && k<='z') k-=32;if(c!=k) match=false;
            }
            if(!match) continue;found=true;
            unsigned bytes=s->parameter[i].value_bytes;
            if(bytes&1) { found=false;break; }
            for(unsigned j=0;j<bytes;j+=2) {
                unsigned c=pe_u16(s->parameter[i].value+j);if(!c && j+2==bytes) break;if(c=='-') continue;
                unsigned digit=c>='0' && c<='9'?c-'0':c>='A' && c<='F'?c-'A'+10:c>='a' && c<='f'?c-'a'+10:16;
                if(digit==16 || digits==12) { found=false;break; }
                address[digits/2]=(uint8_t)((address[digits/2]<<4)|digit);digits++;
            }
            break;
        }
        unsigned slot=0;while(slot<32 && s->config_value[slot].address) slot++;
        unsigned any=0;for(unsigned i=0;i<6;i++) any|=address[i];
        if(!found || digits!=12 || !any || (address[0]&1) || slot==32) {
            output(s,a[1],0);output(s,a[2],0);return output(s,a[0],NDIS_FAILURE);
        }
        uint32_t at=pool_alloc(s,8);if(!at) return output(s,a[0],NDIS_RESOURCES);
        if(!wr(s,at,address,6)) return NDIS_FAILURE;
        s->config_value[slot].address=at;s->config_value[slot].handle=a[3];
        output(s,a[1],at);output(s,a[2],6);return output(s,a[0],NDIS_SUCCESS);
    }
    case NDIS_OP_NdisMQueryAdapterInstanceName:
        if(a[1]!=m->adapter || !s->instance_string) return NDIS_FAILURE;
        { struct ndis_nt_string name;uint8_t text[128];
          if(!rd(s,s->instance_string,&name,8) || name.maximum>sizeof(text) || name.length>name.maximum ||
             !rd(s,name.buffer,text,name.maximum)) return NDIS_FAILURE;
          uint32_t at=pool_alloc(s,name.maximum);if(!at) return NDIS_RESOURCES;
          if(!wr(s,at,text,name.maximum)) return NDIS_FAILURE;name.buffer=at;
          return wr(s,a[0],&name,8)?NDIS_SUCCESS:NDIS_FAILURE; }
    case NDIS_OP_NdisMRegisterAdapterShutdownHandler:
        if(a[0]!=m->adapter || m->shutdown || !ntimage_executable(r->host->image,a[2])) return fault(s,"ndis-shutdown-register");
        m->shutdown=a[2];m->shutdown_context=a[1];return 0;
    case NDIS_OP_NdisMDeregisterAdapterShutdownHandler:
        if(a[0]!=m->adapter) return fault(s,"ndis-shutdown-handle");m->shutdown=m->shutdown_context=0;return 0;
    case NDIS_OP_NdisMRegisterUnloadHandler:
        if(a[0]!=m->wrapper || !ntimage_executable(r->host->image,a[1]) || m->unload) return fault(s,"ndis-unload-register");
        m->unload=a[1];return 0;
    case NDIS_OP_NdisGetRoutineAddress: {
        struct ndis_nt_string name;uint16_t text[128];
        if(!rd(s,a[0],&name,8) || !name.length || (name.length&1) || name.length>name.maximum || name.length>sizeof(text) ||
           !rd(s,name.buffer,text,name.length)) return 0;
        for(unsigned i=0;i<NDIS_SERVICE_COUNT;i++) if(specs[i].implemented) {
            unsigned j=0;for(;j<name.length/2 && specs[i].name[j] && specs[i].name[j]==text[j];j++);
            if(j==name.length/2 && !specs[i].name[j]) return veneer(s,(enum ndis_service_operation)i);
        }
        return 0;
    }
    case NDIS_OP_NdisWriteErrorLogEntry:case NDIS_OP_NdisWriteEventLogEntry: {
        uint32_t values[16];
        if(op==NDIS_OP_NdisWriteErrorLogEntry) {
            if(a[0]!=m->adapter || a[2]>16 || !rd(s,arguments+12,values,a[2]*4)) return fault(s,"ndis-log-bounds");
            s->ops.log(s->ops.context,a[1],values,a[2]);return 0;
        }
        if(a[0]!=m->adapter || a[3] || a[5]>sizeof(values) || (a[5] && !rd(s,a[6],values,a[5]))) return NDIS_FAILURE;
        s->ops.log(s->ops.context,a[1],values,a[5]/4);return NDIS_SUCCESS;
    }
    case NDIS_OP_NdisMIndicateReceivePacket: {
        uint32_t packets[NDIS_RING_SLOTS];
        return a[0]==m->adapter && a[2] && a[2]<=NDIS_RING_SLOTS && rd(s,a[1],packets,a[2]*4) &&
            ndis_mp_receive(m,packets,a[2])?0:fault(s,"ndis-receive-indication");
    }
    case NDIS_OP_NdisMSendComplete:return a[0]==m->adapter && ndis_mp_send_complete(m,a[1],a[2])?0:fault(s,"ndis-send-completion");
    case NDIS_OP_NdisMSendResourcesAvailable:return a[0]==m->adapter && ndis_mp_send_resources(m)?0:fault(s,"ndis-send-resources");
    case NDIS_OP_NdisMResetComplete:return a[0]==m->adapter && a[2]<=1 && ndis_mp_reset_complete(m,a[1],a[2])?0:fault(s,"ndis-reset-completion");
    case NDIS_OP_NdisMQueryInformationComplete:case NDIS_OP_NdisMSetInformationComplete:
        return a[0]==m->adapter && ndis_mp_oid_complete(m,op==NDIS_OP_NdisMSetInformationComplete,a[1])?0:fault(s,"ndis-oid-completion");
    case NDIS_OP_NdisMIndicateStatus:case NDIS_OP_NdisMIndicateStatusComplete:
        if(a[0]!=m->adapter) return fault(s,"ndis-status-handle");
        ndis_mp_status(m,a[1],op==NDIS_OP_NdisMIndicateStatusComplete);return 0;
    case NDIS_OP_NdisMTransferDataComplete:
        return a[0]==m->adapter && ndis_mp_transfer_complete(m,a[1],a[2],a[3])?0:fault(s,"ndis-transfer-completion");
    case NDIS_OP_CiukiEthReceive:
        return a[0]==m->adapter && ndis_mp_lookahead(m,a[1],a[3],a[4],a[5],a[6],a[7])?0:fault(s,"ndis-lookahead");
    case NDIS_OP_CiukiEthReceiveComplete:return a[0]==m->adapter?0:fault(s,"ndis-lookahead-complete");
    default:return fault(s,"ndis-unsupported-service");
    }
}
