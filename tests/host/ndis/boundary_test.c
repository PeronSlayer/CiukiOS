/* SPDX-License-Identifier: GPL-2.0-only
 * Native-width trap-frame/state model. This does not execute i386 recovery. */
#include <assert.h>
#include <stdio.h>
#include "src/kernel/ndis/execution.h"
static struct ndis_execution *slot;
static struct ndis_execution outer_frame, inner_frame;
static struct ndis_callback callback;
static struct ntimage image;
static unsigned irq_depth, fences;
struct ndis_execution **ndis_execution_slot(void) { return &slot; }
unsigned ndis_execution_irq_depth(void) { return irq_depth; }
int ndis_context_save(struct ndis_saved_context *p) { (void)p; assert(0); return 0; }
void ndis_context_recover(void) { assert(0); }
bool registry_valid(int h,gen_t g) { (void)h;(void)g;return true; }
int registry_quarantine(int h,gen_t g) { (void)h;(void)g;return 0; }
static bool fence(void *p,const char *why) { (void)p;assert(why);fences++;return false; }
static void fresh(uint8_t level) {
    image=(struct ntimage){.loaded=true,.scanned=true,.pe={.base=0xc1000000,.size=0x1000,
        .section_count=1,.sections={{.span=0x1000,.flags=0x20000000}}}};
    callback=(struct ndis_callback){.image=&image,.dirql=5,.ops={.fence=fence}};
    assert(ndis_callback_begin(&callback,0xc1000000,level,100,50));
    outer_frame=(struct ndis_execution){.callback=&callback};
    inner_frame=(struct ndis_execution){.callback=&callback,.parent=&outer_frame};
    slot=&outer_frame; irq_depth=0;
}
static struct trap_frame frame(unsigned vector) {
    return (struct trap_frame){.vector=vector,.cs=8,.eip=0xc1000123,.eflags=0x700};
}
static void redirected(const struct trap_frame *tf,const struct ndis_execution *e) {
    assert(tf->eip==(uint32_t)(uintptr_t)ndis_context_recover);
    assert(tf->eax==(uint32_t)(uintptr_t)&e->saved && !(tf->eflags&0x700));
    assert(image.quarantined && !callback.depth && !callback.irql && !callback.fence_idle);
}
int main(void) {
    unsigned vectors[]={0,6,14};
    for(unsigned i=0;i<3;i++) for(unsigned shim=0;shim<2;shim++) {
        fresh(0); struct trap_frame tf=frame(vectors[i]);
        if(shim) { assert(ndis_shim_enter()); tf.eip=0xc2000123; }
        unsigned before=fences;
        assert(ndis_execution_exception(&tf)); redirected(&tf,&outer_frame);
        assert(fences==before+1 && callback.fault_vector==vectors[i] && callback.fault_in_shim==(bool)shim);
    }
    fresh(0); assert(ndis_callback_begin(&callback,0xc1000000,5,101,30));
    slot=&inner_frame; struct trap_frame tf=frame(6);
    assert(ndis_execution_exception(&tf)); redirected(&tf,&outer_frame);
    /* Preserve the native IRQ's epilogue: inner recovery first, then outer
     * recovery at the IRQ return hook after EOI and accounting. */
    fresh(0); assert(ndis_callback_begin(&callback,0xc1000000,5,101,30));
    inner_frame.irq_depth=1; slot=&inner_frame; irq_depth=1; tf=frame(14);
    assert(ndis_execution_exception(&tf)); redirected(&tf,&inner_frame);
    slot=&outer_frame; irq_depth=0; tf=frame(0x2b);
    ndis_execution_tick(&tf,200); redirected(&tf,&outer_frame);
    for(unsigned i=0;i<3;i++) {
        fresh(i==0?0:i==1?2:5); tf=frame(0x20);
        ndis_execution_tick(&tf,149); assert(!image.quarantined && tf.eip==0xc1000123);
        ndis_execution_tick(&tf,150); redirected(&tf,&outer_frame);
    }
    fresh(0); assert(ndis_shim_enter()); tf=frame(0x20); tf.eip=0xc2000123;
    ndis_execution_tick(&tf,150);
    assert(callback.timeout_pending && !image.quarantined && callback.depth==1 && tf.eip==0xc2000123);
    /* No native host service is abandoned by a timer inside that service. */
    fresh(0); assert(ndis_shim_enter()); tf=frame(14); tf.eip=0xc2000123;
    irq_depth=1; assert(!ndis_execution_exception(&tf)); irq_depth=0;
    ndis_host_critical_enter(); assert(!ndis_execution_exception(&tf)); ndis_host_critical_leave();
    tf.vector=2; assert(!ndis_execution_exception(&tf)); tf.vector=8; assert(!ndis_execution_exception(&tf));
    tf.vector=18; assert(!ndis_execution_exception(&tf)); tf.vector=14; tf.cs=3;
    assert(!ndis_execution_exception(&tf)); tf.cs=8; ndis_shim_leave();
    assert(!ndis_execution_exception(&tf) && !image.quarantined);
    slot=0; int32_t result=0;
    assert(!ndis_execute2(0,0,0,0,0,NDIS_PASSIVE,&result) && result==-1);
    puts("PASS recovery frame model image/shim faults nested IRQ epilogues deadlines and host exclusions");
    fflush(stdout);
}
