/* SPDX-License-Identifier: GPL-2.0-only */
#include "fence.h"
#include <ciuki/cpu.h>
#include <ciuki/kernel.h>
bool ndis_pci_disable(const struct pci_func *p,uint16_t *before,uint16_t *after) {
    if(!p || p->dev>=32 || p->fn>=8) return false;
    uint32_t address=0x80000000u|((uint32_t)p->bus<<16)|((uint32_t)p->dev<<11)|((uint32_t)p->fn<<8);
    uint32_t flags=irq_save();
    outl(0xcf8,address);
    if(inl(0xcfc)!=((uint32_t)p->device<<16|p->vendor)) { irq_restore(flags); return false; }
    outl(0xcf8,address|4);
    *before=cpu_inw(0xcfc);
    uint16_t command=(*before&~7u)|0x400u;
    cpu_outw(0xcfc,command);
    *after=cpu_inw(0xcfc);
    irq_restore(flags);
    return !(*after&7) && (*after&0x400);
}
