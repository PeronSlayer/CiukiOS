/* Registry, task, packet and quarantine wiring. Never calls foreign code.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "native.h"
#include "fence.h"
#include "execution.h"
#include "nt_runtime.h"
#include "netif.h"
#include "package.h"
#include <ciuki/sha256.h>
#include <stddef.h>
#include <ciuki/clock.h>
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
static struct ndis_native *adapters[NDIS_HOST_MAX];
bool ndis_package_busy(const struct pci_func *p)
{
    if(!p) return false;
    for(unsigned i=0;i<NDIS_HOST_MAX;i++) {
        const struct ndis_native *n=adapters[i];
        if(n && n->pci.bus==p->bus && n->pci.dev==p->dev && n->pci.fn==p->fn) return true;
    }
    return false;
}
#define NT_POOL_BYTES 65536u
static int nt_start(struct ndis_native *);
static bool owned(void *ctx,uint32_t generation)
{
    struct ndis_native *n=ctx;
    bool valid=generation==n->memory.generation && !n->retained &&
        registry_valid(n->irq_claim,n->irq_generation) &&
        ndis_qualified_profile(n->host.image,&n->pci)==n->qualification;
    const struct resource *irq=registry_get((unsigned)n->irq_claim);
    valid=valid && irq && irq->state==RS_ACTIVE && irq->owner==n->owner;
    for(unsigned i=0;valid && i<n->host.port_count;i++) {
        const struct resource *port=registry_get((unsigned)n->port_claim[i]);
        valid=registry_valid(n->port_claim[i],n->port_generation[i]) && port && port->state==RS_ACTIVE && port->owner==n->owner;
    }
    for(unsigned i=0;valid && i<NDIS_HOST_MAPS;i++) if(n->memory.grant[i].admitted)
        valid=ndis_user_mapping_owned(&n->memory,&n->memory.grant[i].view);
    if(valid && n->memory.image_pin.ticket) valid=mm_pin_valid && mm_pin_valid(&n->memory.image_pin,generation);
    if(valid && n->runtime_pin.ticket) valid=mm_pin_valid && mm_pin_valid(&n->runtime_pin,generation);
    return valid;
}
static bool mapping(void *ctx,const struct ndis_host_map *m)
{ return ndis_user_mapping_owned(&((struct ndis_native *)ctx)->memory,m); }
static int map(void *ctx,const struct ndis_host_map *m,bool install)
{ return ndis_user_mapping_apply(&((struct ndis_native *)ctx)->memory,m,install); }
static bool mask(void *ctx,bool on)
{ struct ndis_native *n=ctx;return n->qualification->mask(n,on); }
static int cause(void *ctx)
{ struct ndis_native *n=ctx;return n->qualification->cause(n); }
static void wake_host(void *ctx)
{ struct ndis_native *n=ctx;if(n->task && n->task->ndis_user_host==&n->host &&
    n->host.state==NDIS_HOST_LIVE) task_start(n->task); }
static void wake_worker(void *ctx)
{ struct ndis_native *n=ctx;if(n->wake_worker) n->wake_worker(n->worker_context); }
static void notify(void *ctx) { wake_host(ctx);wake_worker(ctx); }
static void complete_tx(void *ctx,uint32_t token,int status)
{ struct ndis_native *n=ctx;ndis_netif_tx_done(n,token,status);if(n->complete_tx) n->complete_tx(n->worker_context,token,status); }
static bool fence(void *ctx)
{
    struct ndis_native *n=ctx;uint16_t before=0,after=0;
    /* An optional native engine stop must precede decode removal. Its return
     * cannot suppress the independent command fence or backing retention. */
    bool idle=n->qualification->stop(n);
    bool disabled=ndis_pci_disable(&n->pci,&before,&after);
    return disabled && idle;
}
static bool interrupt(unsigned irq,const char *owner);
static void detach(void *ctx,uint32_t old)
{
    (void)old;struct ndis_native *n=ctx;
    if(n->irq_attached) {
        if(!irq_chain_remove(n->pci.irq_line,interrupt,n->owner)) n->irq_attached=false;
        else n->detach_pending=true; /* registry drops quarantined members, worker retries */
    }
    (void)registry_quarantine(n->irq_claim,n->irq_generation);
    for(unsigned i=0;i<n->port_count;i++)
        (void)registry_quarantine(n->port_claim[i],n->port_generation[i]);
    for(unsigned i=0;i<NDIS_HOST_MAPS;i++) if(n->memory.grant[i].admitted &&
        n->memory.grant[i].view.kind==NDIS_HOST_BAR)
        (void)registry_quarantine(n->memory.grant[i].claim,n->memory.grant[i].claim_generation);
}
static void retain(void *ctx)
{
    struct ndis_native *n=ctx;n->retained=n->memory.retained=true;
    ndis_nt_services_stop(&n->services);
    if(n->host.nt) ndis_nt_stop(n->host.nt);
    ndis_channel_stop(&n->channel,-1); /* accepted TX tokens complete once */
}
static void cancel(void *ctx)
{
    struct ndis_native *n=ctx;
    if(n->task && n->task->ndis_user_host!=&n->host) {
        /* A failed pre-attach bind must never start its raw user payload. */
        if(task_alive(n->task)) task_kill(n->task,-1);
        return;
    }
    if(n->task==g_current) g_need_resched=true;
    else if(n->task && task_alive(n->task)) task_start(n->task); /* return hook kills after fence */
}
static void record(void *ctx,const char *why,uint32_t vector,uint32_t offset)
{
    struct ndis_native *n=ctx;
    klog("[ndis] fault driver=%s vector=%u eip=+%x reason=%s",n->host.image->file,vector,offset,why);
}
static bool interrupt(unsigned irq,const char *owner)
{
    for(unsigned i=0;i<NDIS_HOST_MAX;i++) {
        struct ndis_native *n=adapters[i];
        if(!n || n->owner!=owner || n->pci.irq_line!=irq || !ndis_host_live(&n->host)) continue;
        if(n->miniport.power_phase==4) return false;
        if(!ndis_host_interrupt(&n->host,ndis_execution_now())) return false;
        if(n->irq_delivery!=n->host.irq_token) n->dpc_phase=false;
        n->irq_accounted=false;
        n->irq_delivery=n->host.irq_token;
        if(!irq_chain_pending(irq,n->irq_claim,n->irq_generation,n->irq_delivery)) {
            ndis_host_stop(&n->host,"irq-pending-rejected",0,0,ndis_execution_now());return false;
        }
        wake_worker(n);return false; /* asynchronous != not-mine */
    }
    return false;
}
int ndis_native_bind(struct ndis_native *n,struct task *task,struct ntimage *image,unsigned index,
                      int claim,gen_t claim_gen,const char *owner,uint32_t generation,
                      uint32_t dispatcher,uint32_t stack_top,uint32_t stack_bytes,
                      struct ndis_channel_shared *shared,bool sharing)
{
    const struct pci_func *p=pci_get(index);
    const struct ndis_qualification *q=ndis_qualified_profile(image,p);
    if(!n || !task || !q || !owner || !generation || !shared || !n->wake_worker || !n->complete_tx ||
       !ndis_package_admit(image->file,image->hash,p,q->package_seal) ||
       !q->shared_mask || !p->irq_pin || !pci_irq_routed(p) ||
       !registry_valid(claim,claim_gen) || stack_bytes<4096 || (stack_bytes&4095) ||
       stack_top<stack_bytes*NDIS_HOST_DEPTH || stack_top>=0xc0000000u) return 0;
    const struct resource *r=registry_get((unsigned)claim);
    if(!r || r->type!=RES_IRQ || r->state!=RS_ACTIVE || r->generation!=claim_gen ||
       r->start!=p->irq_line || r->end!=p->irq_line+1u || r->owner!=owner) return 0;
    unsigned slot=0;while(slot<NDIS_HOST_MAX && adapters[slot]) slot++;
    if(slot==NDIS_HOST_MAX || n->host.state!=NDIS_HOST_NEW || n->memory.generation!=generation) return 0;
    n->pci=*p;n->task=task;n->qualification=q;n->owner=owner;n->irq_claim=claim;n->irq_generation=claim_gen;
    if(n->host.port_count>6) return 0;
    n->port_count=n->host.port_count;
    for(unsigned i=0;i<n->host.port_count;i++) {
        if(n->host.ports[i].first>=n->host.ports[i].end || n->host.ports[i].end>65536) return 0;
        for(unsigned j=0;j<i;j++) if(n->host.ports[j].first<n->host.ports[i].end &&
            n->host.ports[i].first<n->host.ports[j].end) return 0;
        const struct resource *port=registry_get((unsigned)n->port_claim[i]);bool found=false;
        for(unsigned bar=0;bar<6;bar++) if((p->bar[bar]&1) && q->bar_bytes[bar] &&
            n->host.ports[i].first==(p->bar[bar]&~3u) &&
            n->host.ports[i].end-n->host.ports[i].first==q->bar_bytes[bar]) found=true;
        if(!found || !port || port->type!=RES_PORT || port->state!=RS_ACTIVE || port->owner!=owner ||
           port->generation!=n->port_generation[i] || port->start!=n->host.ports[i].first ||
           port->end!=n->host.ports[i].end || !registry_valid(n->port_claim[i],n->port_generation[i])) return 0;
    }
    for(unsigned i=0;i<NDIS_HOST_MAPS;i++) if(n->memory.grant[i].admitted && n->memory.grant[i].view.kind==NDIS_HOST_BAR) {
        const struct ndis_user_grant *grant=&n->memory.grant[i];bool found=false;
        for(unsigned bar=0;bar<6;bar++) if(!(p->bar[bar]&1) && q->bar_bytes[bar] &&
            grant->view.physical==(p->bar[bar]&~15u) && grant->view.bytes==q->bar_bytes[bar]) found=true;
        const struct resource *res=registry_get((unsigned)grant->claim);
        if(!found || !res || res->owner!=owner || !ndis_user_mapping_owned(&n->memory,&grant->view)) return 0;
    }
    n->host.image=image;n->host.qualified=true;n->host.native=n;
    if(n->shared_address<0x10000 || n->shared_address>=0xc0000000 ||
       sizeof(*shared)>0xc0000000-n->shared_address || ((uintptr_t)shared&4095) ||
       (n->shared_address&4095)) return 0;
    for(uint32_t at=0;at<sizeof(*shared);at+=4096) {
        uint32_t pte=0,phys=as_lookup(&task->as,n->shared_address+at,&pte);
        if(phys!=V2P(shared)+at || !(pte&PTE_W) || !ledger_page_pinned(phys/4096)) return 0;
    }
    struct ndis_host_ops ops={n,owned,mapping,map,mask,cause,notify,fence,detach,retain,cancel,record};
    if(!ndis_host_init(&n->host,image,generation,&ops,sharing,q->shared_mask,5)) return 0;
    adapters[slot]=n; /* boot owner before any fallible device/map operation */
    for(unsigned i=0;i<NDIS_HOST_MAPS;i++) if(n->memory.grant[i].admitted &&
        !ndis_host_mapping(&n->host,&n->memory.grant[i].view)) {
        ndis_host_stop(&n->host,"native-grant-install-failed",0,0,ndis_execution_now());return 0;
    }
    n->host.dispatcher=dispatcher;n->host.stack_bytes=stack_bytes;n->host.native=n;
    for(unsigned i=0;i<NDIS_HOST_DEPTH;i++) n->host.dispatch_stack[i]=stack_top-i*stack_bytes;
    n->host.channel=&n->channel;
    if(n->runtime_physical && !nt_start(n)) {
        ndis_host_stop(&n->host,"nt-service-init-failed",0,0,ndis_execution_now());return 0;
    }
    if(!ndis_channel_init(&n->channel,shared,generation,n,wake_host,wake_worker,complete_tx) ||
       (n->host.nt && !ndis_native_miniport_init(n)) ||
       !mask(n,true) || !ndis_user_attach(task,&n->host)) {
        ndis_host_stop(&n->host,"native-bind-failed",0,0,ndis_execution_now());return 0;
    }
    if((n->isr || n->dpc) && !ndis_native_irq_bind(n,n->isr,n->dpc)) return 0;
    ndis_native_tick(n);
    return 1; /* DriverEntry can precede MiniportISR registration. */
}
int ndis_native_irq_bind(struct ndis_native *n,uint32_t isr,uint32_t dpc)
{
    if(!n || !ndis_host_live(&n->host) || !n->qualification->shared_mask || n->irq_attached || !ntimage_executable(n->host.image,isr) ||
       !ntimage_executable(n->host.image,dpc)) return 0;
    n->isr=isr;n->dpc=dpc;
    if(irq_chain_add(n->pci.irq_line,interrupt,n->owner)) {
        ndis_host_stop(&n->host,"irq-registration-failed",0,0,ndis_execution_now());return 0;
    }
    n->irq_attached=true;
    if(!n->qualification->mask(n,false)) {
        ndis_host_stop(&n->host,"irq-initial-rearm-failed",0,0,ndis_execution_now());return 0;
    }
    int pending=n->qualification->cause(n);
    if(pending<0) { ndis_host_stop(&n->host,"irq-initial-cause-failed",0,0,ndis_execution_now());return 0; }
    if(pending) {
        if(n->miniport.power_phase==4) return false;
        if(!ndis_host_interrupt(&n->host,ndis_execution_now())) return 0;
        n->irq_delivery=n->host.irq_token;n->irq_accounted=true;wake_worker(n);
    }
    return 1;
}
int ndis_native_take_result(struct ndis_native *n,uint32_t generation,uint32_t token,uint32_t *result)
{
    if(!n || !result || !ndis_host_live(&n->host) || generation!=n->host.generation || !token) return 0;
    uint32_t flags=irq_save();
    for(unsigned i=0;i<NDIS_RING_SLOTS;i++) if(n->result_token[i]==token) {
        *result=n->result[i];n->result_token[i]=0;irq_restore(flags);return 1;
    }
    irq_restore(flags);return 0;
}
int ndis_native_irq_unbind(struct ndis_native *n)
{
    if(!n || !ndis_host_live(&n->host) || !n->irq_attached || n->isr_callback || n->dpc_callback || n->host.irq_pending ||
       !n->qualification->mask(n,true) || irq_chain_remove(n->pci.irq_line,interrupt,n->owner)) return 0;
    n->irq_attached=false;n->isr=n->dpc=0;return 1;
}
int ndis_native_enqueue(struct ndis_native *n,enum ndis_host_kind kind,uint32_t address,
                         unsigned argc,const uint32_t *args,uint64_t now,uint32_t *token)
{
    if(!n || !ndis_host_live(&n->host) || argc>6 || (argc && !args)) return 0;
    uint32_t flags=irq_save();unsigned slot=0;
    while(slot<NDIS_RING_SLOTS && (n->callback_token[slot] || n->result_token[slot])) slot++;
    if(slot==NDIS_RING_SLOTS) { irq_restore(flags);return 0; }
    uint32_t *vector=n->channel.shared->callback[slot];vector[0]=argc;
    for(unsigned i=0;i<argc;i++) vector[i+1]=args[i];
    uint32_t id=0;
    int ok=ndis_host_enqueue(&n->host,kind,address,n->shared_address+
        offsetof(struct ndis_channel_shared,callback)+slot*7*4,now,&id);
    if(ok) { n->callback_token[slot]=id;if(token) *token=id; }
    irq_restore(flags);return ok;
}
void ndis_native_poll(struct ndis_native *n)
{
    if(!n) return;
    if(n->host.nt && n->miniport.power_phase != 4) ndis_nt_poll(n->host.nt);
    if(n->ndis.miniport) ndis_mp_poll(&n->miniport);
    if(n->miniport.power_phase==4) return;
    if(n->detach_pending) {
        int rc=irq_chain_remove(n->pci.irq_line,interrupt,n->owner);
        if(!rc) n->detach_pending=n->irq_attached=false;
    }
    if(!ndis_host_live(&n->host) || !n->host.irq_pending || n->isr_callback || n->dpc_callback) return;
    uint32_t flags=irq_save();
    if(n->irq_delivery!=n->host.irq_token) {
        n->irq_delivery=n->host.irq_token;n->irq_accounted=true;n->dpc_phase=false;
    }
    n->channel.shared->irq_result[0]=n->channel.shared->irq_result[1]=0;
    uint32_t args[3]={n->shared_address+offsetof(struct ndis_channel_shared,irq_result),
        n->shared_address+offsetof(struct ndis_channel_shared,irq_result)+4,n->adapter_context};
    if(!ndis_native_enqueue(n,NDIS_HOST_ISR,n->isr,3,args,ndis_execution_now(),&n->isr_callback))
        ndis_host_stop(&n->host,"isr-queue-full",0,0,ndis_execution_now());
    irq_restore(flags);
}
int ndis_native_isr_result(struct ndis_native *n,uint32_t generation,uint32_t delivery,
                            bool recognized,bool queue_dpc,uint64_t now)
{
    if(!n || !ndis_host_live(&n->host) || generation!=n->host.generation ||
       delivery!=n->irq_delivery || !n->host.irq_pending || !n->isr_callback) return 0;
    if(!n->irq_accounted && !irq_chain_resolve_pending(n->pci.irq_line,n->irq_claim,n->irq_generation,delivery,recognized)) return 0;
    n->irq_accounted=true;
    n->isr_callback=0;
    if(queue_dpc && !recognized) return 0;
    if(queue_dpc) {
        uint32_t args[1]={n->adapter_context};
        /* The 5 ms ISR phase has completed; the separate DPC phase owns its
         * 50 ms budget. The outer callback's deadline is unchanged. */
        if(!n->dpc_phase) {
            n->dpc_phase=true;n->host.irq_deadline=now>UINT64_MAX-50000?UINT64_MAX:now+50000;
        }
        return ndis_native_enqueue(n,NDIS_HOST_DPC,n->dpc,1,args,now,&n->dpc_callback);
    }
    return ndis_host_rearm(&n->host,generation,delivery,now);
}
void ndis_native_callback_result(struct ndis_native *n,const struct ndis_host_event *e,uint32_t result)
{
    (void)result;
    if(!n || !e || !ndis_host_live(&n->host)) return;
    unsigned slot=0;while(slot<NDIS_RING_SLOTS && n->callback_token[slot]!=e->token) slot++;
    if(slot==NDIS_RING_SLOTS) { ndis_host_stop(&n->host,"unknown-native-callback",0,e->address,ndis_execution_now());return; }
    n->callback_token[slot]=0;
    if(n->host.nt) for(unsigned i=0;i<NDIS_NT_OBJECTS;i++) if(n->host.nt->dpc[i].running_token==e->token) {
        n->host.nt->dpc[i].running_token=0;return; /* void NT DPC, no lifecycle result slot */
    }
    if(e->token==n->isr_callback) {
        uint32_t recognized=__atomic_load_n(&n->channel.shared->irq_result[0],__ATOMIC_ACQUIRE);
        uint32_t queue=__atomic_load_n(&n->channel.shared->irq_result[1],__ATOMIC_ACQUIRE);
        if(recognized>1 || queue>1 || !ndis_native_isr_result(n,e->generation,n->irq_delivery,
              recognized!=0,queue!=0,ndis_execution_now()))
            ndis_host_stop(&n->host,"invalid-isr-result",0,e->address,ndis_execution_now());
    } else if(e->token==n->dpc_callback) {
        n->dpc_callback=0;
        if(!ndis_host_rearm(&n->host,e->generation,n->irq_delivery,ndis_execution_now()))
            ndis_host_stop(&n->host,"invalid-dpc-rearm",0,e->address,ndis_execution_now());
    } else { n->result_token[slot]=e->token;n->result[slot]=result;wake_worker(n); }
}
static bool quiescent(void *ctx)
{
    struct ndis_native *n=ctx;
    return n->task && n->task->state==T_BLOCKED && !n->host.depth && !n->retained;
}
int ndis_native_discard(struct ndis_native *n)
{
    if(!n || n->retained || (n->host.image && (n->host.image->entered || n->host.image->quarantined))) return 0;
    while(n->runtime_mapped) {
        uint32_t at=n->runtime_mapped-4096;
        if(as_revoke_borrowed(&n->task->as,n->runtime_base+at,n->runtime_physical+at)) {
            n->retained=true;return 0;
        }
        n->runtime_mapped=at;
    }
    if(n->runtime_pin.ticket && mm_unpin(&n->runtime_pin,n->memory.generation)) {
        n->retained=true;return 0;
    }
    for(uint32_t at=0;at<n->runtime_bytes;at+=4096) mm_free(n->runtime_physical+at,MM_NET);
    n->runtime_physical=n->runtime_bytes=0;
    if(n->host.image && !ntimage_unload(n->host.image)) return 0;
    if(n->task) { task_kill(n->task,-1);n->task=0; }
    return 1;
}
int ndis_native_prepare(struct ndis_native *n,struct ntimage *image,const void *file,uint32_t bytes,
                         const char *name,uint32_t generation,uint32_t reserved_bytes,
                         uint32_t image_base,uint32_t runtime_base)
{
    extern const uint8_t ndis_dispatch_start[],ndis_dispatch_end[];
    uint32_t dispatch_bytes=(uint32_t)(ndis_dispatch_end-ndis_dispatch_start);
    const uint32_t stacks=NDIS_HOST_DEPTH*4096;
    const uint32_t runtime_bytes=12288+stacks+PAGE_ALIGN_UP(sizeof(struct ndis_channel_shared))+NT_POOL_BYTES;
    const uint32_t metadata_bytes=PAGE_ALIGN_UP(sizeof(*n)+sizeof(struct ndis_flag_table)+sizeof(struct ndis_flag_workspace));
    /* No task or allocation before a reviewed profile exists. Reservation is
     * supplied by the common F6 owner; this function never increases it. */
    if(!n || !image || !file || !bytes || bytes>NDIS_IMAGE_LIMIT || !generation || !ndis_profiles_available() ||
       n->task || n->runtime_physical || n->memory.physical || !mm_pin || !mm_unpin || !mm_pin_valid ||
       !reserved_bytes || reserved_bytes>NDIS_IMAGE_LIMIT || reserved_bytes<=runtime_bytes+metadata_bytes ||
       (runtime_base&4095) || runtime_base<0x10000 || runtime_base>=0xc0000000 ||
       runtime_bytes>0xc0000000-runtime_base || dispatch_bytes>256) return 0;
    uint8_t source_hash[32]; sha256(file,bytes,source_hash);
    if(!ndis_package_admit(name,source_hash,0,0)) return 0;
    struct task *t=task_create_user(name,P_DEVICE,runtime_base,runtime_base+12288+stacks,0,0,0);
    if(!t) return 0;
    n->task=t;n->host.image=image;
    n->memory.as=&t->as;n->memory.generation=generation;n->memory.execution_base=image_base;
    n->memory.reserved_bytes=reserved_bytes-runtime_bytes-metadata_bytes;n->memory.context=n;n->memory.quiescent=quiescent;
    n->runtime_base=runtime_base;n->runtime_physical=pmm_alloc_contig(runtime_bytes/4096);
    if(!n->runtime_physical) goto failed;
    n->runtime_bytes=runtime_bytes;mm_claim(n->runtime_physical,runtime_bytes/4096,MM_NET);
    n->runtime_pin=(struct mm_pin){.first_pfn=n->runtime_physical/4096,.pages=runtime_bytes/4096,
        .generation=generation,.address_mask=UINT32_MAX,.alignment=4096,.direction=MM_PIN_CPU};
    if(mm_pin(&n->runtime_pin)) goto failed;
    memset(P2V(n->runtime_physical),0,runtime_bytes);
    for(;n->runtime_mapped<runtime_bytes;n->runtime_mapped+=4096)
        if(as_map_borrowed(&t->as,runtime_base+n->runtime_mapped,n->runtime_physical+n->runtime_mapped,PTE_U|PTE_W)) goto failed;
    memcpy(P2V(n->runtime_physical),ndis_dispatch_start,dispatch_bytes);
    if(!ndis_nt_veneers((uint8_t *)P2V(n->runtime_physical)+256,NDIS_NT_EXPORTS*32,runtime_base+256,
        runtime_base+8192,n->nt_exports,NDIS_NT_EXPORTS,&n->memory.export_count)) goto failed;
    unsigned ndis_count=0;
    if(!ndis_veneers((uint8_t *)P2V(n->runtime_physical)+256+NDIS_NT_EXPORTS*32,
        8192-256-NDIS_NT_EXPORTS*32,runtime_base+256+NDIS_NT_EXPORTS*32,
        n->nt_exports+n->memory.export_count,NDIS_SERVICE_COUNT,&ndis_count)) goto failed;
    n->memory.export_count+=ndis_count;
    n->memory.exports=n->nt_exports;
    if(as_seal_borrowed(&t->as,runtime_base,n->runtime_physical) ||
       as_seal_borrowed(&t->as,runtime_base+4096,n->runtime_physical+4096) ||
       as_seal_borrowed(&t->as,runtime_base+8192,n->runtime_physical+8192)) goto failed;
    struct ntimage_platform platform;ntimage_user_platform(&platform,&n->memory);
    if(!ntimage_load(image,file,bytes,name,&platform)) goto failed;
    if(image_base<runtime_base+runtime_bytes && runtime_base<image_base+image->pe.size) goto failed;
    n->host.dispatcher=runtime_base;n->host.stack_bytes=4096;
    for(unsigned i=0;i<NDIS_HOST_DEPTH;i++) n->host.dispatch_stack[i]=runtime_base+12288+(i+1)*4096;
    n->shared_address=runtime_base+12288+stacks;
    return 1;
failed:
    (void)ndis_native_discard(n);return 0;
}

