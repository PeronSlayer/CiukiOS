/* Native scheduler/trap adapter; all foreign code remains at CPL3.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "userhost_kernel.h"
#include <ciuki/cpu.h>
#include <ciuki/kernel.h>
#include <ciuki/timing.h>
#include <ciuki/process.h>
static struct task *hosts[NDIS_HOST_MAX];
extern int copy_user(void *,const void *,uint32_t);
static uint64_t now_us(void)
{
    if(!g_cpu_tsc || !g_tsc_per_ms) return g_ticks*1000u;
    uint64_t cycles=read_tsc_serialized();
    return (cycles/g_tsc_per_ms)*1000u+(cycles%g_tsc_per_ms)*1000u/g_tsc_per_ms;
}
static bool user_resident(const struct task *t,uint32_t base,uint32_t bytes,bool write)
{
    if(!bytes || !as_range_ok(&t->as,base,bytes,write)) return false;
    for(uint32_t at=base&~4095u;at<(uint64_t)base+bytes;at+=4096) {
        uint32_t pte=0,phys=as_lookup(&t->as,at,&pte);
        if(!phys || !ledger_page_pinned(phys/4096) || (pte&(PTE_PCD|PTE_PWT))) return false;
    }
    return true;
}
int ndis_user_attach(struct task *t,struct ndis_user_host *h)
{
    /* This is not a production entry switch. Even a caller with a model-like
     * LIVE record cannot bind a task until the compile-time gate is reviewed. */
    if(!ndis_user_host_available() || !t || !h || !t->user || t->as.memory ||
       proc_thread_for(t) || t->state!=T_BLOCKED || t->ndis_user_host ||
       !ndis_host_live(h) || !user_resident(t,h->dispatcher,1,false) || !h->stack_bytes) return 0;
    for(unsigned i=0;i<NDIS_HOST_DEPTH;i++) {
        uint32_t top=h->dispatch_stack[i];
        if(top<h->stack_bytes || (top&15) ||
           !user_resident(t,top-h->stack_bytes,h->stack_bytes,true)) return 0;
        for(unsigned j=0;j<i;j++) if(top-h->stack_bytes<h->dispatch_stack[j] &&
            h->dispatch_stack[j]-h->stack_bytes<top) return 0;
    }
    uint32_t f=irq_save();
    for(unsigned i=0;i<NDIS_HOST_MAX;i++) if(!hosts[i]) {
        hosts[i]=t; t->ndis_user_host=h; h->image->entered=true;
        irq_restore(f); return 1;
    }
    irq_restore(f); return 0;
}
void ndis_user_switch(const struct task *t)
{
    if(!t || !t->ndis_user_host) return; /* default deny/VGA installed by task.c */
    struct ndis_user_host *h=t->ndis_user_host;
    if(ndis_host_live(h)) arch_io_install(h->ports,h->port_count);
    else arch_io_install(0,0);
}
void ndis_user_task_stopping(struct task *t,int code)
{
    (void)code;
    if(!t || !t->ndis_user_host) return;
    struct ndis_user_host *h=t->ndis_user_host;
    ndis_host_stop(h,"host-exit",0,0,now_us());
    if(t==g_current) arch_io_install(0,0);
}
void ndis_user_reaped(struct task *t)
{
    if(!t || !t->ndis_user_host) return;
    for(unsigned i=0;i<NDIS_HOST_MAX;i++) if(hosts[i]==t) hosts[i]=0;
    /* Quarantine metadata and backing belong to kernel boot storage, not t. */
    t->ndis_user_host=0;
}
void ndis_user_timer(void)
{
    /* At most NETIF_MAX records, including blocked/yielded/queued hosts. This
     * is independent of the current task and of all driver/shim progress. */
    uint64_t now=now_us();
    for(unsigned i=0;i<NDIS_HOST_MAX;i++) if(hosts[i]) {
        struct task *t=hosts[i]; struct ndis_user_host *h=t->ndis_user_host;
        ndis_host_watchdog(h,now);
        if(!ndis_host_live(h) && !h->syscall_active && t!=g_current && task_alive(t))
            task_kill(t,-1);
    }
    if(g_current && g_current->ndis_user_host && !ndis_host_live(g_current->ndis_user_host)) {
        arch_io_install(0,0); g_need_resched=true;
    }
}
void ndis_user_return(struct trap_frame *tf)
{
    if(!g_current || !g_current->ndis_user_host || (tf->cs&3)!=3) return;
    struct ndis_user_host *h=g_current->ndis_user_host;
    for(;;) {
        ndis_host_watchdog(h,now_us());
        if(!ndis_host_live(h)) { arch_io_install(0,0); task_exit(-1); }
        if(ndis_host_deliver(h,tf,now_us()) || h->depth) break;
        /* Completion abandons the callback stack. No unbudgeted user spin
         * while idle: notification wakes this same parked continuation. */
        g_current->state=T_BLOCKED;
        schedule();
    }
    tf->eflags=(tf->eflags&~(0x3000u|0x20000u|0x4000u))|0x202u;
}
static void user_call(struct ndis_user_host *h,struct trap_frame *tf)
{
    h->syscall_active=true;
    uint64_t now=now_us();
    switch(tf->eax) {
    case NDIS_USER_YIELD: tf->eax=0; g_need_resched=true; break;
    /* TAKE is reserved: callbacks are delivered only by the kernel return
     * hook, never admitted by an untrusted userspace scheduler. */
    case NDIS_USER_COMPLETE:
        if(!ndis_host_return_event(h,tf,tf->ebx,tf->ecx,now))
            ndis_host_stop(h,"invalid-callback-completion",0,tf->eip,now);
        break;
    case NDIS_USER_REARM:
        tf->eax=ndis_host_rearm(h,tf->ebx,tf->ecx,now); break;
    case NDIS_USER_FLAGS: tf->eax=ndis_host_flags(h,tf->eflags); break;
    case NDIS_USER_RESTORE_FLAGS: tf->eax=ndis_host_restore_flags(h,tf->ebx); break;
    default: ndis_host_stop(h,"unsupported-host-syscall",0,tf->eip,now); break;
    }
    h->syscall_active=false;
}
bool ndis_user_trap(struct trap_frame *tf)
{
    if(!g_current || !g_current->ndis_user_host || (tf->cs&3)!=3 ||
       (tf->eflags&0x20000) || (tf->vector>=0x20 && tf->vector<0x30) || tf->vector==7) return false;
    struct ndis_user_host *h=g_current->ndis_user_host;
    if(tf->vector==0x80 && ndis_host_live(h)) user_call(h,tf);
    else {
        uint8_t opcode=0;
        /* No pager/native signal handler. Raw host memory is resident and the
         * kernel copy fixup is the only allowed fault inside this read. */
        int emulated=0;
        if(tf->vector==13 && as_range_ok(&g_current->as,tf->eip,1,false) &&
           !copy_user(&opcode,(const void *)(uintptr_t)tf->eip,1))
            emulated=ndis_host_gp(h,tf,opcode);
        if(!emulated) ndis_host_stop(h,"host-fault",tf->vector,tf->eip,now_us());
        if(emulated==2) g_need_resched=true;
    }
    ndis_user_return(tf); /* faults bypass recoverable native signals */
    if(g_need_resched) schedule();
    return true;
}
