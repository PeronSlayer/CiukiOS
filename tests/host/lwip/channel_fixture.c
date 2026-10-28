/* Actual channel readiness with a synthetic queue state; unused channel
 * entry points are discarded by the host link's function-section GC.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "lwip/sys.h"
#include <stdlib.h>
#define CIUKI_CPU_H
#define P2V(p) ((void *)(uintptr_t)(p))
static uint32_t irq_save(void) { return (uint32_t)sys_arch_protect(); }
static void irq_restore(uint32_t f) { sys_arch_unprotect(f); }
#include "../../../src/kernel/proc/channel.c"
/* Only ordinary-channel readiness is supported by this synthetic fixture.
 * Fail loudly if the production poll path starts using service/session work. */
volatile uint64_t g_ticks;
void pmm_free(uint32_t page) { (void)page; abort(); }
void kfree(void *p) { (void)p; abort(); }
void kwait_wake_all(struct kwait *q) { (void)q; abort(); }
static void fixture_ref(struct proc_object *o) { (void)o; }
struct desktop_description *cu_host_channel(unsigned input,unsigned output,bool peer)
{
    static struct channel channel;
    static struct desktop_description endpoint;
    channel.open[0]=true;channel.open[1]=peer;
    channel.inbox[0].count=input;channel.inbox[1].count=output;
    endpoint.object=(struct proc_object){fixture_ref,fixture_ref,true};
    endpoint.kind=DESKTOP_CHANNEL;endpoint.u.endpoint.channel=&channel;endpoint.u.endpoint.side=0;
    return &endpoint;
}
