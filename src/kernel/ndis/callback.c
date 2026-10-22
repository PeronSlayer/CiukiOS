/* SPDX-License-Identifier: GPL-2.0-only */
#include "callback.h"
static void trace(struct ndis_callback *c,const char *event,const char *reason) {
    if(c->ops.record) c->ops.record(c->ops.context,event,reason,c->irql);
}
void ndis_callback_fault(struct ndis_callback *c,const char *reason) {
    if(c->state==NDIS_CALLBACK_QUARANTINED) return;
    c->state=NDIS_CALLBACK_QUARANTINED;
    if(c->image) c->image->quarantined=true;
    /* Fence first; no mutex, foreign Halt, native netif callback, or freeing.
     * Existing registry quarantine permanently denies reuse of exact claims. */
    c->fence_idle=c->ops.fence && c->ops.fence(c->ops.context,reason);
    trace(c,"fault",reason);
    unsigned n=c->claim_count;
    if(n>NDIS_CALLBACK_CLAIMS) n=NDIS_CALLBACK_CLAIMS;
    for(unsigned i=0;i<n;i++) registry_quarantine(c->claims[i],c->generations[i]);
    trace(c,"quarantine",c->fence_idle?"fence-idle-proved":"fence-idle-unproved-pins-retained");
}
int ndis_callback_begin(struct ndis_callback *c,uint32_t address,uint8_t level,uint64_t now,uint64_t budget) {
    if(c->state==NDIS_CALLBACK_QUARANTINED) return 0;
    if(!c->image || !c->image->loaded || !ntimage_executable(c->image,address) ||
       !c->ops.fence || c->claim_count>NDIS_CALLBACK_CLAIMS || !c->dirql || c->dirql<=2 ||
       c->depth==NDIS_CALLBACK_DEPTH || !budget || now>UINT64_MAX-budget ||
       (level!=0 && level!=2 && level!=c->dirql) || level<c->irql ||
       (c->depth && level!=c->dirql) || (c->depth && c->irql==c->dirql)) {
        ndis_callback_fault(c,"callback-admission"); return 0;
    }
    for(unsigned i=0;i<c->claim_count;i++) if(!registry_valid(c->claims[i],c->generations[i])) {
        ndis_callback_fault(c,"stale-claim"); return 0;
    }
    c->stack[c->depth++]=(struct ndis_callback_frame){c->irql,level,now+budget};
    c->irql=level; c->state=NDIS_CALLBACK_ACTIVE; c->image->entered=true;
    trace(c,"entry",0); return 1;
}
int ndis_callback_end(struct ndis_callback *c) {
    if(c->state==NDIS_CALLBACK_QUARANTINED) return 0;
    if(!c->depth || c->irql!=c->stack[c->depth-1].level) {
        ndis_callback_fault(c,"callback-irql-unbalanced"); return 0;
    }
    trace(c,"exit",0); c->irql=c->stack[--c->depth].previous;
    c->state=c->depth?NDIS_CALLBACK_ACTIVE:NDIS_CALLBACK_IDLE;
    return 1;
}
void ndis_callback_watchdog(struct ndis_callback *c,uint64_t now) {
    if(c->state!=NDIS_CALLBACK_ACTIVE) return;
    for(unsigned i=0;i<c->depth;i++) if(now>=c->stack[i].deadline) {
        ndis_callback_fault(c,"callback-nonreturn-timeout"); return;
    }
}
int ndis_irql_raise(struct ndis_callback *c,uint8_t level,uint8_t *old) {
    if(c->state==NDIS_CALLBACK_QUARANTINED) return 0;
    /* No arbitrary PIC priority emulation: DIRQL transitions require the
     * external short IRQ-save synchronization boundary, not this model. */
    if(!old || !c->depth || level<c->irql || level>2) {
        ndis_callback_fault(c,"illegal-irql-raise"); return 0;
    }
    *old=c->irql; c->irql=level; return 1;
}
int ndis_irql_lower(struct ndis_callback *c,uint8_t level) {
    if(c->state==NDIS_CALLBACK_QUARANTINED) return 0;
    if(!c->depth || level>c->irql || level<c->stack[c->depth-1].level || level>2) {
        ndis_callback_fault(c,"illegal-irql-lower"); return 0;
    }
    c->irql=level; return 1;
}
int ndis_irql_wait(struct ndis_callback *c) {
    if(c->state==NDIS_CALLBACK_QUARANTINED) return 0;
    if(!c->depth || c->irql>1) { ndis_callback_fault(c,"wait-at-raised-irql"); return 0; }
    return 1;
}
#ifdef CIUKI_NDIS_HOST_MODEL
int ndis_callback_model_call2(struct ndis_callback *c,uint32_t address,ndis_callback2 function,
                            uint32_t a,uint32_t b,uint8_t level,uint64_t now,uint64_t budget,int32_t *result) {
    if(!function || !result || !ndis_callback_begin(c,address,level,now,budget)) return 0;
    *result=function(a,b);
    return ndis_callback_end(c);
}
#endif