extern int copy_user(void *,const void *,uint32_t);
static int nt_read(void *ctx,uint32_t address,void *out,uint32_t bytes)
{
    struct ndis_native *n=ctx;
    if(!n->task || !as_range_ok(&n->task->as,address,bytes,false)) return 0;
    uint8_t *p=out;
    while(bytes) {
        uint32_t flags=0,physical=as_lookup(&n->task->as,address,&flags);
        if(!physical || physical>=0x30000000 || !(flags&PTE_U) || (flags&(PTE_PCD|PTE_PWT)) || !ledger_page_pinned(physical/4096)) return 0;
        unsigned count=4096-(address&4095);if(count>bytes) count=bytes;
        memcpy(p,P2V(physical),count);p+=count;address+=count;bytes-=count;
    }
    return 1;
}
static int nt_write(void *ctx,uint32_t address,const void *in,uint32_t bytes)
{
    struct ndis_native *n=ctx;
    if(!n->task || !as_range_ok(&n->task->as,address,bytes,true)) return 0;
    const uint8_t *p=in;
    while(bytes) {
        uint32_t flags=0,physical=as_lookup(&n->task->as,address,&flags);
        if(!physical || physical>=0x30000000 || (flags&(PTE_U|PTE_W))!=(PTE_U|PTE_W) ||
           (flags&(PTE_PCD|PTE_PWT)) || !ledger_page_pinned(physical/4096)) return 0;
        unsigned count=4096-(address&4095);if(count>bytes) count=bytes;
        memcpy(P2V(physical),p,count);p+=count;address+=count;bytes-=count;
    }
    return 1;
}
static uint64_t nt_now(void *ctx)
{ (void)ctx;uint64_t us=ndis_execution_now();return us>UINT64_MAX/10?UINT64_MAX:us*10; }
static uint64_t nt_wall(void *ctx)
{
    (void)ctx;struct clock_seed seed;file_clock_snapshot(&seed);
    if(!seed.valid || seed.utc<0 || (uint64_t)seed.utc>UINT64_MAX/10000000-11644473600ull) return 0;
    uint64_t base=((uint64_t)seed.utc+11644473600ull)*10000000;
    uint64_t delta=g_ticks>=seed.tick?g_ticks-seed.tick:0;
    return delta>(UINT64_MAX-base)/10000?UINT64_MAX:base+delta*10000;
}
static int nt_enqueue(void *ctx,uint32_t fn,unsigned argc,const uint32_t *args,uint32_t *token)
{ return ndis_native_enqueue(ctx,NDIS_HOST_DPC,fn,argc,args,ndis_execution_now(),token); }
static bool nt_cancel(void *ctx,uint32_t token)
{
    struct ndis_native *n=ctx;
    if(!ndis_host_cancel_queued(&n->host,n->host.generation,token)) return false;
    for(unsigned i=0;i<NDIS_RING_SLOTS;i++) if(n->callback_token[i]==token) n->callback_token[i]=0;
    return true;
}
static bool nt_wait(void *ctx,bool (*predicate)(void *),void *argument,uint64_t due)
{
    struct ndis_native *n=ctx;uint32_t flags=read_eflags();
    if(n->host.depth && !predicate(argument)) {
        /* A synchronous kernel wait cannot dispatch user ISR/DPC work on
         * this same thread. The f6-06 user wait bridge must yield through a
         * user continuation; never pretend a self-dependent wait succeeded
         * or report an early timeout. No selected NT export calls this yet. */
        ndis_host_stop(&n->host,"user-wait-continuation-required",0,0,ndis_execution_now());return false;
    }
    uint64_t current=nt_now(n),delta=due>current?due-current:0;
    uint64_t ms=delta/10000+(delta%10000!=0);
    uint64_t ticks=due==UINT64_MAX || ms>UINT64_MAX-g_ticks?UINT64_MAX:g_ticks+ms;
    irq_restore(flags|0x200u);
    bool result=kwait_wait_until(&n->nt_wait,predicate,argument,ticks);
    irq_restore(flags);return result;
}
static void nt_wake(void *ctx)
{ struct ndis_native *n=ctx;kwait_wake_irq(&n->nt_wait,false);wake_host(n); }
static int nt_stall(void *ctx,uint32_t us)
{
    (void)ctx;if(us>1000) return -EINVAL;
    uint32_t flags=read_eflags();irq_restore(flags|0x200u);int result=udelay(us);irq_restore(flags);return result;
}
static uint32_t nt_port(void *ctx,uint32_t address,unsigned width,bool write,uint32_t value,bool *ok)
{
    struct ndis_native *n=ctx;*ok=false;
    if(!ndis_host_live(&n->host) || address>65535 || width>65536-address) return 0;
    for(unsigned i=0;i<n->host.port_count;i++) if(address>=n->host.ports[i].first &&
        address+width<=n->host.ports[i].end) {
        *ok=true;
        if(write) { if(width==1) outb((uint16_t)address,(uint8_t)value);
            else if(width==2) cpu_outw((uint16_t)address,(uint16_t)value);else outl((uint16_t)address,value);return 0; }
        return width==1?inb((uint16_t)address):width==2?cpu_inw((uint16_t)address):inl((uint16_t)address);
    }
    return 0;
}
static bool nt_mmio(void *ctx,uint32_t address,unsigned width,uint32_t value)
{
    struct ndis_native *n=ctx;
    if(!ndis_host_live(&n->host) || (address&(width-1))) return false;
    for(unsigned i=0;i<NDIS_HOST_MAPS;i++) {
        const struct ndis_host_map *m=&n->host.mappings[i];
        if(m->live && m->kind==NDIS_HOST_BAR && address>=m->va && address-m->va<m->bytes && width<=m->bytes-(address-m->va)) {
            if(width==1) *(volatile uint8_t *)(uintptr_t)address=(uint8_t)value;
            else if(width==2) *(volatile uint16_t *)(uintptr_t)address=(uint16_t)value;
            else *(volatile uint32_t *)(uintptr_t)address=value;
            __asm__ volatile("" ::: "memory");return true;
        }
    }
    return false;
}
static int nt_start(struct ndis_native *n)
{
    kwait_init(&n->nt_wait);
    struct ndis_nt_ops ops={n,nt_read,nt_write,nt_now,nt_wall,nt_enqueue,nt_cancel,nt_wait,nt_wake};
    struct clock_seed seed;file_clock_snapshot(&seed);
    if(!seed.valid || !seed.qualified) ops.system_time=0;
    uint32_t offset=n->runtime_bytes-NT_POOL_BYTES;
    if(!ndis_nt_init(&n->nt,&n->host,&ops,n->runtime_base+offset,NT_POOL_BYTES,&n->runtime_pin,offset)) return 0;
    n->host.nt=&n->nt;n->services=(struct ndis_nt_services){.runtime=&n->nt,.context=n,
        .port=nt_port,.mmio=nt_mmio,.stall=nt_stall};return 1;
}
void ndis_native_tick(struct ndis_native *n)
{
    if(!n || !n->runtime_physical || !ndis_host_live(&n->host)) return;
    if(n->host.nt && n->miniport.power_phase != 4) ndis_nt_poll(n->host.nt);
    uint32_t *tick=(uint32_t *)((uint8_t *)P2V(n->runtime_physical)+8192);
    uint64_t value=g_ticks;
    __atomic_store_n(tick+2,(uint32_t)(value>>32),__ATOMIC_RELEASE);
    __atomic_store_n(tick,(uint32_t)value,__ATOMIC_RELEASE);
    __atomic_store_n(tick+1,(uint32_t)(value>>32),__ATOMIC_RELEASE);
}
static uint32_t config_address(const struct pci_func *p,unsigned offset)
{ return 0x80000000u|((uint32_t)p->bus<<16)|((uint32_t)p->dev<<11)|((uint32_t)p->fn<<8)|(offset&0xfcu); }
unsigned ndis_native_pci_read(struct ndis_native *n,unsigned offset,void *out,unsigned bytes)
{
    if(!n || !out || !bytes || offset>=256 || bytes>256-offset || !ndis_host_live(&n->host)) return 0;
    uint8_t *p=out;unsigned done=0;
    while(done<bytes) {
        uint32_t flags=irq_save();outl(0xcf8,config_address(&n->pci,0));
        if(inl(0xcfc)!=((uint32_t)n->pci.device<<16|n->pci.vendor)) { irq_restore(flags);break; }
        unsigned at=offset+done;outl(0xcf8,config_address(&n->pci,at));uint32_t word=inl(0xcfc);
        unsigned count=4-(at&3);if(count>bytes-done) count=bytes-done;
        for(unsigned i=0;i<count;i++) p[done+i]=(uint8_t)(word>>(((at&3)+i)*8));
        irq_restore(flags);done+=count;
    }
    return done;
}
unsigned ndis_native_pci_write(struct ndis_native *n,unsigned offset,const void *in,unsigned bytes)
{
    /* The driver cannot bypass the host's command/DMA lifecycle, size BARs,
     * acknowledge common PCI W1C status or change routing. */
    if(!n || !in || !bytes || bytes>32 || offset<0x40 || offset>=256 || bytes>256-offset || !ndis_host_live(&n->host)) return 0;
    for(unsigned i=0;i<bytes;i++) if(!(n->qualification->config_write[(offset+i)/32]&(1u<<((offset+i)%32)))) return 0;
    unsigned done=0;const uint8_t *p=in;
    while(done<bytes) {
        uint32_t flags=irq_save();outl(0xcf8,config_address(&n->pci,0));
        if(inl(0xcfc)!=((uint32_t)n->pci.device<<16|n->pci.vendor)) { irq_restore(flags);break; }
        outl(0xcf8,config_address(&n->pci,offset+done));outb((uint16_t)(0xcfc+((offset+done)&3)),p[done]);
        irq_restore(flags);done++;
    }
    return done;
}
bool ndis_native_pci_intx_mask(struct ndis_native *n,bool masked)
{
    if(!n || n->pci.dev>=32 || n->pci.fn>=8) return false;
    uint32_t flags=irq_save();outl(0xcf8,config_address(&n->pci,0));
    if(inl(0xcfc)!=((uint32_t)n->pci.device<<16|n->pci.vendor)) { irq_restore(flags);return false; }
    outl(0xcf8,config_address(&n->pci,4));uint16_t command=cpu_inw(0xcfc);
    uint16_t next=masked?(uint16_t)(command|0x400u):(uint16_t)(command&~0x400u);
    cpu_outw(0xcfc,next);bool ok=cpu_inw(0xcfc)==next;irq_restore(flags);return ok;
}
int ndis_native_pci_intx_cause(struct ndis_native *n)
{
    if(!n) return -1;
    uint32_t flags=irq_save();outl(0xcf8,config_address(&n->pci,0));
    if(inl(0xcfc)!=((uint32_t)n->pci.device<<16|n->pci.vendor)) { irq_restore(flags);return -1; }
    outl(0xcf8,config_address(&n->pci,4));uint16_t status=cpu_inw(0xcfe);irq_restore(flags);
    return status==0xffff?-1:!!(status&8u);
}
bool ndis_native_no_idle_proof(struct ndis_native *n)
{ (void)n;return false; /* the independent PCI fence still runs; all pins stay held */ }
/* Intel 8255x manual 6.3.2 and 8.4: SCB M is independent of the CU/RU
 * command byte. Read causes without acknowledging them before the ISR. */
