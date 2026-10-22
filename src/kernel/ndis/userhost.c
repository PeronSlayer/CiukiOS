/* SPDX-License-Identifier: GPL-2.0-only */
#include "userhost.h"
#include <ciuki/kernel.h>
static uint64_t budget(enum ndis_host_kind k)
{
    switch(k) {
    case NDIS_HOST_ISR:return 5000;
    case NDIS_HOST_DPC:return 50000;
    case NDIS_HOST_PASSIVE:return 500000;
    case NDIS_HOST_LIFECYCLE:return 10000000;
    }
    return 0;
}
static uint64_t deadline(uint64_t now,uint64_t us)
{ return now>UINT64_MAX-us?UINT64_MAX:now+us; }
bool ndis_user_host_available(void) { return false; }
bool ndis_host_live(const struct ndis_user_host *h)
{ return h && h->image && h->ops.owned && h->state==NDIS_HOST_LIVE && h->generation &&
    !h->image->quarantined && h->ops.owned(h->ops.context,h->generation); }
int ndis_host_init(struct ndis_user_host *h,struct ntimage *image,uint32_t generation,
                    const struct ndis_host_ops *ops,bool shared,bool masker,uint8_t dirql)
{
#ifndef CIUKI_NDIS_HOST_MODEL
    (void)h;(void)image;(void)generation;(void)ops;(void)shared;(void)masker;(void)dirql;
    return 0; /* closed before consulting any callbacks */
#else
    if(!h || h->state!=NDIS_HOST_NEW || !image || !image->loaded ||
       !image->platform.user_host || image->quarantined || !generation || !ops ||
       !ops->owned || !ops->mapping || !ops->map || !ops->mask || !ops->cause ||
       !ops->notify || !ops->fence || !ops->detach || !ops->retain || !ops->cancel ||
       !ops->record || dirql<=2 || (shared && !masker) || !ops->owned(ops->context,generation)) return 0;
    h->image=image; h->generation=generation; h->ops=*ops; h->dirql=dirql;
    h->virtual_if=true; h->state=NDIS_HOST_LIVE;
    return 1;
#endif
}
void ndis_host_stop(struct ndis_user_host *h,const char *why,uint32_t vector,uint32_t eip,uint64_t now)
{
    if(!h || h->state!=NDIS_HOST_LIVE) return;
    h->state=NDIS_HOST_FENCING; /* no admission, deliveries or user return */
    h->retired_generation=h->generation; h->generation=0;
    h->image->quarantined=true; h->port_count=0;
    h->fault_vector=vector;
    h->fault_offset=eip>=h->image->pe.base && eip-h->image->pe.base<h->image->pe.size ?
        eip-h->image->pe.base:UINT32_MAX;
    h->irq_masked=h->ops.mask(h->ops.context,true);
    h->idle_proved=h->ops.fence(h->ops.context);
    h->ops.detach(h->ops.context,h->retired_generation);
    /* Retain precedes revocation and ordinary reaping, even when the device
     * stop claims idle. The boot quarantine owner never points into user RAM. */
    h->ops.retain(h->ops.context);
    for(unsigned i=0;i<NDIS_HOST_MAPS;i++) if(h->mappings[i].live) {
        if(!h->ops.map(h->ops.context,&h->mappings[i],false)) h->mappings[i].live=false;
    }
    h->count=h->depth=0; h->irq_pending=false; h->irq_token=0;
    h->irql=0; h->synchronized=false;
    h->cancel_deadline=deadline(now,NDIS_HOST_TIMER_US);
    h->state=NDIS_HOST_QUARANTINED;
    h->ops.record(h->ops.context,why,vector,h->fault_offset);
    h->ops.cancel(h->ops.context); /* fence and retain are complete first */
}
void ndis_host_watchdog(struct ndis_user_host *h,uint64_t now)
{
    if(!h) return;
    if(h->state==NDIS_HOST_QUARANTINED) {
        /* Never destroy a suspended kernel continuation. A stalled native
         * operation retains its stack/backing and is diagnosed once. */
        if(h->syscall_active && now>=h->cancel_deadline && !h->cancellation_stalled) {
            h->cancellation_stalled=true;
            h->ops.record(h->ops.context,"native-cancellation-stalled",0,UINT32_MAX);
        }
        return;
    }
    if(h->state!=NDIS_HOST_LIVE) return;
    if(!ndis_host_live(h)) { ndis_host_stop(h,"ownership-revoked",0,0,now); return; }
    bool expired=h->irq_pending && now>=h->irq_deadline;
    for(unsigned i=0;i<h->depth;i++) expired|=now>=h->active[i].deadline;
    for(unsigned i=0;i<h->count;i++) expired|=now>=h->queue[(h->head+i)%NDIS_HOST_QUEUE].deadline;
    if(expired) ndis_host_stop(h,"callback-timeout",0,0,now);
}
int ndis_host_mapping(struct ndis_user_host *h,const struct ndis_host_map *m)
{
    if(!ndis_host_live(h) || !m || m->generation!=h->generation || !m->bytes ||
       (m->bytes&4095) || (m->va&4095) || (m->physical&4095) || m->va<0x10000 ||
       m->va>=0xc0000000 || m->bytes>0xc0000000-m->va ||
       m->bytes-1>UINT32_MAX-m->physical ||
       (m->kind!=NDIS_HOST_BAR && m->kind!=NDIS_HOST_DMA) ||
       (m->kind==NDIS_HOST_BAR && !m->uncached) ||
       (m->kind==NDIS_HOST_DMA && !m->ticket) || !h->ops.mapping(h->ops.context,m)) return 0;
    unsigned slot=NDIS_HOST_MAPS;
    for(unsigned i=0;i<NDIS_HOST_MAPS;i++) {
        const struct ndis_host_map *a=&h->mappings[i];
        if(!a->live) { if(slot==NDIS_HOST_MAPS) slot=i; continue; }
        if(m->va<a->va+a->bytes && a->va<m->va+m->bytes) return 0;
        /* Do not create conflicting cache aliases of the same physical page. */
        if((uint64_t)m->physical<(uint64_t)a->physical+a->bytes &&
           (uint64_t)a->physical<(uint64_t)m->physical+m->bytes && m->uncached!=a->uncached) return 0;
    }
    if(slot==NDIS_HOST_MAPS || h->ops.map(h->ops.context,m,true)) return 0;
    h->mappings[slot]=*m; h->mappings[slot].live=true; return 1;
}
void ndis_host_ports(const struct ndis_user_host *h,uint8_t bitmap[ARCH_IO_BYTES])
{
    bool live=ndis_host_live(h);
    (void)arch_io_bitmap(bitmap,live?h->ports:0,live?h->port_count:0);
}
int ndis_host_enqueue(struct ndis_user_host *h,enum ndis_host_kind kind,uint32_t address,
                       uint32_t argument,uint64_t now,uint32_t *token)
{
    if(!ndis_host_live(h) || !budget(kind) || !ntimage_executable(h->image,address) ||
       h->count==NDIS_HOST_QUEUE || h->next_token==UINT32_MAX) return 0;
    uint32_t id=++h->next_token;
    struct ndis_host_event *e=&h->queue[(h->head+h->count)%NDIS_HOST_QUEUE];
    *e=(struct ndis_host_event){id,address,argument,h->generation,kind,deadline(now,budget(kind))};
    h->count++; if(token) *token=id; h->ops.notify(h->ops.context); return 1;
}
int ndis_host_take(struct ndis_user_host *h,uint64_t now,struct ndis_host_event *event)
{
    ndis_host_watchdog(h,now);
    if(!ndis_host_live(h) || !event || !h->count || h->depth==NDIS_HOST_DEPTH) return 0;
    unsigned pick=h->head;
    if(h->depth) {
        if(!h->virtual_if || h->synchronized || h->irql>=h->dirql) return 0;
        /* Only an ISR preempts an ordinary callback on UP. */
        unsigned i=0;
        for(;i<h->count;i++) if(h->queue[(h->head+i)%NDIS_HOST_QUEUE].kind==NDIS_HOST_ISR) break;
        if(i==h->count) return 0;
        pick=(h->head+i)%NDIS_HOST_QUEUE;
    }
    struct ndis_host_event e=h->queue[pick];
    if(e.kind==NDIS_HOST_ISR && (!h->virtual_if || h->synchronized)) return 0;
    /* Compact the bounded queue; ordinary order and ISR order are preserved. */
    while(pick!=h->head) {
        unsigned prev=(pick+NDIS_HOST_QUEUE-1)%NDIS_HOST_QUEUE;
        h->queue[pick]=h->queue[prev]; pick=prev;
    }
    h->head=(h->head+1)%NDIS_HOST_QUEUE; h->count--;
    if(h->depth && e.deadline>h->active[0].deadline) e.deadline=h->active[0].deadline;
    h->active[h->depth++]=e;
    h->irql=e.kind==NDIS_HOST_ISR?h->dirql:e.kind==NDIS_HOST_DPC?2:0;
    *event=e; return 1;
}
int ndis_host_complete(struct ndis_user_host *h,uint32_t generation,uint32_t token,uint64_t now)
{
    ndis_host_watchdog(h,now);
    if(!ndis_host_live(h) || generation!=h->generation || !h->depth ||
       h->active[h->depth-1].token!=token || h->synchronized) return 0;
    h->depth--;
    h->irql=!h->depth?0:h->active[h->depth-1].kind==NDIS_HOST_ISR?h->dirql:
        h->active[h->depth-1].kind==NDIS_HOST_DPC?2:0;
    return 1;
}
int ndis_host_deliver(struct ndis_user_host *h,struct trap_frame *tf,uint64_t now)
{
    if(!h || !tf || (tf->cs&3)!=3 || (tf->eflags&0x20000) ||
       h->dispatcher<0x10000 || h->dispatcher>=0xc0000000 ||
       !h->stack_bytes || h->depth>=NDIS_HOST_DEPTH) return 0;
    unsigned depth=h->depth;
    uint32_t top=h->dispatch_stack[depth];
    if(top<0x10000 || top>0xc0000000 || (top&15) || h->stack_bytes>top-0x10000) return 0;
    struct ndis_host_event e;
    if(!ndis_host_take(h,now,&e)) return 0;
    h->continuation[depth]=*tf; h->continuation_valid[depth]=true;
    tf->eip=h->dispatcher; tf->user_esp=top;
    tf->eax=e.address; tf->ebx=e.argument; tf->ecx=e.token; tf->edx=e.generation;
    tf->esi=(uint32_t)e.kind;
    tf->eflags=(tf->eflags&~(0x3000u|0x20000u|0x4000u|0x100u))|0x202u;
    return 1;
}
int ndis_host_return_event(struct ndis_user_host *h,struct trap_frame *tf,uint32_t generation,
                           uint32_t token,uint64_t now)
{
    if(!h || !tf || !h->depth || !h->continuation_valid[h->depth-1]) return 0;
    unsigned depth=h->depth-1;
    if(!ndis_host_complete(h,generation,token,now)) return 0;
    *tf=h->continuation[depth];h->continuation_valid[depth]=false;
    return 1;
}
int ndis_host_interrupt(struct ndis_user_host *h,uint64_t now)
{
    if(!ndis_host_live(h)) return 0;
    if(h->irq_pending) return 1; /* coalesce without extending the deadline */
    if(h->irq_sequence==UINT32_MAX || !h->ops.mask(h->ops.context,true)) {
        ndis_host_stop(h,"source-mask-failed",0,0,now); return 0;
    }
    h->irq_masked=true; h->irq_pending=true; h->irq_token=++h->irq_sequence;
    h->irq_deadline=deadline(now,budget(NDIS_HOST_ISR));
    h->ops.notify(h->ops.context); return 1;
}
int ndis_host_rearm(struct ndis_user_host *h,uint32_t generation,uint32_t token,uint64_t now)
{
    ndis_host_watchdog(h,now);
    if(!ndis_host_live(h) || generation!=h->generation || !h->irq_pending ||
       token!=h->irq_token || h->synchronized || !h->virtual_if) return 0;
    int cause=h->ops.cause(h->ops.context);
    if(cause<0) { ndis_host_stop(h,"pending-recheck-failed",0,0,now); return 0; }
    if(cause) { h->ops.notify(h->ops.context); return 1; } /* retain original deadline */
    if(!h->ops.mask(h->ops.context,false)) { ndis_host_stop(h,"source-rearm-failed",0,0,now); return 0; }
    h->irq_pending=false; h->irq_masked=false;
    /* Closing the unmask race is a native operation, not a user assertion. */
    cause=h->ops.cause(h->ops.context);
    if(cause<0) { ndis_host_stop(h,"pending-recheck-failed",0,0,now); return 0; }
    if(cause && !ndis_host_interrupt(h,now)) return 0;
    return 1;
}
int ndis_host_gp(struct ndis_user_host *h,struct trap_frame *tf,uint8_t opcode)
{
    if(!ndis_host_live(h) || !tf || tf->vector!=13 || tf->err ||
       (tf->cs&3)!=3 || (tf->eflags&0x20000) || tf->eip>=0xbfffffff) return 0;
    if(opcode==0xfa) h->virtual_if=false;
    else if(opcode==0xfb) h->virtual_if=true;
    else if(opcode!=0xf4) return 0;
    tf->eip++; tf->eflags=(tf->eflags&~0x3000u)|0x202u;
    if(opcode==0xfb) h->ops.notify(h->ops.context);
    return opcode==0xf4?2:1; /* native caller yields; timer remains enabled */
}
uint32_t ndis_host_flags(const struct ndis_user_host *h,uint32_t physical)
{ return (physical&~0x3200u)|2u|(h->virtual_if?0x200u:0); }
int ndis_host_restore_flags(struct ndis_user_host *h,uint32_t saved)
{
    if(!ndis_host_live(h) || (saved&(0x3000u|0x20000u))) return 0;
    h->virtual_if=!!(saved&0x200u);
    if(h->virtual_if) h->ops.notify(h->ops.context);
    return 1;
}
