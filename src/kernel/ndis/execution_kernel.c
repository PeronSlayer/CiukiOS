/* SPDX-License-Identifier: GPL-2.0-only */
#include "execution.h"
#include <ciuki/kernel.h>
#include <ciuki/task.h>
#include <ciuki/cpu.h>
#include <ciuki/timing.h>
static unsigned host_irq_depth;
struct ndis_execution **ndis_execution_slot(void) {
    return g_current?&g_current->ndis_execution:0;
}
unsigned ndis_execution_irq_depth(void) { return host_irq_depth; }
void ndis_execution_irq_enter(void) { host_irq_depth++; }
void ndis_execution_irq_leave(void) { if(host_irq_depth) host_irq_depth--; }
static uint64_t now_us(void) {
    if(!g_cpu_tsc || !g_tsc_per_ms) return 0;
    uint64_t cycles=read_tsc_serialized();
    return (cycles/g_tsc_per_ms)*1000+(cycles%g_tsc_per_ms)*1000/g_tsc_per_ms;
}
void ndis_execution_timer(struct trap_frame *tf) {
    if(g_current && g_current->ndis_execution && g_cpu_tsc && g_tsc_per_ms)
        ndis_execution_tick(tf,now_us());
}
uint64_t ndis_execution_now(void) { return now_us(); }
void ndis_execution_fault_record(void *context,const char *driver,uint32_t vector,uint32_t offset,bool shim) {
    (void)context;
    klog("[ndis] fault driver=%s vector=%u eip=+%x shim=%u",driver?driver:"<unnamed>",vector,offset,shim);
}
