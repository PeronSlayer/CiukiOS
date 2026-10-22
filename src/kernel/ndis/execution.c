/* SPDX-License-Identifier: GPL-2.0-only */
#include "execution.h"
_Static_assert(sizeof(struct ndis_saved_context)==28,"i386 recovery context");
_Static_assert(offsetof(struct ndis_saved_context,eip)==20,"assembly context eip");
static struct ndis_execution *current(void) {
    struct ndis_execution **slot=ndis_execution_slot(); return slot?*slot:0;
}
static bool foreign(const struct ndis_execution *e,uint32_t eip) {
    return e && e->irq_depth==ndis_execution_irq_depth() &&
        !e->critical_depth && (e->shim_depth || ntimage_executable(e->callback->image,eip));
}
static struct ndis_execution *outer(struct ndis_execution *e) {
    while(e->parent && e->parent->callback==e->callback &&
          e->parent->irq_depth==e->irq_depth) e=e->parent;
    return e;
}
static void redirect(struct trap_frame *tf,struct ndis_execution *e) {
    e=outer(e);
    /* IRET to a trampoline rather than trying to change same-CPL ESP via
     * user_esp (which does not exist in a ring-0 interrupt frame). */
    tf->eax=(uint32_t)(uintptr_t)&e->saved;
    tf->eip=(uint32_t)(uintptr_t)ndis_context_recover;
    tf->eflags&=~(0x200u|0x100u|0x400u); /* IF/TF/DF off until saved flags */
}
int ndis_execute2(struct ndis_runtime *r,uint32_t address,ndis_foreign2 fn,
                  uint32_t a,uint32_t b,enum ndis_callback_kind kind,int32_t *result) {
#ifndef CIUKI_NDIS_HOST_MODEL
    /* Part 1 has not acquired executable host evidence in this sandbox.
     * No new thunk may bypass the production entry gate to compensate. */
    (void)r; (void)address; (void)fn; (void)a; (void)b; (void)kind;
    if(result) *result=-1;
    return 0;
#else
    struct ndis_execution **slot=ndis_execution_slot();
    if(!result) return 0;
    *result=-1;
    if(!r || !r->callback || !r->now || !slot || !fn || (unsigned)kind>NDIS_ISR) return 0;
    struct ndis_callback *c=r->callback;
    if(!c->image || !c->image->scanned || c->depth>=NDIS_CALLBACK_DEPTH ||
       (uintptr_t)fn!=address || (*slot && (*slot)->callback!=c)) return 0;
    uint8_t level=kind==NDIS_ISR?c->dirql:kind==NDIS_DPC?2:0;
    struct ndis_execution *e=&r->frames[c->depth];
    *e=(struct ndis_execution){.parent=*slot,.callback=c,.irq_depth=ndis_execution_irq_depth()};
    if(!ndis_callback_begin(c,address,level,r->now(),r->budgets[kind])) return 0;
    if(ndis_context_save(&e->saved)) {
        *slot=e->parent;
        *result=-1;
        return 0;
    }
    if(!(e->saved.flags&0x200)) {
        ndis_callback_fault(c,"callback-interrupts-disabled"); return 0;
    }
    *slot=e;
    *result=fn(a,b);
    __asm__ volatile("cld" ::: "cc");
    /* The return boundary is also a deadline check, not nonreturn evidence. */
    ndis_callback_watchdog(c,r->now());
    int ok=ndis_callback_end(c);
    *slot=e->parent;
    if(!ok) *result=-1;
    return ok;
#endif
}
int ndis_shim_enter(void) {
    struct ndis_execution *e=current();
    if(!e || e->irq_depth!=ndis_execution_irq_depth() || e->critical_depth ||
       e->callback->state!=NDIS_CALLBACK_ACTIVE || e->shim_depth==NDIS_CALLBACK_DEPTH) return 0;
    e->shim_depth++; return 1;
}
void ndis_shim_leave(void) {
    struct ndis_execution *e=current();
    if(!e || !e->shim_depth || e->critical_depth) return;
    e->shim_depth--;
    if(e->shim_depth) return;
    if(e->callback->timeout_pending) ndis_callback_fault(e->callback,"callback-nonreturn-timeout");
    if(e->callback->state==NDIS_CALLBACK_QUARANTINED) {
        e=outer(e);
        /* Same trampoline as the trap path. EAX is the explicit operand. */
        __asm__ volatile("jmp ndis_context_recover" :: "a"(&e->saved) : "memory");
        __builtin_unreachable();
    }
}
void ndis_host_critical_enter(void) { struct ndis_execution *e=current(); if(e) e->critical_depth++; }
void ndis_host_critical_leave(void) { struct ndis_execution *e=current(); if(e && e->critical_depth) e->critical_depth--; }
bool ndis_execution_exception(struct trap_frame *tf) {
    struct ndis_execution *e=current();
    /* NMI, double fault and machine check are not adapter-local faults. */
    if(!tf || (tf->cs&3) || tf->vector>=32 || tf->vector==2 || tf->vector==8 ||
       tf->vector==18 || !foreign(e,tf->eip)) return false;
    struct ndis_callback *c=e->callback;
    c->fault_vector=tf->vector;
    c->fault_offset=tf->eip-c->image->pe.base;
    c->fault_in_shim=!ntimage_executable(c->image,tf->eip);
    ndis_callback_fault(c,"cpu-exception");
    redirect(tf,e); return true;
}
void ndis_execution_tick(struct trap_frame *tf,uint64_t now) {
    struct ndis_execution *e=current();
    if(!tf || (tf->cs&3) || !e || e->irq_depth!=ndis_execution_irq_depth()) return;
    if(e->shim_depth) {
        if(ndis_callback_expired(e->callback,now)) {
            e->callback->timeout_pending=true;
            e->callback->fault_offset=tf->eip-e->callback->image->pe.base;
            e->callback->fault_in_shim=true;
        }
        return;
    }
    if(ndis_callback_expired(e->callback,now)) {
        e->callback->fault_offset=tf->eip-e->callback->image->pe.base;
        e->callback->fault_in_shim=false;
    }
    ndis_callback_watchdog(e->callback,now);
    /* Timeouts inside a shim are deferred to its return boundary. Never
     * abandon an in-flight native service with internal state half updated. */
    if(e->callback->state==NDIS_CALLBACK_QUARANTINED && !e->shim_depth && foreign(e,tf->eip))
        redirect(tf,e);
}