static uint16_t e100_port(struct ndis_native *n)
{
    if(!n || !n->qualification || n->pci.vendor!=0x8086 || n->pci.device!=0x1229 ||
       !(n->pci.bar[1]&1) || n->qualification->bar_bytes[1]<12) return 0;
    uint32_t base=n->pci.bar[1]&~3u;
    if(!base || base>65524) return 0;
    for(unsigned i=0;i<n->port_count;i++) {
        const struct resource *r=registry_get((unsigned)n->port_claim[i]);
        if(r && r->type==RES_PORT && r->state==RS_ACTIVE && r->owner==n->owner &&
           r->start==base && r->end-base==n->qualification->bar_bytes[1] &&
           registry_valid(n->port_claim[i],n->port_generation[i])) return (uint16_t)base;
    }
    return 0;
}
bool ndis_native_e100_mask(struct ndis_native *n,bool masked)
{
    uint16_t base=e100_port(n);if(!base) return false;
    uint8_t before=inb((uint16_t)(base+3));if(before==0xff) return false;
    outb((uint16_t)(base+3),(uint8_t)((before&0xfcu)|(masked?1u:0u)));
    uint8_t after=inb((uint16_t)(base+3));return after!=0xff && !!(after&1)==masked;
}
int ndis_native_e100_cause(struct ndis_native *n)
{
    uint16_t base=e100_port(n);if(!base) return -1;
    uint8_t status=inb((uint16_t)(base+1));return status==0xff?-1:!!(status&0xfcu);
}
